// 「pull」判读与方案的纯逻辑测试：全部用桩化的 GitQueryResult 与手工搭出的事实驱动生产逻辑，
// 不起真实 Git、不碰文件系统、不接触网络。覆盖：
//   * 各条只读查询的参数形态（-C 绑定、--no-optional-locks、--quiet 语义、-z 清单、
//     merge-tree 的无损预演形态、抓取阶段那两条「影响范围」的配置查询）；
//   * 阶段一判读：分支 / HEAD / 上游三栏 / 四条配置 / 现状，以及「没设」与「查询失败」的区别；
//   * 阶段二判读：rev-list 的「左 右」两数与四种关系、merge-tree 的三种结局
//     （0 无冲突、1 有冲突并能认出文件名、129 视为不支持而降级为保守提示）；
//   * 前提拒绝：无上游、游离 HEAD、尚无提交、merge 进行中、查询失败——一律不产生命令，
//     也不猜 origin、不代设 upstream、不写配置；
//   * 策略尊重配置：branch.<名>.rebase > pull.rebase、pull.ff > merge.ff；分叉而配置未明确时
//     交给用户当场选（默认偏向合并）；命令形态与预检策略一致，变基路线不借用合并预演的结论；
//   * 风险清单：预演冲突、未提交改动重叠、未跟踪文件撞名都要列出来并点名文件；
//   * 执行前复核 DescribePullChange：分支/HEAD/跟踪引用/逐条现状/流程痕迹任一变化都拒绝执行。
// 真实 Git 的两工作区链路（含 fetch→整合落地后的仓库状态）在 pull_probe_fixture_tests.cpp 验证。
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "git/pull_plan.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::GitQueryResult;
using gc::git::PullFetchPlan;
using gc::git::PullFetchPlanState;
using gc::git::PullIntegratePlan;
using gc::git::PullIntegratePlanInput;
using gc::git::PullIntegrateStrategy;
using gc::git::PullMergeDryRun;
using gc::git::PullPlanState;
using gc::git::PullRelationship;
using gc::git::PullRelationshipFacts;
using gc::git::PullRelationshipQueries;
using gc::git::PullStrategyChoice;
using gc::git::PullTargetFacts;
using gc::git::PullTargetQueries;
using gc::git::WorkspaceModel;

constexpr std::wstring_view kRoot = L"D:\\仓 库";
constexpr std::wstring_view kHead = L"1111111111111111111111111111111111111111";
constexpr std::wstring_view kRemote = L"2222222222222222222222222222222222222222";
constexpr std::wstring_view kBase = L"3333333333333333333333333333333333333333";
constexpr std::wstring_view kTree = L"4444444444444444444444444444444444444444";

GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
  return result;
}

// --quiet 系查询与 config 的「明确没有」：退出码 1 且无输出。
GitQueryResult NoResult() { return Answer(1); }

// 带 NUL 的原始输出必须整个按 std::wstring 交进去：wstring_view 与 std::wstring 从 wchar_t*
// 构造都会在第一个 NUL 处截断，而 -z 清单的正是一串以 NUL 分隔的路径。
std::wstring NulList(std::initializer_list<std::wstring_view> items) {
  std::wstring joined;
  for (const std::wstring_view item : items) {
    joined += item;
    joined.push_back(L'\0');
  }
  return joined;
}

GitQueryResult AnswerRaw(int exitCode, const std::wstring& output, const std::wstring& error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = output;
  result.utf16Error = error;
  return result;
}

GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
  return result;
}

bool TextContains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool HasArgument(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

std::string Narrow(std::wstring_view text) {
  std::string out;
  for (const wchar_t value : text) {
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
    } else {
      char buffer[16]{};
      std::snprintf(buffer, sizeof(buffer), "<U+%04X>", static_cast<unsigned>(value));
      out += buffer;
    }
  }
  return out;
}

std::string SizeMessage(std::wstring_view text, size_t actual, size_t expected) {
  return Narrow(text) + "（实际 " + std::to_string(actual) + "，期望 " + std::to_string(expected) + "）";
}

// 一份「在 main 上、有 HEAD、上游是 origin/main、跟踪引用可解析、工作区干净」的桩查询组。
// 四条配置默认都回答「没设」（退出码 1 + 空输出），由各用例按需改写。
PullTargetQueries HealthyTargetQueries() {
  PullTargetQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.headObject = Answer(0, std::wstring(kHead) + L"\n");
  queries.upstreamRan = true;
  queries.upstream = Answer(0, L"origin\trefs/remotes/origin/main\trefs/heads/main\n");
  queries.trackingObjectRan = true;
  queries.trackingObject = Answer(0, std::wstring(kRemote) + L"\n");
  queries.configPullRebase = NoResult();
  queries.configBranchRebase = NoResult();
  queries.configBranchRebaseRan = true;
  queries.configPullFf = NoResult();
  queries.configMergeFf = NoResult();
  queries.statusRan = true;
  queries.status = Answer(0, L"");  // porcelain v2 的「没有任何变化」就是空输出
  // 抓取范围的配置：默认「这类配置一条也没有」（--get-regexp 无匹配 = 退出码 1 + 空输出）。
  // 阶段一的抓取参数由 git/fetch_scope 按这份事实生成，缺了回答就发不出抓取。
  queries.scope.remoteConfig = NoResult();
  queries.scope.globalConfig = NoResult();
  return queries;
}

PullTargetFacts HealthyTargetFacts() { return gc::git::InterpretPullTarget(HealthyTargetQueries()); }

PullRelationshipFacts RelationshipOf(PullRelationshipQueries queries) {
  return gc::git::InterpretPullRelationship(std::move(queries));
}

// 只带「左 右」两个数的一趟判读（阶段二的编排就是这么分两趟发的）。
PullRelationshipFacts CountedRelationship(std::wstring_view counts) {
  PullRelationshipQueries queries;
  queries.aheadBehind = Answer(0, std::wstring(counts) + L"\n");
  return RelationshipOf(queries);
}

// 一份「分叉：本地独有 1、远端独有 2、预演无冲突、带入两个文件」的阶段二查询组。
PullRelationshipQueries DivergedQueries() {
  PullRelationshipQueries queries;
  queries.aheadBehind = Answer(0, L"1\t2\n");
  queries.mergeBaseRan = true;
  queries.mergeBase = Answer(0, std::wstring(kBase) + L"\n");
  queries.incomingRan = true;
  queries.incoming = AnswerRaw(0, NulList({L"docs/说明 文件.txt", L"src/main.cpp"}));
  queries.mergeTreeRan = true;
  queries.mergeTree = Answer(0, std::wstring(kTree) + L"\n");
  return queries;
}

PullRelationshipFacts DivergedFacts() { return RelationshipOf(DivergedQueries()); }

PullIntegratePlanInput IntegrateInput(const PullRelationshipFacts& relationship,
                                      PullStrategyChoice choice = PullStrategyChoice::none,
                                      const PullTargetFacts& target = HealthyTargetFacts()) {
  PullIntegratePlanInput input;
  input.target = target;
  input.relationship = relationship;
  input.repositoryRoot = std::wstring(kRoot);
  input.choice = choice;
  return input;
}

// 手工搭一条工作区条目：只用于检查「本地未提交的东西与带入路径重叠」这类风险，
// 不重复解析 porcelain（那套解析有 workspace_status 自己的用例）。
ChangeItem Item(ChangeKind kind, std::wstring_view statusCode, std::wstring_view path) {
  ChangeItem item;
  item.kind = kind;
  item.statusCode = std::wstring(statusCode);
  item.path = std::wstring(path);
  return item;
}

std::wstring RiskText(const PullIntegratePlan& plan) {
  std::wstring text;
  for (const std::wstring& risk : plan.risks) {
    text += risk + L"\n";
  }
  return text;
}

}  // namespace

// ---- 查询参数形态 ----

GC_TEST(pull_query_arguments_bind_repository_and_stay_read_only) {
  const std::vector<std::wstring> symbolic = gc::git::BuildPullSymbolicRefArguments(kRoot);
  GC_CHECK(HasArgument(symbolic, L"-C") && HasArgument(symbolic, kRoot));
  GC_CHECK(HasArgument(symbolic, L"--no-optional-locks"));
  GC_CHECK(HasArgument(symbolic, L"symbolic-ref") && HasArgument(symbolic, L"--quiet"));
  GC_CHECK(HasArgument(symbolic, L"HEAD"));

  const std::vector<std::wstring> head = gc::git::BuildPullHeadObjectArguments(kRoot);
  GC_CHECK(HasArgument(head, L"rev-parse") && HasArgument(head, L"--verify") &&
           HasArgument(head, L"--quiet"));

  // 没有分支名就没有分支记录可问：这条查询根本不该发出去。
  GC_CHECK(gc::git::BuildPullUpstreamArguments(kRoot, L"").empty());
  const std::vector<std::wstring> upstream = gc::git::BuildPullUpstreamArguments(kRoot, L"main");
  GC_CHECK(HasArgument(upstream, L"for-each-ref"));
  GC_CHECK(HasArgument(upstream, L"refs/heads/main"));
  const bool hasUpstreamFormat = std::any_of(
      upstream.begin(), upstream.end(),
      [](const std::wstring& argument) { return TextContains(argument, L"%(upstream:remotename)"); });
  GC_CHECK_MESSAGE(hasUpstreamFormat, "上游那三条栏目必须由 Git 自己回答，不是本程序拼出来的");

  // 跟踪引用名为空时无从可问。
  GC_CHECK(gc::git::BuildPullTrackingObjectArguments(kRoot, L"").empty());
  GC_CHECK(HasArgument(gc::git::BuildPullTrackingObjectArguments(kRoot, L"refs/remotes/origin/main"),
                       L"refs/remotes/origin/main"));

  // 不合格或半截的对象 ID 根本不送进 Git：这条规矩挡住「拿猜出来的目标去问关系」。
  GC_CHECK(gc::git::BuildPullAheadBehindArguments(kRoot, kHead, L"abcdef").empty());
  GC_CHECK(gc::git::BuildPullMergeBaseArguments(kRoot, L"", kRemote).empty());
  GC_CHECK(gc::git::BuildPullIncomingArguments(kRoot, kBase, L"nope").empty());
  GC_CHECK(!gc::git::BuildPullMergeTreeArguments(kRoot, kHead, kRemote).empty());

  const std::vector<std::wstring> counts = gc::git::BuildPullAheadBehindArguments(kRoot, kHead, kRemote);
  GC_CHECK(HasArgument(counts, L"rev-list") && HasArgument(counts, L"--left-right") &&
           HasArgument(counts, L"--count"));
  GC_CHECK(HasArgument(counts, std::wstring(kHead) + L"..." + std::wstring(kRemote)));

  // 冲突预演用 merge-tree --write-tree：不碰工作区与索引；--name-only 让清单一行一个路径。
  const std::vector<std::wstring> dryRun = gc::git::BuildPullMergeTreeArguments(kRoot, kHead, kRemote);
  GC_CHECK(HasArgument(dryRun, L"merge-tree") && HasArgument(dryRun, L"--write-tree") &&
           HasArgument(dryRun, L"--name-only"));

  // 带入清单与未合并清单都走 -z：路径含空格、中文、& 时仍然原样可比。
  const std::vector<std::wstring> incoming = gc::git::BuildPullIncomingArguments(kRoot, kBase, kRemote);
  GC_CHECK(HasArgument(incoming, L"--name-only") && HasArgument(incoming, L"-z"));
  const std::vector<std::wstring> conflicts = gc::git::BuildPullConflictListingArguments(kRoot);
  GC_CHECK(HasArgument(conflicts, L"--diff-filter=U") && HasArgument(conflicts, L"-z"));

  GC_CHECK(gc::git::BuildPullConfigArguments(kRoot, L"").empty());
  const std::vector<std::wstring> config = gc::git::BuildPullConfigArguments(kRoot, L"pull.rebase");
  GC_CHECK(HasArgument(config, L"config") && HasArgument(config, L"--get") &&
           HasArgument(config, L"pull.rebase"));
}

// ---- 阶段一判读 ----

GC_TEST(pull_target_facts_read_branch_head_upstream_and_config) {
  const PullTargetFacts facts = HealthyTargetFacts();
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.onBranch && facts.branchRef == L"refs/heads/main" && facts.branchName == L"main");
  GC_CHECK(facts.headResolved && facts.headObjectId == std::wstring(kHead));
  GC_CHECK(facts.upstreamConfigured);
  GC_CHECK(facts.upstreamRemote == L"origin");
  GC_CHECK(facts.upstreamTrackingRef == L"refs/remotes/origin/main");
  GC_CHECK(facts.upstreamRemoteBranch == L"refs/heads/main");
  GC_CHECK(facts.trackingResolved && facts.trackingObjectId == std::wstring(kRemote));
  GC_CHECK(facts.statusOk && facts.statusRan);
  // 四条配置都回答「没设」：字段留空，而不是「设成了空字符串」。
  GC_CHECK(facts.configPullRebase.empty() && facts.configBranchRebase.empty() &&
           facts.configPullFf.empty() && facts.configMergeFf.empty());
}

GC_TEST(pull_target_facts_distinguish_unset_config_from_set_value) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.configPullRebase = Answer(0, L"merges\n");
  queries.configBranchRebase = Answer(0, L"true\n");
  queries.configPullFf = Answer(0, L"only\n");
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.configPullRebase == L"merges");
  GC_CHECK(facts.configBranchRebase == L"true");  // 分支层覆盖 pull 层：判读层只如实搬运
  GC_CHECK(facts.configPullFf == L"only");
  GC_CHECK(facts.configMergeFf.empty());
}

GC_TEST(pull_target_facts_detached_head_answers_without_upstream_queries) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.symbolicRef = NoResult();  // symbolic-ref --quiet：不在分支上就是退出码 1 + 空输出
  queries.upstreamRan = false;
  queries.trackingObjectRan = false;
  queries.configBranchRebaseRan = false;
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));  // 「游离」是明确答案，不是查询失败
  GC_CHECK(!facts.onBranch);
  GC_CHECK(facts.branchName.empty() && facts.branchRef.empty());
  GC_CHECK(!facts.upstreamConfigured && !facts.trackingResolved);
}

GC_TEST(pull_target_facts_unborn_branch_is_answered_not_failed) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.headObject = NoResult();  // 这个分支还没有提交
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.headQueried && !facts.headResolved);
}

GC_TEST(pull_target_facts_missing_tracking_ref_is_a_fact_not_a_failure) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.trackingObject = NoResult();  // 上游配置指向一个本地还没有的跟踪引用
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.upstreamConfigured && !facts.trackingResolved);
}

GC_TEST(pull_target_facts_no_upstream_keeps_configured_false) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.upstream = Answer(0, L"\t\t\n");  // 分支记录里上游两栏都是空
  queries.trackingObjectRan = false;
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(!facts.upstreamConfigured);
}

GC_TEST(pull_target_facts_launch_failure_is_reported_not_guessed) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.upstream = LaunchFailed();
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_CHECK(!facts.queryOk);
  GC_CHECK(TextContains(facts.queryFailure, L"上游"));
}

GC_TEST(pull_target_facts_malformed_object_id_is_refused) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.trackingObject = Answer(0, L"not-an-object-id\n");
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_CHECK(!facts.queryOk);
  GC_CHECK(TextContains(facts.queryFailure, L"对象 ID"));
}

GC_TEST(pull_upstream_line_parsing_tolerates_missing_columns) {
  const gc::git::PullUpstreamInfo full =
      gc::git::ParsePullUpstreamLine(L"origin\trefs/remotes/origin/feat\1\trefs/heads/feat\1");
  GC_CHECK(full.configured && full.remote == L"origin");
  GC_CHECK(full.trackingRef == L"refs/remotes/origin/feat\1");
  GC_CHECK(full.remoteBranch == L"refs/heads/feat\1");

  const gc::git::PullUpstreamInfo partial = gc::git::ParsePullUpstreamLine(L"origin");
  GC_CHECK(!partial.configured);
  GC_CHECK(partial.remote == L"origin");
  GC_CHECK(partial.trackingRef.empty() && partial.remoteBranch.empty());
}

GC_TEST(pull_branch_name_only_accepts_heads_refs) {
  std::wstring branchName;
  GC_CHECK(gc::git::PullBranchNameFromRef(L"refs/heads/feature/x", &branchName));
  GC_CHECK(branchName == L"feature/x");
  branchName.clear();
  GC_CHECK(!gc::git::PullBranchNameFromRef(L"refs/tags/v1", &branchName));
  GC_CHECK(branchName.empty());
}

// ---- 阶段二判读 ----

GC_TEST(pull_relationship_counts_map_to_four_states) {
  const PullRelationshipFacts same = CountedRelationship(L"0\t0");
  GC_REQUIRE_MESSAGE(same.queryOk, Narrow(same.queryFailure));
  GC_CHECK(same.relationship == PullRelationship::upToDate);
  GC_CHECK(gc::git::PullRelationshipLabel(same.relationship) == L"已一致");

  const PullRelationshipFacts behind = CountedRelationship(L"0\t3");
  GC_CHECK(behind.relationship == PullRelationship::fastForward && behind.behind == 3 &&
           behind.ahead == 0);

  const PullRelationshipFacts ahead = CountedRelationship(L"2\t0");
  GC_CHECK(ahead.relationship == PullRelationship::aheadOnly);

  const PullRelationshipFacts both = CountedRelationship(L"2\t3");
  GC_CHECK(both.relationship == PullRelationship::diverged && both.ahead == 2 && both.behind == 3);
}

GC_TEST(pull_relationship_rejects_malformed_counts) {
  GC_CHECK(!CountedRelationship(L"1").queryOk);      // 只有一个数
  GC_CHECK(!CountedRelationship(L"a\tb").queryOk);   // 不是数字
  GC_CHECK(!CountedRelationship(L"-1\t2").queryOk);  // 负数不合约定
  const PullRelationshipFacts missing = RelationshipOf(PullRelationshipQueries{});
  GC_CHECK(!missing.queryOk);  // 查询本身没成功
}

GC_TEST(pull_merge_tree_dry_run_reads_clean_conflict_and_unsupported) {
  // 退出码 0：预演没有冲突。
  PullRelationshipQueries queries;
  queries.aheadBehind = Answer(0, L"1\t1\n");
  queries.mergeTreeRan = true;
  queries.mergeTree = Answer(0, std::wstring(kTree) + L"\n");
  const PullRelationshipFacts noConflict = RelationshipOf(queries);
  GC_CHECK(noConflict.dryRun == PullMergeDryRun::supported_clean);
  GC_CHECK(noConflict.dryRunConflicts.empty());
  GC_CHECK(noConflict.queryOk);  // 预演结果本身不是查询失败

  // 退出码 1 是「有冲突」的明确答案（实测 Git 2.53）：文件名取自 tree 行之后、空行之前。
  queries.mergeTree = Answer(1,
                             std::wstring(kTree) + L"\nf.txt\ng/h.cpp\n\n"
                             L"Auto-merging f.txt\nCONFLICT (content): Merge conflict in f.txt\n");
  const PullRelationshipFacts conflicted = RelationshipOf(queries);
  GC_CHECK(conflicted.dryRun == PullMergeDryRun::supported_conflict);
  GC_CHECK_MESSAGE(conflicted.dryRunConflicts.size() == 2,
                       SizeMessage(L"冲突清单", conflicted.dryRunConflicts.size(), 2));
  GC_CHECK(conflicted.dryRunConflicts[0] == L"f.txt");
  GC_CHECK(conflicted.dryRunConflicts[1] == L"g/h.cpp");

  // 这个 Git 不认 merge-tree --write-tree（用法错误 129）：判成「不支持」，
  // 而不是当作「预演说没冲突」。
  queries.mergeTree = Answer(129, L"", L"error: unknown option `write-tree'\n");
  const PullRelationshipFacts unsupported = RelationshipOf(queries);
  GC_CHECK(unsupported.dryRun == PullMergeDryRun::unsupported);
  GC_CHECK(TextContains(unsupported.dryRunDetail, L"129"));
  GC_CHECK(unsupported.queryOk);
}

GC_TEST(pull_incoming_paths_are_split_on_nul_and_keep_exotic_names) {
  PullRelationshipQueries queries;
  queries.aheadBehind = Answer(0, L"0\t1\n");
  queries.mergeBaseRan = true;
  queries.mergeBase = Answer(0, std::wstring(kBase) + L"\n");
  queries.incomingRan = true;
  queries.incoming =
      AnswerRaw(0, NulList({L"a b.txt", L"带 中文 & % 号.txt", L"under_score/[bracket].txt"}));
  const PullRelationshipFacts facts = RelationshipOf(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk && facts.incomingOk, Narrow(facts.incomingDetail));
  GC_REQUIRE_MESSAGE(facts.incomingPaths.size() == 3, "三条路径都要原样读回来");
  GC_CHECK(facts.incomingPaths[0] == L"a b.txt");
  GC_CHECK(facts.incomingPaths[1] == L"带 中文 & % 号.txt");
  GC_CHECK(facts.incomingPaths[2] == L"under_score/[bracket].txt");
}

// ---- 阶段一方案：抓取前的前提 ----

GC_TEST(pull_fetch_plan_ready_names_both_branches_and_promises_scope) {
  const PullFetchPlan plan = gc::git::BuildPullFetchPlan(HealthyTargetFacts(), kRoot);
  GC_REQUIRE_MESSAGE(plan.state == PullFetchPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.remoteName == L"origin");
  GC_CHECK(plan.trackingRef == L"refs/remotes/origin/main");
  GC_CHECK(plan.localBranchRef == L"refs/heads/main");
  // 抓取参数出自 git/fetch_scope：四个中和项一个都不能少，也不许出现任何扩大范围的形态。
  GC_CHECK(plan.arguments == std::vector<std::wstring>({L"fetch", L"--recurse-submodules=no",
                                                        L"--no-prune", L"--no-prune-tags", L"--no-tags",
                                                        L"origin"}));
  GC_CHECK(!HasArgument(plan.arguments, L"--all"));
  GC_CHECK(!HasArgument(plan.arguments, L"--multiple"));
  GC_CHECK(!HasArgument(plan.arguments, L"--prune"));
  GC_CHECK(!HasArgument(plan.arguments, L"--tags"));
  GC_CHECK(!HasArgument(plan.arguments, L"pull"));
  GC_CHECK(!HasArgument(plan.arguments, L"--rebase"));
  GC_CHECK(plan.operationId == L"pull-fetch");
  GC_CHECK(TextContains(plan.confirmationText, L"refs/heads/main"));
  GC_CHECK(TextContains(plan.confirmationText, L"refs/remotes/origin/main"));
  GC_CHECK(TextContains(plan.confirmationText,
                        L"git fetch --recurse-submodules=no --no-prune --no-prune-tags --no-tags origin"));
  // 范围承诺与界面 fetch 按钮同出一处：允许命名空间、被中和的配置、会变的东西都得说清。
  GC_CHECK(TextContains(plan.confirmationText, L"只更新远端「origin」在 refs/remotes/origin/ 之下"));
  GC_CHECK(TextContains(plan.confirmationText, L".git/FETCH_HEAD"));
  GC_CHECK(TextContains(plan.confirmationText, L"对象库"));
  GC_CHECK(TextContains(plan.confirmationText, L"不说「磁盘完全只读」"));
  GC_CHECK(TextContains(plan.confirmationText, L"确实会被写"));
  // 确认框必须交代这是两步里的第一步，以及这一步不做什么。
  GC_CHECK(TextContains(plan.notice, L"第一步"));
  GC_CHECK(TextContains(plan.confirmationText, L"整合是它成功之后的下一步"));
  GC_CHECK(TextContains(plan.confirmationText, L"不合并、不改上游"));
  // 已有配置要展示出来：策略不是界面替人定的。
  GC_CHECK(TextContains(plan.confirmationText, L"pull.rebase"));
  GC_CHECK(TextContains(plan.confirmationText, L"pull.ff"));
  GC_CHECK(TextContains(plan.notice, L"不合并"));
}

GC_TEST(pull_fetch_plan_blocked_without_upstream_and_makes_no_guess) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.upstream = Answer(0, L"\t\t\n");
  queries.trackingObjectRan = false;
  const PullTargetFacts facts = gc::git::InterpretPullTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  const PullFetchPlan plan = gc::git::BuildPullFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == PullFetchPlanState::blocked);
  GC_CHECK(plan.arguments.empty());  // 拒绝就是不产生命令，而不是「先跑一条看看」
  GC_CHECK(TextContains(plan.explanation, L"没有设置上游"));
  GC_CHECK(TextContains(plan.explanation, L"不猜"));
  GC_CHECK(TextContains(plan.explanation, L"--set-upstream"));  // 给出可操作的办法
  GC_CHECK(TextContains(plan.explanation, L"没有发出任何命令"));
}

GC_TEST(pull_fetch_plan_blocked_when_head_detached_or_unborn) {
  PullTargetQueries detached = HealthyTargetQueries();
  detached.symbolicRef = NoResult();
  detached.upstreamRan = false;
  detached.trackingObjectRan = false;
  detached.configBranchRebaseRan = false;
  const PullFetchPlan detachedPlan =
      gc::git::BuildPullFetchPlan(gc::git::InterpretPullTarget(detached), kRoot);
  GC_CHECK(detachedPlan.state == PullFetchPlanState::blocked);
  GC_CHECK(TextContains(detachedPlan.explanation, L"游离 HEAD"));

  PullTargetQueries unborn = HealthyTargetQueries();
  unborn.headObject = NoResult();
  const PullFetchPlan unbornPlan =
      gc::git::BuildPullFetchPlan(gc::git::InterpretPullTarget(unborn), kRoot);
  GC_CHECK(unbornPlan.state == PullFetchPlanState::blocked);
  GC_CHECK(TextContains(unbornPlan.explanation, L"还没有任何提交"));
}

GC_TEST(pull_fetch_plan_blocked_while_merge_or_rebase_in_progress) {
  PullTargetFacts facts = HealthyTargetFacts();
  facts.workflow.mergeInProgress = true;
  facts.workflowProbed = true;
  const PullFetchPlan plan = gc::git::BuildPullFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == PullFetchPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(TextContains(plan.explanation, L"git merge"));
  // 这类阻止不能靠「强制继续」绕过：Git 自己就先不接受。
  GC_CHECK(TextContains(plan.explanation, L"绕不过"));

  facts = HealthyTargetFacts();
  facts.workflow.rebaseInProgress = true;
  facts.workflowProbed = true;
  GC_CHECK(gc::git::BuildPullFetchPlan(facts, kRoot).state == PullFetchPlanState::blocked);
}

GC_TEST(pull_fetch_plan_blocked_when_queries_failed_or_directory_missing) {
  PullTargetFacts broken;
  broken.queryOk = false;
  broken.queryFailure = L"没能问出当前分支：Git 查询超时";
  const PullFetchPlan plan = gc::git::BuildPullFetchPlan(broken, kRoot);
  GC_CHECK(plan.state == PullFetchPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"Git 查询超时"));

  const PullFetchPlan noDir = gc::git::BuildPullFetchPlan(HealthyTargetFacts(), L"");
  GC_CHECK(noDir.state == PullFetchPlanState::blocked);
  GC_CHECK(TextContains(noDir.explanation, L"工作区根目录"));
}

// ---- 阶段二方案：关系与命令 ----

GC_TEST(pull_integrate_plan_nothing_to_integrate_emits_no_command) {
  PullIntegratePlan plan =
      gc::git::BuildPullIntegratePlan(IntegrateInput(CountedRelationship(L"0\t0")));
  GC_CHECK(plan.state == PullPlanState::nothingToIntegrate);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(TextContains(plan.explanation, L"已经一致"));

  // 本地领先：整合方向是反的，那属于 push 的范围，这一步同样什么都不做。
  plan = gc::git::BuildPullIntegratePlan(IntegrateInput(CountedRelationship(L"2\t0")));
  GC_CHECK(plan.state == PullPlanState::nothingToIntegrate);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(TextContains(plan.explanation, L"领先 2 个提交"));
  GC_CHECK(TextContains(plan.explanation, L"push"));
}

GC_TEST(pull_integrate_plan_blocked_when_tracking_ref_missing_after_fetch) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.trackingObject = NoResult();  // 抓取之后本地仍没有这个引用（远端分支被删等）
  const PullIntegratePlan plan =
      gc::git::BuildPullIntegratePlan(IntegrateInput(DivergedFacts(), PullStrategyChoice::none,
                                                     gc::git::InterpretPullTarget(queries)));
  GC_CHECK(plan.state == PullPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(TextContains(plan.explanation, L"refs/remotes/origin/main"));
}

GC_TEST(pull_integrate_plan_fast_forward_command_is_pinned_to_the_fetched_commit) {
  const PullIntegratePlan plan =
      gc::git::BuildPullIntegratePlan(IntegrateInput(CountedRelationship(L"0\t2")));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::fastForward);
  GC_CHECK(plan.arguments ==
           std::vector<std::wstring>({L"-c", L"submodule.recurse=false", L"merge", L"--no-autostash",
                                      L"--ff-only", std::wstring(kRemote)}));
  GC_CHECK(plan.targetObjectId == std::wstring(kRemote));
  GC_CHECK(plan.operationId == L"pull-integrate");
  GC_CHECK(!plan.requiresForce);  // 现状干净、没有重叠：没有要用户额外承担的风险
  // 确认文字里必须出现完整 ID：命令钉的是「刚抓回来的那一份」，不是引用名。
  GC_CHECK(TextContains(plan.confirmationText, std::wstring(kRemote)));
  GC_CHECK(TextContains(plan.confirmationText, L"不会替你 stash"));
  GC_CHECK(TextContains(plan.confirmationText, L"预检不是保证"));
  GC_CHECK(TextContains(plan.notice, L"pull 第二步"));
  // 没有 --no-verify、没有 --force：hooks 与签名照常生效。
  GC_CHECK(!HasArgument(plan.arguments, L"--no-verify"));
  GC_CHECK(!HasArgument(plan.arguments, L"--force"));
}

GC_TEST(pull_integrate_plan_honours_merge_ff_false_on_a_fast_forwardable_branch) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.configMergeFf = Answer(0, L"false\n");
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(CountedRelationship(L"0\t2"), PullStrategyChoice::none,
                     gc::git::InterpretPullTarget(queries)));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  // 配置说「绝不快进」：那就按配置产生合并提交，而不是悄悄替用户改主意。
  GC_CHECK(plan.strategy == PullIntegrateStrategy::merge);
  GC_CHECK(HasArgument(plan.arguments, L"--no-ff"));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(TextContains(RiskText(plan), L"merge.ff = false"));
}

GC_TEST(pull_integrate_plan_asks_for_strategy_when_diverged_without_config) {
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(IntegrateInput(DivergedFacts()));
  GC_CHECK(plan.state == PullPlanState::chooseStrategy);
  GC_CHECK(plan.arguments.empty());
  GC_REQUIRE_MESSAGE(plan.strategyCandidates.size() == 2,
                       SizeMessage(L"候选", plan.strategyCandidates.size(), 2));
  GC_CHECK(TextContains(plan.strategyCandidates[0], L"合并"));  // 默认偏向合并，排第一
  GC_CHECK(TextContains(plan.strategyCandidates[1], L"变基"));
  GC_CHECK(TextContains(plan.explanation, L"已经分叉"));
  GC_CHECK(TextContains(plan.explanation, L"不替你定"));
}

GC_TEST(pull_integrate_plan_diverged_merge_keeps_dry_run_conflicts_as_risk) {
  PullRelationshipQueries queries = DivergedQueries();
  queries.mergeTree = Answer(1,
                             std::wstring(kTree) + L"\nsrc/main.cpp\n\n"
                             L"CONFLICT (content): Merge conflict in src/main.cpp\n");
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(RelationshipOf(queries), PullStrategyChoice::chooseMerge));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::merge);
  GC_CHECK(plan.requiresForce);
  const std::wstring risks = RiskText(plan);
  GC_CHECK(TextContains(risks, L"src/main.cpp"));
  GC_CHECK(TextContains(risks, L"merge-tree"));
  GC_CHECK(TextContains(plan.confirmationText, L"风险提示"));
  GC_CHECK(HasArgument(plan.arguments, L"--no-edit"));
  GC_CHECK(!HasArgument(plan.arguments, L"--no-verify"));
}

GC_TEST(pull_integrate_plan_unsupported_dry_run_degrades_to_conservative_warning) {
  PullRelationshipQueries queries = DivergedQueries();
  queries.mergeTree = Answer(129, L"", L"error: unknown option `write-tree'\n");
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(RelationshipOf(queries), PullStrategyChoice::chooseMerge));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.requiresForce);  // 不知道会不会冲突，就不能装作知道
  GC_CHECK(TextContains(RiskText(plan), L"没能预演"));
}

GC_TEST(pull_integrate_plan_rebase_path_does_not_borrow_merge_dry_run_verdict) {
  // 预演（合并路线）说没有冲突——选了变基之后，这句结论不能被拿来给变基背书。
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(DivergedFacts(), PullStrategyChoice::chooseRebase));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::rebase);
  GC_CHECK(plan.arguments == std::vector<std::wstring>({L"-c", L"submodule.recurse=false", L"rebase",
                                                        L"--no-autostash", std::wstring(kRemote)}));
  GC_CHECK(plan.requiresForce);  // 变基会改写本地提交，本身就是要用户确认的风险
  GC_CHECK(TextContains(RiskText(plan), L"重写本地"));
  GC_CHECK(TextContains(RiskText(plan), L"那种结论是假的"));
  GC_CHECK(TextContains(plan.confirmationText, L"变基路线不做合并式预演"));
  GC_CHECK(!TextContains(plan.confirmationText, L"预演：已预演：没有内容冲突"));
}

GC_TEST(pull_integrate_plan_configured_rebase_decides_without_asking) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.configPullRebase = Answer(0, L"true\n");
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(DivergedFacts(), PullStrategyChoice::none, gc::git::InterpretPullTarget(queries)));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::rebase);
  GC_CHECK(TextContains(plan.strategySource, L"pull.rebase = true"));
}

GC_TEST(pull_integrate_plan_branch_rebase_overrides_pull_rebase) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.configPullRebase = Answer(0, L"true\n");
  queries.configBranchRebase = Answer(0, L"false\n");  // 分支层优先
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(DivergedFacts(), PullStrategyChoice::none, gc::git::InterpretPullTarget(queries)));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::merge);
  GC_CHECK(TextContains(plan.strategySource, L"branch.main.rebase = false"));
}

GC_TEST(pull_integrate_plan_ff_only_config_on_diverged_branch_stays_faithful) {
  PullTargetQueries queries = HealthyTargetQueries();
  queries.configPullFf = Answer(0, L"only\n");
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInput(DivergedFacts(), PullStrategyChoice::chooseMerge,
                     gc::git::InterpretPullTarget(queries)));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  // 分叉而配置要求只快进：忠实反映配置 = 让 Git 自己拒绝，而不是违背配置去「讨好」用户。
  GC_CHECK(HasArgument(plan.arguments, L"--ff-only"));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(TextContains(RiskText(plan), L"pull.ff = only"));
  GC_CHECK(TextContains(RiskText(plan), L"违背你的配置"));
}

GC_TEST(pull_integrate_plan_lists_overlapping_local_changes_as_risk) {
  PullIntegratePlanInput input = IntegrateInput(DivergedFacts(), PullStrategyChoice::chooseMerge);
  input.target.model.unstaged.push_back(Item(ChangeKind::modified, L".M", L"src/main.cpp"));
  input.target.model.unstaged.push_back(Item(ChangeKind::untracked, L"??", L"docs/说明 文件.txt"));
  input.target.model.unstaged.push_back(Item(ChangeKind::modified, L".M", L"unrelated.txt"));
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(input);
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.requiresForce);
  const std::wstring risks = RiskText(plan);
  GC_CHECK(TextContains(risks, L"src/main.cpp"));
  GC_CHECK(TextContains(risks, L"重叠"));
  GC_CHECK(TextContains(risks, L"docs/说明 文件.txt"));  // 带空格与中文的路径原样点名
  GC_CHECK(TextContains(risks, L"未跟踪"));
  GC_CHECK(!TextContains(risks, L"unrelated.txt"));  // 不在这次带入路径里的不该被牵连
  GC_CHECK(TextContains(risks, L"stash"));           // 明确说不会替用户 stash
}

// ---- 执行前复核 ----

GC_TEST(pull_recheck_accepts_identical_facts_and_rejects_any_change) {
  const PullTargetFacts preflight = HealthyTargetFacts();
  GC_CHECK(gc::git::DescribePullChange(preflight, preflight).empty());

  PullTargetFacts headMoved = preflight;
  headMoved.headObjectId = std::wstring(kRemote);
  const std::wstring headText = gc::git::DescribePullChange(preflight, headMoved);
  GC_CHECK(!headText.empty());
  GC_CHECK(TextContains(headText, L"HEAD"));
  GC_CHECK(TextContains(headText, L"没有发出任何整合命令"));

  PullTargetFacts trackingMoved = preflight;
  trackingMoved.trackingObjectId = std::wstring(kBase);
  const std::wstring trackingText = gc::git::DescribePullChange(preflight, trackingMoved);
  GC_CHECK(TextContains(trackingText, L"refs/remotes/origin/main"));
  GC_CHECK(TextContains(trackingText, L"远端跟踪引用"));

  PullTargetFacts branchSwitched = preflight;
  branchSwitched.branchRef = L"refs/heads/other";
  branchSwitched.branchName = L"other";
  GC_CHECK(TextContains(gc::git::DescribePullChange(preflight, branchSwitched), L"当前分支"));

  PullTargetFacts worktreeChanged = preflight;
  worktreeChanged.model.unstaged.push_back(Item(ChangeKind::modified, L".M", L"x.txt"));
  const std::wstring worktreeText = gc::git::DescribePullChange(preflight, worktreeChanged);
  GC_CHECK(TextContains(worktreeText, L"工作区/索引"));
  GC_CHECK(TextContains(worktreeText, L"0→1"));

  PullTargetFacts flowAppeared = preflight;
  flowAppeared.workflow.rebaseInProgress = true;
  const std::wstring flowText = gc::git::DescribePullChange(preflight, flowAppeared);
  GC_CHECK(TextContains(flowText, L"流程"));

  PullTargetFacts broken = preflight;
  broken.queryOk = false;
  broken.queryFailure = L"Git 查询超时";
  const std::wstring brokenText = gc::git::DescribePullChange(preflight, broken);
  GC_CHECK(TextContains(brokenText, L"复核没能完成"));
  GC_CHECK(TextContains(brokenText, L"Git 查询超时"));
}

GC_TEST(pull_conflict_state_lists_unmerged_paths_and_survives_missing_info) {
  const GitQueryResult listing =
      AnswerRaw(0, NulList({L"src/a.cpp", L"src/b.cpp", L"src/a.cpp"}));
  const gc::git::PullConflictState state = gc::git::InterpretPullConflictState(
      listing, Answer(0, L"refs/heads/main\n"), Answer(0, std::wstring(kHead) + L"\n"));
  GC_REQUIRE_MESSAGE(state.readOk, Narrow(state.readFailure));
  // 去重 + 排序：未合并条目按 Git 给的路径原样列出，界面据此点名。
  GC_REQUIRE_MESSAGE(state.conflictPaths.size() == 2, "重复路径只留一条");
  GC_CHECK(state.conflictPaths[0] == L"src/a.cpp");
  GC_CHECK(state.conflictPaths[1] == L"src/b.cpp");
  GC_CHECK(state.branchRef == L"refs/heads/main");
  GC_CHECK(state.headObjectId == std::wstring(kHead));

  // 「不在分支上」「HEAD 读不到」都不算取证失败：只是那两句说明留空。
  const gc::git::PullConflictState detached =
      gc::git::InterpretPullConflictState(Answer(0, L""), NoResult(), NoResult());
  GC_CHECK(detached.readOk);
  GC_CHECK(detached.conflictPaths.empty() && detached.branchRef.empty() &&
           detached.headObjectId.empty());

  // 清单本身读不到时必须如实报告，不能显示成「没有冲突」。
  const gc::git::PullConflictState failed =
      gc::git::InterpretPullConflictState(LaunchFailed(), NoResult(), NoResult());
  GC_CHECK(!failed.readOk);
  GC_CHECK(!failed.readFailure.empty());
}
