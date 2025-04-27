// 「fetch」判读与方案的纯逻辑测试：全部用桩化的 GitQueryResult 驱动生产逻辑，
// 不起真实 Git、不碰文件系统。覆盖：三条目标查询的参数形态（-C 绑定、--quiet 语义）、
// git remote -v 输出的解析（fetch/push 行、只有名字的行、中文与空格 URL）、
// 目标判定的四条规矩——分支配置命中即确定；配置与清单对不上/没配置/游离 HEAD 时
// 一律摆出既有远端让人选；一个也没有就明说不猜 origin；查询失败不在这上面猜。
// 抓取范围（prune／标签／映射）本身在 tests/fetch_scope_tests.cpp 专项覆盖，这里只核对
// 两个入口都从那份策略取参数与确认文字。
// 真实 Git 的远端效果与「B 推 A 抓」的完整链路在 fetch_fixture_tests.cpp 用临时仓库验证。
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "git/fetch_plan.h"
#include "git/repository.h"
#include "git/undo_commit_plan.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FetchPlan;
using gc::git::FetchPlanState;
using gc::git::FetchTargetFacts;
using gc::git::FetchTargetQueries;
using gc::git::GitQueryResult;

constexpr std::wstring_view kRoot = L"D:\\repo";

GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
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

// 一份「分支 main、配置了 origin、清单里有 origin」的桩查询组。
// 抓取范围的两条配置查询默认回答「这类配置一条也没有」（--get-regexp 无匹配 = 退出码 1 + 空输出），
// 需要按配置判定的形态由各用例改写 queries.scope（详见 tests/fetch_scope_tests.cpp）。
FetchTargetQueries HealthyQueries() {
  FetchTargetQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.branchRemoteRan = true;
  queries.branchRemote = Answer(0, L"origin\n");
  queries.remotes = Answer(0, L"origin\tD:\\\\bare\\\\origin.git (fetch)\n"
                              L"origin\tD:\\\\bare\\\\origin.git (push)\n");
  queries.scope.remoteConfig = Answer(1, L"");
  queries.scope.globalConfig = Answer(1, L"");
  return queries;
}

FetchTargetFacts HealthyFacts() { return gc::git::InterpretFetchTarget(HealthyQueries()); }

}  // namespace

// ---- 查询参数形态 ----

GC_TEST(fetch_query_arguments_bind_repository_and_stay_read_only) {
  const auto symbolic = gc::git::BuildFetchSymbolicRefArguments(kRoot);
  GC_CHECK(HasArgument(symbolic, L"-C"));
  GC_CHECK(HasArgument(symbolic, kRoot));
  GC_CHECK(HasArgument(symbolic, L"--no-optional-locks"));
  GC_CHECK(HasArgument(symbolic, L"symbolic-ref"));
  GC_CHECK(HasArgument(symbolic, L"--quiet"));

  const auto configured = gc::git::BuildFetchBranchRemoteArguments(kRoot, L"main");
  GC_CHECK(HasArgument(configured, L"config"));
  GC_CHECK(HasArgument(configured, L"--get"));
  GC_CHECK(HasArgument(configured, L"branch.main.remote"));

  // 分支名为空（游离 HEAD）根本不拼查询：绝不允许 branch..remote 这种半截键送进 Git。
  GC_CHECK(gc::git::BuildFetchBranchRemoteArguments(kRoot, L"").empty());

  const auto remotes = gc::git::BuildFetchRemotesArguments(kRoot);
  GC_CHECK(HasArgument(remotes, L"remote"));
  GC_CHECK(HasArgument(remotes, L"-v"));
}

// ---- 判读 ----

GC_TEST(fetch_interpret_reads_branch_config_and_remote_urls) {
  const FetchTargetFacts facts = HealthyFacts();
  GC_CHECK_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.onBranch);
  GC_CHECK(facts.branchName == L"main");
  GC_CHECK(facts.configuredRemote == L"origin");
  GC_REQUIRE_MESSAGE(facts.remotes.size() == 1, "健康桩应解析出一个远端");
  GC_CHECK(facts.remotes[0].name == L"origin");
  // (fetch) 行给 URL；(push) 行不产生第二个条目。
  GC_CHECK(facts.remotes[0].fetchUrl.find(L"origin.git") != std::wstring::npos);

  // 中文与空格的 URL 原样保留（宽字符进出，不经码页）。
  FetchTargetQueries unicode = HealthyQueries();
  unicode.remotes = Answer(0, L"公司\tD:\\项目 共享\\远端.git (fetch)\n");
  const FetchTargetFacts read = gc::git::InterpretFetchTarget(unicode);
  GC_REQUIRE_MESSAGE(read.remotes.size() == 1, "中文 URL 应解析出一个远端");
  GC_CHECK(read.remotes[0].name == L"公司");
  GC_CHECK(read.remotes[0].fetchUrl == L"D:\\项目 共享\\远端.git");

  // 只有名字的行（git remote 形态）也接受，URL 留空。
  FetchTargetQueries namesOnly = HealthyQueries();
  namesOnly.remotes = Answer(0, L"origin\ngitee\n");
  const FetchTargetFacts parsed = gc::git::InterpretFetchTarget(namesOnly);
  GC_REQUIRE_MESSAGE(parsed.remotes.size() == 2, "纯名字行应解析出两个远端");
  GC_CHECK(parsed.remotes[0].fetchUrl.empty());
  GC_CHECK(parsed.remotes[1].name == L"gitee");

  // 只有 (push) 行的远端：退用 push URL 展示，不让列表里出现「查无此人」。
  FetchTargetQueries pushOnly = HealthyQueries();
  pushOnly.remotes = Answer(0, L"origin\tD:\\x.git (push)\n");
  const FetchTargetFacts fallback = gc::git::InterpretFetchTarget(pushOnly);
  GC_REQUIRE_MESSAGE(fallback.remotes.size() == 1, "只有 push 行也应留下一个远端");
  GC_CHECK(fallback.remotes[0].fetchUrl == L"D:\\x.git");
}

GC_TEST(fetch_interpret_treats_absent_answers_as_honest_nothings) {
  // 游离 HEAD：symbolic-ref 以退出码 1 + 空输出作答，是明确答案而不是失败；
  // 平台层因此根本没发「分支配置的远端」那条查询。
  FetchTargetQueries detached = HealthyQueries();
  detached.symbolicRef = Answer(1, L"");
  detached.branchRemoteRan = false;
  detached.branchRemote = GitQueryResult{};
  const FetchTargetFacts facts = gc::git::InterpretFetchTarget(detached);
  GC_CHECK_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(!facts.onBranch);
  GC_CHECK(facts.configuredRemote.empty());

  // 在分支但没配置：config --get 未设时退出码 1 + 空输出，同样算「没有」。
  FetchTargetQueries unconfigured = HealthyQueries();
  unconfigured.branchRemote = Answer(1, L"");
  const FetchTargetFacts read = gc::git::InterpretFetchTarget(unconfigured);
  GC_CHECK_MESSAGE(read.queryOk, Narrow(read.queryFailure));
  GC_CHECK(read.onBranch);
  GC_CHECK(read.configuredRemote.empty());
}

GC_TEST(fetch_interpret_fails_loudly_on_broken_queries) {
  FetchTargetQueries broken = HealthyQueries();
  broken.symbolicRef = LaunchFailed();
  GC_CHECK(!gc::git::InterpretFetchTarget(broken).queryOk);

  broken = HealthyQueries();
  broken.branchRemote = Answer(2, L"", L"fatal: bad config line");
  const FetchTargetFacts configFailed = gc::git::InterpretFetchTarget(broken);
  GC_CHECK(!configFailed.queryOk);
  GC_CHECK(TextContains(configFailed.queryFailure, L"branch.main.remote"));

  broken = HealthyQueries();
  broken.remotes = Answer(128, L"", L"fatal: not a git repository");
  GC_CHECK(!gc::git::InterpretFetchTarget(broken).queryOk);
}

// ---- 方案判定 ----

GC_TEST(fetch_ready_only_from_explicit_branch_remote_config) {
  const FetchPlan plan = gc::git::BuildFetchPlan(HealthyFacts(), kRoot);
  GC_CHECK(plan.state == FetchPlanState::ready);
  GC_CHECK(plan.remoteName == L"origin");
  // 参数是 git/fetch_scope 生成的那一份：递归关掉 + prune/pruneTags/标签逐项中和 + 点名单个远端。
  GC_REQUIRE_MESSAGE(plan.arguments.size() == 6, "ready 方案应凑出六条参数");
  GC_CHECK(plan.arguments[0] == L"fetch");
  GC_CHECK(plan.arguments[1] == L"--recurse-submodules=no");
  GC_CHECK(plan.arguments[2] == L"--no-prune");
  GC_CHECK(plan.arguments[3] == L"--no-prune-tags");
  GC_CHECK(plan.arguments[4] == L"--no-tags");
  GC_CHECK(plan.arguments[5] == L"origin");
  // 范围承诺：没有 --prune/--all/--multiple，也不会顺带 pull。
  GC_CHECK(!HasArgument(plan.arguments, L"--prune"));
  GC_CHECK(!HasArgument(plan.arguments, L"--all"));
  GC_CHECK(!HasArgument(plan.arguments, L"--multiple"));
  GC_CHECK(!HasArgument(plan.arguments, L"pull"));
  GC_CHECK(plan.operationId == L"fetch" && plan.displayName == L"fetch");
  GC_CHECK(TextContains(plan.confirmationText, L"branch.main.remote"));
  GC_CHECK(TextContains(plan.confirmationText,
                        L"git fetch --recurse-submodules=no --no-prune --no-prune-tags --no-tags origin"));
  GC_CHECK(TextContains(plan.confirmationText, L"HEAD 与本地分支都不在这次的范围里"));
  GC_CHECK(TextContains(plan.confirmationText, L"不删除任何引用"));
  GC_CHECK(TextContains(plan.confirmationText, L".git/FETCH_HEAD"));
  GC_CHECK(TextContains(plan.confirmationText, L"工作目录"));
  GC_CHECK(TextContains(plan.notice, L"只更新远端"));
}

GC_TEST(fetch_without_any_remote_is_blocked_without_guessing) {
  FetchTargetFacts facts = HealthyFacts();
  facts.configuredRemote.clear();
  facts.remotes.clear();
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == FetchPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  // 「不猜 origin、不创建远端、不改配置」必须出现在拒绝里。
  GC_CHECK(TextContains(plan.explanation, L"不替你猜一个 origin"));
  GC_CHECK(TextContains(plan.explanation, L"不创建远端"));
}

GC_TEST(fetch_unconfigured_but_single_remote_still_needs_an_explicit_choice) {
  // 只有一个普通远端：作为候选直接摆出来（默认选中就是它），但点不点仍由用户决定。
  FetchTargetFacts facts = HealthyFacts();
  facts.configuredRemote.clear();
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == FetchPlanState::chooseRemote);
  GC_REQUIRE_MESSAGE(plan.candidates.size() == 1, "应摆出唯一的既有远端");
  GC_CHECK(plan.candidates[0].name == L"origin");
  GC_CHECK(TextContains(plan.explanation, L"没有配置远端"));
  GC_CHECK(plan.arguments.empty());  // 选择界面点头之前不产生命令。

  // 用户在选择界面点了它之后才凑出 ready：来源说明必须写明「是选的不是猜的」。
  const FetchPlan chosen = gc::git::ChooseFetchRemote(facts, L"origin", kRoot);
  GC_CHECK(chosen.state == FetchPlanState::ready);
  GC_CHECK(chosen.remoteName == L"origin");
  GC_CHECK(TextContains(chosen.confirmationText, L"你在远端选择界面里刚选定的既有远端"));
  GC_REQUIRE_MESSAGE(chosen.arguments.size() == 6, "选定的方案应凑出六条参数");
  GC_CHECK(chosen.arguments[1] == L"--recurse-submodules=no");
}

GC_TEST(fetch_multiple_remotes_must_show_the_target) {
  FetchTargetFacts facts = HealthyFacts();
  facts.configuredRemote.clear();
  facts.remotes.push_back(gc::git::FetchRemoteEntry{L"gitee", L"https://example.invalid/x.git"});
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == FetchPlanState::chooseRemote);
  GC_REQUIRE_MESSAGE(plan.candidates.size() == 2, "应列出两个既有远端");
  GC_CHECK(plan.candidates[0].name == L"origin");
  GC_CHECK(plan.candidates[1].name == L"gitee");

  // 名字不在刚刚读回的清单里（外部改过仓库）：拒绝，不将就把用户点的字符串送进命令。
  const FetchPlan stale = gc::git::ChooseFetchRemote(facts, L"另一家", kRoot);
  GC_CHECK(stale.state == FetchPlanState::blocked);
  GC_CHECK(stale.arguments.empty());
}

GC_TEST(fetch_configured_remote_missing_from_list_falls_back_to_choice) {
  // branch.main.remote = origin，但清单里只有 gitee：配置与清单对不上，
  // 不修改配置、不猜「那就算了吧」，摆出真实清单让人选。
  FetchTargetFacts facts = HealthyFacts();
  facts.remotes = {gc::git::FetchRemoteEntry{L"gitee", L"D:\\x.git"}};
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == FetchPlanState::chooseRemote);
  GC_REQUIRE_MESSAGE(plan.candidates.size() == 1, "配置对不上时仍应摆出唯一的既有远端");
  GC_CHECK(TextContains(plan.explanation, L"不在"));
  GC_CHECK(TextContains(plan.explanation, L"不修改你的配置"));
}

GC_TEST(fetch_configured_remote_missing_and_no_remotes_blocks) {
  // 配置指向一个已不存在的远端，而且清单整个是空的：无从选择，明说并拒绝。
  FetchTargetFacts facts = HealthyFacts();
  facts.remotes.clear();
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, kRoot);
  GC_CHECK(plan.state == FetchPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"origin"));
  GC_CHECK(TextContains(plan.explanation, L"没有配置任何远端"));
  GC_CHECK(plan.arguments.empty());
}

GC_TEST(fetch_detached_head_still_chooses_from_existing_remotes) {
  FetchTargetQueries queries = HealthyQueries();
  queries.symbolicRef = Answer(1, L"");
  queries.branchRemoteRan = false;
  const FetchPlan plan = gc::git::BuildFetchPlan(gc::git::InterpretFetchTarget(queries), kRoot);
  // 游离 HEAD 与 fetch 并不冲突：抓取目标由用户在既有远端里点出来。
  GC_CHECK(plan.state == FetchPlanState::chooseRemote);
  GC_CHECK(TextContains(plan.explanation, L"不在任何分支上"));
}
