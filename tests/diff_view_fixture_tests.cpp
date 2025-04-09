// 双击查看差异/内容的集成测试：用真实临时仓库跑出工作区状态，把生产代码构造出来的
// Git 参数数组原样交给真实 Git 执行，核对「命令窗口里会出现什么」。
// 覆盖：部分暂存时两侧内容各不相同、删除、新增、重命名必须带旧路径、字面 pathspec 不过度命中、
// 未跟踪文件的内容显示与二进制提示、初始提交之前的暂存差异、冲突条目的组合差异、
// 子模块只给提交指针，以及一整轮预览之后索引内容与工作区文件分毫未动。
// 只在夹具自建的临时仓库里跑 Git，绝不接触真实仓库或远端。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "git/diff_view.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::ChangeSide;
using gc::git::DiffViewKind;
using gc::git::DiffViewPlan;
using gc::git::WorktreeFileFacts;
using gc::git::WorkspaceModel;
using gc::git::WorkspaceStatusParseResult;
using gc::test::GitFixture;
using gc::test::GitRun;

std::string Narrow(std::wstring_view text) { return gc::platform::Utf16ToUtf8(std::wstring(text)); }

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

bool Has(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

WorktreeFileFacts TextFacts(unsigned long long size = 64ULL) {
  WorktreeFileFacts facts;
  facts.probed = true;
  facts.exists = true;
  facts.readable = true;
  facts.sizeBytes = size;
  return facts;
}

WorktreeFileFacts BinaryFacts() {
  WorktreeFileFacts facts = TextFacts();
  facts.containsNullByte = true;
  return facts;
}

std::string ReadBytes(std::wstring_view absolutePath) {
  std::ifstream file(std::filesystem::path(absolutePath), std::ios::binary);
  if (!file.is_open()) {
    return std::string("<读不到>");
  }
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 用生产解析器从真实 git status 取条目：喂给 BuildDiffViewPlan 的 kind/oldPath/子模块位
// 全是 Git 自己的回答，而不是测试手搓的近似值。workingDirectory 为相对夹具根的路径。
// 夹具的 Run 在 workingDirectory 为空时落在**临时根**而不是仓库目录，
// 因此“就在当前仓库里执行”必须显式走 RunInRepo，否则 Git 会报“不是仓库”。
GitRun RunAt(GitFixture& fixture, const std::vector<std::wstring>& arguments,
             std::wstring_view workingDirectory) {
  return workingDirectory.empty() ? fixture.RunInRepo(arguments) : fixture.Run(arguments, workingDirectory);
}

WorkspaceModel LoadWorkspace(GitFixture& fixture, std::wstring_view workingDirectory) {
  const std::wstring root = workingDirectory.empty() ? fixture.RepoDir()
                                                    : fixture.PathInRoot(workingDirectory);
  const GitRun run = RunAt(fixture, gc::git::BuildWorkspaceStatusArguments(root), workingDirectory);
  GC_REQUIRE_MESSAGE(run.exitCode == 0, "读取工作区状态失败：" + Narrow(run.err));
  const WorkspaceStatusParseResult parsed = gc::git::ParseWorkspacePorcelainV2(run.out);
  GC_REQUIRE_MESSAGE(parsed.error.empty(), "porcelain v2 解析失败：" + Narrow(parsed.error));
  return parsed.model;
}

const ChangeItem* Find(const std::vector<ChangeItem>& items, std::wstring_view path) {
  for (const ChangeItem& item : items) {
    if (item.path == path) {
      return &item;
    }
  }
  return nullptr;
}

// 必需条目缺失就直接中止用例：后面的断言全建立在「Git 确实这样回答了」之上。
const ChangeItem& RequireItem(const std::vector<ChangeItem>& items, std::wstring_view path,
                              const std::string& label) {
  const ChangeItem* found = Find(items, path);
  if (found == nullptr) {
    // 把这一侧实际有哪些条目一起报出来：否者只说“缺少条目”根本分不清是
    // Git 没回答、解析没对上，还是测试自己把侧别搞反了。
    std::string seen;
    for (const ChangeItem& item : items) {
      seen += "(" + Narrow(item.path) + "/" + Narrow(item.StatusLabel()) + ")";
    }
    GC_REQUIRE_MESSAGE(false, label + " 缺少条目：" + Narrow(path) + " 该侧实际有：" + seen);
  }
  return *found;
}

// 把生产方案原样交给真实 Git（工作目录=仓库根，与命令窗口执行器给的落点一致）。
GitRun RunPlan(GitFixture& fixture, const DiffViewPlan& plan, std::wstring_view workingDirectory) {
  GC_REQUIRE_MESSAGE(!plan.arguments.empty(), "方案没有参数，不该执行");
  return RunAt(fixture, plan.arguments, workingDirectory);
}

std::string Describe(const GitRun& run) {
  return "exit=" + std::to_string(run.exitCode) + " out=[" + Narrow(run.out) + "] err=[" +
         Narrow(run.err) + "]";
}

}  // namespace

GC_TEST(diff_view_real_two_sides_show_different_content) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"partial.txt", "A\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  // 暂存一次改动后继续编辑：同一文件同时出现在两侧（AGENTS 明确要求两侧各看各的）。
  fixture.WriteFile(L"partial.txt", "B\n");
  fixture.Stage({L"partial.txt"});
  fixture.WriteFile(L"partial.txt", "C\n");

  const WorkspaceModel model = LoadWorkspace(fixture, {});
  const ChangeItem& unstaged = RequireItem(model.unstaged, L"partial.txt", "未暂存侧");
  const ChangeItem& staged = RequireItem(model.staged, L"partial.txt", "已暂存侧");

  const GitRun worktree =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, unstaged, {}), {});
  const GitRun index =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::staged, staged, {}), {});
  // 实测：git diff 找到差异时仍返回 0（只有 --exit-code 才改成非 0）。
  GC_CHECK_MESSAGE(worktree.exitCode == 0, "未暂存侧退出码应为 0：" + Describe(worktree));
  GC_CHECK_MESSAGE(index.exitCode == 0, "已暂存侧退出码应为 0：" + Describe(index));
  // 未暂存侧 = 索引(B) -> 工作区(C)；已暂存侧 = HEAD(A) -> 索引(B)。绝不混成一次完整 diff。
  GC_CHECK_MESSAGE(Has(worktree.out, L"-B") && Has(worktree.out, L"+C"),
                   "未暂存侧应是 B->C：" + Narrow(worktree.out));
  GC_CHECK_MESSAGE(!Has(worktree.out, L"-A"), "未暂存侧不该混入 HEAD 的 A：" + Narrow(worktree.out));
  GC_CHECK_MESSAGE(Has(index.out, L"-A") && Has(index.out, L"+B"),
                   "已暂存侧应是 A->B：" + Narrow(index.out));
  GC_CHECK_MESSAGE(!Has(index.out, L"+C"), "已暂存侧不该混入工作区的 C：" + Narrow(index.out));
  GC_CHECK(worktree.out != index.out);
}

GC_TEST(diff_view_real_deleted_added_and_rename) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"gone.txt", "contents to lose\n");
  fixture.WriteFile(L"old name.txt", "rename me\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  // 工作区删除（未暂存）+ 新文件已暂存 + git mv 产生的已暂存重命名。
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(fixture.PathInRoot(L"repo/gone.txt")), ec);
  GC_REQUIRE_MESSAGE(!ec, "删除工作区文件失败");
  fixture.WriteFile(L"fresh.txt", "brand new\n");
  fixture.Stage({L"fresh.txt"});
  fixture.RunChecked({L"mv", L"old name.txt", L"new name.txt"}, L"repo");

  const WorkspaceModel model = LoadWorkspace(fixture, {});

  // 删除项：走 git diff 的删除差异。传空的事实也照样出参数 —— 程序不去打开已不存在的路径。
  const ChangeItem& deleted = RequireItem(model.unstaged, L"gone.txt", "未暂存侧");
  GC_CHECK(deleted.kind == ChangeKind::deleted);
  const GitRun deletion =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, deleted, {}), {});
  GC_CHECK_MESSAGE(Has(deletion.out, L"deleted file mode"), Describe(deletion));
  GC_CHECK_MESSAGE(Has(deletion.out, L"+++ /dev/null"),
                   "删除差异的右侧应是 /dev/null：" + Narrow(deletion.out));
  GC_CHECK_MESSAGE(Has(deletion.out, L"-contents to lose"), Narrow(deletion.out));

  // 已暂存的新文件：new file mode + 内容。
  const ChangeItem& added = RequireItem(model.staged, L"fresh.txt", "已暂存侧");
  GC_CHECK(added.kind == ChangeKind::added);
  const GitRun addition =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::staged, added, {}), {});
  GC_CHECK_MESSAGE(Has(addition.out, L"new file mode"), Describe(addition));
  GC_CHECK_MESSAGE(Has(addition.out, L"+brand new"), Narrow(addition.out));

  // 重命名：必须带旧路径，否则 Git 把它显示成「新增文件」，与列表上的「重命名」自相矛盾。
  const ChangeItem& renamed = RequireItem(model.staged, L"new name.txt", "已暂存侧");
  GC_REQUIRE_MESSAGE(renamed.kind == ChangeKind::renamed,
                    "应识别为重命名，实际 kind=" + Narrow(renamed.StatusLabel()));
  GC_CHECK(renamed.oldPath == L"old name.txt");
  const GitRun rename =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::staged, renamed, {}), {});
  GC_CHECK_MESSAGE(Has(rename.out, L"rename from old name.txt"), Describe(rename));
  GC_CHECK_MESSAGE(Has(rename.out, L"rename to new name.txt"), Describe(rename));

  // 反证：只给新路径时确实看不到 rename —— 这正是方案要带两个 pathspec 的理由。
  const std::vector<std::wstring> onlyNew{L"--no-optional-locks", L"diff", L"--cached",
                                          L"--find-renames", L"--",
                                          gc::git::MakeLiteralPathspec(L"new name.txt")};
  const GitRun oneSided = fixture.Run(onlyNew, L"repo");
  GC_CHECK_MESSAGE(!Has(oneSided.out, L"rename to"),
                   "只给新路径时不该出现 rename：" + Narrow(oneSided.out));
  GC_CHECK_MESSAGE(Has(oneSided.out, L"new file mode"), Narrow(oneSided.out));
}

GC_TEST(diff_view_real_literal_pathspec_does_not_over_match) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  // Windows 合法、但会被 glob 当成字符类的档名。
  fixture.WriteFile(L"a[b].txt", "one\n");
  fixture.WriteFile(L"ab.txt", "one\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"a[b].txt", "two\n");
  fixture.WriteFile(L"ab.txt", "two\n");

  const WorkspaceModel model = LoadWorkspace(fixture, {});
  const ChangeItem& bracket = RequireItem(model.unstaged, L"a[b].txt", "未暂存侧");
  const GitRun literal =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, bracket, {}), {});
  GC_CHECK_MESSAGE(Has(literal.out, L"a[b].txt"), Describe(literal));
  GC_CHECK_MESSAGE(!Has(literal.out, L"diff --git a/ab.txt "),
                   "字面 pathspec 不该连带命中 ab.txt：" + Narrow(literal.out));

  // 反证：不带 :(literal) 时 [b] 被当字符类，一次查看会把别人的改动一起倒进窗口。
  const std::vector<std::wstring> globbed{L"--no-optional-locks", L"diff", L"--", L"a[b].txt"};
  const GitRun glob = fixture.Run(globbed, L"repo");
  GC_CHECK_MESSAGE(Has(glob.out, L"diff --git a/ab.txt "),
                   "对照场景应显示过度命中：" + Narrow(glob.out));
}

GC_TEST(diff_view_real_untracked_content_and_binary_notice) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"note dir/新建 文件.txt", "first line\nsecond line\n");
  fixture.WriteFile(L"empty-untracked.txt", "");
  // 带 ASCII 标记的“二进制”档案：断言标记不会作为文字被倒进窗口。
  std::string bytes;
  bytes.append("\x00\x01\x02\x00", 4);
  bytes += "BINARYMARKERSHOULDNOTBEDUMPED\n";
  bytes.append("\x00\xff", 2);
  fixture.WriteFile(L"asset.bin", bytes);

  const WorkspaceModel model = LoadWorkspace(fixture, {});
  const ChangeItem& text = RequireItem(model.unstaged, L"note dir/新建 文件.txt", "未暂存侧");
  GC_REQUIRE_MESSAGE(text.kind == ChangeKind::untracked, "该条目应是未跟踪，实际 kind=" + Narrow(text.StatusLabel()));
  const GitRun content =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, text, TextFacts()), {});
  // --no-index 找到差异返回 1（实测），界面按“正常完成”解释，不能说成失败。
  GC_CHECK_MESSAGE(content.exitCode == 1, Describe(content));
  GC_CHECK_MESSAGE(Has(content.out, L"--- /dev/null"), Narrow(content.out));
  GC_CHECK_MESSAGE(Has(content.out, L"+first line") && Has(content.out, L"+second line"),
                   "未跟踪文件应显示内容：" + Narrow(content.out));

  const ChangeItem& binary = RequireItem(model.unstaged, L"asset.bin", "未暂存侧");
  const GitRun binaryRun =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, binary, BinaryFacts()), {});
  GC_CHECK_MESSAGE(Has(binaryRun.out, L"Binary files"), Describe(binaryRun));
  GC_CHECK_MESSAGE(!Has(binaryRun.out, L"BINARYMARKERSHOULDNOTBEDUMPED"),
                   "二进制内容不应作为文字输出：" + Narrow(binaryRun.out));

  // 档案在外部被删除时：Git 报「Could not access」，退出码与「有差异」完全相同（都是 1），
  // 靠退出码分不出来 —— 所以界面必须在开窗口之前自己确认档案还在（blocked 分支）。
  const ChangeItem& empty = RequireItem(model.unstaged, L"empty-untracked.txt", "未暂存侧");
  const std::vector<std::wstring> missing{L"--no-optional-locks", L"diff", L"--no-index", L"--",
                                          gc::git::kEmptySidePath, L"note dir/不存在.txt"};
  const GitRun missingRun = fixture.Run(missing, L"repo");
  GC_CHECK_MESSAGE(missingRun.exitCode == 1 && Has(missingRun.err, L"Could not access"),
                   Describe(missingRun));
  WorktreeFileFacts gone;
  gone.probed = true;  // exists 保持 false
  const DiffViewPlan gonePlan = gc::git::BuildDiffViewPlan(ChangeSide::unstaged, empty, gone);
  GC_CHECK(gonePlan.kind == DiffViewKind::blocked);
  GC_CHECK(gonePlan.arguments.empty());
}

GC_TEST(diff_view_real_staged_diff_works_before_initial_commit) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"first 文件.txt", "first ever\n");
  fixture.Stage({L"first 文件.txt"});
  GC_REQUIRE_MESSAGE(fixture.HeadSha().empty(), "本用例的前提是 HEAD 还不可解析");

  const WorkspaceModel model = LoadWorkspace(fixture, {});
  const ChangeItem& staged = RequireItem(model.staged, L"first 文件.txt", "已暂存侧");
  const GitRun run =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::staged, staged, {}), {});
  // Git 以空树为基准：初始提交之前暂存的新文件照样能看（实测）。
  GC_CHECK_MESSAGE(run.exitCode == 0, Describe(run));
  GC_CHECK_MESSAGE(Has(run.out, L"new file mode"), Narrow(run.out));
  GC_CHECK_MESSAGE(Has(run.out, L"+first ever"), Narrow(run.out));
  GC_CHECK_MESSAGE(Has(run.out, L"first 文件.txt"), Narrow(run.out));
}

GC_TEST(diff_view_real_conflicted_item_yields_combined_diff) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"c.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.RunChecked({L"checkout", L"-b", L"side"}, L"repo");
  fixture.WriteFile(L"c.txt", "THEIRS\n");
  fixture.StageAll();
  fixture.Commit(L"theirs");
  fixture.RunChecked({L"checkout", L"main"}, L"repo");
  fixture.WriteFile(L"c.txt", "OURS\n");
  fixture.StageAll();
  fixture.Commit(L"ours");
  // 合并必然冲突：非 0 退出是用例预期，因此用 Run 而不是 RunChecked。
  const GitRun merge = fixture.Run({L"merge", L"--no-edit", L"side"}, L"repo");
  GC_REQUIRE_MESSAGE(merge.exitCode != 0 && Has(merge.out, L"CONFLICT"), Describe(merge));

  const WorkspaceModel model = LoadWorkspace(fixture, {});
  const ChangeItem& conflicted = RequireItem(model.unstaged, L"c.txt", "未暂存侧");
  GC_REQUIRE_MESSAGE(conflicted.kind == ChangeKind::conflicted,
                    "未合并项应作为一条冲突条目落在未暂存侧");
  const GitRun worktree =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, conflicted, {}), {});
  GC_CHECK_MESSAGE(worktree.exitCode == 0, Describe(worktree));
  // 组合差异里带着工作区文件中的冲突标记：不是某一侧的干净差异，但正是解决冲突要看的。
  GC_CHECK_MESSAGE(Has(worktree.out, L"diff --cc"),
                   "冲突条目应输出组合差异：" + Narrow(worktree.out));
  GC_CHECK_MESSAGE(Has(worktree.out, L"<<<<"), Narrow(worktree.out));

  // 同一档案若从已暂存侧查看，Git 如实说它未合并，而不是伪造一份差异。
  const std::vector<std::wstring> cached{L"--no-optional-locks", L"diff", L"--cached", L"--no-ext-diff",
                                         L"--no-textconv", L"--ignore-submodules=none",
                                         L"--submodule=short", L"--",
                                         gc::git::MakeLiteralPathspec(L"c.txt")};
  const GitRun index = fixture.Run(cached, L"repo");
  GC_CHECK_MESSAGE(Has(index.out, L"Unmerged path"), Describe(index));

  // 合并现场必须原样保留：只读查看绝不替用户 reset/abort。
  const GitRun mergeHead =
      fixture.Run({L"rev-parse", L"-q", L"--verify", L"MERGE_HEAD"}, L"repo");
  GC_CHECK_MESSAGE(mergeHead.exitCode == 0, "只读查看不应结束合并现场");
}

GC_TEST(diff_view_real_submodule_shows_pointer_only) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 先建「子模块仓库」，再建父仓库并把前者作为子模块加入。
  fixture.InitRepository(L"inner");
  fixture.WriteFile(L"inside.txt", "inside v1\n");
  fixture.StageAll();
  fixture.Commit(L"子模块首次提交");
  const std::wstring innerDirectory = fixture.RepoDir();

  fixture.InitRepository(L"parent");
  fixture.RunChecked({L"-c", L"protocol.file.allow=always", L"submodule", L"add", innerDirectory, L"sub"},
                     L"parent");
  fixture.RunChecked({L"commit", L"-m", L"加入子模块"}, L"parent");
  // 在父仓库看到的子模块工作区里改内容并提交：提交指针移动，同时留下未跟踪文件。
  fixture.WriteFile(L"sub/inside.txt", "inside v2\n");
  fixture.WriteFile(L"sub/untracked-inside.txt", "not mine\n");
  fixture.RunChecked({L"commit", L"-am", L"子模块第二次提交"}, L"parent/sub");

  const WorkspaceModel model = LoadWorkspace(fixture, L"parent");
  // 父仓库只有一条 gitlink 记录，内部文件绝不拆成可操作的条目（既定设计）。
  GC_CHECK_MESSAGE(Find(model.unstaged, L"sub/untracked-inside.txt") == nullptr,
                   "父仓库列表不应出现子模块内部文件");
  GC_CHECK_MESSAGE(Find(model.staged, L"sub/untracked-inside.txt") == nullptr,
                   "父仓库列表不应出现子模块内部文件（已暂存侧）");
  const ChangeItem& item = RequireItem(model.unstaged, L"sub", "未暂存侧");
  GC_REQUIRE_MESSAGE(item.kind == ChangeKind::submodule,
                    "应识别为子模块，实际 kind=" + Narrow(item.StatusLabel()));
  GC_CHECK(item.submodule.commitChanged);

  const GitRun run = RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::unstaged, item, {}), L"parent");
  GC_CHECK_MESSAGE(Has(run.out, L"Subproject commit"), Describe(run));
  // --submodule=short 的实际效果：父仓库的一次查看不递归进子模块列内部文件差异。
  GC_CHECK_MESSAGE(!Has(run.out, L"sub/inside.txt"),
                   "父仓库的子模块差异里不应出现内部文件：" + Narrow(run.out));

  // 只暂存提交指针之后，已暂存侧看到的还是那一条指针差异（父仓库暂存子模块的范围仅此）。
  fixture.RunChecked({L"add", L"--", L"sub"}, L"parent");
  const WorkspaceModel stagedModel = LoadWorkspace(fixture, L"parent");
  const ChangeItem& stagedItem = RequireItem(stagedModel.staged, L"sub", "已暂存侧");
  const GitRun stagedRun =
      RunPlan(fixture, gc::git::BuildDiffViewPlan(ChangeSide::staged, stagedItem, {}), L"parent");
  GC_CHECK_MESSAGE(Has(stagedRun.out, L"Subproject commit"), Describe(stagedRun));
  GC_CHECK_MESSAGE(!Has(stagedRun.out, L"sub/inside.txt"), Narrow(stagedRun.out));
}

GC_TEST(diff_view_real_preview_leaves_repository_untouched) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"both.txt", "A\n");
  fixture.WriteFile(L"kept.txt", "keep\n");
  fixture.WriteFile(L"gone.txt", "will vanish\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  // 让 both.txt 同时出现在两侧（两侧都要预览一次），并留下删除、未跟踪与已暂存重命名。
  fixture.WriteFile(L"both.txt", "B\n");
  fixture.Stage({L"both.txt"});
  fixture.WriteFile(L"both.txt", "C\n");
  fixture.WriteFile(L"untracked.txt", "look at me\n");
  fixture.RunChecked({L"mv", L"kept.txt", L"moved.txt"}, L"repo");
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(fixture.PathInRoot(L"repo/gone.txt")), ec);

  // 预览前的凭据：索引内容（条目+blob，不含 stat 缓存）、工作区状态全文、HEAD、档案字节。
  const std::wstring indexEntries =
      fixture.RunChecked({L"ls-files", L"--stage"}, L"repo").out;
  const std::wstring porcelainBefore =
      fixture.RunChecked(gc::git::BuildWorkspaceStatusArguments(fixture.RepoDir()), L"repo").out;
  const std::wstring headBefore = fixture.HeadSha();
  const std::string untrackedBytes = ReadBytes(fixture.PathInRoot(L"repo/untracked.txt"));
  const std::string bothBytes = ReadBytes(fixture.PathInRoot(L"repo/both.txt"));

  const WorkspaceModel model = LoadWorkspace(fixture, {});
  struct Target {
    ChangeSide side;
    std::wstring path;
  };
  const std::vector<Target> targets = {
      {ChangeSide::unstaged, L"both.txt"},
      {ChangeSide::staged, L"both.txt"},
      {ChangeSide::unstaged, L"gone.txt"},
      {ChangeSide::unstaged, L"untracked.txt"},
      {ChangeSide::staged, L"moved.txt"},
  };
  for (const Target& target : targets) {
    const std::vector<ChangeItem>& items =
        target.side == ChangeSide::staged ? model.staged : model.unstaged;
    const ChangeItem& item = RequireItem(items, target.path, Narrow(target.path));
    const WorktreeFileFacts facts =
        item.kind == ChangeKind::untracked ? TextFacts() : WorktreeFileFacts{};
    const GitRun run =
        RunPlan(fixture, gc::git::BuildDiffViewPlan(target.side, item, facts), {});
    // 0（无差异）与 1（--no-index 有差异）都属正常完成，绝不该出现用法错误之类的失败码。
    GC_CHECK_MESSAGE(run.exitCode <= 1,
                     Narrow(target.path) + " 的预览命令退出码异常：" + Describe(run));
  }

  // 索引内容与工作区状态必须一模一样。
  // 注：Git 可能刷新它自己的 stat 缓存（实测 `--no-optional-locks` 也挡不住），
  // 那不改变暂存了哪些内容与文件字节，因此比较的是条目与 blob，而不是 .git/index 的字节。
  GC_CHECK_MESSAGE(indexEntries == fixture.RunChecked({L"ls-files", L"--stage"}, L"repo").out,
                   "索引内容被改动了");
  GC_CHECK_MESSAGE(porcelainBefore ==
                       fixture.RunChecked(gc::git::BuildWorkspaceStatusArguments(fixture.RepoDir()), L"repo")
                           .out,
                   "工作区状态被改动了");
  GC_CHECK_MESSAGE(headBefore == fixture.HeadSha(), "HEAD 被改动了");
  GC_CHECK_MESSAGE(untrackedBytes == ReadBytes(fixture.PathInRoot(L"repo/untracked.txt")),
                   "未跟踪文件内容被改动了");
  GC_CHECK_MESSAGE(bothBytes == ReadBytes(fixture.PathInRoot(L"repo/both.txt")), "工作区文件内容被改动了");

  // 不留 Git 锁文件：只读预览绝不该把仓库留在「正在被改动」的形态上。
  std::vector<std::string> foundLocks;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(std::filesystem::path(fixture.PathInRoot(L"repo/.git")))) {
    if (entry.is_regular_file() && entry.path().extension() == L".lock") {
      foundLocks.push_back(entry.path().filename().string());
    }
  }
  GC_CHECK_MESSAGE(foundLocks.empty(), "残留锁文件：" + [&foundLocks] {
    std::string text;
    for (const std::string& name : foundLocks) {
      text += name + " ";
    }
    return text;
  }());
}
