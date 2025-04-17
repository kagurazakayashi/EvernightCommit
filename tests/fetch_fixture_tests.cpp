// fetch 的集成测试：在夹具自己创建并认领所有权的临时目录里，用真实 Git 驱动
// 生产编排（platform::CollectFetchTarget / CollectUndoPreflight）与生产方案层
// （git::BuildFetchPlan / ChooseFetchRemote / BuildUndoCommitPlan），再把方案合成的
// 命令原样交给真实 Git 执行，逐项核对：
//   * B 推送新提交后，A 用产品命令 fetch：远端跟踪引用（refs/remotes/origin/main）
//     确实前进，而 A 的 HEAD、本地分支、索引与工作区一个字节都不动；
//   * fetch 之后「撤回最近提交」的发布状态判断随之更新：同一条本地提交，
//     从「本地信息未发现已发布」变成「已知已发布」，走「强制撤回（仅本地）」确认；
//   * 没有远端的仓库：目标判读如实 blocked，不猜 origin、也不留下任何配置；
//   * 指向根内不存在目录的「无效本地目标」：fetch 以 Git 的真实失败收场，
//     已有配置原样保留（本程序不删配置、不自动重试）；
//   * 测试远端守卫拒绝协议 URL / scp 形态 / UNC / 根外路径。
// 远端只能是同根下的本地 bare 仓库，全程绝不接触网络。
#include <string>
#include <string_view>
#include <vector>

#include "git/fetch_plan.h"
#include "git/undo_commit_plan.h"
#include "platform/windows/fetch_probe.h"
#include "platform/windows/undo_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FetchPlan;
using gc::git::FetchPlanState;
using gc::git::FetchTargetFacts;
using gc::git::GitQueryResult;
using gc::git::UndoCommitPlan;
using gc::git::UndoCommitPlanInput;
using gc::git::UndoPreflightFacts;
using gc::git::UndoPublishEvidence;
using gc::test::GitFixture;
using gc::test::GitRun;
using gc::test::PrerequisiteFailure;
using gc::test::RemoteRig;

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool LinesContain(const std::vector<std::wstring>& lines, std::wstring_view needle) {
  for (const std::wstring& line : lines) {
    if (line == needle) {
      return true;
    }
  }
  return false;
}

// 用夹具的隔离执行器装配生产 fetch 目标预检依赖：查询走与界面完全相同的代码路径。
gc::platform::FetchProbeDeps MakeFetchDeps(GitFixture& fixture) {
  gc::platform::FetchProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) -> GitQueryResult {
    static_cast<void>(exePath);  // 夹具固定使用自己验证过的 git.exe。
    const GitRun run = fixture.Run(arguments, directory);
    GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = run.out;
    result.utf16Error = run.err;
    return result;
  };
  return deps;
}

gc::platform::UndoProbeDeps MakeUndoDeps(GitFixture& fixture) {
  gc::platform::UndoProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) -> GitQueryResult {
    static_cast<void>(exePath);
    const GitRun run = fixture.Run(arguments, directory);
    GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = run.out;
    result.utf16Error = run.err;
    return result;
  };
  return deps;
}

FetchTargetFacts ProbeFetchTarget(GitFixture& fixture) {
  gc::platform::FetchProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectFetchTarget(request, MakeFetchDeps(fixture));
}

UndoPreflightFacts ProbeUndo(GitFixture& fixture) {
  gc::platform::UndoProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectUndoPreflight(request, MakeUndoDeps(fixture));
}

UndoCommitPlan PlanFromUndoFacts(GitFixture& fixture, const UndoPreflightFacts& facts) {
  UndoCommitPlanInput input;
  input.facts = facts;
  input.repositoryRoot = fixture.RepoDir();
  return gc::git::BuildUndoCommitPlan(input);
}

// 索引的逐条清单：fetch 前后必须一字不差（fetch 根本没有碰索引的余地，这里用证据说话）。
std::wstring IndexListing(GitFixture& fixture) {
  return fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;
}

}  // namespace

GC_TEST(fetch_updates_tracking_refs_without_touching_head_index_or_worktree) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  // B 提交并推送一条新提交：bare 远端前进，A 对此一无所知（还没 fetch）。
  rig.UseB();
  fixture.WriteFile(L"b.txt", "from B\n");
  fixture.StageAll();
  fixture.Commit(L"B 推上去的新提交");
  fixture.Push(L"origin", L"main");
  const std::wstring bSha = fixture.HeadSha();

  // A 一侧先把「不能被动的东西」全部留档。
  rig.UseA();
  const std::wstring headBefore = fixture.HeadSha();
  const std::wstring mainBefore = fixture.RevParseVerified(L"refs/heads/main");
  const std::wstring trackingBefore = fixture.RevParseVerified(L"refs/remotes/origin/main");
  const std::wstring indexBefore = IndexListing(fixture);
  GC_REQUIRE_MESSAGE(!trackingBefore.empty() && trackingBefore != bSha,
                     "fetch 前 A 的跟踪引用应停在旧提交");

  // 生产判读：A 的分支 main 明确配置了 origin → 目标直接确定，无需用户选。
  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.onBranch && facts.branchName == L"main");
  GC_CHECK(facts.configuredRemote == L"origin");
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));

  // 原样执行方案合成的命令（界面走的将是同一条参数数组，只是外面套命令窗口）。
  const GitRun fetch = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(fetch.Success(),
                     "fetch 应成功；退出码 " + std::to_string(fetch.exitCode) + "：" + ToUtf8(fetch.err));

  // 跟踪引用前进了；HEAD、本地分支、索引、工作区一个字节都没动。
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == bSha);
  GC_CHECK(fixture.HeadSha() == headBefore);
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == mainBefore);
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "fetch 不许改索引");
  GC_CHECK_MESSAGE(fixture.StatusPorcelain().empty(), "fetch 不许改工作区");
}

GC_TEST(fetch_updates_undo_published_judgment) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  // A 提交一条只存在于本地的 C1；本地跟踪引用停在初始提交，查不到 C1 已发布。
  fixture.WriteFile(L"a-only.txt", "A 本地\n");
  fixture.StageAll();
  fixture.Commit(L"A 本地未推送的提交");
  const std::wstring localCommit = fixture.HeadSha();

  UndoPreflightFacts before = ProbeUndo(fixture);
  GC_REQUIRE_MESSAGE(before.head.headObjectId == localCommit, "预检必须读到最后这条本地提交");
  GC_CHECK_MESSAGE(before.publish == UndoPublishEvidence::notFound,
                   "有跟踪引用但没人包含它：应判「本地信息未发现已发布」，而不是断言未推送");
  UndoCommitPlan beforePlan = PlanFromUndoFacts(fixture, before);
  GC_CHECK_MESSAGE(!beforePlan.requiresForce, ToUtf8(beforePlan.previewText));

  // 制造「远端其实已包含它」：B 从 A 的本地路径 fetch 后快进，再推到 bare 远端。
  // 全程仍是临时根内的本地路径（测试远端守卫同样把关这一次 AddRemote）。
  rig.UseB();
  fixture.AddRemote(L"a-local", rig.DirectoryA());
  fixture.RunCheckedInRepo({L"fetch", L"a-local"});
  fixture.RunCheckedInRepo({L"merge", L"--ff-only", L"a-local/main"});
  fixture.Push(L"origin", L"main");

  // A fetch：产品方案层给的那条命令原样执行。
  rig.UseA();
  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));
  GC_REQUIRE_MESSAGE(fixture.Run(plan.arguments, fixture.RepoDir()).Success(), "fetch 应成功");
  // fetch 不改本地分支：HEAD 还停在 A 自己的那条提交上。
  GC_CHECK(fixture.HeadSha() == localCommit);

  // 发布状态判断随之更新：同一条提交现在是「已知已发布」，撤回必须走强制确认。
  before = ProbeUndo(fixture);
  GC_REQUIRE_MESSAGE(before.head.headObjectId == localCommit, "预检读的必须还是 A 的 HEAD");
  GC_CHECK_MESSAGE(before.publish == UndoPublishEvidence::contained,
                   "fetch 之后应依据新读回的跟踪引用改判「已知已发布」");
  GC_CHECK(LinesContain(before.containingRemoteRefs, L"refs/remotes/origin/main"));
  beforePlan = PlanFromUndoFacts(fixture, before);
  GC_CHECK_MESSAGE(!beforePlan.blocked, ToUtf8(beforePlan.blockedReason));
  GC_CHECK_MESSAGE(beforePlan.requiresForce, "已知已发布必须走「强制撤回（仅本地）」确认");
  GC_CHECK(Contains(beforePlan.previewText, L"已知已发布"));
  GC_CHECK(Contains(beforePlan.previewText, L"refs/remotes/origin/main"));
}

GC_TEST(fetch_blocked_without_any_remote_and_leaves_no_config) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"无远端仓库的唯一提交");

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  GC_CHECK(facts.remoteListOk);
  GC_CHECK_MESSAGE(facts.remotes.empty(), "这个仓库本来就没有远端");
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_CHECK_MESSAGE(plan.state == FetchPlanState::blocked, ToUtf8(plan.explanation));
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(Contains(plan.explanation, L"origin"));  // 明说「不猜 origin」。

  // 拒绝不等于顺手改配置：判读全程只读，仓库的远端配置一条也没多出来。
  GC_CHECK(fixture.RunCheckedInRepo({L"remote"}).out.empty());
}

GC_TEST(fetch_against_invalid_local_target_fails_and_keeps_config) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();
  rig.UseA();

  // 「无效本地目标」：根内一个从未创建过的目录——守卫放行（仍在根内的本地路径），
  // 但 Git 自己会拒绝从它抓取。
  const std::wstring missing = fixture.PathInRoot(L"never-created.git");
  fixture.AddRemote(L"broken", missing);

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  // 分支配置指向 origin（有效），无效目标必须由用户明确点出来——fetch 从不自动挑它。
  GC_CHECK(facts.configuredRemote == L"origin");
  const FetchPlan plan = gc::git::ChooseFetchRemote(facts, L"broken", fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));

  const std::wstring headBefore = fixture.HeadSha();
  const GitRun fetch = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_CHECK_MESSAGE(!fetch.Success() && fetch.exitCode != 0 && !fetch.timedOut,
                   "对不存在的路径 fetch 必须以 Git 的真实失败收场");
  // 失败不带来任何副作用：配置原样、HEAD 原样，程序不删配置也不该在这里重试。
  GC_CHECK(fixture.RunCheckedInRepo({L"config", L"--get", L"remote.broken.url"}).out.find(
               L"never-created.git") != std::wstring::npos);
  GC_CHECK(fixture.HeadSha() == headBefore);
}

GC_TEST(test_remote_guard_rejects_network_forms_and_out_of_root) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitRepository(L"repo");

  std::string why;
  // 一切带协议的 URL 都不放行——包括 file://（它能写成指向网络共享的形态）。
  GC_CHECK(!fixture.IsAllowedTestRemote(L"https://github.com/u/r.git", why));
  GC_CHECK(!fixture.IsAllowedTestRemote(L"http://127.0.0.1:1/r.git", why));
  GC_CHECK(!fixture.IsAllowedTestRemote(L"ssh://git@host/repo.git", why));
  GC_CHECK(!fixture.IsAllowedTestRemote(L"git://host/repo.git", why));
  GC_CHECK(!fixture.IsAllowedTestRemote(L"file://server/share/repo.git", why));
  GC_CHECK(!fixture.IsAllowedTestRemote(L"git@github.com:u/r.git", why));  // scp 形态。
  // UNC/网络共享与根外路径。
  GC_CHECK(!fixture.IsAllowedTestRemote(L"\\\\server\\share\\repo.git", why));
  GC_CHECK(!fixture.IsAllowedTestRemote(L"C:\\Users\\Public\\other.git", why));
  // 相对路径会被按当前目录解释，同样拒绝。
  GC_CHECK(!fixture.IsAllowedTestRemote(L"sub/bare.git", why));
  // 临时根内的绝对本地路径是唯一放行的形态。
  const std::wstring inside = fixture.PathInRoot(L"origin.git");
  GC_CHECK_MESSAGE(fixture.IsAllowedTestRemote(inside, why), why);

  // AddRemote 与 Clone 都走同一道守卫：越界 URL 抛前置失败，绝不落到 Git 命令行。
  bool threw = false;
  try {
    fixture.AddRemote(L"bad", L"https://github.com/u/r.git");
  } catch (const PrerequisiteFailure&) {
    threw = true;
  }
  GC_CHECK(threw);
  threw = false;
  try {
    fixture.CloneRepository(L"C:\\Users\\Public\\other.git", L"evil");
  } catch (const PrerequisiteFailure&) {
    threw = true;
  }
  GC_CHECK(threw);
  GC_CHECK(fixture.RunCheckedInRepo({L"remote"}).out.empty());  // 被拒绝的从没进过配置。
}

GC_TEST(remote_rig_is_isolated_and_both_workspaces_track_the_same_bare) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);

  // A、B 的 origin 都指同一个根内 bare；A 的上游已绑定（push -u 做过）。
  rig.UseA();
  GC_CHECK(rig.fixture().RunCheckedInRepo({L"remote", L"get-url", L"origin"}).out.find(
               L"origin.git") != std::wstring::npos);
  GC_CHECK(rig.fixture().RevParseVerified(L"refs/remotes/origin/main") == rig.fixture().HeadSha());
  rig.UseB();
  GC_CHECK(rig.fixture().RevParseVerified(L"refs/remotes/origin/main") == rig.fixture().HeadSha());
  // B 的克隆没有沾到任何协议形态 URL：remote -v 里只出现本地路径。
  const std::wstring listing = rig.fixture().RunCheckedInRepo({L"remote", L"-v"}).out;
  GC_CHECK(listing.find(L"://") == std::wstring::npos);
  GC_CHECK(Contains(listing, rig.OriginUrl()));
}
