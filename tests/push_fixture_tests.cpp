// push 的集成测试：全部在夹具自己创建并认领所有权的临时目录里，用真实 Git 驱动生产编排
// （platform::CollectPushPreflight）与生产方案层（git::BuildPushPlan），再把方案合成的那条命令
// **原样**交给真实 Git 执行，然后拿 bare 远端的实际引用验结论。远端只有本机临时根里的 bare
// 仓库，全程不联网（夹具环境写死 GIT_ALLOW_PROTOCOL=file，任何网络 URL 在发起连接前就被 Git 拒绝）。
//
// 覆盖的场景（A/B 两个本地工作区 + 同根下的 bare origin，另加若干 bare 当发布目标）：
//   * 正常领先：预检从真实配置里读出分支/上游/发布目标，命令只带这一条完整 refspec，
//     执行后 bare 上那条引用真的到了本地这一份提交，本地跟踪引用也跟着前移；
//   * 范围没被扩大：本地另有分支与标签时，对端只多出被点名的那一条；
//   * 仓库配置想扩大范围：remote.<远端>.push（全部分支+标签）、tagOpt=--tags、push.followTags、
//     push.default=current 都进不去；remote.<远端>.mirror=true 实测会让整条命令以 128 失败，
//     方案为这一个子进程补的 -c ...=false 把它还原成「只推这一条」；
//   * 已是最新：Git 自己回答 Everything up-to-date，对端一个字没动；
//   * 非快进（撤回已发布提交 / 别人先推进过）：Git 拒绝，命令里没有也不会有 --force，
//     本地 HEAD 与对端引用都停在原位，核实环节把对端实况原样报出来；
//   * 无上游：判读如实 blocked，不产生命令、不写 branch.<名>.remote、不凭空造跟踪引用，
//     夹具那份「用户层」配置档案依旧为空；游离 HEAD 同样拒绝；
//   * 发布目标另有去处：独立 pushurl 时东西真的落在 pushurl 那个 bare，抓取那一侧的 bare
//     拿不到；url.*.insteadOf 改写时同样按改写后的地址为准展示与核实；多个 pushurl 会每个都收到；
//     两个无 pushurl 的地址各被 pushInsteadOf 改写时，逐条展开的顺序与各自落点都对得上；
//   * 源侧钉死完整提交 ID：预检之后外部把分支再推进，命令送出去的仍是被确认的那一份；
//     该形态下 pre-push hook 照常执行、钩子看到的仍是目标引用与那份提交，跟踪引用前移
//     以 Git 的回答为准钉住（这三条是「改用对象 ID 不牺牲别的承诺」的凭据）；
//   * 省略取值的布尔裸键（配置里不写 `= ` 的 mirror）：整份配置照读（旧实现会因这条记录
//     把整个预检判死），并按真值中和；
//   * 无效本地目标：远端 URL 指向一个根本没建出来的目录时，命令失败，核实环节只说「没能问到」，
//     绝不把「窗口已打开」或本地引用看起来一致当成推送成功。
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "git/push_plan.h"
#include "git/repository.h"
#include "platform/windows/push_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::GitQueryResult;
using gc::git::PushPlan;
using gc::git::PushPlanState;
using gc::git::PushPreflightFacts;
using gc::git::PushTargetCheck;
using gc::git::PushVerificationReport;
using gc::git::PushVerificationVerdict;
using gc::platform::PushProbeDeps;
using gc::platform::PushProbeRequest;
using gc::test::GitFixture;
using gc::test::GitRun;
using gc::test::RemoteRig;

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

std::wstring Trimmed(const std::wstring& text) { return gc::git::TrimWide(text); }

GitQueryResult AsQuery(const GitRun& run) {
  GitQueryResult result;
  result.started = run.started;
  result.timedOut = run.timedOut;
  result.exited = run.exited;
  result.exitCode = static_cast<int>(run.exitCode);
  result.utf16Output = run.out;
  result.utf16Error = run.err;
  return result;
}

// 用夹具的隔离执行器装配生产预检依赖：查询走与界面完全相同的代码路径
// （同一套参数构造、同一套判读、同一条隐藏子进程执行链）。
PushProbeDeps MakePushDeps(GitFixture& fixture) {
  PushProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);  // 夹具固定使用自己验证过的 git.exe。
    return AsQuery(fixture.Run(arguments, directory));
  };
  return deps;
}

std::wstring AbsoluteGitDir(GitFixture& fixture) {
  return Trimmed(fixture.RunCheckedInRepo({L"rev-parse", L"--absolute-git-dir"}).out);
}

// 完整跑一遍生产预检。界面用的就是这同一个入口（CollectPushPreflight）。
PushPreflightFacts Probe(GitFixture& fixture) {
  PushProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.absoluteGitDir = AbsoluteGitDir(fixture);
  request.timeoutMilliseconds = 20000;
  const PushProbeDeps deps = MakePushDeps(fixture);
  return gc::platform::CollectPushPreflight(request, deps);
}

// bare 远端上某个引用的实际位置（问的是那个 bare 目录本身，不借任何本地引用说话）。
std::wstring RemoteRef(GitFixture& fixture, const std::wstring& url, std::wstring_view ref) {
  const GitRun run = fixture.Run({L"ls-remote", L"--", url, std::wstring(ref)}, fixture.RepoDir());
  if (!run.Success()) {
    return std::wstring();
  }
  for (const std::wstring& line : gc::git::SplitLines(run.out)) {
    const size_t tab = line.find(L'\t');
    if (tab == std::wstring::npos) {
      continue;
    }
    if (Trimmed(line.substr(tab + 1)) == std::wstring(ref)) {
      return Trimmed(line.substr(0, tab));
    }
  }
  return std::wstring();
}

// 直接读那个 bare 自己的引用库（`--git-dir` + rev-parse）：不经过传输，因此也不会被
// url.*.insteadOf 改写。要证明「东西到底落到了哪一个 bare」就用这一条。
std::wstring BareLocalRef(GitFixture& fixture, const std::wstring& bareDirectory,
                          std::wstring_view ref) {
  const GitRun run = fixture.Run({L"--git-dir", bareDirectory, L"rev-parse", L"--verify", L"--quiet",
                                  std::wstring(ref)},
                                 fixture.RepoDir());
  return run.Success() ? Trimmed(run.out) : std::wstring();
}

// bare 远端上所有引用名（用来证明「别的分支与标签没被顺手推上去」）。
std::vector<std::wstring> RemoteRefs(GitFixture& fixture, const std::wstring& url) {
  const GitRun run = fixture.Run({L"ls-remote", url}, fixture.RepoDir());
  std::vector<std::wstring> refs;
  if (!run.Success()) {
    return refs;
  }
  for (const std::wstring& line : gc::git::SplitLines(run.out)) {
    const size_t tab = line.find(L'\t');
    if (tab != std::wstring::npos) {
      refs.push_back(Trimmed(line.substr(tab + 1)));
    }
  }
  return refs;
}

bool HasRef(const std::vector<std::wstring>& refs, std::wstring_view ref) {
  for (const std::wstring& candidate : refs) {
    if (candidate == ref) {
      return true;
    }
  }
  return false;
}

// 把方案合成的那条命令原样交给真实 Git（界面里它走命令窗口，参数数组是同一份）。
GitRun ExecutePlan(GitFixture& fixture, const PushPlan& plan) {
  return fixture.Run(plan.arguments, fixture.RepoDir());
}

// 用生产的判读与措辞函数跑一遍「向发布目标核实」：核实请求本身由界面在终态之后异步发出，
// 这里注入夹具的执行器，问的是同一批参数、走的是同一条判读链。
PushVerificationReport Verify(GitFixture& fixture, const PushPlan& plan, bool commandSucceeded,
                              std::wstring_view conclusion) {
  std::vector<PushTargetCheck> checks;
  for (const std::wstring& url : plan.pushUrls) {
    const std::vector<std::wstring> arguments = gc::git::BuildPushRemoteProbeArguments(
        fixture.RepoDir(), url, plan.remoteBranchRef);
    const GitQueryResult result = AsQuery(fixture.Run(arguments, fixture.RepoDir()));
    checks.push_back(gc::git::InterpretPushTargetCheck(url, result, plan.remoteBranchRef));
  }
  return gc::git::ComposePushVerification(checks, plan.pushedObjectId, plan.remoteBranchRef,
                                          commandSucceeded, conclusion);
}

void ExpectNoGlobalConfig(GitFixture& fixture) {
  // 「用户层」配置档案仍然是空的：整条链路一个字都没写进去（本程序从不改配置）。
  GC_CHECK_MESSAGE(Trimmed(fixture.RunCheckedInRepo({L"config", L"--global", L"--list"}).out).empty(),
                   "推送把使用者的全局配置文件改了");
}

}  // namespace

// ---- 正常领先：真实配置 → 命令 → 对端真的收到 ----

GC_TEST(push_ahead_command_lands_on_the_bare_and_updates_the_tracking_ref) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  // 夹具建台时已经把初始提交推上去了；这里再让 A 本地领先一条。
  fixture.WriteFile(L"pushed-by-app.txt", "推送实测\n");
  fixture.StageAll();
  fixture.Commit(L"待推送的第二条提交");

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.onBranch && facts.branchName == L"main");
  GC_CHECK(facts.upstreamConfigured && facts.upstreamRemote == L"origin");
  GC_CHECK(facts.upstreamTrackingRef == L"refs/remotes/origin/main");
  GC_CHECK(facts.upstreamRemoteRef == L"refs/heads/main");
  GC_CHECK(facts.headObjectId == fixture.HeadSha());
  GC_CHECK(facts.pushRemoteName == L"origin");
  GC_CHECK(facts.pushRemoteExists);
  GC_CHECK(facts.pushUrls.size() == 1 && facts.pushUrls.front() == originUrl);
  GC_CHECK(facts.pushTargetIsFetchTarget);
  GC_CHECK_MESSAGE(facts.relationship.known && facts.relationship.ahead == 1 &&
                       facts.relationship.behind == 0,
                   "领先/落后应按真实的跟踪引用判出（ahead=" + std::to_string(facts.relationship.ahead) +
                       " behind=" + std::to_string(facts.relationship.behind) + "）");
  // 干净的克隆里不该冒出任何「要用户明确点头」的风险。
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(!plan.requiresForce);
  GC_CHECK(plan.pushedObjectId == fixture.HeadSha());
  GC_CHECK_MESSAGE(plan.arguments ==
                       gc::git::BuildPushCommandArguments(L"origin", plan.pushedObjectId,
                                                          L"refs/heads/main", false, false),
                   "实际发出的参数与参数构造器的结果不一致");
  // 源侧钉死为完整提交 ID：命令里出现的不是分支名，是「这一份提交」。
  GC_CHECK_MESSAGE(plan.arguments.back() == plan.pushedObjectId + L":refs/heads/main",
                   "命令源侧没有钉在完整提交 ID 上：" + ToUtf8(plan.arguments.back()));

  const std::wstring beforeOnRemote = RemoteRef(fixture, originUrl, L"refs/heads/main");
  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  // 以「对端那条引用真的到了这一份提交」为准，而不是拿本地某个引用看起来一致充数。
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == plan.pushedObjectId);
  GC_CHECK(beforeOnRemote != plan.pushedObjectId);
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == plan.pushedObjectId);

  const PushVerificationReport report = Verify(fixture, plan, true, L"执行成功");
  GC_CHECK(report.verdict == PushVerificationVerdict::confirmed);
  GC_CHECK_MESSAGE(Contains(report.headline, L"已核实"), ToUtf8(report.headline));
  ExpectNoGlobalConfig(fixture);
}

// ---- 范围：别的分支与标签一个字都不过去 ----

GC_TEST(push_scoped_refspec_leaves_other_branches_and_tags_alone) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  fixture.WriteFile(L"third.txt", "3\n");
  fixture.StageAll();
  fixture.Commit(L"第三条");
  // 本地另有其名：一条分支、一个附注标签、一个轻量标签——都不该被这次推送带过去。
  fixture.RunCheckedInRepo({L"branch", L"feature/side"});
  fixture.RunCheckedInRepo({L"tag", L"-a", L"v-tag", L"-m", L"附注标签"});
  fixture.RunCheckedInRepo({L"tag", L"light-tag"});

  const PushPlan plan = gc::git::BuildPushPlan(Probe(fixture), fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == plan.pushedObjectId);

  const std::vector<std::wstring> refs = RemoteRefs(fixture, originUrl);
  GC_CHECK(!HasRef(refs, L"refs/heads/feature/side"));
  GC_CHECK(!HasRef(refs, L"refs/tags/v-tag"));
  GC_CHECK(!HasRef(refs, L"refs/tags/light-tag"));
}

// ---- 仓库配置想扩大范围：一概进不去 ----

GC_TEST(push_hazard_configs_cannot_widen_the_scope_and_mirror_is_neutralized) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  fixture.WriteFile(L"hazard.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"带危险配置的那一条");
  fixture.RunCheckedInRepo({L"branch", L"extra"});
  fixture.RunCheckedInRepo({L"tag", L"-a", L"hazard-tag", L"-m", L"标签"});
  // 每一条都是「让 push 顺手多推点东西」的真实配置形态。
  fixture.RunCheckedInRepo({L"config", L"--add", L"remote.origin.push",
                            L"+refs/heads/*:refs/heads/*"});
  fixture.RunCheckedInRepo({L"config", L"--add", L"remote.origin.push", L"refs/tags/*:refs/tags/*"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.tagOpt", L"--tags"});
  fixture.RunCheckedInRepo({L"config", L"push.followTags", L"true"});
  fixture.RunCheckedInRepo({L"config", L"push.default", L"current"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.mirror", L"true"});

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.mirrorConfigured && facts.tagOptConfigured && facts.followTagsConfigured);
  GC_CHECK(facts.extraPushRefspecsConfigured);
  GC_CHECK(facts.pushDefault == L"current");

  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  // mirror 的存在要被中和（实测它会让带 refspec 的 push 整条失败），tagOpt 也一样。
  GC_CHECK_MESSAGE(
    std::find(plan.arguments.begin(), plan.arguments.end(), L"remote.origin.mirror=false") !=
            plan.arguments.end(),
    "没有为这一个子进程中和 remote.origin.mirror");
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"remote.origin.tagopt=") !=
           plan.arguments.end());
  GC_CHECK(!Contains(plan.commandLabel, L"--force"));

  // 先证明这些危险配置确实是「生效的」：不带中和的那几句时，同一条命令会被 Git 拒绝。
  const GitRun raw = fixture.Run({L"push", L"--recurse-submodules=no", L"origin",
                                  L"refs/heads/main:refs/heads/main"}, fixture.RepoDir());
  GC_CHECK_MESSAGE(!raw.Success(), "带 mirror=true 的裸 push 本该失败，却成功了");
  GC_CHECK_MESSAGE(Contains(raw.err, L"--mirror") || Contains(raw.err, L"mirror"),
                   ToUtf8(raw.err));

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == plan.pushedObjectId);
  const std::vector<std::wstring> refs = RemoteRefs(fixture, originUrl);
  GC_CHECK(!HasRef(refs, L"refs/heads/extra"));  // remote.origin.push 的全部分支没过去
  GC_CHECK(!HasRef(refs, L"refs/tags/hazard-tag"));  // tagOpt / followTags 都没带出标签
  ExpectNoGlobalConfig(fixture);
}

// ---- 已是最新：Git 自己说 up-to-date，对端一个字没动 ----

GC_TEST(push_when_already_up_to_date_leaves_the_bare_untouched) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  const PushPreflightFacts facts = Probe(fixture);  // A 推过初始提交，这里两边一致
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.relationship.known && facts.relationship.ahead == 0 && facts.relationship.behind == 0);
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  // 「看起来已一致」要当风险摆出来：本地这份了解可能早就过期，仍由用户决定要不要去问个准话。
  GC_CHECK(plan.requiresForce);

  const std::wstring before = RemoteRef(fixture, originUrl, L"refs/heads/main");
  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK_MESSAGE(Contains(pushed.out + pushed.err, L"Everything up-to-date"),
                   ToUtf8(pushed.out + pushed.err));
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == before);
  GC_CHECK(Contains(Verify(fixture, plan, true, L"执行成功").headline, L"已核实"));
}

// ---- 非快进：Git 拒绝，程序不升级 ----

GC_TEST(push_non_fast_forward_is_rejected_without_any_escalation) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  // 先把远端推进（B 提交并推送），再让 A 抓取到那个位置，最后 A 本地另起一条提交：
  // 这就是「撤回已发布提交后再推」之外的另一种 non-fast-forward 现场。
  rig.UseB();
  fixture.WriteFile(L"from-b.txt", "B 先推的\n");
  fixture.StageAll();
  fixture.Commit(L"B 先到的那一条");
  fixture.Push(L"origin", L"main");
  const std::wstring bSha = fixture.HeadSha();

  rig.UseA();
  fixture.RunChecked({L"fetch", L"origin"}, fixture.RepoDir());
  fixture.WriteFile(L"from-a.txt", "A 本地的一条\n");
  fixture.StageAll();
  fixture.Commit(L"A 后起的那一条");

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK_MESSAGE(facts.relationship.known && facts.relationship.behind >= 1,
                   "抓取之后应当看见远端有本地没有的提交");
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);
  for (const std::wstring& argument : plan.arguments) {
    GC_CHECK(!Contains(argument, L"--force") && !Contains(argument, L"--force-with-lease"));
  }
  GC_CHECK(Contains(plan.explanation, L"refs/heads/main"));

  const std::wstring localHead = fixture.HeadSha();
  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(!pushed.Success(), "非快进的推送本该被 Git 拒绝，却成功了");
  GC_CHECK_MESSAGE(Contains(pushed.err, L"[rejected]") || Contains(pushed.err, L"failed to push"),
                   ToUtf8(pushed.err));
  // 现场保留：对端仍是 B 那一条，本地仍是 A 那一条，谁也没被抹掉。
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == bSha);
  GC_CHECK(fixture.HeadSha() == localHead);
  // 核实环节的作用就在这类场合：把「对端到底在哪」如实报出来，而不是只说「失败了」。
  const PushVerificationReport report = Verify(fixture, plan, false, L"退出码非 0");
  GC_CHECK(report.verdict == PushVerificationVerdict::mismatched);
  GC_CHECK(!Contains(report.headline, L"已核实"));
  GC_CHECK_MESSAGE(Contains(report.lines.front(), L"不是这次推出去的那一份"),
                   ToUtf8(report.lines.empty() ? std::wstring() : report.lines.front()));
  ExpectNoGlobalConfig(fixture);
}

// ---- 无上游 / 游离 HEAD：拒绝且不写任何配置 ----

GC_TEST(push_without_upstream_is_refused_and_writes_nothing) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  // 一个只 init + 提交、连远端都没有的仓库：push 没有任何可依据的目标。
  fixture.InitRepository(L"lonely");
  fixture.WriteFile(L"only.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"还没有远端的提交");

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.upstreamRan && !facts.upstreamConfigured);
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_CHECK_MESSAGE(plan.state == PushPlanState::blocked, ToUtf8(plan.explanation));
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(Contains(plan.explanation, L"没有设置上游"));
  GC_CHECK(Contains(plan.explanation, L"不猜"));
  GC_CHECK(Contains(plan.explanation, L"--set-upstream"));  // 拒绝的同时给出可操作的办法

  // 拒绝不等于顺手补配置：既没有 branch.main.remote，也没有凭空多出来的跟踪引用。
  GC_CHECK(Trimmed(fixture.Run({L"config", L"--get", L"branch.main.remote"}, fixture.RepoDir()).out)
               .empty());
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main").empty());
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(push_detached_head_and_unborn_branch_are_refused) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring branchHead = fixture.HeadSha();

  fixture.RunCheckedInRepo({L"checkout", L"--detach", L"HEAD"});
  const PushPreflightFacts detached = Probe(fixture);
  GC_REQUIRE_MESSAGE(detached.queryOk, ToUtf8(detached.queryFailure));
  GC_CHECK(!detached.onBranch);
  const PushPlan detachedPlan = gc::git::BuildPushPlan(detached, fixture.RepoDir());
  GC_CHECK(detachedPlan.state == PushPlanState::blocked);
  GC_CHECK(detachedPlan.arguments.empty());
  GC_CHECK(Contains(detachedPlan.explanation, L"游离 HEAD"));
  fixture.RunCheckedInRepo({L"checkout", L"main"});  // 切回分支，别把现场留给下一个用例
  GC_CHECK(fixture.HeadSha() == branchHead);

  // 尚无提交的仓库：连「要推哪一份」都没有，程序不猜也不创建空提交。
  fixture.InitRepository(L"unborn");
  const PushPreflightFacts unborn = Probe(fixture);
  GC_CHECK_MESSAGE(unborn.queryOk && !unborn.headResolved, ToUtf8(unborn.queryFailure));
  const PushPlan unbornPlan = gc::git::BuildPushPlan(unborn, fixture.RepoDir());
  GC_CHECK(unbornPlan.state == PushPlanState::blocked);
  GC_CHECK(Contains(unbornPlan.explanation, L"还没有任何提交"));
}

// ---- 发布目标与抓取目标不在一处 ----

GC_TEST(push_goes_to_the_push_url_and_not_to_the_fetch_url) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  fixture.InitBareRepository(L"publish.git");
  const std::wstring publishUrl = fixture.PathInRoot(L"publish.git");
  rig.UseA();  // 建 bare 会把夹具的活动工作区切过去：写完这一句才回到 A

  fixture.WriteFile(L"publish-only.txt", "只该出现在发布目标上\n");
  fixture.StageAll();
  fixture.Commit(L"要推到独立 push URL 的那一条");
  fixture.RunCheckedInRepo({L"config", L"remote.origin.pushurl", publishUrl});

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.pushUrls.size() == 1 && facts.pushUrls.front() == publishUrl);
  GC_CHECK(!facts.pushTargetIsFetchTarget);
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK_MESSAGE(plan.requiresForce, "发布目标与抓取目标不同处时必须要求明确点头");
  // 界面上说的是「目标远端 origin」，实际去的是 publish.git：两个都必须看得见。
  GC_CHECK(Contains(plan.confirmationText, L"目标远端：origin"));
  GC_CHECK(Contains(plan.confirmationText, publishUrl));

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  const std::wstring expected = fixture.HeadSha();
  // 关键的一条：发布目标收到了，抓取的那个裸仓库并没有。
  GC_CHECK(RemoteRef(fixture, publishUrl, L"refs/heads/main") == expected);
  GC_CHECK(BareLocalRef(fixture, rig.OriginUrl(), L"refs/heads/main") != expected);
  // 实测：Git 推送成功后照样把本地 refs/remotes/origin/main 挪到了这一份提交，哪怕东西实际
  // 去的是另一个地址。所以「本地跟踪引用看起来一致」绝不能当推送成功的证据——只认发布目标的回答。
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == expected);
  // 核实按发布目标的地址问，因此能核上；换成抓取的裸仓库就永远核不上。
  GC_CHECK(Contains(Verify(fixture, plan, true, L"执行成功").headline, L"已核实"));
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(push_rewrites_with_instead_of_and_reports_the_effective_target) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  fixture.InitBareRepository(L"mirror.git");
  const std::wstring mirrorUrl = fixture.PathInRoot(L"mirror.git");
  rig.UseA();
  const std::wstring originUrl = rig.OriginUrl();

  fixture.WriteFile(L"rewritten.txt", "被 insteadOf 改写\n");
  fixture.StageAll();
  fixture.Commit(L"改写之后才知道去了哪");
  // Git 的 insteadOf 会在发起传输前把地址换掉：界面必须显示换过之后的那个，
  // 而且核实也要问那个地方，否则「抓取的远端」与「实际发布目标」就会被混为一谈。
  fixture.RunCheckedInRepo({L"config", L"url." + mirrorUrl + L".insteadOf", originUrl});

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.pushUrlRewritten);
  GC_CHECK(facts.pushUrls.size() == 1 && facts.pushUrls.front() == mirrorUrl);
  GC_CHECK(Contains(facts.pushUrlNote, L"insteadOf"));
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(Contains(plan.confirmationText, L"insteadOf"));

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  // 两个 bare 都用 --git-dir 直接读自己的引用库：ls-remote 也会被 insteadOf 改写，
  // 拿它问「原来的地址收到了吗」只会问出改写后的那个，证不了这一条。
  GC_CHECK(BareLocalRef(fixture, mirrorUrl, L"refs/heads/main") == plan.pushedObjectId);
  GC_CHECK(BareLocalRef(fixture, originUrl, L"refs/heads/main") != plan.pushedObjectId);
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(push_reaches_every_one_of_multiple_push_urls) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  fixture.InitBareRepository(L"copy-one.git");
  fixture.InitBareRepository(L"copy-two.git");
  const std::wstring oneUrl = fixture.PathInRoot(L"copy-one.git");
  const std::wstring twoUrl = fixture.PathInRoot(L"copy-two.git");
  rig.UseA();

  fixture.WriteFile(L"multi.txt", "两个发布目标\n");
  fixture.StageAll();
  fixture.Commit(L"要同时送两个地方的那一条");
  fixture.RunCheckedInRepo({L"config", L"--add", L"remote.origin.pushurl", oneUrl});
  fixture.RunCheckedInRepo({L"config", L"--add", L"remote.origin.pushurl", twoUrl});

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.pushUrls.size() == 2);  // 一个都不许漏：Git 实测会推给每一个
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(Contains(plan.confirmationText, L"共 2 个"));
  bool saysEveryTarget = false;
  for (const std::wstring& risk : plan.risks) {
    saysEveryTarget = saysEveryTarget || Contains(risk, L"每一个");
  }
  GC_CHECK_MESSAGE(saysEveryTarget && Contains(plan.confirmationText, L"每一个"),
                   "多个发布目标时必须说清每一个都会收到");

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK(RemoteRef(fixture, oneUrl, L"refs/heads/main") == plan.pushedObjectId);
  GC_CHECK(RemoteRef(fixture, twoUrl, L"refs/heads/main") == plan.pushedObjectId);
  const PushVerificationReport report = Verify(fixture, plan, true, L"执行成功");
  GC_CHECK(report.verdict == PushVerificationVerdict::confirmed);
  GC_CHECK(report.lines.size() == 2);
  ExpectNoGlobalConfig(fixture);
}

// ---- 两个目标各被 pushInsteadOf 改写：展开、推送、核实三者必须是同一批地址 ----

GC_TEST(push_resolves_two_urls_rewritten_by_push_instead_of) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();
  // 两个真实存在的 bare 当「改写后落点」；两个「写进配置」的地址里第一个就是抓取的 bare，
  // 第二条只是一个从没建出来的路径——push 方向若真的逐条改写，命令才能成功。
  fixture.InitBareRepository(L"landed-one.git");
  fixture.InitBareRepository(L"landed-two.git");
  const std::wstring landedOne = fixture.PathInRoot(L"landed-one.git");
  const std::wstring landedTwo = fixture.PathInRoot(L"landed-two.git");
  rig.UseA();
  const std::wstring writtenTwo = fixture.PathInRoot(L"written-two.git");

  fixture.WriteFile(L"rewritten-multi.txt", "两个都被改写\n");
  fixture.StageAll();
  fixture.Commit(L"两个发布目标都过 pushInsteadOf 的那一条");
  // 没有 pushurl：推送目标落回 url 清单（本机实测：`--push --all` 会把 pushInsteadOf
  // 逐条叠在落回 url 的场合，抓取方向不受 pushInsteadOf 影响）。
  fixture.RunCheckedInRepo({L"config", L"--add", L"remote.origin.url", writtenTwo});
  fixture.RunCheckedInRepo({L"config", L"url." + landedOne + L".pushInsteadOf", originUrl});
  fixture.RunCheckedInRepo({L"config", L"url." + landedTwo + L".pushInsteadOf", writtenTwo});

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK_MESSAGE(facts.pushUrls.size() == 2,
                   "两个发布地址都要逐条展开：" + ToUtf8(gc::git::FormatPushUrlList(facts.pushUrls)));
  GC_CHECK(facts.pushUrlRewritten);
  GC_CHECK_MESSAGE(facts.pushUrls.size() == 2 && facts.pushUrls[0] == landedOne &&
                       facts.pushUrls[1] == landedTwo,
                   "展开结果的顺序或内容不对：" + ToUtf8(gc::git::FormatPushUrlList(facts.pushUrls)));
  GC_CHECK(!facts.pushTargetIsFetchTarget);
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);  // 多目标 + 与抓取那侧不同处

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  // 推送真正落到的，必须就是界面上展开、事后要核实的那一批地址；其中一个原样地址（抓取的 bare）
  // 反而不该收到——这同时钉住「push 方向按 pushInsteadOf 改写、展示与核实跟着改写后的走」。
  for (const std::wstring& url : plan.pushUrls) {
    GC_CHECK_MESSAGE(BareLocalRef(fixture, url, L"refs/heads/main") == plan.pushedObjectId,
                     "展开列出的目标没有收到：" + ToUtf8(url));
  }
  GC_CHECK(BareLocalRef(fixture, writtenTwo, L"refs/heads/main").empty());  // 那地址根本不存在
  const PushVerificationReport report = Verify(fixture, plan, true, L"执行成功");
  GC_CHECK_MESSAGE(report.verdict == PushVerificationVerdict::confirmed, ToUtf8(report.headline));
  GC_CHECK_MESSAGE(report.lines.size() == 2, "逐目标各有一条结论");
  ExpectNoGlobalConfig(fixture);
}

// ---- 源侧钉死：确认之后分支被外部推进，送出去的仍是那一份 ----

GC_TEST(push_sends_the_pinned_object_id_even_if_the_branch_advances_afterward) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  fixture.WriteFile(L"pinned-first.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"预检时确认的那一条");
  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  const std::wstring pinned = plan.pushedObjectId;

  // 外部（另一个终端）在点头之后又把分支推进了一条。界面层会在执行前复核时作废旧方案；
  // 这条用例钉的是**最后一道防线**：即便极端竞态越过复核，命令源侧写死的是 pinned，
  // Git 送出去的也只能是它——新那一条留在本地，远端不会替用户做决定。
  fixture.WriteFile(L"pinned-second.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"预检之后外部推进的那一条");
  GC_CHECK_MESSAGE(fixture.HeadSha() != pinned, "夹具没能让分支真的前进");

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == pinned);
  // 核实按「被确认的那一份」对照：已核实，而不是把新 HEAD 混进来。
  const PushVerificationReport report = Verify(fixture, plan, true, L"执行成功");
  GC_CHECK_MESSAGE(report.verdict == PushVerificationVerdict::confirmed, ToUtf8(report.headline));
  ExpectNoGlobalConfig(fixture);
}

// ---- 对象 ID 源形态不牺牲别的承诺：pre-push 照常执行、钩子看得见目标与那一份提交 ----

GC_TEST(push_object_id_source_still_runs_pre_push_hook_with_the_same_facts) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  // 记录型 pre-push hook：不拦截，只把 argv 与 stdin 的逐行内容落到仓库根的文件里，
  // 用例直接读那份文件——「没有 --no-verify，钩子照常生效」从口径变成可核对的证据。
  fixture.WriteFile(L".git/hooks/pre-push",
                    "#!/bin/sh\n"
                    "echo \"args $1 $2\" >> pre-push-input.txt\n"
                    "cat >> pre-push-input.txt\n"
                    "exit 0\n");
  fixture.RunCheckedInRepo({L"config", L"core.hooksPath", L".git/hooks"});

  fixture.WriteFile(L"hooked.txt", "钩子要看的那一条\n");
  fixture.StageAll();
  fixture.Commit(L"钩子现场的那一条");
  const PushPlan plan = gc::git::BuildPushPlan(Probe(fixture), fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  for (const std::wstring& argument : plan.arguments) {
    GC_CHECK(!Contains(argument, L"--no-verify"));
  }

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  std::ifstream input(ToUtf8(fixture.RepoDir()) + "\\pre-push-input.txt", std::ios::binary);
  const std::string capturedBytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
  const std::wstring captured = gc::platform::Utf8ToUtf16(capturedBytes);
  GC_CHECK_MESSAGE(!captured.empty(), "对象 ID 作源侧时 pre-push 没有执行——钩子承诺失效了");
  // 钩子必须看得见：目标远端名、目标引用、以及「推的就是被钉住的那份提交」。
  GC_CHECK(Contains(captured, L"origin"));
  GC_CHECK(Contains(captured, plan.remoteBranchRef));
  GC_CHECK_MESSAGE(Contains(captured, plan.pushedObjectId),
                   "钩子的输入里没有那份提交 ID：" + ToUtf8(captured));
  // 落点与跟踪引用：对端确实到了那一份；本地跟踪引用是否随推送前移由 Git 回答，
  // 这里的断言把「用对象 ID 不削弱跟踪引用更新」钉住（若 Git 在该形态下不前移，本条会红，
  // 需要改的是承诺口径而不是悄悄放行——见 PROGRESS 的剩余风险）。
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == plan.pushedObjectId);
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == plan.pushedObjectId);
  ExpectNoGlobalConfig(fixture);
}

// ---- 省略取值的布尔裸键：整份配置照读、按真值中和（旧实现会把预检整体判死） ----

GC_TEST(push_valueless_boolean_mirror_config_is_understood_and_neutralized) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();

  fixture.WriteFile(L"bare-bool.txt", "裸键 mirror\n");
  fixture.StageAll();
  fixture.Commit(L"带布尔裸键配置的那一条");

  // 配置里 `mirror` 不写 `= `——Git 存储与 `--list --null` 输出的就是 `remote.origin.mirror\0`
  // 这种省略取值的记录（本机实测）。`git config` 的命令行没法治出这种形态，直接在夹具
  // 自有的 .git/config 里插那一行（只动这个测试自己认领的目录）。
  const std::string configPath = ToUtf8(fixture.RepoDir()) + "\\.git\\config";
  {
    std::ifstream file(configPath, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string marker = "[remote \"origin\"]";
    const size_t at = contents.find(marker);
    GC_REQUIRE_MESSAGE(at != std::string::npos, "夹具配置里没有 [remote \"origin\"] 段");
    contents.insert(at + marker.size(), "\n\tmirror");
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    out << contents;
  }

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_REQUIRE_MESSAGE(facts.configOk, ToUtf8(facts.config.readFailure));
  GC_CHECK_MESSAGE(facts.mirrorConfigured,
                   "省略取值的 remote.origin.mirror 没被读成真值（旧缺陷回归钉）");

  // 先证明这条裸键设置确实生效：不带中和的同一形态会被 Git 按 mirror 规则拒绝。
  const GitRun raw = fixture.Run({L"push", L"--recurse-submodules=no", L"origin",
                                  L"refs/heads/main:refs/heads/main"}, fixture.RepoDir());
  GC_CHECK_MESSAGE(!raw.Success(), "mirror 裸键没生效？该场合裸 push 本该失败");

  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(),
                    L"remote.origin.mirror=false") != plan.arguments.end());
  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK(RemoteRef(fixture, originUrl, L"refs/heads/main") == plan.pushedObjectId);
  ExpectNoGlobalConfig(fixture);
}

// ---- 无效本地目标：命令失败，核实不谎报 ----

GC_TEST(push_to_invalid_local_target_fails_and_verification_stays_honest) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  // 根内一个从没建出来的目录：URL 合法、远端也在清单里，可那个地方根本不是一个 Git 仓库。
  const std::wstring ghostUrl = fixture.PathInRoot(L"never-created.git");

  fixture.WriteFile(L"ghost.txt", "发不出去\n");
  fixture.StageAll();
  fixture.Commit(L"要推给不存在目标的那一条");
  fixture.AddRemote(L"ghost", ghostUrl);  // 走夹具的本地远端守卫
  fixture.RunCheckedInRepo({L"config", L"branch.main.pushRemote", L"ghost"});

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.pushRemoteName == L"ghost");
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);  // 发布目标与抓取的那一侧不同处，本来就要明确点头

  const std::wstring localHead = fixture.HeadSha();
  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(!pushed.Success(), "推到一个不存在的地方居然成功了");
  GC_CHECK(fixture.HeadSha() == localHead);
  // 抓取的远端没有被动过：这次失败没有波及原有目标。
  GC_CHECK(RemoteRef(fixture, rig.OriginUrl(), L"refs/heads/main") != localHead);

  const PushVerificationReport report = Verify(fixture, plan, false, L"退出码非 0");
  GC_CHECK(report.verdict == PushVerificationVerdict::nothingChecked);
  GC_CHECK(!Contains(report.headline, L"已核实"));
  GC_CHECK_MESSAGE(Contains(report.headline, L"只能以命令窗口里 Git 的真实输出为准"),
                   ToUtf8(report.headline));
  ExpectNoGlobalConfig(fixture);
}

// ---- 复核：点头之后分支被外部挪走，旧方案作废 ----

GC_TEST(push_execution_recheck_uses_real_facts_and_discards_stale_plan) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  fixture.WriteFile(L"recheck.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"预检时看到的那一条");
  const PushPreflightFacts preflight = Probe(fixture);
  GC_REQUIRE_MESSAGE(preflight.queryOk, ToUtf8(preflight.queryFailure));
  GC_CHECK(gc::git::DescribePushChange(preflight, preflight).empty());  // 同一份事实：一致

  // 外部（另一个终端）在点头之后又提交了一条：要推的东西已经不是预检那一份。
  fixture.WriteFile(L"later.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"预检之后外部又提交的一条");
  const PushPreflightFacts latest = Probe(fixture);
  const std::wstring change = gc::git::DescribePushChange(preflight, latest);
  GC_CHECK_MESSAGE(!change.empty(), "外部把分支挪走了，复核却说什么也没变");
  GC_CHECK(Contains(change, L"要推送的提交"));
  GC_CHECK(Contains(change, L"没有发出任何命令"));

  // 工作区的改动不该让一次合法的推送作废：推送只送已有的提交，根本不碰工作区。
  fixture.WriteFile(L"uncommitted.txt", "还没暂存的改动\n");
  const PushPreflightFacts dirty = Probe(fixture);
  GC_CHECK_MESSAGE(gc::git::DescribePushChange(latest, dirty).empty(),
                   "只是工作区多了个未跟踪文件，不该作废这次的推送方案");
  fixture.RunCheckedInRepo({L"clean", L"-fd"});  // 用例自己收尾，不留垃圾给临时目录销毁
  ExpectNoGlobalConfig(fixture);
}

// ---- 中文与特殊字符的分支名：引用名原样进 refspec，不被改写 ----

GC_TEST(push_carries_non_ascii_branch_names_unchanged) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  const std::wstring originUrl = rig.OriginUrl();
  // 分支名里带中文、`%`、`&`、`+`、`#` 这些在 shell/cmd 里有特殊含义的字符（Git 都允许）。
  const std::wstring branchName = L"功能/分支-名_100%&+#";
  const std::wstring branchRef = L"refs/heads/" + branchName;

  fixture.RunCheckedInRepo({L"checkout", L"-b", branchName});
  fixture.WriteFile(L"中文路径 与&特殊%字符.txt", "非 ASCII 分支\n");
  fixture.StageAll();
  fixture.Commit(L"非 ASCII 分支上的第一条");
  // 夹具的 -u 只是搭台（产品路径从不 --set-upstream）：把上游建立起来。
  fixture.Push(L"origin", branchName, /*setUpstream=*/true);

  fixture.WriteFile(L"second.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"待推送的第二条");

  const PushPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.branchName == branchName);
  GC_CHECK(facts.branchRef == branchRef);
  GC_CHECK(facts.upstreamRemoteRef == branchRef);  // Git 答出来的远端引用也是同一个名字
  const PushPlan plan = gc::git::BuildPushPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == PushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK_MESSAGE(plan.arguments.back() == plan.pushedObjectId + L":" + branchRef,
                   "refspec 的目标侧（或钉住的提交 ID）被改写了：" + ToUtf8(plan.arguments.back()));
  GC_CHECK(Contains(plan.confirmationText, branchRef));
  GC_CHECK(Contains(plan.commandLabel, plan.pushedObjectId));

  const GitRun pushed = ExecutePlan(fixture, plan);
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(pushed.err));
  GC_CHECK(BareLocalRef(fixture, originUrl, branchRef) == plan.pushedObjectId);
  // 只推了这一条：main 仍在夹具建台时的那一份提交上。
  GC_CHECK(BareLocalRef(fixture, originUrl, L"refs/heads/main") != plan.pushedObjectId);
  ExpectNoGlobalConfig(fixture);
}
