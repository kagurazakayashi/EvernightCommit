// 「push」判读与方案的纯逻辑测试：全部用桩化的 GitQueryResult 与手工搭出的事实驱动生产逻辑，
// 不起真实 Git、不碰文件系统、不接触网络。覆盖：
//   * 各条只读查询与那条推送命令的参数形态（-C 绑定、--no-optional-locks、--null 清单、
//     `remote get-url --push`、显式两侧 refspec、`--` 隔开的 ls-remote 核对）；
//   * 命令形态的范围承诺：绝不含 --force / --force-with-lease / --mirror / --all / --tags /
//     --no-verify；--recurse-submodules=no 写死；mirror / tagOpt 只在仓库真有时才补 -c 中和；
//   * `git config --list --null` 的判读：单值取最后一条、多值按顺序全收、远端清单去重、
//     「没设」与「设成空值」分得开，遇到不合约定的记录整份拒用；
//   * 发布远端的优先序：branch.<名>.pushRemote > remote.pushDefault > branch.<名>.remote，
//     并且据此决定命令里写哪个远端名字；
//   * 前提拒绝：无上游、游离 HEAD、尚无提交、上游没给出远端引用、目标远端不存在、
//     问不出发布 URL——一律不产生命令，也不猜 origin、不代设 upstream、不写配置；
//   * 风险清单：非快进、发布目标与抓取目标不同处、多个 push URL、远端还没有这条分支、
//     本地与已知远端位置已一致，都要列出来并要求明确点头；
//   * 执行前复核 DescribePushChange：分支 / 要推的那一份提交 / 上游指向 / 发布远端 /
//     发布 URL / 会被中和的设置任一变化都拒绝执行；工作区的改动不在比对范围之内；
//   * 推送后的核实：逐目标的 ls-remote 判读与四种结论措辞（命令报成功却没核实上不得写成成功）；
//   * 展示与日志里不出现凭据：内嵌 userinfo 的 URL 一律掩码。
// 真实 Git 的临时 bare 远端链路在 push_fixture_tests.cpp 验证。
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "git/push_plan.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

using gc::git::GitQueryResult;
using gc::git::PushConfigListing;
using gc::git::PushPlan;
using gc::git::PushPlanState;
using gc::git::PushPreflightFacts;
using gc::git::PushPreflightQueries;
using gc::git::PushRemoteChoice;
using gc::git::PushTargetCheck;
using gc::git::PushVerificationReport;
using gc::git::PushVerificationVerdict;

constexpr std::wstring_view kRoot = L"D:\\仓 库";
constexpr std::wstring_view kHead = L"1111111111111111111111111111111111111111";
constexpr std::wstring_view kRemoteCommit = L"2222222222222222222222222222222222222222";
constexpr std::wstring_view kUrl = L"D:\\bare\\origin.git";

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

GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
  return result;
}

// `git config --list --null` 的原始输出：每条 `key=value` 以 NUL 结尾。
// 必须整个按 std::wstring 交进去（wstring_view 与从 wchar_t* 构造的 wstring 都会在第一个 NUL 截断）。
std::wstring NulJoined(std::initializer_list<std::wstring_view> records) {
  std::wstring joined;
  for (const std::wstring_view record : records) {
    joined += record;
    joined.push_back(L'\0');
  }
  return joined;
}

std::wstring ConfigText(std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> pairs) {
  std::wstring joined;
  for (const auto& pair : pairs) {
    joined += pair.first;
    joined += L'=';
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

// 一份「在 main 上、有 HEAD、上游是 origin/main、跟踪引用可解析、origin 只有一个 url」的桩查询组。
// 领先 2 个、落后 0 个；不带任何会引起额外范围的设置。
PushPreflightQueries HealthyPushQueries() {
  PushPreflightQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.headObject = Answer(0, std::wstring(kHead) + L"\n");
  queries.upstreamRan = true;
  queries.upstream = Answer(0, L"origin\trefs/remotes/origin/main\trefs/heads/main\n");
  queries.trackingRan = true;
  queries.trackingObject = Answer(0, std::wstring(kRemoteCommit) + L"\n");
  queries.configListing = ConfigListingAnswer(
      {{L"core.repositoryformatversion", L"0"},
       {L"remote.origin.url", kUrl},
       {L"remote.origin.fetch", L"+refs/heads/*:refs/remotes/origin/*"},
       {L"branch.main.remote", L"origin"},
       {L"branch.main.merge", L"refs/heads/main"}});
  queries.remoteUrlRan = true;
  queries.remoteUrl = Answer(0, std::wstring(kUrl) + L"\n");
  queries.aheadBehindRan = true;
  queries.aheadBehind = Answer(0, L"2\t0\n");
  return queries;
}

PushPreflightFacts HealthyFacts() { return gc::git::InterpretPushPreflight(HealthyPushQueries()); }

PushPlan PlanFor(const PushPreflightFacts& facts) { return gc::git::BuildPushPlan(facts, kRoot); }

const std::wstring* FindRisk(const PushPlan& plan, std::wstring_view needle) {
  for (const std::wstring& risk : plan.risks) {
    if (TextContains(risk, needle)) {
      return &risk;
    }
  }
  return nullptr;
}

}  // namespace

// ---- 查询与命令的参数形态 ----

GC_TEST(push_query_arguments_bind_repository_and_stay_read_only) {
  const std::vector<std::wstring> listing = gc::git::BuildPushConfigListingArguments(kRoot);
  GC_CHECK(HasArgument(listing, L"-C"));
  GC_CHECK(HasArgument(listing, kRoot));
  GC_CHECK(HasArgument(listing, L"--no-optional-locks"));
  GC_CHECK(HasArgument(listing, L"--null"));
  GC_CHECK(listing.front() == L"-C" && listing[1] == kRoot);  // -C 必须紧跟仓库目录

  const std::vector<std::wstring> url = gc::git::BuildPushRemoteUrlArguments(kRoot, L"origin");
  GC_CHECK(HasArgument(url, L"remote"));
  GC_CHECK(HasArgument(url, L"get-url"));
  GC_CHECK(HasArgument(url, L"--push"));
  GC_CHECK(HasArgument(url, L"origin"));
  // 名字定不下来时一条查询也不发（空数组就是「没问」的记号）。
  GC_CHECK(gc::git::BuildPushRemoteUrlArguments(kRoot, L"").empty());
  GC_CHECK(gc::git::BuildPushRemoteUrlArguments(kRoot, L"--help").empty());
}

GC_TEST(push_probe_arguments_put_url_after_double_dash) {
  const std::vector<std::wstring> probe =
      gc::git::BuildPushRemoteProbeArguments(kRoot, kUrl, L"refs/heads/main");
  GC_CHECK(HasArgument(probe, L"ls-remote"));
  GC_CHECK(HasArgument(probe, L"--"));
  GC_CHECK(HasArgument(probe, L"refs/heads/main"));
  // `--` 必须排在 URL 之前：以 `-` 开头的地址才会被当成位置参数而不是选项。
  const auto dash = std::find(probe.begin(), probe.end(), L"--");
  const auto urlItem = std::find(probe.begin(), probe.end(), std::wstring(kUrl));
  GC_CHECK(dash < urlItem);
  GC_CHECK(gc::git::BuildPushRemoteProbeArguments(kRoot, L"", L"refs/heads/main").empty());
  GC_CHECK(gc::git::BuildPushRemoteProbeArguments(kRoot, kUrl, L"main").empty());
  GC_CHECK(gc::git::BuildPushRemoteProbeArguments(kRoot, kUrl, L"refs/heads/a:b").empty());
}

GC_TEST(push_command_is_a_single_scoped_refspec_with_no_escalation) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main", L"refs/heads/main", false, false);
  GC_CHECK(HasArgument(arguments, L"push"));
  GC_CHECK(HasArgument(arguments, L"--recurse-submodules=no"));
  GC_CHECK(HasArgument(arguments, L"origin"));
  GC_CHECK(HasArgument(arguments, L"refs/heads/main:refs/heads/main"));
  // 范围承诺：这些一个都不许出现（撤回/整合之后出现的 non-fast-forward 也只能由 Git 拒绝）。
  for (std::wstring_view forbidden :
       {L"--force", L"--force-with-lease", L"--mirror", L"--all", L"--tags", L"--follow-tags",
        L"--no-verify", L"-f", L"--atomic", L"--delete", L"--prune"}) {
    GC_CHECK_MESSAGE(!AnyArgumentContains(arguments, forbidden),
                     "命令里出现了不该有的参数：" + Narrow(forbidden));
  }
  // 只推这一条：参数里除它自己没有别的引用，也没有 `--` 之外的通配写法。
  GC_CHECK_MESSAGE(!AnyArgumentContains(arguments, L"*"), "refspec 不允许出现通配");
}

GC_TEST(push_command_neutralizes_only_the_configs_that_exist) {
  const std::vector<std::wstring> plain =
      gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main", L"refs/heads/main", false, false);
  GC_CHECK(!AnyArgumentContains(plain, L"mirror"));
  GC_CHECK(!AnyArgumentContains(plain, L"tagopt"));
  // push.followTags 恒关：显式 refspec 之下实测它不会带标签，但仍为这一个子进程钉死。
  GC_CHECK(HasArgument(plain, L"push.followTags=false"));

  const std::vector<std::wstring> guarded =
      gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main", L"refs/heads/main", true, true);
  GC_CHECK(HasArgument(guarded, L"remote.origin.mirror=false"));
  GC_CHECK(HasArgument(guarded, L"remote.origin.tagopt="));
  // -c 是 git 的全局选项：必须排在 push 子命令之前，否则整条命令会被 Git 判为用法错误。
  const auto pushItem = std::find(guarded.begin(), guarded.end(), L"push");
  GC_CHECK(pushItem != guarded.end());
  for (std::wstring_view key : {std::wstring_view(L"remote.origin.mirror=false"),
                                std::wstring_view(L"remote.origin.tagopt="),
                                std::wstring_view(L"push.followTags=false")}) {
    const auto found = std::find(guarded.begin(), guarded.end(), std::wstring(key));
    GC_CHECK_MESSAGE(found < pushItem, "-c 的取值必须排在 push 之前");
    GC_CHECK(found != guarded.begin() && *(found - 1) == L"-c");
  }

  // 中和的是「这一个远端」的设置：换一个远端就得写另一个键名。
  const std::vector<std::wstring> other =
      gc::git::BuildPushCommandArguments(L"upstream", L"refs/heads/main", L"refs/heads/main", true, false);
  GC_CHECK(HasArgument(other, L"remote.upstream.mirror=false"));
  GC_CHECK(!AnyArgumentContains(other, L"tagopt"));
}

GC_TEST(push_command_rejects_malformed_targets_instead_of_guessing) {
  GC_CHECK(gc::git::BuildPushCommandArguments(L"", L"refs/heads/main", L"refs/heads/main", false, false)
               .empty());
  GC_CHECK(gc::git::BuildPushCommandArguments(L"origin", L"main", L"refs/heads/main", false, false)
               .empty());  // 本地侧必须是完整引用，不写缩写
  GC_CHECK(
      gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main", L"main", false, false).empty());
  GC_CHECK(gc::git::BuildPushCommandArguments(L"origin", L"HEAD", L"refs/heads/main", false, false)
               .empty());  // 更不许拿 HEAD 顶替分支
  GC_CHECK(gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/ma\"in", L"refs/heads/main",
                                             false, false)
               .empty());
  GC_CHECK(gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/m\nain", L"refs/heads/main",
                                             false, false)
               .empty());
  GC_CHECK(gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main",
                                             L"refs/heads/a:b", false, false)
               .empty());  // 远端侧含 `:` 会让 refspec 变成两段以上
}

// ---- 生效配置清单的判读 ----

GC_TEST(push_config_listing_last_value_wins_and_multi_values_keep_order) {
  const PushConfigListing config =
      gc::git::ParsePushConfigListing(ConfigListingAnswer({
          {L"remote.origin.url", L"D:\\one.git"},
          {L"remote.origin.url", L"D:\\two.git"},  // 多值 url：两条都要收
          {L"remote.origin.pushurl", L"D:\\push.git"},
          {L"branch.main.remote", L"origin"},
          {L"remote.pushdefault", L"first"},
          {L"remote.pushdefault", L"second"},  // 单值语义：后一条才是生效的那条
      }));
  GC_CHECK(config.readOk);
  GC_CHECK(config.Value(L"remote.pushdefault") == L"second");
  const std::vector<std::wstring> urls = config.RemoteValues(L"origin", L"url");
  GC_CHECK_MESSAGE(urls.size() == 2, "多值 url 必须按顺序全收");
  GC_CHECK(urls.size() == 2 && urls[0] == L"D:\\one.git" && urls[1] == L"D:\\two.git");
  GC_CHECK(config.RemoteValues(L"origin", L"pushurl").size() == 1);
  GC_CHECK(config.Values(L"branch.main.remote").size() == 1);
  // 「没设」与「设成空值」是两回事：HasKey 分得开。
  GC_CHECK(!config.HasKey(L"branch.main.pushremote"));
  GC_CHECK(config.Value(L"branch.main.pushremote").empty());
  const std::vector<std::wstring> names = config.RemoteNames();
  GC_CHECK(names.size() == 1 && names[0] == L"origin");  // 只有有 url/pushurl 的才算远端
}

GC_TEST(push_config_listing_keeps_subsection_case_and_empty_values) {
  const PushConfigListing config = gc::git::ParsePushConfigListing(ConfigListingAnswer({
      {L"branch.功能分支.remote", L"origin"},
      {L"branch.功能分支.pushremote", L""},
      {L"remote.only-push.pushurl", L"D:\\p.git"},
  }));
  GC_CHECK(config.readOk);
  // 段名与变量名被 Git 小写化，subsection（分支名/远端名）原样保留——查表必须按这个形态拼。
  GC_CHECK(config.Value(L"branch.功能分支.remote") == L"origin");
  GC_CHECK(config.HasKey(L"branch.功能分支.pushremote"));
  GC_CHECK(config.Value(L"branch.功能分支.pushremote").empty());
  const std::vector<std::wstring> names = config.RemoteNames();
  GC_CHECK(names.size() == 1 && names[0] == L"only-push");  // 只有 pushurl 的也算存在
}

GC_TEST(push_config_listing_refuses_records_it_cannot_parse) {
  GitQueryResult broken;
  broken.started = true;
  broken.exited = true;
  broken.exitCode = 0;
  broken.utf16Output = NulJoined({L"remote.origin.url", L"D:\\x.git"});  // 第一条少了 `=`
  const PushConfigListing config = gc::git::ParsePushConfigListing(broken);
  GC_CHECK(!config.readOk);
  GC_CHECK_MESSAGE(TextContains(config.readFailure, L"不含 `=`"), "拒绝原因要说清是哪一条");
  GC_CHECK(config.entries.empty());  // 半份清单不能留着给人误用

  const PushConfigListing empty = gc::git::ParsePushConfigListing(Answer(0));
  GC_CHECK_MESSAGE(empty.readOk, "一条配置也没有是明确答案，不是失败");

  const PushConfigListing failed = gc::git::ParsePushConfigListing(Answer(128, L"", L"bad config"));
  GC_CHECK(!failed.readOk);
  GC_CHECK(!failed.readFailure.empty());
}

// ---- URL 掩码与展示 ----

GC_TEST(push_url_masking_hides_credentials_but_not_paths) {
  GC_CHECK(gc::git::MaskPushUrlCredentials(L"https://user:secretp@github.com/o/r.git") ==
           L"https://***@github.com/o/r.git");
  GC_CHECK(gc::git::MaskPushUrlCredentials(L"ssh://git@github.com/o/r.git") == L"ssh://***@github.com/o/r.git");
  GC_CHECK(gc::git::MaskPushUrlCredentials(L"https://github.com/o/r.git") == L"https://github.com/o/r.git");
  GC_CHECK(gc::git::MaskPushUrlCredentials(kUrl) == kUrl);  // 本机绝对路径没有 userinfo 的位置
  GC_CHECK(gc::git::MaskPushUrlCredentials(L"git@github.com:o/r.git") == L"git@github.com:o/r.git");
  // 端口与查询段不能被当成路径分隔提前截断 authority。
  GC_CHECK(gc::git::MaskPushUrlCredentials(L"https://u:p@host:8443/r.git") == L"https://***@host:8443/r.git");
  GC_CHECK(!TextContains(gc::git::MaskPushUrlCredentials(L"https://u:p@host/r.git"), L"secretp"));

  const std::wstring single = gc::git::FormatPushUrlList(std::vector<std::wstring>{L"https://u:p@h/r"});
  GC_CHECK(single == L"https://***@h/r");
  const std::wstring many = gc::git::FormatPushUrlList(
      std::vector<std::wstring>{L"https://u:p@h/one", L"D:\\bare\\两.git"});
  GC_CHECK(TextContains(many, L"共 2 个"));
  GC_CHECK(TextContains(many, L"1) https://***@h/one"));
  GC_CHECK(TextContains(many, L"2) D:\\bare\\两.git"));
  GC_CHECK(TextContains(gc::git::FormatPushUrlList(std::vector<std::wstring>{}), L"没能问出"));
}

// ---- 发布远端的优先序 ----

GC_TEST(push_remote_resolution_follows_git_precedence) {
  const PushConfigListing both = gc::git::ParsePushConfigListing(ConfigListingAnswer({
      {L"branch.main.pushremote", L"fork"},
      {L"remote.pushdefault", L"central"},
      {L"remote.origin.url", kUrl},
      {L"remote.fork.url", L"D:\\bare\\fork.git"},
  }));
  PushRemoteChoice choice = gc::git::ResolvePushRemote(both, L"main", L"origin");
  GC_CHECK(choice.remoteName == L"fork");
  GC_CHECK(TextContains(choice.source, L"branch.main.pushRemote"));

  const PushConfigListing defaultOnly = gc::git::ParsePushConfigListing(ConfigListingAnswer({
      {L"remote.pushdefault", L"central"},
      {L"remote.origin.url", kUrl},
  }));
  choice = gc::git::ResolvePushRemote(defaultOnly, L"main", L"origin");
  GC_CHECK(choice.remoteName == L"central");
  GC_CHECK(TextContains(choice.source, L"remote.pushDefault"));

  const PushConfigListing none = gc::git::ParsePushConfigListing(ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
  }));
  choice = gc::git::ResolvePushRemote(none, L"main", L"origin");
  GC_CHECK(choice.remoteName == L"origin");  // 退到上游所在的远端——这正是 Git 自己的行为
  GC_CHECK(TextContains(choice.source, L"branch.main.remote"));

  // 设成空值不是「没设」：定不出目标就是定不出，不退回上游那个远端去凑。
  const PushConfigListing empty = gc::git::ParsePushConfigListing(ConfigListingAnswer({
      {L"branch.main.pushremote", L""},
      {L"remote.origin.url", kUrl},
  }));
  choice = gc::git::ResolvePushRemote(empty, L"main", L"origin");
  GC_CHECK(choice.remoteName.empty());
  GC_CHECK(TextContains(choice.source, L"空值"));
  GC_CHECK(gc::git::ResolvePushRemote(PushConfigListing{}, L"main", L"origin").remoteName.empty());
}

// ---- 预检判读 ----

GC_TEST(push_preflight_reads_branch_head_upstream_and_target) {
  const PushPreflightFacts facts = HealthyFacts();
  GC_CHECK(facts.queryOk);
  GC_CHECK(facts.onBranch && facts.branchName == L"main" && facts.branchRef == L"refs/heads/main");
  GC_CHECK(facts.headResolved && facts.headObjectId == kHead);
  GC_CHECK(facts.upstreamConfigured && facts.upstreamRemote == L"origin");
  GC_CHECK(facts.upstreamTrackingRef == L"refs/remotes/origin/main");
  GC_CHECK(facts.upstreamRemoteRef == L"refs/heads/main");
  GC_CHECK(facts.trackingResolved && facts.trackingObjectId == kRemoteCommit);
  GC_CHECK(facts.configOk && facts.pushRemoteName == L"origin");
  GC_CHECK(facts.pushRemoteExists);
  GC_CHECK(facts.pushUrls.size() == 1 && facts.pushUrls.front() == kUrl);
  GC_CHECK(facts.pushTargetIsFetchTarget);
  GC_CHECK(!facts.pushUrlRewritten);
  GC_CHECK(facts.relationship.known && facts.relationship.ahead == 2 && facts.relationship.behind == 0);
  GC_CHECK(!facts.mirrorConfigured && !facts.tagOptConfigured && !facts.followTagsConfigured);
}

GC_TEST(push_preflight_separates_missing_from_failed) {
  PushPreflightQueries detached = HealthyPushQueries();
  detached.symbolicRef = NoResult();  // 不在分支上：Git 用退出码 1 + 空输出明确回答
  const PushPreflightFacts detachedFacts = gc::git::InterpretPushPreflight(detached);
  GC_CHECK(detachedFacts.queryOk);
  GC_CHECK(!detachedFacts.onBranch);

  PushPreflightQueries unborn = HealthyPushQueries();
  unborn.headObject = NoResult();
  const PushPreflightFacts unbornFacts = gc::git::InterpretPushPreflight(unborn);
  GC_CHECK(unbornFacts.queryOk);
  GC_CHECK(unbornFacts.headQueried && !unbornFacts.headResolved);

  PushPreflightQueries noUpstream = HealthyPushQueries();
  noUpstream.upstream = Answer(0);  // for-each-ref 成功但那一行是空的：没有上游记录
  const PushPreflightFacts noUpstreamFacts = gc::git::InterpretPushPreflight(noUpstream);
  GC_CHECK(noUpstreamFacts.queryOk);
  GC_CHECK(noUpstreamFacts.upstreamRan && !noUpstreamFacts.upstreamConfigured);

  PushPreflightQueries failed = HealthyPushQueries();
  failed.headObject = LaunchFailed();
  const PushPreflightFacts failedFacts = gc::git::InterpretPushPreflight(failed);
  GC_CHECK(!failedFacts.queryOk);
  GC_CHECK(!failedFacts.queryFailure.empty());
}

GC_TEST(push_preflight_tracks_where_the_push_will_actually_go) {
  // 独立 push URL：发布目标不再等于 url，也不再等于抓取的那一侧。
  PushPreflightQueries pushUrl = HealthyPushQueries();
  pushUrl.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.pushurl", L"D:\\bare\\publish.git"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  pushUrl.remoteUrl = Answer(0, L"D:\\bare\\publish.git\n");
  const PushPreflightFacts pushed = gc::git::InterpretPushPreflight(pushUrl);
  GC_CHECK(pushed.pushUrls.size() == 1 && pushed.pushUrls.front() == L"D:\\bare\\publish.git");
  GC_CHECK(!pushed.pushTargetIsFetchTarget);

  // 多个 push URL：全部保留，界面要一个个列出来（实测 Git 会推给每一个）。
  PushPreflightQueries multi = HealthyPushQueries();
  multi.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.pushurl", L"D:\\bare\\one.git"},
      {L"remote.origin.pushurl", L"D:\\bare\\two.git"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  multi.remoteUrl = Answer(0, L"D:\\bare\\one.git\n");  // get-url 只回答第一条
  const PushPreflightFacts many = gc::git::InterpretPushPreflight(multi);
  GC_CHECK(many.pushUrls.size() == 2);
  GC_CHECK(many.pushUrls[1] == L"D:\\bare\\two.git");
  GC_CHECK(!many.pushTargetIsFetchTarget);
  GC_CHECK(TextContains(many.pushUrlNote, L"只能"));

  // insteadOf 改写：展示的是 Git 算出来的那个地址，同时说明配置里写的原样是什么。
  PushPreflightQueries rewritten = HealthyPushQueries();
  rewritten.remoteUrl = Answer(0, L"ssh://git@internal/镜像.git\n");
  const PushPreflightFacts rewrite = gc::git::InterpretPushPreflight(rewritten);
  GC_CHECK(rewrite.pushUrlRewritten);
  GC_CHECK(rewrite.pushUrls.front() == L"ssh://git@internal/镜像.git");
  GC_CHECK(!rewrite.pushTargetIsFetchTarget);
  GC_CHECK(TextContains(rewrite.pushUrlNote, L"insteadOf"));

  // get-url 失败（远端根本不存在）：留不下「目标已定」的假象。
  PushPreflightQueries missing = HealthyPushQueries();
  missing.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
      {L"branch.main.pushremote", L"nowhere"},
  });
  missing.remoteUrl = Answer(128, L"", L"error: No such remote 'nowhere'");
  const PushPreflightFacts missingFacts = gc::git::InterpretPushPreflight(missing);
  GC_CHECK(missingFacts.pushRemoteName == L"nowhere");
  GC_CHECK(!missingFacts.pushRemoteExists);
  GC_CHECK(missingFacts.pushUrls.empty());
}

GC_TEST(push_preflight_spots_configs_that_would_widen_the_scope) {
  PushPreflightQueries widened = HealthyPushQueries();
  widened.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.mirror", L"true"},
      {L"remote.origin.tagopt", L"--tags"},
      {L"remote.origin.push", L"+refs/heads/*:refs/heads/*"},
      {L"push.followtags", L"true"},
      {L"push.default", L"current"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  const PushPreflightFacts facts = gc::git::InterpretPushPreflight(widened);
  GC_CHECK(facts.mirrorConfigured && facts.tagOptConfigured && facts.followTagsConfigured);
  GC_CHECK(facts.extraPushRefspecsConfigured);
  GC_CHECK(facts.pushDefault == L"current");

  // 明确的假值不算「有这条设置」：不该为此多带一句 -c。
  PushPreflightQueries falsy = HealthyPushQueries();
  falsy.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.mirror", L"false"},
      {L"push.followtags", L"no"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  const PushPreflightFacts off = gc::git::InterpretPushPreflight(falsy);
  GC_CHECK(!off.mirrorConfigured && !off.followTagsConfigured);

  // Git 不认的布尔取值（实测会让整条命令失败）按「会生效」处理：正是要中和它。
  PushPreflightQueries garbage = HealthyPushQueries();
  garbage.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.mirror", L"always"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  GC_CHECK(gc::git::InterpretPushPreflight(garbage).mirrorConfigured);
}

GC_TEST(push_preflight_fails_closed_when_config_cannot_be_read) {
  PushPreflightQueries queries = HealthyPushQueries();
  queries.configListing = Answer(128, L"", L"fatal: bad config file line 7");
  const PushPreflightFacts facts = gc::git::InterpretPushPreflight(queries);
  GC_CHECK(!facts.queryOk);
  GC_CHECK(TextContains(facts.queryFailure, L"生效配置"));
}

GC_TEST(push_preflight_reports_unparseable_ahead_behind_without_inventing_it) {
  PushPreflightQueries queries = HealthyPushQueries();
  queries.aheadBehind = Answer(0, L"两 个\n");
  const PushPreflightFacts facts = gc::git::InterpretPushPreflight(queries);
  GC_CHECK(facts.queryOk);
  GC_CHECK(!facts.relationship.known);
  GC_CHECK(!facts.relationship.detail.empty());

  PushPreflightQueries skipped = HealthyPushQueries();
  skipped.aheadBehindRan = false;
  skipped.aheadBehind = LaunchFailed();
  const PushPreflightFacts never = gc::git::InterpretPushPreflight(skipped);
  GC_CHECK(!never.relationship.known);
}

// ---- 方案：拒绝的场合 ----

GC_TEST(push_plan_refuses_without_touching_anything) {
  PushPreflightFacts facts = HealthyFacts();

  // 无上游：不猜 origin、不代设 upstream。
  PushPreflightQueries queries = HealthyPushQueries();
  queries.upstream = Answer(0);
  PushPlan plan = PlanFor(gc::git::InterpretPushPreflight(queries));
  GC_CHECK(plan.state == PushPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(TextContains(plan.explanation, L"没有设置上游"));
  GC_CHECK(TextContains(plan.explanation, L"--set-upstream"));
  GC_CHECK(!TextContains(plan.explanation, L"已推送"));

  // 游离 HEAD。
  queries = HealthyPushQueries();
  queries.symbolicRef = NoResult();
  queries.upstreamRan = false;
  queries.upstream = LaunchFailed();
  plan = PlanFor(gc::git::InterpretPushPreflight(queries));
  GC_CHECK(plan.state == PushPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(TextContains(plan.explanation, L"游离 HEAD"));

  // 分支还没有提交。
  queries = HealthyPushQueries();
  queries.headObject = NoResult();
  plan = PlanFor(gc::git::InterpretPushPreflight(queries));
  GC_CHECK(plan.state == PushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"还没有任何提交"));

  // 目标远端不在清单里：不猜另一个远端顶替，但把既有远端说出来。
  queries = HealthyPushQueries();
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
      {L"branch.main.pushremote", L"nowhere"},
  });
  queries.remoteUrl = Answer(128, L"", L"error: No such remote 'nowhere'");
  plan = PlanFor(gc::git::InterpretPushPreflight(queries));
  GC_CHECK(plan.state == PushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"nowhere"));
  GC_CHECK(TextContains(plan.explanation, L"既有的远端是：origin"));

  // 上游没有给出远端那一侧的引用：不「改写成一个看起来对的」。
  queries = HealthyPushQueries();
  queries.upstream = Answer(0, L"origin\trefs/remotes/origin/main\t\n");
  plan = PlanFor(gc::git::InterpretPushPreflight(queries));
  GC_CHECK(plan.state == PushPlanState::blocked);
  GC_CHECK(TextContains(plan.explanation, L"远端引用形态不合格"));

  // 仓库目录为空（界面还没认出台账）：一条命令也不构造。
  GC_CHECK(gc::git::BuildPushPlan(facts, L"").state == PushPlanState::blocked);
}

// ---- 方案：可以推的场合 ----

GC_TEST(push_plan_ready_shows_source_target_and_scoped_command) {
  const PushPlan plan = PlanFor(HealthyFacts());
  GC_CHECK(plan.state == PushPlanState::ready);
  GC_CHECK(plan.localBranchRef == L"refs/heads/main");
  GC_CHECK(plan.remoteBranchRef == L"refs/heads/main");
  GC_CHECK(plan.remoteName == L"origin");
  GC_CHECK(plan.pushedObjectId == kHead);
  GC_CHECK(plan.arguments == gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main",
                                                                L"refs/heads/main", false, false));
  GC_CHECK(plan.operationId == L"push");
  GC_CHECK(!plan.requiresForce);
  GC_CHECK(plan.risks.empty());
  // 界面上要能一眼看到「源分支 / 目标远端 / 目标分支」这三样，而且说的是完整引用。
  GC_CHECK(TextContains(plan.confirmationText, L"源分支：refs/heads/main"));
  GC_CHECK(TextContains(plan.confirmationText, L"目标远端：origin"));
  GC_CHECK(TextContains(plan.confirmationText, L"目标分支：refs/heads/main"));
  GC_CHECK(TextContains(plan.confirmationText, kUrl));
  GC_CHECK(TextContains(plan.confirmationText,
                        L"push --recurse-submodules=no origin refs/heads/main:refs/heads/main"));
  GC_CHECK(TextContains(plan.confirmationText, L"领先 2 个提交"));
  GC_CHECK(TextContains(plan.notice, L"不带 --force"));
  GC_CHECK(TextContains(plan.explanation, L"refs/heads/main"));
  // 展示的命令与实际发出的命令一字不差，且不含任何凭据。
  GC_CHECK(plan.commandLabel == L"git -c push.followTags=false push --recurse-submodules=no origin "
                                L"refs/heads/main:refs/heads/main");
}

GC_TEST(push_plan_lists_every_risk_and_never_escalates) {
  PushPreflightQueries queries = HealthyPushQueries();
  queries.aheadBehind = Answer(0, L"0\t3\n");  // 本地落后：非快进会被 Git 拒绝
  queries.trackingRan = false;
  queries.trackingObject = LaunchFailed();
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.mirror", L"true"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  const PushPreflightFacts facts = gc::git::InterpretPushPreflight(queries);
  const PushPlan plan = PlanFor(facts);
  GC_CHECK(plan.state == PushPlanState::ready);
  GC_CHECK(plan.requiresForce);
  GC_CHECK(FindRisk(plan, L"non-fast-forward") != nullptr);
  GC_CHECK(FindRisk(plan, L"本地没有") != nullptr);  // 跟踪引用还没问回来时说得清清楚楚
  GC_CHECK(FindRisk(plan, L"mirror") != nullptr);
  // 风险再多，命令形态一个字都不变：不补 --force，也不减掉任何范围限制。
  GC_CHECK(plan.arguments == gc::git::BuildPushCommandArguments(L"origin", L"refs/heads/main",
                                                                L"refs/heads/main", true, false));
  GC_CHECK(!AnyArgumentContains(plan.arguments, L"--force"));
  GC_CHECK(TextContains(plan.confirmationText, L"non-fast-forward"));
}

GC_TEST(push_plan_warns_when_target_differs_from_fetch_side_and_is_up_to_date) {
  PushPreflightQueries queries = HealthyPushQueries();
  queries.aheadBehind = Answer(0, L"0\t0\n");  // 与已知位置一致
  queries.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
      {L"branch.main.pushremote", L"fork"},
      {L"remote.fork.url", L"https://u:secretp@host/fork.git"},
  });
  queries.remoteUrl = Answer(0, L"https://u:secretp@host/fork.git\n");
  const PushPlan plan = PlanFor(gc::git::InterpretPushPreflight(queries));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(FindRisk(plan, L"fork") != nullptr);   // 发布目标与上游不在一处
  GC_CHECK(FindRisk(plan, L"up-to-date") != nullptr);
  GC_CHECK(plan.remoteName == L"fork");
  // facts 里存的是 Git 给的原样地址，展示之前必须掩码。
  GC_CHECK(plan.pushUrls.front() == L"https://u:secretp@host/fork.git");
  GC_CHECK(TextContains(plan.notice, L"https://***@host/fork.git"));
  // 凭据不会从展示文字里漏出去：确认文字与范围说明都只用掩码后的地址。
  GC_CHECK(!TextContains(plan.confirmationText, L"secretp"));
  GC_CHECK(!TextContains(plan.notice, L"secretp"));
}

// ---- 执行前复核 ----

GC_TEST(push_recheck_accepts_identical_and_rejects_moved_head) {
  const PushPreflightFacts facts = HealthyFacts();
  GC_CHECK(gc::git::DescribePushChange(facts, facts).empty());

  PushPreflightQueries moved = HealthyPushQueries();
  moved.headObject = Answer(0, L"9999999999999999999999999999999999999999\n");
  const PushPreflightFacts latest = gc::git::InterpretPushPreflight(moved);
  const std::wstring change = gc::git::DescribePushChange(facts, latest);
  GC_CHECK(!change.empty());
  GC_CHECK(TextContains(change, L"要推送的提交"));
  GC_CHECK(TextContains(change, L"没有发出任何命令"));
}

GC_TEST(push_recheck_notices_target_and_config_drift) {
  const PushPreflightFacts preflight = HealthyFacts();

  PushPreflightQueries switched = HealthyPushQueries();
  switched.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.pushurl", L"D:\\bare\\elsewhere.git"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  switched.remoteUrl = Answer(0, L"D:\\bare\\elsewhere.git\n");
  std::wstring change = gc::git::DescribePushChange(preflight, gc::git::InterpretPushPreflight(switched));
  GC_CHECK(TextContains(change, L"发布 URL"));

  PushPreflightQueries checkout = HealthyPushQueries();
  checkout.symbolicRef = Answer(0, L"refs/heads/other\n");
  checkout.upstream = Answer(0, L"origin\trefs/remotes/origin/other\trefs/heads/other\n");
  change = gc::git::DescribePushChange(preflight, gc::git::InterpretPushPreflight(checkout));
  GC_CHECK(TextContains(change, L"当前分支"));

  PushPreflightQueries mirrorOn = HealthyPushQueries();
  mirrorOn.configListing = ConfigListingAnswer({
      {L"remote.origin.url", kUrl},
      {L"remote.origin.mirror", L"true"},
      {L"branch.main.remote", L"origin"},
      {L"branch.main.merge", L"refs/heads/main"},
  });
  change = gc::git::DescribePushChange(preflight, gc::git::InterpretPushPreflight(mirrorOn));
  GC_CHECK(TextContains(change, L"影响推送范围的配置"));

  // 复核本身失败 = 不执行：不能拿「问不出来」当成「没变」。
  PushPreflightFacts unreadable = HealthyFacts();
  unreadable.queryOk = false;
  unreadable.queryFailure = L"没能问出当前分支";
  change = gc::git::DescribePushChange(preflight, unreadable);
  GC_CHECK(TextContains(change, L"执行前的复核没能完成"));
}

GC_TEST(push_recheck_ignores_worktree_changes_because_push_doesnt_touch_them) {
  // 两份事实的差别只在「跟踪引用位置」以外都没有变化时才算一致。
  // 这里刻意不比较工作区/索引：推送只送已有的提交，点头之后用户又改了一个没暂存的文件，
  // 不该让一次合法的推送作废。facts 里根本没有工作区模型，故此路径按「两份一致」处理。
  const PushPreflightFacts facts = HealthyFacts();
  GC_CHECK(gc::git::DescribePushChange(facts, facts).empty());
}

// ---- 推送之后的核实 ----

GC_TEST(push_verification_reads_only_the_requested_ref) {
  const GitQueryResult answer = Answer(0, std::wstring(kHead) + L"\trefs/heads/main\n" +
                                           std::wstring(kRemoteCommit) + L"\trefs/heads/other\n");
  const PushTargetCheck check = gc::git::InterpretPushTargetCheck(kUrl, answer, L"refs/heads/main");
  GC_CHECK(check.queried && check.ok && check.refPresent);
  GC_CHECK(check.remoteObjectId == kHead);  // 顺带回别的引用不能顶替目标

  const PushTargetCheck absent =
      gc::git::InterpretPushTargetCheck(kUrl, Answer(0), L"refs/heads/main");
  GC_CHECK(absent.queried && absent.ok && !absent.refPresent);  // 问成功但对端没这条引用

  const PushTargetCheck failed =
      gc::git::InterpretPushTargetCheck(kUrl, Answer(128, L"", L"fatal: could not read"), L"refs/heads/main");
  GC_CHECK(failed.queried && !failed.ok);
  GC_CHECK(!failed.failure.empty());

  const PushTargetCheck never =
      gc::git::InterpretPushTargetCheck(kUrl, LaunchFailed(), L"refs/heads/main");
  GC_CHECK(!never.queried);

  const PushTargetCheck malformed = gc::git::InterpretPushTargetCheck(
      kUrl, Answer(0, L"短ID\trefs/heads/main\n"), L"refs/heads/main");
  GC_CHECK(!malformed.refPresent);
}

GC_TEST(push_verification_report_kinds_and_wording) {
  PushTargetCheck good;
  good.url = kUrl;
  good.queried = good.ok = good.refPresent = true;
  good.remoteObjectId = kHead;

  PushTargetCheck bad;
  bad.url = L"D:\\bare\\two.git";
  bad.queried = bad.ok = true;
  bad.refPresent = true;
  bad.remoteObjectId = kRemoteCommit;

  PushTargetCheck unreachable;
  unreachable.url = L"D:\\bare\\three.git";
  unreachable.queried = true;
  unreachable.failure = L"Git 查询超时";

  // 全部核上：措辞是「已核实」，并且说的是同一份提交。
  const PushVerificationReport confirmed =
      gc::git::ComposePushVerification({good}, kHead, L"refs/heads/main", true, L"执行成功");
  GC_CHECK(confirmed.verdict == PushVerificationVerdict::confirmed);
  GC_CHECK(TextContains(confirmed.headline, L"已核实"));
  GC_CHECK(confirmed.lines.size() == 1);
  GC_CHECK(TextContains(confirmed.lines.front(), L"正是这次推出去的那一份"));
  GC_CHECK(!TextContains(confirmed.lines.front(), kHead));  // 展示用短 ID，不抄整串

  // 命令报成功、目标上却不是那一份：绝不能写成推送成功。
  const PushVerificationReport mismatched =
      gc::git::ComposePushVerification({good, bad}, kHead, L"refs/heads/main", true, L"执行成功");
  GC_CHECK(mismatched.verdict == PushVerificationVerdict::mismatched);
  GC_CHECK(TextContains(mismatched.headline, L"不把它算成推送成功"));
  GC_CHECK(TextContains(mismatched.lines[1], L"不是这次推出去的那一份"));

  // 命令失败但对端已经到了：以实况为准，仍然不写「推送成功」。
  const PushVerificationReport failedButThere =
      gc::git::ComposePushVerification({good}, kHead, L"refs/heads/main", false, L"退出码 1");
  GC_CHECK(failedButThere.verdict == PushVerificationVerdict::confirmed);
  GC_CHECK(!TextContains(failedButThere.headline, L"推送成功"));
  GC_CHECK(TextContains(failedButThere.headline, L"以实况为准"));

  // 有目标没问到 = 部分核实，说清各占几个。
  const PushVerificationReport mixed =
      gc::git::ComposePushVerification({good, unreachable}, kHead, L"refs/heads/main", true, L"执行成功");
  GC_CHECK(mixed.verdict == PushVerificationVerdict::mixed);
  GC_CHECK(TextContains(mixed.headline, L"没能问到"));
  GC_CHECK(TextContains(mixed.lines[1], L"没能问到（Git 查询超时）"));

  // 一个都没核上：结论只能回到命令窗口的输出。
  const PushVerificationReport nothing =
      gc::git::ComposePushVerification({unreachable}, kHead, L"refs/heads/main", true, L"执行成功");
  GC_CHECK(nothing.verdict == PushVerificationVerdict::nothingChecked);
  GC_CHECK(TextContains(nothing.headline, L"只能以命令窗口里 Git 的真实输出为准"));

  const PushVerificationReport empty =
      gc::git::ComposePushVerification({}, kHead, L"refs/heads/main", true, L"执行成功");
  GC_CHECK(empty.verdict == PushVerificationVerdict::nothingChecked);
  GC_CHECK(TextContains(empty.headline, L"没有向发布目标发出核对"));

  // 对端引用缺席按「不符」处理：那是明确的不同，不是没问到。
  PushTargetCheck missingRef;
  missingRef.url = kUrl;
  missingRef.queried = missingRef.ok = true;
  const PushVerificationReport absent =
      gc::git::ComposePushVerification({missingRef}, kHead, L"refs/heads/main", true, L"执行成功");
  GC_CHECK(absent.verdict == PushVerificationVerdict::mismatched);
  GC_CHECK(TextContains(absent.lines.front(), L"对端没有这条引用"));

  GC_CHECK(gc::git::PushVerificationVerdictLabel(PushVerificationVerdict::mixed) == L"部分核实");
}

GC_TEST(push_verification_expected_id_must_be_a_full_object_id) {
  PushTargetCheck check;
  check.url = kUrl;
  check.queried = check.ok = check.refPresent = true;
  check.remoteObjectId = kHead;
  // 拿不到「这次推的是哪一份」时不假装核得上：只能说「对端有一条引用」，不写「正是那一份」。
  const PushVerificationReport report =
      gc::git::ComposePushVerification({check}, L"", L"refs/heads/main", true, L"执行成功");
  GC_CHECK(report.verdict == PushVerificationVerdict::mismatched);
  GC_CHECK(TextContains(report.lines.front(), L"不是这次推出去的那一份"));
}
