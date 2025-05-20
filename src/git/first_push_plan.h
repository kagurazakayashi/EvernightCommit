#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/push_plan.h"     // PushConfigListing / PushTargetCheck / 形態判定 / URL 解析
#include "git/repository.h"

namespace gc::git {

// 「首次推送：分支還沒有上游」的可移植決策邏輯（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）：
//   1) 判讀「倉庫裡有哪些遠端、各自實際會去哪裡」，擺出候選讓使用者**當場選**；
//   2) 構造並判讀選定目標之後的只讀預檢（引用名合不合格、對端那條引用在不在、停在哪）；
//   3) 把事實核對成「可以首次推送／必須拒絕」，並湊出推送那一条命令與**兩件事分別列出**的確認文字；
//   4) 點頭之後執行前復核（同一組查詢原樣重發再比對）。
//
// 為什麼要單獨有這個模組（而不是把 git/push_plan 的「無上游一律拒絕」改成放行）：
//   PushPlan 的契約是「推當前分支到它的上游」，目标由倉庫配置決定、不由界面臨場挑。
//   沒有上游時硬套那個契約，就只能猜 origin/<分支> 或替使用者寫配置——那正是 AGENTS 與
//   push_plan 文件頭明令禁止的動作。首次推送把「目標」這件事交回使用者當場選，
//   因此它是一條**独立的裁決鏈**：候选清單、目標引用名、對端現狀、要不要寫上游，
//   每一步都要有明確答案，任何一步含糊就拒絕。
//
// 「不猜」的界線（逐條與實作對應）：
//   * 候選遠端清單取自 `git config --list --null`（Git 疊好 include 與優先序之後的生效值），
//     每個候選的發布地址再逐個用 `git remote get-url --push --all <遠端>` 讓 **Git 自己**展開
//     （與普通推送同一份解析函數）；展開不成的候選如實標成「不能選」，不拿配置裡的原樣地址頂替。
//   * 目標分支名是使用者輸入的**資料**：先在輸入框用形態底線攔掉明顯不可能的寫法，
//     再由 `git check-ref-format` 這條只讀查詢給出權威裁決（實測本機 Git 2.56：合格 0、
//     不合格 1、空串 1，三種都沒有輸出；這個命令不認 `--`，帶 `-` 開頭的裸名字會以 129
//     用法錯誤死在自己的參數解析上，所以本程式一律交完整 `refs/heads/…` 形態，絕不交裸名字）。
//     Git 說不合格就是不合格：不改寫、不加前綴湊一個、不順手換成分支名。
//   * 對端那條引用「在不在」用每個發布地址各問一次只讀 `ls-remote`（與普通推送的核實同一形態）：
//     退出碼 0 而回答裡沒有那一行 = 對端目前沒有這條引用；非 0、啟動不成、超時 = **問不到**
//     （認證、權限、網路都在這一類裡）。這兩件事的措辭必須分開，絕不把「問不到」寫成「沒有」。
//   * 各發布地址答出的位置不一致時不合併成一個數字：本程式不假設多個目標是同一個倉庫。
//   * 非快進風險只在「本地問得出來」時才談：對端那個對象本地讀得到時才發
//     `rev-list --left-right --count <對端那份>...<這次要推的那份>`（與 pull 的關係查詢同一形態），
//     左邊就是「只有對端有」的提交數——大於 0 這次會被 Git 以 non-fast-forward 拒絕。
//     那個對象不在本地時這個查詢會死在 Git 自己的解析上（實測 128 + fatal），那種場合就是
//     「判斷不了」，絕不把它当成 0 也不把它当成「會被拒」。
//   * **绝不 force**：命令裡沒有 --force / --force-with-lease，首次推送更不是覆蓋對端已有分支的理由。
//     對端已有那條引用時這條推送只可能被快進接受、否則被 Git 拒絕，本程式不重試、不加碼、
//     不先替使用者抓取或整合。
//   * 推送與「設置本地上游」是**兩件分開的事、兩條各有結果的命令**：
//     - 推送沿用钉死完整提交 ID 的那份命令（git/push_plan 的 BuildPushCommandArguments），
//       範圍承諾一字不差；
//     - 上游寫入用兩條 `git config`（branch.<分支>.remote、branch.<分支>.merge），
//       各自在命令窗口裡跑、各自有退出碼，寫哪把鍵、寫什麼值、對哪個分支生效全部進確認文字，
//       配置文件的落點由 `git rev-parse --git-path config` 當場問回（實測本機 2.56：主工作區答
//       `.git/config`；即使倉庫開了 extensions.worktreeConfig，普通 `git config` 的寫入點也是它——
//       一次性自有倉庫實測核對過，所以本程式不承諾「一定寫在哪個檔案」，只展示 Git 自己的回答）。
//     為什麼不用 `git push -u`：-u 把「遠端收没收到」與「本地配置寫没寫成」揉進同一條命令，
//     那種場合配置沒寫成只会在 Git 的輸出裡留一句話，退出碼仍是推送的那份，本程式就無法
//     把兩個結果分別報告（也不允許自動重推）。更何況源側寫的是完整對象 ID 而不是分支名，
//     Git 由 refspec 反推「要給哪個分支寫上游」的那一步不是本程式敢依賴的前提。
//     因此這裡**不發 -u**，也**不為了用 -u 而把源側退回會變的分支名**。
//   * 使用者在任何一步取消：不打開命令窗口、不接觸遠端、不寫任何配置。
//
// 联网邊界（必須如實說）：選定目標之後的那次預檢裡，ls-remote **已經訪問過發布目標**
// （只讀詢問，不改動任何一邊）。這一步需要認證時由 Git 自己的方式處理，且後台查詢帶
// GIT_TERMINAL_PROMPT=0，問不到就以「問不到」收場，不會停在一個看不見的提問上。

// ---- 要不要走向導 ----

// 普通推送被「這個分支沒有上游」這一条卡住時，界面問這一句：能不能改走首次推送向導。
// 成立條件全部是事實判讀，沒有任何一項需要「猜」：在分支上且分支引用形態合格、
// HEAD 已解析出完整提交 ID、上游查詢確實發過且 Git 明確回答「沒有配置」（不是問不成）、
// 生效配置讀回來了、倉庫裡至少有一個带地址的遠端。
// 返回 false 時界面照舊展示 PushPlan 的拒絕說明，一個字都不改。
[[nodiscard]] bool CanOfferFirstPush(const PushPreflightFacts& facts);

// ---- 候選遠端（名字 + Git 自己展開的發布地址） ----

struct FirstPushRemoteCandidate {
  std::wstring name;
  std::vector<std::wstring> publishUrls;  // 未掩碼（展示前才掩碼）；展開不成時為空
  std::wstring failure;                   // 為什麼這個候選不能選（空 = 可選）

  [[nodiscard]] bool selectable() const { return failure.empty() && !publishUrls.empty(); }
  // 列表第二栏的展示文字：逐條編號並做憑據掩碼；問不到時原樣是那句「為什麼問不到」。
  [[nodiscard]] std::wstring urlsDisplay() const;
};

// 判讀候選清單：遠端名取自 `config --list --null`（按首次出現順序去重），
// remoteUrlQueries 必須是按同一順序逐個遠端發的 `get-url --push --all` 回答。
// 條數對不上時，多出來的名字一律記「這條查詢沒發出去」——絕不拿前一個遠端的地址充數。
// 生效配置讀不回來時 candidates 為空、configOk 為 false：界面據此拒絕，不擺一個空列表。
struct FirstPushCandidateFacts {
  bool configOk = false;
  std::wstring configFailure;
  std::vector<FirstPushRemoteCandidate> candidates;
};

[[nodiscard]] FirstPushCandidateFacts InterpretFirstPushCandidates(
    const GitQueryResult& configListing, const std::vector<GitQueryResult>& remoteUrlQueries);

// 一個候選都擺不出來（或全都問不出發布地址）時要向使用者說明的話。
// 界面在兩種場合都用它：候選為空、以及清單不空但每一個都標了「不能選」。
[[nodiscard]] std::wstring DescribeFirstPushCandidateRefusal(const FirstPushCandidateFacts& candidates);


// ---- 目標分支名的形態底線（只用於輸入框即時攔截） ----

// 返回空串表示這個寫法看起來可以試（權威裁決仍由 git check-ref-format 在後台問回來：
// 對話框裡不能同步等子進程，所以這裡只做純形態判讀，不宣稱「Git 一定接受」）。
[[nodiscard]] std::wstring DescribeFirstPushBranchNameProblem(std::wstring_view branchName);

// refs/heads/<branchName>；branchName 不合格（空、含 `:`、引號、控制字符、以 `/` 收尾等）時返回空串。
[[nodiscard]] std::wstring BuildFirstPushTargetBranchRef(std::wstring_view branchName);

// ---- 只讀查詢的參數（全部顯式 -C 綁定倉庫根） ----

// `git -C <倉庫> --no-optional-locks check-ref-format <完整引用>`。
// targetBranchRef 必須是 refs/ 開頭且形態合格（IsUsableRemoteRef），否則返回空數組：
// 裸名字、以 `-` 開頭的寫法會被這個命令當成選項，那種場合它連問題都沒聽見就用法錯誤。
[[nodiscard]] std::vector<std::wstring> BuildFirstPushRefFormatArguments(
    std::wstring_view repositoryDirectory, std::wstring_view targetBranchRef);
// `git -C <倉庫> --no-optional-locks rev-parse --git-path config`：本地配置文件的落點（相對路徑），
// 只用於確認文字裡那句「會寫到哪」。問不到不構成拒絕理由，如實留白即可。
[[nodiscard]] std::vector<std::wstring> BuildFirstPushConfigPathArguments(
    std::wstring_view repositoryDirectory);
// `git -C <倉庫> --no-optional-locks rev-list --left-right --count <對端那份>...<這次要推的那份>`
// （形態與 pull 的關係查詢同一份實現）。兩個 ID 必須都是完整對象 ID，否則返回空數組。
[[nodiscard]] std::vector<std::wstring> BuildFirstPushRelationshipArguments(
    std::wstring_view repositoryDirectory, std::wstring_view remoteObjectId,
    std::wstring_view localObjectId);

// ---- 上游寫入的兩條命令 ----

// 一步寫入：一把配置鍵 + 它的值，獨立成一條在命令窗口裡跑的 `git config`。
// 字段全部是給使用者看與給執行器用的同一份依據，界面不得自己再拼一遍。
struct UpstreamWriteStep {
  std::wstring key;          // branch.<分支名>.remote（原樣展示用的鍵名）
  std::wstring value;        // 要寫進去的那個值
  std::wstring purpose;      // 這一步在幹什麼（展示用一句話）
  std::vector<std::wstring> arguments;  // {config, <鍵>, <值>}：工作目錄已由命令窗口綁定到倉庫根
  std::wstring operationId;  // 執行器操作 ID（純 ASCII）
  std::wstring displayName;  // L"設置上游（…）"
  std::wstring commandLabel;  // 展示用命令
  std::wstring conclusion;   // 這一步成功之後要補的那句結果說明
};

// 兩步寫入（順序就是 Git 自己設上游時寫的那兩把鍵）：
//   1) branch.<本地分支名>.remote = <目標遠端名>
//   2) branch.<本地分支名>.merge  = <目標遠端引用，例如 refs/heads/foo>
// 任一形态不合格（分支名/遠端名/引用名里有命令窗口無法安全表達的寫法）時返回空數組：
// 調用方據此拒絕寫上游——本函數絕不「修一下湊一對鍵值出來」。
[[nodiscard]] std::vector<UpstreamWriteStep> BuildUpstreamWriteSteps(std::wstring_view branchName,
                                                                    std::wstring_view remoteName,
                                                                    std::wstring_view targetBranchRef);

// 寫上游之前對現場的一次核實（推送與核實都在命令窗口與網路那里耗時間，其間倉庫可能被別人動過）。
// 依據是與預檢同族的只讀事實（PushPreflightFacts）：当初「這條分支還沒有上游」這個前提要是
// 不再成立，那份方案就已經不屬於此刻的倉庫了——`git config` 是覆蓋式的寫法，把過時的值蓋上去
// 會抹掉別人剛設好的上游，所以這裡只判定「還能不能寫」，絕不「改成別的值再寫」。
// 回空字串 = 前提仍然成立（可以照原方案寫）；否則回「為什麼不能寫」的完整說明。
// 分档：
//   * 查詢本身沒跑成 / 問不回來：不能確定，就不寫（「問不到」永遠不是「沒有」）。
//   * 當前分支已不是当初那條（切走了或已被刪）：按分支名落的鍵就不再是確認過的那件事。
//   * 上游已經被設好：與原方案逐值相同時无事可做（回「已是這個值」，一條命令都不發）；
//     值不同時拒絕覆蓋，並把現在是哪一對擺出來。
[[nodiscard]] std::wstring DescribeUpstreamWriteStaleness(const PushPreflightFacts& latest,
                                                          const std::vector<UpstreamWriteStep>& steps,
                                                          std::wstring_view branchRef);

// ---- 選定目標之後的預檢判讀 ----

// 逐發布目標的「在／不在／問不到」合成出的結論。平台層在預檢中途也用它決定
// 「還值不值得再問一次領先落後」，判讀函數與平台層因此共用同一份聚合規則：
//   * 有一個目標問不成 → known=false（連「對端有沒有」都還说不清）；
//   * 問成的目標裡沒有一行是那個引用 → exists=false，也就是「對端現在沒有這條分支」；
//   * 有引用但各目標答出的位置不一樣 → disagrees=true，不合併成一個位置（多個發布地址
//     本來就可能不是同一個倉庫）。
struct FirstPushPresenceSummary {
  bool known = false;
  bool exists = false;
  bool disagrees = false;
  std::wstring remoteObjectId;  // exists 且各目標一致時有效
  std::wstring detail;          // 問不成／位置不一致時的具體說明（已做憑據掩碼）
};

[[nodiscard]] FirstPushPresenceSummary SummarizeFirstPushPresence(
    const std::vector<PushTargetCheck>& checks);

// 一組原始查詢結果。哪幾條發了、哪幾條因為前提不成立而根本沒發，用 *Ran / 條數如實帶入，
// 判讀函數自己不猜（與 push 的 PushPreflightQueries 同一套規矩）。
struct FirstPushProbeQueries {
  GitQueryResult symbolicRef;
  GitQueryResult headObject;
  GitQueryResult upstream;
  bool upstreamRan = false;
  GitQueryResult configListing;
  std::wstring chosenRemoteName;    // 使用者當場選的那個遠端（判讀據此核對，不重新猜）
  GitQueryResult chosenRemoteUrl;
  bool chosenRemoteUrlRan = false;
  std::wstring targetBranchRef;     // 發 check-ref-format 時問的那個引用
  GitQueryResult refFormat;
  bool refFormatRan = false;
  std::vector<GitQueryResult> presenceQueries;  // 與展開後的每條發布 URL 一一對應
  GitQueryResult relationship;
  bool relationshipRan = false;
  GitQueryResult configPath;
  bool configPathRan = false;
};

struct FirstPushFacts {
  bool queryOk = false;
  std::wstring queryFailure;  // queryOk 為 false 時面向界面的完整說明

  bool onBranch = false;
  std::wstring branchRef;
  std::wstring branchName;

  bool headResolved = false;
  std::wstring headObjectId;

  bool upstreamRan = false;
  bool upstreamConfigured = false;  // true 表示「上游已經被設好了」：向導的前提沒了
  std::wstring upstreamRemote;
  std::wstring upstreamTrackingRef;

  bool configOk = false;
  PushConfigListing config;
  std::wstring chosenRemoteName;
  bool chosenRemoteNameUsable = false;
  bool chosenRemoteExists = false;
  std::vector<std::wstring> publishUrls;
  std::vector<std::wstring> rawPublishUrls;
  bool publishUrlRewritten = false;
  std::wstring publishUrlFailure;
  std::wstring publishUrlNote;
  PushScopeConfigEffects scopeEffects;

  std::wstring targetBranchRef;
  bool refFormatRan = false;
  bool refFormatAccepted = false;
  std::wstring refFormatDetail;  // 「Git 說這個引用名不能用」或「這條查詢沒問成」

  // 逐發布目標的在/不在/問不到（複用推送核實那一份結構，語義完全相同）。
  std::vector<PushTargetCheck> presence;
  bool presenceKnown = false;      // 每個發布目標都問成了（才能談「對端有沒有」）
  bool remoteRefExists = false;    // 至少一個目標上那條引用已存在
  bool presenceDisagrees = false;  // 各目標答出的位置不一樣（多發布地址時的誠實結論）
  std::wstring remoteObjectId;     // 各目標一致時：對端那條引用現在停在哪（完整 ID）

  bool relationshipKnown = false;
  long long remoteOnly = 0;  // 只有對端有的提交數：大於 0 這次就不是快進，Git 會拒絕
  long long localOnly = 0;   // 只有本地有的提交數：這次要送出去的量
  std::wstring relationshipDetail;

  std::wstring configFilePath;       // `rev-parse --git-path config` 的回答（僅展示）
  std::wstring configFilePathFailure;
};

[[nodiscard]] FirstPushFacts InterpretFirstPushProbe(const FirstPushProbeQueries& queries);

// ---- 方案 ----

enum class FirstPushPlanState {
  blocked = 0,  // 前提不成立：不產生命令、不寫配置
  ready,        // 目標與範圍已定：給確認框（有風險時要求明確點頭）
};

struct FirstPushPlan {
  FirstPushPlanState state = FirstPushPlanState::blocked;
  std::wstring explanation;  // 兩種狀態都要有把話說完的文案（界面原樣展示）

  // 推送那一步：與普通推送同一份命令構造、同一批範圍承諾。
  std::wstring branchName;
  std::wstring localBranchRef;
  std::wstring pushedObjectId;
  std::wstring remoteName;
  std::wstring targetBranchRef;
  std::vector<std::wstring> pushUrls;
  std::wstring remoteUrlDisplay;
  std::vector<std::wstring> arguments;
  std::wstring operationId;  // L"push"（與普通推送同一個執行器身份，不另造一套）
  std::wstring displayName;  // L"推送"
  std::wstring commandLabel;
  std::wstring confirmationText;
  std::wstring notice;
  bool requiresForce = false;
  std::vector<std::wstring> risks;
  std::vector<std::wstring> notes;

  // 上游那一步：使用者沒點頭要寫時整個向導只發推送。
  bool setUpstreamRequested = false;
  std::vector<UpstreamWriteStep> upstreamSteps;
  std::wstring upstreamTargetFile;  // Git 答出的配置文件落點；問不到時這裡是那句「問不到」
};

struct FirstPushPlanInput {
  FirstPushFacts facts;
  std::wstring repositoryDirectory;
  bool setUpstreamRequested = false;
};

// 主入口：核對事實並產出方案。前提不成立一律 blocked（不產生任何命令、不寫任何配置）。
[[nodiscard]] FirstPushPlan BuildFirstPushPlan(const FirstPushPlanInput& input);

// 點頭之後、啟動命令之前的執行前復核：比對兩份首次推送事實。
// 分支、要推的那一份提交、「上游還是沒有」、選定遠端在不在清單裡、展開後的發布 URL、
// 目標引用名的 Git 裁決、對端那條引用的位置、會被中和的設定的有無——任何一處對不上，
// 預檢得出的那份方案就不屬於此刻的倉庫。返回空字串表示一致。
// 特別的一條：上游在這期間被設好了（外部或使用者自己跑了一句），那份方案作廢，
// 並請使用者改用普通推送——這時目標已經由倉庫配置說清楚了。
[[nodiscard]] std::wstring DescribeFirstPushChange(const FirstPushFacts& preflight,
                                                   const FirstPushFacts& latest);

}  // namespace gc::git
