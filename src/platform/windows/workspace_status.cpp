#include "platform/windows/workspace_status.h"

#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"

namespace gc::platform {

git::WorkspaceSnapshot LoadWorkspaceStatus(const WorkspaceStatusRequest& request,
                                           const WorkspaceStatusDeps& deps) {
  git::WorkspaceSnapshot snapshot;
  if (request.repositoryDirectory.empty()) {
    // 沒有可用的工作區根目錄就沒有任何安全落點，不發查詢。
    snapshot.status = git::WorkspaceLoadStatus::failed;
    snapshot.error = git::RepoError::inputNotDirectory;
    snapshot.message = git::BuildWorkspaceFailureMessage(snapshot.error, request.repositoryDirectory);
    return snapshot;
  }
  if (!deps.runner) {
    snapshot.status = git::WorkspaceLoadStatus::failed;
    snapshot.error = git::RepoError::gitUnavailable;
    snapshot.message = git::BuildWorkspaceFailureMessage(snapshot.error, {});
    return snapshot;
  }

  // 顯式 -C 並把子進程工作目錄綁定到該倉庫：不依賴也不改動本進程的全局當前目錄。
  const git::GitQueryResult result = deps.runner(request.exePath, request.repositoryDirectory,
                                                 git::BuildWorkspaceStatusArguments(request.repositoryDirectory));

  std::wstring detail;
  const git::RepoError failure = git::ClassifyGitFailure(result, detail);
  if (failure != git::RepoError::none) {
    snapshot.status = git::WorkspaceLoadStatus::failed;
    snapshot.error = failure;
    snapshot.message = git::BuildWorkspaceFailureMessage(failure, detail);
    return snapshot;
  }

  const git::WorkspaceStatusParseResult parsed = git::ParseWorkspacePorcelainV2(result.utf16Output);
  if (!parsed.error.empty()) {
    // 輸出不合約定：整份丟棄而不是採用解析出來的一半，見 ParseWorkspacePorcelainV2 的說明。
    snapshot.status = git::WorkspaceLoadStatus::failed;
    snapshot.error = git::RepoError::badOutput;
    snapshot.message = git::BuildWorkspaceFailureMessage(snapshot.error, parsed.error);
    return snapshot;
  }

  snapshot.status = git::WorkspaceLoadStatus::loaded;
  snapshot.model = std::move(parsed.model);
  snapshot.skippedRecords = parsed.skippedRecords;
  snapshot.message = git::BuildWorkspaceSummary(snapshot.model);
  if (snapshot.skippedRecords > 0) {
    // 認識但刻意不進列表的記錄（如 --ignored 未請求、# 頭部記錄）要讓使用者知道不是漏讀。
    snapshot.message += L"（另有 " + std::to_wstring(snapshot.skippedRecords) +
                        L" 条 Git 记录按约定未列入。）";
  }
  return snapshot;
}

WorkspaceStatusDeps MakeWorkspaceStatusDeps(unsigned long timeoutMilliseconds) {
  WorkspaceStatusDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    // 機器輸出以 NUL 分隔，平台邊界只做 UTF-8→UTF-16 解碼，不改寫任何分隔符與路徑字節。
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

WorkspaceLoadOutcome RunWorkspaceStatusLoad(const WorkspaceStatusRequest& request) {
  // 身份回顯必須原樣帶回：判定「這份結果還算不算數」的是界面執行緒，而它只看得到這裡帶回的值。
  WorkspaceLoadOutcome outcome;
  outcome.readSerial = request.readSerial;
  outcome.bindingGeneration = request.bindingGeneration;
  outcome.snapshot = LoadWorkspaceStatus(request, MakeWorkspaceStatusDeps(request.timeoutMilliseconds));
  return outcome;
}

}  // namespace gc::platform
