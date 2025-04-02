#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "git/command_window.h"

namespace gc::app {

// 本模組是「什麼時候可以讀、讀到的結果還算不算數」的單一裁決點：
// 界面負責發起與落地，GitTaskWorker / CommandWindowRunner 負責執行，
// 而身份版本、合併槽與單槽互斥都集中在這裡，因此可以用可控的亂序完成來測試，
// 不需要真的開窗口或跑 Git。
//
// 為什麼需要身份版本：後台讀取是異步的，使用者可能在結果回來之前就已換了倉庫
// 或改了 Git 程序路徑。若只按「最後一次提交」作廢，切到一個沒有工作區的倉庫
// （裸倉庫、识别失敗）就不會再發起新的讀取，舊倉庫的列表會被當成現狀顯示。

// RequestRefresh 的調度結論。
enum class RefreshSchedule {
  start,   // 當前沒有讀取在途：調用方應安排一次後台讀取
  merged,  // 已有讀取在途：這次請求併入其中，讀完後自動補一次，不排隊
  ignored, // 沒有有效的倉庫身份：沒有任何安全的查詢落點
};

// 讀取結果的採納判定。
enum class ReadDisposition {
  accepted,        // 仍屬於當前倉庫身份的最新一次讀取，可以落地
  staleRepository, // 期間切換了倉庫或 Git 程序，丟棄
  notPending,      // 不是本槽正在等待的那次讀取（重複或遲到的通知）
};

// 倉庫身份：Git 程序 + 工作區根目錄，加上只在兩者真正變化時遞增的版本號。
struct RepositoryIdentity {
  unsigned long long generation = 0;
  std::wstring gitExePath;
  std::wstring workTreeRoot;

  // generation 為 0 表示尚未綁定（未識別、識別中或倉庫沒有工作區）。
  [[nodiscard]] bool Valid() const noexcept { return generation != 0 && !gitExePath.empty() && !workTreeRoot.empty(); }
};

// 一次在途讀取的憑證：序號隨請求交給工作線程、完成通知原樣帶回；
// 身份版本由協調器自己保管（完成時只需帶序號，不必把路徑再傳回來）。
struct ReadTicket {
  unsigned long long serial = 0;
  unsigned long long generation = 0;
};

// 一次外部命令窗口操作的收尾結論。
struct OperationOutcome {
  unsigned long long serial = 0;
  std::wstring displayName;
  git::CommandCompletion completion = git::CommandCompletion::launchFailed;
  long exitCode = 0;
  bool recognised = false;   // 是否就是本槽當前在途的那次操作
  bool succeeded = false;    // 只有拿到 Git 退出碼且為 0 才算成功
  bool refreshRequested = false;  // 成敗都要重讀工作區
  std::wstring note;         // 面向狀態欄的結論說明
};

class TaskCoordinator {
public:
  // ---- 倉庫身份 ----

  // 綁定身份。與當前不同時版本遞增並丟棄在途讀取（含 Git 程序換路徑）；
  // 返回 true 表示身份確實變了，調用方應清空舊列表。相同身份重複綁定不遞增，
  // 這樣「刷新」裡重新識別同一倉庫不會把自己的在途讀取判成過期。
  [[nodiscard]] bool BindRepository(std::wstring_view gitExePath, std::wstring_view workTreeRoot);

  // 解除綁定：未選擇倉庫、識別失敗或該形態沒有工作區。在途結果一律作廢。
  void UnbindRepository();

  [[nodiscard]] const RepositoryIdentity& Identity() const noexcept { return identity_; }

  // ---- 內部只讀刷新（單槽合併）----

  // 請求一次刷新：沒有讀取在途時返回 start（調用方隨後 BeginRead），
  // 有讀取在途時把這次請求併入那次讀取——合併槽只有一個布爾標記，
  // 因此無論使用者連點幾次、幾個來源同時到達，都不會形成無窮排隊。
  [[nodiscard]] RefreshSchedule RequestRefresh();
  // 只在 RequestRefresh 返回 start 後調用：佔住讀取槽並取回憑證。
  [[nodiscard]] ReadTicket BeginRead();
  // 完成通知帶回序號：只有仍屬當前身份且正是本槽等待的那次讀取才被接受。
  [[nodiscard]] ReadDisposition CompleteRead(unsigned long long serial);
  [[nodiscard]] bool ReadInFlight() const noexcept { return readInFlight_; }
  // 在途期間是否又收到過刷新請求（界面據此在讀取結束後補一次，不再排隊）。
  [[nodiscard]] bool RefreshStillQueued() const noexcept { return queuedRefresh_; }

  // ---- 外部命令窗口操作（同一時刻只允許一個）----

  // 佔用操作槽；返回 false 表示已有操作在跑，調用方不得啟動第二個。
  [[nodiscard]] bool BeginOperation(std::wstring_view displayName, unsigned long long* outSerial);
  [[nodiscard]] bool OperationInFlight() const noexcept { return operationInFlight_; }
  // 拿到執行器的最終判定後結案：釋放槽位（保留的命令窗口不會把界面鎖住），
  // 並总是安排一次刷新。
  [[nodiscard]] OperationOutcome FinishOperation(unsigned long long serial, git::CommandCompletion completion,
                                                 long exitCode, std::wstring_view failureReason);
  // 完成通知丟失（執行器已不認識這個操作 ID）：判為「結果未知」並釋放槽位，
  // 讓使用者能再次刷新看到真實狀態，而不是永遠顯示「執行中」。
  [[nodiscard]] OperationOutcome ForgetOperation(unsigned long long serial, std::wstring_view reason);

  // ---- 狀態欄文案 ----

  // 讀取開始時的說明：只讀、不彈命令窗口。
  [[nodiscard]] std::wstring ReadStartedText() const;
  // 讀取結束時的說明。若這次讀取是為某個剛結束的操作補做的，結論會連同操作結果一起給出，
  // 使用者能在同一行看到「操作怎麼樣」與「倉庫現在怎麼樣」。
  [[nodiscard]] std::wstring ReadFinishedText(std::wstring_view outcome);
  // 記住最近一次操作結論（供 ReadFinishedText 拼接；被用掉一次即清除）。
  void RememberOperationConclusion(std::wstring_view conclusion);

private:
  RepositoryIdentity identity_;
  unsigned long long readSerial_ = 0;
  unsigned long long operationSerial_ = 0;
  ReadTicket pendingRead_{};       // 本槽正在等待的那次讀取（serial 為 0 表示沒有）
  bool readInFlight_ = false;
  bool queuedRefresh_ = false;
  bool operationInFlight_ = false;
  std::wstring operationName_;
  std::wstring followUpConclusion_;  // 尚未隨刷新一起展示的操作結論
};

}  // namespace gc::app
