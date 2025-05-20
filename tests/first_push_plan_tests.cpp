// 「首次推送（分支还没有上游）」判读与方案的纯逻辑测试：全部用桩化的 GitQueryResult 与手工搭出的
// 事实驱动生产逻辑，不起真实 Git、不碰文件系统、不接触网络。覆盖：
//   * 向导的可用范围（CanOfferFirstPush）：只有「在分支上、有提交、上游问过且明确没有、配置读回来了、
//     仓库里至少有一个带地址的远端」这几件事同时成立才放行，其余一律沿用普通推送那句拒绝；
//   * 候选远端清单：顺序按 Git 给出的原样、问不成的候选带着原因留在清单里但不可选，
//     生效配置读不回来时一个候选都不摆；
//   * 三条查询的参数形态：check-ref-format 只收 refs/ 开头的完整引用（不带 `--`，实测挡不住）、
//     rev-parse --git-path config、以及与 pull 同形态的 rev-list --left-right --count；
//   * 上游写入的两条 git config：键名与取值逐字钉住（branch.<分支>.remote / .merge），
//     命令里绝不含 -u / --set-upstream / --global / --file 这类替代写法，形态不合格时一步都不构造；
//   * 目标分支名的即时形态底线与 refs/heads/ 构造；
//   * 方案的前提拒绝：引用名被 Git 判不能用、选定远端已从配置里消失、发布 URL 问不出、
//     上游在此期间被设好——一律 blocked，不产生命令也不构造配置写入；
//   * 风险清单：对端已有那条引用、各发布目标答的位置不一致、问不到（认证/权限/网络）
//     与「对端没有这条引用」必须分开措辞，多个发布地址、mirror/tagOpt 的中和照普通推送口径；
//   * 非快进：对端那份有本地没有的提交时明确写「会被 Git 以 non-fast-forward 拒绝」，
//     并且命令里永远不出现 --force / --force-with-lease；
//   * 执行前复核 DescribeFirstPushChange：分支 / 那份提交 / 上游是否还是空 / 选定远端 /
//     发布 URL / 引用名裁定 / 对端位置 / 会被中和的设置，任一变化都要作废；
//   * 展示与诊断里的凭据掩码（内嵌 userinfo 的 URL 不落进任何界面文字）。
// 真实 Git 的落地链路（真 push、真 git config 写入）在 first_push_fixture_tests.cpp 验证。
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/first_push_plan.h"
#include "git/push_plan.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FirstPushCandidateFacts;
using gc::git::FirstPushFacts;
using gc::git::FirstPushPlan;
using gc::git::FirstPushPlanInput;
using gc::git::FirstPushPlanState;
using gc::git::FirstPushPresenceSummary;
using gc::git::FirstPushProbeQueries;
using gc::git::FirstPushRemoteCandidate;
using gc::git::GitQueryResult;
using gc::git::PushPlanState;
using gc::git::PushPreflightFacts;
using gc::git::PushPreflightQueries;
using gc::git::PushTargetCheck;
using gc::git::UpstreamWriteStep;

constexpr std::wstring_view kRoot = L"D:\\仓 库";
constexpr std::wstring_view kHead = L"1111111111111111111111111111111111111111";
constexpr std::wstring_view kRemoteCommit = L"2222222222222222222222222222222222222222";
constexpr std::wstring_view kUrl = L"D:\\bare\\origin.git";
constexpr std::wstring_view kTopicRef = L"refs/heads/topic";

GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
  return result;
}

// 「明确没有」：退出码 1 且无输出（check-ref-format 对不合格引用名就是这个形态）。
GitQueryResult NoResult() { return Answer(1); }

GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
  return result;
}

std::wstring NulJoined(std::initializer_list<std::wstring_view> records) {
  std::wstring joined;
  for (const std::wstring_view record : records) {
    joined += record;
    joined.push_back(L'\0');
  }
  return joined;
}

std::wstring ConfigText(
    std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> pairs) {
  std::wstring joined;
  for (const auto& pair : pairs) {
    joined += pair.first;
    joined += L'\n';
    joined += pair.second;
    joined.push_back(L'\0');
  }
  return joined;
}

GitQueryResult ConfigListingAnswer(
    std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> pairs) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = 0;
  result.utf16Output = ConfigText(pairs);
  return result;
}

bool TextContains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool HasArgument(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

bool AnyArgumentContains(const std::vector<std::wstring>& arguments, std::wstring_view needle) {
  for (const std::wstring& argument : arguments) {
    if (TextContains(argument, needle)) {
      return true;
    }
  }
  return false;
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

// 一份「在 topic 上、有 HEAD、topic 没有上游、origin 只有一个 url、目标引用名可用、
// 对端还没有那条引用」的桩查询组：首次推送最常见的形态。
FirstPushProbeQueries HealthyProbeQueries() {
  FirstPushProbeQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/topic\n");
  queries.headObject = Answer(0, std::wstring(kHead) + L"\n");
  queries.upstreamRan = true;
  // for-each-ref 对没有上游的分支回答的是三个空字段（不是报错）：判读层据此得「没有上游」。
  queries.upstream = Answer(0, L"\t\t\n");
  queries.configListing = ConfigListingAnswer(
      {{L"core.repositoryformatversion", L"0"},
       {L"remote.origin.url", kUrl},
       {L"remote.origin.fetch", L"+refs/heads/*:refs/remotes/origin/*"}});
  queries.chosenRemoteName = L"origin";
  queries.chosenRemoteUrlRan = true;
  queries.chosenRemoteUrl = Answer(0, std::wstring(kUrl) + L"\n");
  queries.targetBranchRef = kTopicRef;
  queries.refFormatRan = true;
  queries.refFormat = Answer(0);
  // ls-remote 成功但对端没有那一行：这是「对端还没有这条分支」的明确答案。
  queries.presenceQueries = {Answer(0)};
  queries.configPathRan = true;
  queries.configPath = Answer(0, L".git/config\n");
  return queries;
}

FirstPushFacts HealthyFacts() { return gc::git::InterpretFirstPushProbe(HealthyProbeQueries()); }

FirstPushPlan PlanFor(const FirstPushFacts& facts, bool setUpstream = true) {
  return gc::git::BuildFirstPushPlan(
      FirstPushPlanInput{facts, std::wstring(kRoot), setUpstream});
}

const std::wstring* FindRisk(const FirstPushPlan& plan, std::wstring_view needle) {
  for (const std::wstring& risk : plan.risks) {
    if (TextContains(risk, needle)) {
      return &risk;
    }
  }
  return nullptr;
}

// 「还要交代的」那几句合成一段：断言只针对内容，不依赖它排在第几条。
std::wstring JoinNotes(const FirstPushPlan& plan) {
  std::wstring joined;
  for (const std::wstring& note : plan.notes) {
    joined += note;
  }
  return joined;
}

// 普通推送预检事实的桩：首次推送的「要不要走向导」判的就是这份事实。
PushPreflightQueries NoUpstreamPushQueries() {
  PushPreflightQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/topic\n");
  queries.headObject = Answer(0, std::wstring(kHead) + L"\n");
  queries.upstreamRan = true;
  queries.upstream = Answer(0, L"\t\t\n");
  queries.configListing = ConfigListingAnswer({{L"remote.origin.url", kUrl}});
  return queries;
}

// ---- 查询参数形态 ----

GC_TEST(first_push_ref_format_arguments_only_accept_full_refs) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildFirstPushRefFormatArguments(kRoot, kTopicRef);
  GC_CHECK_MESSAGE(!arguments.empty(), Narrow(arguments.empty() ? L"（空）" : arguments.front()));
  GC_CHECK(arguments.front() == L"-C");
  GC_CHECK(arguments[1] == kRoot);  // 仓库根显式绑定，不依赖进程全局目录
  GC_CHECK(HasArgument(arguments, L"--no-optional-locks"));
  GC_CHECK(HasArgument(arguments, L"check-ref-format"));
  GC_CHECK(arguments.back() == kTopicRef);
  // 实测本机 Git 的 check-ref-format 不认 `--`：带它仍然以 129 用法错误死在自己的参数解析上。
  // 因此这里只交 refs/ 开头的完整引用，绝不交裸名字，也不放 `--`。
  GC_CHECK(!HasArgument(arguments, L"--"));
  GC_CHECK(gc::git::BuildFirstPushRefFormatArguments(kRoot, L"topic").empty());
  GC_CHECK(gc::git::BuildFirstPushRefFormatArguments(kRoot, L"HEAD").empty());
  GC_CHECK(gc::git::BuildFirstPushRefFormatArguments(kRoot, L"refs/heads/a:b").empty());
  GC_CHECK(gc::git::BuildFirstPushRefFormatArguments({}, kTopicRef).empty());
}

GC_TEST(first_push_config_path_and_relationship_argument_shapes) {
  const std::vector<std::wstring> path = gc::git::BuildFirstPushConfigPathArguments(kRoot);
  GC_CHECK(HasArgument(path, L"rev-parse"));
  GC_CHECK(HasArgument(path, L"--git-path"));
  GC_CHECK(HasArgument(path, L"config"));
  GC_CHECK(gc::git::BuildFirstPushConfigPathArguments({}).empty());

  // 与 pull 的关系查询同一形态，但左右相反：写在前面的那个是对端的位置，
  // 于是「左=只有对端有」正是非快进的量。
  const std::vector<std::wstring> relationship =
      gc::git::BuildFirstPushRelationshipArguments(kRoot, kRemoteCommit, kHead);
  GC_CHECK(HasArgument(relationship, L"rev-list"));
  GC_CHECK(HasArgument(relationship, L"--left-right"));
  GC_CHECK(HasArgument(relationship, L"--count"));
  GC_CHECK(relationship.back() == std::wstring(kRemoteCommit) + L"..." + std::wstring(kHead));
  GC_CHECK(AnyArgumentContains(relationship, L"--no-replace-objects"));
  GC_CHECK(gc::git::BuildFirstPushRelationshipArguments(kRoot, L"abc", kHead).empty());
  GC_CHECK(gc::git::BuildFirstPushRelationshipArguments(kRoot, kRemoteCommit, L"HEAD").empty());
}

GC_TEST(first_push_target_branch_ref_is_built_as_full_head_ref) {
  GC_CHECK(gc::git::BuildFirstPushTargetBranchRef(L"topic") == kTopicRef);
  GC_CHECK(gc::git::BuildFirstPushTargetBranchRef(L"feature/nested") == L"refs/heads/feature/nested");
  GC_CHECK(gc::git::BuildFirstPushTargetBranchRef({}).empty());
  // 冒号与引号进不了命令窗口，也进不了这里构造的引用名。
  GC_CHECK(gc::git::BuildFirstPushTargetBranchRef(L"a:b").empty());
  GC_CHECK(gc::git::BuildFirstPushTargetBranchRef(L"a\"b").empty());
  GC_CHECK(gc::git::BuildFirstPushTargetBranchRef(std::wstring(L"a") + wchar_t{0x01}).empty());
}

GC_TEST(first_push_branch_name_guard_only_blocks_obvious_shapes) {
  GC_CHECK(gc::git::DescribeFirstPushBranchNameProblem(L"topic").empty());
  GC_CHECK(gc::git::DescribeFirstPushBranchNameProblem(L"feature/nested").empty());
  GC_CHECK(TextContains(gc::git::DescribeFirstPushBranchNameProblem({}), L"不能是空的"));
  GC_CHECK(TextContains(gc::git::DescribeFirstPushBranchNameProblem(L" a"), L"首尾"));
  GC_CHECK(TextContains(gc::git::DescribeFirstPushBranchNameProblem(L"a b"), L"空格"));
  GC_CHECK(!gc::git::DescribeFirstPushBranchNameProblem(L"a:b").empty());
  // 这里只做纯形态提示：像 `a..b`、`a.lock` 这类要由 Git 的 check-ref-format 裁定，
  // 输入框不冒充权威（它也不能在这里同步等子进程）。
  GC_CHECK(gc::git::DescribeFirstPushBranchNameProblem(L"a..b").empty());
  GC_CHECK(gc::git::DescribeFirstPushBranchNameProblem(L"a.lock").empty());
}

// ---- 上游写入的两条命令 ----

GC_TEST(first_push_upstream_steps_write_exactly_two_keys) {
  const std::vector<UpstreamWriteStep> steps =
      gc::git::BuildUpstreamWriteSteps(L"topic", L"origin", kTopicRef);
  GC_CHECK_MESSAGE(steps.size() == 2, "steps size is not 2");
  if (steps.size() != 2) {
    return;
  }
  // 「就这两个字面上的 git config 键值对」：先把它写成变量，再整只比较
  // （GC_CHECK 是宏，容纳不了带逗号的列表初始化）。
  const std::vector<std::wstring> expectedRemote{L"config", L"branch.topic.remote", L"origin"};
  const std::vector<std::wstring> expectedMerge{L"config", L"branch.topic.merge",
                                                std::wstring(kTopicRef)};
  GC_CHECK(steps[0].key == L"branch.topic.remote");
  GC_CHECK(steps[0].value == L"origin");
  GC_CHECK(steps[0].arguments == expectedRemote);
  GC_CHECK(steps[1].key == L"branch.topic.merge");
  GC_CHECK(steps[1].value == kTopicRef);
  GC_CHECK(steps[1].arguments == expectedMerge);
  for (const UpstreamWriteStep& step : steps) {
    // 只写这一把键、只走本地：不带 --global/--system/--file，也不带 -u/--set-upstream
    // （本程序刻意不用 -u：那样「推送」与「写配置」就只剩一个退出码，分不开报告）。
    GC_CHECK(!AnyArgumentContains(step.arguments, L"--global"));
    GC_CHECK(!AnyArgumentContains(step.arguments, L"--system"));
    GC_CHECK(!AnyArgumentContains(step.arguments, L"--file"));
    GC_CHECK(!AnyArgumentContains(step.arguments, L"--set-upstream"));
    GC_CHECK(!AnyArgumentContains(step.arguments, L"-u"));
    GC_CHECK(!AnyArgumentContains(step.arguments, L"--no-verify"));
    // 操作 ID 必须能安全进消息、文件名与结果比对（纯 ASCII、长度受限）。
    GC_CHECK(!step.operationId.empty() && step.operationId.size() <= 32);
    for (const wchar_t c : step.operationId) {
      GC_CHECK((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_');
    }
  }
  GC_CHECK(steps[0].operationId != steps[1].operationId);
  // 分支名里带斜杠时键名照原样：Git 的 branch.<名> 子段就是这个写法。
  const std::vector<UpstreamWriteStep> nested =
      gc::git::BuildUpstreamWriteSteps(L"feature/nested", L"origin", L"refs/heads/feature/nested");
  GC_CHECK(nested.size() == 2 && nested[0].key == L"branch.feature/nested.remote");
}

GC_TEST(first_push_upstream_steps_refuse_unusable_names) {
  GC_CHECK(gc::git::BuildUpstreamWriteSteps({}, L"origin", kTopicRef).empty());
  GC_CHECK(gc::git::BuildUpstreamWriteSteps(L"topic", {}, kTopicRef).empty());
  GC_CHECK(gc::git::BuildUpstreamWriteSteps(L"topic", L"-origin", kTopicRef).empty());
  GC_CHECK(gc::git::BuildUpstreamWriteSteps(L"topic", L"or:igin", kTopicRef).empty());
  GC_CHECK(gc::git::BuildUpstreamWriteSteps(L"topic", L"origin", L"topic").empty());
  GC_CHECK(gc::git::BuildUpstreamWriteSteps(std::wstring(L"top") + wchar_t{0x01}, L"origin",
                                            kTopicRef)
               .empty());
}

// ---- 向导的可用范围 ----

GC_TEST(first_push_offer_only_when_upstream_is_confirmed_absent) {
  const PushPreflightFacts facts = gc::git::InterpretPushPreflight(NoUpstreamPushQueries());
  GC_CHECK_MESSAGE(facts.queryOk && facts.configOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.upstreamRan && !facts.upstreamConfigured && facts.upstreamRemote.empty());
  GC_CHECK(gc::git::CanOfferFirstPush(facts));

  PushPreflightQueries detached = NoUpstreamPushQueries();
  detached.symbolicRef = NoResult();  // 游离 HEAD：symbolic-ref 明确回答「不在分支上」
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(detached)));

  PushPreflightQueries unborn = NoUpstreamPushQueries();
  unborn.headObject = NoResult();  // 还没有任何提交：连「推哪一份」都没有
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(unborn)));

  PushPreflightQueries notAsked = NoUpstreamPushQueries();
  notAsked.upstreamRan = false;  // 上游压根没问过：不能断言「没有上游」
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(notAsked)));

  PushPreflightQueries upstreamFailed = NoUpstreamPushQueries();
  upstreamFailed.upstream = Answer(128, {}, L"fatal: bad config");
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(upstreamFailed)));

  PushPreflightQueries noRemotes = NoUpstreamPushQueries();
  noRemotes.configListing = ConfigListingAnswer({{L"core.repositoryformatversion", L"0"}});
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(noRemotes)));

  PushPreflightQueries configFailed = NoUpstreamPushQueries();
  configFailed.configListing = LaunchFailed();
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(configFailed)));

  // 已经有上游的仓：走普通推送，不该出现在这里。
  PushPreflightQueries withUpstream = NoUpstreamPushQueries();
  withUpstream.upstream = Answer(0, L"origin\trefs/remotes/origin/topic\trefs/heads/topic\n");
  withUpstream.trackingRan = true;
  withUpstream.trackingObject = Answer(0, std::wstring(kRemoteCommit) + L"\n");
  withUpstream.remoteUrlRan = true;
  withUpstream.remoteUrl = Answer(0, std::wstring(kUrl) + L"\n");
  GC_CHECK(!gc::git::CanOfferFirstPush(gc::git::InterpretPushPreflight(withUpstream)));
}

// ---- 候选远端清单 ----

GC_TEST(first_push_candidates_keep_git_order_and_failures) {
  const GitQueryResult config = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.fork.url", L"D:\\bare\\fork.git"},
      {L"remote.fork.pushurl", L"D:\\bare\\fork-push.git"},
      {L"remote.onlyfetch.url", L"D:\\bare\\onlyfetch.git"},
  });
  std::vector<GitQueryResult> urlQueries;
  urlQueries.push_back(Answer(0, std::wstring(kUrl) + L"\n"));
  urlQueries.push_back(Answer(0, L"D:\\bare\\fork-push.git\n"));
  urlQueries.push_back(Answer(128, {}, L"fatal: 'onlyfetch' does not appear to be a git repository"));
  const FirstPushCandidateFacts candidates =
      gc::git::InterpretFirstPushCandidates(config, urlQueries);
  GC_CHECK(candidates.configOk);
  // 远端清单是「有 url 或 pushurl 的名字，按首次出现顺序去重」：origin、fork、onlyfetch 三个。
  GC_CHECK(candidates.candidates.size() == 3);
  if (candidates.candidates.size() != 3) {
    return;
  }
  GC_CHECK(candidates.candidates[0].name == L"origin");
  GC_CHECK(candidates.candidates[0].selectable());
  GC_CHECK(candidates.candidates[0].urlsDisplay() == gc::git::FormatPushUrlList({std::wstring(kUrl)}));
  // 有 pushurl 时 Git 展开的就是 pushurl：候选展示的是实际发布地址，不是抓取地址。
  GC_CHECK(candidates.candidates[1].name == L"fork");
  GC_CHECK(candidates.candidates[1].publishUrls.size() == 1 &&
           candidates.candidates[1].publishUrls[0] == L"D:\\bare\\fork-push.git");
  // 问不成的候选带着原因留在清单里，但不可能被选中。
  GC_CHECK(!candidates.candidates[2].selectable());
  GC_CHECK(TextContains(candidates.candidates[2].urlsDisplay(), L"不能选"));
  // 查询条数比名字少：多出来的名字如实记成「没发出去」，不拿前一个远端的地址充数。
  const FirstPushCandidateFacts shortList =
      gc::git::InterpretFirstPushCandidates(config, {urlQueries[0]});
  GC_CHECK(shortList.candidates.size() == 3);
  if (shortList.candidates.size() == 3) {
    GC_CHECK(!shortList.candidates[1].selectable());
    GC_CHECK(TextContains(shortList.candidates[1].failure, L"没有发出去"));
    GC_CHECK(shortList.candidates[0].selectable());
  }
}

GC_TEST(first_push_candidates_refuse_unreadable_config) {
  GitQueryResult truncated = Answer(0, L"remote.origin.url\nD:\\bare\\origin.git");  // 缺结尾 NUL
  const FirstPushCandidateFacts candidates = gc::git::InterpretFirstPushCandidates(truncated, {});
  GC_CHECK(!candidates.configOk);
  GC_CHECK(candidates.candidates.empty());
  GC_CHECK(TextContains(candidates.configFailure, L"记录约定"));
  const std::wstring refusal = gc::git::DescribeFirstPushCandidateRefusal(candidates);
  GC_CHECK(TextContains(refusal, L"生效配置"));
  GC_CHECK(TextContains(refusal, L"不猜一个远端名"));
}

GC_TEST(first_push_candidate_refusal_names_every_unusable_remote) {
  const GitQueryResult config = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.fork.url", L"D:\\bare\\fork.git"},
  });
  const std::vector<GitQueryResult> bothFailed = {
      Answer(128, {}, L"fatal: no such directory"),
      Answer(128, {}, L"fatal: no such directory"),
  };
  const FirstPushCandidateFacts candidates =
      gc::git::InterpretFirstPushCandidates(config, bothFailed);
  GC_CHECK(candidates.candidates.size() == 2);
  const std::wstring refusal = gc::git::DescribeFirstPushCandidateRefusal(candidates);
  GC_CHECK(TextContains(refusal, L"origin") && TextContains(refusal, L"fork"));
  GC_CHECK(TextContains(refusal, L"没有一个能确定发布地址"));
}

// ---- 方案：ready 的两步与只推送 ----

GC_TEST(first_push_plan_lists_push_and_upstream_separately) {
  const FirstPushPlan plan = PlanFor(HealthyFacts(), /*setUpstream=*/true);
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.branchName == L"topic");
  GC_CHECK(plan.remoteName == L"origin");
  GC_CHECK(plan.targetBranchRef == kTopicRef);
  GC_CHECK(plan.pushedObjectId == kHead);
  GC_CHECK(plan.arguments ==
           gc::git::BuildPushCommandArguments(L"origin", kHead, kTopicRef, false, false));
  GC_CHECK(!AnyArgumentContains(plan.arguments, L"--force"));
  GC_CHECK(!AnyArgumentContains(plan.arguments, L"--set-upstream"));
  GC_CHECK(!AnyArgumentContains(plan.arguments, L"-u"));
  GC_CHECK(plan.upstreamSteps.size() == 2);
  GC_CHECK(plan.operationId == L"push");
  GC_CHECK(plan.setUpstreamRequested);

  // 确认框必须把两件事分别列出来，并写清上游那两个键与值、适用分支与文件落点。
  GC_CHECK(TextContains(plan.confirmationText, L"第 1 步（推送）"));
  GC_CHECK(TextContains(plan.confirmationText, L"第 2 步（设置本地上游）"));
  GC_CHECK(TextContains(plan.confirmationText, L"git push"));
  GC_CHECK(TextContains(plan.confirmationText, L"branch.topic.remote = origin"));
  GC_CHECK(TextContains(plan.confirmationText, L"branch.topic.merge = refs/heads/topic"));
  GC_CHECK(TextContains(plan.confirmationText, L"git config branch.topic.remote origin"));
  GC_CHECK(TextContains(plan.confirmationText, L"适用的分支只有 topic"));
  GC_CHECK(TextContains(plan.confirmationText, L".git/config"));
  GC_CHECK(TextContains(plan.confirmationText, L"为什么不用 git push -u"));
  GC_CHECK(TextContains(plan.confirmationText, L"完整 ID " + std::wstring(kHead)));
  GC_CHECK(TextContains(plan.explanation, L"首次推送"));
  GC_CHECK(TextContains(plan.notice, L"上游那两条 git config 只在推送成功之后才发"));
}

GC_TEST(first_push_plan_without_upstream_write_says_so) {
  const FirstPushPlan plan = PlanFor(HealthyFacts(), /*setUpstream=*/false);
  GC_CHECK(plan.state == FirstPushPlanState::ready);
  GC_CHECK(plan.upstreamSteps.empty());
  GC_CHECK(!plan.setUpstreamRequested);
  GC_CHECK(TextContains(plan.confirmationText, L"这次**不做**"));
  GC_CHECK(TextContains(plan.notice, L"本次不写任何配置文件"));
  GC_CHECK(PlanFor(HealthyFacts(), true).arguments == plan.arguments);  // 推送那一步不受选择影响
}

GC_TEST(first_push_plan_blocks_unusable_branch_name_before_building_anything) {
  // 分支名里有命令窗口无法安全表达的写法：既构造不出那条 push，也构造不出上游的两条配置键。
  // 「答应了推送并设置上游」却不能只做一半，因此整个方案作废。
  const std::wstring dirtyRef = std::wstring(L"refs/heads/top") + wchar_t{0x01};
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.symbolicRef = Answer(0, dirtyRef + L"\n");
  queries.targetBranchRef = dirtyRef;
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  const FirstPushPlan plan = PlanFor(facts, true);
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::blocked, Narrow(plan.explanation));
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.upstreamSteps.empty());
  GC_CHECK(TextContains(plan.explanation, L"形态不合格"));
  GC_CHECK(TextContains(plan.explanation, L"没有构造任何命令"));
}

// ---- 方案：前提不成立 ----

GC_TEST(first_push_invalid_ref_name_is_blocked_before_any_command) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.refFormat = NoResult();  // Git 明确回答：这个引用名不能用
  queries.presenceQueries.clear();  // 那种场合连向外问都不该问（夹具的编排也按这个顺序做）
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  GC_CHECK(facts.refFormatRan && !facts.refFormatAccepted);
  const FirstPushPlan plan = PlanFor(facts);
  GC_CHECK(plan.state == FirstPushPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.upstreamSteps.empty());
  GC_CHECK(TextContains(plan.explanation, L"check-ref-format"));
  GC_CHECK(TextContains(plan.explanation, L"没有发出任何命令"));
}

GC_TEST(first_push_ref_format_query_failure_is_not_a_rejection_of_the_name) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.refFormat = Answer(128, {}, L"fatal: unable to read");  // 问不成，不是「名字不合格」
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(!facts.refFormatAccepted);
  GC_CHECK(TextContains(facts.refFormatDetail, L"没能问出来"));
  const FirstPushPlan plan = PlanFor(facts);
  GC_CHECK(plan.state == FirstPushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"裁定没有通过"));
}

GC_TEST(first_push_blocked_when_upstream_was_configured_in_meantime) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.upstream = Answer(0, L"origin\trefs/remotes/origin/topic\trefs/heads/topic\n");
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.upstreamConfigured);
  const FirstPushPlan plan = PlanFor(facts);
  GC_CHECK(plan.state == FirstPushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"已经有上游"));
  GC_CHECK(TextContains(plan.explanation, L"普通推送"));
  GC_CHECK(plan.arguments.empty() && plan.upstreamSteps.empty());
}

GC_TEST(first_push_blocked_when_chosen_remote_left_the_repository) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.chosenRemoteName = L"fork";  // 配置里已经没有这个远端
  queries.chosenRemoteUrl = Answer(128, {}, L"fatal: 'fork' does not exist");
  queries.presenceQueries.clear();
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.chosenRemoteNameUsable && !facts.chosenRemoteExists);
  const FirstPushPlan plan = PlanFor(facts);
  GC_CHECK(plan.state == FirstPushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"已经没有地址"));
  GC_CHECK(TextContains(plan.explanation, L"git remote add"));
}

GC_TEST(first_push_blocked_when_publish_url_cannot_be_resolved) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.chosenRemoteUrl = Answer(0);  // 成功返回但一个地址都没答：这个远端实际没有可推的目标
  queries.presenceQueries.clear();
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.publishUrls.empty() && !facts.publishUrlFailure.empty());
  const FirstPushPlan plan = PlanFor(facts);
  GC_CHECK(plan.state == FirstPushPlanState::blocked);
  // 绝不退回配置里那个未经解析的原样地址。
  GC_CHECK(!TextContains(plan.explanation, std::wstring(kUrl)));
  GC_CHECK(TextContains(plan.explanation, L"不会拿配置里未经解析的原样地址"));
}

GC_TEST(first_push_blocked_for_detached_and_unborn) {
  FirstPushProbeQueries detached = HealthyProbeQueries();
  detached.symbolicRef = NoResult();
  GC_CHECK(TextContains(PlanFor(gc::git::InterpretFirstPushProbe(detached)).explanation,
                        L"游离 HEAD"));

  FirstPushProbeQueries unborn = HealthyProbeQueries();
  unborn.headObject = NoResult();
  GC_CHECK(TextContains(PlanFor(gc::git::InterpretFirstPushProbe(unborn)).explanation,
                        L"还没有任何提交"));

  GC_CHECK(PlanFor(HealthyFacts(), true).state == FirstPushPlanState::ready);
  GC_CHECK(PlanFor(HealthyFacts(), true).state == FirstPushPlanState::ready);
}

GC_TEST(first_push_blocked_without_repository_directory) {
  const FirstPushPlan plan = gc::git::BuildFirstPushPlan(
      FirstPushPlanInput{HealthyFacts(), std::wstring(), true});
  GC_CHECK(plan.state == FirstPushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"工作区根目录"));
}

// ---- 对端现状：在 / 不在 / 问不到 必须分开 ----

GC_TEST(first_push_new_branch_on_remote_is_stated_as_creation) {
  const FirstPushFacts facts = HealthyFacts();
  GC_CHECK(facts.presenceKnown && !facts.remoteRefExists);
  const FirstPushPlan plan = PlanFor(facts, false);
  GC_CHECK(plan.state == FirstPushPlanState::ready);
  GC_CHECK(TextContains(plan.confirmationText, L"对端现状：每个发布目标都答了——那边现在**没有**"));
  GC_CHECK(FindRisk(plan, L"新建") == nullptr);  // 这是范围说明，不是风险：不必额外点头
}

GC_TEST(first_push_existing_remote_branch_requires_explicit_nod) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.presenceQueries = {Answer(0, std::wstring(kRemoteCommit) + L"\trefs/heads/topic\n")};
  queries.relationshipRan = true;
  queries.relationship = Answer(0, L"1\t3\n");  // 对端 1 个独有、本地 3 个独有
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.presenceKnown && facts.remoteRefExists && !facts.presenceDisagrees);
  GC_CHECK(facts.remoteObjectId == kRemoteCommit);
  GC_CHECK(facts.relationshipKnown && facts.remoteOnly == 1 && facts.localOnly == 3);
  const FirstPushPlan plan = PlanFor(facts, false);
  GC_CHECK(plan.state == FirstPushPlanState::ready);
  GC_CHECK(plan.requiresForce);
  const std::wstring* risk = FindRisk(plan, L"不是在对端新建分支");
  GC_CHECK(risk != nullptr);
  if (risk != nullptr) {
    GC_CHECK(TextContains(*risk, L"没有 --force"));
  }
  GC_CHECK(TextContains(plan.confirmationText, L"已经存在"));
  GC_CHECK(TextContains(plan.confirmationText, L"non-fast-forward 拒绝"));
}

GC_TEST(first_push_fast_forwardable_existing_branch_says_so) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.presenceQueries = {Answer(0, std::wstring(kRemoteCommit) + L"\trefs/heads/topic\n")};
  queries.relationshipRan = true;
  queries.relationship = Answer(0, L"0\t3\n");
  const FirstPushPlan plan = PlanFor(gc::git::InterpretFirstPushProbe(queries), false);
  GC_CHECK(TextContains(plan.confirmationText, L"推送是快进"));
  GC_CHECK(FindRisk(plan, L"会被 Git 以 non-fast-forward 拒绝") == nullptr);
}

GC_TEST(first_push_already_same_commit_is_stated_plainly) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.presenceQueries = {Answer(0, std::wstring(kHead) + L"\trefs/heads/topic\n")};
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.remoteObjectId == kHead);
  const FirstPushPlan plan = PlanFor(facts, false);
  GC_CHECK(TextContains(plan.confirmationText, L"Everything up-to-date"));
}

GC_TEST(first_push_relationship_query_failure_is_not_zero) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.presenceQueries = {Answer(0, std::wstring(kRemoteCommit) + L"\trefs/heads/topic\n")};
  queries.relationshipRan = true;
  queries.relationship = Answer(128, {}, L"fatal: Invalid symmetric difference expression");
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(!facts.relationshipKnown);
  const FirstPushPlan plan = PlanFor(facts, false);
  GC_CHECK(TextContains(plan.confirmationText, L"快进判断：没能问出来"));
  GC_CHECK(!TextContains(plan.confirmationText, L"落后 0 个"));
}

GC_TEST(first_push_unreachable_target_is_not_reported_as_absent) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.presenceQueries = {
      Answer(128, {}, L"fatal: Authentication failed for 'https://user:secret@host/'")};
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.publishUrls.size() == 1);
  GC_CHECK(!facts.presenceKnown);
  GC_CHECK(!facts.remoteRefExists);
  const FirstPushPlan plan = PlanFor(facts, true);
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::ready, Narrow(plan.explanation));
  GC_CHECK(plan.requiresForce);
  const std::wstring* risk = FindRisk(plan, L"与「对端没有这条分支」是两件事");
  GC_CHECK(risk != nullptr);
  GC_CHECK(risk != nullptr);
  // 问不到不是拒绝理由：这条推送仍然可以发出去，非快进由 Git 自己拦。
  GC_CHECK(plan.state == FirstPushPlanState::ready);
  // 认证失败那句话里带着内嵌凭据的地址：进界面之前必须掩码。
  GC_CHECK(!TextContains(plan.confirmationText, L"secret"));
  GC_CHECK(!TextContains(plan.explanation, L"secret"));
}

GC_TEST(first_push_partial_target_answers_keep_each_url) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.pushurl", L"https://user:secret@host/one.git"},
      {L"remote.origin.pushurl", L"https://user:secret@host/two.git"},
  });
  // 配了 pushurl 时 `get-url --push` 答的就是 pushurl 那两条（条数须与配置里的 pushurl 条目一致，
  // 否则 ResolvePushUrls 会按「看不清完整地址」拒绝）。
  queries.chosenRemoteUrl =
      Answer(0, L"https://user:secret@host/one.git\nhttps://user:secret@host/two.git\n");
  queries.presenceQueries = {
      Answer(0, std::wstring(kHead) + L"\trefs/heads/topic\n"),
      Answer(128, {}, L"fatal: Could not read from remote repository"),
  };
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.publishUrls.size() == 2);
  const FirstPushPlan plan = PlanFor(facts, true);
  GC_CHECK(plan.state == FirstPushPlanState::ready);
  GC_CHECK(!facts.presenceKnown);  // 一个问到、一个没问到：不能整体当成「对端没有」
  GC_CHECK(plan.pushUrls == facts.publishUrls);
  GC_CHECK(TextContains(plan.confirmationText, L"***@host"));  // 逐条展示都掩过凭据
  GC_CHECK(!TextContains(plan.confirmationText, L"user:secret"));
  GC_CHECK(FindRisk(plan, L"个发布地址，实测 Git 会推给每一个") != nullptr);
}

GC_TEST(first_push_targets_disagreeing_are_not_merged) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.pushurl", L"D:\\bare\\one.git"},
      {L"remote.origin.pushurl", L"D:\\bare\\two.git"},
  });
  queries.chosenRemoteUrl = Answer(0, L"D:\\bare\\one.git\nD:\\bare\\two.git\n");
  queries.presenceQueries = {
      Answer(0, std::wstring(kRemoteCommit) + L"\trefs/heads/topic\n"),
      Answer(0, std::wstring(kHead) + L"\trefs/heads/topic\n"),
  };
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.presenceKnown && facts.remoteRefExists && facts.presenceDisagrees);
  const FirstPushPlan plan = PlanFor(facts, true);
  GC_CHECK(TextContains(plan.confirmationText, L"各个发布目标答的位置不一样"));
  GC_CHECK(FindRisk(plan, L"不把它们合并成一个位置") != nullptr);
  // 位置不一致时不发关系查询，也不写「快进/非快进」的结论。
  GC_CHECK(!facts.relationshipKnown);
  GC_CHECK(!TextContains(plan.confirmationText, L"推送是快进"));
}

GC_TEST(first_push_presence_summary_covers_not_asked_targets) {
  PushTargetCheck answered;
  answered.url = kUrl;
  answered.queried = true;
  answered.ok = true;
  PushTargetCheck missing;
  missing.url = L"D:\\bare\\other.git";
  missing.queried = false;
  missing.failure = L"这条询问没有发出去";
  const FirstPushPresenceSummary summary =
      gc::git::SummarizeFirstPushPresence({answered, missing});
  GC_CHECK(!summary.known);
  GC_CHECK(!summary.exists);
  GC_CHECK(TextContains(summary.detail, L"没能问到"));
  GC_CHECK(TextContains(summary.detail, L"与「对端没有这条分支」是两件事"));
  GC_CHECK(!gc::git::SummarizeFirstPushPresence({}).known);
  GC_CHECK(gc::git::SummarizeFirstPushPresence({answered}).known);
}

// ---- 与普通推送同一份 URL 解析与中和口径 ----

GC_TEST(first_push_shares_url_resolution_with_push) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  // 配置里是抓取地址，Git 展开成 insteadOf 改写后的发布地址：与 push_plan 同一份判读。
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", L"https://host/origin.git"},
      {L"url.https://mirror.host/.insteadof", L"https://host/"},
  });
  queries.chosenRemoteUrl = Answer(0, L"https://mirror.host/origin.git\n");
  queries.presenceQueries = {Answer(0)};
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.publishUrls.size() == 1 &&
           facts.publishUrls[0] == L"https://mirror.host/origin.git");
  GC_CHECK(facts.publishUrlRewritten);
  const FirstPushPlan plan = PlanFor(facts, true);
  GC_CHECK(plan.state == FirstPushPlanState::ready);
  GC_CHECK(FindRisk(plan, L"insteadOf") != nullptr);
  GC_CHECK(TextContains(plan.notes.front() + JoinNotes(plan), L"改写之后的结果"));
}

GC_TEST(first_push_neutralizes_mirror_and_tagopt_of_the_chosen_remote) {
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.mirror", L"true"},
      {L"remote.origin.tagopt", L"--tags"},
      {L"push.followtags", L"true"},
  });
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(facts.scopeEffects.mirrorConfigured && facts.scopeEffects.tagOptConfigured &&
           facts.scopeEffects.followTagsConfigured);
  const FirstPushPlan plan = PlanFor(facts, true);
  GC_CHECK(HasArgument(plan.arguments, L"-c"));
  GC_CHECK(HasArgument(plan.arguments, L"remote.origin.mirror=false"));
  GC_CHECK(HasArgument(plan.arguments, L"remote.origin.tagopt="));
  GC_CHECK(HasArgument(plan.arguments, L"push.followTags=false"));
  // 中和项的说明必须出现在确认框里（与普通推送同一份措辞）。
  GC_CHECK(TextContains(plan.confirmationText, L"临时按 -c remote.origin.mirror=false 发出"));
  GC_CHECK(FindRisk(plan, L"配置文件一个字不写") != nullptr);
  // -c 覆盖是累加的，不会替换多值键：这里不拿 -c 去钉目标（普通推送同口径）。
  GC_CHECK(!AnyArgumentContains(plan.arguments, L"remote.origin.pushurl="));
}

GC_TEST(first_push_scope_flags_come_from_the_chosen_remote) {
  // mirror 配在别的远端上：选定 origin 时不该为它补 -c。
  FirstPushProbeQueries queries = HealthyProbeQueries();
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.fork.url", L"D:\\bare\\fork.git"},
      {L"remote.fork.mirror", L"true"},
  });
  const FirstPushFacts facts = gc::git::InterpretFirstPushProbe(queries);
  GC_CHECK(!facts.scopeEffects.mirrorConfigured);
  GC_CHECK(!AnyArgumentContains(PlanFor(facts, true).arguments, L"mirror=false"));
}

// ---- 执行前复核 ----

GC_TEST(first_push_recheck_accepts_identical_facts) {
  const FirstPushFacts preflight = HealthyFacts();
  GC_CHECK(gc::git::DescribeFirstPushChange(preflight, HealthyFacts()).empty());
}

GC_TEST(first_push_recheck_detects_target_url_change) {
  const FirstPushFacts preflight = HealthyFacts();
  FirstPushProbeQueries moved = HealthyProbeQueries();
  moved.configListing = ConfigListingAnswer({
      {L"remote.origin.url", L"D:\\bare\\moved.git"},
  });
  moved.chosenRemoteUrl = Answer(0, L"D:\\bare\\moved.git\n");
  moved.presenceQueries = {Answer(0)};
  const std::wstring change =
      gc::git::DescribeFirstPushChange(preflight, gc::git::InterpretFirstPushProbe(moved));
  GC_CHECK_MESSAGE(!change.empty(), Narrow(change));
  GC_CHECK(TextContains(change, L"发布 URL"));
  GC_CHECK(TextContains(change, L"D:\\bare\\moved.git"));
  GC_CHECK(TextContains(change, L"没有发出任何命令"));
}

GC_TEST(first_push_recheck_detects_upstream_appearing) {
  const FirstPushFacts preflight = HealthyFacts();
  FirstPushProbeQueries nowConfigured = HealthyProbeQueries();
  nowConfigured.upstream = Answer(0, L"origin\trefs/remotes/origin/topic\trefs/heads/topic\n");
  const std::wstring change =
      gc::git::DescribeFirstPushChange(preflight, gc::git::InterpretFirstPushProbe(nowConfigured));
  GC_CHECK(TextContains(change, L"上游已经被设好"));
  GC_CHECK(TextContains(change, L"普通推送"));
}

GC_TEST(first_push_recheck_detects_head_branch_and_remote_changes) {
  const FirstPushFacts preflight = HealthyFacts();

  FirstPushProbeQueries headMoved = HealthyProbeQueries();
  headMoved.headObject = Answer(0, std::wstring(kRemoteCommit) + L"\n");
  GC_CHECK(TextContains(gc::git::DescribeFirstPushChange(preflight,
                                                         gc::git::InterpretFirstPushProbe(headMoved)),
                        L"要推送的提交从"));

  FirstPushProbeQueries branchMoved = HealthyProbeQueries();
  branchMoved.symbolicRef = Answer(0, L"refs/heads/other\n");
  GC_CHECK(TextContains(gc::git::DescribeFirstPushChange(preflight,
                                                         gc::git::InterpretFirstPushProbe(branchMoved)),
                        L"当前分支从"));

  FirstPushProbeQueries remoteGone = HealthyProbeQueries();
  remoteGone.configListing = ConfigListingAnswer({{L"core.repositoryformatversion", L"0"}});
  remoteGone.chosenRemoteUrl = Answer(128, {}, L"fatal: 'origin' does not exist");
  remoteGone.presenceQueries.clear();
  GC_CHECK(TextContains(
      gc::git::DescribeFirstPushChange(preflight, gc::git::InterpretFirstPushProbe(remoteGone)),
      L"已经没有地址"));

  FirstPushProbeQueries formatFlipped = HealthyProbeQueries();
  formatFlipped.refFormat = NoResult();
  formatFlipped.presenceQueries.clear();
  GC_CHECK(TextContains(
      gc::git::DescribeFirstPushChange(preflight, gc::git::InterpretFirstPushProbe(formatFlipped)),
      L"裁定也变了"));
}

GC_TEST(first_push_recheck_detects_remote_position_change) {
  const FirstPushFacts preflight = HealthyFacts();
  FirstPushProbeQueries appeared = HealthyProbeQueries();
  appeared.presenceQueries = {Answer(0, std::wstring(kRemoteCommit) + L"\trefs/heads/topic\n")};
  const std::wstring change =
      gc::git::DescribeFirstPushChange(preflight, gc::git::InterpretFirstPushProbe(appeared));
  GC_CHECK(TextContains(change, L"对端那条引用的现状与预检时不一样了"));
  GC_CHECK(TextContains(change, L"还没有"));
  GC_CHECK(TextContains(change, L"已有"));
}

GC_TEST(first_push_recheck_refuses_incomplete_recheck) {
  const FirstPushFacts preflight = HealthyFacts();
  FirstPushFacts latest;  // 一条都没问成
  const std::wstring change = gc::git::DescribeFirstPushChange(preflight, latest);
  GC_CHECK(TextContains(change, L"复核没能完成"));
  GC_CHECK(TextContains(change, L"没有写任何配置"));
}

}  // namespace

// ---- R1：写上游之前的现场复核（DescribeUpstreamWriteStaleness）----

namespace {

// 「前提还在」的那一份现场：还是这条分支、上游问过且明确没有。
PushPreflightFacts StillNoUpstreamFacts() {
  PushPreflightFacts facts;
  facts.queryOk = true;
  facts.onBranch = true;
  facts.branchRef = L"refs/heads/main";
  facts.branchName = L"main";
  facts.headQueried = true;
  facts.headResolved = true;
  facts.headObjectId = std::wstring(kHead);
  facts.upstreamRan = true;
  facts.upstreamConfigured = false;
  return facts;
}

std::vector<UpstreamWriteStep> TwoUpstreamSteps() {
  return gc::git::BuildUpstreamWriteSteps(L"main", L"origin", L"refs/heads/main");
}

}  // namespace

GC_TEST(first_push_upstream_staleness_only_allows_the_reviewed_plan) {
  const std::vector<UpstreamWriteStep> steps = TwoUpstreamSteps();
  GC_REQUIRE(steps.size() == 2, "两条上游步骤应生成成功");

  // 前提原样成立：回空串，调用方据此照原方案发命令。
  GC_CHECK(gc::git::DescribeUpstreamWriteStaleness(StillNoUpstreamFacts(), steps,
                                                   L"refs/heads/main")
               .empty());

  // 空步骤不构成「可以写」。
  GC_CHECK(!gc::git::DescribeUpstreamWriteStaleness(StillNoUpstreamFacts(), {}, L"refs/heads/main")
                .empty());

  // 问不回现场 = 不能确定，绝不写（「问不到」永远不是「没有」）。
  PushPreflightFacts unreadable = StillNoUpstreamFacts();
  unreadable.queryOk = false;
  unreadable.queryFailure = L"git 没能启动";
  const std::wstring refuseUnread =
      gc::git::DescribeUpstreamWriteStaleness(unreadable, steps, L"refs/heads/main");
  GC_CHECK(!refuseUnread.empty());
  GC_CHECK(TextContains(refuseUnread, L"一条配置都不写"));
  GC_CHECK(TextContains(refuseUnread, L"git 没能启动"));

  PushPreflightFacts upstreamNotAsked = StillNoUpstreamFacts();
  upstreamNotAsked.upstreamRan = false;
  const std::wstring refuseNotAsked =
      gc::git::DescribeUpstreamWriteStaleness(upstreamNotAsked, steps, L"refs/heads/main");
  GC_CHECK(!refuseNotAsked.empty());
  GC_CHECK(TextContains(refuseNotAsked, L"没能问出"));

  // 分支不在了 / 换成别的分支：按分支名落的键就不再是确认过的那件事。
  PushPreflightFacts detached = StillNoUpstreamFacts();
  detached.onBranch = false;
  detached.branchRef.clear();
  const std::wstring refuseDetached =
      gc::git::DescribeUpstreamWriteStaleness(detached, steps, L"refs/heads/main");
  GC_CHECK(!refuseDetached.empty());
  GC_CHECK(TextContains(refuseDetached, L"不再是命名分支"));

  PushPreflightFacts switched = StillNoUpstreamFacts();
  switched.branchRef = L"refs/heads/dev";
  switched.branchName = L"dev";
  const std::wstring refuseSwitched =
      gc::git::DescribeUpstreamWriteStaleness(switched, steps, L"refs/heads/main");
  GC_CHECK(!refuseSwitched.empty());
  GC_CHECK(TextContains(refuseSwitched, L"当前分支已经换掉"));

  // 上游在这期间被设好：值相同就无事可做（一条命令都不发），值不同绝不覆盖。
  PushPreflightFacts alreadySame = StillNoUpstreamFacts();
  alreadySame.upstreamConfigured = true;
  alreadySame.upstreamRemote = L"origin";
  alreadySame.upstreamRemoteRef = L"refs/heads/main";
  const std::wstring sameText =
      gc::git::DescribeUpstreamWriteStaleness(alreadySame, steps, L"refs/heads/main");
  GC_CHECK(!sameText.empty());
  GC_CHECK(TextContains(sameText, L"逐条相同"));
  GC_CHECK(TextContains(sameText, L"没有发出任何命令"));

  PushPreflightFacts alreadyOther = StillNoUpstreamFacts();
  alreadyOther.upstreamConfigured = true;
  alreadyOther.upstreamRemote = L"fork";
  alreadyOther.upstreamRemoteRef = L"refs/heads/other";
  const std::wstring otherText =
      gc::git::DescribeUpstreamWriteStaleness(alreadyOther, steps, L"refs/heads/main");
  GC_CHECK(!otherText.empty());
  GC_CHECK(TextContains(otherText, L"不把别人的设置盖掉"));
  GC_CHECK(TextContains(otherText, L"fork"));
}
