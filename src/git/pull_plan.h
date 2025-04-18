#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_plan.h"       // RepositoryWorkflowState（Git 目录里的流程痕迹）
#include "git/repository.h"
#include "git/undo_commit_plan.h"  // 复用 UndoQueryRead / ReadUndoQuery 这一套判读
#include "git/workspace_model.h"

namespace gc::git {

// 「pull」的可移植決策邏輯（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）：
//   1) 構造兩個階段要發的只讀查詢參數；
//   2) 把平台層帶回的 GitQueryResult 判讀成乾淨的事實（PullTargetFacts / PullRelationshipFacts）；
//   3) 把事實核對成「可以抓取／可以整合／必須拒絕／要用户選策略」，並湊出命令與確認文字。
//
// pull 分兩個階段，邊界寫死在這裡：
//   * 階段一「抓取」：先把本地分支與它對應的遠端分支擺出來，經用户點頭後在**命令窗口**裡
//     fetch（網絡操作必須看得見，絕不藏在界面的「刷新」裡）；
//   * 階段二「整合」：fetch 成功後重新讀回事實，判出本地與遠端的關係與可預見的風險，
//     再經用户點頭執行原生的整合命令。
// 整合用的是 fetch 下來的那一份具體提交（完整對象 ID），不是 `git pull`：後者會再抓取一次，
// 那樣「預檢看的」與「實際合的」就可能不是同一份東西。獲取與整合分開，才有得核對。
//
// 前提與「不猜」的界線：
//   * 必須在分支上、分支有明確上游（branch.<分支名>.remote + branch.<分支名>.merge，
//     由 for-each-ref 的 %(upstream…) 如實回答）；無上游／游離 HEAD／尚無提交／有 merge/rebase
//     等流程在走／還有未解決衝突——一律給出具體原因並拒絕，不代設 upstream、不猜 origin；
//   * 整合策略尊重既有配置（branch.<分支名>.rebase > pull.rebase、pull.ff > merge.ff），
//     並在確認文字裡說清是哪條配置定的；分叉而配置未明確時讓用户當場選（默認偏向合併），
//     本程序絕不寫任何配置檔；
//   * 內容衝突預演用 `git merge-tree --write-tree`：不碰工作區與索引（只在對象庫裡留一個
//     不可達的 tree 物件，gc 自行回收），也絕不在用户工作區先真合一次再 reset 回去。
//     版本不支持（退出碼不是 0/1）時降級為「無法預判」的保守提示，不假稱無衝突；
//   * 變基策略下不做合併式預演：重放是逐提交的，衝突點可能不同，用合併預檢宣稱「變基無衝突」
//     是假話——那種場合只給範圍與提示。
//
// 各查詢全部帶 `--no-optional-locks`；symbolic-ref／rev-parse --verify 帶 `--quiet`
// （「不在分支上」「引用不可解析」以退出碼 1 + 空輸出作答，是明確答案而不是錯誤）；
// `git config --get` 未設置時同樣退出碼 1 + 空輸出。`--name-only -z` 的清單以 NUL 分隔，
// 路徑原樣取用（含空格、中文、`&`、`%` 不會被引號規則改写）。

// ---- 本地與遠端的關係（以 fetch 之後的遠端跟蹤引用為準） ----
enum class PullRelationship {
  unknown = 0,
  upToDate,    // 不 ahead 不 behind：同一個提交
  fastForward, // 本地是遠端的祖先：只要往前挪引用
  aheadOnly,   // 遠端沒有新東西，倒是本地領先：整合方向是反的（那屬於 push）
  diverged,    // 兩邊各有各的提交：需要合併或變基
};

[[nodiscard]] std::wstring_view PullRelationshipLabel(PullRelationship relationship) noexcept;

// 實際採用的整合形態。
enum class PullIntegrateStrategy {
  fastForward,  // git merge --ff-only <id>
  merge,        // git merge --no-edit <id>
  rebase,       // git rebase <id>
};

[[nodiscard]] std::wstring_view PullIntegrateStrategyLabel(PullIntegrateStrategy strategy) noexcept;

// 合併內容衝突的無損預演結果。
enum class PullMergeDryRun {
  notRun = 0,
  supported_clean,   // 退出碼 0：預演沒有衝突
  supported_conflict,  // 退出码 1：預演报出衝突文件
  unsupported,       // Git 不認這個子命令/選項：無法預演，只能保守提示
  failed,            // 其他失敗：同樣無法預演
};

[[nodiscard]] std::wstring_view PullMergeDryRunLabel(PullMergeDryRun dryRun) noexcept;

// 用户在「分叉且配置沒定策略」時當場選的那一句。
enum class PullStrategyChoice {
  none = 0,     // 沒選（配置說了話就用配置的）
  chooseMerge,
  chooseRebase,
};

// ---- 只讀查詢的參數（全部顯式 -C 綁定倉庫根） ----

// symbolic-ref 的那一行是不是 refs/heads/〈分支名〉；是則取出分支名。
// 平台層編排「要不要發後續查詢」時也問這一句，判據因此只有一份實現在這裡。
[[nodiscard]] bool PullBranchNameFromRef(std::wstring_view symbolicRefOutput, std::wstring* branchName);

// 上游那一行（for-each-ref 的三欄）的拆解結果。
struct PullUpstreamInfo {
  bool configured = false;  // 至少問到了本地遠端跟蹤引用的全名
  std::wstring remote;       // origin
  std::wstring trackingRef;  // refs/remotes/origin/main
  std::wstring remoteBranch; // refs/heads/main（遠端那邊的分支，僅供展示）
};

[[nodiscard]] PullUpstreamInfo ParsePullUpstreamLine(std::wstring_view line);


[[nodiscard]] std::vector<std::wstring> BuildPullSymbolicRefArguments(std::wstring_view repositoryDirectory);
[[nodiscard]] std::vector<std::wstring> BuildPullHeadObjectArguments(std::wstring_view repositoryDirectory);
// branchName 為空時返回空數組：不在分支上就沒有分支記錄可問上游。
// 輸出三欄：%(upstream:remotename) TAB %(upstream) TAB %(upstream:remoteref)，
// 分別是「遠端名」「本地遠端跟蹤引用的全名」「遠端那邊的分支引用」——三者都由 Git 按配置回答。
[[nodiscard]] std::vector<std::wstring> BuildPullUpstreamArguments(std::wstring_view repositoryDirectory,
                                                                  std::wstring_view branchName);
// trackingRef 為空時返回空數組。問的是本地那個遠端跟蹤引用目前停在哪個提交。
[[nodiscard]] std::vector<std::wstring> BuildPullTrackingObjectArguments(
    std::wstring_view repositoryDirectory, std::wstring_view trackingRef);
[[nodiscard]] std::vector<std::wstring> BuildPullConfigArguments(std::wstring_view repositoryDirectory,
                                                                std::wstring_view key);
// 點擊_pull_ 之後在命令窗口裡發出的那條抓取：與界面 fetch 按鈕同一個範圍口徑。
[[nodiscard]] std::vector<std::wstring> BuildPullFetchArguments(std::wstring_view repositoryDirectory,
                                                               std::wstring_view remoteName);
// ---- 階段二（fetch 之後）的關係查詢 ----
[[nodiscard]] std::vector<std::wstring> BuildPullAheadBehindArguments(
    std::wstring_view repositoryDirectory, std::wstring_view headObjectId,
    std::wstring_view trackingObjectId);
[[nodiscard]] std::vector<std::wstring> BuildPullMergeBaseArguments(
    std::wstring_view repositoryDirectory, std::wstring_view headObjectId,
    std::wstring_view trackingObjectId);
// base 與 target 必須都是完整對象 ID，否則返回空數組（調用方據此跳過）。
[[nodiscard]] std::vector<std::wstring> BuildPullIncomingArguments(
    std::wstring_view repositoryDirectory, std::wstring_view baseObjectId,
    std::wstring_view targetObjectId);
[[nodiscard]] std::vector<std::wstring> BuildPullMergeTreeArguments(
    std::wstring_view repositoryDirectory, std::wstring_view headObjectId,
    std::wstring_view trackingObjectId);

// 整合留下冲突后问那一句「索引里现在有哪些未合并条目」。只读，不带 --no-index 之类的形态，
// 也不做任何解决冲突的动作——它只是把 Git 已经记下来的现场读回来给界面看。
[[nodiscard]] std::vector<std::wstring> BuildPullConflictListingArguments(
    std::wstring_view repositoryDirectory);

// ---- 階段一：本地分支／上游配置／現狀 ----

struct PullTargetQueries {
  GitQueryResult symbolicRef;
  GitQueryResult headObject;
  GitQueryResult upstream;          // 不在分支上時根本不發
  bool upstreamRan = false;
  GitQueryResult trackingObject;    // 沒有上游引用時不發
  bool trackingObjectRan = false;
  GitQueryResult configPullRebase;
  GitQueryResult configBranchRebase;
  bool configBranchRebaseRan = false;  // 没有分支名时根本不问（键名拼不出来）
  GitQueryResult configPullFf;
  GitQueryResult configMergeFf;
  GitQueryResult status;
  bool statusRan = false;
  // Git 目录里的流程痕迹：由平台层按档案存在性探得后随查询一起交进来（本模組不碰檔案系統），
  // 判读函数只负责把它如实搬到事实里。probed 为 false 表示这次没探（按「没有痕迹」处理）。
  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
};

struct PullTargetFacts {
  bool queryOk = false;
  std::wstring queryFailure;  // queryOk 為 false 時面向界面的完整說明

  bool onBranch = false;
  std::wstring branchRef;    // refs/heads/main
  std::wstring branchName;   // main

  bool headQueried = false;
  bool headResolved = false;
  std::wstring headObjectId;

  bool upstreamRan = false;
  bool upstreamConfigured = false;
  std::wstring upstreamRemote;        // origin
  std::wstring upstreamTrackingRef;   // refs/remotes/origin/main（本地那個跟蹤引用）
  std::wstring upstreamRemoteBranch;  // refs/heads/main（遠端那邊的分支，僅供展示）

  bool trackingObjectRan = false;
  bool trackingResolved = false;
  std::wstring trackingObjectId;
  std::wstring trackingDetail;  // 「跟蹤引用還不存在」等可展示的具體說明

  // 四條配置的原始值；空字串表示「未設置」（Git 以退出碼 1 明確回答沒有）。
  std::wstring configPullRebase;
  std::wstring configBranchRebase;
  std::wstring configPullFf;
  std::wstring configMergeFf;

  bool statusRan = false;
  bool statusOk = false;
  std::wstring statusDetail;
  WorkspaceModel model;

  // Git 目录里的流程痕迹（merge/rebase/cherry-pick…）与「索引是不是正被别人写着」。
  // 由平台层按档案存在性探测带进（与创建提交/撤回同一來源），本模組不碰檔案系統。
  // workflowProbed 為 false 時一律按「沒有痕跡」處理：本程序不因為「看不見」而拒絕一次合法操作，
  // 真能不能整合由 Git 自己說——它那種場合本來就先不接受。
  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
};

// 把一組原始查詢判讀成事實。純函數：所有輸入都是平台層帶回的结果，可用樁輸出完整測試。
[[nodiscard]] PullTargetFacts InterpretPullTarget(const PullTargetQueries& queries);

// ---- 階段二：本地與遠端的關係 ----

struct PullRelationshipQueries {
  GitQueryResult aheadBehind;
  GitQueryResult mergeBase;
  bool mergeBaseRan = false;
  GitQueryResult incoming;
  bool incomingRan = false;
  GitQueryResult mergeTree;
  bool mergeTreeRan = false;
};

struct PullRelationshipFacts {
  bool queryOk = false;
  std::wstring queryFailure;

  PullRelationship relationship = PullRelationship::unknown;
  long long ahead = 0;   // 只有本地有的提交數
  long long behind = 0;  // 只有遠端有的提交數（這次要整合進來的量）

  bool mergeBaseRan = false;
  bool mergeBaseResolved = false;
  std::wstring mergeBaseObjectId;

  bool incomingRan = false;
  bool incomingOk = false;
  std::vector<std::wstring> incomingPaths;  // 相對於基準點「遠端改動了哪些路徑」
  std::wstring incomingDetail;

  bool dryRunRan = false;
  PullMergeDryRun dryRun = PullMergeDryRun::notRun;
  std::vector<std::wstring> dryRunConflicts;
  std::wstring dryRunDetail;
};

[[nodiscard]] PullRelationshipFacts InterpretPullRelationship(const PullRelationshipQueries& queries);

// ---- 方案：階段一（抓取） ----

enum class PullFetchPlanState {
  blocked = 0,  // 前提不成立：不產生命令
  ready,        // 本地分支與遠端分支都問清了：給確認框
};

struct PullFetchPlan {
  PullFetchPlanState state = PullFetchPlanState::blocked;
  std::wstring explanation;  // 兩種狀態都要有把話說完的文案

  std::wstring remoteName;
  std::wstring trackingRef;
  std::wstring remoteBranchRef;
  std::wstring localBranchRef;

  std::vector<std::wstring> arguments;  // {fetch, --recurse-submodules=no, <remote>}
  std::wstring operationId;             // 執行器操作 ID（純 ASCII：pull-fetch）
  std::wstring displayName;             // L"pull 抓取"
  std::wstring commandLabel;
  std::wstring confirmationText;
  std::wstring notice;
};

// 主入口：核對階段一的事實並產出抓取方案。流程痕跡就在 facts.workflow 里（平台层填）。
[[nodiscard]] PullFetchPlan BuildPullFetchPlan(const PullTargetFacts& facts,
                                               std::wstring_view repositoryDirectory = {});

// ---- 方案：階段二（整合） ----

struct PullIntegratePlanInput {
  PullTargetFacts target;                              // 抓取之後重讀的那一份（含流程痕迹）
  PullRelationshipFacts relationship;
  std::wstring repositoryRoot;
  PullStrategyChoice choice = PullStrategyChoice::none;  // 用户在當場選的那一句
};

enum class PullPlanState {
  blocked = 0,       // 前提不成立：不產生命令
  nothingToIntegrate,  // 遠端沒有新東西：本來就無事可做
  chooseStrategy,    // 分叉而配置未明確：讓用户選合併還是變基
  ready,             // 策略與目標已定：給確認框
};

struct PullIntegratePlan {
  PullPlanState state = PullPlanState::blocked;
  std::wstring explanation;  // blocked / nothingToIntegrate / chooseStrategy 時界面原樣展示

  // chooseStrategy 時的兩個候選（合併排第一，默認偏向它）；ready 時空。
  std::vector<std::wstring> strategyCandidates;

  PullIntegrateStrategy strategy = PullIntegrateStrategy::merge;
  bool requiresForce = false;            // 有風險條目：必须點「仍要繼續」
  std::vector<std::wstring> risks;       // 逐條風險（含受影響文件名）
  std::wstring strategySource;           // 這個策略是誰定的（哪條配置 / 用户剛選 / 默認）

  std::vector<std::wstring> arguments;   // {-c, submodule.recurse=false, merge|rebase, …, <完整 ID>}
  std::wstring operationId;              // 執行器操作 ID（純 ASCII：pull-integrate）
  std::wstring displayName;              // L"pull 整合"
  std::wstring commandLabel;
  std::wstring confirmationText;         // ready 時確認框正文
  std::wstring notice;                   // 隨操作一直顯示的範圍說明
  std::wstring targetObjectId;           // 本次整合進來的具體提交（完整 ID）
};

// 主入口：核對兩個階段的事實並產出整合方案。前提不成立一律 blocked（不產生任何命令）。
[[nodiscard]] PullIntegratePlan BuildPullIntegratePlan(const PullIntegratePlanInput& input);

// ---- 整合結束若留下冲突：把现场读回来给界面看 ----

// 流程痕跡本身由平台層按 Git 目錄里的檔案判斷（RepositoryWorkflowState，與提交/撤回同一來源）；
// 這裡只問「索引里的未合併條目」與「分支/HEAD 現在在哪」——界面向用户交代现场时才有实据，
// 而不是只说一句「整合没有完成」。
struct PullConflictState {
  bool readOk = false;
  std::wstring readFailure;
  std::vector<std::wstring> conflictPaths;  // 去重後的未合并路徑（Git 給的相對路徑原樣保留）
  std::wstring branchRef;                   // 可能為空：不在分支上
  std::wstring headObjectId;                // 可能為空：HEAD 不可解析
};

[[nodiscard]] PullConflictState InterpretPullConflictState(const GitQueryResult& conflictListing,
                                                           const GitQueryResult& symbolicRef,
                                                           const GitQueryResult& headObject);

// ---- 點頭之後、啟動整合命令之前的執行前復核 ----

// 用户点头与实际执行之间，外部终端完全可能又动了同一个仓库（切分支、提交、甚至另一次 fetch）。
// 界面因此把预检那套只读查询原样再发一遍，然後用本函数比对两份事实：
// 分支、HEAD、远端跟踪引用的位置、工作区/索引的逐条现状——任何一处对不上，
// 预检得出的那份方案就不再属于此刻的这个仓库，必须放弃执行。
// 返回空串表示两回一致，可以照原方案执行；非空就是给界面原样展示的原因。
[[nodiscard]] std::wstring DescribePullChange(const PullTargetFacts& preflight,
                                              const PullTargetFacts& latest);

}  // namespace gc::git
