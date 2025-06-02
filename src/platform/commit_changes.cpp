#include "platform/commit_changes.h"

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId
#include "git/repository.h"      // ClassifyGitFailure / RepoErrorLabel
#include "platform/git_query.h"

namespace gc::platform {

CommitChangesOutcome LoadCommitChanges(const CommitChangesRequest& request,
                                            const CommitChangesDeps& deps) {
  CommitChangesOutcome outcome;
  // 对象 ID 在发起任何查询之前再核一次：界面到工作线程之间隔着好几层，
  // 这里万一拿到的是空串或被污染的文本，宁可当场拒绝，也不把不可信参数交给 Git。
  if (!git::LooksLikeFullObjectId(request.objectId)) {
    outcome.error = git::RepoError::badOutput;
    outcome.message = L"提交对象 ID 不符合约定，未读取文件变化。";
    return outcome;
  }
  if (request.repositoryDirectory.empty()) {
    outcome.error = git::RepoError::inputNotDirectory;
    outcome.message = L"没有可用的仓库工作区根目录，未读取文件变化。";
    return outcome;
  }
  if (!deps.runner) {
    outcome.error = git::RepoError::gitUnavailable;
    outcome.message = L"Git 执行器不可用，未读取文件变化。";
    return outcome;
  }
  if (StopRequested(request.stopFlag)) {
    outcome.error = git::RepoError::gitUnavailable;
    outcome.message = L"程序正在退出，未读取提交文件变化。";
    return outcome;
  }

  const git::GitQueryResult result =
      deps.runner(request.exePath, request.repositoryDirectory,
                  git::BuildCommitChangesArguments(request.repositoryDirectory, request.objectId));

  std::wstring detail;
  const git::RepoError failure = git::ClassifyGitFailure(result, detail);
  if (failure != git::RepoError::none) {
    outcome.error = failure;
    outcome.message = std::wstring(git::RepoErrorLabel(failure)) + L"（读取提交文件变化）" +
                      (detail.empty() ? std::wstring() : L"：" + detail);
    return outcome;
  }

  const git::CommitChangesParseResult parsed = git::ParseCommitChanges(result.utf16Output);
  if (!parsed.error.empty()) {
    outcome.error = git::RepoError::badOutput;
    outcome.message = std::wstring(git::RepoErrorLabel(git::RepoError::badOutput)) +
                      L"（读取提交文件变化）：" + parsed.error;
    return outcome;
  }

  outcome.changes = parsed.changes;
  const std::wstring shortId = git::ShortObjectId(request.objectId);
  if (outcome.changes.empty()) {
    // 空清单是合法事实（空提交，或合并提交的变化都在第一个父提交里），如实说明，不装作没读到。
    outcome.message = L"提交 " + shortId + L" 相对第一个父提交没有文件变化。";
  } else {
    outcome.message = L"提交 " + shortId + L" 的文件变化 " +
                      std::to_wstring(outcome.changes.size()) + L" 条。";
  }
  return outcome;
}

CommitChangesDeps MakeCommitChangesDeps(unsigned long timeoutMilliseconds, StopFlag cancel) {
  CommitChangesDeps deps;
  deps.runner = [timeoutMilliseconds, cancel](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds, cancel);
  };
  return deps;
}

CommitChangesOutcome RunCommitChangesLoad(const CommitChangesRequest& request) {
  // 身份回显必须原样带回：判定「这份结果还收不算数」的是界面线程，而它只看得到这里带回的值。
  CommitChangesOutcome outcome =
      LoadCommitChanges(request, MakeCommitChangesDeps(request.timeoutMilliseconds, request.stopFlag));
  // objectId 也属于身份回显：用户连着点了好几条提交时，光有读取序号还不足以说明「这是哪一条的清单」。
  outcome.objectId = request.objectId;
  outcome.readSerial = request.readSerial;
  outcome.bindingGeneration = request.bindingGeneration;
  return outcome;
}

}  // namespace gc::platform
