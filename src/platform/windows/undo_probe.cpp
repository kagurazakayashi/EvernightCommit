#include "platform/windows/undo_probe.h"

#include <vector>

#include "git/workspace_status.h"
#include "platform/windows/git_query_result.h"

namespace gc::platform {
namespace {

git::GitQueryResult RunQuery(const git::GitQueryRunner& runner, const std::wstring& exePath,
                             const std::wstring& directory,
                             const std::vector<std::wstring>& arguments) {
  if (!runner || arguments.empty()) {
    git::GitQueryResult failed;
    failed.started = false;
    return failed;
  }
  return runner(exePath, directory, arguments);
}

git::UndoPreflightFacts FailedFacts(std::wstring reason) {
  git::UndoPreflightFacts facts;
  facts.head.queryFailure = std::move(reason);
  facts.statusDetail = L"预检未能发起";
  return facts;
}

}  // namespace

git::UndoPreflightFacts CollectUndoPreflight(const UndoProbeRequest& request,
                                             const UndoProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedFacts(L"没有可用的仓库工作区根目录");
  }
  if (!deps.runner) {
    return FailedFacts(L"没有可用的 Git 程序");
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::UndoPreflightQueries queries;
  queries.symbolicRef = RunQuery(deps.runner, request.exePath, dir,
                                 git::BuildUndoSymbolicRefArguments(dir));
  queries.headCommit =
      RunQuery(deps.runner, request.exePath, dir, git::BuildUndoHeadCommitArguments(dir));
  // 退出收尾：停止信号已起就不再发后续查询（这一趟结果随窗口一起作废）。
  if (StopRequested(request.stopFlag)) {
    return FailedFacts(L"程序正在退出，预检中止");
  }

  // HEAD 可解析才追問父提交、父對象、淺倉庫狀態、標題與遠端包含：那種查詢在空倉庫裡必然非 0 退出，
  // 不是錯誤卻要解釋；而且沒有 HEAD ID 也沒有可查詢的目標。
  const git::UndoQueryRead headRead = git::ReadUndoQuery(queries.headCommit);
  if (headRead.outcome == git::UndoQueryOutcome::answered &&
      !git::BuildUndoRemoteContainsArguments(dir, headRead.firstLine).empty()) {
    queries.commitDependentRan = true;
    const std::wstring& headSha = headRead.firstLine;  // 已經過完整對象 ID 形態校驗
    queries.parents =
        RunQuery(deps.runner, request.exePath, dir, git::BuildUndoParentsArguments(dir, headSha));
    queries.commitObject =
        RunQuery(deps.runner, request.exePath, dir, git::BuildUndoCommitObjectArguments(dir, headSha));
    queries.shallowState = RunQuery(deps.runner, request.exePath, dir,
                                    git::BuildUndoShallowStateArguments(dir));
    // 只問「歷史視圖給出的那個第一父」讀不讀得到；兩份證據一致時它就是撤回的目標，
    // 不一致時判讀層本來就會拒絕，不需要再多問一條。
    const std::wstring firstParent = git::UndoFirstReportedParent(queries.parents);
    const std::vector<std::wstring> parentObjectArguments =
        git::BuildUndoParentObjectArguments(dir, firstParent);
    if (!parentObjectArguments.empty()) {
      queries.parentObjectRan = true;
      queries.parentObjectQueryOid = firstParent;
      queries.parentObject =
          RunQuery(deps.runner, request.exePath, dir, parentObjectArguments);
    }
    queries.headSummary =
        RunQuery(deps.runner, request.exePath, dir, git::BuildUndoHeadSummaryArguments(dir, headSha));
    queries.remoteRefs = RunQuery(deps.runner, request.exePath, dir, git::BuildUndoRemoteRefsArguments(dir));
    queries.remoteContains =
        RunQuery(deps.runner, request.exePath, dir, git::BuildUndoRemoteContainsArguments(dir, headSha));
  }
  // 「这个名字本身是不是符号引用」与「这条分支有没有被别的工作树用着」：只有问出了完整分支引用名
  // 才发得出去。判读层把「没问过」一律当成不能放行，所以这里不问就不能假装问过。
  if (StopRequested(request.stopFlag)) {
    return FailedFacts(L"程序正在退出，预检中止");
  }
  const git::UndoHeadFacts earlyHead =
      git::InterpretUndoHeadSnapshot(queries.symbolicRef, queries.headCommit);
  queries.worktreeRoot = dir;
  if (earlyHead.onBranch) {
    queries.branchSymref =
        RunQuery(deps.runner, request.exePath, dir,
                 git::BuildRefSymbolicProbeArguments(dir, earlyHead.branchRef));
    queries.worktrees = RunQuery(deps.runner, request.exePath, dir, git::BuildWorktreeListArguments(dir));
  }
  if (StopRequested(request.stopFlag)) {
    return FailedFacts(L"程序正在退出，预检中止");
  }
  queries.status = RunQuery(deps.runner, request.exePath, dir, git::BuildWorkspaceStatusArguments(dir));
  return git::InterpretUndoPreflight(queries);
}

UndoProbeDeps MakeUndoProbeDeps(unsigned long timeoutMilliseconds) {
  UndoProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  return deps;
}

git::UndoHeadFacts CaptureUndoHeadSnapshot(const std::wstring& exePath,
                                           const std::wstring& repositoryDirectory,
                                           unsigned long timeoutMilliseconds) {
  UndoProbeDeps deps = MakeUndoProbeDeps(timeoutMilliseconds);
  const std::wstring dir = repositoryDirectory;
  const git::GitQueryResult symbolicRef =
      RunQuery(deps.runner, exePath, dir, git::BuildUndoSymbolicRefArguments(dir));
  const git::GitQueryResult headCommit =
      RunQuery(deps.runner, exePath, dir, git::BuildUndoHeadCommitArguments(dir));
  git::UndoHeadFacts head = git::InterpretUndoHeadSnapshot(symbolicRef, headCommit);
  // 确认后这一轮问的不只是「分支和那份提交还没变」：名字本身是不是符号引用、这条分支有没有被
  // 别的工作树检出，都得按此刻的现场重问一次（预检那一刻的答案不属于现在）。
  if (head.onBranch && git::IsSafeUndoTargetRef(head.branchRef)) {
    const git::GitQueryResult branchSymref =
        RunQuery(deps.runner, exePath, dir, git::BuildRefSymbolicProbeArguments(dir, head.branchRef));
    const git::GitQueryResult worktrees =
        RunQuery(deps.runner, exePath, dir, git::BuildWorktreeListArguments(dir));
    head.refIntegrity =
        git::InterpretRefIntegrity(branchSymref, worktrees, head.branchRef, dir);
  }
  return head;
}

UndoProbeOutcome RunUndoProbeLoad(const UndoProbeRequest& request) {
  UndoProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原樣回顯：判別在界面線程。
  outcome.facts = CollectUndoPreflight(request, MakeUndoProbeDeps(request.timeoutMilliseconds));
  return outcome;
}

}  // namespace gc::platform
