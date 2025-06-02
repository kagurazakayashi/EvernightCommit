#include "platform/git_query.h"

#include <utility>

#include "platform/linux/environment.h"
#include "platform/linux/subprocess.h"

namespace gc::platform {

ProcessRunResult RunGitCaptured(std::wstring_view program, const std::vector<std::wstring>& arguments,
                                std::wstring_view workingDirectory, unsigned long timeoutMilliseconds,
                                StopFlag cancel, std::wstring* environmentNotice) {
  const PosixChildEnvironment environment =
      BuildGitChildEnvironment(git::GitRunPurpose::backgroundProbe, {});
  if (environment.entries.empty()) {
    // 装配失败就不启动：与其让 Git 拿着一个未知的环境去猜仓库，不如这一次查询明确没有答案。
    ProcessRunResult failed;
    failed.launchErrorText = environment.failureReason;
    return failed;
  }
  if (environmentNotice != nullptr) {
    *environmentNotice = environment.notice;
  }
  ProcessRequest request;
  request.program = std::wstring(program);
  request.arguments = arguments;
  request.workingDirectory = std::wstring(workingDirectory);
  request.timeoutMilliseconds = timeoutMilliseconds;
  request.cancel = std::move(cancel);
  return RunCapturedProcess(request, environment.entries);
}

git::GitQueryResult RunGitBackgroundQuery(const std::wstring& exePath,
                                          const std::vector<std::wstring>& arguments,
                                          const std::wstring& workingDirectory,
                                          unsigned long timeoutMilliseconds, StopFlag cancel) {
  std::wstring notice;
  git::GitQueryResult result =
      MakeGitQueryResult(RunGitCaptured(exePath, arguments, workingDirectory, timeoutMilliseconds,
                                        std::move(cancel), &notice));
  result.environmentNotice = std::move(notice);
  return result;
}

}  // namespace gc::platform
