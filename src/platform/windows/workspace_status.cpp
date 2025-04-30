#include "platform/windows/workspace_status.h"

#include "git/commit_history.h"
#include "platform/windows/git_query_result.h"
#include "platform/windows/local_time.h"

namespace gc::platform {
namespace {

// 在同一次讀取里追問提交歷史。失敗只記錄在 snapshot 的 historyError/historyMessage 上，
// 不拖垮已經讀好的兩個文件列表；尚無提交的倉庫不發這條查詢（git log 在那裡必然非 0 退出）。
void LoadRecentCommits(const WorkspaceStatusRequest& request, const WorkspaceStatusDeps& deps,
                       git::WorkspaceSnapshot* snapshot) {
  if (!request.repositoryHasCommits) {
    return;
  }
  if (StopRequested(request.stopFlag)) {
    // 退出收尾：不再追问提交历史，按「没读到」如实记录，列表本身照常（这一趟随窗口作废）。
    snapshot->historyError = git::RepoError::gitUnavailable;
    snapshot->historyMessage = L"程序正在退出，未读取提交历史。文件列表来自 git status，仍然照常显示。";
    return;
  }
  const git::GitQueryResult result =
      deps.runner(request.exePath, request.repositoryDirectory,
                  git::BuildRecentCommitsArguments(request.repositoryDirectory,
                                                   git::kRecentCommitLimit));
  std::wstring detail;
  const git::RepoError failure = git::ClassifyGitFailure(result, detail);
  if (failure != git::RepoError::none) {
    snapshot->historyError = failure;
    snapshot->historyMessage = std::wstring(git::RepoErrorLabel(failure)) +
                               L"（读取提交历史）" +
                               (detail.empty() ? std::wstring() : L"：" + detail) +
                               L"。文件列表来自 git status，仍然照常显示。";
    return;
  }

  const git::CommitTimeFormatter formatTime = [](long long epochSeconds) {
    return FormatLocalEpochSeconds(epochSeconds);
  };
  const git::CommitHistoryParseResult parsed =
      git::ParseRecentCommits(result.utf16Output, git::kRecentCommitLimit, formatTime);
  if (!parsed.error.empty()) {
    snapshot->historyError = git::RepoError::badOutput;
    snapshot->historyMessage =
        std::wstring(git::RepoErrorLabel(git::RepoError::badOutput)) +
        L"（读取提交历史）：" + parsed.error + L"。文件列表来自 git status，仍然照常显示。";
    return;
  }

  snapshot->model.recentCommits = std::move(parsed.commits);
  const std::wstring notice = git::BuildRecentCommitsNotice(snapshot->model.recentCommits.size(),
                                                            parsed.truncated);
  if (!notice.empty()) {
    snapshot->message += L"；" + notice;
  }
}

}  // namespace

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
  // 工作區讀好后才輪到提交歷史：兩條查詢共用這一次刷新，不另建觸發機制。
  LoadRecentCommits(request, deps, &snapshot);
  if (snapshot.historyError != git::RepoError::none) {
    snapshot.message += L" " + snapshot.historyMessage;
  }
  return snapshot;
}

WorkspaceStatusDeps MakeWorkspaceStatusDeps(unsigned long timeoutMilliseconds) {
  WorkspaceStatusDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    // 機器輸出以 NUL 分隔，平台邊界只做 UTF-8→UTF-16 解碼，不改寫任何分隔符與路徑字節。
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
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
