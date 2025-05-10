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
//   2) 把平臺層帶回的 GitQueryResult 判讀成一份乾淨的事實（UndoPreflightFacts）；
//   3) 把事實核對成「可以撤回／必須拒絕」，並湊出送進命令窗口的參數與確認文字。
//
// 撤回的語義就是「把當前分支引用挪回它最新一次提交的第一父提交」：只移動那一個引用，
// 索引與工作區一個字節都不動，原提交的改動相對新 HEAD 表現為「已暫存的更改」。
// 它不是 revert（那會產生反向提交），更不是 hard reset（那會丟改動），也絕不觸碰遠端。
//
// 執行形態一律是帶預期舊值的原子引用更新（本任務的關鍵修復）：
//   * 普通提交：`git update-ref --create-reflog -m <reason> <完整分支引用> <父完整ID> <原完整ID>`
//     —— Git 文檔寫明三參數形態「after verifying that the current value of the <ref> matches
//     <old-oid>」，值對不上即以非 0 拒絕、引用原樣不動。
//   * 真正根提交：`git update-ref -d -m <reason> <完整分支引用> <原完整ID>`
//     —— 文檔寫明 `-d` 形態同樣「deletes the named <ref> after verifying that it still
//     contains <old-oid>」；刪除的是那個分支引用本身，索引與工作區照舊一字不動。
//   兩條路徑都不再使用 `git reset --soft`（它沒有「預期舊值」這個前提：確認到執行之間分支被
//   別的流程推進的話，軟撤回會從那個新位置再退一步，撤掉根本不屬於用戶確認的那條提交），
//   也不再拿可變的 `HEAD` 當作目標名稱——兩個分支可以指向同一個提交，`HEAD` 換指另一個分支後，
//   「分支引用名 + 完整舊值」纔是唯一可靠的綁定對象。
//   符號引用（HEAD 究竟還指著哪個分支）無法由單條 update-ref 一起核對：本程序用「確認後、
//   啟動命令窗口前的同步復核」加上引用名綁定來處理，並在確認文字裡如實說明這個殘餘窗口，
//   不自稱鎖住了任意外部寫入者（完整事務需要 git update-ref --stdin，命令窗口沒有 stdin 通道）。
//
// 目標提交（父提交）必須由兩份互不相通的證據共同確認，任何不一致都明確拒絕（見 UndoTargetKind）：
//   * `rev-list --parents -n 1 <原完整ID>`：Git 的**歷史視圖**。淺倉庫的歷史邊界會讓它把有父的
//     提交報成沒有父（父對象沒被抓下來，關係被切斷），所以它單獨不可信。
//   * `cat-file commit <原完整ID>`：提交**對象自己**記錄的 parent 行。淺克隆仍保留完整對象，
//     因此這裡能看到被歷史視圖隱藏的父；兩者不一致就說明父關係不可信。
//   * `rev-parse --is-shallow-repository`：倉庫是不是淺倉庫。真正根提交只有在「兩處都說沒有父」
//     **且**倉庫不是淺倉庫時才成立，才會進入刪除引用的路徑。
//   * `rev-parse --verify --quiet <第一父完整ID>^{commit}`：要挪去的那一個提交對象在本地讀不讀得到。
//     讀不到（或存在卻剝不出一個提交）就拒絕（需要用戶自己 `git fetch` / `--unshallow` 補全歷史），
//     絕不自動聯網、不自動 unshallow。
//     實測更正（本機 Git 2.53.0.windows.3）：原本形態 `cat-file -t --quiet <ID>` 不可用——
//     `git cat-file` 根本不認 `--quiet`，帶著它一律退出碼 129 用法錯誤，預檢一問就報錯。
//     改用同族 `--quiet` 契約的剝皮問法後實測核對：缺失對象與 blob 對象都以退出碼 1 + 空輸出
//     作答（明確「沒有」），合格提交以退出碼 0 回答那個 ID 自己。
//   所有對象查詢都帶 `--no-replace-objects`：`git replace` 造出的替換對象不會改變判定結果，
//   被判讀採信的始終是倉庫裡那個真實對象。
//
// 命令形態與各項查詢的行為在本機 Git 2.56 與自建的臨時倉庫（含本地 bare 遠端）實測：
//   * 撤回只移動分支引用：索引與工作區保持原樣，原本已暫存的其他改動與原提交的改動一起留在
//     索引裡——Git 不記錄「哪個字節屬於哪次提交」，沒有辦法把兩者分開展示，確認文字必須說清。
//   * 根提交撤回後是「分支引用被刪除、HEAD 懸在一個還沒有提交的分支名上」，正是 `git init`
//     之後的形態；找回要靠**原提交完整 ID**（對象仍在對象庫裡）。分支自己的 reflog 是否隨引用
//     刪除而保留由 Git 決定，本程序不把它當作找回依據，也不承諾一條可能已經消失的記錄。
//   * 是否「已被遠端包含」只能拿本地已有的遠端跟蹤引用（refs/remotes/*）說話：
//     `git for-each-ref --contains <id> refs/remotes` 命中即有實錘；查不到、或本地根本沒有
//     遠端跟蹤引用，都**不能**斷言「從未推送」——本步驟不聯網 fetch，引用可能早就過期。
//     因此判據是三態：已知已發布／本地信息未發現已發布／無法判斷，後兩者語氣必須誠實。
//   * 各查詢全部帶 `--no-optional-locks`；`for-each-ref --format=%(refname)` 一行一個引用名，
//     refs/refname 不會含換行，按行取用安全。
//   * symbolic-ref／rev-parse --verify（含父對象的 `<ID>^{commit}` 剝皮問法）都帶 `--quiet`：
//     「不在分支上」「HEAD 不可解析」「對象不存在或剝不出提交」是以退出碼 1 + 空輸出作答的正常結論，
//     不是錯誤；其餘非 0 退出纔是 Git 報了問題。

// ---- 只讀預檢的查詢參數（全部顯式 -C 綁定倉庫根，不依賴進程全局目錄） ----

[[nodiscard]] std::vector<std::wstring> BuildUndoSymbolicRefArguments(std::wstring_view repositoryDirectory);
[[nodiscard]] std::vector<std::wstring> BuildUndoHeadCommitArguments(std::wstring_view repositoryDirectory);
// 僅在 HEAD 可解析時才該執行（headCommit 已拿到完整 ID）。
// 父提交與後續三條對象查詢都綁定那個**完整對象 ID**，不再寫 HEAD：HEAD 是可變的，
// 「rev-parse 拿到的那個提交」與「rev-list 回答的那個提交」必須是同一個對象，否則判定無意義。
// headSha 不是合格的完整對象 ID 時返回空數組：調用方據此跳過查詢，絕不把半截 ID 送給 Git。
[[nodiscard]] std::vector<std::wstring> BuildUndoParentsArguments(std::wstring_view repositoryDirectory,
                                                                  std::wstring_view headSha);
[[nodiscard]] std::vector<std::wstring> BuildUndoHeadSummaryArguments(
    std::wstring_view repositoryDirectory, std::wstring_view headSha);
// 讀提交對象自己的 parent 行（與 rev-list 的歷史視圖互相核對）。
[[nodiscard]] std::vector<std::wstring> BuildUndoCommitObjectArguments(
    std::wstring_view repositoryDirectory, std::wstring_view headSha);
// 問倉庫是不是淺倉庫（真正根提交的判據之一）。
[[nodiscard]] std::vector<std::wstring> BuildUndoShallowStateArguments(
    std::wstring_view repositoryDirectory);
// 問「要挪去的那個父提交對象」在本地是否存在、是不是提交對象。
[[nodiscard]] std::vector<std::wstring> BuildUndoParentObjectArguments(
    std::wstring_view repositoryDirectory, std::wstring_view parentSha);
// 全部遠端跟蹤引用：用來把「查詢成功但沒有命中」與「本地根本沒有引用可查」分開。
[[nodiscard]] std::vector<std::wstring> BuildUndoRemoteRefsArguments(std::wstring_view repositoryDirectory);
// headSha 不是合格的完整對象 ID 時返回空數組：調用方據此跳過查詢，絕不把半截 ID 送進 Git。
[[nodiscard]] std::vector<std::wstring> BuildUndoRemoteContainsArguments(std::wstring_view repositoryDirectory,
                                                                         std::wstring_view headSha);
// 工作區狀態復用 git/workspace_status 的同一套參數與解析：撤回要面對的「索引現狀」
// 與界面列表必須是同一定義，不能在這裡另發一種 status。

// ---- 查詢結果的判讀 ----

// 一次只讀預檢查詢的判讀結論。`--quiet` 系查詢的「退出碼 1 + 無輸出」是明確答案
// （不在分支上／HEAD 不可解析／對象不存在），單列 noResult，不混進 failed。
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

// 從 rev-list --parents 的回答裡取出「歷史視圖給出的第一個父提交 ID」。
// 平臺層用它決定要不要追問那個父對象讀不讀得到；解析只此一份，判讀函數不另猜一輪。
// 沒有父、自身 ID 不合格或查詢沒答上來時返回空串（調用方據此跳過那條查詢）。
[[nodiscard]] std::wstring UndoFirstReportedParent(const GitQueryResult& parentsQuery);

// ---- 撤回目標（父關係）的分類 ----

// 一份「rev-list 的歷史視圖 + 提交對象自己記錄的父 + 淺倉庫狀態 + 目標父對象可讀性」合成出來的
// 分類。只有 verifiedRoot／singleParent／mergeParents 三種是可撤回的，其餘一律明確拒絕。
enum class UndoTargetKind {
  undetermined = 0,    // 查詢失敗、輸出不合約定：沒問出可採信的答案
  verifiedRoot,        // 真正根提交：兩處都記錄「沒有父」，且倉庫明確不是淺倉庫
  singleParent,        // 普通提交：有且僅有一個父，且那個父對象本地可讀
  mergeParents,        // 合併提交：多個父且第一父對象本地可讀（目標取第一父）
  hiddenByShallow,     // 淺邊界：對象記錄了父，歷史視圖卻不給（或淺倉庫裡查不到父關係）
  unreadableParent,    // 父對象在本地讀不到：歷史不完整，需要用戶自己補全
  inconsistentParents, // 兩處父關係對不上、或目標根本不是提交對象
};

[[nodiscard]] std::wstring_view UndoTargetKindLabel(UndoTargetKind kind) noexcept;

struct UndoTargetEvidence {
  bool queried = false;  // 三條對象/淺倉庫查詢有沒有發過（HEAD 不可解析時根本不發）
  UndoTargetKind kind = UndoTargetKind::undetermined;
  std::wstring failure;  // 非可撤回分類時面向界面的完整說明

  std::vector<std::wstring> recordedParentIds;  // 提交對象頭部裡的 parent 行（按出現順序）
  std::wstring unreadableParentId;              // unreadableParent 時：讀不到的那個父
  std::wstring mismatchedParentId;              // inconsistentParents 時：不是提交對象的那個

  bool shallowQueried = false;      // --is-shallow-repository 得到明確答案
  bool repositoryIsShallow = false;  // shallowQueried 時有效
  std::wstring shallowDetail;        // 查詢失敗時的具體原因
};

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
  std::vector<std::wstring> parentObjectIds;  // rev-list 的歷史視圖，每個都通過完整 ID 校驗
  bool selfMatchesHead = false;               // rev-list 的自身 ID 與 headObjectId 一致

  UndoTargetEvidence target;  // 與 parentObjectIds 一起讀：分類決定能不能撤回、撤回去哪兒

  std::wstring headSummary;  // 僅供確認文字展示；查不到留空，不影響可撤回性判定
};

// 遠端包含證據的三態（外加「沒問過」）。含義邊界見文件頭：只有 contained 能說「已發布」，
// notFound / noRemoteRefs 都只是「本地信息沒有」，queryFailed 連這個都說不了。
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

// 預檢查詢的原始結果打包：平臺層按順序執行只讀查詢後原樣交給判讀函數，
// 「哪些查詢發了、哪些因 HEAD 不可解析／沒有合格父 ID 而跳過」用布爾標記如實帶入，判讀函數自己不猜。
struct UndoPreflightQueries {
  GitQueryResult symbolicRef;
  GitQueryResult headCommit;
  bool commitDependentRan = false;  // parents／commitObject／shallow／summary／remoteContains 是否真的執行了
  GitQueryResult parents;
  GitQueryResult commitObject;
  GitQueryResult shallowState;
  bool parentObjectRan = false;    // 目標父對象那條查詢是否發了
  std::wstring parentObjectQueryOid;  // 發出去時問的是哪個 ID（判讀據此核對，不重新猜）
  GitQueryResult parentObject;
  GitQueryResult headSummary;
  GitQueryResult remoteRefs;
  GitQueryResult remoteContains;
  GitQueryResult status;  // porcelain v2 NUL 分隔輸出
};

// 把一組原始查詢判讀成事實。純函數：所有輸入都是平臺層帶回的結果，可用樁輸出完整測試。
[[nodiscard]] UndoPreflightFacts InterpretUndoPreflight(const UndoPreflightQueries& queries);

// ---- 確認後的同步復核（只複查 HEAD 與分支這兩件事） ----

// 把「symbolic-ref + rev-parse」兩條查詢的結果判成 UndoHeadFacts 的簡化形態
// （不含父提交與摘要）。界面在用戶點頭之後、啟動命令窗口之前復核用：
// 兩處 branchRef 與 headObjectId 任一不同，說明點擊之後 HEAD／分支又變了，必須放棄本次執行。
// 注意：父關係綁定在不可變的對象 ID 上，不會隨外部進程改變，所以復核不必重問父提交；
// 而「分支引用還得確實指著那個舊值」由執行命令裡的預期舊值由 Git 原子核對。
[[nodiscard]] UndoHeadFacts InterpretUndoHeadSnapshot(const GitQueryResult& symbolicRef,
                                                      const GitQueryResult& headCommit);

// ---- 撤回方案 ----

struct UndoCommitPlanInput {
  UndoPreflightFacts facts;
  RepositoryWorkflowState workflow;      // Git 目錄裡的流程痕跡（平臺層探測，與提交同一來源）
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

  // 實際要執行的命令：update-ref 帶預期舊值（普通路徑）或 update-ref -d 帶預期舊值（根提交路徑）。
  std::vector<std::wstring> arguments;
  std::wstring operationId;             // 執行器操作 ID（純 ASCII：undo-commit）
  std::wstring displayName;             // L"撤回最近提交"
  std::wstring commandLabel;            // 展示用命令開頭，如 L"git update-ref"
  std::wstring previewText;             // 確認框正文
  std::wstring notice;                  // 隨操作一直顯示的範圍說明
  std::wstring restoreHint;             // 含原提交完整 ID 的恢復線索（成功後寫進結果）
  std::wstring stateChangeNote;         // 非空 → 點擊之後倉庫現狀確實變了
  std::wstring targetDisplay;           // 撤回目標的展示文本（父提交短 ID 或「尚無提交」）

  // 方案綁定的三件事，測試與界面診斷都直接讀它們，不從 arguments 裡反推：
  std::wstring targetRef;               // 完整分支引用（refs/heads/…），絕不是 HEAD
  std::wstring expectedOldObjectId;     // 用戶確認時那個分支引用的完整舊值
  std::wstring newObjectId;             // 撤回後分支應指向的完整 ID；根提交路徑為空（刪除引用）
  UndoTargetKind targetKind = UndoTargetKind::undetermined;
};

// 撤回目標的分支引用名校驗：必須是 refs/ 開頭的完整引用名，且不含會破壞命令行/說明書形態的字符。
// Git 自己還會再按 refname 規則核對一次；這裡只是不讓一個來歷不明的名字進入命令。
[[nodiscard]] bool IsSafeUndoTargetRef(std::wstring_view branchRef);

// 主入口：核對事實並產出方案。前提不成立一律 blocked（不產生任何命令）。
// 明確拒絕、不靠「強制」繞過的條件：不在分支上（detached）、HEAD 不可解析、
// 分支引用名不合格、父關係兩處證據對不上（淺邊界）、目標父對象讀不到、
// 有特殊流程進行中（merge/rebase/cherry-pick/revert/bisect）、有未解決衝突、
// 預檢查詢或工作區狀態沒讀回來。這些是 Git 自己會拒絕（或結果無法保證）的狀態，
// 「強制繼續」不等於繞過它們。
[[nodiscard]] UndoCommitPlan BuildUndoCommitPlan(const UndoCommitPlanInput& input);

// 「确认后、执行前的复核」裁决：把后台读回的 HEAD/分支与方案绑定的那份预检事实逐条比对。
// 返回空串 = 一致，可以发出那条 update-ref；否则返回要原样写进「任务状态」的完整说明
// （「复核没能完成」与「HEAD/分支又变了」各有各的措辞，绝不合并成一句“请重试”）。
// 调用方拿到非空答案就不得发命令：这一轮放弃的是「这一份现状」，不是用户的意图——
// 表单与仓库都不动，重读回来后由用户决定是否再来一次。
[[nodiscard]] std::wstring DescribeUndoRecheckMismatch(const UndoHeadFacts& recheck,
                                                       const UndoPreflightFacts& preflight);

}  // namespace gc::git
