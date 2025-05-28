#include "platform/windows/commit_detail.h"

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId
#include "git/repository.h"      // ClassifyGitFailure / RepoErrorLabel
#include "platform/windows/git_query_result.h"
#include "platform/windows/local_time.h"

namespace gc::platform {

CommitDetailOutcome LoadCommitDetail(const CommitDetailRequest& request,
                                     const CommitDetailDeps& deps) {
  CommitDetailOutcome outcome;
  // 对象 ID 在发起任何查询之前再核一次：界面到工作线程之间隔着好几层，
  // 这里万一拿到的是空串或被污染的文本，宁可当场拒绝，也不把不可信参数交给 Git。
  if (!git::LooksLikeFullObjectId(request.objectId)) {
    outcome.error = git::RepoError::badOutput;
    outcome.message = L"提交对象 ID 不符合约定，未读取提交详情。";
    return outcome;
  }
  if (request.repositoryDirectory.empty()) {
    outcome.error = git::RepoError::inputNotDirectory;
    outcome.message = L"没有可用的仓库工作区根目录，未读取提交详情。";
    return outcome;
  }
  if (!deps.runner) {
    outcome.error = git::RepoError::gitUnavailable;
    outcome.message = L"Git 执行器不可用，未读取提交详情。";
    return outcome;
  }
  if (StopRequested(request.stopFlag)) {
    outcome.error = git::RepoError::gitUnavailable;
    outcome.message = L"程序正在退出，未读取提交详情。";
    return outcome;
  }

  const git::GitQueryResult result =
      deps.runner(request.exePath, request.repositoryDirectory,
                  git::BuildCommitDetailArguments(request.repositoryDirectory, request.objectId));

  std::wstring detail;
  const git::RepoError failure = git::ClassifyGitFailure(result, detail);
  if (failure != git::RepoError::none) {
    outcome.error = failure;
    outcome.message = std::wstring(git::RepoErrorLabel(failure)) + L"（读取提交详情）" +
                      (detail.empty() ? std::wstring() : L"：" + detail);
    return outcome;
  }

  // 两个时间由本机时区规则换算成展示文本，与「最近提交」列表的时间列同一个依据：
  // 同一个秒数在两处必须显示成同一刻，否则用户会以为界面读到了两条不同的提交。
  const git::CommitTimeFormatter formatTime = [](long long epochSeconds) {
    return FormatLocalEpochSeconds(epochSeconds);
  };
  const git::CommitDetailParseResult parsed =
      git::ParseCommitDetail(result.utf16Output, request.objectId, formatTime);
  if (!parsed.ok) {
    outcome.error = git::RepoError::badOutput;
    outcome.message = std::wstring(git::RepoErrorLabel(git::RepoError::badOutput)) +
                      L"（读取提交详情）：" + parsed.error;
    return outcome;
  }

  outcome.detail = parsed.detail;
  outcome.text = git::FormatCommitDetailText(outcome.detail);
  outcome.message = L"提交 " + git::ShortObjectId(request.objectId) + L" 的完整信息已读出。";
  return outcome;
}

CommitDetailDeps MakeCommitDetailDeps(unsigned long timeoutMilliseconds) {
  CommitDetailDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  return deps;
}

CommitDetailOutcome RunCommitDetailLoad(const CommitDetailRequest& request) {
  // 身份回显必须原样带回：判定「这份结果还收不算数」的是界面线程，而它只看得到这里带回的值。
  CommitDetailOutcome outcome =
      LoadCommitDetail(request, MakeCommitDetailDeps(request.timeoutMilliseconds));
  // objectId 也属于身份回显：用户连着点了好几条提交时，光有读取序号不足以说明「这是哪一条的详情」。
  outcome.objectId = request.objectId;
  outcome.readSerial = request.readSerial;
  outcome.bindingGeneration = request.bindingGeneration;
  return outcome;
}

}  // namespace gc::platform
