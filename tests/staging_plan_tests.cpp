// 「加入暂存区」方案构造的纯逻辑测试：不碰文件系统、不起 Git，只断言
// 「选中的条目 → 交给 git add 的范围」这一件事。
// 覆盖：空选择绝不退化成全量、只处理选中的条目、重命名成对给出旧+新路径、
// 路径与条目类别的准入校验、未合并与子模块的确认文案、
// 清单文件/字面 pathspec 两种传递形态的选择与旧版本的拒绝策略，以及 Git 版本判定本身。
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

}  // namespace
