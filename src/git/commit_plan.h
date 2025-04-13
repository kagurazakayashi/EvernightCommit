#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "git/author_config.h"
#include "git/command_window.h"
#include "git/commit_date.h"
#include "git/commit_identity.h"
#include "git/repository.h"
#include "git/workspace_model.h"

namespace gc::git {

// 「创建提交」的可移植決策邏輯（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）：
//   1) 把「剛剛重新讀回的倉庫現狀」核對成可以提交／必須拒絕兩種結論，拒絕時給出可操作的原因；
//   2) 湊出送進命令窗口的 Git 參數與只覆蓋本次子進程的環境變數；
//   3) 把使用者點頭前要看到的東西（提交範圍、訊息、身份、兩個時間與偏移）排成一段確認文字。
//
// 命令形态在本機 Git 2.53 與自建臨時倉庫實測：
//   * `git commit --cleanup=verbatim -F <UTF-8 檔案>`：訊息一字不改地進物件。
//     反過來說，`--cleanup=whitespace`（Git 對 -F 的預設）會收攏連續空行、`default`/`strip`
//     還會去掉註釋行 —— 那等於程式偷偷收拾了使用者的文字。這裡把形態寫死在命令裡，
//     使用者的 commit.cleanup 設定不會改變提交訊息的內容。
//   * 身份與時間走環境變數（GIT_AUTHOR_NAME / GIT_AUTHOR_EMAIL / GIT_AUTHOR_DATE /
//     GIT_COMMITTER_DATE），不走 `--author=`：後者對壞形態是「靜默拆錯」的（見 git/commit_identity），
//     而環境值不經過 cmd 的引號與百分號展開，含 `"` `&` `%` 的姓名也能原樣送達。
//   * 只覆蓋「作者身份 + 兩個時間」。提交者身份仍由這個倉庫的有效 Git 設定決定，
//     程式既不寫設定檔，也不改使用者自己的環境。
//   * 不帶 `-a`、不帶 `--pathspec-from-file`、不帶任何路徑：提交範圍就是索引裡那一份，
//     未暫存的改動一律留在工作區。也絕不加 `--no-verify`，hooks 與簽名設定照常生效。
//   * `GIT_AUTHOR_DATE` 的取值形态見 git/commit_date（實測：不加 `@` 前綴時，
//     不足 9 位數的秒數會被 Git 判為「invalid date format」並讓整條命令以 128 失敗）。

// 倉庫裡「有特殊流程正在進行」的痕跡。由平台層按 Git 目錄裡的檔案是否存在探測後帶入
// （本模組不碰檔案系統），界面據此決定要不要拒絕提交。
struct RepositoryWorkflowState {
  bool mergeInProgress = false;      // MERGE_HEAD：合并尚未完成
  bool revertInProgress = false;     // REVERT_HEAD
  bool cherryPickInProgress = false; // CHERRY_PICK_HEAD
  bool bisectInProgress = false;     // BISECT_LOG
  bool rebaseInProgress = false;     // rebase-merge/ 或 rebase-apply/
  bool indexLocked = false;          // index.lock：另一個 Git 進程正在寫索引

  [[nodiscard]] bool HasSpecialFlowInProgress() const noexcept {
    return mergeInProgress || revertInProgress || cherryPickInProgress || bisectInProgress ||
           rebaseInProgress;
  }
  // 正在進行哪一種流程的說明（併發多種時全部列出）。空字串表示沒有。
  [[nodiscard]] std::wstring SpecialFlowText() const;
};

// 一個時間的兩種形态：交給 Git 的值 + 給人看的說明。
// gitDate 為空表示平台層沒能算出這一刻（换算失敗），方案層會因此拒絕。
struct CommitTimeChoice {
  std::string gitDate;
  std::wstring displayText;
  CivilTime wall{};
  int offsetMinutes = 0;
};

// 按下按鈕那一瞬間界面所顯示的倉庫摘要。方案層把它和「剛讀回的現狀」對比，
// 兩者不同時必須在確認文字裡說清楚「以下內容是剛剛重讀的」，不能假裝舊狀態還成立。
struct CapturedSnapshot {
  bool valid = false;             // false 表示沒有可比的快照（首次點擊）。
  std::wstring shortSha;          // 當時的 HEAD 短 ID；拿不到時留空（空值不參與比較，只比能不能解析）
  bool hasHead = false;           // 當時 HEAD 是否可解析
  size_t stagedItems = 0;         // 當時「已暫存的更改」的條目數
};

// 構造方案的輸入。全部是呼叫方已經取得的事實，函式本身不再讀界面狀態。
struct CommitPlanInput {
  WorkspaceModel model;                    // 剛剛讀回的工作區快照（提交範圍取自 model.staged）
  RepoDetection detection;                 // 剛剛讀回的倉庫身份（root / absoluteGitDir / branch / shortSha）
  RepositoryWorkflowState workflow;        // Git 目錄里的流程痕跡（平台層探測）
  CapturedSnapshot captured;               // 點擊瞬間的界面摘要（用於「現狀是否已變」）

  CommitterIdentityState committer = CommitterIdentityState::unknown;
  std::wstring committerIdentityText;      // 有效設定裡的提交者身份（供確認文字複述）
  GitIdentity author;                      // 表單作者（已由 git/commit_identity 解析）

  std::wstring message;                    // 合成後的完整提交訊息（寬字元，僅供確認文字）
  size_t messageUtf8Bytes = 0;             // 寫進檔案的位元組數
  std::wstring messageFilePath;            // 已寫好的 UTF-8 訊息檔案（絕對路徑）

  CommitTimeChoice authorTime;
  CommitTimeChoice committerTime;
  bool timesSynced = false;                // 「時間同步修改」是否勾選（只影響確認文字的說法）
};

struct CommitPlan {
  bool blocked = false;
  std::wstring blockedReason;              // blocked 時的完整說明（界面原樣顯示）

  std::vector<std::wstring> arguments;     // {L"commit", L"--cleanup=verbatim", L"-F", <檔案>}
  std::vector<EnvironmentOverride> environmentOverrides;  // 作者身份與兩個時間
  std::wstring operationId;                // 執行器操作 ID（純 ASCII：L"commit"）
  std::wstring displayName;                // L"创建提交"
  std::wstring commandLabel;               // L"git commit -F"
  std::wstring previewText;                // 確認框正文（提交範圍 + 訊息 + 身份 + 時間）
  std::wstring stateChangeNote;            // 非空 → 點擊之後倉庫現狀確實變了
  std::wstring notice;                     // 隨操作一直顯示的範圍說明
  size_t stagedItems = 0;                  // 本次提交的條目數
};

// 主入口：核對事實並產出方案。任何一項前提不成立都返回 blocked（不產生任何命令），
// 原因一律寫成「為什麼不行 + 該怎麼辦」，讓使用者不用猜。
[[nodiscard]] CommitPlan BuildCommitPlan(const CommitPlanInput& input);

}  // namespace gc::git
