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
};

// 執行依賴以回調注入，使「怎麼問、怎麼解析」的分類邏輯可脫離 Win32 用樁輸出測試。
struct WorkspaceStatusDeps {
  git::GitQueryRunner runner;
};

// 只讀地取回並解析工作區狀態：一條 git status 查詢，不寫對象庫、不訪問遠端、不改動倉庫。
[[nodiscard]] git::WorkspaceSnapshot LoadWorkspaceStatus(const WorkspaceStatusRequest& request,
                                                          const WorkspaceStatusDeps& deps);

// 用本工程的隱藏窗口子進程執行器裝配 WorkspaceStatusDeps。
[[nodiscard]] WorkspaceStatusDeps MakeWorkspaceStatusDeps(unsigned long timeoutMilliseconds);

// 工作線程任務體：裝配依賴並執行讀取（GitQueryRunner 在這裡綁定子進程執行器）。
[[nodiscard]] git::WorkspaceSnapshot RunWorkspaceStatusLoad(const WorkspaceStatusRequest& request);

// 工作區讀取的後台控制器：讀取在工作線程執行，GUI 線程不凍結；
// 連續切換倉庫時舊結果按序號作廢，不會把上個倉庫的列表混進新倉庫。
using WorkspaceStatusWorker = GitTaskWorker<WorkspaceStatusRequest, git::WorkspaceSnapshot>;

}  // namespace gc::platform
