#pragma once

#include <string>
#include <utility>

#include "git/undo_commit_plan.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「撤回前預檢」的請求。工作區根取識別結果，不取輸入框原文；
// repositoryDirectory 原樣回顯進結果，界面據此判別「這份預檢還是不是這個倉庫的」。
struct UndoProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  unsigned long timeoutMilliseconds = 0;
  StopFlag stopFlag;  // 由 worker 挂上：退出收尾时逐条查询前核对，剩余查询不再发起。
};

// 一次後臺預檢的成品：判讀結果加請求攜帶的目錄回顯。
struct UndoProbeOutcome {
  git::UndoPreflightFacts facts;
  std::wstring repositoryDirectory;
};

// 執行依賴以回調注入，使「問哪幾條、怎麼判讀」的編排可脫離 Win32 用真實臨時倉庫測試。
struct UndoProbeDeps {
  git::GitQueryRunner runner;
};

// 只讀地發起一組撤回預檢查詢（分支 / HEAD 完整 ID / 父提交 / 提交對象自己的 parent 行 /
// 是否淺倉庫 / 目標父對象可讀性 / 標題 / 遠端跟蹤引用 / 工作區狀態），
// 並把回答交給 git/undo_commit_plan 判讀。全程隱藏窗口子進程，不彈命令窗口、不寫對象庫、
// 不訪問遠端；HEAD 不可解析時根本不發依賴它的那幾條查詢（那種倉庫本來也沒有可撤回的提交），
// 歷史視圖裡沒有父提交時也不發「父對象可讀性」那條查詢（沒有目標可問）。
[[nodiscard]] git::UndoPreflightFacts CollectUndoPreflight(const UndoProbeRequest& request,
                                                           const UndoProbeDeps& deps);

// 用本工程的隱藏窗口子進程執行器裝配 UndoProbeDeps。
[[nodiscard]] UndoProbeDeps MakeUndoProbeDeps(unsigned long timeoutMilliseconds);

// 用戶在確認框點頭之後、啓動命令窗口之前，還要比對一次 HEAD 與分支。這一步走
// GitTaskWorker 在後台執行（不凍結界面）：返回的 facts 只填 symbolic-ref/rev-parse
// 相關字段，超時/啟動失敗如實記進 queryOk=false，由調用方按「複核不過就放棄執行」處理。
[[nodiscard]] git::UndoHeadFacts CaptureUndoHeadSnapshot(const std::wstring& exePath,
                                                         const std::wstring& repositoryDirectory,
                                                         unsigned long timeoutMilliseconds);

// 工作線程任務體：裝配依賴並執行預檢，請求攜帶的目錄原樣帶進結果。
[[nodiscard]] UndoProbeOutcome RunUndoProbeLoad(const UndoProbeRequest& request);

// 預檢的後臺控制器：查詢在工作線程執行，GUI 線程不凍結；
// 連續點擊時舊結果按序號作廢，不會把上一個倉庫的事實混進確認框。
using UndoProbeWorker = GitTaskWorker<UndoProbeRequest, UndoProbeOutcome>;

// 「點頭之後、發命令之前」的 HEAD/分支複核也走後台：同一種請求、只取回這一組事實。
// 遲到的舊複核按序號作廢；複核沒回來之前不啟動命令窗口（複核不過就放棄執行）。
using UndoHeadRecheckWorker = GitTaskWorker<UndoProbeRequest, git::UndoHeadFacts>;

}  // namespace gc::platform
