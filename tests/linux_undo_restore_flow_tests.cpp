// 撤回 / 按记录恢复控制器的接线测试（Linux 真实 Git，隔离临时仓库）：
//   1) 预检回来时仓库已被换掉：那一次结果必须作废，绝不把旧仓库的事实带进新仓库；
//   2) 按记录恢复的绑定同理（工作区根 + 绝对 Git 目录 + git 程序 + 代次）；
//   3) 撤回控制器真的把带预期旧值的 update-ref 交给命令窗口，并带上可回退的历史线索；
//      执行那条命令之后，索引与工作区文件字节保持不变（只移动分支引用）。
// 全程不执行 git push、不联网；远端相关的一条都没用。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/task_coordinator.h"
#include "git/commit_history.h"
#include "git/repository.h"
#include "git/undo_commit_plan.h"
#include "platform/main_thread_dispatch.h"
#include "platform/path.h"
#include "platform/repo_detect.h"
#include "platform/undo_probe.h"
#include "support/fake_operation_host.h"
#include "support/linux_git_fixture.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"
#include "ui/restore_flow.h"
#include "ui/undo_flow.h"

namespace {

using gc::platform::MainThreadDispatcher;
using gc::test::FakeOperationHost;
using gc::test::LinuxGitFixture;
using gc::test::RequireReadFile;
using gc::ui::OperationContext;
using gc::ui::RestoreFlow;
using gc::ui::UndoFlow;

constexpr uint64_t kProbeSource = 1;
constexpr uint64_t kRecheckSource = 2;

void RequirePrepared(LinuxGitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), "Linux Git 夹具前置失败：" + reason);
}

std::wstring TrimLine(std::wstring text) {
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
    text.pop_back();
  }
  return text;
}

OperationContext MakeContext(const LinuxGitFixture& fixture, std::wstring root,
                             std::wstring absoluteGitDir, unsigned long long generation) {
  OperationContext ctx;
  ctx.notify = nullptr;
  ctx.gitExecutable = fixture.GitExe();
  ctx.detection.root = std::move(root);
  ctx.detection.absoluteGitDir = std::move(absoluteGitDir);
  ctx.repoUsable = true;
  ctx.repositoryIdentity.generation = generation;
  ctx.repositoryIdentity.gitExePath = ctx.gitExecutable;
  ctx.repositoryIdentity.workTreeRoot = ctx.detection.root;
  return ctx;
}

// 等一个指定 sourceId 的完成通知；超时返回 false（测试失败，不无限等）。
bool WaitForNotice(const std::shared_ptr<MainThreadDispatcher>& dispatcher, uint64_t sourceId,
                   MainThreadDispatcher::Notice* out) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
             .count() < 15000) {
    for (const MainThreadDispatcher::Notice& notice : dispatcher->Drain()) {
      if (notice.sourceId == sourceId) {
        *out = notice;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

GC_TEST(undo_flow_discards_probe_that_finished_after_repository_switch) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();

  FakeOperationHost host;
  UndoFlow flow;
  flow.SetCompletionDispatcher(dispatcher, kProbeSource, kRecheckSource);
  const OperationContext original =
      MakeContext(fixture, fixture.RepoDir(), fixture.RepoDir() + L"/.git", 1);
  flow.Start(host, original);

  // 预检还在跑的时候界面换了仓库：回来的那份结果属于旧仓库，必须整份作废。
  OperationContext switched =
      MakeContext(fixture, fixture.PathInRoot(L"another-worktree"), fixture.RepoDir() + L"/.git", 2);
  MainThreadDispatcher::Notice notice;
  GC_REQUIRE(WaitForNotice(dispatcher, kProbeSource, &notice), "撤回预检应完成");
  flow.OnProbeCompleted(host, switched, notice.serial);

  GC_CHECK_MESSAGE(host.launches.empty(), "换仓库之后不得发出任何 update-ref");
  GC_CHECK_MESSAGE(gc::test::LastStatus(host).find(L"仓库已经换掉") != std::wstring::npos,
                   "状态栏必须明确说是仓库换掉导致本次作废");
  GC_CHECK(!flow.Active());

  flow.BeginStop();
  flow.JoinWorkers();
  dispatcher->Close();
}

GC_TEST(restore_flow_discards_probe_when_repository_identity_changed) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();

  FakeOperationHost host;
  RestoreFlow flow;
  flow.SetCompletionDispatcher(dispatcher, kProbeSource, kRecheckSource);
  const OperationContext original =
      MakeContext(fixture, fixture.RepoDir(), fixture.RepoDir() + L"/.git", 1);

  gc::git::RestoreClues clues;
  clues.valid = true;
  clues.repositoryRoot = fixture.RepoDir();
  clues.branchRef = L"refs/heads/main";
  clues.expectedCurrentOid = std::wstring(40, L'a');
  clues.moveToOid = std::wstring(40, L'b');
  flow.Start(host, original, clues);

  OperationContext switched =
      MakeContext(fixture, fixture.PathInRoot(L"other"), fixture.RepoDir() + L"/.git", 2);
  MainThreadDispatcher::Notice notice;
  GC_REQUIRE(WaitForNotice(dispatcher, kProbeSource, &notice), "恢复预检应完成");
  flow.OnProbeCompleted(host, switched, notice.serial);

  GC_CHECK_MESSAGE(host.launches.empty(), "仓库身份变了之后不得发出任何恢复命令");
  GC_CHECK_MESSAGE(gc::test::LastStatus(host).find(L"仓库身份已经变了") != std::wstring::npos,
                   "状态栏必须点明是仓库身份变化导致本次作废");
  GC_CHECK(!flow.Active());

  flow.BeginStop();
  flow.JoinWorkers();
  dispatcher->Close();
}

GC_TEST(undo_flow_launches_expected_ref_update_and_binds_history) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "第一份内容\n", true);
  fixture.Commit(L"第一次提交");
  fixture.WriteFile(L"b.txt", "第二份内容\n", true);
  fixture.Commit(L"第二次提交");

  const std::wstring headBefore = fixture.HeadSha();
  const std::wstring parentBefore = TrimLine(fixture.RunInRepo({L"rev-parse", L"HEAD^"}).out);
  const std::wstring indexBefore = fixture.RunInRepo({L"ls-files", L"-s", L"-z"}).out;
  const std::wstring worktreeFile = fixture.RepoDir() + L"/b.txt";
  std::string worktreeBefore;
  RequireReadFile(gc::platform::WideToUtf8(worktreeFile), worktreeBefore);

  gc::platform::RepoDetectRequest detectRequest;
  detectRequest.exePath = fixture.GitExe();
  detectRequest.directory = fixture.RepoDir();
  detectRequest.timeoutMilliseconds = 20000;
  const gc::git::RepoDetection detection =
      gc::platform::DetectRepository(detectRequest, fixture.MakeDetectDeps());
  GC_REQUIRE(detection.kind == gc::git::RepoKind::plainWorktree, "仓库识别应成功");

  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  FakeOperationHost host;  // 默认对确认框回答「确定」
  UndoFlow flow;
  flow.SetCompletionDispatcher(dispatcher, kProbeSource, kRecheckSource);
  const OperationContext ctx =
      MakeContext(fixture, detection.root, detection.absoluteGitDir, 1);
  flow.Start(host, ctx);

  MainThreadDispatcher::Notice probeNotice;
  GC_REQUIRE(WaitForNotice(dispatcher, kProbeSource, &probeNotice), "撤回预检应完成");
  flow.OnProbeCompleted(host, ctx, probeNotice.serial);
  // 「本地没有远端跟踪引用」不能断言「从未推送」，因此这条撤回走的是风险确认；
  // 两种确认框只要有一个被摆出来，就说明方案没有在确认之前被拒绝。
  GC_REQUIRE_MESSAGE(!host.confirmations.empty() || !host.riskConfirms.empty(),
                     "预检之后必须弹出确认框；状态：" +
                         gc::platform::WideToUtf8(gc::test::LastStatus(host)) +
                         "；信息框数：" + std::to_string(host.infos.size()));
  MainThreadDispatcher::Notice recheckNotice;
  GC_REQUIRE(WaitForNotice(dispatcher, kRecheckSource, &recheckNotice), "执行前复核应完成");
  flow.OnRecheckCompleted(host, ctx, recheckNotice.serial);

  GC_REQUIRE(host.launches.size() == 1, "复核通过后应恰好启动一条 update-ref");
  const gc::git::CommandWindowOperation operation = host.launches[0].operation;
  const gc::ui::CommandLaunchOptions options = host.launches[0].options;
  GC_CHECK(operation.operationId == L"undo-commit");
  GC_CHECK(!operation.arguments.empty() && operation.arguments[0] == L"update-ref");
  const bool hasTargetRef =
      std::find(operation.arguments.begin(), operation.arguments.end(), L"refs/heads/main") !=
      operation.arguments.end();
  GC_CHECK_MESSAGE(hasTargetRef, "命令必须按确认过的完整分支引用发出，不能用可变的 HEAD");
  const bool hasParent =
      std::find(operation.arguments.begin(), operation.arguments.end(), parentBefore) !=
      operation.arguments.end();
  const bool hasHead =
      std::find(operation.arguments.begin(), operation.arguments.end(), headBefore) !=
      operation.arguments.end();
  GC_CHECK_MESSAGE(hasParent && hasHead, "命令必须同时带新值与预期旧值");

  // 历史线索：撤回前指着的原提交、撤回后应指的父提交，都是完整 ID。
  GC_REQUIRE(options.history.record, "撤回应落一条操作历史");
  GC_CHECK(options.history.flow == gc::app::HistoryFlow::undo);
  GC_CHECK(options.history.restoreKind == gc::app::HistoryRestoreKind::refMove);
  GC_CHECK(options.history.restoreBranchRef == L"refs/heads/main");
  GC_CHECK(options.history.restoreExpectedCurrentId == parentBefore);
  GC_CHECK(options.history.restoreUndoToObjectId == headBefore);
  GC_CHECK_MESSAGE(!options.history.restoreNote.empty(), "恢复线索要带一句面向人的说明");

  // 执行控制器交出的参数：只移动分支引用，索引与工作区文件字节保持不变。
  fixture.RunCheckedInRepo(operation.arguments);
  GC_CHECK(fixture.HeadSha() == parentBefore);
  GC_CHECK_MESSAGE(fixture.RunInRepo({L"ls-files", L"-s", L"-z"}).out == indexBefore,
                   "撤回前后索引必须一字不差");
  std::string worktreeAfter;
  RequireReadFile(gc::platform::WideToUtf8(worktreeFile), worktreeAfter);
  GC_CHECK_MESSAGE(worktreeAfter == worktreeBefore, "撤回前后工作区文件字节必须保持不变");

  flow.BeginStop();
  flow.JoinWorkers();
  dispatcher->Close();
}

GC_TEST(undo_plan_expected_old_value_rejects_branch_moved_after_confirmation) {
  // 用户确认之后、命令发出之前，外部又推进了这条分支：方案里带的预期旧值必须让 Git
  // 原样拒绝，不能从新位置再退一步，也不能留下任何索引/工作区痕迹。
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "第一份\n", true);
  fixture.Commit(L"第一次提交");
  fixture.WriteFile(L"b.txt", "第二份\n", true);
  fixture.Commit(L"第二次提交");

  const std::wstring confirmedHead = fixture.HeadSha();

  gc::platform::RepoDetectRequest detectRequest;
  detectRequest.exePath = fixture.GitExe();
  detectRequest.directory = fixture.RepoDir();
  detectRequest.timeoutMilliseconds = 20000;
  const gc::git::RepoDetection detection =
      gc::platform::DetectRepository(detectRequest, fixture.MakeDetectDeps());
  GC_REQUIRE(detection.kind == gc::git::RepoKind::plainWorktree, "仓库识别应成功");

  gc::platform::UndoProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = detection.root;
  request.timeoutMilliseconds = 20000;
  gc::platform::UndoProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& workingDirectory,
                           const std::vector<std::wstring>& arguments) {
    return fixture.Query(exePath, arguments, workingDirectory, 20000);
  };
  const gc::git::UndoPreflightFacts facts = gc::platform::CollectUndoPreflight(request, deps);

  gc::git::UndoCommitPlanInput input;
  input.facts = facts;
  input.workflow = gc::platform::ProbeRepositoryWorkflowState(detection.absoluteGitDir);
  input.repositoryRoot = detection.root;
  input.captured.valid = true;
  input.captured.hasHead = true;
  input.captured.shortSha = gc::git::ShortObjectId(confirmedHead);
  const gc::git::UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_REQUIRE_MESSAGE(!plan.blocked, gc::platform::WideToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(!plan.arguments.empty() && plan.arguments[0] == L"update-ref",
                   "撤回方案必须发出 update-ref");
  GC_CHECK(plan.expectedOldObjectId == confirmedHead);

  // 确认之后外部推进分支：再造一条提交，分支现在指着新提交。
  fixture.WriteFile(L"c.txt", "第三份\n", true);
  fixture.Commit(L"外部推进的提交");
  const std::wstring movedHead = fixture.HeadSha();
  GC_REQUIRE(movedHead != confirmedHead, "外部推进应改变分支落点");
  const std::wstring indexBeforeExecute = fixture.RunInRepo({L"ls-files", L"-s", L"-z"}).out;

  const gc::test::LinuxGitRun rejected = fixture.RunInRepo(plan.arguments);
  GC_CHECK_MESSAGE(!rejected.Success(),
                   "分支已被推进时，带预期旧值的 update-ref 必须由 Git 拒绝");
  GC_CHECK_MESSAGE(fixture.HeadSha() == movedHead, "被拒绝的执行不得移动分支");
  GC_CHECK_MESSAGE(fixture.RunInRepo({L"ls-files", L"-s", L"-z"}).out == indexBeforeExecute,
                   "被拒绝的执行不得改动索引");
}
