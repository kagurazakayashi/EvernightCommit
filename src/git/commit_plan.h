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
//
// 確認範圍的兩道核對（本模組負責判讀，發問在 platform/windows/commit_probe）：
//   1) 彈確認框之前：一組只讀身份查詢（見下方 CommitIdentityFacts）必須與界面剛讀回的現狀
//      是同一個東西，兩邊對不上就不出確認框；
//   2) 按下「確定」之後、啟動命令之前：同一組查詢原樣重發一遍逐條比對，任何一條對不上
//      就不發命令（表單一字不動，請使用者重新確認）。
//      這道複核擋得住的是「從確認到建立行程之間」的外部改動；行程建立之後到 Git 真正讀索引
//      之間那段窗口歸 Git 自己的 index.lock 管，本程式不持有也無法持有那個鎖（持著它再啟動
//      `git commit` 只會必然失敗），因此絕不把它說成事務級的保證，界面與文件都要如實交代。

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

// ---- 一次提交的「身份」：確認框點頭時到底核對了什麼 ----
//
// 為什麼要這一層：確認框彈出來之前讀回的現狀，與 `git commit` 真正讀索引的那一刻之間，
// 隔著一次使用者思考（可能幾分鐘），期間外部終端照樣可能改了同一個倉庫。只看「已暫存的條目數」
// 與 HEAD 短 ID 擋不住這種改動——條目數不變而內容換了、換到另一條分支而 HEAD 還是同一份提交，
// 在那兩個數字上都看不出區別。因此把一次提交綁到一組可逐條複核的事實上：
//   * 工作區根 + 絕對 Git 目錄：這份方案屬於哪個倉庫的哪一塊工作樹；
//   * 完整分支引用與 HEAD 的完整物件 ID：在「哪條線」的「哪一個提交」上；
//   * 索引內容標識（`git write-tree` 算出的樹物件 ID）：暫存的到底是哪一份內容；
//   * Git 目錄里的流程痕跡：有沒有人已經開始 merge / rebase；
//   * 有效設定里的提交者身分：Git 會把這次提交記在誰名下；
//   * 提交訊息檔案與位元組數、作者身分、兩個交給 Git 的時間值：這些由本程式自己保管，
//     複核時不參與比較（外部改不了它們），但啟動命令時一律以這份為準，不再回讀界面狀態。
//
// 樹物件 ID 為什麼能當索引內容標識（本機 Git 2.56.0.windows.1 自建臨時倉庫實測）：
//   * 成功時只輸出一行完整 ID（退出碼 0），它隨索引里的路徑、模式與 blob 一起變——
//     同名同數的兩份不同內容必然是不同的樹，反過來說內容沒變就不會被誤判成「變了」；
//   * 它讀的是索引，不看工作區內容：工作區里的未暫存改動、檔案時間戳被碰過都不改變它；
//   * 索引里有未合併條目時以退出碼 128 失敗、不輸出 ID：那種索引本來就不是可提交的內容，
//     方案層在更早的衝突檢查里已拒絕，複核時拿不到 ID 同樣按作廢處理；
//   * 要如實交代的副作用：它把算出的樹物件寫進物件庫（沒有引用指向它，之後由 Git 的垃圾回收
//     清理），索引里過期的檔案狀態資訊可能被順帶刷新（條目與內容一字不動）。
//     兩者都不移動分支、不改 HEAD、不暫存新東西、不碰工作區檔案。
//   * 反面方案為什麼不用：把 `git ls-files --stage` 的原文自己雜湊一份看起來更「唯讀」，
//     但那條查詢的標準輸出受捕獲位元組上限約束，超大倉庫會讀不全，而「讀不全」只能當未知——
//     等於讓大倉庫永久無法提交；樹 ID 只有一行，沒有這個上限問題。

// 工作區根與絕對 Git 目錄：一趟查詢回兩行，行序固定為「絕對 Git 目錄、工作樹根」
// （與 git/repository 的形態路徑組同一順序；裸倉庫與 .git 內部不會輸出第二行）。
[[nodiscard]] std::vector<std::wstring> BuildCommitWorktreeArguments(std::wstring_view repositoryDirectory);
// 當前分支的完整引用（refs/heads/…）。不在分支上（游離 HEAD）時 Git 以退出碼 1、無輸出作答。
[[nodiscard]] std::vector<std::wstring> BuildCommitBranchRefArguments(std::wstring_view repositoryDirectory);
// HEAD 的完整物件 ID。尚無任何提交時同樣是退出碼 1、無輸出。
[[nodiscard]] std::vector<std::wstring> BuildCommitHeadObjectArguments(std::wstring_view repositoryDirectory);
// 索引內容標識：把當前索引算成一個樹物件並輸出它的完整 ID（副作用見上方說明）。
[[nodiscard]] std::vector<std::wstring> BuildCommitIndexTreeArguments(std::wstring_view repositoryDirectory);
// 提交者身分取自有效設定，查詢形態沿用 git/author_config（兩項各問一條，合併問在 2.56 不成立）。
inline constexpr std::wstring_view kCommitCommitterConfigKeys[] = {L"user.name", L"user.email"};

// 一組身份查詢的原始回答，由平台層執行後原樣交給判讀函數（判讀自己不發問）。
// workflow 的依據是 Git 目錄里的檔案，不是查詢：平台層用「剛問到的那個 Git 目錄」探測後帶入，
// 這樣探測的對象與這份事實屬於同一塊工作樹，不會出現拿舊目錄的痕跡配新目錄的 HEAD。
struct CommitIdentityQueries {
  GitQueryResult worktree;
  GitQueryResult branchRef;
  GitQueryResult headObject;
  GitQueryResult indexTree;
  GitQueryResult configName;
  GitQueryResult configEmail;
  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
  // 請求裡帶的工作區根（識別結果）。與 --show-toplevel 的回答核對：兩者不一致時這趟查詢
  // 讀到的不是界面選中的那塊工作區，整份事實作廢，絕不拿來給確認框或複核比對。
  std::wstring requestedRepositoryDirectory;
};

// 判讀後的身份事實。queryOk 為 false 時下面所有欄位都不可信（原因見 queryFailure）。
struct CommitIdentityFacts {
  bool queryOk = false;
  std::wstring queryFailure;

  std::wstring repositoryDirectory;  // --show-toplevel 的回答
  std::wstring absoluteGitDir;

  bool onBranch = false;
  std::wstring branchRef;  // refs/heads/main；不在分支上為空
  std::wstring branchName;

  bool headResolved = false;
  std::wstring headObjectId;  // 已通過完整物件 ID 形態校驗

  bool indexTreeResolved = false;
  std::wstring indexTreeOid;  // 同上：這一刻索引的內容標識

  bool committerConfigRead = false;  // 兩條 config 都得到明確回答（「沒設過」也算回答了）
  CommitterIdentityState committer = CommitterIdentityState::unknown;
  std::wstring committerIdentityText;

  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
};

// 把一組原始查詢判讀成事實。純函數：判定順序沿用 git/repository 的慣例（先看啟動失敗/超時/
// 輸出完整度，再看退出碼，最後才解析輸出），所有欄位都以 Git 自己的回答為準，不猜、不補。
[[nodiscard]] CommitIdentityFacts InterpretCommitIdentity(const CommitIdentityQueries& queries);

// 按下「確定」那一刻記下的那一份身份：複核比對以它為基準，啟動命令也以它為唯一取值來源。
// 除了會隨外部改動的事實，還帶著本程式自己產生的那幾樣（訊息檔案、時間值、作者身分），
// 這樣「確認框上寫的那一份」與「真正交給 Git 的那一份」是同一個東西。
struct CommitPlanIdentity {
  std::wstring gitExecutable;              // 用哪個 git.exe（不復核，但啟動時用它，不再回讀界面）
  std::wstring repositoryDirectory;        // 命令的工作目錄
  std::wstring absoluteGitDir;

  bool onBranch = false;
  std::wstring branchRef;
  std::wstring branchName;

  bool headResolved = false;
  std::wstring headObjectId;

  std::wstring indexTreeOid;

  RepositoryWorkflowState workflow;

  GitIdentity author;
  std::wstring committerIdentityText;

  std::wstring messageFilePath;
  size_t messageUtf8Bytes = 0;
  std::string authorGitDate;
  std::string committerGitDate;

  size_t stagedItems = 0;  // 展示與結論用；內容身份是 indexTreeOid，不是這個數
};

// 確認之後、執行之前的複核比對：回報「哪裡已經和確認時不一樣」的完整說明，
// 以及該怎麼辦（作廢舊方案、表單不動、重新確認）。完全一致時返回空字串——
// 只有空字串才允許把那条命令發出去。
[[nodiscard]] std::wstring DescribeCommitIdentityChange(const CommitPlanIdentity& confirmed,
                                                       const CommitIdentityFacts& current);

// 按下按鈕那一瞬間界面所顯示的倉庫摘要。方案層把它和「剛讀回的現狀」對比，
// 兩者不同時必須在確認文字裡說清楚「以下內容是剛剛重讀的」，不能假裝舊狀態還成立。
struct CapturedSnapshot {
  bool valid = false;             // false 表示沒有可比的快照（首次點擊）。
  std::wstring shortSha;          // 當時的 HEAD 短 ID；拿不到時留空（空值不參與比較，只比能不能解析）
  bool hasHead = false;           // 當時 HEAD 是否可解析
  size_t stagedItems = 0;         // 當時「已暫存的更改」的條目數
};

// 構造方案的輸入。全部是呼叫方已經取得的事實，函式本身不再讀界面狀態。
// 提交者可得性與那串身份文字不在這裡重複攜帶：它們一律取自 identity（同一趟問回的事實），
// 這樣「確認框上寫的提交者」與「複核時比對的提交者」必然來自同一次查詢。
struct CommitPlanInput {
  WorkspaceModel model;                    // 剛剛讀回的工作區快照（提交範圍取自 model.staged）
  RepoDetection detection;                 // 剛剛讀回的倉庫身份（root / absoluteGitDir / branch / shortSha）
  CommitIdentityFacts identity;            // 同一趟裡問回的身份事實（流程痕跡与提交者可得性也从它取）
  CapturedSnapshot captured;               // 點擊瞬間的界面摘要（用於「現狀是否已變」）

  std::wstring gitExecutable;              // 本程式驗證過的 git.exe（啟動時綁定用）

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
  // 這份方案綁定的身份（含訊息檔案、兩個時間與 git.exe）。界面的執行前複核拿它比對，
  // 啟動命令時的工作目錄與 Git 程序也從它取——blocked 時是空的，不該被讀取。
  CommitPlanIdentity identity;
};

// 主入口：核對事實並產出方案。任何一項前提不成立都返回 blocked（不產生任何命令），
// 原因一律寫成「為什麼不行 + 該怎麼辦」，讓使用者不用猜。
[[nodiscard]] CommitPlan BuildCommitPlan(const CommitPlanInput& input);

}  // namespace gc::git
