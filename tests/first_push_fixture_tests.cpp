// 「首次推送」的真实 Git 集成测试：全部在夹具自己创建并认领所有权的临时目录里，用真实 Git
// 驱动生产编排（platform::CollectFirstPushTargets / CollectFirstPushProbe）与生产方案层
// （git::BuildFirstPushPlan），再把方案合成的那条 push 与那两条 git config **原样**交给真实 Git 执行，
// 然后拿 bare 远端与仓库配置文件里的实际状态验结论。远端只有本机临时根里的 bare 仓库，
// 全程不联网（夹具环境写死 GIT_ALLOW_PROTOCOL=file，任何网络 URL 在发起连接前就被 Git 拒绝）。
//
// 分组口径（运行器按文件把未列入只读名单的用例归入 git-mutating）：
//   * 只读那两条只发 check-ref-format / rev-parse / ls-remote，不建提交、不推送；
//   * 其余用例会在自己的临时目录里 commit 与 push（含向 bare 的推送），一律交给维护者运行。
//
// 覆盖的场景：
//   * check-ref-format 的退出码契约与「不认 `--`」这条实测事实（本程序因此只交完整 refs/ 形态）；
//   * 发布目标的只读询问：对端「没有那条引用」与「问不到」是两种答案，措辞与字段都分得开；
//   * 候选清单：逐个远端按配置顺序展开实际发布地址；
//   * 非法引用名：Git 判不能用时，连一条对外的 ls-remote 都不发（按调用计数核实，不看文案）；
//   * 首次推送落地：bare 上新建那条引用、两条 git config 真的把上游写成 Git 自己认得的形态，
//     写完之后普通推送的预检就不再把它当「没有上游」；
//   * 对端已有那条引用且是快进：不带 --force 也能落地，且本地那份历史原样；
//   * 对端已有那条引用且不是快进：Git 拒绝，对端与本地都不动，而且**不写任何配置**；
//   * 上游写入的落点：extensions.worktreeConfig 开着时，两条 git config 仍写进 .git/config。
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "git/first_push_plan.h"
#include "git/push_plan.h"
#include "git/repository.h"
#include "platform/windows/push_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FirstPushCandidateFacts;
using gc::git::FirstPushFacts;
using gc::git::FirstPushPlan;
using gc::git::FirstPushPlanState;
using gc::git::FirstPushProbeQueries;
using gc::git::GitQueryResult;
using gc::git::PushPreflightFacts;
using gc::git::PushTargetCheck;
using gc::git::UpstreamWriteStep;
using gc::platform::FirstPushProbeRequest;
using gc::platform::FirstPushTargetsRequest;
using gc::platform::PushProbeDeps;
using gc::test::GitFixture;
using gc::test::GitRun;

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

// 用夹具的隔离执行器装配生产预检依赖：界面走的是同一条编排，只是子进程执行换成本夹具的环境。
PushProbeDeps MakeProbeDeps(GitFixture& fixture) {
  PushProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);  // 夹具固定使用自己验证过的 git.exe。
    return AsQuery(fixture.Run(arguments, directory));
  };
  return deps;
}

// 同上，但把每一次发出的参数都记下来：用来证明「引用名不合格时确实一条对外的询问都没发」，
// 靠调用计数，而不是靠文案里有没有那句话。
struct RecordingProbeDeps {
  PushProbeDeps deps;
  std::vector<std::vector<std::wstring>> calls;
};

RecordingProbeDeps MakeRecordingProbeDeps(GitFixture& fixture) {
  RecordingProbeDeps recording;
  recording.deps.runner = [&recording, &fixture](const std::wstring& exePath,
                                                 const std::wstring& directory,
                                                 const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);
    recording.calls.push_back(arguments);
    return AsQuery(fixture.Run(arguments, directory));
  };
  return recording;
}

bool AnyCallContains(const RecordingProbeDeps& recording, std::wstring_view needle) {
  for (const std::vector<std::wstring>& call : recording.calls) {
    for (const std::wstring& argument : call) {
      if (Contains(argument, needle)) {
        return true;
      }
    }
  }
  return false;
}

std::wstring AbsoluteGitDir(GitFixture& fixture) {
  return Trimmed(fixture.RunCheckedInRepo({L"rev-parse", L"--absolute-git-dir"}).out);
}

// bare 上那条引用的实际位置（问的是那个 bare 目录本身，不借任何本地引用说话）。
std::wstring BareLocalRef(GitFixture& fixture, const std::wstring& bareDirectory,
                          std::wstring_view ref) {
  const GitRun run = fixture.Run({L"--git-dir", bareDirectory, L"rev-parse", L"--verify", L"--quiet",
                                  std::wstring(ref)},
                                 fixture.RepoDir());
  return run.Success() ? Trimmed(run.out) : std::wstring();
}

// 问一次生效配置里的某个键（`--get` 未设置时以退出码 1 + 空输出回答）。
std::wstring ConfigValue(GitFixture& fixture, const std::wstring& key) {
  const GitRun run = fixture.RunInRepo({L"config", L"--get", key});
  return run.Success() ? Trimmed(run.out) : std::wstring();
}

void ExpectNoGlobalConfig(GitFixture& fixture) {
  GC_CHECK_MESSAGE(Trimmed(fixture.RunCheckedInRepo({L"config", L"--global", L"--list"}).out).empty(),
                   "首次推送把使用者的全局配置文件改了");
}

FirstPushProbeRequest MakeProbeRequest(GitFixture& fixture, const std::wstring& remoteName,
                                       const std::wstring& targetBranchRef) {
  FirstPushProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.remoteName = remoteName;
  request.targetBranchRef = targetBranchRef;
  request.timeoutMilliseconds = 30000;
  return request;
}

}  // namespace

// ---- 只读：查询形态与退出码契约（不建提交、不推送） ----

GC_TEST(first_push_ref_format_fixture_verdicts_match_the_contract) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");

  // 生产构造的那条查询：合格引用 → 0，不合格 → 1，两种都没有输出。
  // 这正是判读层把「退出码 1」当明确答案（而不是错误）的依据。
  const GitRun accepted = fixture.Run(
      gc::git::BuildFirstPushRefFormatArguments(fixture.RepoDir(), L"refs/heads/topic"),
      fixture.RepoDir());
  GC_CHECK(accepted.started && accepted.exited);
  GC_CHECK_MESSAGE(accepted.exitCode == 0, ToUtf8(accepted.err));
  GC_CHECK(Trimmed(accepted.out).empty());

  for (const std::wstring_view bad : {std::wstring_view(L"refs/heads/a..b"),
                                      std::wstring_view(L"refs/heads/topic.lock"),
                                      std::wstring_view(L"refs/heads/a b")}) {
    const std::wstring ref(bad);
    const GitRun rejected =
        fixture.Run(gc::git::BuildFirstPushRefFormatArguments(fixture.RepoDir(), ref),
                    fixture.RepoDir());
    GC_CHECK_MESSAGE(rejected.exited && rejected.exitCode == 1,
                     ToUtf8(ref + L" -> exit " + std::to_wstring(rejected.exitCode)));
    GC_CHECK(Trimmed(rejected.out).empty());
  }

  // 裸名字与 `--`：本程序从不这样发问（构造期就返回空参数），因为实测 check-ref-format
  // 不认 `--`，带 `-` 开头的写法会先以 129 用法错误死在它自己的参数解析上。
  GC_CHECK(gc::git::BuildFirstPushRefFormatArguments(fixture.RepoDir(), L"topic").empty());
  const GitRun dashDash =
      fixture.Run({L"check-ref-format", L"--", L"-oops"}, fixture.RepoDir());
  GC_CHECK(dashDash.exited);
  GC_CHECK_MESSAGE(dashDash.exitCode == 129,
                   ToUtf8(L"exit " + std::to_wstring(dashDash.exitCode)));

  // 配置文件的落点：主工作区里就是 .git/config（确认框那句「写到哪」用的正是这条回答）。
  const GitRun path =
      fixture.Run(gc::git::BuildFirstPushConfigPathArguments(fixture.RepoDir()), fixture.RepoDir());
  GC_CHECK(path.Success());
  GC_CHECK(Trimmed(path.out) == L".git/config");
}

GC_TEST(first_push_remote_probe_fixture_separates_absent_from_unreachable) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.InitBareRepository(L"lonely.git");
  const std::wstring lonelyUrl = fixture.PathInRoot(L"lonely.git");
  fixture.SetActiveRepository(L"repo");
  const std::wstring ghostUrl = fixture.PathInRoot(L"never-created.git");

  // 「问成功而对端没有那一行」与「问不到」必须落在不同字段上：判读层据此决定
  // 界面说的是「对端还没有这条分支」还是「没能问清对端有没有」。
  const GitQueryResult absent = AsQuery(
      fixture.Run(gc::git::BuildPushRemoteProbeArguments(fixture.RepoDir(), lonelyUrl,
                                                         L"refs/heads/topic"),
                  fixture.RepoDir()));
  const PushTargetCheck absentCheck =
      gc::git::InterpretPushTargetCheck(lonelyUrl, absent, L"refs/heads/topic");
  GC_CHECK(absentCheck.queried && absentCheck.ok);
  GC_CHECK(!absentCheck.refPresent && absentCheck.failure.empty());

  const GitQueryResult unreachable = AsQuery(
      fixture.Run(gc::git::BuildPushRemoteProbeArguments(fixture.RepoDir(), ghostUrl,
                                                         L"refs/heads/topic"),
                  fixture.RepoDir()));
  const PushTargetCheck unreachableCheck =
      gc::git::InterpretPushTargetCheck(ghostUrl, unreachable, L"refs/heads/topic");
  GC_CHECK(unreachableCheck.queried);
  GC_CHECK(!unreachableCheck.ok && !unreachableCheck.failure.empty());
  GC_CHECK(!unreachableCheck.refPresent);

  // 聚合规则同样分得开：一个问到、一个问不到 ⇒ 整体「问不到」，不是「对端没有」。
  const gc::git::FirstPushPresenceSummary mixed =
      gc::git::SummarizeFirstPushPresence({absentCheck, unreachableCheck});
  GC_CHECK(!mixed.known && !mixed.exists);
  GC_CHECK(Contains(mixed.detail, L"没能问到"));
  const gc::git::FirstPushPresenceSummary onlyAbsent =
      gc::git::SummarizeFirstPushPresence({absentCheck});
  GC_CHECK(onlyAbsent.known && !onlyAbsent.exists);
}

// ---- 候选清单与「非法引用名不发对外询问」 ----

GC_TEST(first_push_targets_fixture_lists_configured_remotes) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.InitBareRepository(L"one.git");
  fixture.InitBareRepository(L"two.git");
  const std::wstring oneUrl = fixture.PathInRoot(L"one.git");
  const std::wstring twoUrl = fixture.PathInRoot(L"two.git");
  fixture.SetActiveRepository(L"repo");
  fixture.AddRemote(L"origin", oneUrl);
  fixture.AddRemote(L"fork", twoUrl);
  // 一个「只有抓取地址」与一个「有 pushurl」的远端都在清单里，但发布地址各自按 Git 的回答来。
  fixture.AddRemote(L"pushonly", twoUrl);
  fixture.RunCheckedInRepo({L"config", L"remote.pushonly.pushurl", oneUrl});

  FirstPushTargetsRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const FirstPushCandidateFacts candidates =
      gc::platform::CollectFirstPushTargets(request, MakeProbeDeps(fixture));
  GC_CHECK(candidates.configOk);
  GC_CHECK(candidates.candidates.size() == 3);
  for (const gc::git::FirstPushRemoteCandidate& candidate : candidates.candidates) {
    GC_CHECK_MESSAGE(candidate.selectable(), ToUtf8(candidate.name + L"：" + candidate.failure));
  }
  if (candidates.candidates.size() != 3) {
    return;
  }
  // 展示栏里的地址必须是掩过凭据的形态；这里都是本机路径，原样即可。
  GC_CHECK(Contains(candidates.candidates[0].urlsDisplay(), oneUrl));
  GC_CHECK(Contains(candidates.candidates[2].urlsDisplay(), oneUrl));  // pushurl 优先于 url
}

GC_TEST(first_push_probe_fixture_sends_no_remote_query_for_rejected_ref_name) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.InitBareRepository(L"lonely.git");
  fixture.SetActiveRepository(L"repo");
  fixture.AddRemote(L"origin", fixture.PathInRoot(L"lonely.git"));
  fixture.WriteFile(L"only.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"还没有远端的提交");

  // 引用名被 Git 判不能用：连一条 ls-remote 都不发（按调用计数核实）。
  RecordingProbeDeps recording = MakeRecordingProbeDeps(fixture);
  const FirstPushFacts facts = gc::platform::CollectFirstPushProbe(
      MakeProbeRequest(fixture, L"origin", L"refs/heads/a..b"), recording.deps);
  GC_CHECK(facts.queryOk);
  GC_CHECK(facts.refFormatRan && !facts.refFormatAccepted);
  GC_CHECK(facts.presence.empty());
  GC_CHECK(AnyCallContains(recording, L"check-ref-format"));
  GC_CHECK(!AnyCallContains(recording, L"ls-remote"));
  const FirstPushPlan plan =
      gc::git::BuildFirstPushPlan({facts, fixture.RepoDir(), true});
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::blocked, ToUtf8(plan.explanation));
  GC_CHECK(plan.arguments.empty() && plan.upstreamSteps.empty());

  // 合格的引用名才会走到对外询问那一步。
  RecordingProbeDeps acceptedRun = MakeRecordingProbeDeps(fixture);
  const FirstPushFacts goodFacts = gc::platform::CollectFirstPushProbe(
      MakeProbeRequest(fixture, L"origin", L"refs/heads/fresh"), acceptedRun.deps);
  GC_CHECK(goodFacts.refFormatAccepted);
  GC_CHECK(goodFacts.presence.size() == 1);
  GC_CHECK(AnyCallContains(acceptedRun, L"ls-remote"));
}

// ---- 落地：首次推送 + 两条 git config ----

GC_TEST(first_push_full_chain_lands_on_bare_and_two_config_steps_set_upstream) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.InitBareRepository(L"lonely.git");
  const std::wstring lonelyUrl = fixture.PathInRoot(L"lonely.git");
  fixture.SetActiveRepository(L"repo");
  fixture.AddRemote(L"origin", lonelyUrl);
  fixture.WriteFile(L"only.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"还没有上游的提交");
  const std::wstring localHead = fixture.HeadSha();
  GC_REQUIRE(!localHead.empty(), "夹具的 HEAD 没能问出提交 ID");

  // 向导第一步的候选清单：这个仓库此刻唯一的远端就是那个 bare。
  FirstPushTargetsRequest targetsRequest;
  targetsRequest.exePath = fixture.GitExe();
  targetsRequest.repositoryDirectory = fixture.RepoDir();
  targetsRequest.timeoutMilliseconds = 20000;
  const FirstPushCandidateFacts candidates =
      gc::platform::CollectFirstPushTargets(targetsRequest, MakeProbeDeps(fixture));
  GC_CHECK(candidates.candidates.size() == 1 && candidates.candidates[0].selectable());

  // 向导第二步的目标预检：本地这条分支确实没有上游，bare 上也还没有那条引用。
  const PushProbeDeps deps = MakeProbeDeps(fixture);
  FirstPushFacts facts =
      gc::platform::CollectFirstPushProbe(MakeProbeRequest(fixture, L"origin", L"refs/heads/fresh"),
                                         deps);
  GC_CHECK_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.branchName == L"main" && !facts.upstreamConfigured);
  GC_CHECK(facts.refFormatAccepted);
  GC_CHECK(facts.publishUrls.size() == 1 && facts.publishUrls[0] == lonelyUrl);
  GC_CHECK(facts.presenceKnown && !facts.remoteRefExists);
  GC_CHECK(facts.configFilePath == L".git/config");

  FirstPushPlan plan = gc::git::BuildFirstPushPlan({facts, fixture.RepoDir(), true});
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::ready, ToUtf8(plan.explanation));
  GC_REQUIRE(plan.arguments.size() >= 3, "方案没构造出推送命令");
  GC_REQUIRE(plan.upstreamSteps.size() == 2, "方案没构造出两条上游写入");
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"--force") ==
           plan.arguments.end());
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"--set-upstream") ==
           plan.arguments.end());

  // 第 1 步：那条 push 原样交给真实 Git（界面里它在命令窗口跑，参数数组是同一份）。
  const GitRun pushed = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(L"push exit=" + std::to_wstring(pushed.exitCode) +
                                             L" " + pushed.err));
  GC_CHECK(BareLocalRef(fixture, lonelyUrl, L"refs/heads/fresh") == localHead);

  // 第 2 步：两条 git config 各自一条、各自成功。
  for (const UpstreamWriteStep& step : plan.upstreamSteps) {
    const GitRun written = fixture.Run(step.arguments, fixture.RepoDir());
    GC_CHECK_MESSAGE(written.Success(), ToUtf8(step.commandLabel + L" exit=" +
                                                 std::to_wstring(written.exitCode) + L" " +
                                                 written.err));
  }
  GC_CHECK(ConfigValue(fixture, L"branch.main.remote") == L"origin");
  GC_CHECK(ConfigValue(fixture, L"branch.main.merge") == L"refs/heads/fresh");

  // Git 自己认得这个上游了：%(upstream:remotename) 答的就是刚写入的那个远端。
  const GitRun upstream = fixture.RunInRepo(
      {L"for-each-ref", L"--format=%(upstream:remotename)\t%(upstream)\t%(upstream:remoteref)",
       L"refs/heads/main"});
  GC_CHECK(upstream.Success());
  GC_CHECK(Contains(upstream.out, L"origin") && Contains(upstream.out, L"refs/heads/fresh"));

  // 上游写好了：普通推送的预检不再把它当「没有上游」，向导也就不会再被提起。
  PushPreflightFacts afterPush;
  {
    gc::platform::PushProbeRequest probeRequest;
    probeRequest.exePath = fixture.GitExe();
    probeRequest.repositoryDirectory = fixture.RepoDir();
    probeRequest.absoluteGitDir = AbsoluteGitDir(fixture);
    probeRequest.timeoutMilliseconds = 20000;
    afterPush = gc::platform::CollectPushPreflight(probeRequest, deps);
  }
  GC_CHECK(afterPush.queryOk && afterPush.upstreamConfigured);
  GC_CHECK(!gc::git::CanOfferFirstPush(afterPush));

  // bare 上只多出被点名的那一条引用；用户层的全局配置一个字没动。
  const GitRun listed = fixture.Run({L"ls-remote", lonelyUrl}, fixture.RepoDir());
  GC_CHECK(listed.Success());
  GC_CHECK(Contains(listed.out, L"refs/heads/fresh"));
  GC_CHECK(!Contains(listed.out, L"refs/tags/"));
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(first_push_onto_existing_remote_branch_fast_forwards_without_force) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.InitBareRepository(L"lonely.git");
  const std::wstring lonelyUrl = fixture.PathInRoot(L"lonely.git");
  fixture.SetActiveRepository(L"repo");
  fixture.AddRemote(L"origin", lonelyUrl);
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"第一条");
  const std::wstring firstCommit = fixture.HeadSha();
  // 先把对端那条引用建出来（本地 main 之后还会往前走）。
  const GitRun seeded =
      fixture.Run({L"push", L"origin", L"main:refs/heads/fresh"}, fixture.RepoDir());
  GC_CHECK_MESSAGE(seeded.Success(), ToUtf8(seeded.err));

  fixture.WriteFile(L"a.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"第二条");
  const std::wstring secondCommit = fixture.HeadSha();

  const FirstPushFacts facts = gc::platform::CollectFirstPushProbe(
      MakeProbeRequest(fixture, L"origin", L"refs/heads/fresh"), MakeProbeDeps(fixture));
  GC_CHECK(facts.presenceKnown && facts.remoteRefExists);
  GC_CHECK(facts.remoteObjectId == firstCommit);
  // 对端那份本地读得到：关系查询按「对端那份...这次要推的那份」发出去，左=只有对端有=0。
  GC_CHECK(facts.relationshipKnown);
  GC_CHECK(facts.remoteOnly == 0 && facts.localOnly == 1);

  const FirstPushPlan plan = gc::git::BuildFirstPushPlan({facts, fixture.RepoDir(), false});
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);  // 对端已有那条引用：必须明确点头
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"--force") ==
           plan.arguments.end());
  const GitRun pushed = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_CHECK_MESSAGE(pushed.Success(), ToUtf8(L"exit=" + std::to_wstring(pushed.exitCode) + pushed.err));
  GC_CHECK(BareLocalRef(fixture, lonelyUrl, L"refs/heads/fresh") == secondCommit);
  // 没选「设置上游」：配置一个字都不写。
  GC_CHECK(ConfigValue(fixture, L"branch.main.remote").empty());
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(first_push_non_fast_forward_is_rejected_and_writes_no_config) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.InitBareRepository(L"lonely.git");
  const std::wstring lonelyUrl = fixture.PathInRoot(L"lonely.git");
  fixture.SetActiveRepository(L"repo");
  fixture.AddRemote(L"origin", lonelyUrl);
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"共同的第一条");
  const GitRun seeded =
      fixture.Run({L"push", L"origin", L"main:refs/heads/fresh"}, fixture.RepoDir());
  GC_CHECK_MESSAGE(seeded.Success(), ToUtf8(seeded.err));

  // 另一个工作区把对端那条引用往前推：本地这边根本没见过那个提交。
  fixture.CloneRepository(lonelyUrl, L"other");
  fixture.WriteFile(L"b.txt", "9\n");
  fixture.StageAll();
  fixture.Commit(L"只有那边有的提交");
  const std::wstring otherCommit = fixture.HeadSha();
  const GitRun fromOther =
      fixture.Run({L"push", L"origin", L"HEAD:refs/heads/fresh"}, fixture.RepoDir());
  GC_CHECK_MESSAGE(fromOther.Success(), ToUtf8(fromOther.err));

  // 回到第一个工作区，并往前再走一步：两边就此分叉。
  fixture.SetActiveRepository(L"repo");
  fixture.WriteFile(L"a.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"本地第二条");
  const std::wstring localHead = fixture.HeadSha();

  const FirstPushFacts facts = gc::platform::CollectFirstPushProbe(
      MakeProbeRequest(fixture, L"origin", L"refs/heads/fresh"), MakeProbeDeps(fixture));
  GC_CHECK(facts.presenceKnown && facts.remoteRefExists);
  GC_CHECK(facts.remoteObjectId == otherCommit);
  // 对端那份不在本地：rev-list 死在自己的解析上，结论只能是「判断不了」，不是 0、也不是「会被拒」。
  GC_CHECK(!facts.relationshipKnown);

  const FirstPushPlan plan = gc::git::BuildFirstPushPlan({facts, fixture.RepoDir(), true});
  GC_CHECK_MESSAGE(plan.state == FirstPushPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(plan.upstreamSteps.size() == 2);
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"--force") ==
           plan.arguments.end());

  // 非快进：Git 自己拒绝。界面在这一步之后不会发那两条 git config（推送没成功就不写配置）。
  const GitRun pushed = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_CHECK(pushed.exited);
  GC_CHECK_MESSAGE(pushed.exitCode != 0, ToUtf8(L"这条非快进的推送竟然成功了：" + pushed.out));
  GC_CHECK(BareLocalRef(fixture, lonelyUrl, L"refs/heads/fresh") == otherCommit);
  GC_CHECK(fixture.HeadSha() == localHead);
  GC_CHECK(ConfigValue(fixture, L"branch.main.remote").empty());
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(first_push_config_write_steps_stay_in_local_config_with_worktree_config) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  // extensions.worktreeConfig 开着时，普通 `git config`（无 --worktree）的写入点仍是 .git/config：
  // 确认框那句「写到哪个文件」因此可以直接用 git rev-parse --git-path config 的回答。
  fixture.RunCheckedInRepo({L"config", L"extensions.worktreeConfig", L"true"});
  const std::vector<UpstreamWriteStep> steps =
      gc::git::BuildUpstreamWriteSteps(L"main", L"origin", L"refs/heads/fresh");
  GC_REQUIRE(steps.size() == 2, "上游写入的两条命令没构造出来");
  for (const UpstreamWriteStep& step : steps) {
    const GitRun written = fixture.Run(step.arguments, fixture.RepoDir());
    GC_CHECK_MESSAGE(written.Success(), ToUtf8(step.commandLabel + L" exit=" +
                                                 std::to_wstring(written.exitCode) + L" " +
                                                 written.err));
  }
  const std::wstring gitDirectory = AbsoluteGitDir(fixture);
  std::wifstream commonConfig(gitDirectory + L"\\config");
  std::wstring commonText;
  for (std::wstring line; std::getline(commonConfig, line);) {
    commonText += line;
  }
  GC_CHECK(Contains(commonText, L"[branch \"main\"]"));
  GC_CHECK(Contains(commonText, L"remote = origin"));
  GC_CHECK(Contains(commonText, L"merge = refs/heads/fresh"));
  GC_CHECK(ConfigValue(fixture, L"branch.main.merge") == L"refs/heads/fresh");
  ExpectNoGlobalConfig(fixture);
}
