#include "platform/windows/author_config.h"

#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"

namespace gc::platform {

git::AuthorIdentityConfig LoadAuthorIdentity(const AuthorConfigRequest& request,
                                             const AuthorConfigDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    // 沒有可用的工作區根目錄就沒有查詢落點：一律歸為「查不到」，讓界面保留原有輸入。
    git::ConfigValueRead refusal;
    refusal.state = git::ConfigValueState::failed;
    refusal.error = git::RepoError::inputNotDirectory;
    refusal.detail = L"没有可用的仓库工作区根目录";
    return git::CombineIdentityConfig(refusal, refusal);
  }
  if (!deps.runner) {
    git::ConfigValueRead refusal;
    refusal.state = git::ConfigValueState::failed;
    refusal.error = git::RepoError::gitUnavailable;
    refusal.detail = L"没有可用的 Git 程序";
    return git::CombineIdentityConfig(refusal, refusal);
  }

  const git::ConfigValueRead name = git::ParseIdentityConfigQuery(
      deps.runner(request.exePath, request.repositoryDirectory,
                  git::BuildIdentityConfigArguments(request.repositoryDirectory,
                                                    git::kUserConfigKeys[0])),
      git::kUserConfigKeys[0]);
  if (name.state == git::ConfigValueState::failed) {
    // 第一條查詢就以結構性原因失敗（啟動不了、逾時、目錄不是倉庫、設定檔損壞……），
    // 第二條只會得到同樣的答案：不再多起一個程序，直接把同一份失敗交給兩個欄位。
    return git::CombineIdentityConfig(name, name);
  }

  const git::ConfigValueRead email = git::ParseIdentityConfigQuery(
      deps.runner(request.exePath, request.repositoryDirectory,
                  git::BuildIdentityConfigArguments(request.repositoryDirectory,
                                                    git::kUserConfigKeys[1])),
      git::kUserConfigKeys[1]);
  return git::CombineIdentityConfig(name, email);
}

AuthorConfigDeps MakeAuthorConfigDeps(unsigned long timeoutMilliseconds) {
  AuthorConfigDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    // --null 的輸出以 NUL 收尾：平台邊界只做 UTF-8→UTF-16 解碼，不修剪空白，也不增刪任何分隔字節，
    // 否則「值本身以空白開頭/結尾」會被誤讀成另一個身份。
    const SubprocessRunResult run = RunHiddenCaptured(exePath, arguments, directory, timeoutMilliseconds);
    git::GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = Utf8ToUtf16(run.utf8Stdout);
    result.utf16Error = Utf8ToUtf16(run.utf8Stderr);
    return result;
  };
  return deps;
}

AuthorConfigOutcome RunAuthorIdentityLoad(const AuthorConfigRequest& request) {
  AuthorConfigOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原樣回顯：判別在界面執行緒。
  outcome.config = LoadAuthorIdentity(request, MakeAuthorConfigDeps(request.timeoutMilliseconds));
  return outcome;
}

}  // namespace gc::platform
