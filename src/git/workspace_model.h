#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// Git 工作區變化的類別。由 porcelain v2 的狀態字元直接判定，
// 界面據此選取簡短狀態文案，後續步驟據此決定該條目該用哪條 Git 命令處理。
enum class ChangeKind {
  unknown,
  modified,
  added,
  deleted,
  renamed,
  copied,
  untracked,
  conflicted,
  typeChange,
  submodule,
};

// 子模組條目的內部狀態，對應 porcelain v2 的 <sub> 欄位「S<c><m><u>」。
//
// 父倉庫對子模組只有一條 gitlink 記錄：在父倉庫「加入暫存區」只會把子模組的提交指標
// 寫進父倉庫索引，子模組內部已跟蹤檔案的改動與未跟蹤檔案都歸屬那個子模組自己的倉庫，
// 必須進入子模組工作區另行暫存與提交。父倉庫的列表因此不會把子模組內部的檔案拆成
// 可暫存條目，也不會替使用者遞迴提交。
struct SubmoduleState {
  bool commitChanged = false;     // <c>：子模組 HEAD 與父倉庫記錄的提交指標不同
  bool trackedChanges = false;    // <m>：子模組內已跟蹤檔案有改動
  bool untrackedChanges = false;  // <u>：子模組內有未跟蹤檔案
};

struct ChangeItem {
  ChangeKind kind = ChangeKind::unknown;
  std::wstring statusCode;  // Git 原始 XY 兩字元（如 "RM"、"UU"），保留原文供後續命令構造使用
  std::wstring path;        // 倉庫相對路径，按 Git 原樣保留（正斜杠分隔，不依本地代碼頁轉寫）
  std::wstring oldPath;     // 重命名/複製來源路径，普通條目為空
  std::wstring similarity;  // 重命名/複製相似度欄位原文（如 "R100"），普通條目為空
  SubmoduleState submodule;  // kind 為 submodule 時有效

  // 狀態列文字：只用於顯示，任何操作參數都從上面的原始欄位取得，不由顯示文字反解。
  [[nodiscard]] std::wstring StatusLabel() const;
  // 路徑列文字：重命名/複製顯示「舊路径 -> 新路径」，其餘顯示自身路径。
  [[nodiscard]] std::wstring PathLabel() const;
};

struct CommitItem {
  std::wstring objectId;  // 完整對象 ID（40 或 64 個十六進制字符，按 %H 原樣保存）
  std::wstring summary;
  std::wstring author;
  std::wstring authoredAt;         // 作者時間的本機時區展示文本（由平台層的時間格式化回調填入）
  long long authorEpochSeconds = 0;  // 作者時間的 Unix 秒：展示文本由它換算，命令一律用完整 ID
  bool isMergeCommit = false;      // 父提交兩個以上：查看詳情時要說明組合差異可能為空
};

// 工作區變化模型。索引狀態與工作區狀態分開建模：同一個路徑可以同時出現在
// staged 與 unstaged（例如暫存一次修改後又繼續編輯），兩側各保留自己的狀態字元。
struct WorkspaceModel {
  // 工作區相對索引的變化：未暫存的修改/刪除/類型變化，加上未跟蹤檔案，
  // 以及未合併（衝突）條目。衝突條目放在這一側，因為它需要在工作區裡解決，
  // 解決方式正是「加入暫存區」（git add）；其 kind 為 conflicted，不會被當成普通刪除。
  std::vector<ChangeItem> unstaged;
  // 索引相對 HEAD 的變化：已暫存、等待創建提交的條目。
  std::vector<ChangeItem> staged;
  std::vector<CommitItem> recentCommits;  // 從當前 HEAD 可達的最近提交（有上限，見 git/commit_history）

  [[nodiscard]] bool HasAnyItems() const noexcept {
    return !unstaged.empty() || !staged.empty() || !recentCommits.empty();
  }
  [[nodiscard]] bool HasConflicts() const noexcept;
  // 左側列表中屬於子模組的條目（含衝突側以外的全部子模組記錄）。
  [[nodiscard]] std::vector<ChangeItem> SubmoduleChanges() const;
};

// 工作區讀取（git status）的生命週期。界面必須能區分「讀到了、確實沒有變化」
// 與「還沒讀/正在讀/讀取失敗」，後幾種情況不能顯示成空倉庫。
enum class WorkspaceLoadStatus {
  unloaded,  // 尚無可用倉庫，未發起讀取
  loading,   // 後台只讀查詢進行中
  loaded,    // 讀取成功，模型即當前快照
  failed,    // 讀取失敗，failureLabel 為已歸類的簡短原因
};

struct EmptyStateTexts {
  std::wstring unstaged;
  std::wstring staged;
  std::wstring history;
};

// 三塊列表在「該列沒有條目」時要顯示的說明，按讀取週期與倉庫形態選取。
// failureLabel 只在 status 為 failed 時使用（取 RepoErrorLabel 的文案即可）。
// historyFailureLabel：工作區讀取成功、但 git log 這一項失敗時的簡短原因（空表示沒有失敗）。
// 提交歷史與工作區共用同一次讀取，「歷史列為空」可能是倉庫還沒有提交、這一段讀取失敗、
// 或讀取根本沒跑——幾種說法必須互相區分，都不能顯示成一份理應如此的空列表。
EmptyStateTexts WorkspaceEmptyTexts(WorkspaceLoadStatus status, std::wstring_view failureLabel,
                                    bool repositoryHasCommits, std::wstring_view historyFailureLabel);

// 子模組變化的集中說明文字：解釋父倉庫暫存只會記錄提交指標。沒有子模組變化時返回空字串。
[[nodiscard]] std::wstring SubmoduleExplanationText(const WorkspaceModel& model);

}  // namespace gc::git
