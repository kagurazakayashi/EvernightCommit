// pull 的集成测试：全部在夹具自己创建并认领所有权的临时目录里，用真实 Git 驱动生产编排
// （platform::CollectPullTarget / CollectPullRelationship）与生产方案层
// （git::BuildPullFetchPlan / BuildPullIntegratePlan），再把方案合成的命令原样交给真实 Git 执行。
//
// 覆盖的关系与场景（A/B 两个本地工作区 + 同根下的 bare origin，全程不联网）：
//   * 无上游：阶段一判读如实 blocked，不产生命令、不写 branch.<名>.remote、也不凭空造出跟踪引用；
//   * 已一致 / 本地领先：nothingToIntegrate，一条命令都不给；
//   * 可快进：命令钉在刚抓回来的那一份提交（完整 ID），执行后只有分支引用前移；
//   * 无冲突分叉：配置没定策略时给两个候选（合并排第一）；选合并后真的产生一次合并提交；
//   * 文本冲突：merge-tree 预演点名冲突文件，且预检本身一字不改（HEAD/索引/工作区/流程痕迹全原样）；
//     真跑一次合并确实留下冲突现场，未合并清单读回同一份文件名；
//     用例自己的收尾用 git merge --abort——产品代码从不替用户 abort；
//   * 本地未提交改动重叠 / 未跟踪文件撞名：预检列出受影响文件并要求明确继续；
//     真跑时 Git 的拒绝与预检结论一致，本地未提交的内容一字未动；
//   * 已配置 pull.rebase=true：分叉时不问策略、直接给变基命令，执行后历史是线性的；
//   * 取消整合：那次可见的 fetch 只留下「远端跟踪引用前移」，本地分支/索引/工作区不动；
//   * 执行前复核：外部把分支挪走时 DescribePullChange 给出原因（旧方案作废）；
//   * 整条链路跑完后，夹具那份「用户层」配置档案仍然是空的——本程序从不写配置。
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "git/pull_plan.h"
#include "git/repository.h"
#include "platform/windows/pull_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::GitQueryResult;
using gc::git::PullFetchPlan;
using gc::git::PullFetchPlanState;
using gc::git::PullIntegratePlan;
using gc::git::PullIntegratePlanInput;
using gc::git::PullIntegrateStrategy;
using gc::git::PullMergeDryRun;
using gc::git::PullPlanState;
using gc::git::PullRelationship;
using gc::git::PullStrategyChoice;
using gc::git::PullTargetFacts;
using gc::platform::PullProbeDeps;
using gc::platform::PullProbeOutcome;
using gc::platform::PullProbeRequest;
using gc::test::GitFixture;
using gc::test::GitRun;
using gc::test::RemoteRig;

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool ListContains(const std::vector<std::wstring>& lines, std::wstring_view needle) {
  for (const std::wstring& line : lines) {
    if (line == needle) {
      return true;
    }
  }
  return false;
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

GitQueryResult NotRun() { return GitQueryResult{}; }  // started=false：这条根本没发

// 用夹具的隔离执行器装配生产预检依赖：查询走与界面完全相同的代码路径
// （同一套参数构造、同一套判读、同一条隐藏子进程执行链）。
PullProbeDeps MakePullDeps(GitFixture& fixture) {
  PullProbeDeps deps;
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

PullProbeRequest MakeRequest(GitFixture& fixture, bool includeRelationship) {
  PullProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.absoluteGitDir = AbsoluteGitDir(fixture);
  request.timeoutMilliseconds = 20000;
  request.includeRelationship = includeRelationship;
  return request;
}

// 完整跑一遍生产编排：阶段一（+ 需要时阶段二）。界面用的就是这同一个入口。
PullProbeOutcome Probe(GitFixture& fixture, bool includeRelationship) {
  const PullProbeRequest request = MakeRequest(fixture, includeRelationship);
  const PullProbeDeps deps = MakePullDeps(fixture);
  PullProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;
  outcome.target = gc::platform::CollectPullTarget(request, deps);
  if (includeRelationship) {
    outcome.relationship = gc::platform::CollectPullRelationship(outcome.target, request, deps);
  }
  return outcome;
}

PullTargetFacts ProbeTarget(GitFixture& fixture) { return Probe(fixture, false).target; }

PullIntegratePlanInput IntegrateInputFrom(const PullProbeOutcome& outcome,
                                          PullStrategyChoice choice = PullStrategyChoice::none) {
  PullIntegratePlanInput input;
  input.target = outcome.target;
  input.relationship = outcome.relationship;
  input.repositoryRoot = outcome.repositoryDirectory;
  input.choice = choice;
  return input;
}

// 分支位置、索引与现状的留档：取消、失败、预检这三条路都要求它一字不变。
struct RepoStamp {
  std::wstring head;
  std::wstring mainRef;
  std::wstring trackingRef;
  std::wstring indexListing;
  std::wstring statusLines;
  std::wstring mergeHead;  // 空 = 没有合并现场
};

RepoStamp Stamp(GitFixture& fixture) {
  RepoStamp stamp;
  stamp.head = fixture.HeadSha();
  stamp.mainRef = fixture.RevParseVerified(L"refs/heads/main");
  stamp.trackingRef = fixture.RevParseVerified(L"refs/remotes/origin/main");
  stamp.indexListing = fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;
  for (const std::wstring& line : fixture.StatusPorcelain()) {
    stamp.statusLines += line + L"\n";
  }
  stamp.mergeHead = Trimmed(
      fixture.Run({L"rev-parse", L"--verify", L"--quiet", L"MERGE_HEAD"}, fixture.RepoDir()).out);
  return stamp;
}

std::string StampDiff(const RepoStamp& before, const RepoStamp& after) {
  std::string text;
  if (before.head != after.head) {
    text += "HEAD " + ToUtf8(before.head) + "→" + ToUtf8(after.head) + "；";
  }
  if (before.mainRef != after.mainRef) {
    text += "refs/heads/main " + ToUtf8(before.mainRef) + "→" + ToUtf8(after.mainRef) + "；";
  }
  if (before.trackingRef != after.trackingRef) {
    text += "跟踪引用 " + ToUtf8(before.trackingRef) + "→" + ToUtf8(after.trackingRef) + "；";
  }
  if (before.indexListing != after.indexListing) {
    text += "索引变了；";
  }
  if (before.statusLines != after.statusLines) {
    text += "现状变了 [" + ToUtf8(before.statusLines) + "] vs [" + ToUtf8(after.statusLines) + "]；";
  }
  if (before.mergeHead != after.mergeHead) {
    text += "合并现场 " + ToUtf8(before.mergeHead) + "→" + ToUtf8(after.mergeHead) + "；";
  }
  return text;
}

std::wstring RiskText(const PullIntegratePlan& plan) {
  std::wstring text;
  for (const std::wstring& risk : plan.risks) {
    text += risk + L"\n";
  }
  return text;
}

// 执行方案合成的命令：与界面同一个形态——参数数组原样交给 git.exe，
// 只是界面那边外面套的是命令窗口，这里是夹具的隔离执行器。
GitRun ExecutePlan(GitFixture& fixture, const std::vector<std::wstring>& arguments) {
  return fixture.Run(arguments, fixture.RepoDir());
}

std::string ReadWorktreeFile(GitFixture& fixture, const std::wstring& absolutePath) {
  FILE* handle = nullptr;
  if (::fopen_s(&handle, ToUtf8(absolutePath).c_str(), "rb") != 0 || handle == nullptr) {
    return std::string();
  }
  std::string content;
  char buffer[256]{};
  for (size_t read = ::fread(buffer, 1, sizeof(buffer), handle); read > 0;
       read = ::fread(buffer, 1, sizeof(buffer), handle)) {
    content.append(buffer, read);
  }
  ::fclose(handle);
  return content;
}

// 让 A 与 B 的共同起点里就有这么一份「已被跟踪的文件」。
// 内容冲突与「未提交改动撞上带入路径」这两种场景都必须建立在「同一份已跟踪内容被两边各自改动」上，
// 而不是两边各自新增同名文件——那种场合 Git 报的是 add/add，未跟踪文件也另有其处理规则。
void SeedTrackedFile(RemoteRig& rig, std::wstring_view path, const std::string& content) {
  GitFixture& fixture = rig.fixture();
  rig.UseA();
  fixture.WriteFile(path, content);
  fixture.StageAll();
  fixture.Commit(L"把共有文件放进起点");
  fixture.Push(L"origin", L"main");
  rig.UseB();
  fixture.RunCheckedInRepo({L"fetch", L"origin"});
  fixture.RunCheckedInRepo({L"merge", L"--ff-only", L"origin/main"});
  rig.UseA();
}

}  // namespace

// ---- 无上游：拒绝，且不顺手改任何东西 ----

GC_TEST(pull_preflight_blocked_without_upstream_and_writes_nothing) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "A 的初始内容\n");
  fixture.StageAll();
  fixture.Commit(L"本地初始提交");
  // 有远端、但分支没有上游：fetch 那一步会把既有远端摆出来让人选，pull 不行——
  // 「整合哪一条远端分支」不能靠猜。
  fixture.AddRemote(L"origin", fixture.PathInRoot(L"never-pushed.git"));

  const PullTargetFacts facts = ProbeTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.onBranch && facts.branchName == L"main");
  GC_CHECK(facts.headResolved);
  GC_CHECK_MESSAGE(!facts.upstreamConfigured, "这个分支本来就没有上游");

  const PullFetchPlan plan = gc::git::BuildPullFetchPlan(facts, fixture.RepoDir());
  GC_CHECK_MESSAGE(plan.state == PullFetchPlanState::blocked, ToUtf8(plan.explanation));
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(Contains(plan.explanation, L"没有设置上游"));
  GC_CHECK(Contains(plan.explanation, L"不猜"));
  GC_CHECK(Contains(plan.explanation, L"--set-upstream"));  // 给出可操作的办法

  // 拒绝不等于顺手补配置：既没有 branch.main.remote，也没有凭空多出来的跟踪引用。
  GC_CHECK(Trimmed(fixture.Run({L"config", L"--get", L"branch.main.remote"}, fixture.RepoDir()).out)
               .empty());
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main").empty());
  // 「用户层」配置档案仍然是空的：本程序从不写配置。
  GC_CHECK(Trimmed(fixture.RunCheckedInRepo({L"config", L"--global", L"--list"}).out).empty());
}

// ---- 已一致 / 本地领先：无事可做，一条命令都不给 ----

GC_TEST(pull_up_to_date_and_local_ahead_produce_no_command) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  rig.UseA();

  const PullProbeOutcome same = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(same.target.queryOk, ToUtf8(same.target.queryFailure));
  GC_REQUIRE_MESSAGE(same.relationship.queryOk, ToUtf8(same.relationship.queryFailure));
  GC_CHECK(same.relationship.relationship == PullRelationship::upToDate);
  const PullIntegratePlan samePlan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(same));
  GC_CHECK(samePlan.state == PullPlanState::nothingToIntegrate);
  GC_CHECK(samePlan.arguments.empty());
  GC_CHECK(Contains(samePlan.explanation, L"已经一致"));

  // A 自己再提交一条：远端没有新东西，倒是本地领先——整合方向是反的，仍什么都不做。
  fixture.WriteFile(L"a-only.txt", "只有本地有\n");
  fixture.StageAll();
  fixture.Commit(L"A 的本地提交");
  const RepoStamp before = Stamp(fixture);
  const PullProbeOutcome ahead = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(ahead.relationship.queryOk, ToUtf8(ahead.relationship.queryFailure));
  GC_CHECK(ahead.relationship.relationship == PullRelationship::aheadOnly);
  GC_CHECK(ahead.relationship.ahead == 1 && ahead.relationship.behind == 0);
  const PullIntegratePlan aheadPlan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(ahead));
  GC_CHECK(aheadPlan.state == PullPlanState::nothingToIntegrate);
  GC_CHECK(aheadPlan.arguments.empty());
  GC_CHECK(Contains(aheadPlan.explanation, L"领先 1 个提交"));
  GC_CHECK(Contains(aheadPlan.explanation, L"push"));
  // 预检全程只读：什么都没动。
  GC_CHECK_MESSAGE(StampDiff(before, Stamp(fixture)).empty(),
                   StampDiff(before, Stamp(fixture)));
}

// ---- 可快进：命令钉在刚抓回来的那一份提交上 ----

GC_TEST(pull_fast_forward_integrates_the_exact_fetched_commit) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  // B 提交并推送；A 对此一无所知。
  rig.UseB();
  fixture.WriteFile(L"b.txt", "来自 B\n");
  fixture.StageAll();
  fixture.Commit(L"B 推上去的新提交");
  fixture.Push(L"origin", L"main");
  const std::wstring bSha = fixture.HeadSha();

  rig.UseA();
  const std::wstring headBefore = fixture.HeadSha();

  // 第一步：阶段一预检 + 那条可见的获取命令。
  const PullProbeOutcome target = Probe(fixture, false);
  GC_REQUIRE_MESSAGE(target.target.queryOk, ToUtf8(target.target.queryFailure));
  const PullFetchPlan fetchPlan = gc::git::BuildPullFetchPlan(target.target, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(fetchPlan.state == PullFetchPlanState::ready, ToUtf8(fetchPlan.explanation));
  GC_CHECK(fetchPlan.trackingRef == L"refs/remotes/origin/main");
  GC_CHECK(fetchPlan.remoteBranchRef == L"refs/heads/main");
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, fetchPlan.arguments).Success(), "获取应当成功");
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == bSha);
  GC_CHECK(fixture.HeadSha() == headBefore);  // 获取不动 HEAD、不动本地分支

  // 第二步：重读事实 → 判关系 → 出整合命令。
  const PullProbeOutcome outcome = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(outcome.relationship.queryOk, ToUtf8(outcome.relationship.queryFailure));
  GC_CHECK(outcome.relationship.relationship == PullRelationship::fastForward);
  GC_CHECK(outcome.relationship.behind == 1);
  GC_CHECK(ListContains(outcome.relationship.incomingPaths, L"b.txt"));
  // 可快进的场合内容不可能冲突：预演根本不该发出去。
  GC_CHECK(!outcome.relationship.dryRunRan);

  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(outcome));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::fastForward);
  // 命令里用的是那次获取读回来的完整 ID，不是「到时候再看引用停在哪儿」。
  GC_CHECK(plan.targetObjectId == bSha);
  GC_CHECK(plan.arguments.back() == bSha);
  GC_CHECK(!plan.requiresForce);
  // 远端 URL 是临时根内的本地路径：全程没有网络形态。
  GC_CHECK(fixture.RunCheckedInRepo({L"remote", L"-v"}).out.find(L"://") == std::wstring::npos);

  const GitRun integrated = ExecutePlan(fixture, plan.arguments);
  GC_REQUIRE_MESSAGE(integrated.Success(),
                     "快进应当成功；退出码 " + std::to_string(integrated.exitCode) + "：" +
                         ToUtf8(integrated.err));
  GC_CHECK(fixture.HeadSha() == bSha);
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == bSha);
  GC_CHECK(fixture.StatusPorcelain().empty());
  GC_CHECK(fixture.ParentShaOfHead() == headBefore);  // 快进：不产生合并提交
  GC_CHECK(fixture.ShowFileAtHead(L"b.txt") == "来自 B\n");
  // 夹具那份「用户层」配置档案依旧为空：整条链路一个字都没写进去。
  GC_CHECK(Trimmed(fixture.RunCheckedInRepo({L"config", L"--global", L"--list"}).out).empty());
}

// ---- 无冲突分叉：配置没定策略就问，选合并就真的合并 ----

GC_TEST(pull_diverged_without_config_asks_then_merges_when_choice_is_merge) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"b.txt", "来自 B\n");
  fixture.StageAll();
  fixture.Commit(L"B 的提交");
  fixture.Push(L"origin", L"main");
  const std::wstring bSha = fixture.HeadSha();

  rig.UseA();
  fixture.WriteFile(L"a.txt", "来自 A\n");
  fixture.StageAll();
  fixture.Commit(L"A 的提交");
  const std::wstring aSha = fixture.HeadSha();
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");

  const PullProbeOutcome outcome = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(outcome.relationship.queryOk, ToUtf8(outcome.relationship.queryFailure));
  GC_CHECK(outcome.relationship.relationship == PullRelationship::diverged);
  GC_CHECK(outcome.relationship.ahead == 1 && outcome.relationship.behind == 1);
  GC_CHECK(outcome.relationship.mergeBaseResolved);
  GC_CHECK(outcome.relationship.mergeBaseObjectId ==
           fixture.RevParseVerified(L"refs/remotes/origin/main~1"));

  // 配置里没写 pull.rebase：本程序不替用户定，也不去写配置——两个候选摆出来。
  const PullIntegratePlan asking = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(outcome));
  GC_CHECK(asking.state == PullPlanState::chooseStrategy);
  GC_CHECK_MESSAGE(asking.strategyCandidates.size() == 2,
                   "候选数：" + std::to_string(asking.strategyCandidates.size()));
  GC_CHECK(Contains(asking.strategyCandidates[0], L"合并"));
  GC_CHECK(Contains(asking.strategyCandidates[1], L"变基"));
  GC_CHECK(Contains(asking.explanation, L"不替你定"));

  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInputFrom(outcome, PullStrategyChoice::chooseMerge));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::merge);
  GC_CHECK(outcome.relationship.dryRun == PullMergeDryRun::supported_clean);
  GC_CHECK_MESSAGE(!plan.requiresForce, ToUtf8(RiskText(plan)));
  GC_CHECK(Contains(plan.strategySource, L"策略选择里点的「合并」"));

  const GitRun integrated = ExecutePlan(fixture, plan.arguments);
  GC_REQUIRE_MESSAGE(integrated.Success(),
                     "合并应当成功；退出码 " + std::to_string(integrated.exitCode) + "：" +
                         ToUtf8(integrated.err));
  const std::wstring merged = fixture.HeadSha();
  GC_CHECK(merged != aSha && merged != bSha);
  // 合并提交：两个父，一个来自本地、一个正是刚抓回来的那份远端提交。
  const std::wstring parents = Trimmed(
      fixture.RunCheckedInRepo({L"rev-list", L"--parents", L"-n", L"1", L"HEAD"}).out);
  GC_CHECK(Contains(parents, bSha));
  GC_CHECK(Contains(parents, aSha));
  GC_CHECK(Trimmed(fixture.RunCheckedInRepo({L"rev-list", L"--count", L"HEAD"}).out) == L"4");
  GC_CHECK(fixture.ShowFileAtHead(L"a.txt") == "来自 A\n");
  GC_CHECK(fixture.ShowFileAtHead(L"b.txt") == "来自 B\n");
  GC_CHECK(fixture.StatusPorcelain().empty());
}

// ---- 文本冲突：预演点名文件，预检本身一个字都不改 ----

GC_TEST(pull_predicted_text_conflict_leaves_repository_untouched) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  SeedTrackedFile(rig, L"f.txt", "第一行\n共同的起点\n第三行\n");
  GitFixture& fixture = rig.fixture();

  // 两边改同一份已被跟踪的文件、改的是同一行：这才是「文本内容冲突」。
  rig.UseB();
  fixture.WriteFile(L"f.txt", "第一行\nB 改的\n第三行\n");
  fixture.StageAll();
  fixture.Commit(L"B 改了第二行");
  fixture.Push(L"origin", L"main");

  rig.UseA();
  fixture.WriteFile(L"f.txt", "第一行\nA 改的\n第三行\n");
  fixture.StageAll();
  fixture.Commit(L"A 也改了第二行");
  const std::wstring aSha = fixture.HeadSha();
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");

  const RepoStamp before = Stamp(fixture);
  const PullProbeOutcome outcome = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(outcome.relationship.queryOk, ToUtf8(outcome.relationship.queryFailure));
  GC_CHECK(outcome.relationship.dryRun == PullMergeDryRun::supported_conflict);
  GC_CHECK(ListContains(outcome.relationship.dryRunConflicts, L"f.txt"));

  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(
      IntegrateInputFrom(outcome, PullStrategyChoice::chooseMerge));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK_MESSAGE(plan.requiresForce, "预告过的冲突必须走明确继续");
  GC_CHECK(Contains(RiskText(plan), L"f.txt"));
  GC_CHECK(Contains(RiskText(plan), L"merge-tree"));
  GC_CHECK(Contains(RiskText(plan), L"不会 abort"));

  // 预检只读：merge-tree --write-tree 只在对象库里留一个不可达的 tree，其余什么都不动。
  const RepoStamp afterPreflight = Stamp(fixture);
  GC_CHECK_MESSAGE(StampDiff(before, afterPreflight).empty(), StampDiff(before, afterPreflight));
  GC_CHECK(afterPreflight.mergeHead.empty());

  // 真跑一次：合并如预检所说留下冲突现场，分支引用一步没挪。
  const GitRun integrated = ExecutePlan(fixture, plan.arguments);
  GC_CHECK_MESSAGE(!integrated.Success() && integrated.exitCode != 0,
                   "有冲突的合并应当以非 0 收场，而不是被假装成功");
  GC_CHECK(fixture.HeadSha() == aSha);
  GC_CHECK(!Stamp(fixture).mergeHead.empty());  // MERGE_HEAD 还在：这正是「不擅自 abort」要保住的现场
  const std::vector<std::wstring> porcelain = fixture.StatusPorcelain();
  GC_CHECK_MESSAGE(!porcelain.empty(), "冲突条目要能在 status 里看见");
  bool showsUnmerged = false;
  for (const std::wstring& line : porcelain) {
    if (line.size() > 1 && (line[0] == L'U' || line[1] == L'U')) {
      showsUnmerged = true;
    }
  }
  GC_CHECK_MESSAGE(showsUnmerged, "status 里应有一条未合并（U）记录");

  // 未合并清单读回来的文件名与预演点名的那份一致。
  const GitRun listing =
      ExecutePlan(fixture, gc::git::BuildPullConflictListingArguments(fixture.RepoDir()));
  GC_REQUIRE_MESSAGE(listing.Success(), ToUtf8(listing.err));
  const gc::git::PullConflictState state =
      gc::git::InterpretPullConflictState(AsQuery(listing), NotRun(), NotRun());
  GC_REQUIRE_MESSAGE(state.readOk, ToUtf8(state.readFailure));
  GC_CHECK_MESSAGE(ListContains(state.conflictPaths, L"f.txt"),
                   "未合并清单里应有 f.txt，实际读到 " + std::to_string(state.conflictPaths.size()) +
                       " 条");

  // 用例自己的收尾：产品代码从不替用户 abort，这里为了清理现场自己来一次。
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"merge", L"--abort"}).Success(), "收尾用 abort 应当成功");
  GC_CHECK(fixture.HeadSha() == aSha);
  GC_CHECK(Stamp(fixture).mergeHead.empty());
  GC_CHECK(fixture.ShowFileAtHead(L"f.txt").find("A 改的") != std::string::npos);
}

// ---- 本地未提交改动与带入路径重叠：预检点名，Git 真的拒绝 ----

GC_TEST(pull_uncommitted_overlap_is_predicted_and_worktree_survives) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  SeedTrackedFile(rig, L"f.txt", "共同的起点\n");
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"f.txt", "远端改过的内容\n");
  fixture.StageAll();
  fixture.Commit(L"B 改了 f.txt");
  fixture.Push(L"origin", L"main");

  rig.UseA();
  // A 本地把同一个已跟踪文件改了但没提交——这正是「整合会撞上你没提交的东西」的形态。
  fixture.WriteFile(L"f.txt", "本地还没提交的内容\n");
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");
  // 现场留档放在获取之后：获取会让跟踪引用前移（那正是它的职责），预检则一个字节都不该动。
  const RepoStamp before = Stamp(fixture);
  GC_CHECK(before.statusLines != L"" );  // 未提交改动确实在现状里

  const PullProbeOutcome outcome = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(outcome.relationship.queryOk, ToUtf8(outcome.relationship.queryFailure));
  GC_CHECK(outcome.relationship.relationship == PullRelationship::fastForward);
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(outcome));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK_MESSAGE(plan.requiresForce, ToUtf8(RiskText(plan)));
  GC_CHECK(Contains(RiskText(plan), L"f.txt"));
  GC_CHECK(Contains(RiskText(plan), L"重叠"));
  GC_CHECK(Contains(RiskText(plan), L"stash"));  // 明确说不会替你 stash

  // 预检之后仓库还是预检之前的样子（预检不会先把改动藏起来）。
  GC_CHECK_MESSAGE(StampDiff(before, Stamp(fixture)).empty(), StampDiff(before, Stamp(fixture)));

  // 真跑一次：Git 的拒绝与预检的结论一致，而本地那份未提交的内容一字未动。
  const GitRun integrated = ExecutePlan(fixture, plan.arguments);
  GC_CHECK_MESSAGE(!integrated.Success(), "带着重叠改动快进应当被 Git 拒绝");
  const RepoStamp after = Stamp(fixture);
  GC_CHECK(after.head == before.head);
  GC_CHECK(after.mainRef == before.mainRef);
  GC_CHECK(after.indexListing == before.indexListing);
  GC_CHECK(after.statusLines == before.statusLines);
  GC_CHECK(ReadWorktreeFile(fixture, fixture.RepoDir() + L"\\f.txt").find("本地还没提交的内容") !=
           std::string::npos);
}

// ---- 未跟踪文件撞名：预检点名，Git 不会覆盖它 ----

GC_TEST(pull_untracked_collision_is_predicted_and_file_is_not_overwritten) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"notes.md", "远端带来的笔记\n");
  fixture.StageAll();
  fixture.Commit(L"B 新增 notes.md");
  fixture.Push(L"origin", L"main");

  rig.UseA();
  fixture.WriteFile(L"notes.md", "我自己还没跟踪的笔记\n");  // 未跟踪，且与远端带来的同名
  const RepoStamp before = Stamp(fixture);
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");

  const PullProbeOutcome outcome = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(outcome.relationship.queryOk, ToUtf8(outcome.relationship.queryFailure));
  GC_CHECK(ListContains(outcome.relationship.incomingPaths, L"notes.md"));
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(outcome));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(Contains(RiskText(plan), L"未跟踪"));
  GC_CHECK(Contains(RiskText(plan), L"notes.md"));

  const GitRun integrated = ExecutePlan(fixture, plan.arguments);
  GC_CHECK_MESSAGE(!integrated.Success(), "Git 不该覆盖未跟踪文件");
  const RepoStamp after = Stamp(fixture);
  GC_CHECK(after.head == before.head);
  GC_CHECK(after.mainRef == before.mainRef);
  // 远端那份 notes.md 没有进 HEAD（整合被拒绝），而磁盘上仍是用户自己写的内容。
  GC_CHECK(fixture.Run({L"cat-file", L"-e", L"HEAD:notes.md"}, fixture.RepoDir()).exitCode != 0);
  GC_CHECK(ReadWorktreeFile(fixture, fixture.RepoDir() + L"\\notes.md").find("我自己还没跟踪的笔记") !=
           std::string::npos);
  GC_CHECK_MESSAGE(Contains(after.statusLines, L"?? notes.md"),
                   "那个文件到这一步仍应是未跟踪：" + ToUtf8(after.statusLines));
}

// ---- 已配置 rebase：不问策略、给变基命令，执行后历史是线性的 ----

GC_TEST(pull_configured_rebase_decides_without_asking_and_replays_linearly) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"b.txt", "来自 B\n");
  fixture.StageAll();
  fixture.Commit(L"B 的提交");
  fixture.Push(L"origin", L"main");
  const std::wstring bSha = fixture.HeadSha();

  rig.UseA();
  fixture.WriteFile(L"a.txt", "来自 A\n");
  fixture.StageAll();
  fixture.Commit(L"A 的提交");
  const std::wstring aSha = fixture.HeadSha();
  // 仓库级配置：这个仓库的 pull 要变基。预检读的就是这条，不问第二遍。
  fixture.RunCheckedInRepo({L"config", L"pull.rebase", L"true"});
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");

  const PullProbeOutcome outcome = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(outcome.target.queryOk, ToUtf8(outcome.target.queryFailure));
  GC_CHECK(outcome.target.configPullRebase == L"true");
  GC_CHECK(outcome.relationship.relationship == PullRelationship::diverged);

  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(outcome));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(plan.strategy == PullIntegrateStrategy::rebase);
  GC_CHECK(Contains(plan.strategySource, L"pull.rebase = true"));
  GC_CHECK(plan.arguments.front() == L"-c");
  GC_CHECK(ListContains(plan.arguments, L"submodule.recurse=false"));
  GC_CHECK(ListContains(plan.arguments, L"rebase"));
  GC_CHECK(ListContains(plan.arguments, L"--no-autostash"));  // 配置里的 autoStash 也不照办
  GC_CHECK(plan.arguments.back() == bSha);
  GC_CHECK(plan.requiresForce);
  GC_CHECK(Contains(RiskText(plan), L"重写本地"));
  // 变基这一路不借用合并预演的结论。
  GC_CHECK(Contains(plan.confirmationText, L"变基路线不做合并式预演"));

  const GitRun integrated = ExecutePlan(fixture, plan.arguments);
  GC_REQUIRE_MESSAGE(integrated.Success(),
                     "变基应当成功；退出码 " + std::to_string(integrated.exitCode) + "：" +
                         ToUtf8(integrated.err));
  const std::wstring newHead = fixture.HeadSha();
  GC_CHECK(newHead != aSha);                            // 本地那条提交被重放了（ID 变了）
  GC_CHECK(fixture.ParentShaOfHead() == bSha);          // 线性：父提交正是远端那一份
  GC_CHECK(fixture.ShowFileAtHead(L"a.txt") == "来自 A\n");
  GC_CHECK(fixture.ShowFileAtHead(L"b.txt") == "来自 B\n");
  GC_CHECK(fixture.StatusPorcelain().empty());
  const std::wstring parents =
      Trimmed(fixture.RunCheckedInRepo({L"rev-list", L"--parents", L"-n", L"1", L"HEAD"}).out);
  GC_CHECK(parents == newHead + L" " + bSha);           // 只有一个父：没有合并提交
}

// ---- 取消整合：那次可见的 fetch 只留下「跟踪引用前移」 ----

GC_TEST(pull_cancel_after_visible_fetch_leaves_only_the_tracking_ref) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"b.txt", "来自 B\n");
  fixture.StageAll();
  fixture.Commit(L"B 的提交");
  fixture.Push(L"origin", L"main");
  const std::wstring bSha = fixture.HeadSha();

  rig.UseA();
  const RepoStamp before = Stamp(fixture);
  GC_CHECK(before.trackingRef != bSha);  // 抓取之前跟踪引用还停在旧位置

  // 用户在命令窗口里点头跑完了获取，然后在整合的确认框上按了取消。
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");
  const PullProbeOutcome outcome = Probe(fixture, true);
  const PullIntegratePlan plan = gc::git::BuildPullIntegratePlan(IntegrateInputFrom(outcome));
  GC_REQUIRE_MESSAGE(plan.state == PullPlanState::ready, ToUtf8(plan.explanation));

  const RepoStamp cancelled = Stamp(fixture);  // 不执行 plan.arguments：这就是「取消」
  GC_CHECK(cancelled.head == before.head);
  GC_CHECK(cancelled.mainRef == before.mainRef);
  GC_CHECK(cancelled.indexListing == before.indexListing);
  GC_CHECK(cancelled.statusLines == before.statusLines);
  GC_CHECK(cancelled.mergeHead.empty());
  GC_CHECK_MESSAGE(cancelled.trackingRef == bSha,
                   "取消整合不该把已经抓下来的跟踪引用退回去：" + ToUtf8(cancelled.trackingRef));
  GC_CHECK(Contains(plan.confirmationText, L"预检不是保证"));
}

// ---- 执行前复核：外部改动会让旧方案作废 ----

GC_TEST(pull_execution_recheck_notices_external_change) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"b.txt", "来自 B\n");
  fixture.StageAll();
  fixture.Commit(L"B 的提交");
  fixture.Push(L"origin", L"main");

  rig.UseA();
  GC_REQUIRE_MESSAGE(ExecutePlan(fixture, {L"fetch", L"--recurse-submodules=no", L"origin"}).Success(),
                     "获取应当成功");
  const PullProbeOutcome preflight = Probe(fixture, true);
  GC_REQUIRE_MESSAGE(preflight.target.queryOk, ToUtf8(preflight.target.queryFailure));
  // 同一份事实自己跟自己比：没有变化，可以照原方案执行。
  GC_CHECK(gc::git::DescribePullChange(preflight.target, preflight.target).empty());

  // 点头之后、执行之前，外部动作把本地分支挪走了：预检那份方案不再成立。
  fixture.WriteFile(L"external.txt", "外部提交\n");
  fixture.StageAll();
  fixture.Commit(L"外部程序又提交了一次");
  const PullProbeOutcome latest = Probe(fixture, false);
  const std::wstring change = gc::git::DescribePullChange(preflight.target, latest.target);
  GC_CHECK(!change.empty());
  GC_CHECK(Contains(change, L"HEAD"));
  GC_CHECK(Contains(change, L"没有发出任何整合命令"));

  // 只多了一个未跟踪文件也算现状变化：条目要逐条比，不能只数个数。
  fixture.RunCheckedInRepo({L"reset", L"--hard", L"--quiet", preflight.target.headObjectId});
  fixture.WriteFile(L"scratch.txt", "没提交的草稿\n");
  const PullProbeOutcome dirty = Probe(fixture, false);
  const std::wstring worktreeChange = gc::git::DescribePullChange(preflight.target, dirty.target);
  GC_CHECK_MESSAGE(!worktreeChange.empty(),
                   "多出一个未跟踪文件也该被复核发现：" + ToUtf8(preflight.target.branchRef));
  GC_CHECK(Contains(worktreeChange, L"工作区/索引"));
}
