#pragma once

#include <string>
#include <utility>

#include "git/workspace_status.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次工作區讀取任務的請求：倉庫目錄取識別結果的工作區根，不取輸入框原文
// （用戶可能選了子目錄，也可能選了根本沒有工作區的裸倉庫）。
struct WorkspaceStatusRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  unsigned long timeoutMilliseconds = 0;
  // HEAD 是否可解析（仓库至少有一条提交）。由仓库识别时 Git 自己的回答带来（headResolved），
  // 尚无提交的仓库根本不发 git log——那条命令在空仓库里必然非 0 退出，不是错误却要解释。
  bool repositoryHasCommits = false;
  // 讀取憑證的回顯欄位：界面把本次讀取的序號與倉庫身份版本填在這裡，
  // 任務體原樣帶回。切換倉庫後才遲遲完成的舊讀取就是靠它被判為過期，
  // 而不是靠「是不是最後一次提交」——換到一個讀不了的倉庫時根本不會再有新的提交。
  unsigned long long readSerial = 0;
  unsigned long long bindingGeneration = 0;
};

// 一次後台讀取的成品：快照加上請求攜帶的身份回顯。
struct WorkspaceLoadOutcome {
  git::WorkspaceSnapshot snapshot;
  unsigned long long readSerial = 0;
  unsigned long long bindingGeneration = 0;
};

// 執行依賴以回調注入，使「怎麼問、怎麼解析」的分類邏輯可脫離 Win32 用樁輸出測試。
struct WorkspaceStatusDeps {
  git::GitQueryRunner runner;
};

// 只讀地取回並解析工作區狀態與最近提交歷史：一次 git status，外加（僅當 HEAD 可解析時）
// 一次 git log。兩條都是隱藏窗口的只讀查詢，不寫對象庫、不訪問遠端、不改動倉庫。
// 兩者的成敗互相獨立：status 失敗時整體作廢；log 失敗時列表照常落地，
// 失敗原因寫進 snapshot 的 historyError/historyMessage，界面不會把「讀不到」顯示成「沒有提交」。
[[nodiscard]] git::WorkspaceSnapshot LoadWorkspaceStatus(const WorkspaceStatusRequest& request,
                                                          const WorkspaceStatusDeps& deps);

// 用本工程的隱藏窗口子進程執行器裝配 WorkspaceStatusDeps。
[[nodiscard]] WorkspaceStatusDeps MakeWorkspaceStatusDeps(unsigned long timeoutMilliseconds);

// 工作線程任務體：裝配依賴並執行讀取，把請求攜帶的讀取序號與身份版本原樣帶进結果。
[[nodiscard]] WorkspaceLoadOutcome RunWorkspaceStatusLoad(const WorkspaceStatusRequest& request);

// 工作區讀取的後台控制器：讀取在工作線程執行，GUI 線程不凍結；
// 連續切換倉庫時舊結果按序號作廢，不會把上個倉庫的列表混進新倉庫。
using WorkspaceStatusWorker = GitTaskWorker<WorkspaceStatusRequest, WorkspaceLoadOutcome>;

}  // namespace gc::platform
