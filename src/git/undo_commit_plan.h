#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_plan.h"
#include "git/repository.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"

namespace gc::git {

// 「撤回最近提交」的可移植決策邏輯（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）：
//   1) 構造點擊瞬間要重新發起的幾條只讀預檢查詢的參數（HEAD／分支／父提交／遠端跟蹤引用／工作區狀態）；
//   2) 把平台層帶回的 GitQueryResult 判讀成一份乾淨的事實（UndoPreflightFacts）；
//   3) 把事實核對成「可以撤回／必須拒絕」，並湊出送進命令窗口的參數與確認文字。
//
// 撤回的語義就是普通場景下的 `git reset --soft HEAD^`：只把當前分支引用挪回第一父提交，
// 索引與工作區一個字節都不動，原提交的改動相對新 HEAD 表現為「已暫存的更改」。
// 它不是 revert（那會產生反向提交），更不是 hard reset（那會丟改動），也絕不觸碰遠端。
//
// 命令形態與各查詢的行為在本機 Git 2.55/2.56 與自建的臨時倉庫（含本地 bare 遠端）實測：
//   * `git reset --soft <完整父提交ID>`：分支挪回父提交；索引與工作區保持原樣，
//     原本已暫存的其他改動與原提交的改動一起留在索引裡——Git 不記錄「哪個字節屬於哪次提交」，
//     沒有办法把兩者分開展示，確認文字必須把這一點說清楚。
//   * 根提交沒有 HEAD^，`git reset --soft` 表達不了「回到尚無提交」：
//     改用受控路徑 `git update-ref -d HEAD <原完整ID>`——Git 只在分支引用確實還指向這個 ID
//     時才刪除它（值不符即非 0 拒絕，實測报 `cannot lock ref 'HEAD': is at X but expected Y`），
//     索引與工作區照樣一字不動，撤回後 `git status` 把原提交的全部内容顯示為已暫存的新增。
//     這種「分支消失、HEAD 懸在尚無提交的分支名上」的狀態正是 `git init` 後的形態，
//     之後再 `git reset --soft <原完整ID>` 可整條找回（實測可用，含尚無提交狀態下）。
//   * 是否「已被遠端包含」只能拿本地已有的遠端跟蹤引用（refs/remotes/*）說話：
//     `git for-each-ref --contains <id> refs/remotes` 命中即有實錘；查不到、或本地根本沒有
//     遠端跟蹤引用，都**不能**斷言「從未推送」——本步驟不聯網 fetch，引用可能早就過期。
//     因此判據是三態：已知已發布／本地信息未發現已發布／無法判斷，後兩者語氣必須誠實。
//   * 各查詢全部帶 `--no-optional-locks`，其中 `rev-list --parents -n 1 HEAD` 輸出
//     「自身ID 父1 父2…」一行（根提交只有自身 ID；合併提交從第二個 token 起是多個父）；
//     `for-each-ref --format=%(refname)` 一行一個引用名，refs/refname 不會含換行，按行取用安全。
//   * symbolic-ref／rev-parse 都帶 `--quiet`：「不在分支上」「HEAD 不可解析」是以退出碼 1 +
//     空輸出作答的正常結論，不是錯誤；其餘非 0 退出才是 Git 报了問題。

// ---- 只讀預檢的查詢參數（全部顯式 -C 綁定倉庫根，不依賴進程全局目錄） ----

[[nodiscard]] std::vector<std::wstring> BuildUndoSymbolicRefArguments(std::wstring_view repositoryDirectory);
[[nodiscard]] std::vector<std::wstring> BuildUndoHeadCommitArguments(std::wstring_view repositoryDirectory);
// 僅在 HEAD 可解析時才該執行（headCommit 已拿到完整 ID）。
[[nodiscard]] std::vector<std::wstring> BuildUndoParentsArguments(std::wstring_view repositoryDirectory);
[[nodiscard]] std::vector<std::wstring> BuildUndoHeadSummaryArguments(
    std::wstring_view repositoryDirectory);
// 全部遠端跟蹤引用：用來把「查詢成功但沒有命中」與「本地根本沒有引用可查」分開。
[[nodiscard]] std::vector<std::wstring> BuildUndoRemoteRefsArguments(std::wstring_view repositoryDirectory);
// headSha 不是合格的完整對象 ID 時返回空數組：調用方據此跳過查詢，絕不把半截 ID 送給 Git。
[[nodiscard]] std::vector<std::wstring> BuildUndoRemoteContainsArguments(std::wstring_view repositoryDirectory,
                                                                         std::wstring_view headSha);
// 工作區狀態復用 git/workspace_status 的同一套參數與解析：撤回要面對的「索引現狀」
// 與界面列表必須是同一定義，不能在這裡另發一種 status。

// ---- 查詢結果的判讀 ----

// 一次只讀預檢查詢的判讀結論。`--quiet` 系查詢的「退出碼 1 + 無輸出」是明確答案
// （不在分支上／HEAD 不可解析），單列 noResult，不混進 failed。
enum class UndoQueryOutcome {
  failed = 0,
  answered,  // 退出碼 0，輸出可按約定解析
  noResult,  // 退出碼 1 且無輸出：Git 明確回答「沒有」（僅 --quiet 系查詢）
};

struct UndoQueryRead {
  UndoQueryOutcome outcome = UndoQueryOutcome::failed;
  RepoError error = RepoError::none;  // failed 時的歸類
  std::wstring detail;                // failed 時的具體原因
  std::wstring firstLine;             // answered 時的第一行（已修剪行尾）
  std::vector<std::wstring> lines;    // answered 時的全部非空行（已修剪）
};

// 把一條 GitQueryResult 判讀成 UndoQueryRead。判定順序與 git/repository 的慣例一致：
// 先看啟動失敗/超時這類結構性事實，再看退出碼，最後才解析輸出。
[[nodiscard]] UndoQueryRead ReadUndoQuery(const GitQueryResult& result);

// HEAD／分支／父提交這一組查詢的判讀結論。
struct UndoHeadFacts {
  bool queryOk = false;      // 兩條「總是執行」的查詢都得到了明確答案
  std::wstring queryFailure; // queryOk 為 false 時面向界面的完整說明

  bool onBranch = false;     // symbolic-ref 給出了引用（含尚無提交的分支）
  std::wstring branchRef;    // symbolic-ref 原樣輸出，如 refs/heads/main
  std::wstring branchName;   // refs/heads/ 之後的部分；不是 refs/heads/ 時留空

  bool headResolved = false;
  std::wstring headObjectId;  // 已通過 LooksLikeFullObjectId 校驗的完整 ID

  bool parentsQueried = false;  // 父提交查詢執行了嗎（HEAD 不可解析時根本不發）
  bool parentsResolved = false;
  std::wstring parentsFailure;  // parentsQueried 且解析失敗時的說明
  std::vector<std::wstring> parentObjectIds;  // 每個都通過完整 ID 校驗
  bool selfMatchesHead = false;               // rev-list 的自身 ID 與 headObjectId 一致

  std::wstring headSummary;  // 僅供確認文字展示；查不到留空，不影響可撤回性判定
};

// 遠端包含證據的三態（外加「沒問過」）。含義邊界見文件頭：只有 contained 能說「已發布」，
// notFound / noRemoteRefs 都隻是「本地信息沒有」，queryFailed 連這個都說不了。
enum class UndoPublishEvidence {
  notRun = 0,
  queryFailed,    // 查詢本身失敗：無法判斷
  noRemoteRefs,   // 本地沒有任何遠端跟蹤引用：無從判斷
  notFound,       // 有引用、查詢成功，但沒有任何一個包含該提交
  contained,      // 至少一個遠端跟蹤引用包含該提交：已知已發布
};

[[nodiscard]] std::wstring_view UndoPublishEvidenceLabel(UndoPublishEvidence evidence) noexcept;

// 一次撤回預檢的全部事實（由 InterpretUndoPreflight 判讀，界面與方案層只讀它）。
struct UndoPreflightFacts {
  UndoHeadFacts head;

  bool publishQueried = false;
  UndoPublishEvidence publish = UndoPublishEvidence::notRun;
  std::vector<std::wstring> containingRemoteRefs;  // contained 時列舉（最多展示數由方案層限制）
  std::wstring publishFailureDetail;               // queryFailed 時的具體原因

  bool statusQueried = false;
  bool statusOk = false;                // status 讀成功且輸出符合 porcelain v2 約定
  RepoError statusError = RepoError::none;
  std::wstring statusDetail;
  WorkspaceModel model;  // statusOk 時有效：衝突檢查與「現有改動將一起保留」的條目數都取自它
};

// 預檢查詢的原始結果打包：平台層按順序執行只讀查詢後原樣交給判讀函數，
// 「哪些查詢發了、哪些因 HEAD 不可解析而跳過」用布爾標記如實帶入，判讀函數自己不猜。
struct UndoPreflightQueries {
  GitQueryResult symbolicRef;
  GitQueryResult headCommit;
  bool commitDependentRan = false;  // parents／summary／remoteContains 是否真的執行了
  GitQueryResult parents;
  GitQueryResult headSummary;
  GitQueryResult remoteRefs;
  GitQueryResult remoteContains;
  GitQueryResult status;  // porcelain v2 NUL 分隔輸出
};

// 把一組原始查詢判讀成事實。純函數：所有輸入都是平台層帶回的结果，可用樁輸出完整測試。
[[nodiscard]] UndoPreflightFacts InterpretUndoPreflight(const UndoPreflightQueries& queries);

// ---- 確認後的同步復核（只複查 HEAD 與分支這兩件事） ----

// 把「symbolic-ref + rev-parse」兩條查詢的結果判成 UndoHeadFacts 的簡化形態
// （不含父提交與摘要）。界面在用戶點頭之後、啟動命令窗口之前復核用：
// 兩處 branchRef 與 headObjectId 任一不同，說明點擊之後 HEAD／分支又變了，必須放棄本次執行。
[[nodiscard]] UndoHeadFacts InterpretUndoHeadSnapshot(const GitQueryResult& symbolicRef,
                                                      const GitQueryResult& headCommit);

// ---- 撤回方案 ----

struct UndoCommitPlanInput {
  UndoPreflightFacts facts;
  RepositoryWorkflowState workflow;      // Git 目錄里的流程痕跡（平台層探測，與提交同一來源）
  std::wstring repositoryRoot;           // 剛剛讀回的工作區根
  CapturedSnapshot captured;             // 點擊瞬間界面顯示的摘要（用於「現狀已變」說明）
};

struct UndoCommitPlan {
  bool blocked = false;
  std::wstring blockedReason;  // blocked 時的完整說明（界面原樣顯示）

  // 普通確認（確定撤回／取消）之外的風險確認：true 表示用戶必須明確點
  // 「強制撤回（僅本地）」才放行。這個「強制」只越過本程序的風險提示，
  // 不追加任何更激烈的命令——命令形態與普通撤回完全相同，永遠不含 --hard、不含 force push。
  bool requiresForce = false;

  std::vector<std::wstring> arguments;  // {reset, --soft, <父ID>} 或 {update-ref, -d, HEAD, <原ID>}
  std::wstring operationId;             // 執行器操作 ID（純 ASCII：undo-commit）
  std::wstring displayName;             // L"撤回最近提交"
  std::wstring commandLabel;            // 展示用命令開頭，如 L"git reset --soft"
  std::wstring previewText;             // 確認框正文
  std::wstring notice;                  // 隨操作一直顯示的範圍說明
  std::wstring restoreHint;             // 含原提交完整 ID 的恢復線索（成功後寫進結果）
  std::wstring stateChangeNote;         // 非空 → 點擊之後倉庫現狀確實變了
  std::wstring targetDisplay;           // 撤回目標的展示文本（父提交短 ID 或「尚無提交」）
};

// 主入口：核對事實並產出方案。前提不成立一律 blocked（不產生任何命令）。
// 明確拒絕、不靠「強制」繞過的條件：不在分支上（detached）、HEAD 不可解析、
// 有特殊流程進行中（merge/rebase/cherry-pick/revert/bisect）、有未解決衝突、
// 預檢查詢或工作區狀態沒讀回來。這些是 Git 自己會拒絕（或結果無法保證）的狀態，
// 「強制繼續」不等於繞過它們。
[[nodiscard]] UndoCommitPlan BuildUndoCommitPlan(const UndoCommitPlanInput& input);

}  // namespace gc::git
