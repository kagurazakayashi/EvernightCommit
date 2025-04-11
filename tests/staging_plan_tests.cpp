// 「加入暂存区」与「← 移出暂存区」方案构造的纯逻辑测试：不碰文件系统、不起 Git，只断言
// 「选中的条目 → 交给 Git 的范围」这一件事。
// 覆盖：空选择绝不退化成全量、只处理选中的条目、重命名成对给出旧+新路径、
// 路径与条目类别的准入校验、未合并与子模块的确认文案、
// 清单文件/字面 pathspec 两种传递形态的选择与旧版本的拒绝策略，以及 Git 版本判定本身。
// 取消暂存一侧还覆盖：有无 HEAD 时的命令选择、只写索引的命令形态、
// 以及「绝不出现 --hard / 不带路径的 git reset / 取消全部暂存」这一条底线。
#include <algorithm>
#include <string>
#include <vector>

#include "git/staging_plan.h"
#include "git/workspace_model.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::StagingDelivery;
using gc::git::StagingPlan;
using gc::git::StagingPlanOptions;

ChangeItem Make(ChangeKind kind, std::wstring_view path, std::wstring_view oldPath = {}) {
  ChangeItem item;
  item.kind = kind;
  item.statusCode = L"1";
  item.path = std::wstring(path);
  item.oldPath = std::wstring(oldPath);
  return item;
}

// 已暂存条目：状态栏按 porcelain v2 的 XY 原样给（X 是索引相对 HEAD，Y 是工作区相对索引），
// 「内容只剩索引里那一份」的判定就落在这两个字元上。
ChangeItem MakeStaged(ChangeKind kind, std::wstring_view path, std::wstring_view statusCode,
                      std::wstring_view oldPath = {}) {
  ChangeItem item = Make(kind, path, oldPath);
  item.statusCode = std::wstring(statusCode);
  return item;
}

bool HasArgument(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

bool HasEntry(const std::vector<std::wstring>& entries, std::wstring_view value) {
  return std::find(entries.begin(), entries.end(), std::wstring(value)) != entries.end();
}

size_t EntryCount(const std::vector<std::wstring>& entries, std::wstring_view value) {
  return static_cast<size_t>(
      std::count(entries.begin(), entries.end(), std::wstring(value)));
}

StagingPlanOptions FileMode() {
  StagingPlanOptions options;
  options.pathspecFileSupported = true;
  return options;
}

StagingPlanOptions InlineMode() {
  StagingPlanOptions options;
  options.pathspecFileSupported = false;
  return options;
}

const std::wstring kDot = L".";
const std::wstring kAll = L"-A";
const std::wstring kAllLong = L"--all";
const std::wstring kUpdate = L"-u";
const std::wstring kDotSlash = L"./";

// 方案收集到的每一条都是字面 pathspec 元素：实测 NUL 分隔只关闭引号转义、不关闭 glob，
// 所以清单与参数里的每一项都必须自带 :(literal) 前缀。
std::wstring Lit(std::wstring_view path) { return std::wstring(L":(literal)") + std::wstring(path); }

// 任何一条“全仓库范围”的形态都不许出现在方案里。
GC_TEST(staging_never_uses_repository_wide_arguments) {
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::modified, L"a.txt"),
      Make(ChangeKind::deleted, L"b.txt"),
      Make(ChangeKind::untracked, L"c.txt"),
  };
  for (const StagingPlan& plan : {gc::git::BuildStagingAddPlan(selection, FileMode()),
                                 gc::git::BuildStagingAddPlan(selection, InlineMode())}) {
    GC_CHECK_MESSAGE(plan.delivery != StagingDelivery::blocked, "两种形态都应可执行");
    GC_CHECK(!HasArgument(plan.arguments, kDot));
    GC_CHECK(!HasArgument(plan.arguments, kAll));
    GC_CHECK(!HasArgument(plan.arguments, kAllLong));
    GC_CHECK(!HasArgument(plan.arguments, kUpdate));
    GC_CHECK(!HasArgument(plan.arguments, kDotSlash));
    GC_CHECK(!HasEntry(plan.pathspecEntries, kDot));
    // -p / -i 这类交互形态同样不许：命令窗口里跑一次就得给出结果，不能等按键。
    GC_CHECK(!HasArgument(plan.arguments, L"-p"));
    GC_CHECK(!HasArgument(plan.arguments, L"-i"));
    GC_CHECK(HasArgument(plan.arguments, L"add"));
  }
}

GC_TEST(staging_empty_selection_is_refused_not_treated_as_stage_all) {
  const StagingPlan plan = gc::git::BuildStagingAddPlan({}, FileMode());
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.pathspecEntries.empty());
  GC_CHECK(plan.selectedItems == 0);
  GC_CHECK(!plan.blockedReason.empty());
  // 说明里必须把“不会替你暂存全部”讲明白，而不是只说一句“未选择”。
  GC_CHECK(plan.blockedReason.find(L"没有选中") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"绝不会") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"git add .") != std::wstring::npos);
}

GC_TEST(staging_selection_only_pathspecs_of_selected_items) {
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::modified, L"docs/readme.md"),
      Make(ChangeKind::deleted, L"src/old.cpp"),
      Make(ChangeKind::typeChange, L"link"),
      Make(ChangeKind::untracked, L"new file with space.txt"),
  };
  StagingPlanOptions options = FileMode();
  options.totalUnstagedItems = 9;  // 另有 5 项未选中
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, options);

  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(plan.selectedItems == 4);
  GC_CHECK(plan.pathspecCount == 4);
  GC_CHECK(plan.pathspecEntries ==
           (std::vector<std::wstring>{Lit(L"docs/readme.md"), Lit(L"src/old.cpp"), Lit(L"link"),
                                      Lit(L"new file with space.txt")}));
  GC_CHECK(plan.operationId == L"stage-add");
  GC_CHECK(plan.displayName.find(L"4 项") != std::wstring::npos);
  // 清单形态下参数里只有 add 与全局选项，路径一条都不出现（长度与注入面都最小）。
  GC_CHECK(!HasArgument(plan.arguments, L"docs/readme.md"));
  GC_CHECK(plan.arguments.size() == 5);
  // 范围说明要同时讲清「只暂存这几项」与「其余未选中的不动」。
  GC_CHECK(plan.notice.find(L"只暂存选中的 4 项") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"另有 5 项未选中") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.empty());
}

GC_TEST(staging_rename_item_pairs_old_and_new_path) {
  // 索引侧的重命名条目带来源路径：一次变化 = 旧路径的删除 + 新路径的添加。
  ChangeItem renamed = Make(ChangeKind::renamed, L"src/new_name.cpp", L"src/old_name.cpp");
  const StagingPlan plan = gc::git::BuildStagingAddPlan({renamed}, FileMode());
  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(plan.selectedItems == 1);
  GC_CHECK(plan.pathspecCount == 2);
  // 旧路径在前：与 diff 输出里 rename from/to 的阅读顺序一致。
  GC_CHECK(plan.pathspecEntries ==
           (std::vector<std::wstring>{Lit(L"src/old_name.cpp"), Lit(L"src/new_name.cpp")}));
  GC_CHECK(plan.notice.find(L"重命名的来源路径") != std::wstring::npos);
}

GC_TEST(staging_worktree_rename_as_two_selected_items) {
  // 工作区里改名的真实形态（porcelain v2 实测）：一条 .D 旧路径 + 一条 ?? 新路径。
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::deleted, L"old.txt"),
      Make(ChangeKind::untracked, L"renamed.txt"),
  };
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, FileMode());
  GC_CHECK(plan.pathspecEntries ==
           (std::vector<std::wstring>{Lit(L"old.txt"), Lit(L"renamed.txt")}));
  GC_CHECK(plan.pathspecCount == 2);
}

GC_TEST(staging_dedupes_repeated_paths) {
  ChangeItem renamed = Make(ChangeKind::renamed, L"a.txt", L"b.txt");
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::modified, L"a.txt"),
      Make(ChangeKind::modified, L"a.txt"),  // 同一目录两侧各一条的场合也不该写两遍
      renamed,
      Make(ChangeKind::deleted, L"b.txt"),  // 与重命名的来源路径重复
  };
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, FileMode());
  GC_CHECK(EntryCount(plan.pathspecEntries, Lit(L"a.txt")) == 1);
  GC_CHECK(EntryCount(plan.pathspecEntries, Lit(L"b.txt")) == 1);
  GC_CHECK(plan.pathspecCount == 2);
}

GC_TEST(staging_rejects_item_whose_kind_is_not_unstaged_change) {
  // added 只可能来自索引侧；出现在选中条目里说明模型与显示已不一致，整份拒绝。
  const StagingPlan plan = gc::git::BuildStagingAddPlan({Make(ChangeKind::added, L"a.txt")}, FileMode());
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.pathspecEntries.empty());
  GC_CHECK(plan.blockedReason.find(L"刷新") != std::wstring::npos);

  const StagingPlan unknown =
      gc::git::BuildStagingAddPlan({Make(ChangeKind::unknown, L"a.txt")}, FileMode());
  GC_CHECK(unknown.delivery == StagingDelivery::blocked);
}

GC_TEST(staging_rejects_unsafe_path_before_running_anything) {
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::modified, L"ok.txt"),
      Make(ChangeKind::modified, L"../outside.txt"),
  };
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, FileMode());
  // 一条不合格就不发出命令，也不交出「合格的那一条」：半份清单会让人误以为全选中了。
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.pathspecEntries.empty());
  GC_CHECK(plan.blockedReason.find(L"outside") != std::wstring::npos);

  const std::vector<ChangeItem> withQuote = {Make(ChangeKind::untracked, L"we\"ird.txt")};
  GC_CHECK(gc::git::BuildStagingAddPlan(withQuote, FileMode()).delivery == StagingDelivery::blocked);

  const std::vector<ChangeItem> absolute = {Make(ChangeKind::modified, L"C:/Windows/x.txt")};
  GC_CHECK(gc::git::BuildStagingAddPlan(absolute, FileMode()).delivery == StagingDelivery::blocked);
}

GC_TEST(staging_conflicted_item_requires_explicit_confirmation) {
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::modified, L"clean.txt"),
      Make(ChangeKind::conflicted, L"conflicted.txt"),
  };
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, FileMode());
  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);  // 仍然可执行，但要用户先点头。
  GC_CHECK(!plan.confirmationText.empty());
  GC_CHECK(plan.confirmationText.find(L"未合并") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.find(L"conflicted.txt") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.find(L"已解决") != std::wstring::npos);
  // 程序没有检查冲突是否解决完 —— 文案必须承认这一点，并说明标记也会被暂存。
  GC_CHECK(plan.confirmationText.find(L"没有检查") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.find(L"<<<<<<<") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.find(L"取消") != std::wstring::npos);
}

GC_TEST(staging_submodule_explains_pointer_only) {
  ChangeItem pointerMoved = Make(ChangeKind::submodule, L"vendor/lib");
  pointerMoved.submodule.commitChanged = true;
  pointerMoved.submodule.untrackedChanges = true;
  const StagingPlan moved =
      gc::git::BuildStagingAddPlan({Make(ChangeKind::modified, L"a.txt"), pointerMoved}, FileMode());
  GC_CHECK(moved.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(moved.pathspecEntries == (std::vector<std::wstring>{Lit(L"a.txt"), Lit(L"vendor/lib")}));
  GC_CHECK(!moved.confirmationText.empty());
  GC_CHECK(moved.confirmationText.find(L"gitlink") != std::wstring::npos);
  GC_CHECK(moved.confirmationText.find(L"提交指针已移动") != std::wstring::npos);
  GC_CHECK(moved.confirmationText.find(L"内部有未跟踪文件") != std::wstring::npos);
  // 不递归：要保存内部改动必须进那个仓库自己提交。
  GC_CHECK(moved.confirmationText.find(L"进入 vendor/lib 那个仓库") != std::wstring::npos);

  // 指针未移动、只有内部改动：git add 在父仓库里是空操作，也要如实说明。
  ChangeItem onlyDirty = Make(ChangeKind::submodule, L"vendor/lib");
  onlyDirty.submodule.trackedChanges = true;
  const StagingPlan dirty = gc::git::BuildStagingAddPlan({onlyDirty}, FileMode());
  GC_CHECK(dirty.confirmationText.find(L"提交指针未移动") != std::wstring::npos);
  GC_CHECK(dirty.confirmationText.find(L"内部已跟踪文件有未提交改动") != std::wstring::npos);
}

GC_TEST(staging_inline_fallback_for_old_git_uses_literal_pathspecs) {
  const std::vector<ChangeItem> selection = {
      Make(ChangeKind::modified, L"brk[x].txt"),
      Make(ChangeKind::deleted, L"-dash.txt"),
      Make(ChangeKind::untracked, L"中文 文件.txt"),
  };
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, InlineMode());
  GC_CHECK(plan.delivery == StagingDelivery::inlinePathspec);
  GC_CHECK(HasArgument(plan.arguments, L"--"));
  GC_CHECK(HasArgument(plan.arguments, L":(literal)brk[x].txt"));
  GC_CHECK(HasArgument(plan.arguments, L":(literal)-dash.txt"));
  GC_CHECK(HasArgument(plan.arguments, L":(literal)中文 文件.txt"));
  // 参数形态下条目顺序与 -- 终止符仍然保持选中顺序。
  GC_CHECK(plan.arguments.size() == 6 + selection.size());
  GC_CHECK(plan.notice.find(L"2.25") != std::wstring::npos);
}

GC_TEST(staging_inline_fallback_refuses_instead_of_broadening) {
  std::vector<ChangeItem> selection;
  for (size_t index = 0; index < 60; ++index) {
    selection.push_back(Make(ChangeKind::modified, L"file" + std::to_wstring(index) + L".txt"));
  }
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, InlineMode());
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.pathspecEntries.size() == 60);  // 清单本身是完整的，只是这次不执行。
  GC_CHECK(plan.blockedReason.find(L"git add .") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"2.25") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"分批") != std::wstring::npos);

  // 同样的 60 条在新版本上走清单文件，不受条数限制。
  const StagingPlan filePlan = gc::git::BuildStagingAddPlan(selection, FileMode());
  GC_CHECK(filePlan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(filePlan.pathspecCount == 60);
}

GC_TEST(staging_pathspec_file_option_appending) {
  std::vector<std::wstring> arguments = {L"add"};
  GC_CHECK(gc::git::AppendPathspecFileOptions(&arguments, L"C:\\Temp\\gc-add-1-1.nul"));
  GC_CHECK(arguments ==
           (std::vector<std::wstring>{L"add", L"--pathspec-from-file=C:\\Temp\\gc-add-1-1.nul",
                                      L"--pathspec-file-nul"}));

  std::vector<std::wstring> rejected = {L"add"};
  const size_t before = rejected.size();
  GC_CHECK(!gc::git::AppendPathspecFileOptions(&rejected, L""));
  GC_CHECK(!gc::git::AppendPathspecFileOptions(&rejected, L"C:\\we\"ird.nul"));
  GC_CHECK(!gc::git::AppendPathspecFileOptions(&rejected, L"C:\\con\atrol.nul"));
  GC_CHECK(rejected.size() == before);  // 拒绝时不留下半套参数
  GC_CHECK(!gc::git::AppendPathspecFileOptions(nullptr, L"C:\\Temp\\x.nul"));
}

GC_TEST(staging_pathspec_file_capability_by_version) {
  GC_CHECK(gc::git::SupportsPathspecFileDelivery(L"2.56.0.windows.1"));
  GC_CHECK(gc::git::SupportsPathspecFileDelivery(L"2.25.0"));
  GC_CHECK(gc::git::SupportsPathspecFileDelivery(L"2.26.2.windows.1"));
  GC_CHECK(gc::git::SupportsPathspecFileDelivery(L"3.0.0"));
  GC_CHECK(gc::git::SupportsPathspecFileDelivery(L"git version 2.30.1"));
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L"2.24.3.windows.1"));
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L"1.9.5"));
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L"2"));
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L"2."));
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L""));
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L"development build"));
  // 版本号首位异常（解析溢出）时按“不支持”处理，绝不猜成支持。
  GC_CHECK(!gc::git::SupportsPathspecFileDelivery(L"99999999.1.0"));
}

GC_TEST(staging_restore_command_capability_by_version) {
  GC_CHECK(gc::git::SupportsRestoreCommand(L"2.56.0.windows.1"));
  GC_CHECK(gc::git::SupportsRestoreCommand(L"2.23.0"));
  GC_CHECK(gc::git::SupportsRestoreCommand(L"3.0.0"));
  GC_CHECK(!gc::git::SupportsRestoreCommand(L"2.22.3.windows.1"));
  GC_CHECK(!gc::git::SupportsRestoreCommand(L"1.9.5"));
  GC_CHECK(!gc::git::SupportsRestoreCommand(L""));
  GC_CHECK(!gc::git::SupportsRestoreCommand(L"development build"));
  // 与清单接口的门槛各是一条独立的判定：2.23 有 restore 但还没有 --pathspec-from-file（2.25 起）。
  GC_CHECK(gc::git::SupportsRestoreCommand(L"2.24.0") &&
           !gc::git::SupportsPathspecFileDelivery(L"2.24.0"));
}

GC_TEST(staging_summary_lists_selection_within_limit) {
  std::vector<ChangeItem> selection;
  for (size_t index = 0; index < 10; ++index) {
    selection.push_back(Make(ChangeKind::modified, L"f" + std::to_wstring(index) + L".txt"));
  }
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selection, FileMode());
  GC_CHECK(plan.selectionSummary.find(L"等 10 项") != std::wstring::npos);
  GC_CHECK(plan.selectionSummary.find(L"f6.txt") == std::wstring::npos);

  const StagingPlan small =
      gc::git::BuildStagingAddPlan({Make(ChangeKind::modified, L"only.txt")}, FileMode());
  GC_CHECK(small.selectionSummary == L"only.txt");
}

// ---------------------------------------------------------------------------
// ← 移出暂存区：同一套准入、同一套字面 pathspec，只是命令方向相反
// ---------------------------------------------------------------------------

StagingPlanOptions UnstageMode(bool repositoryHasHead = true) {
  StagingPlanOptions options;
  options.pathspecFileSupported = true;
  options.repositoryHasHead = repositoryHasHead;
  return options;
}

// 有 HEAD 但这个 Git 太旧、没有 restore 子命令（2.23 起才有）。
StagingPlanOptions UnstageOldGitMode() {
  StagingPlanOptions options = UnstageMode(/*repositoryHasHead=*/true);
  options.restoreCommandSupported = false;
  return options;
}

StagingPlanOptions UnstageInlineMode(bool repositoryHasHead = true) {
  StagingPlanOptions options = UnstageMode(repositoryHasHead);
  options.pathspecFileSupported = false;
  return options;
}

GC_TEST(unstage_empty_selection_is_refused_not_treated_as_unstage_all) {
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan({}, UnstageMode());
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.pathspecEntries.empty());
  // 这一句必须点名最危险的那条后备命令：不带路径的 git reset 会连 HEAD 一起动。
  GC_CHECK(plan.blockedReason.find(L"没有选中") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"绝不会") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"git reset") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"git checkout") != std::wstring::npos);
}

GC_TEST(unstage_uses_restore_staged_when_head_exists) {
  const std::vector<ChangeItem> selection = {
      MakeStaged(ChangeKind::modified, L"a.txt", L"M."),
      MakeStaged(ChangeKind::added, L"new.txt", L"A."),
      MakeStaged(ChangeKind::deleted, L"gone.txt", L"D."),
  };
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan(selection, UnstageMode());
  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(HasArgument(plan.arguments, L"restore"));
  GC_CHECK(HasArgument(plan.arguments, L"--staged"));
  GC_CHECK(plan.operationId == L"stage-unstage");
  GC_CHECK(plan.selectedItems == 3);
  GC_CHECK(plan.pathspecEntries ==
           (std::vector<std::wstring>{Lit(L"a.txt"), Lit(L"new.txt"), Lit(L"gone.txt")}));
  // 范围说明要讲清「只写索引、不改工作区」，并复述字面 pathspec 这条底线。
  GC_CHECK(plan.notice.find(L"只写索引") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"退回最近一次提交") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L":(literal)") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.empty());
}

GC_TEST(unstage_uses_index_only_reset_when_repository_has_no_commits) {
  const std::vector<ChangeItem> selection = {
      MakeStaged(ChangeKind::added, L"first.txt", L"A."),
      MakeStaged(ChangeKind::added, L"second.txt", L"A."),
  };
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan(selection, UnstageMode(/*repositoryHasHead=*/false));
  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(HasArgument(plan.arguments, L"reset"));
  GC_CHECK(HasArgument(plan.arguments, L"-q"));
  GC_CHECK(!HasArgument(plan.arguments, L"restore"));
  // 文案必须解释为什么这里换成了 reset，以及它同样不会碰工作区与 HEAD。
  GC_CHECK(plan.notice.find(L"还没有任何提交") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"could not resolve") != std::wstring::npos ||
           plan.notice.find(L"直接失败") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"原样保留") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"未跟踪") != std::wstring::npos);
}

GC_TEST(unstage_never_uses_destructive_or_repository_wide_forms) {
  const std::vector<ChangeItem> selection = {
      MakeStaged(ChangeKind::modified, L"a.txt", L"MM"),
      MakeStaged(ChangeKind::deleted, L"b.txt", L"D."),
      MakeStaged(ChangeKind::renamed, L"c.txt", L"R.", L"b.txt"),
  };
  for (const bool hasHead : {true, false}) {
    for (const StagingPlan& plan : {gc::git::BuildStagingUnstagePlan(selection, UnstageMode(hasHead)),
                                    gc::git::BuildStagingUnstagePlan(selection, UnstageInlineMode(hasHead))}) {
      GC_CHECK_MESSAGE(plan.delivery != StagingDelivery::blocked, "两种形态都应可执行");
      // 丢弃工作区的形态一条都不许出现；范围只能来自选中的字面路径。
      for (const std::wstring_view forbidden : {L"--hard", L"--soft", L"--mixed", L"--merge", L"--keep",
                                                L"--recurse-submodules", L"--worktree", L".", L"-A", L"-u"}) {
        GC_CHECK(!HasArgument(plan.arguments, forbidden));
      }
      GC_CHECK(!HasEntry(plan.pathspecEntries, L"."));
      // 参数形态下 -- 终止符必须在路径之前，否则前导减号的档名会被当成选项。
      if (plan.delivery == StagingDelivery::inlinePathspec) {
        GC_CHECK(HasArgument(plan.arguments, L"--"));
      }
    }
  }
}

GC_TEST(unstage_rename_item_pairs_old_and_new_path) {
  // 实测：只撤新路径会留下「旧路径仍是暂存的删除」，一次重命名被拆成半成品。
  const ChangeItem renamed = MakeStaged(ChangeKind::renamed, L"src/new_name.cpp", L"R100", L"src/old_name.cpp");
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan({renamed}, UnstageMode());
  GC_CHECK(plan.selectedItems == 1);
  GC_CHECK(plan.pathspecCount == 2);
  GC_CHECK(plan.pathspecEntries ==
           (std::vector<std::wstring>{Lit(L"src/old_name.cpp"), Lit(L"src/new_name.cpp")}));
  GC_CHECK(plan.notice.find(L"重命名的来源路径") != std::wstring::npos);
}

GC_TEST(unstage_rejects_kinds_that_are_not_index_changes) {
  // untracked 根本不在索引里；conflicted 按本程序的设计只出现在未暂存一侧。
  const StagingPlan untracked =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::untracked, L"a.txt", L"U")}, UnstageMode());
  GC_CHECK(untracked.delivery == StagingDelivery::blocked);
  GC_CHECK(untracked.pathspecEntries.empty());

  const StagingPlan conflicted =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::conflicted, L"a.txt", L"UU")}, UnstageMode());
  GC_CHECK(conflicted.delivery == StagingDelivery::blocked);

  const StagingPlan unknown =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::unknown, L"a.txt", L"??")}, UnstageMode());
  GC_CHECK(unknown.delivery == StagingDelivery::blocked);
}

GC_TEST(unstage_refuses_unmerged_entry_instead_of_resolving_it) {
  // 状态字里带 U 就是未合并记录。实测 restore --staged 会把三份 stage 记录收拢成 HEAD 那一份，
  // 冲突现场被抹掉一半 —— 那不是「解决冲突」，程序必须拒绝而不是替用户决定。
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan(
      {MakeStaged(ChangeKind::modified, L"clean.txt", L"M."), MakeStaged(ChangeKind::modified, L"conf.txt", L"UU")},
      UnstageMode());
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.pathspecEntries.empty());  // 连合法的那一条也不单独执行。
  GC_CHECK(plan.blockedReason.find(L"未合并") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"解决冲突") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"conf.txt") != std::wstring::npos);
}

GC_TEST(unstage_warns_when_the_index_holds_the_only_copy) {
  // AD：HEAD 里没有这个路径，磁盘上又已经没有了 —— 索引是那份内容的唯一容身之处。
  const StagingPlan lost =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::added, L"only-in-index.txt", L"AD")},
                                       UnstageMode());
  GC_CHECK(lost.delivery == StagingDelivery::pathspecFile);  // 仍可执行，但必须点头。
  GC_CHECK(!lost.confirmationText.empty());
  GC_CHECK(lost.confirmationText.find(L"只存在于索引") != std::wstring::npos);
  GC_CHECK(lost.confirmationText.find(L"only-in-index.txt") != std::wstring::npos);
  GC_CHECK(lost.confirmationText.find(L"没有任何副本") != std::wstring::npos);

  // 重命名同理（RD）：新路径的内容既不在 HEAD 也不在磁盘。
  const StagingPlan renamed =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::renamed, L"new.txt", L"RD", L"old.txt")},
                                       UnstageMode());
  GC_CHECK(!renamed.confirmationText.empty());

  // 而工作区还有副本的普通条目（MM / A.）不需要确认：取消暂存改不了磁盘上的任何东西。
  const StagingPlan safe = gc::git::BuildStagingUnstagePlan(
      {MakeStaged(ChangeKind::modified, L"both.txt", L"MM"), MakeStaged(ChangeKind::added, L"new.txt", L"A.")},
      UnstageMode());
  GC_CHECK(safe.confirmationText.empty());

  // 无 HEAD 时同一判定照样生效（命令换成 reset，风险不变）。
  const StagingPlan unborn =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::added, L"only-in-index.txt", L"AD")},
                                       UnstageMode(/*repositoryHasHead=*/false));
  GC_CHECK(!unborn.confirmationText.empty());
  GC_CHECK(unborn.confirmationText.find(L"git reset") != std::wstring::npos);
}

GC_TEST(unstage_submodule_explains_pointer_only) {
  ChangeItem pointer = MakeStaged(ChangeKind::submodule, L"vendor/lib", L"M.");
  pointer.submodule.trackedChanges = true;
  const StagingPlan plan =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::modified, L"a.txt", L"M."), pointer},
                                       UnstageMode());
  GC_CHECK(plan.pathspecEntries == (std::vector<std::wstring>{Lit(L"a.txt"), Lit(L"vendor/lib")}));
  GC_CHECK(!plan.confirmationText.empty());
  GC_CHECK(plan.confirmationText.find(L"gitlink") != std::wstring::npos);
  // 实测：父仓库这一侧退回指针后，子模块检出的提交没变，指针改动会回到未暂存那一侧。
  GC_CHECK(plan.confirmationText.find(L"不会进入 vendor/lib") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.find(L"未暂存的更改") != std::wstring::npos);
  GC_CHECK(plan.confirmationText.find(L"内部已跟踪文件有未提交改动") != std::wstring::npos);
}

GC_TEST(unstage_uses_reset_when_git_too_old_for_restore) {
  // restore 这个子命令是 2.23 才有的。对有 HEAD 但太旧的 Git，发一条 Git 根本不认识的
  // 子命令只会得到 "git: 'restore' is not a git command"，所以改用同样只写索引的 reset。
  const StagingPlan plan =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::modified, L"a.txt", L"M.")},
                                       UnstageOldGitMode());
  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(HasArgument(plan.arguments, L"reset"));
  GC_CHECK(!HasArgument(plan.arguments, L"restore"));
  GC_CHECK(plan.commandLabel == L"git reset -q");
  // 文案要说清换命令的理由是「版本太旧」，不能拿「无提交时 restore 会失败」那句话来糊弄。
  GC_CHECK(plan.notice.find(L"2.23") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"会直接失败") == std::wstring::npos);
  GC_CHECK(plan.notice.find(L"只写索引") != std::wstring::npos);

  // 无 HEAD 时给的是另一种理由（restore 会直接失败），两条不能混成一句。
  const StagingPlan unborn =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::added, L"a.txt", L"A.")},
                                       UnstageMode(/*repositoryHasHead=*/false));
  GC_CHECK(unborn.commandLabel == L"git reset -q");
  GC_CHECK(unborn.notice.find(L"还没有任何提交") != std::wstring::npos);
  GC_CHECK(unborn.notice.find(L"会直接失败") != std::wstring::npos);
  GC_CHECK(unborn.notice.find(L"未跟踪") != std::wstring::npos);
}

GC_TEST(unstage_selection_only_pathspecs_of_selected_items) {
  StagingPlanOptions options = UnstageMode();
  options.totalStagedItems = 7;  // 另有 4 项未选中
  const std::vector<ChangeItem> selection = {
      MakeStaged(ChangeKind::modified, L"docs/readme.md", L"M."),
      MakeStaged(ChangeKind::added, L"new file with space.txt", L"A."),
      MakeStaged(ChangeKind::deleted, L"src/old.cpp", L"D."),
  };
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan(selection, options);
  GC_CHECK(plan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(plan.pathspecCount == 3);
  // 清单形态下命令行里一条路径都不出现，参数只有子命令、--staged 与清单选项。
  GC_CHECK(!HasArgument(plan.arguments, L"docs/readme.md"));
  GC_CHECK(plan.arguments.size() == 6);
  GC_CHECK(plan.notice.find(L"只移出暂存区选中的 3 项") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"另有 4 项未选中") != std::wstring::npos);
  GC_CHECK(plan.displayName.find(L"移出暂存区 3 项") != std::wstring::npos);
}

GC_TEST(unstage_rejects_unsafe_path_before_running_anything) {
  const StagingPlan escaping = gc::git::BuildStagingUnstagePlan(
      {MakeStaged(ChangeKind::modified, L"ok.txt", L"M."),
       MakeStaged(ChangeKind::modified, L"../outside.txt", L"M.")},
      UnstageMode());
  GC_CHECK(escaping.delivery == StagingDelivery::blocked);
  GC_CHECK(escaping.pathspecEntries.empty());
  GC_CHECK(escaping.blockedReason.find(L"outside") != std::wstring::npos);

  const StagingPlan absolute =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::modified, L"C:/Windows/x.txt", L"M.")},
                                       UnstageMode());
  GC_CHECK(absolute.delivery == StagingDelivery::blocked);

  // 不该出现来源路径的条目带上了来源路径（模型与显示不一致），整份拒绝。
  const StagingPlan strayOld =
      gc::git::BuildStagingUnstagePlan({MakeStaged(ChangeKind::modified, L"a.txt", L"M.", L"b.txt")},
                                        UnstageMode());
  GC_CHECK(strayOld.delivery == StagingDelivery::blocked);
}

GC_TEST(unstage_inline_fallback_keeps_the_same_scope) {
  const std::vector<ChangeItem> selection = {
      MakeStaged(ChangeKind::modified, L"brk[x].txt", L"M."),
      MakeStaged(ChangeKind::modified, L"-dash.txt", L"M."),
      MakeStaged(ChangeKind::modified, L"中文 文件.txt", L"M."),
  };
  for (const bool hasHead : {true, false}) {
    const StagingPlan plan = gc::git::BuildStagingUnstagePlan(selection, UnstageInlineMode(hasHead));
    GC_CHECK(plan.delivery == StagingDelivery::inlinePathspec);
    GC_CHECK(HasArgument(plan.arguments, L":(literal)brk[x].txt"));
    GC_CHECK(HasArgument(plan.arguments, L":(literal)-dash.txt"));
    GC_CHECK(HasArgument(plan.arguments, L":(literal)中文 文件.txt"));
    GC_CHECK(plan.notice.find(L"2.25") != std::wstring::npos);
    // 拒绝扩大范围的那句话要按实际使用的命令说，不能一套文案套两种命令。
    GC_CHECK(plan.notice.find(hasHead ? L"git restore --staged" : L"git reset") != std::wstring::npos);
  }
}

GC_TEST(unstage_inline_refuses_instead_of_broadening) {
  std::vector<ChangeItem> selection;
  for (size_t index = 0; index < 60; ++index) {
    selection.push_back(MakeStaged(ChangeKind::modified, L"f" + std::to_wstring(index) + L".txt", L"M."));
  }
  const StagingPlan plan = gc::git::BuildStagingUnstagePlan(selection, UnstageInlineMode());
  GC_CHECK(plan.delivery == StagingDelivery::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.pathspecEntries.size() == 60);  // 清单本身是完整的，只是这次不执行。
  GC_CHECK(plan.blockedReason.find(L"不带路径的 git restore --staged") != std::wstring::npos);
  GC_CHECK(plan.blockedReason.find(L"分批") != std::wstring::npos);

  // 无 HEAD 时同一批条目也不能退化成不带路径的 git reset。
  const StagingPlan unborn = gc::git::BuildStagingUnstagePlan(selection, UnstageInlineMode(false));
  GC_CHECK(unborn.delivery == StagingDelivery::blocked);
  GC_CHECK(unborn.blockedReason.find(L"不带路径的 git reset") != std::wstring::npos);

  const StagingPlan filePlan = gc::git::BuildStagingUnstagePlan(selection, UnstageMode());
  GC_CHECK(filePlan.delivery == StagingDelivery::pathspecFile);
  GC_CHECK(filePlan.pathspecCount == 60);
}

}  // namespace
