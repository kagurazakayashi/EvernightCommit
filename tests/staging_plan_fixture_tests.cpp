// 「加入暂存区」的集成测试：用真实临时仓库跑出工作区状态，把**生产代码**构造出来的
// git add 方案原样交给真实 Git 执行，再核对索引内容与工作区文件。
// 覆盖验收清单：多选但存在未选变化、文件删除、工作区重命名、部分暂存后再次添加、
// 特殊字符路径的字面语义、大量文件一次传完、未合并条目被记为已解决、
// 子模块只记录提交指针、选中条目消失时“全有或全无”的真实失败，
// 以及旧 Git 退回字面 pathspec 参数形态后的同一结果。
// 断言的是「索引里到底有什么」，不是命令文本；相关用例还逐字节确认工作区文件没被改写。
// 只在夹具自建的临时仓库里跑 Git，绝不接触真实仓库或远端。
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "git/staging_plan.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/pathspec_file.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/workspace_status.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::StagingDelivery;
using gc::git::StagingPlan;
using gc::git::StagingPlanOptions;
using gc::git::WorkspaceLoadStatus;
using gc::git::WorkspaceModel;
using gc::git::WorkspaceSnapshot;
using gc::test::GitFixture;
using gc::test::GitRun;

std::string ToUtf8(std::wstring_view text) { return gc::platform::Utf16ToUtf8(std::wstring(text)); }

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

// 生产读取路径：夹具只提供隔离执行器，条目全部来自真实 git status。
WorkspaceModel Load(GitFixture& fixture) {
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const WorkspaceSnapshot snapshot = gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
  GC_REQUIRE_MESSAGE(snapshot.status == WorkspaceLoadStatus::loaded,
                     "读取工作区失败：" + ToUtf8(snapshot.message));
  return snapshot.model;
}

const ChangeItem* Find(const std::vector<ChangeItem>& items, std::wstring_view path) {
  for (const ChangeItem& item : items) {
    if (item.path == path) {
      return &item;
    }
  }
  return nullptr;
}

size_t CountKind(const std::vector<ChangeItem>& items, ChangeKind kind) {
  return static_cast<size_t>(
      std::count_if(items.begin(), items.end(), [kind](const ChangeItem& item) { return item.kind == kind; }));
}

std::string Listed(const std::vector<ChangeItem>& items) {
  std::string text;
  for (const ChangeItem& item : items) {
    text += "[" + ToUtf8(item.path) + "/" + ToUtf8(item.StatusLabel()) + "]";
  }
  return text;
}

const ChangeItem& RequireItem(const std::vector<ChangeItem>& items, std::wstring_view path,
                              const std::string& side) {
  const ChangeItem* found = Find(items, path);
  GC_REQUIRE_MESSAGE(found != nullptr, side + " 缺少条目 " + ToUtf8(path) + "，该侧实际有：" + Listed(items));
  return *found;
}

// 清单临时文件用完即删：夹具里的 Git 是同步执行、返回时早已退出。
class ScopedPathspecFile {
public:
  explicit ScopedPathspecFile(std::wstring path) : path_(std::move(path)) {}
  ScopedPathspecFile(const ScopedPathspecFile&) = delete;
  ScopedPathspecFile& operator=(const ScopedPathspecFile&) = delete;
  ~ScopedPathspecFile() { gc::platform::RemoveNulPathspecFile(path_); }

private:
  std::wstring path_;
};

struct AppliedStaging {
  StagingPlan plan;
  GitRun run;
};

// 把「选中的路径」交给生产方案构造 + 生产清单写入 + 真实 Git 执行，和界面点击走的是同一条链路。
AppliedStaging StageSelection(GitFixture& fixture, const WorkspaceModel& model,
                               const std::vector<std::wstring>& selectedPaths,
                               bool pathspecFileSupported = true) {
  std::vector<ChangeItem> selection;
  for (const std::wstring& path : selectedPaths) {
    selection.push_back(RequireItem(model.unstaged, path, "未暂存侧"));
  }

  StagingPlanOptions options;
  options.pathspecFileSupported = pathspecFileSupported;
  options.totalUnstagedItems = model.unstaged.size();
  AppliedStaging applied;
  applied.plan = gc::git::BuildStagingAddPlan(selection, options);
  GC_REQUIRE_MESSAGE(applied.plan.delivery != StagingDelivery::blocked,
                     "方案被拒绝：" + ToUtf8(applied.plan.blockedReason));

  std::vector<std::wstring> arguments = applied.plan.arguments;
  std::unique_ptr<ScopedPathspecFile> holder;
  if (applied.plan.delivery == StagingDelivery::pathspecFile) {
    const gc::platform::PathspecFileWrite written =
        gc::platform::WriteNulPathspecFile(applied.plan.pathspecEntries);
    GC_REQUIRE_MESSAGE(written.written, "写清单文件失败：" + ToUtf8(written.failureReason));
    holder = std::make_unique<ScopedPathspecFile>(written.path);
    GC_REQUIRE_MESSAGE(gc::git::AppendPathspecFileOptions(&arguments, written.path),
                       "清单路径无法交给 git add：" + ToUtf8(written.path));
  }
  applied.run = fixture.RunInRepo(arguments);  // 工作目录 = 仓库根，与命令窗口执行器一致。
  return applied;
}

std::string DescribeRun(const GitRun& run) {
  return "exit=" + std::to_string(run.exitCode) + " err=[" + ToUtf8(run.err) + "]";
}

// 索引里此刻的暂存条目（用生产解析器重新读一次）。
WorkspaceModel StagedNow(GitFixture& fixture) { return Load(fixture); }

std::vector<std::wstring> SortedPaths(const std::vector<ChangeItem>& items) {
  std::vector<std::wstring> paths;
  for (const ChangeItem& item : items) {
    paths.push_back(item.path);
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

void RequirePaths(const std::vector<ChangeItem>& items, const std::vector<std::wstring>& expected,
                  const std::string& side) {
  std::vector<std::wstring> wanted(expected.begin(), expected.end());
  std::sort(wanted.begin(), wanted.end());
  GC_CHECK_MESSAGE(SortedPaths(items) == wanted, side + " 与预期不符：实际 " + Listed(items));
}

// 工作区逐字节快照（不含 .git 目录内容）：一次 add 只该动索引，绝不该动文件。
std::map<std::wstring, std::string> SnapshotWorktree(GitFixture& fixture) {
  std::map<std::wstring, std::string> snapshot;
  const std::filesystem::path root(std::wstring(fixture.RepoDir()));
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(root,
                                                   std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    GC_REQUIRE_MESSAGE(false, "遍历工作区失败：" + ec.message());
    return snapshot;
  }
  for (const std::filesystem::directory_entry& entry : it) {
    const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
    if (ec) {
      continue;
    }
    std::wstring key = relative.wstring();
    std::replace(key.begin(), key.end(), L'\\', L'/');
    if (key.empty() || key == L".git" || key.rfind(L".git/", 0) == 0) {
      continue;  // 索引与对象库本来就要被 git add 改动，不在“工作区文件不许被改”的断言范围。
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    std::ifstream file(entry.path(), std::ios::binary);
    snapshot[key] = file.is_open() ? std::string((std::istreambuf_iterator<char>(file)),
                                                 std::istreambuf_iterator<char>())
                                   : std::string("<读不到>");
  }
  return snapshot;
}

std::string ReadWorktreeFile(GitFixture& fixture, std::wstring_view repositoryRelative) {
  std::ifstream file(std::filesystem::path(std::wstring(fixture.PathInRoot(
                         std::wstring(L"repo/") + std::wstring(repositoryRelative)))),
                     std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void InitWithCommit(GitFixture& fixture, std::wstring_view directoryName = L"repo") {
  fixture.InitRepository(directoryName);
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");
}

}  // namespace

GC_TEST(staging_fixture_selected_only_leaves_unselected_changes_untouched) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  for (const std::wstring& name :
       {std::wstring(L"picked.txt"), std::wstring(L"nested/picked-too.txt"), std::wstring(L"skipped.txt"),
        std::wstring(L"removed.txt")}) {
    fixture.WriteFile(name, "base\n");
  }
  fixture.StageAll();
  fixture.Commit(L"补齐基线");

  fixture.WriteFile(L"picked.txt", "picked-new\n");
  fixture.WriteFile(L"nested/picked-too.txt", "picked-too-new\n");
  fixture.WriteFile(L"skipped.txt", "skipped-new\n");  // 未选中
  fixture.WriteFile(L"brand-new.txt", "new\n");        // 未跟踪、选中
  fixture.WriteFile(L"unselected-new.txt", "other\n");  // 未跟踪、未选中
  std::filesystem::remove(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/removed.txt"))));

  const WorkspaceModel before = Load(fixture);
  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);
  const AppliedStaging applied =
      StageSelection(fixture, before, {L"picked.txt", L"nested/picked-too.txt", L"removed.txt", L"brand-new.txt"});
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git add 失败：" + DescribeRun(applied.run));
  GC_CHECK(applied.plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(applied.plan.selectedItems == 4);
  // 清单形态下参数里只有 add 与全局选项，一条路径都不出现在命令行上。
  GC_CHECK(applied.plan.arguments.size() == 5);

  const WorkspaceModel after = StagedNow(fixture);
  // 索引内容 = 选择范围：修改 2 项、删除 1 项（记下删除也算暂存）、新增 1 项。
  RequirePaths(after.staged,
               {L"picked.txt", L"nested/picked-too.txt", L"removed.txt", L"brand-new.txt"}, "已暂存侧");
  const ChangeItem& removal = RequireItem(after.staged, L"removed.txt", "已暂存侧");
  GC_CHECK_MESSAGE(removal.kind == ChangeKind::deleted,
                   "被删文件应记为删除：" + ToUtf8(removal.statusCode));
  const ChangeItem& addition = RequireItem(after.staged, L"brand-new.txt", "已暂存侧");
  GC_CHECK(addition.kind == ChangeKind::added);
  // 没选中的一律保持原状，仍留在未暂存侧。
  RequirePaths(after.unstaged, {L"skipped.txt", L"unselected-new.txt"}, "未暂存侧");
  // 工作区逐字节不变：一次暂存只写索引。
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "git add 不应改写工作区任何文件");
}

GC_TEST(staging_fixture_worktree_rename_requires_both_entries) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"old.txt", "content\n");
  fixture.StageAll();
  fixture.Commit(L"加入 old.txt");
  std::filesystem::rename(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/old.txt"))),
                          std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/new.txt"))));

  const WorkspaceModel renamed = Load(fixture);
  // 实测形态：工作区改名在 porcelain v2 里是「.D old.txt」+「?? new.txt」两条未暂存条目。
  const ChangeItem& removed = RequireItem(renamed.unstaged, L"old.txt", "未暂存侧");
  const ChangeItem& created = RequireItem(renamed.unstaged, L"new.txt", "未暂存侧");
  GC_CHECK(removed.kind == ChangeKind::deleted);
  GC_CHECK(created.kind == ChangeKind::untracked);

  // 只选新文件：旧路径的删除必须原地保留，不能被顺带暂存。
  const AppliedStaging half = StageSelection(fixture, renamed, {L"new.txt"});
  GC_CHECK_MESSAGE(half.run.exitCode == 0, "git add 失败：" + DescribeRun(half.run));
  const WorkspaceModel afterHalf = StagedNow(fixture);
  RequirePaths(afterHalf.staged, {L"new.txt"}, "已暂存侧");
  RequirePaths(afterHalf.unstaged, {L"old.txt"}, "未暂存侧");

  // 再选旧路径：两条一起交给同一条命令，索引里就是一次完整的重命名。
  const AppliedStaging both = StageSelection(fixture, afterHalf, {L"old.txt"});
  GC_CHECK_MESSAGE(both.run.exitCode == 0, "git add 失败：" + DescribeRun(both.run));
  const WorkspaceModel afterBoth = StagedNow(fixture);
  GC_CHECK_MESSAGE(afterBoth.staged.size() == 1 && afterBoth.staged[0].path == L"new.txt" &&
                       afterBoth.staged[0].kind == ChangeKind::renamed &&
                       afterBoth.staged[0].oldPath == L"old.txt",
                   "重命名应成为一条带来源路径的暂存条目：" + Listed(afterBoth.staged));
  GC_CHECK_MESSAGE(afterBoth.unstaged.empty(), "重命名两条都暂存后不该有残留：" + Listed(afterBoth.unstaged));
}

GC_TEST(staging_fixture_re_add_updates_index_to_latest_worktree) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"part.txt", "v1\n");
  fixture.StageAll();
  fixture.Commit(L"part 基线");

  fixture.WriteFile(L"part.txt", "v2\n");
  const WorkspaceModel model = Load(fixture);
  GC_CHECK_MESSAGE(StageSelection(fixture, model, {L"part.txt"}).run.exitCode == 0, "第一次暂存失败");
  const GitRun staged = fixture.RunCheckedInRepo({L"show", L":part.txt"});
  GC_CHECK_MESSAGE(ToUtf8(staged.out) == "v2\n", "索引里应是 v2：" + ToUtf8(staged.out));
  GC_CHECK(fixture.ShowFileAtHead(L"part.txt") == "v1\n");

  // 已暂存一版之后继续编辑，再点一次：索引内容更新为工作区当前内容（不是保留旧的一版）。
  fixture.WriteFile(L"part.txt", "v3\n");
  const WorkspaceModel again = Load(fixture);
  GC_CHECK_MESSAGE(Find(again.unstaged, L"part.txt") != nullptr,
                   "同一文件已暂存后又改动，应同时出现在未暂存侧：" + Listed(again.unstaged));
  GC_CHECK_MESSAGE(StageSelection(fixture, again, {L"part.txt"}).run.exitCode == 0, "第二次暂存失败");
  const GitRun latest = fixture.RunCheckedInRepo({L"show", L":part.txt"});
  GC_CHECK_MESSAGE(ToUtf8(latest.out) == "v3\n", "索引里应是最新内容：" + ToUtf8(latest.out));
  const WorkspaceModel after = StagedNow(fixture);
  RequirePaths(after.staged, {L"part.txt"}, "已暂存侧");
  GC_CHECK_MESSAGE(after.unstaged.empty(), "工作区已与索引一致：" + Listed(after.unstaged));
}

GC_TEST(staging_fixture_special_paths_stay_literal) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  // 一批 Windows 上合法、但对 pathspec 语法“不干净”的文件名。
  const std::vector<std::wstring> tricky = {
      L"中文 文件.txt", L"brk[x].txt",     L"100%.txt",  L"!bang.txt",
      L"#hash.txt",    L"-dash.txt",       L"sp ace.txt", L"a+b&c~d`e'f.txt",
  };
  // 每一“条字面路径”都配一个“按 glob 会被同一模式命中的兄弟文件”，用来证明没有过度命中：
  // brk[x].txt 若被当字符类就会命中 brkx.txt，fresh[1].txt 同理命中 fresh1.txt。
  const std::vector<std::wstring> globVictims = {L"brkx.txt"};
  std::vector<std::wstring> baseline;
  for (const std::wstring& name : tricky) {
    baseline.push_back(name);
  }
  for (const std::wstring& name : globVictims) {
    baseline.push_back(name);
  }
  baseline.push_back(L"fresh1.txt");
  for (const std::wstring& name : baseline) {
    fixture.WriteFile(name, "base\n");
  }
  fixture.StageAll();
  fixture.Commit(L"特殊文件名基线");

  std::vector<std::wstring> selection;
  for (const std::wstring& name : tricky) {
    fixture.WriteFile(name, "changed\n");
    selection.push_back(name);
  }
  for (const std::wstring& name : globVictims) {
    fixture.WriteFile(name, "must-stay-unstaged\n");  // 未选中
  }
  fixture.WriteFile(L"fresh[1].txt", "untracked\n");
  selection.push_back(L"fresh[1].txt");
  fixture.WriteFile(L"fresh1.txt", "must-stay-unstaged\n");  // 未选中（且未跟踪）

  const WorkspaceModel before = Load(fixture);
  const AppliedStaging applied = StageSelection(fixture, before, selection);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git add 失败：" + DescribeRun(applied.run));

  const WorkspaceModel after = StagedNow(fixture);
  RequirePaths(after.staged, selection, "已暂存侧");
  // 关键：通配、取反、注释与“看起来像选项”的形态一律没有生效，也没连带暂存没选中的兄弟文件。
  RequirePaths(after.unstaged, {L"brkx.txt", L"fresh1.txt"}, "未暂存侧");
}

GC_TEST(staging_fixture_many_files_in_one_command) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  constexpr int kCount = 1200;
  std::vector<std::wstring> created;
  created.reserve(kCount);
  for (int index = 0; index < kCount; ++index) {
    const std::wstring path =
        L"many/dir " + std::to_wstring(index % 7) + L"/file-" + std::to_wstring(index) + L".txt";
    fixture.WriteFile(path, "x\n");
    created.push_back(path);
  }

  const WorkspaceModel before = Load(fixture);
  GC_CHECK_MESSAGE(static_cast<long>(before.unstaged.size()) == kCount,
                   "未跟踪目录应展开成单条文件：" + std::to_string(before.unstaged.size()));
  // 一次点击选全部：清单形态不受 Windows 命令行长度限制（同样的路径逐个进参数要 6 万字符以上）。
  const AppliedStaging applied = StageSelection(fixture, before, created);
  GC_CHECK(applied.plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(applied.plan.pathspecCount == created.size());
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git add 失败：" + DescribeRun(applied.run));

  const WorkspaceModel after = StagedNow(fixture);
  GC_CHECK_MESSAGE(after.staged.size() == created.size(),
                   "全部选中项都应进索引：" + std::to_string(after.staged.size()));
  GC_CHECK_MESSAGE(after.unstaged.empty(), "不应有残留：" + Listed(after.unstaged));
}

GC_TEST(staging_fixture_old_git_inline_mode_reaches_same_index) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  for (const std::wstring& name : {std::wstring(L"a.txt"), std::wstring(L"brk[x].txt"), std::wstring(L"brkx.txt")}) {
    fixture.WriteFile(name, "base\n");
  }
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"a.txt", "new\n");
  fixture.WriteFile(L"brk[x].txt", "new\n");
  fixture.WriteFile(L"brkx.txt", "new\n");  // 未选中
  fixture.WriteFile(L"fresh 文件.txt", "untracked\n");

  const WorkspaceModel before = Load(fixture);
  // 模拟旧 Git（不支持清单接口）：同一份选择改用字面 pathspec 参数形态，结果必须完全一致。
  const AppliedStaging applied =
      StageSelection(fixture, before, {L"a.txt", L"brk[x].txt", L"fresh 文件.txt"}, /*pathspecFileSupported=*/false);
  GC_CHECK(applied.plan.delivery == StagingDelivery::inlinePathspec);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git add 失败：" + DescribeRun(applied.run));

  const WorkspaceModel after = StagedNow(fixture);
  RequirePaths(after.staged, {L"a.txt", L"brk[x].txt", L"fresh 文件.txt"}, "已暂存侧");
  RequirePaths(after.unstaged, {L"brkx.txt"}, "未暂存侧");
}

GC_TEST(staging_fixture_conflicted_path_is_marked_resolved) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"conf.txt", "shared\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"conf.txt", "A-side\n");
  fixture.StageAll();
  fixture.Commit(L"A 侧");
  fixture.RunCheckedInRepo({L"checkout", L"-b", L"other", L"HEAD~1"});
  fixture.WriteFile(L"conf.txt", "B-side\n");
  fixture.StageAll();
  fixture.Commit(L"B 侧");
  fixture.RunCheckedInRepo({L"checkout", L"main"});
  // 合并故意留下冲突：夹具只在这里跑 Git，判定仍由生产代码与真实 Git 给出。
  const GitRun merged = fixture.RunInRepo({L"merge", L"other", L"-m", L"故意冲突"});
  GC_REQUIRE_MESSAGE(merged.exitCode != 0, "合并应当失败并留下冲突：" + DescribeRun(merged));

  const WorkspaceModel before = Load(fixture);
  const ChangeItem& conflicted = RequireItem(before.unstaged, L"conf.txt", "未暂存侧");
  GC_CHECK_MESSAGE(conflicted.kind == ChangeKind::conflicted,
                   "冲突条目应被识别为未合并：" + ToUtf8(conflicted.statusCode));

  // 生产方案必须把「add 就是把当前内容记为已解决」这句话交出来，由界面提请用户确认。
  StagingPlanOptions options;
  options.pathspecFileSupported = true;
  options.totalUnstagedItems = before.unstaged.size();
  const StagingPlan plan = gc::git::BuildStagingAddPlan({conflicted}, options);
  GC_REQUIRE_MESSAGE(plan.delivery != StagingDelivery::blocked, ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(!plan.confirmationText.empty(), "未合并条目必须要求用户确认");
  GC_CHECK_MESSAGE(plan.confirmationText.find(L"没有检查") != std::wstring::npos,
                   "确认文案不能声称冲突已解决：" + ToUtf8(plan.confirmationText));

  const AppliedStaging applied = StageSelection(fixture, before, {L"conf.txt"});
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git add 失败：" + DescribeRun(applied.run));

  const WorkspaceModel after = StagedNow(fixture);
  GC_CHECK_MESSAGE(CountKind(after.unstaged, ChangeKind::conflicted) == 0,
                   "暂存后不应再有未合并条目：" + Listed(after.unstaged));
  RequirePaths(after.staged, {L"conf.txt"}, "已暂存侧");
  // 实测语义：进索引的是工作区当前字节（连冲突标记一起）。程序不判断冲突是否真的解决。
  const GitRun indexBytes = fixture.RunCheckedInRepo({L"show", L":conf.txt"});
  const std::string onDisk = ReadWorktreeFile(fixture, L"conf.txt");
  GC_CHECK_MESSAGE(ToUtf8(indexBytes.out) == onDisk, "索引应与工作区当前内容逐字节一致");
  GC_CHECK_MESSAGE(onDisk.find("<<<<<<<") != std::string::npos, "用例前提：工作区里还留着冲突标记");
  GC_CHECK_MESSAGE(fixture.ShowFileAtHead(L"conf.txt") != onDisk, "索引内容不是 HEAD 版本");
}

GC_TEST(staging_fixture_submodule_records_pointer_only) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 子模块来源只能是本机临时仓库。
  fixture.InitRepository(L"child-origin");
  fixture.WriteFile(L"inside.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"子模块基线");
  const std::wstring childSource = fixture.RepoDir();

  InitWithCommit(fixture, L"parent");
  fixture.RunCheckedInRepo(
      {L"-c", L"protocol.file.allow=always", L"submodule", L"add", L"--", childSource, L"child"});
  fixture.RunCheckedInRepo({L"commit", L"-qm", L"登记子模块"});
  const std::wstring childDir = fixture.PathInRoot(L"parent/child");

  // 1) 只有内部改动：父仓库这一次 add 是空操作，内部文件一条都不会进父索引。
  fixture.WriteFile(L"child/inside.txt", "dirty\n");
  fixture.WriteFile(L"child/scratch.tmp", "untracked\n");
  const WorkspaceModel dirty = Load(fixture);
  const ChangeItem& dirtyEntry = RequireItem(dirty.unstaged, L"child", "未暂存侧");
  GC_CHECK_MESSAGE(dirtyEntry.kind == ChangeKind::submodule && !dirtyEntry.submodule.commitChanged,
                   "指针未移动时应报告为子模块条目且 commitChanged 为假：" + ToUtf8(dirtyEntry.statusCode));
  const AppliedStaging noOp = StageSelection(fixture, dirty, {L"child"});
  GC_CHECK_MESSAGE(noOp.run.exitCode == 0, "子模块空操作也应是 0 退出：" + DescribeRun(noOp.run));
  const WorkspaceModel afterNoOp = StagedNow(fixture);
  GC_CHECK_MESSAGE(afterNoOp.staged.empty(),
                   "指针未移动时不该产生任何暂存：" + Listed(afterNoOp.staged));
  // 内部改动仍属于子模块自己的仓库，父仓库没有替它暂存。
  const GitRun childStatus = fixture.RunChecked({L"status", L"--porcelain=v1"}, childDir);
  GC_CHECK_MESSAGE(!childStatus.out.empty(), "子模块内部改动必须原样保留：" + ToUtf8(childStatus.out));

  // 2) 在子模块里提交之后，父仓库这一次 add 才记录新的提交指针。
  fixture.RunChecked({L"add", L"-A"}, childDir);
  fixture.RunChecked({L"commit", L"-qm", L"子模块内部提交"}, childDir);
  const WorkspaceModel moved = Load(fixture);
  const ChangeItem& movedEntry = RequireItem(moved.unstaged, L"child", "未暂存侧");
  GC_CHECK_MESSAGE(movedEntry.submodule.commitChanged, "指针应已移动：" + ToUtf8(movedEntry.statusCode));
  const AppliedStaging pointer = StageSelection(fixture, moved, {L"child"});
  GC_CHECK_MESSAGE(pointer.run.exitCode == 0, "git add 失败：" + DescribeRun(pointer.run));

  const WorkspaceModel after = StagedNow(fixture);
  const ChangeItem& stagedPointer = RequireItem(after.staged, L"child", "已暂存侧");
  GC_CHECK_MESSAGE(stagedPointer.kind == ChangeKind::submodule, "暂存的应只有 gitlink");
  GC_CHECK_MESSAGE(after.staged.size() == 1, "父索引里不该出现子模块内部文件：" + Listed(after.staged));
  GC_CHECK_MESSAGE(Find(after.staged, L"child/scratch.tmp") == nullptr &&
                       Find(after.staged, L"child/inside.txt") == nullptr,
                   "父仓库绝不能递归暂存子模块内部文件：" + Listed(after.staged));
}

GC_TEST(staging_fixture_missing_path_fails_without_partial_stage) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"picked.txt", "base\n");
  fixture.WriteFile(L"vanished.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"picked.txt", "changed\n");
  // 未跟踪的第三份改动，用来验证“失败是全有或全无”。
  fixture.WriteFile(L"extra.txt", "extra\n");

  WorkspaceModel before = Load(fixture);
  GC_REQUIRE_MESSAGE(Find(before.unstaged, L"picked.txt") != nullptr, "picked.txt 应有未暂存的修改");
  GC_REQUIRE_MESSAGE(Find(before.unstaged, L"vanished.txt") == nullptr, "vanished.txt 此刻应已干净");
  // 制造一个“列表里有、仓库里已经没有了”的条目：先读进模型，再从两侧同时删掉它。
  fixture.WriteFile(L"ghost.txt", "untracked\n");
  before = Load(fixture);
  GC_REQUIRE_MESSAGE(Find(before.unstaged, L"ghost.txt") != nullptr,
                     "应先读到新建的未跟踪文件：" + Listed(before.unstaged));
  std::filesystem::remove(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/ghost.txt"))));

  const AppliedStaging applied = StageSelection(fixture, before, {L"picked.txt", L"extra.txt", L"ghost.txt"});
  GC_CHECK_MESSAGE(applied.run.exitCode != 0, "路径消失必须失败，而不是静默跳过：" + DescribeRun(applied.run));
  GC_CHECK_MESSAGE(applied.run.err.find(L"did not match") != std::wstring::npos,
                   "应保留 Git 的原始错误：" + ToUtf8(applied.run.err));
  // 全有或全无：另外两条合法路径也不能被“顺带”暂存一半。
  const WorkspaceModel after = StagedNow(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "失败时索引不该有任何变化：" + Listed(after.staged));
  RequirePaths(after.unstaged, {L"picked.txt", L"extra.txt"}, "未暂存侧");
}
