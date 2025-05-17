#include "platform/windows/restore_probe.h"

#include <vector>

#include "platform/windows/git_query_result.h"  // RunGitBackgroundQuery
#include "platform/windows/win_path.h"          // ProbeRepositoryWorkflowState

namespace gc::platform {
namespace {

git::GitQueryResult NotStartedResult() {
  git::GitQueryResult result;
  result.started = false;
  return result;
}

}  // namespace

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

  if (!request.absoluteGitDir.empty()) {
    queries.workflow = ProbeRepositoryWorkflowState(request.absoluteGitDir);
    queries.workflowProbed = true;
  }
  return queries;
}

RestoreProbeOutcome RunRestoreProbeLoad(const RestoreProbeRequest& request) {
  RestoreProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  outcome.queries = CollectRestoreFacts(request);
  return outcome;
}

git::GitQueryResult CaptureRestoreBranchRecheck(const std::wstring& exePath,
                                                const std::wstring& repositoryDirectory,
                                                const std::wstring& branchRef,
                                                unsigned long timeoutMilliseconds) {
  const std::vector<std::wstring> arguments =
      git::BuildRestoreBranchValueArguments(repositoryDirectory, branchRef);
  if (repositoryDirectory.empty() || arguments.empty()) {
    return NotStartedResult();
  }
  return RunGitBackgroundQuery(exePath, arguments, repositoryDirectory, timeoutMilliseconds);
}

}  // namespace gc::platform
