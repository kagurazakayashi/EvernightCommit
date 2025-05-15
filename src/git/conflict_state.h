#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_history.h"   // LooksLikeFullObjectId / ShortObjectId
#include "git/repository.h"       // GitQueryResult / NulRecordsAreComplete
#include "git/undo_commit_plan.h" // 復用 UndoQueryRead / ReadUndoQuery 這一套三態判讀

namespace gc::git {

// 「倉庫已經停在某個 Git 流程裡」的可移植決策邏輯
// （本模組不碰任何 Win32 API、不起子進程、不讀檔案系統：痕跡的存在性與檔案內容一律由平台層帶回）：
//   1) 把平台層帶回的痕跡與三條只讀查詢判讀成「現在停著的是哪一種流程、要繼續還缺什麼」；
//   2) 湊出「查看」「繼續」「中止」三個入口各自要給界面看的話，以及真正發給命令窗口的參數；
//   3) 把確認框前後兩份狀態核對成一句「還是不是同一件事」。
//
// 這一模組的邊界（本任務的硬性要求）：
//   * 只做「看清現場 + 把那一条 Git 命令交出去」。絕不替你選任何一邊的內容：
//     沒有 -X ours/-X theirs、沒有 checkout --ours/--theirs、沒有 merge-file、沒有 rerere 操作；
//   * 沒有流程痕跡時不生成任何 --abort；未合併條目還在時不生成任何 --continue——
//     「可以繼續」这四个字只從 Git 自己讀回來的實據裡得出，讀不回來就說「沒問出來」；
//   * 未知或不一致的痕跡（多種流程並存、認不出的 rebase-apply 形態、只有 SQUASH_MSG、
//     二分定位、檔案讀不回來）一律明確拒絕並點名看到了什麼，不猜一種「大概能恢復」的方式；
//   * --continue 會由 Git 建立提交（可能不止一次），因此鉤子、簽名、編輯器都是它自己的事：
//     本模組刻意不傳 --no-verify、--no-edit、-m，確認文字必須把這件事說在前面；
//   * --abort 會改動工作區與索引（按 Git 文檔各自的口徑），風險必須具體寫在確認框裡，
//     絕不出現「已經幫你備份／可以安心按」這類話，也絕不後台自動執行任何恢復動作。
//
// 痕跡的判讀依據只有 Git 自己留在「絕對 Git 目錄」裡的檔案（連結工作樹下那層是
// <主倉>\.git\worktrees\<名>，MERGE_HEAD 等就落在這一層）：
//   MERGE_HEAD / REVERT_HEAD / CHERRY_PICK_HEAD / BISECT_LOG / SQUASH_MSG / MERGE_MODE /
//   MERGE_AUTOSTASH / REBASE_AUTOSTASH / index.lock / rebase-merge\ / rebase-apply\ / sequencer\。
// 變基進行中時 Git 本來就會同時留 CHERRY_PICK_HEAD（序列化器用它告訴 git commit 這是祇選提交），
// --rebase-merges 的合併衝突場合也可能留 MERGE_HEAD：那種「同伴痕跡」不是兩種流程並存在跑，
// 因此 rebase-*\ 目錄一律優先，否則會把一次正常變基說成「狀態不一致」而拒絕到底。

// 停著的是哪一種流程（或者為什麼無法判定）。
enum class ConflictFlowKind {
  unreadable = 0,    // 連 Git 目錄都沒探成，或探到的檔案讀不回來：未知，不據此做任何決定
  none,              // 沒有流程停在进行中
  merge,             // MERGE_HEAD：合併停在衝突/--no-commit 那一步
  rebaseMergeBackend, // rebase-merge\：變基（合併後端，預設；含 --rebase-merges 與交互式）
  rebaseApplyBackend, // 只有 rebase-apply\ 且 head-name/onto 讀得回：變基（apply 後端）
  cherryPick,        // CHERRY_PICK_HEAD（沒有變基目錄）
  revert,            // REVERT_HEAD（沒有變基目錄）
  bisect,            // BISECT_LOG：認得，但本程式不接手（--continue/--abort 不是它的恢復方式）
  squashOnly,        // 只有 SQUASH_MSG：--squash 那種「改動已進暫存區、沒有 MERGE_HEAD」的停法
  mixed,             // 多種互不相容的痕跡並存：不一致，只報告看到了什麼
  ambiguousRebase,   // 只有 rebase-apply\ 且形態讀不回：無法區分變基(apply 後端)與 git am
};

[[nodiscard]] std::wstring ConflictFlowKindLabel(ConflictFlowKind kind);

// 一次「衝突與暫停流程」查看裡，界面最多逐條列出幾個未合併檔案（其餘只報個數）。
inline constexpr size_t kConflictListedPathCap = 8;

// 平台層只讀帶回的原始痕跡。存在性由檔案/目錄判斷，內容由限長讀取帶回：
// 讀不回來（不存在以外的任何理由：打不開、超上限、不是合法 UTF-8）時對應欄位留空，
// 並將檔案名記進 contentFailures——「讀不回來」永遠不等於「沒有」。
struct ConflictMarkerFacts {
  bool probed = false;          // false：這次根本沒探（Git 目錄不可用）→ 一切按「問不到」收場
  std::wstring probeFailure;    // probed 為 false 時面向界面的說明

  bool mergeHead = false;
  bool revertHead = false;
  bool cherryPickHead = false;
  bool bisectLog = false;
  bool rebaseMergeDir = false;
  bool rebaseApplyDir = false;
  bool sequencerDir = false;  // 序列待辦所在目錄（多步祇選/撤銷；連結工作樹下它可能在公共層）
  bool squashMsg = false;
  bool mergeMode = false;
  bool mergeAutostash = false;
  bool rebaseAutostash = false;
  bool indexLock = false;

  // 變基後端線索：rebase-merge\interactive 或 rebase-apply\interactive 存在與否。
  bool rebaseInteractiveMark = false;

  // 限長讀回的檔案內容（已由平台層嚴格解碼成 UTF-16 並去掉行尾；不合格式時留空）。
  std::wstring mergeHeadOid;   // MERGE_HEAD
  std::wstring pickHeadOid;    // CHERRY_PICK_HEAD 或 REVERT_HEAD（兩者由存在性欄位區分）
  std::wstring rebaseHeadName; // head-name：這條變基完成後要落回去的那條分支
  std::wstring rebaseOnto;     // onto：变基的落點提交
  std::wstring rebaseOrigHead; // orig-head：變基開始時 HEAD 所在
  std::wstring rebaseMsgnum;   // 現在停在第幾步（讀不到留空）
  std::wstring rebaseEnd;      // 一共幾步（讀不到留空）

  // 哪些檔案存在卻沒能原樣讀回（只列檔案名，不含內容）：展示時如實交代「這部分問不到」。
  std::vector<std::wstring> contentFailures;
};

// 判讀後的現場事實：狀態說明、繼續/中止兩個方案與執行前複核都只看這一份。
struct ConflictStateFacts {
  bool probed = false;
  std::wstring probeFailure;
  ConflictFlowKind kind = ConflictFlowKind::unreadable;

  // 看到了哪些痕跡（人類可讀的名字，並存時全部列出；不參與判讀，只參與展示）。
  std::vector<std::wstring> markersSeen;

  // 当前分支與 HEAD（三條只讀查詢讀回來的部分；問不到就留空，絕不当成「沒有分支」）。
  bool branchQueried = false;
  bool onBranch = false;
  std::wstring branchRef;
  bool headQueried = false;
  std::wstring headObjectId;

  // 索引裡還有哪些未合併條目（與 pull 現場讀取同一條查詢、同一套完整性門檻）。
  bool unmergedReadOk = false;
  std::wstring unmergedReadFailure;
  std::vector<std::wstring> unmergedPaths;

  // 流程自己的目標（各 kind 各自那份；讀不回來時對應說明會寫「問不到」）。
  std::wstring flowTarget;
  std::wstring rebaseHeadName;
  std::wstring rebaseOntoShort;
  std::wstring rebaseProgress;
  bool rebaseInteractive = false;

  bool sequencerPending = false;
  bool autostashEntry = false;
  bool indexLock = false;
  std::vector<std::wstring> unreadableContents;  // contentFailures 的展示形態（檔案名）

  // 兩個入口能不能發出去（判據全在本模組，界面只讀結論）。
  bool continueAvailable = false;
  bool abortAvailable = false;
  std::wstring continueBlockedReason;
  std::wstring abortBlockedReason;

  [[nodiscard]] bool HasUnmergedEntries() const noexcept { return !unmergedPaths.empty(); }
};

// 把痕跡與三條查詢判讀成現場事實。純函數：所有輸入都是平台層帶回的结果，可用樁輸出完整測試。
// unmergedListing 是 `git diff --name-only -z --diff-filter=U`（與 pull 現場讀取同源），
// symbolicRef / headObject 是「當前分支」「HEAD 现在在哪」那兩條（與 pull 預檢同源）。
[[nodiscard]] ConflictStateFacts InterpretConflictState(const ConflictMarkerFacts& markers,
                                                        const GitQueryResult& unmergedListing,
                                                        const GitQueryResult& symbolicRef,
                                                        const GitQueryResult& headObject);

// 「查看」入口給界面原樣展示的那段話：流程類型、當前分支/目標、未合併檔案、繼續前提、
// 兩個入口現在的可用性與原因，以及「差異在哪裡看」的既有路徑。
[[nodiscard]] std::wstring DescribeConflictState(const ConflictStateFacts& facts);

// 一條要交進命令窗口的流程命令（繼續或中止）。blocked 時 arguments 一律為空：
// 「沒有流程」與「還有未合併檔案」這類場合不生成任何命令，更不發出去。
struct ConflictOperationPlan {
  bool blocked = true;
  std::wstring blockedReason;  // blocked 時界面原樣展示的那段話

  std::vector<std::wstring> arguments;  // {-c, submodule.recurse=false, <子命令>, --continue|--abort}
  std::wstring operationId;             // 執行器操作 ID（純 ASCII：conflict-continue/conflict-abort）
  std::wstring displayName;             // L"冲突流程 继续"
  std::wstring commandLabel;            // L"merge --continue"
  std::wstring confirmationText;        // 確認框／風險確認框正文
  std::wstring notice;                  // 隨操作一直顯示的範圍說明
};

[[nodiscard]] ConflictOperationPlan BuildConflictContinuePlan(const ConflictStateFacts& facts);
[[nodiscard]] ConflictOperationPlan BuildConflictAbortPlan(const ConflictStateFacts& facts);

// 點頭之後、發命令之前的複核：把預檢那組查詢原樣重發一遍，兩份事實用本函數比對。
// 返回空串表示「確認框上寫的那件事此刻還成立」；非空就是界面原樣展示的原因。
// 特別地：流程痕跡在這裡已經消失時（外部終端把那個流程走完或中止了），
// 必須說成「那個流程已经不在这个仓库里了」，不能說成「沒讀回來」，也不能照原方案發命令。
[[nodiscard]] std::wstring DescribeConflictStateChange(const ConflictStateFacts& confirmed,
                                                       const ConflictStateFacts& latest);

}  // namespace gc::git
