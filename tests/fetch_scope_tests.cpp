// 「抓取范围策略」的纯逻辑测试：全部用桩回答的 `git config --null --get-regexp` 输出驱动生产逻辑，
// 不起真实 Git、不碰文件系统。覆盖验收要求的每一类影响抓取范围的配置：
//   * 全局与远端级 prune／pruneTags、标签自动跟随与 remote.<名>.tagOpt、fetch.all、
//     递归相关配置 → 命令行逐项中和，并在确认正文里逐条披露；
//   * 自定义 fetch 映射指向 refs/heads/、refs/tags/、别的远端命名空间、镜像仓库那种 refs/*
//     省略目标端的写法 → 执行前拒绝并点名具体那一条配置（不猜映射、不改配置）；
//   * 合法的单分支过滤与 ^ 开头的负 refspec → 原样放行；
//   * mirror 与多地址 → 按 Git 文档如实说明「不影响 fetch」，不因此拒绝；
//   * 记录残缺、布尔取值读不懂、远端名含点、URL 里带凭据 → 各自的处理（拒绝/归组/掩码）；
//   * 界面 fetch 按钮与 pull 第一步：同一份远端与配置事实必须产出逐字相同的参数与范围正文。
// 真实 Git 的效果（引用确实没被删、标签确实没被写）在 fetch_fixture_tests.cpp 用临时仓库验证。
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "git/fetch_plan.h"
#include "git/fetch_scope.h"
#include "git/pull_plan.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FetchPlan;
using gc::git::FetchPlanState;
using gc::git::FetchScopeDecision;
using gc::git::FetchScopeFacts;
using gc::git::FetchScopeQueries;
using gc::git::FetchScopeRecord;
using gc::git::FetchTargetFacts;
using gc::git::FetchTargetQueries;
using gc::git::GitQueryResult;
using gc::git::PullFetchPlan;
using gc::git::PullFetchPlanState;
using gc::git::PullTargetFacts;
using gc::git::PullTargetQueries;

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

// 半份输出（读管道失败/超上限）：形态上仍要能构造，用来钉住「残缺不算答案」。
GitQueryResult Truncated(std::wstring_view partialOutput) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = 0;
  result.outputComplete = false;
  result.incompleteReason = L"标准输出超过字节上限，已截断";
  result.utf16Output = std::wstring(partialOutput);
  return result;
}

GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
  return result;
}

// `git config --null --get-regexp` 的真实记录形态（本机 Git 2.53 逐字节实测）：
// `键<换行>值<NUL>`；布尔键省略取值时只有 `键<NUL>`。
void AppendRecord(std::wstring& dump, std::wstring_view key, std::wstring_view value) {
  dump += key;
  dump.push_back(L'\n');
  dump += value;
  dump.push_back(L'\0');
}

void AppendValueless(std::wstring& dump, std::wstring_view key) {
  dump += key;
  dump.push_back(L'\0');
}

bool Contains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
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

// 一份「仓库里没有任何影响抓取范围的配置」的桩回答（--get-regexp 无匹配 = 退出码 1 + 空输出）。
FetchScopeQueries EmptyScopeQueries() {
  FetchScopeQueries queries;
  queries.remoteConfig = Answer(1, L"");
  queries.globalConfig = Answer(1, L"");
  return queries;
}

FetchScopeFacts EmptyScopeFacts() { return gc::git::InterpretFetchScope(EmptyScopeQueries()); }

// 一份「标准 clone 形态」的远端级配置：默认那条映射 + 一个本地路径 URL。
FetchScopeQueries StandardCloneScopeQueries(std::wstring_view remoteName = L"origin") {
  FetchScopeQueries queries;
  std::wstring remote;
  AppendRecord(remote, L"remote." + std::wstring(remoteName) + L".url", L"D:\\\\bare\\\\origin.git");
  AppendRecord(remote, L"remote." + std::wstring(remoteName) + L".fetch",
               L"+refs/heads/*:refs/remotes/" + std::wstring(remoteName) + L"/*");
  queries.remoteConfig = Answer(0, remote);
  queries.globalConfig = Answer(1, L"");
  return queries;
}

FetchScopeDecision DecideWith(const FetchScopeQueries& queries, std::wstring_view remoteName) {
  return gc::git::DecideFetchScope(gc::git::InterpretFetchScope(queries), remoteName);
}

// 把 FetchTargetFacts 拼成「分支 main 配了 origin、清单里有 origin」那一形态（fetch 入口用）。
FetchTargetFacts FetchFactsWith(const FetchScopeQueries& scopeQueries) {
  FetchTargetQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.branchRemoteRan = true;
  queries.branchRemote = Answer(0, L"origin\n");
  queries.remotes = Answer(0, L"origin\tD:\\\\bare\\\\origin.git (fetch)\n");
  queries.scope = scopeQueries;
  return gc::git::InterpretFetchTarget(queries);
}

// 把 PullTargetFacts 拼成「在 main 上、有完整上游、现状干净」那一形态（pull 第一步用）。
PullTargetFacts PullFactsWith(const FetchScopeQueries& scopeQueries) {
  PullTargetQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.headObject = Answer(0, L"1111111111111111111111111111111111111111\n");
  queries.upstreamRan = true;
  queries.upstream = Answer(0, L"origin\trefs/remotes/origin/main\trefs/heads/main\n");
  queries.trackingObjectRan = true;
  queries.trackingObject = Answer(0, L"2222222222222222222222222222222222222222\n");
  queries.configPullRebase = Answer(1, L"");
  queries.configBranchRebaseRan = true;
  queries.configBranchRebase = Answer(1, L"");
  queries.configPullFf = Answer(1, L"");
  queries.configMergeFf = Answer(1, L"");
  queries.statusRan = true;
  queries.status = Answer(0, L"");  // porcelain v2 的「没有任何变化」就是空输出
  queries.workflowProbed = true;
  queries.scope = scopeQueries;
  return gc::git::InterpretPullTarget(queries);
}

}  // namespace

// ---- 查询参数形态 ----

GC_TEST(fetch_scope_queries_bind_repository_and_stay_read_only) {
  const auto remote = gc::git::BuildFetchScopeRemoteConfigArguments(kRoot);
  GC_REQUIRE_MESSAGE(remote.size() >= 6, "远端级配置查询应有六条参数，实际 " +
                                             std::to_string(remote.size()));
  GC_CHECK(HasArgument(remote, L"-C"));
  GC_CHECK(HasArgument(remote, kRoot));
  GC_CHECK(HasArgument(remote, L"--no-optional-locks"));
  GC_CHECK(HasArgument(remote, L"config"));
  GC_CHECK(HasArgument(remote, L"--null"));
  GC_CHECK(HasArgument(remote, L"--get-regexp"));
  // 远端级：五类会影响抓取范围的键一次问回（fetch 映射是总闸门，prune/pruneTags 会删引用，
  // tagOpt 决定标签，url 与 mirror 用于披露）。
  const auto global = gc::git::BuildFetchScopeGlobalConfigArguments(kRoot);
  GC_CHECK(HasArgument(global, L"--get-regexp"));
  for (std::wstring_view argument : remote) {
    if (Contains(argument, L"remote")) {
      GC_CHECK(Contains(argument, L"fetch"));
      GC_CHECK(Contains(argument, L"prune"));
      GC_CHECK(Contains(argument, L"prunetags"));
      GC_CHECK(Contains(argument, L"tagopt"));
      GC_CHECK(Contains(argument, L"mirror"));
      GC_CHECK(Contains(argument, L"url"));
    }
  }
  for (std::wstring_view argument : global) {
    if (Contains(argument, L"fetch")) {
      GC_CHECK(Contains(argument, L"prune"));
      GC_CHECK(Contains(argument, L"all"));
      GC_CHECK(Contains(argument, L"recursesubmodules"));
    }
  }
  // 这两条都只读配置，不会带上任何写入或联网形态的参数。
  GC_CHECK(!HasArgument(remote, L"--global"));
  GC_CHECK(!HasArgument(remote, L"--file"));
}

// ---- 记录判读 ----

GC_TEST(fetch_scope_reads_records_case_and_valueless_forms) {
  FetchScopeQueries queries;
  std::wstring remote;
  // 远端名保留大小写（Git 的 subsection 区分大小写），变量名被 Git 小写化。
  AppendRecord(remote, L"remote.MyR.url", L"https://user:secret@host/path.git");
  AppendRecord(remote, L"remote.MyR.fetch", L"+refs/heads/*:refs/remotes/MyR/*");
  AppendValueless(remote, L"remote.MyR.prune");  // 省略取值 = 布尔语法里的真
  // Git 实际打印时把变量名小写化（remote.MyR.tagopt）；这里故意给一条混合大小写的记录，
  // 核对判读对键的大小写是容忍的，而远端名一律按原样比对。
  AppendRecord(remote, L"remote.MyR.tagOpt", L"--tags");
  AppendRecord(remote, L"remote.my.remote.fetch", L"+refs/heads/*:refs/remotes/my.remote/*");
  queries.remoteConfig = Answer(0, remote);
  std::wstring global;
  AppendRecord(global, L"fetch.prune", L"true");
  AppendRecord(global, L"submodule.recurse", L"true");
  queries.globalConfig = Answer(0, global);

  const FetchScopeFacts facts = gc::git::InterpretFetchScope(queries);
  GC_CHECK_MESSAGE(facts.readOk, Narrow(facts.readFailure));
  // 含点的远端名按「第一个点与最后一个点」归组：不能把它读成远端「my」的「remote.fetch」。
  GC_REQUIRE_MESSAGE(facts.remoteNames.size() == 2, "应认出两个远端");
  GC_CHECK(facts.remoteNames[0] == L"MyR");
  GC_CHECK(facts.remoteNames[1] == L"my.remote");
  GC_REQUIRE_MESSAGE(facts.remotes.size() == 2, "远端事实与名字必须一一对应");
  GC_CHECK(facts.remotes[0].urls.size() == 1);
  GC_CHECK(facts.remotes[0].fetchRefspecs.size() == 1);
  GC_REQUIRE_MESSAGE(facts.remotes[0].prune.size() == 1, "省略取值的 prune 也要留下");
  GC_CHECK(!facts.remotes[0].prune[0].hasValue);
  GC_CHECK(Contains(facts.remotes[0].tagOpt[0].value, L"--tags"));
  GC_CHECK(facts.remotes[1].fetchRefspecs.front() == L"+refs/heads/*:refs/remotes/my.remote/*");
  GC_CHECK(facts.globalPrune.size() == 1);
  GC_CHECK(facts.submoduleRecurse.size() == 1);
}

GC_TEST(fetch_scope_refuses_incomplete_or_unparseable_records) {
  // 启动失败：这份事实没读回来，不能按「没有配置」放行。
  FetchScopeQueries broken = StandardCloneScopeQueries();
  broken.remoteConfig = LaunchFailed();
  const FetchScopeDecision launch = DecideWith(broken, L"origin");
  GC_CHECK(!launch.allowed);
  GC_CHECK(launch.arguments.empty());

  // 缺结尾 NUL：半条映射看起来也像一条映射。
  broken = StandardCloneScopeQueries();
  std::wstring half = L"remote.origin.fetch\n+refs/heads/*:refs/remotes/origin/*\0";
  half += L"remote.origin.prune\ntru";
  broken.remoteConfig = Answer(0, half);
  const FetchScopeDecision partial = DecideWith(broken, L"origin");
  GC_CHECK(!partial.allowed);
  GC_CHECK(Contains(partial.refusal, L"记录约定"));

  // 输出被截断（字节上限）：读取层已经标了不完整，判读必须先拒绝。
  broken = StandardCloneScopeQueries();
  broken.remoteConfig = Truncated(L"remote.origin.fetch\n+refs/he");
  GC_CHECK(!DecideWith(broken, L"origin").allowed);

  // 认不出键名的记录：不猜它属于哪个远端。
  broken = StandardCloneScopeQueries();
  std::wstring weird = L"remote\n";
  AppendRecord(weird, L"justakey", L"value");
  broken.remoteConfig = Answer(0, weird);
  const FetchScopeDecision weirdDecision = DecideWith(broken, L"origin");
  GC_CHECK(!weirdDecision.allowed);
  GC_CHECK(Contains(weirdDecision.refusal, L"remote.<远端>.<变量>"));
}

// ---- 参数形态：两个入口唯一的命令行 ----

GC_TEST(fetch_scope_arguments_neutralize_every_side_effect) {
  FetchScopeQueries queries = StandardCloneScopeQueries();
  std::wstring remote;
  AppendRecord(remote, L"remote.origin.url", L"D:\\\\bare\\\\origin.git");
  AppendRecord(remote, L"remote.origin.fetch", L"+refs/heads/*:refs/remotes/origin/*");
  AppendRecord(remote, L"remote.origin.prune", L"true");
  AppendRecord(remote, L"remote.origin.pruneTags", L"true");
  AppendRecord(remote, L"remote.origin.tagOpt", L"--tags");
  queries.remoteConfig = Answer(0, remote);
  std::wstring global;
  AppendRecord(global, L"fetch.prune", L"true");
  AppendRecord(global, L"fetch.pruneTags", L"true");
  AppendRecord(global, L"fetch.all", L"true");
  AppendRecord(global, L"fetch.recurseSubmodules", L"on-demand");
  queries.globalConfig = Answer(0, global);

  const FetchScopeDecision decision = DecideWith(queries, L"origin");
  GC_REQUIRE_MESSAGE(decision.allowed, Narrow(decision.refusal));
  GC_CHECK(decision.arguments ==
           (std::vector<std::wstring>{L"fetch", L"--recurse-submodules=no", L"--no-prune",
                                       L"--no-prune-tags", L"--no-tags", L"origin"}));
  GC_CHECK(decision.commandLabel ==
           L"git fetch --recurse-submodules=no --no-prune --no-prune-tags --no-tags origin");
  // 绝不带上任何扩大范围的形态。
  GC_CHECK(!HasArgument(decision.arguments, L"--all"));
  GC_CHECK(!HasArgument(decision.arguments, L"--multiple"));
  GC_CHECK(!HasArgument(decision.arguments, L"--prune"));
  GC_CHECK(!HasArgument(decision.arguments, L"-p"));
  GC_CHECK(!HasArgument(decision.arguments, L"--tags"));
  GC_CHECK(!HasArgument(decision.arguments, L"-P"));
  GC_CHECK(!HasArgument(decision.arguments, L"--recurse-submodules=on-demand"));
  // 被中和的配置必须逐条说给用户看，而不是悄悄中和。
  GC_CHECK(Contains(decision.scopeParagraph, L"fetch.prune"));
  GC_CHECK(Contains(decision.scopeParagraph, L"--no-prune"));
  GC_CHECK(Contains(decision.scopeParagraph, L"--no-prune-tags"));
  GC_CHECK(Contains(decision.scopeParagraph, L"--no-tags"));
  GC_CHECK(Contains(decision.scopeParagraph, L"tagOpt"));
  GC_CHECK(Contains(decision.scopeParagraph, L"fetch.all"));
  GC_CHECK(Contains(decision.scopeParagraph, L"recurseSubmodules"));
  // 「工作区/索引不变」与「FETCH_HEAD、对象库、跟踪引用会变」必须分开说，不许写「磁盘只读」。
  GC_CHECK(Contains(decision.scopeParagraph, L".git/FETCH_HEAD"));
  GC_CHECK(Contains(decision.scopeParagraph, L"对象库"));
  GC_CHECK(Contains(decision.scopeParagraph, L"不说「磁盘完全只读」"));
  GC_CHECK(Contains(decision.scopeParagraph, L"确实会被写"));
  GC_CHECK(Contains(decision.noticeCore, L"refs/remotes/origin/"));
  GC_CHECK(Contains(decision.noticeCore, L"FETCH_HEAD"));
}

// ---- 无法安全表达的映射：执行前拒绝 ----

GC_TEST(fetch_scope_refuses_mappings_outside_allowed_namespace) {
  // 逐条单独核对（每条都是「只有这一句映射」的仓库）：这些映射都落在承诺之外，
  // 而且命令行参数绕不过去，只能拒绝——不猜一个「更安全」的映射，也不改用户配置。
  const std::pair<std::wstring, std::wstring_view> specs[] = {
      {L"+refs/heads/*:refs/heads/*", L"会把引用写进别的本地命名空间"},
      {L"+refs/heads/*:refs/tags/*", L"会把引用写进别的本地命名空间"},
      {L"+refs/heads/*:refs/remotes/other/*", L"会把引用写进别的本地命名空间"},
      {L"+refs/*:refs/*", L"会把引用写进别的本地命名空间"},
      {L"refs/heads/main", L"无法确定它会把引用写到哪儿"},
      {L"refs/heads/*:", L"当成无效 refspec"},
  };
  for (const auto& entry : specs) {
    FetchScopeQueries queries;
    std::wstring remote;
    AppendRecord(remote, L"remote.origin.url", L"D:\\\\bare\\\\origin.git");
    AppendRecord(remote, L"remote.origin.fetch", entry.first);
    queries.remoteConfig = Answer(0, remote);
    queries.globalConfig = Answer(1, L"");
    const FetchScopeDecision decision = DecideWith(queries, L"origin");
    GC_CHECK_MESSAGE(!decision.allowed, "映射「" + Narrow(entry.first) + "」必须拒绝");
    GC_CHECK(decision.arguments.empty());
    // 拒绝必须点名那一条配置，并说明「不改你的配置、不替你猜映射」。
    GC_CHECK_MESSAGE(Contains(decision.refusal, entry.second),
                     Narrow(entry.first) + " 的拒绝原因没说到点上：" + Narrow(decision.refusal));
    GC_CHECK(Contains(decision.refusal, L"remote.origin.fetch"));
    GC_CHECK(Contains(decision.refusal, L"不改写你的"));
  }
}

GC_TEST(fetch_scope_allows_legitimate_filters_and_negative_refspecs) {
  FetchScopeQueries queries;
  std::wstring remote;
  AppendRecord(remote, L"remote.origin.url", L"D:\\\\bare\\\\origin.git");
  // 单分支过滤：合法的分支过滤与上游映射必须原样放行，不能误拒成「超出范围」。
  AppendRecord(remote, L"remote.origin.fetch", L"+refs/heads/main:refs/remotes/origin/main");
  AppendRecord(remote, L"remote.origin.fetch", L"+refs/heads/release:refs/remotes/origin/release");
  // 负 refspec：只排除、不含目标端（Git 文档），作用是缩小范围。
  AppendRecord(remote, L"remote.origin.fetch", L"^refs/heads/secret");
  AppendRecord(remote, L"remote.origin.fetch", L"^refs/heads/wip/*");
  queries.remoteConfig = Answer(0, remote);
  queries.globalConfig = Answer(1, L"");

  const FetchScopeDecision decision = DecideWith(queries, L"origin");
  GC_REQUIRE_MESSAGE(decision.allowed, Narrow(decision.refusal));
  GC_CHECK(Contains(decision.scopeParagraph, L"refs/heads/main:refs/remotes/origin/main"));
  GC_CHECK(Contains(decision.scopeParagraph, L"负 refspec"));
  // 排除项不算「正向映射」，不能把它当成会写引用的条目列进核对清单。
  GC_CHECK(!Contains(decision.scopeParagraph, L"^refs/heads/secret\n"));
}

GC_TEST(fetch_scope_default_mapping_and_no_records_still_allowed) {
  // 配置里连这个远端的一条记录都没有：按 Git 文档的默认映射判定，并如实这么说。
  const FetchScopeDecision decision = DecideWith(EmptyScopeQueries(), L"origin");
  GC_REQUIRE_MESSAGE(decision.allowed, Narrow(decision.refusal));
  GC_CHECK(Contains(decision.scopeParagraph, L"没有配置 fetch 映射"));
  GC_CHECK(Contains(decision.scopeParagraph, L"refs/heads/*:refs/remotes/origin/*"));

  // 目标名字还没定下来（用户在选远端时取消了、或上游问不到）：不产出任何命令。
  const FetchScopeDecision noTarget = DecideWith(EmptyScopeQueries(), L"");
  GC_CHECK(!noTarget.allowed);
  GC_CHECK(noTarget.arguments.empty());
}

// ---- 只作披露，不构成扩大 ----

GC_TEST(fetch_scope_discloses_mirror_multi_url_and_hides_credentials) {
  FetchScopeQueries queries;
  std::wstring remote;
  AppendRecord(remote, L"remote.origin.url", L"https://user:token@host/one.git");
  AppendRecord(remote, L"remote.origin.url", L"https://host/two.git");
  AppendRecord(remote, L"remote.origin.mirror", L"true");
  AppendRecord(remote, L"remote.origin.fetch", L"+refs/heads/*:refs/remotes/origin/*");
  queries.remoteConfig = Answer(0, remote);
  queries.globalConfig = Answer(1, L"");

  const FetchScopeDecision decision = DecideWith(queries, L"origin");
  GC_REQUIRE_MESSAGE(decision.allowed, Narrow(decision.refusal));
  // mirror=true 按 Git 文档只影响 push：说明清楚，但不因此拒绝一次合法抓取。
  GC_CHECK(Contains(decision.scopeParagraph, L"mirror"));
  GC_CHECK(Contains(decision.scopeParagraph, L"只影响 push"));
  // 多地址：文档写明 fetch 只用第一个，也要说给用户。
  GC_CHECK(Contains(decision.scopeParagraph, L"2 个抓取地址"));
  GC_CHECK(Contains(decision.scopeParagraph, L"第一个"));
  // 任何展示出来的配置原文都必须先过凭据掩码。
  GC_CHECK(!Contains(decision.scopeParagraph, L"token"));
  GC_CHECK(!Contains(decision.refusal, L"token"));
}

GC_TEST(fetch_scope_refuses_bool_values_it_cannot_interpret) {
  const std::wstring_view keys[] = {L"fetch.prune", L"remote.origin.prune", L"remote.origin.pruneTags",
                                    L"fetch.pruneTags", L"fetch.all"};
  for (std::wstring_view key : keys) {
    FetchScopeQueries queries = StandardCloneScopeQueries();
    const bool remoteScoped = Contains(key, L"remote.");
    std::wstring& dump = remoteScoped ? queries.remoteConfig.utf16Output : queries.globalConfig.utf16Output;
    AppendRecord(dump, key, L"maybe");
    // 记录里有了内容，Git 的 --get-regexp 就不再是「无匹配（退出码 1）」而是 0。
    (remoteScoped ? queries.remoteConfig : queries.globalConfig).exitCode = 0;
    const FetchScopeDecision decision = DecideWith(queries, L"origin");
    GC_CHECK_MESSAGE(!decision.allowed, "布尔取值读不懂时必须拒绝：" + Narrow(key));
    GC_CHECK_MESSAGE(Contains(decision.refusal, L"布尔"),
                     Narrow(key) + " 的拒绝原因没说清读不懂的是布尔取值：" + Narrow(decision.refusal));
  }
  // tagOpt 只认 Git 文档给出的那几种写法。
  FetchScopeQueries tagGarbage = StandardCloneScopeQueries();
  AppendRecord(tagGarbage.remoteConfig.utf16Output, L"remote.origin.tagOpt", L"--everything");
  const FetchScopeDecision tagDecision = DecideWith(tagGarbage, L"origin");
  GC_CHECK(!tagDecision.allowed);
  GC_CHECK(Contains(tagDecision.refusal, L"tagOpt"));
  // --no-tags 这种「本来就在承诺里」的写法照旧放行。
  FetchScopeQueries tagNoTags = StandardCloneScopeQueries();
  AppendRecord(tagNoTags.remoteConfig.utf16Output, L"remote.origin.tagOpt", L"--no-tags");
  const FetchScopeDecision noTags = DecideWith(tagNoTags, L"origin");
  GC_CHECK_MESSAGE(noTags.allowed, Narrow(noTags.refusal));
}

// ---- 两个入口复用同一份策略 ----

GC_TEST(fetch_and_pull_share_one_fetch_scope_policy) {
  const FetchScopeQueries queries = StandardCloneScopeQueries();

  const FetchTargetFacts fetchFacts = FetchFactsWith(queries);
  GC_REQUIRE_MESSAGE(fetchFacts.queryOk, Narrow(fetchFacts.queryFailure));
  const FetchPlan fetchPlan = gc::git::BuildFetchPlan(fetchFacts, kRoot);
  GC_REQUIRE_MESSAGE(fetchPlan.state == FetchPlanState::ready, Narrow(fetchPlan.explanation));

  const PullTargetFacts pullFacts = PullFactsWith(queries);
  GC_REQUIRE_MESSAGE(pullFacts.queryOk, Narrow(pullFacts.queryFailure));
  const PullFetchPlan pullPlan = gc::git::BuildPullFetchPlan(pullFacts, kRoot);
  GC_REQUIRE_MESSAGE(pullPlan.state == PullFetchPlanState::ready, Narrow(pullPlan.explanation));

  // 参数与命令展示：逐字相同——「两个入口复用同一范围策略」唯一的可核对形态。
  GC_CHECK(fetchPlan.arguments == pullPlan.arguments);
  GC_CHECK(fetchPlan.commandLabel == pullPlan.commandLabel);
  // 用户点头前看到的那段范围承诺也必须同出一处（两个入口各自的可变内容在别的地方）。
  const FetchScopeDecision shared = DecideWith(queries, L"origin");
  GC_REQUIRE_MESSAGE(shared.allowed, Narrow(shared.refusal));
  GC_CHECK(Contains(fetchPlan.confirmationText, shared.scopeParagraph));
  GC_CHECK(Contains(pullPlan.confirmationText, shared.scopeParagraph));
  GC_CHECK(Contains(fetchPlan.notice, shared.noticeCore));
  GC_CHECK(Contains(pullPlan.notice, shared.noticeCore));
}

GC_TEST(fetch_and_pull_both_refuse_an_unsafe_mapping) {
  FetchScopeQueries queries;
  std::wstring remote;
  AppendRecord(remote, L"remote.origin.url", L"D:\\\\bare\\\\origin.git");
  AppendRecord(remote, L"remote.origin.fetch", L"+refs/heads/*:refs/heads/*");
  queries.remoteConfig = Answer(0, remote);
  queries.globalConfig = Answer(1, L"");

  const FetchPlan fetchPlan = gc::git::BuildFetchPlan(FetchFactsWith(queries), kRoot);
  GC_CHECK(fetchPlan.state == FetchPlanState::blocked);
  GC_CHECK(fetchPlan.arguments.empty());

  const PullFetchPlan pullPlan = gc::git::BuildPullFetchPlan(PullFactsWith(queries), kRoot);
  GC_CHECK(pullPlan.state == PullFetchPlanState::blocked);
  GC_CHECK(pullPlan.arguments.empty());
  // 两边给出的原因必须是同一份策略的那一句（不是各自临时编的措辞）。
  GC_CHECK(Contains(fetchPlan.explanation, L"refs/remotes/origin/"));
  GC_CHECK(Contains(pullPlan.explanation, L"refs/remotes/origin/"));
  GC_CHECK(Contains(pullPlan.explanation, L"没有发出任何命令"));
}

GC_TEST(fetch_scope_choice_path_uses_the_same_policy) {
  // 分支没配远端 → 用户在选择界面点了 origin：选完也要经过同一道范围核对。
  FetchTargetQueries queries;
  queries.symbolicRef = Answer(1, L"");  // 游离 HEAD：没有分支配置可参考
  queries.remotes = Answer(0, L"origin\tD:\\\\bare\\\\origin.git (fetch)\n");
  std::wstring remote;
  AppendRecord(remote, L"remote.origin.fetch", L"+refs/heads/*:refs/heads/*");
  queries.scope.remoteConfig = Answer(0, remote);
  queries.scope.globalConfig = Answer(1, L"");
  const FetchTargetFacts facts = gc::git::InterpretFetchTarget(queries);
  GC_REQUIRE_MESSAGE(facts.queryOk, Narrow(facts.queryFailure));
  const FetchPlan before = gc::git::BuildFetchPlan(facts, kRoot);
  GC_CHECK(before.state == FetchPlanState::chooseRemote);
  const FetchPlan chosen = gc::git::ChooseFetchRemote(facts, L"origin", kRoot);
  GC_CHECK_MESSAGE(chosen.state == FetchPlanState::blocked, Narrow(chosen.explanation));
  GC_CHECK(chosen.arguments.empty());

  // 同一份清单配上合格的映射：选完就是 ready，参数与 fetch_scope 给的一字不差。
  std::wstring good;
  AppendRecord(good, L"remote.origin.fetch", L"+refs/heads/*:refs/remotes/origin/*");
  queries.scope.remoteConfig = Answer(0, good);
  const FetchTargetFacts okFacts = gc::git::InterpretFetchTarget(queries);
  const FetchPlan okPlan = gc::git::ChooseFetchRemote(okFacts, L"origin", kRoot);
  GC_REQUIRE_MESSAGE(okPlan.state == FetchPlanState::ready, Narrow(okPlan.explanation));
  const FetchScopeDecision expected = DecideFetchScope(okFacts.scope, L"origin");
  GC_REQUIRE_MESSAGE(expected.allowed, Narrow(expected.refusal));
  GC_CHECK(okPlan.arguments == expected.arguments);
  GC_CHECK(okPlan.commandLabel == expected.commandLabel);
}
