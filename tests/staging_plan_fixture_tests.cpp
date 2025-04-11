// 「加入暂存区」与「← 移出暂存区」的集成测试：用真实临时仓库跑出工作区状态，把**生产代码**构造出来的
// 命令原样交给真实 Git 执行，再核对索引内容与工作区文件。
// 暂存一侧覆盖：多选但存在未选变化、文件删除、工作区重命名、部分暂存后再次添加、
// 特殊字符路径的字面语义、大量文件一次传完、未合并条目被记为已解决、
// 子模块只记录提交指针、选中条目消失时“全有或全无”的真实失败，
// 以及旧 Git 退回字面 pathspec 参数形态后的同一结果。
// 取消暂存一侧覆盖：有 HEAD 时索引逐条退回 HEAD 而工作区字节不变、部分暂存（MM）只退回索引那一份、
// 尚无提交时改用只写索引的形态且磁盘文件原样留下、重命名旧新路径成对撤回、
// 子模块指针退回而不进入子模块、特殊字符不过度命中、大量多选，以及路径消失时的失败行为。
// 断言的是「索引里到底有什么」（对象 ID 一致才算内容一致），不是命令文本；
// 相关用例还逐字节确认工作区文件没被改写。
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

// 把「选中的已暂存条目」交给生产方案构造 + 生产清单写入 + 真实 Git 执行，
// 与界面上点「← 移出暂存区」走的是同一条链路。
// repositoryHasHead 与 restoreCommandSupported 一起决定命令形态：两者任一不成立都用
// 带路径的 git reset（restore --staged 在那两种场合分别会 fatal 或根本不是子命令）。
AppliedStaging UnstageSelection(GitFixture& fixture, const WorkspaceModel& model,
                                const std::vector<std::wstring>& selectedPaths, bool repositoryHasHead,
                                bool pathspecFileSupported = true, bool restoreCommandSupported = true) {
  std::vector<ChangeItem> selection;
  for (const std::wstring& path : selectedPaths) {
    selection.push_back(RequireItem(model.staged, path, "已暂存侧"));
  }

  StagingPlanOptions options;
  options.pathspecFileSupported = pathspecFileSupported;
  options.repositoryHasHead = repositoryHasHead;
  options.restoreCommandSupported = restoreCommandSupported;
  options.totalStagedItems = model.staged.size();
  AppliedStaging applied;
  applied.plan = gc::git::BuildStagingUnstagePlan(selection, options);
  GC_REQUIRE_MESSAGE(applied.plan.delivery != StagingDelivery::blocked,
                     "方案被拒绝：" + ToUtf8(applied.plan.blockedReason));

  std::vector<std::wstring> arguments = applied.plan.arguments;
  std::unique_ptr<ScopedPathspecFile> holder;
  if (applied.plan.delivery == StagingDelivery::pathspecFile) {
    const gc::platform::PathspecFileWrite written =
        gc::platform::WriteNulPathspecFile(applied.plan.pathspecEntries, L"gc-unstage");
    GC_REQUIRE_MESSAGE(written.written, "写清单文件失败：" + ToUtf8(written.failureReason));
    holder = std::make_unique<ScopedPathspecFile>(written.path);
    GC_REQUIRE_MESSAGE(gc::git::AppendPathspecFileOptions(&arguments, written.path),
                       "清单路径无法交给 Git：" + ToUtf8(written.path));
  }
  applied.run = fixture.RunInRepo(arguments);  // 工作目录 = 仓库根，与命令窗口执行器一致。
  return applied;
}

// 「索引里到底有什么」只能用对象 ID 断言：内容一致 ID 才一致，
// 因此把 ls-files / ls-tree 的每行按路径收成 blob 表，再逐条比较。
std::map<std::wstring, std::string> ParseBlobTable(const GitRun& run, size_t blobFieldIndex) {
  std::map<std::wstring, std::string> table;
  size_t cursor = 0;
  while (cursor < run.out.size()) {
    const size_t newline = run.out.find(L'\n', cursor);
    const size_t end = newline == std::wstring::npos ? run.out.size() : newline;
    std::wstring line = run.out.substr(cursor, end - cursor);
    cursor = end + 1;
    if (!line.empty() && line.back() == L'\r') {
      line.pop_back();
    }
    const size_t tab = line.find(L'\t');
    if (tab == std::wstring::npos || tab == 0) {
      continue;
    }
    // ls-files -s：「<mode> <sha> <stage>\t<路径>」；ls-tree -r：「<mode> blob <sha>\t<路径>」。
    std::vector<std::wstring> fields;
    size_t at = 0;
    while (at < tab) {
      const size_t space = line.find(L' ', at);
      const size_t stop = (space == std::wstring::npos || space > tab) ? tab : space;
      fields.push_back(line.substr(at, stop - at));
      at = stop + 1;
    }
    if (fields.size() <= blobFieldIndex) {
      continue;
    }
    table[line.substr(tab + 1)] = ToUtf8(fields[blobFieldIndex]);
  }
  return table;
}

std::map<std::wstring, std::string> IndexBlobs(GitFixture& fixture) {
  return ParseBlobTable(fixture.RunCheckedInRepo({L"ls-files", L"-s"}), 1);
}

// HEAD 的文件表；尚无任何提交时 HEAD 不可解析，返回空表（调用方按「不该有任何条目」断言）。
std::map<std::wstring, std::string> HeadBlobs(GitFixture& fixture) {
  const GitRun run = fixture.RunInRepo({L"ls-tree", L"-r", L"HEAD"});
  if (!run.Success()) {
    return {};
  }
  return ParseBlobTable(run, 2);
}

std::string DescribeTable(const std::map<std::wstring, std::string>& table) {
  std::string text;
  for (const auto& [path, blob] : table) {
    text += "[" + ToUtf8(path) + "=" + blob.substr(0, 8) + "]";
  }
  return text;
}

// 路径在表里的那条记录（不存在时返回 "<缺失>"），用于逐条断言而不用整表比较。
std::string BlobOf(const std::map<std::wstring, std::string>& table, std::wstring_view path) {
  const auto found = table.find(std::wstring(path));
  return found == table.end() ? std::string("<缺失>") : found->second;
}

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

// 直接按绝对路径读文件（子模块那类不在本仓库工作区根之下的目录要用它）。
std::string ReadFileAt(const std::wstring& absolutePath) {
  std::ifstream file(std::filesystem::path(absolutePath), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileAt(const std::wstring& absolutePath, const std::string& utf8Content) {
  std::ofstream file(std::filesystem::path(absolutePath), std::ios::binary | std::ios::trunc);
  file << utf8Content;
}

// 字面 pathspec 元素：方案里收集到的每条都带这个前缀，断言时写起来更短。
std::wstring Lit(std::wstring_view path) { return std::wstring(L":(literal)") + std::wstring(path); }

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

// ---------------------------------------------------------------------------
// ← 移出暂存区：断言的是「索引退回什么」与「磁盘上的字节有没有被动过」
// ---------------------------------------------------------------------------

GC_TEST(unstage_fixture_head_repo_returns_index_to_head_and_keeps_worktree) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  for (const std::wstring& name :
       {std::wstring(L"mod.txt"), std::wstring(L"del.txt"), std::wstring(L"part.txt"), std::wstring(L"keep.txt")}) {
    fixture.WriteFile(name, "head\n");
  }
  fixture.StageAll();
  fixture.Commit(L"补齐基线");

  fixture.WriteFile(L"mod.txt", "mod-staged\n");        // 只暂存了修改
  fixture.WriteFile(L"part.txt", "part-staged\n");
  fixture.Stage({L"mod.txt", L"part.txt"});
  fixture.WriteFile(L"part.txt", "part-latest\n");      // MM：索引一份、磁盘另一份
  std::filesystem::remove(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/del.txt"))));
  fixture.Stage({L"del.txt"});                          // 已暂存的删除
  fixture.WriteFile(L"brand.txt", "brand new file with completely different bytes\n");
  fixture.Stage({L"brand.txt"});                        // 已暂存的新增（HEAD 里没有）
  fixture.WriteFile(L"keep.txt", "keep-staged\n");
  fixture.Stage({L"keep.txt"});                         // 已暂存但本次不选中

  const WorkspaceModel before = Load(fixture);
  GC_REQUIRE_MESSAGE(before.staged.size() == 5, "应有 5 条已暂存条目：" + Listed(before.staged));
  GC_REQUIRE_MESSAGE(before.unstaged.size() == 1, "部分暂存的那条应同时在未暂存侧：" + Listed(before.unstaged));
  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);
  const std::map<std::wstring, std::string> head = HeadBlobs(fixture);
  const std::map<std::wstring, std::string> indexBefore = IndexBlobs(fixture);

  const AppliedStaging applied =
      UnstageSelection(fixture, before, {L"mod.txt", L"del.txt", L"part.txt", L"brand.txt"},
                       /*repositoryHasHead=*/true);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git restore --staged 失败：" + DescribeRun(applied.run));
  GC_CHECK(applied.plan.delivery == StagingDelivery::pathspecFile);
  // 清单形态下参数只有全局选项、restore 与 --staged：一条路径都不出现在命令行上。
  GC_CHECK(applied.plan.arguments.size() == 6);
  GC_CHECK_MESSAGE(applied.plan.confirmationText.empty(),
                   "普通条目不该要求额外确认：" + ToUtf8(applied.plan.confirmationText));

  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  // 选中的四条逐条退回 HEAD：修改恢复旧内容、删除把路径放回索引、新增整个离开索引。
  GC_CHECK_MESSAGE(BlobOf(indexAfter, L"mod.txt") == BlobOf(head, L"mod.txt"),
                   "mod.txt 的索引内容应退回 HEAD：" + DescribeTable(indexAfter));
  GC_CHECK(BlobOf(indexAfter, L"part.txt") == BlobOf(head, L"part.txt"));
  GC_CHECK(BlobOf(indexAfter, L"del.txt") == BlobOf(head, L"del.txt"));
  GC_CHECK(BlobOf(indexAfter, L"brand.txt") == "<缺失>");
  // 没选中的那条保持它已经暂存的那一份，绝不能被“顺带”退回。
  GC_CHECK(BlobOf(indexAfter, L"keep.txt") == BlobOf(indexBefore, L"keep.txt"));
  GC_CHECK(BlobOf(indexAfter, L"keep.txt") != BlobOf(head, L"keep.txt"));
  // 工作区逐字节不变：取消暂存只该动索引，磁盘上既不新增也不删除任何文件。
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "取消暂存不该改写工作区任何文件");
  // 部分暂存的那条：最新的工作区字节仍在磁盘上，索引退回 HEAD，两份内容都没丢。
  GC_CHECK(ReadWorktreeFile(fixture, L"part.txt") == "part-latest\n");

  const WorkspaceModel after = Load(fixture);
  RequirePaths(after.staged, {L"keep.txt"}, "已暂存侧");
  RequirePaths(after.unstaged, {L"mod.txt", L"del.txt", L"part.txt", L"brand.txt"}, "未暂存侧");
  GC_CHECK(RequireItem(after.unstaged, L"mod.txt", "未暂存侧").kind == ChangeKind::modified);
  GC_CHECK(RequireItem(after.unstaged, L"del.txt", "未暂存侧").kind == ChangeKind::deleted);
  GC_CHECK(RequireItem(after.unstaged, L"brand.txt", "未暂存侧").kind == ChangeKind::untracked);
}

GC_TEST(unstage_fixture_unborn_repo_leaves_files_untracked_on_disk) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"first.txt", "one\n");
  fixture.WriteFile(L"second.txt", "two\n");
  fixture.Stage({L"first.txt", L"second.txt"});
  fixture.WriteFile(L"second.txt", "two-latest\n");  // AM：索引里是 two，磁盘上是 two-latest

  const WorkspaceModel before = Load(fixture);
  GC_REQUIRE_MESSAGE(before.staged.size() == 2, "尚无提交时暂存两条：" + Listed(before.staged));
  GC_CHECK_MESSAGE(RequireItem(before.staged, L"second.txt", "已暂存侧").statusCode == L"AM",
                   "部分暂存的条目状态字应为 AM：" + ToUtf8(before.staged[0].statusCode));
  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);

  // 前提复核（实测）：没有 HEAD 时 Git 原生的 restore --staged 走不通，
  // 而且失败得分毫未动 —— 这正是方案层换用只写索引形态的理由，不是随手加的分支。
  const GitRun native = fixture.RunInRepo({L"restore", L"--staged", L"--", L":(literal)first.txt"});
  GC_CHECK_MESSAGE(native.exitCode != 0, "无 HEAD 时 restore --staged 应当失败：" + DescribeRun(native));
  GC_CHECK_MESSAGE(native.err.find(L"could not resolve") != std::wstring::npos,
                   "应保留 Git 的原始错误：" + ToUtf8(native.err));
  GC_CHECK_MESSAGE(IndexBlobs(fixture).size() == 2, "失败的 restore 不该改动索引");

  const AppliedStaging applied =
      UnstageSelection(fixture, before, {L"first.txt", L"second.txt"}, /*repositoryHasHead=*/false);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git reset -q -- <路径> 失败：" + DescribeRun(applied.run));
  GC_CHECK(applied.plan.arguments.size() == 6);  // 全局选项 + reset + -q
  GC_CHECK(std::find(applied.plan.arguments.begin(), applied.plan.arguments.end(), L"reset") !=
           applied.plan.arguments.end());
  GC_CHECK(std::find(applied.plan.arguments.begin(), applied.plan.arguments.end(), L"restore") ==
           applied.plan.arguments.end());

  GC_CHECK_MESSAGE(IndexBlobs(fixture).empty(),
                   "两条都应离开索引：" + DescribeTable(IndexBlobs(fixture)));
  // 关键：绝不能因为“命令不好写”就去删文件。磁盘上的字节与工作区形态必须原样。
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "取消暂存不该删除或改写磁盘上的文件");
  GC_CHECK(ReadWorktreeFile(fixture, L"second.txt") == "two-latest\n");

  const WorkspaceModel after = Load(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "索引里不该再有条目：" + Listed(after.staged));
  RequirePaths(after.unstaged, {L"first.txt", L"second.txt"}, "未暂存侧");
  GC_CHECK(RequireItem(after.unstaged, L"first.txt", "未暂存侧").kind == ChangeKind::untracked);
}

GC_TEST(unstage_fixture_unborn_index_only_entry_needs_confirmation) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"keep.txt", "keep\n");
  fixture.WriteFile(L"ghost.txt", "only-in-index\n");
  fixture.Stage({L"keep.txt", L"ghost.txt"});
  std::filesystem::remove(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/ghost.txt"))));

  const WorkspaceModel before = Load(fixture);
  const ChangeItem& ghost = RequireItem(before.staged, L"ghost.txt", "已暂存侧");
  GC_REQUIRE_MESSAGE(ghost.statusCode == L"AD", "索引里唯一的一份内容应报 AD：" + ToUtf8(ghost.statusCode));

  // 这份字节只活在索引里，取消暂存就是删掉它 —— 必须让用户先点头，程序不替他决定。
  StagingPlanOptions options;
  options.repositoryHasHead = false;
  options.totalStagedItems = before.staged.size();
  const StagingPlan preview = gc::git::BuildStagingUnstagePlan({ghost}, options);
  GC_REQUIRE_MESSAGE(preview.delivery != StagingDelivery::blocked, ToUtf8(preview.blockedReason));
  GC_CHECK_MESSAGE(!preview.confirmationText.empty(), "AD 条目必须要求确认");
  GC_CHECK(preview.confirmationText.find(L"只存在于索引") != std::wstring::npos);

  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);
  const AppliedStaging applied = UnstageSelection(fixture, before, {L"ghost.txt"}, /*repositoryHasHead=*/false);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git reset -q 失败：" + DescribeRun(applied.run));

  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  // 未选中的那条留在索引里，选中的那条整个消失（磁盘上本来就没有它）。
  GC_CHECK(BlobOf(indexAfter, L"keep.txt") != "<缺失>");
  GC_CHECK(BlobOf(indexAfter, L"ghost.txt") == "<缺失>");
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "命令失败或成功都不该改动磁盘上的文件");
}

GC_TEST(unstage_fixture_rename_restores_both_paths_at_once) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"old.txt", "content\n");
  fixture.StageAll();
  fixture.Commit(L"加入 old.txt");
  fixture.RunCheckedInRepo({L"mv", L"--", L"old.txt", L"new.txt"});

  const WorkspaceModel before = Load(fixture);
  const ChangeItem& renamed = RequireItem(before.staged, L"new.txt", "已暂存侧");
  GC_REQUIRE_MESSAGE(renamed.kind == ChangeKind::renamed && renamed.oldPath == L"old.txt",
                     "应读成一条重命名：" + ToUtf8(renamed.statusCode) + " " + ToUtf8(renamed.oldPath));
  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);
  const std::map<std::wstring, std::string> head = HeadBlobs(fixture);

  const AppliedStaging applied = UnstageSelection(fixture, before, {L"new.txt"}, /*repositoryHasHead=*/true);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git restore --staged 失败：" + DescribeRun(applied.run));
  // 一条重命名条目要交出两条路径：只撤新路径会留下“旧路径仍是暂存的删除”这种半成品。
  GC_CHECK(applied.plan.pathspecCount == 2);
  GC_CHECK(applied.plan.pathspecEntries ==
           (std::vector<std::wstring>{Lit(L"old.txt"), Lit(L"new.txt")}));

  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  GC_CHECK(BlobOf(indexAfter, L"old.txt") == BlobOf(head, L"old.txt"));
  GC_CHECK(BlobOf(indexAfter, L"new.txt") == "<缺失>");
  // 改名本身发生在磁盘上，取消暂存不许把文件搬回去、也不许把新文件删掉。
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "重命名撤回不该改动工作区");

  const WorkspaceModel after = Load(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "重命名撤回后索引应与 HEAD 一致：" + Listed(after.staged));
  // 回到“未暂存侧的旧路径删除 + 新路径未跟踪”，正是「加入暂存区」那一步认得的形态。
  RequirePaths(after.unstaged, {L"old.txt", L"new.txt"}, "未暂存侧");
  GC_CHECK(RequireItem(after.unstaged, L"old.txt", "未暂存侧").kind == ChangeKind::deleted);
  GC_CHECK(RequireItem(after.unstaged, L"new.txt", "未暂存侧").kind == ChangeKind::untracked);
}

GC_TEST(unstage_fixture_rename_single_path_would_leave_half_state) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"old.txt", "content\n");
  fixture.StageAll();
  fixture.Commit(L"加入 old.txt");
  fixture.RunCheckedInRepo({L"mv", L"--", L"old.txt", L"new.txt"});

  // 反面用例（实测）：只把新路径交给 Git，旧路径那次删除会留在索引里 ——
  // 界面会显示“还有一条已暂存的删除”，这就是本步骤必须成对给两条路径的依据。
  const GitRun half = fixture.RunInRepo({L"restore", L"--staged", L"--", L":(literal)new.txt"});
  GC_CHECK_MESSAGE(half.exitCode == 0, "命令本身应成功：" + DescribeRun(half));
  const WorkspaceModel afterHalf = Load(fixture);
  GC_CHECK_MESSAGE(afterHalf.staged.size() == 1 &&
                       RequireItem(afterHalf.staged, L"old.txt", "已暂存侧").kind == ChangeKind::deleted,
                   "只撤一条会留下半套暂存：" + Listed(afterHalf.staged));

  // 补上旧路径再撤，索引才真正回到 HEAD。
  GC_CHECK_MESSAGE(
      fixture.RunInRepo({L"restore", L"--staged", L"--", L":(literal)old.txt"}).exitCode == 0,
      "第二次撤回失败");
  const WorkspaceModel afterBoth = Load(fixture);
  GC_CHECK_MESSAGE(afterBoth.staged.empty(), "两条都撤后索引应干净：" + Listed(afterBoth.staged));
  GC_CHECK_MESSAGE(IndexBlobs(fixture) == HeadBlobs(fixture), "索引应与 HEAD 的文件表逐条一致");
}

GC_TEST(unstage_fixture_special_paths_stay_literal) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  const std::vector<std::wstring> tricky = {
      L"中文 文件.txt", L"brk[x].txt",     L"100%.txt",  L"!bang.txt",
      L"#hash.txt",    L"-dash.txt",       L"sp ace.txt", L"a+b&c~d`e'f.txt",
  };
  const std::vector<std::wstring> globVictims = {L"brkx.txt", L"fresh1.txt"};
  std::vector<std::wstring> baseline(tricky);
  baseline.insert(baseline.end(), globVictims.begin(), globVictims.end());
  for (const std::wstring& name : baseline) {
    fixture.WriteFile(name, "base\n");
  }
  fixture.StageAll();
  fixture.Commit(L"特殊文件名基线");

  // 全部改成同一份内容并暂存，再只撤“麻烦名字”那几条：
  // 通配兄弟文件必须原样留在索引里，撤掉的范围一条不多、一条不少。
  for (const std::wstring& name : baseline) {
    fixture.WriteFile(name, "staged\n");
  }
  fixture.StageAll();
  const WorkspaceModel before = Load(fixture);
  GC_REQUIRE_MESSAGE(before.staged.size() == baseline.size(),
                     "应先暂存全部改动：" + Listed(before.staged));
  const std::map<std::wstring, std::string> stagedBefore = IndexBlobs(fixture);

  const AppliedStaging applied = UnstageSelection(fixture, before, tricky, /*repositoryHasHead=*/true);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git restore --staged 失败：" + DescribeRun(applied.run));

  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  const std::map<std::wstring, std::string> head = HeadBlobs(fixture);
  for (const std::wstring& name : tricky) {
    GC_CHECK_MESSAGE(BlobOf(indexAfter, name) == BlobOf(head, name),
                     "选中的这条应退回 HEAD：" + ToUtf8(name) + " " + DescribeTable(indexAfter));
  }
  for (const std::wstring& name : globVictims) {
    GC_CHECK_MESSAGE(BlobOf(indexAfter, name) == BlobOf(stagedBefore, name),
                     "未选中的兄弟文件不该被顺带撤回：" + ToUtf8(name));
    GC_CHECK(BlobOf(indexAfter, name) != BlobOf(head, name));
  }
}

GC_TEST(unstage_fixture_submodule_pointer_returns_to_head) {
  GitFixture fixture;
  PrepareFixture(fixture);
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

  // 子模块自己前进一次，父仓库把新的提交指针暂存起来。
  const std::wstring childFile = childDir + L"\\inside.txt";
  WriteFileAt(childFile, "later\n");
  fixture.RunChecked({L"add", L"-A"}, childDir);
  fixture.RunChecked({L"commit", L"-qm", L"子模块内部提交"}, childDir);
  // 再留一份内部脏改动与未跟踪文件，用来证明取消暂存不会进入子模块动它们。
  WriteFileAt(childFile, "dirty\n");
  WriteFileAt(childDir + L"\\scratch.tmp", "untracked\n");
  fixture.RunCheckedInRepo({L"add", L"--", L":(literal)child"});

  const WorkspaceModel before = Load(fixture);
  const ChangeItem& pointer = RequireItem(before.staged, L"child", "已暂存侧");
  GC_REQUIRE_MESSAGE(pointer.kind == ChangeKind::submodule,
                     "暂存的应是 gitlink：" + ToUtf8(pointer.statusCode));

  const AppliedStaging applied = UnstageSelection(fixture, before, {L"child"}, /*repositoryHasHead=*/true);
  GC_CHECK_MESSAGE(!applied.plan.confirmationText.empty(), "子模块条目必须说明只退指针");
  GC_CHECK(applied.plan.confirmationText.find(L"gitlink") != std::wstring::npos);
  GC_CHECK(applied.plan.confirmationText.find(L"不会进入 child") != std::wstring::npos);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git restore --staged 失败：" + DescribeRun(applied.run));

  // 父索引里的 gitlink 回到 HEAD 记录的那个提交 ID，父仓库没有别的改动。
  const std::map<std::wstring, std::string> head = HeadBlobs(fixture);
  GC_CHECK(BlobOf(IndexBlobs(fixture), L"child") == BlobOf(head, L"child"));
  const WorkspaceModel after = Load(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "指针撤回后父索引不该再有条目：" + Listed(after.staged));
  // 实测：子模块检出的提交没变，父仓库仍会在未暂存一侧报出这条指针改动。
  const ChangeItem& stillUnstaged = RequireItem(after.unstaged, L"child", "未暂存侧");
  GC_CHECK(stillUnstaged.kind == ChangeKind::submodule);
  GC_CHECK(stillUnstaged.submodule.commitChanged);
  // 内部文件一个字节都没被改动，未跟踪文件也还在。
  GC_CHECK(ReadFileAt(childFile) == "dirty\n");
  GC_CHECK(std::filesystem::exists(std::filesystem::path(childDir + L"\\scratch.tmp")));
  const GitRun childStatus = fixture.RunChecked({L"status", L"--porcelain=v1"}, childDir);
  GC_CHECK_MESSAGE(!childStatus.out.empty(), "子模块内部的改动必须原样保留：" + ToUtf8(childStatus.out));
}

GC_TEST(unstage_fixture_many_selected_paths_in_one_command) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  constexpr int kCount = 600;
  std::vector<std::wstring> paths;
  paths.reserve(kCount);
  for (int index = 0; index < kCount; ++index) {
    const std::wstring path =
        L"bulk/dir " + std::to_wstring(index % 5) + L"/file-" + std::to_wstring(index) + L".txt";
    fixture.WriteFile(path, "head\n");
    paths.push_back(path);
  }
  fixture.StageAll();
  fixture.Commit(L"大量文件基线");
  for (const std::wstring& path : paths) {
    fixture.WriteFile(path, "changed\n");
  }
  fixture.StageAll();

  const WorkspaceModel before = Load(fixture);
  GC_REQUIRE_MESSAGE(static_cast<long>(before.staged.size()) == kCount,
                    "应先暂存全部改动：" + std::to_string(before.staged.size()));
  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);
  // 一次点击选全部：清单形态不受 Windows 命令行长度限制（同样的路径逐个进参数要三万字符以上）。
  const AppliedStaging applied = UnstageSelection(fixture, before, paths, /*repositoryHasHead=*/true);
  GC_CHECK(applied.plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(applied.plan.pathspecCount == paths.size());
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git restore --staged 失败：" + DescribeRun(applied.run));

  GC_CHECK_MESSAGE(IndexBlobs(fixture) == HeadBlobs(fixture),
                   "全部选中项都应退回 HEAD 的文件表：" + std::to_string(IndexBlobs(fixture).size()));
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "取消暂存不该改写工作区任何文件");
  const WorkspaceModel after = Load(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "索引该干净了：" + Listed(after.staged));
  GC_CHECK_MESSAGE(after.unstaged.size() == paths.size(),
                   "改动应全部回到未暂存侧：" + std::to_string(after.unstaged.size()));
}

GC_TEST(unstage_fixture_vanished_path_fails_without_partial_unstage) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"ok.txt", "ok\n");
  fixture.WriteFile(L"extra.txt", "extra\n");
  fixture.Stage({L"ok.txt", L"extra.txt"});

  WorkspaceModel before = Load(fixture);
  GC_REQUIRE_MESSAGE(before.staged.size() == 2, "应先读到两条已暂存：" + Listed(before.staged));
  // 制造“列表里有、索引里已经没有了”的条目：另一个进程把它从索引里撤掉了。
  fixture.RunCheckedInRepo({L"rm", L"--cached", L"-q", L"--", L":(literal)extra.txt"});
  std::filesystem::remove(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/extra.txt"))));

  const AppliedStaging applied = UnstageSelection(fixture, before, {L"ok.txt", L"extra.txt"},
                                                 /*repositoryHasHead=*/true);
  GC_CHECK_MESSAGE(applied.run.exitCode != 0, "路径消失必须失败，而不是静默跳过：" + DescribeRun(applied.run));
  GC_CHECK_MESSAGE(applied.run.err.find(L"did not match") != std::wstring::npos,
                   "应保留 Git 的原始错误：" + ToUtf8(applied.run.err));
  // 全有或全无：合法的那一条也没有被“顺带”撤回一半。
  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  GC_CHECK_MESSAGE(BlobOf(indexAfter, L"ok.txt") != "<缺失>",
                   "ok.txt 应仍留在索引里：" + DescribeTable(indexAfter));
  const WorkspaceModel after = Load(fixture);
  GC_CHECK_MESSAGE(RequireItem(after.staged, L"ok.txt", "已暂存侧").kind == ChangeKind::added,
                   "未撤回的那条应仍是暂存的新增：" + Listed(after.staged));
}

GC_TEST(unstage_fixture_old_git_inline_mode_reaches_same_index) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"a.txt", "base\n");
  fixture.WriteFile(L"brk[x].txt", "base\n");
  fixture.WriteFile(L"brkx.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"a.txt", "new\n");
  fixture.WriteFile(L"brk[x].txt", "new\n");
  fixture.WriteFile(L"brkx.txt", "new\n");  // 未选中
  fixture.WriteFile(L"fresh 文件.txt", "untracked\n");
  fixture.StageAll();

  const WorkspaceModel before = Load(fixture);
  // 模拟旧 Git（没有清单接口）：同一份选择改用字面 pathspec 参数形态，结果必须完全一致。
  const AppliedStaging applied =
      UnstageSelection(fixture, before, {L"a.txt", L"brk[x].txt", L"fresh 文件.txt"},
                       /*repositoryHasHead=*/true, /*pathspecFileSupported=*/false);
  GC_CHECK(applied.plan.delivery == StagingDelivery::inlinePathspec);
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git restore --staged 失败：" + DescribeRun(applied.run));

  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  const std::map<std::wstring, std::string> head = HeadBlobs(fixture);
  GC_CHECK(BlobOf(indexAfter, L"a.txt") == BlobOf(head, L"a.txt"));
  GC_CHECK(BlobOf(indexAfter, L"brk[x].txt") == BlobOf(head, L"brk[x].txt"));
  GC_CHECK(BlobOf(indexAfter, L"fresh 文件.txt") == "<缺失>");
  GC_CHECK(BlobOf(indexAfter, L"brkx.txt") != BlobOf(head, L"brkx.txt"));  // 仍是暂存的那一份

  // 旧 Git 的无 HEAD 仓库：参数形态同样只写索引，文件留在磁盘上。
  GitFixture unborn;
  PrepareFixture(unborn);
  unborn.InitRepository(L"repo");
  unborn.WriteFile(L"first.txt", "one\n");
  unborn.Stage({L"first.txt"});
  const WorkspaceModel beforeUnborn = Load(unborn);
  const AppliedStaging inlineUnborn =
      UnstageSelection(unborn, beforeUnborn, {L"first.txt"}, /*repositoryHasHead=*/false,
                       /*pathspecFileSupported=*/false);
  GC_CHECK(inlineUnborn.plan.delivery == StagingDelivery::inlinePathspec);
  GC_CHECK_MESSAGE(inlineUnborn.run.exitCode == 0, "git reset -q 失败：" + DescribeRun(inlineUnborn.run));
  GC_CHECK_MESSAGE(IndexBlobs(unborn).empty(), "索引里的这条应已离开：" + DescribeTable(IndexBlobs(unborn)));
  GC_CHECK(ReadWorktreeFile(unborn, L"first.txt") == "one\n");  // 文件没有被删掉
}

GC_TEST(unstage_fixture_reset_form_reaches_the_same_index_when_head_exists) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"a.txt", "base\n");
  fixture.WriteFile(L"b.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"a.txt", "new a\n");
  fixture.WriteFile(L"b.txt", "new b\n");
  std::filesystem::remove(std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/b.txt"))));
  fixture.Stage({L"a.txt", L"b.txt"});

  const WorkspaceModel before = Load(fixture);
  const std::map<std::wstring, std::string> worktreeBefore = SnapshotWorktree(fixture);
  // 有 HEAD 但 Git 太旧没有 restore 子命令（2.23 起才有）：带路径的 reset 走同一条只写索引的路。
  const AppliedStaging applied =
      UnstageSelection(fixture, before, {L"a.txt", L"b.txt"}, /*repositoryHasHead=*/true,
                       /*pathspecFileSupported=*/true, /*restoreCommandSupported=*/false);
  GC_CHECK(std::find(applied.plan.arguments.begin(), applied.plan.arguments.end(), L"reset") !=
           applied.plan.arguments.end());
  GC_CHECK_MESSAGE(applied.run.exitCode == 0, "git reset -q 失败：" + DescribeRun(applied.run));

  const std::map<std::wstring, std::string> indexAfter = IndexBlobs(fixture);
  const std::map<std::wstring, std::string> head = HeadBlobs(fixture);
  GC_CHECK(BlobOf(indexAfter, L"a.txt") == BlobOf(head, L"a.txt"));
  // b.txt 那次“已暂存的删除”同样退回 HEAD：索引里重新有它，磁盘上依然没有（不会被凭空写出来）。
  GC_CHECK(BlobOf(indexAfter, L"b.txt") == BlobOf(head, L"b.txt"));
  GC_CHECK_MESSAGE(IndexBlobs(fixture) == head,
                   "索引的文件表应与 HEAD 逐条一致：" + DescribeTable(indexAfter));
  GC_CHECK_MESSAGE(SnapshotWorktree(fixture) == worktreeBefore, "带路径的 reset 不该改动工作区");
  const WorkspaceModel after = Load(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "两条都应离开暂存侧：" + Listed(after.staged));
  RequirePaths(after.unstaged, {L"a.txt", L"b.txt"}, "未暂存侧");
  GC_CHECK(RequireItem(after.unstaged, L"a.txt", "未暂存侧").kind == ChangeKind::modified);
  GC_CHECK(RequireItem(after.unstaged, L"b.txt", "未暂存侧").kind == ChangeKind::deleted);
}
