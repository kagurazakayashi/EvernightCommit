// 抓取范围策略的集成测试：全部在夹具自己创建并认领所有权的临时目录里，用真实 Git 驱动
// 生产编排（platform::CollectFetchTarget / CollectPullTarget）与生产方案层
// （git::BuildFetchPlan / ChooseFetchRemote / BuildPullFetchPlan），再把方案合成的命令
// 原样交给真实 git.exe，用「操作前后的完整引用清单、HEAD、索引与工作区快照」证明：
// 命令行上的中和项确实拦住了配置带来的副作用。
//
// 本文件分成两组，**分组标记在用例上方与这里列出**：
//
// A 组（不产生任何提交，也不推送：只建库、加同根内的本地 bare 远端、写配置与引用，可直接运行）：
//   * fetch_scope_fixture_neutralizes_prune_and_tagopt   —— 全局/远端级 prune + pruneTags +
//     tagOpt=--tags：产品命令跑完后过期跟踪引用与本地标签都还在；同一份配置让 Git 自己
//     --dry-run 会报出「[deleted]」，证明那句「没删」不是空跑；
//   * fetch_scope_fixture_global_prune_config_is_neutralized —— 用户层（全局）fetch.prune=true 同样被中和；
//   * fetch_scope_fixture_refuses_branch_mapping         —— 映射写进 refs/heads/：执行前拒绝、不改配置；
//   * fetch_scope_fixture_refuses_mirror_clone_mapping   —— 镜像克隆那种 +refs/*:refs/*：拒绝；
//     而只有 remote.<名>.mirror=true（映射照常）时放行，因为文档写明它只管 push；
//   * fetch_scope_fixture_allows_filter_and_negative_refspec —— 合法的单分支过滤 + ^ 负 refspec：放行；
//   * fetch_scope_fixture_unreadable_bool_value_is_refused —— 布尔取值读不懂：拒绝并说明。
//
// B 组（要有真实提交/推送才能造出「远端有新提交、有标签、分支被删」的场面）：
// 按本工程约束**由用户运行**。用例只写在夹具自有的临时仓库里、远端只是同根内的本地 bare，
// 全程不接触网络，也不碰开发仓库：
//   * fetch_scope_fixture_auto_followed_tags_not_written —— 远端有新标签时，产品命令不写 refs/tags/；
//   * fetch_scope_fixture_tagopt_tags_still_writes_no_local_tags —— tagOpt=--tags 也被 --no-tags 覆盖；
//   * fetch_scope_fixture_deleted_remote_branch_survives —— 远端删掉分支后产品命令不 prune 那条跟踪引用；
//   * fetch_scope_fixture_pull_uses_the_same_command     —— pull 第一步与 fetch 按钮逐字同一条命令。
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "git/fetch_plan.h"
#include "git/fetch_scope.h"
#include "git/pull_plan.h"
#include "git/repository.h"
#include "platform/windows/fetch_probe.h"
#include "platform/windows/pull_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FetchPlan;
using gc::git::FetchPlanState;
using gc::git::FetchTargetFacts;
using gc::git::GitQueryResult;
using gc::git::PullFetchPlanState;
using gc::platform::CollectPullTarget;
using gc::platform::FetchProbeDeps;
using gc::platform::FetchProbeRequest;
using gc::platform::PullProbeDeps;
using gc::platform::PullProbeRequest;
using gc::test::GitFixture;
using gc::test::GitRun;
using gc::test::RemoteRig;

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

bool Contains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

bool HasArgument(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

// 把一次夹具执行转成生产查询接口要的结果形态（与界面/其它夹具用例同一写法）。
GitQueryResult AsQuery(const GitRun& run) {
  GitQueryResult result;
  result.started = run.started;
  result.timedOut = run.timedOut;
  result.exited = run.exited;
  result.exitCode = static_cast<int>(run.exitCode);
  result.outputComplete = run.outputComplete;
  result.utf16Output = run.out;
  result.utf16Error = run.err;
  return result;
}

FetchProbeDeps MakeFetchDeps(GitFixture& fixture) {
  FetchProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);  // 夹具固定使用自己验证过的 git.exe。
    return AsQuery(fixture.Run(arguments, directory));
  };
  return deps;
}

PullProbeDeps MakePullDeps(GitFixture& fixture) {
  PullProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);
    return AsQuery(fixture.Run(arguments, directory));
  };
  return deps;
}

FetchTargetFacts ProbeFetchTarget(GitFixture& fixture) {
  FetchProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectFetchTarget(request, MakeFetchDeps(fixture));
}

// ---- 快照：承诺之外「一个字都没变」要用证据说话 ----

// 完整引用清单（名字 + 对象 ID）：一次操作前后逐字符比对，覆盖 refs/heads/、refs/tags/、
// refs/remotes/ 与其余所有命名空间，不给「只看了 main」这种自证留空间。
std::wstring RefSnapshot(GitFixture& fixture) {
  return fixture.RunCheckedInRepo({L"for-each-ref", L"--format=%(refname)%09%(objectname)"}).out;
}

std::wstring IndexSnapshot(GitFixture& fixture) {
  return fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;
}

std::wstring WorktreeSnapshot(GitFixture& fixture) {
  return fixture.RunCheckedInRepo({L"status", L"--porcelain=v1"}).out;
}

// HEAD 可能是符号引用（在分支上），也可能还不可解析（尚无提交）：两种形态都原样留档。
std::wstring HeadSnapshot(GitFixture& fixture) {
  const GitRun symbolic = fixture.RunInRepo({L"symbolic-ref", L"--quiet", L"HEAD"});
  const std::wstring ref = symbolic.Success() ? gc::git::TrimWide(symbolic.out) : std::wstring();
  return ref + L"@" + fixture.HeadSha();
}

struct RepoSnapshot {
  std::wstring refs;
  std::wstring index;
  std::wstring worktree;
  std::wstring head;
};

RepoSnapshot TakeSnapshot(GitFixture& fixture) {
  RepoSnapshot snapshot;
  snapshot.refs = RefSnapshot(fixture);
  snapshot.index = IndexSnapshot(fixture);
  snapshot.worktree = WorktreeSnapshot(fixture);
  snapshot.head = HeadSnapshot(fixture);
  return snapshot;
}

void CheckNothingElseMoved(GitFixture& fixture, const RepoSnapshot& before, std::string_view context) {
  const RepoSnapshot after = TakeSnapshot(fixture);
  GC_CHECK_MESSAGE(after.head == before.head, std::string(context) + "：HEAD 不该被动");
  GC_CHECK_MESSAGE(after.index == before.index, std::string(context) + "：索引不该被动");
  GC_CHECK_MESSAGE(after.worktree == before.worktree, std::string(context) + "：工作区不该被动");
  GC_CHECK_MESSAGE(after.refs == before.refs,
                   std::string(context) + "：引用清单不该有变化（前：" + ToUtf8(before.refs) +
                       " 后：" + ToUtf8(after.refs) + "）");
}

std::wstring Trimmed(GitFixture& fixture, const std::vector<std::wstring>& arguments) {
  return gc::git::TrimWide(fixture.RunCheckedInRepo(arguments).out);
}

// 一次 fetch 是否真的把 FETCH_HEAD 写了：范围说明里那句「不是磁盘完全只读」要有事实支撑。
bool FetchHeadExists(GitFixture& fixture) {
  const std::wstring path = Trimmed(fixture, {L"rev-parse", L"--git-path", L"FETCH_HEAD"});
  std::error_code error;
  return std::filesystem::exists(std::filesystem::path(path), error);
}

// 把「当前分支的上游是谁」配好：真实仓库里这是 clone 或 `git push -u` 留下的，
// 这里用配置原样搭出来，让抓取目标由分支配置直接确定（不靠用户在选择界面里点）。
void ConfigureUpstream(GitFixture& fixture, const std::wstring& remoteName = L"origin") {
  fixture.RunCheckedInRepo({L"config", L"branch.main.remote", remoteName});
  fixture.RunCheckedInRepo({L"config", L"branch.main.merge", L"refs/heads/main"});
}

// ---- A 组用的空远端夹具：建库、加本地远端、造引用与配置，全程不产生任何提交 ----

// 「origin 是同根内的 bare，本地有一条抓不到的跟踪引用与一个本地标签」。
// 那些引用指向一个用 hash-object 就地造出来的 blob 对象：prune 只看引用名在远端还在不在，
// 与被指对象是什么类型无关，所以不必为了让用例成立而去制造提交。
// （本地分支不能用这种写法造：实测 Git 直接拒绝把非提交对象写进 refs/heads/，
//   「本地分支没被动」由前后完整引用清单逐条比对来证明。）
void PrepareNoCommitRepository(GitFixture& fixture, const std::wstring& remoteName = L"origin") {
  fixture.InitBareRepository(L"origin.git");
  fixture.InitRepository(L"repo");
  fixture.AddRemote(remoteName, fixture.PathInRoot(L"origin.git"));
  ConfigureUpstream(fixture, remoteName);
  fixture.WriteFile(L"scope-probe-only.txt", "只为造一个真实存在的对象，内容与抓取范围无关\n");
  const GitRun hashed =
      fixture.RunCheckedInRepo({L"hash-object", L"-w", L"scope-probe-only.txt"});
  GC_REQUIRE_MESSAGE(hashed.Success(), "用 hash-object 造一个对象应成功：" + ToUtf8(hashed.err));
  const std::wstring objectId = gc::git::TrimWide(hashed.out);
  GC_REQUIRE_MESSAGE(!objectId.empty(), "应当造出一个可用的对象 ID");
  fixture.RunCheckedInRepo({L"update-ref", L"refs/remotes/" + remoteName + L"/gone", objectId});
  fixture.RunCheckedInRepo({L"update-ref", L"refs/tags/local-stale", objectId});
}

}  // namespace

// ============================ A 组：不含提交与推送 ============================

GC_TEST(fetch_scope_fixture_neutralizes_prune_and_tagopt) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  PrepareNoCommitRepository(fixture);

  // 把「会让抓取越界」的配置一次配齐：全局层 + 远端层都开 prune，再开 pruneTags 与 tagOpt=--tags。
  fixture.RunCheckedInRepo({L"config", L"fetch.prune", L"true"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.prune", L"true"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.pruneTags", L"true"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.tagOpt", L"--tags"});

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(HasArgument(plan.arguments, L"--no-prune"));
  GC_CHECK(HasArgument(plan.arguments, L"--no-prune-tags"));
  GC_CHECK(HasArgument(plan.arguments, L"--no-tags"));
  GC_CHECK(HasArgument(plan.arguments, L"--recurse-submodules=no"));
  // 被中和的配置必须在确认正文里点名，不能悄悄中和。
  GC_CHECK(Contains(plan.confirmationText, L"fetch.prune"));
  GC_CHECK(Contains(plan.confirmationText, L"remote.origin.pruneTags"));
  GC_CHECK(Contains(plan.confirmationText, L"tagOpt"));

  const RepoSnapshot before = TakeSnapshot(fixture);
  const GitRun fetch = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(fetch.Success(), "带中和项的 fetch 应成功；退出码 " +
                                          std::to_string(fetch.exitCode) + "：" + ToUtf8(fetch.err));

  // 承诺之外分毫未动：过期跟踪引用、本地标签、本地分支、HEAD、索引、工作区全部原样。
  CheckNothingElseMoved(fixture, before, "产品命令抓取后");
  GC_CHECK(Contains(RefSnapshot(fixture), L"refs/remotes/origin/gone"));
  GC_CHECK(Contains(RefSnapshot(fixture), L"refs/tags/local-stale"));

  // 反证：同一份配置下让 Git 自己按默认口径 --dry-run 一次，它会报出要删哪些——
  // 上面那句「没删」不是因为本来没什么可删。--dry-run 不落盘，仓库不受影响。
  const GitRun control = fixture.RunInRepo({L"fetch", L"--dry-run", L"-v", L"origin"});
  GC_CHECK_MESSAGE(Contains(control.err, L"[deleted]"),
                   "对照组应显示 Git 按配置本来会 prune：" + ToUtf8(control.err));
  GC_CHECK_MESSAGE(Contains(control.err, L"origin/gone"),
                   "对照组点名的必须是那条过期跟踪引用：" + ToUtf8(control.err));

  // FETCH_HEAD 确实被写了：这就是范围正文里「不是磁盘完全只读」那句的根据。
  GC_CHECK(FetchHeadExists(fixture));
}

GC_TEST(fetch_scope_fixture_global_prune_config_is_neutralized) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  PrepareNoCommitRepository(fixture);

  // 只设「用户层（全局）」的 fetch.prune：夹具的隔离环境把 GIT_CONFIG_GLOBAL 指向它自己的档案，
  // 因此这条正好验证全局层的配置也被读进了范围核对，而不是只看了仓库局部。
  fixture.WriteUserConfig("[fetch]\n\tprune = true\n");

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(Contains(plan.confirmationText, L"fetch.prune"));

  const RepoSnapshot before = TakeSnapshot(fixture);
  GC_REQUIRE_MESSAGE(fixture.Run(plan.arguments, fixture.RepoDir()).Success(), "抓取应成功");
  CheckNothingElseMoved(fixture, before, "全局 prune 被中和后");
  // 同一份全局配置下让 Git 自己跑一次 --dry-run：它照样要删（说明中和项真的起作用）。
  const GitRun control = fixture.RunInRepo({L"fetch", L"--dry-run", L"-v", L"origin"});
  GC_CHECK_MESSAGE(Contains(control.err, L"[deleted]"), ToUtf8(control.err));
}

GC_TEST(fetch_scope_fixture_refuses_branch_mapping) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  PrepareNoCommitRepository(fixture);

  // 在一个本来正常的仓库里加这句映射：它会把远端分支直接写成本地分支。
  fixture.RunCheckedInRepo(
      {L"config", L"--add", L"remote.origin.fetch", L"+refs/heads/*:refs/heads/*"});

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_CHECK_MESSAGE(plan.state == FetchPlanState::blocked, ToUtf8(plan.explanation));
  GC_CHECK(plan.arguments.empty());  // 拒绝就是不产生命令，而不是「先跑一条看看」。
  GC_CHECK(Contains(plan.explanation, L"+refs/heads/*:refs/heads/*"));
  GC_CHECK(Contains(plan.explanation, L"不改写你的"));

  // 用户在选择界面里点名同一个远端也一样被拦住：两条入口都过同一道核对。
  const FetchPlan chosen = gc::git::ChooseFetchRemote(facts, L"origin", fixture.RepoDir());
  GC_CHECK(chosen.state == FetchPlanState::blocked);
  GC_CHECK(chosen.arguments.empty());

  // 拒绝不顺手改配置：那句映射与仓库现状都原样留着。
  const std::vector<std::wstring> specs =
      gc::git::SplitLines(Trimmed(fixture, {L"config", L"--get-all", L"remote.origin.fetch"}));
  GC_REQUIRE_MESSAGE(specs.size() == 2, "两句映射都必须原样保留：" +
                                            std::to_string(specs.size()) + " 条");
  GC_CHECK(Contains(specs[1], L"refs/heads/*"));
  // 拒绝也没有触碰仓库：引用清单与前后一致（那句映射本身也还在）。
  GC_CHECK(Contains(RefSnapshot(fixture), L"refs/remotes/origin/gone"));
}

GC_TEST(fetch_scope_fixture_refuses_mirror_clone_mapping) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  fixture.InitBareRepository(L"origin.git");
  fixture.InitRepository(L"repo");
  fixture.AddRemote(L"origin", fixture.PathInRoot(L"origin.git"));
  ConfigureUpstream(fixture);
  // 镜像克隆那种配置：整棵引用树直接映到本地根，外加 mirror=true。
  fixture.RunCheckedInRepo(
      {L"config", L"--replace-all", L"remote.origin.fetch", L"+refs/*:refs/*"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.mirror", L"true"});

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan blocked = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_CHECK_MESSAGE(blocked.state == FetchPlanState::blocked, ToUtf8(blocked.explanation));
  GC_CHECK(blocked.arguments.empty());
  GC_CHECK(Contains(blocked.explanation, L"+refs/*:refs/*"));

  // 把映射改回标准形态、只留 mirror=true：Git 文档写明 mirror 只管 push，因此这一步放行，
  // 但要把这个配置说出来，让用户知道本程序看懂了它、不是没看见。
  fixture.RunCheckedInRepo({L"config", L"--replace-all", L"remote.origin.fetch",
                            L"+refs/heads/*:refs/remotes/origin/*"});
  const FetchPlan allowed = gc::git::BuildFetchPlan(ProbeFetchTarget(fixture), fixture.RepoDir());
  GC_REQUIRE_MESSAGE(allowed.state == FetchPlanState::ready, ToUtf8(allowed.explanation));
  GC_CHECK(Contains(allowed.confirmationText, L"mirror"));
  GC_CHECK(Contains(allowed.confirmationText, L"push"));
}

GC_TEST(fetch_scope_fixture_allows_filter_and_negative_refspec) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  PrepareNoCommitRepository(fixture);

  // 用户自己配的分支过滤与排除项必须原样放行，不能被误判成「超出范围」。
  // 实测本机 Git 2.53：把过滤写成精确的单个分支（refs/heads/main）而远端根本没有那条分支时，
  // `git fetch` 会以「couldn't find remote ref」失败——那是 Git 对远端的如实回答，与范围核对无关。
  // 这里用形态相同的通配过滤，让「放行 + 命令真的能跑」这两件事都能核对到。
  fixture.RunCheckedInRepo({L"config", L"--replace-all", L"remote.origin.fetch",
                            L"+refs/heads/topic*:refs/remotes/origin/topic*"});
  fixture.RunCheckedInRepo({L"config", L"--add", L"remote.origin.fetch", L"^refs/heads/wip/*"});

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(Contains(plan.confirmationText, L"+refs/heads/topic*:refs/remotes/origin/topic*"));
  GC_CHECK(Contains(plan.confirmationText, L"负 refspec"));

  const RepoSnapshot before = TakeSnapshot(fixture);
  const GitRun fetch = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_CHECK_MESSAGE(fetch.Success(), "合法的过滤与排除不该被 Git 拒绝；退出码 " +
                                         std::to_string(fetch.exitCode) + "：" + ToUtf8(fetch.err));
  CheckNothingElseMoved(fixture, before, "过滤形态抓取后");
}

GC_TEST(fetch_scope_fixture_unreadable_bool_value_is_refused) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
  PrepareNoCommitRepository(fixture);
  // 这句取值连 Git 自己都不认（remote.<名>.prune 必须是布尔）。
  fixture.RunCheckedInRepo({L"config", L"--replace-all", L"remote.origin.prune", L"maybe"});

  // 实测本机 Git 2.53：这种写法让**任何**一条要读配置的 Git 命令先失败
  // （`git remote -v` 就报 "bad boolean config value"），所以本程序的预检连远端清单都读不回来。
  // 结论与预期一致：拒绝执行、不产生命令，并把 Git 的原始原因带给用户——
  // 「在看不懂的配置上承诺范围」这件事根本不会发生。（判读层自己那条「布尔取值读不懂」的
  // 拒绝路径由 tests/fetch_scope_tests.cpp 用桩钉住：那种场合是配置能被读回、但取值不认识。）
  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_CHECK_MESSAGE(!facts.queryOk, "读不懂的布尔配置不该被当成可执行的事实");
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_CHECK(plan.state == FetchPlanState::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(Contains(plan.explanation, L"没有发出任何命令"));
  GC_CHECK(Contains(plan.explanation, L"bad boolean config value"));
  // 拒绝之后配置仍是用户自己写的那个样子（本程序不修配置，也不「当它没有」）。
  GC_CHECK(Trimmed(fixture, {L"config", L"--get", L"remote.origin.prune"}) == L"maybe");
}

// ==================== B 组：需要真实提交与推送，交由用户运行 ====================

GC_TEST(fetch_scope_fixture_auto_followed_tags_not_written) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  // B 提交一条并打上标签推上去：远端现在有一个「A 一旦抓到就会自动跟随」的标签。
  rig.UseB();
  fixture.WriteFile(L"tagged.txt", "from B\n");
  fixture.StageAll();
  fixture.Commit(L"B 带标签的新提交");
  fixture.RunCheckedInRepo({L"tag", L"-a", L"-m", L"远端新标签", L"v-from-remote"});
  fixture.Push(L"origin", L"main");
  fixture.Push(L"origin", L"refs/tags/v-from-remote");
  const std::wstring remoteMain = fixture.HeadSha();
  rig.UseA();

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));

  const RepoSnapshot before = TakeSnapshot(fixture);
  const std::wstring localMainBefore = fixture.RevParseVerified(L"refs/heads/main");
  GC_REQUIRE_MESSAGE(fixture.Run(plan.arguments, fixture.RepoDir()).Success(), "抓取应成功");

  // 标签一个字都不写；允许命名空间里的跟踪引用按承诺前进；HEAD/本地分支/索引/工作区不动。
  GC_CHECK_MESSAGE(!Contains(RefSnapshot(fixture), L"refs/tags/v-from-remote"),
                   "--no-tags 之下不该出现远端带来的标签");
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == remoteMain);
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == localMainBefore);
  GC_CHECK(HeadSnapshot(fixture) == before.head);
  GC_CHECK(IndexSnapshot(fixture) == before.index);
  GC_CHECK(WorktreeSnapshot(fixture) == before.worktree);
}

GC_TEST(fetch_scope_fixture_tagopt_tags_still_writes_no_local_tags) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"tagged.txt", "from B\n");
  fixture.StageAll();
  fixture.Commit(L"B 带标签的新提交");
  fixture.RunCheckedInRepo({L"tag", L"-a", L"-m", L"远端新标签", L"v-tagopt"});
  fixture.Push(L"origin", L"main");
  fixture.Push(L"origin", L"refs/tags/v-tagopt");
  const std::wstring remoteMain = fixture.HeadSha();
  rig.UseA();
  // tagOpt=--tags 就是「让这次抓取把所有标签写进本地」的远端级配置；Git 文档对该配置项写明
  // 命令行可以直接覆盖它。本用例负责把这句话钉在实测里，而不是让它停留在推断。
  fixture.RunCheckedInRepo({L"config", L"remote.origin.tagOpt", L"--tags"});

  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));
  GC_CHECK(Contains(plan.confirmationText, L"tagOpt"));

  const RepoSnapshot before = TakeSnapshot(fixture);
  const std::wstring localMainBefore = fixture.RevParseVerified(L"refs/heads/main");
  GC_REQUIRE_MESSAGE(fixture.Run(plan.arguments, fixture.RepoDir()).Success(), "抓取应成功");
  GC_CHECK_MESSAGE(!Contains(RefSnapshot(fixture), L"refs/tags/v-tagopt"),
                   "tagOpt=--tags 也必须被 --no-tags 覆盖：本地标签不该被写出来");
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == remoteMain);
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == localMainBefore);
  GC_CHECK(IndexSnapshot(fixture) == before.index);
  GC_CHECK(WorktreeSnapshot(fixture) == before.worktree);
}

GC_TEST(fetch_scope_fixture_deleted_remote_branch_survives) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  // B 推一条新分支，A 抓下来；然后 B 把远端那条分支删掉。
  rig.UseB();
  fixture.RunCheckedInRepo({L"checkout", L"-b", L"topic"});
  fixture.WriteFile(L"topic.txt", "work\n");
  fixture.StageAll();
  fixture.Commit(L"topic 上的提交");
  fixture.Push(L"origin", L"topic");
  rig.UseA();
  fixture.RunCheckedInRepo({L"fetch", L"origin"});
  const std::wstring topicBefore = fixture.RevParseVerified(L"refs/remotes/origin/topic");
  GC_REQUIRE_MESSAGE(!topicBefore.empty(), "A 应先把那条跟踪引用抓下来");
  rig.UseB();
  fixture.Push(L"origin", L":refs/heads/topic");  // 删除远端那条分支
  rig.UseA();

  // 全局 prune 开着：默认口径的 `git fetch` 会删掉那条跟踪引用，而产品命令不会。
  fixture.RunCheckedInRepo({L"config", L"fetch.prune", L"true"});
  const FetchTargetFacts facts = ProbeFetchTarget(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, ToUtf8(facts.queryFailure));
  const FetchPlan plan = gc::git::BuildFetchPlan(facts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(plan.state == FetchPlanState::ready, ToUtf8(plan.explanation));

  const RepoSnapshot before = TakeSnapshot(fixture);
  const std::wstring localMainBefore = fixture.RevParseVerified(L"refs/heads/main");
  GC_REQUIRE_MESSAGE(fixture.Run(plan.arguments, fixture.RepoDir()).Success(), "抓取应成功");
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/topic") == topicBefore);
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == localMainBefore);
  GC_CHECK(IndexSnapshot(fixture) == before.index);

  // 反证：同一份配置让 Git 自己按默认口径 --dry-run 一次，它会报出要删那条引用。
  const GitRun control = fixture.RunInRepo({L"fetch", L"--dry-run", L"-v", L"origin"});
  GC_CHECK_MESSAGE(Contains(control.err, L"[deleted]"),
                   "对照组应报出会被删除的引用：" + ToUtf8(control.err));
  GC_CHECK_MESSAGE(Contains(control.err, L"topic"),
                   "对照组点名的必须是那条分支：" + ToUtf8(control.err));
}

GC_TEST(fetch_scope_fixture_pull_uses_the_same_command) {
  RemoteRig rig;
  std::string reason;
  GC_REQUIRE_MESSAGE(rig.Prepare(reason), reason);
  GitFixture& fixture = rig.fixture();

  rig.UseB();
  fixture.WriteFile(L"b.txt", "from B\n");
  fixture.StageAll();
  fixture.Commit(L"B 推上去的新提交");
  fixture.Push(L"origin", L"main");
  const std::wstring remoteMain = fixture.HeadSha();
  rig.UseA();
  // 与 fetch 按钮同一份配置：两个入口必须在同一道范围核对上给出同样的答案。
  fixture.RunCheckedInRepo({L"config", L"fetch.prune", L"true"});
  fixture.RunCheckedInRepo({L"config", L"remote.origin.pruneTags", L"true"});

  const FetchPlan fetchPlan = gc::git::BuildFetchPlan(ProbeFetchTarget(fixture), fixture.RepoDir());
  GC_REQUIRE_MESSAGE(fetchPlan.state == FetchPlanState::ready, ToUtf8(fetchPlan.explanation));

  PullProbeRequest pullRequest;
  pullRequest.exePath = fixture.GitExe();
  pullRequest.repositoryDirectory = fixture.RepoDir();
  pullRequest.absoluteGitDir =
      Trimmed(fixture, {L"rev-parse", L"--absolute-git-dir"});
  pullRequest.timeoutMilliseconds = 20000;
  pullRequest.includeRelationship = false;
  const gc::git::PullTargetFacts pullFacts = CollectPullTarget(pullRequest, MakePullDeps(fixture));
  GC_REQUIRE_MESSAGE(pullFacts.queryOk, ToUtf8(pullFacts.queryFailure));
  const gc::git::PullFetchPlan pullFetchPlan =
      gc::git::BuildPullFetchPlan(pullFacts, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(pullFetchPlan.state == PullFetchPlanState::ready,
                     ToUtf8(pullFetchPlan.explanation));
  // 逐字相同：参数数组与展示命令都不许有第二种写法。
  GC_CHECK(pullFetchPlan.arguments == fetchPlan.arguments);
  GC_CHECK(pullFetchPlan.commandLabel == fetchPlan.commandLabel);

  const RepoSnapshot before = TakeSnapshot(fixture);
  const std::wstring localMainBefore = fixture.RevParseVerified(L"refs/heads/main");
  GC_REQUIRE_MESSAGE(fixture.Run(pullFetchPlan.arguments, fixture.RepoDir()).Success(),
                     "pull 第一步的抓取应成功");
  // 与 fetch 按钮同样的效果：跟踪引用前移到远端那一份，本地分支/索引/工作区分毫未动。
  GC_CHECK(fixture.RevParseVerified(L"refs/remotes/origin/main") == remoteMain);
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == localMainBefore);
  GC_CHECK(HeadSnapshot(fixture) == before.head);
  GC_CHECK(IndexSnapshot(fixture) == before.index);
  GC_CHECK(WorktreeSnapshot(fixture) == before.worktree);
}
