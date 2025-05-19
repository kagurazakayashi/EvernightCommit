#include "platform/windows/restore_probe.h"

#include <vector>

#include "platform/windows/git_query_result.h"  // RunGitBackgroundQuery
#include "platform/windows/win_path.h"          // ProbeRepositoryWorkflowState

namespace gc::platform {

git::RestorePreflightQueries CollectRestoreFacts(const RestoreProbeRequest& request) {
  git::RestorePreflightQueries queries;
  if (request.repositoryDirectory.empty()) {
    return queries;  // 没有仓库落点：branchRan 保持 false，判读层据此走「未能判定」。
  }
  const std::wstring& dir = request.repositoryDirectory;

  const std::vector<std::wstring> branchArguments =
      git::BuildRestoreBranchValueArguments(dir, request.branchRef);
  if (!branchArguments.empty()) {
    queries.branchRan = true;
    queries.branchValue =
        RunGitBackgroundQuery(request.exePath, branchArguments, dir, request.timeoutMilliseconds);
  }
  if (StopRequested(request.stopFlag)) {
    return queries;  // 退出收尾：剩余查询不再发起，这一趟结果随窗口作废。
  }

  // 删除形态没有「要挪去的对象」可验，根本不问那条查询（与撤回预检「没有目标就不发」同理）。
  if (!request.isRootDeletion && !request.moveToOid.empty()) {
    const std::vector<std::wstring> moveToArguments =
        git::BuildRestoreTargetObjectArguments(dir, request.moveToOid);
    if (!moveToArguments.empty()) {
      queries.moveToRan = true;
      queries.moveToValue =
          RunGitBackgroundQuery(request.exePath, moveToArguments, dir, request.timeoutMilliseconds);
    }
  }

  // 引用名自身的形态与占用：`update-ref` 默认追随符号引用，而这条分支也可能正被别的工作树用着。
  // 这两条都在问分支现值之后问，任何一条没答上，判读层就只能按「不知道」拒绝（不是「没问题」）。
  if (queries.branchRan && !request.branchRef.empty()) {
    if (StopRequested(request.stopFlag)) {
      return queries;
    }
    const std::vector<std::wstring> symrefArguments =
        git::BuildRefSymbolicProbeArguments(dir, request.branchRef);
    const std::vector<std::wstring> worktreeArguments = git::BuildWorktreeListArguments(dir);
    if (!symrefArguments.empty() && !worktreeArguments.empty()) {
      queries.refKindRan = true;
      queries.currentWorktreeRoot = dir;
      queries.branchSymref =
          RunGitBackgroundQuery(request.exePath, symrefArguments, dir, request.timeoutMilliseconds);
      if (!StopRequested(request.stopFlag)) {
        queries.worktrees =
            RunGitBackgroundQuery(request.exePath, worktreeArguments, dir, request.timeoutMilliseconds);
      } else {
        queries.refKindRan = false;  // 只问到一半：另一半压根没问，判读层据此走「未能判定」。
      }
    }
  }

  // 流程痕迹用「带第三态」的探法：读不动就是不能确定，绝不返回六个 false 冒充「没有流程停着」。
  if (!request.absoluteGitDir.empty()) {
    const git::WorkflowProbeResult probe =
        ProbeRepositoryWorkflowStateStrict(request.absoluteGitDir);
    queries.workflow = probe.state;
    queries.workflowProbed = true;
    queries.workflowReadable = probe.readable;
    queries.workflowFailure = probe.failure;
  }
  return queries;
}

RestoreProbeOutcome RunRestoreProbeLoad(const RestoreProbeRequest& request) {
  RestoreProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  outcome.queries = CollectRestoreFacts(request);
  return outcome;
}

git::RestoreRecheckScene CaptureRestoreRecheck(const RestoreProbeRequest& request) {
  // 与 CollectRestoreFacts 同一组查询、同一个落点：预检问什么，复核就问什么。
  // 差别只在用途——这一份是用来判定「旧的那份确认还属不属于此刻」。
  git::RestoreRecheckScene scene;
  if (request.repositoryDirectory.empty() || request.branchRef.empty()) {
    return scene;  // 连问哪儿都不知道：一切按「没问到」，判读层据此拒绝。
  }
  const std::wstring& dir = request.repositoryDirectory;

  const std::vector<std::wstring> branchArguments =
      git::BuildRestoreBranchValueArguments(dir, request.branchRef);
  if (!branchArguments.empty()) {
    scene.branchRan = true;
    scene.branchValue =
        RunGitBackgroundQuery(request.exePath, branchArguments, dir, request.timeoutMilliseconds);
  }
  if (StopRequested(request.stopFlag)) {
    return scene;
  }
  const std::vector<std::wstring> symrefArguments =
      git::BuildRefSymbolicProbeArguments(dir, request.branchRef);
  const std::vector<std::wstring> worktreeArguments = git::BuildWorktreeListArguments(dir);
  if (!symrefArguments.empty() && !worktreeArguments.empty()) {
    const git::GitQueryResult symref =
        RunGitBackgroundQuery(request.exePath, symrefArguments, dir, request.timeoutMilliseconds);
    scene.integrityRan = !StopRequested(request.stopFlag);
    const git::GitQueryResult worktrees =
        scene.integrityRan
            ? RunGitBackgroundQuery(request.exePath, worktreeArguments, dir,
                                    request.timeoutMilliseconds)
            : git::GitQueryResult{};
    scene.integrity =
        git::InterpretRefIntegrity(symref, worktrees, request.branchRef, dir);
  }
  if (!request.absoluteGitDir.empty() && !StopRequested(request.stopFlag)) {
    const git::WorkflowProbeResult probe =
        ProbeRepositoryWorkflowStateStrict(request.absoluteGitDir);
    scene.workflowProbed = true;
    scene.workflowReadable = probe.readable;
    scene.workflow = probe.state;
    scene.workflowFailure = probe.failure;
  }
  return scene;
}

}  // namespace gc::platform
