#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/commit_plan.h"       // RepositoryWorkflowState（Git 目录里的流程痕迹）
#include "git/pull_plan.h"         // 复用上游行解析、HEAD/分支查询与 ahead/behind 判读
#include "git/repository.h"
#include "git/undo_commit_plan.h"  // 复用 UndoQueryRead / ReadUndoQuery 这一套判读

namespace gc::git {

// 「push」的可移植決策邏輯（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）：
//   1) 構造預檢要發的只讀查詢參數，以及點頭之後真正發出的那條 push 參數；
//   2) 把平台層帶回的 GitQueryResult 判讀成乾淨的事實（PushPreflightFacts）；
//   3) 把事實核對成「可以推送／必須拒絕」，並湊出命令、目標展示與確認文字；
//   4) 執行前復核（DescribePushChange）與執行後的實況核對（DescribePushVerification）。
//
// 範圍口徑（本步驟的承諾，全部落在命令形態裡）：
//   * 只推當前分支這一条引用：命令帶**完整顯式 refspec**
//     `<本地分支現在的完整提交ID>:refs/heads/<遠端分支>`——源側釘在確認那一刻問回的
//     完整物件 ID 上（不再寫會變的分支名），兩個位置都由 Git 自己給出的回答構成，
//     本程式不改寫、不縮寫、不「就近取一個同名分支」；
//   * 沒有 --force、--force-with-lease、--mirror、--all、--tags、--follow-tags，
//     也不推標籤、不递归子模組（--recurse-submodules=no 寫死在命令里）；
//   * 沒有 --no-verify：pre-push hook、簽名與憑據交互全部照使用者既有設定生效。
//
// 以下幾條是「顯式 refspec 到底能不能擋住倉庫配置」的本機實測結論（Git 2.53.0.windows.3，
// 全部在臨時倉庫＋同根 bare 遠端做出，測試裡逐條钉死）：
//   * push.default = nothing 時帶 refspec 仍然推送成功 ⇒ 顯式 refspec 覆蓋 push.default，
//     不會因使用者的 push.default 而多推分支或改推別的分支；
//   * remote.<遠端>.push 配了額外 refspec（標籤／全部分支）時，多餘的 refspec **不會**被帶上，
//     對端只收到命令行這一條；
//   * push.followTags = true 與 remote.<遠端>.tagOpt = --tags 同理：帶顯式 refspec 時標籤不會過去；
//   * remote.<遠端>.mirror = true 卻會讓整條命令以 128 失敗
//     （`fatal: --mirror can't be combined with refspecs`）——因此本程式在它存在時補一句
//     `-c remote.<遠端>.mirror=false`（實測能把這條死路還原成「只推這一条」）；
//   * 同一遠端配了多個 remote.<遠端>.pushurl 時，`git push <遠端>` 會**推給每一個**（實測兩個 bare
//     都收到了同一份提交）。這不是本程式擴大範圍，是 Git 的既定行為：界面必須把每一個目標都列出來，
//     並要求使用者明確點頭；多個目標合在一條命令裡，退出碼是各目標的合計，因此逐目標的
//     「到底收到沒有」以推送後的逐目標核實為準；
//   * `git remote get-url --push --all <遠端>` 由 Git 自己算出「實際會去哪裡」的**全部**地址
//     （pushurl 優先於 url，`url.*.insteadOf`／落回 url 場合的 `pushInsteadOf` 改寫已經疊好，
//     順序按配置原樣），本程式拿它當場證據展示與核實，不自己 reimplement 改寫規則。
//
// 「不猜」的界線：
//   * 必須在分支上（游離 HEAD 沒有分支可推，本程式不替它挑一個）；本模組的 PushPlan 只服務
//     「已有明確上游」的推送（branch.<分支>.remote + .merge），無上游時一律 blocked：
//     不 --set-upstream、不猜 origin/main、不創建遠端分支設定、不寫任何配置文件。
//     這種場合界面是否改走「首次推送嚮導」（用户當場選遠端與目標分支、推送與寫上游分成
//     兩個各有結果的階段）由 git/first_push_plan 裁決，本模組的拒絕契約不變；
//   * 分支還沒有任何提交（HEAD 不可解析）時拒絕：那種場合连「要推哪一份」都問不出來；
//   * 實際發布目標與界面上的抓取目標不一致時（branch.<分支>.pushRemote／remote.pushDefault／
//     獨立 pushurl／insteadOf 改寫）一律解析出來如實展示，並列為需明確點頭的風險；
//     目標遠端根本不在倉庫的遠端清單裡、或問不出一個可用的發布 URL——拒絕，不湊合，
//     也**絕不退回配置裡未經解析的原樣位址**繼續發布。
//
// 發布 URL 的解析（本任務的關鍵修復，全部是本機 Git 2.53.0.windows.3 實測）：
//   * 查詢形態是 `git remote get-url --push --all <遠端>`：不加 `--all` 時多個 pushurl 只回答
//     第一條；加上之後 Git 把**每一個實際發布地址**都按配置順序答出來，`pushurl` 優先於 `url`、
//     `url.*.insteadOf` 的改寫已疊在每條上。`--push` 對「落回 url 的場合」還會疊 pushInsteadOf
//     （實測：無 pushurl 時 `--push` 答出 pushInsteadOf 改寫後的地址，而抓取方向的 get-url 不變）；
//     對**顯式 pushurl 條目**它不疊 pushInsteadOf（實測），本程式把 get-url 的回答當作唯一依據。
//   * 這條查詢失敗（非 0、啟動不成、輸出空、行數與配置清單裡的條目數對不上、答出的地址含
//     控制字元這種沒法作為單個參數交出去的形態）就是「看不清要去哪裡」：整次推送拒絕，
//     不拿未經解析的原樣 URL 頂替。
//   * 核實（ls-remote）問的就是上面這一份已展開的 URL 清單——與確認框承諾的是同一批地點。
//     已展開的地址再交回 Git 時仍可能被 url.*.insteadOf 命中而**再次改寫**（實測：空值的
//     `-c url.<base>.insteadOf=` 並不能中和既有規則，所以本程式不做那種假中和）；核實因此
//     如實陳述「問到哪裡算哪裡」，核不上就是核不上，不猜。
//
// 源的綁定（本任務的另一半）：命令的源側寫的是確認那一刻問回的**完整提交 ID**
// （`<oid>:refs/heads/<遠端分支>`），不再寫會被人挪走的分支名——這樣「確認推哪一份」與
// 「Git 真正送出哪一份」是同一件事，執行之前外部把分支推進了也不會把沒確認過的提交帶出去。
// 目標側的完整 refspec、非強制（無 --force／--force-with-lease）、單引用範圍、不推標籤與
// 子模組等承諾全部原樣保留。源側改成對象 ID 這一形态對 pre-push hook、本地跟蹤引用前移與
// 多發布目標行為的影響，由 push_fixture_tests 在真實 Git 上逐條钉住（钩子仍被唤起、對端每條
// 目標都收到、跟踪引用是否前移以 Git 的回答為準），界面上「推送成功與否以發布目標核實為準」的
// 口径不依賴跟蹤引用，所以這一形態不會弱化任何既有安全承諾。
//
// 本地對遠端的了解只到上一次 fetch 為止：refs/remotes/<遠端>/<分支> 的位置不等於遠端現在的位置。
// 因此「本地領先幾個」這句只用於說明與預警，絕不用來宣稱推送成功；推送之後本程式另向**發布目標**
// （不是抓取的遠端）發一次只讀 ls-remote 核對那一条引用到底停在哪（PushVerification），
// 核不上就是核不上，命令退出碼 0 也不算成功；「命令成功但一個目標都没核到」與「核到但不是
// 那一份」是兩種結論，前者說「已推送但未核實」，後者說「與預期不符」。
//
// 各查詢全部帶 `--no-optional-locks`；symbolic-ref／rev-parse --verify 帶 `--quiet`
// （「不在分支上」「引用不可解析」以退出碼 1 + 空輸出作答，是明確答案而不是錯誤）；
// `git config --list --null` 一次問回這個倉庫此刻的全部生效配置（include 疊加、各層優先序都由
// Git 自己算好），比逐條 `config --get` 少開十來個進程，而且多值鍵（remote.<x>.url／pushurl、
// remote.<x>.push、url.*.insteadOf）的重複項原樣保留在順序裡。它的記錄形態是本機 Git 2.53
// od -c 逐字節實測的三種：`key<換行>value<NUL>`（普通條目；值裡可能有換行，所以只按**第一個**
// 換行切）、`key<NUL>`（布爾鍵省略取值——配置文件裡不寫 `= `的那種寫法，語義就是 true）、
// `key<換行><NUL>`（顯式空值，Git 布爾解析裡空串是 false，與上一種、與「根本沒有這個鍵」是三件
// 不同的事）。判讀函數同時認 `key=value<NUL>` 那一種歷史寫法。除此之外認不出來的記錄
// （空鍵、變量名段含非法字符）就整份拒用（看不清實際發布目標就不推）。

// ---- 生效配置清單（`git config --list --null` 的判讀結果） ----

// 三段「能不能安全交出去」的形態底線。普通推送與「首次推送」向導共用這一份判定：
// 本地分支引用必須是 refs/heads/ 開頭，遠端側引用必須是 refs/ 開頭（上游把分支映射到
// refs/heads/ 以外是 Git 允許的形態，本程序照它推、不糾正），遠端名不得為空或以 - 開頭；
// 三者都不接受雙引號、控制字符，refspec 源與目標還不接受冒號。返回 false 就是「不構造命令」，
// 由調用方給出具體原因，絕不湊一個看起來能跑的。
[[nodiscard]] bool IsUsableLocalBranchRef(std::wstring_view ref);
[[nodiscard]] bool IsUsableRemoteRef(std::wstring_view ref);
[[nodiscard]] bool IsUsableRemoteName(std::wstring_view name);

// 一條生效配置。Git 明確分得開三件事，這裡必須原樣帶過去：
//   * 「根本沒有這個鍵」——entries 裡查不到；
//   * 「設成了空值」（`key =` 這種寫法，輸出是 `key<換行><NUL>`）——valueOmitted=false、value 為空；
//   * 「布爾鍵省略取值」（配置檔裡只寫鍵名、不寫 `= `，輸出是 `key<NUL>`）——valueOmitted=true。
//     省略取值在 Git 的布爾讀取裡就是 true；顯式空值經 `git_parse_maybe_bool` 是 false。
//     這兩者都跟「沒設」不同，混起來判讀就會把「用户没开 mirror」讀成「开了」或反之。
struct PushConfigEntry {
  std::wstring key;    // 段/變數名已由 Git 小寫化；subsection（遠端名、分支名）原樣保留大小寫
  std::wstring value;  // valueOmitted 為 true 時是空串
  bool valueOmitted = false;
};

// 一次問回的「這個倉庫此刻真正生效」的配置條目，按 Git 給出的順序原樣保留。
// 同一個鍵多次出現就是多值鍵：單值語義取最後一條（與 `git config --get` 一致），多值語義取全部。
struct PushConfigListing {
  bool readOk = false;
  std::wstring readFailure;
  std::vector<PushConfigEntry> entries;

  // 單值查詢：沒有這一項時返回空字串。需要區分「沒設／設成空值／省略取值」的調用方
  // 用 LastEntry()（找不到返回 nullptr），HasKey() 只回答存不存在。
  [[nodiscard]] std::wstring Value(std::wstring_view key) const;
  [[nodiscard]] std::vector<std::wstring> Values(std::wstring_view key) const;
  [[nodiscard]] bool HasKey(std::wstring_view key) const;
  // 該鍵最後一次出現的那條（Git 的單值讀取同樣取最後一條）。沒有這個鍵時返回 nullptr。
  [[nodiscard]] const PushConfigEntry* LastEntry(std::wstring_view key) const;
  // `remote.<名字>.<變數>` 形式的鍵：subsection 為遠端名（Git 原樣保留大小寫）。
  [[nodiscard]] std::vector<std::wstring> RemoteValues(std::wstring_view remoteName,
                                                       std::wstring_view variable) const;
  // 倉庫既有遠端：有 url 或 pushurl 的遠端名，按首次出現的順序去重。
  [[nodiscard]] std::vector<std::wstring> RemoteNames() const;
  // 這個遠端在倉庫的遠端清單裡（有 url 或 pushurl）——與「定得出遠端名」是兩件事。
  [[nodiscard]] bool HasRemote(std::wstring_view remoteName) const;
};

[[nodiscard]] PushConfigListing ParsePushConfigListing(const GitQueryResult& listing);

// 把一個可能內嵌帳號口令的 URL 收成可展示形態：`https://user:token@host/path` →
// `https://***@host/path`。推送命令裡出現的是遠端**名字**而不是 URL，但展示與日誌裡一旦出現
// URL（pushurl／get-url 的回答／核實目標）就必須先過這一道，避免把憑據抄進界面。
// 無 userinfo 的 URL（含本機絕對路徑、scp 形態但無 @ 的寫法）原樣返回。
[[nodiscard]] std::wstring MaskPushUrlCredentials(std::wstring_view url);

// 對一段自由文字（Git 的錯誤轉述、查詢失敗原因、诊断摘要）裡**每一處** `scheme://…@…`
// 形態的 URL 套用同一個掩碼。發布鏈路上凡要把 Git 原文帶進界面的地方都先過這一道，
// 免得 `fatal: … 'https://user:token@host'` 這種回答把口令攤到螢幕上。
[[nodiscard]] std::wstring MaskPushUrlCredentialsInText(std::wstring_view text);

// ---- 落盘/导出用的保守脱敏（比展示掩码多去掉查询串与 fragment）----
// 上面两道只收 userinfo，而对象存储型远端把令牌放在**查询串**里（Azure Blob、SAS、
// 各类 presigned URL：`https://host/repo.git?sig=…&token=…`）或 fragment 里，那同样是凭据。
// 凡是写进本地记录文件、导出文件、恢复说明的字段一律走这里；界面上「将要去哪个地址」
// 仍然走上面的展示掩码，因为用户需要看见完整路径才能核对目标。
// 这两个函数只产生「给人看/给记录用的副本」，真正执行与核实用的地址必须保持原值，
// 绝不允许把这些返回值回填成 Git 参数。
// 规则（保守，但不靠关键字表）：
//   * 带 scheme 的 URL：掩码 userinfo，并去掉第一个 '?' 或 '#' 起的全部尾串（查询串 + fragment）。
//   * scp 形态 `user@host:path`：掩码 user 段（Git 文档承认的写法，同样可能带敏感账号）。
//   * 本机绝对路径、相对路径、不含凭据位置的文本：原样返回，绝不改动分支名或路径。
[[nodiscard]] std::wstring MaskStoredPushUrl(std::wstring_view url);

// 自由文字里的每一处 URL 按上面的规则逐处处理（多地址、句读收尾、引号包裹都要拦住）。
[[nodiscard]] std::wstring MaskStoredPushUrlInText(std::wstring_view text);

// 多個目標 URL 折成一行展示文字（逐條編號，全部已做憑據掩碼）。
[[nodiscard]] std::wstring FormatPushUrlList(const std::vector<std::wstring>& urls);

// ---- 發布 URL 解析與範圍設置判定（普通推送與「首次推送嚮導」共用的唯一一份） ----
// 抽出來不是為了整齊：同一條解析鏈路若有兩份實現，界面承諾的地点與命令實際去的地方
// 就可能各算一套。首次推送的一切範圍承諾都以這裡的函數為準。

// 配置裡這個遠端**未經解析**的地址清單：有 pushurl 就用 pushurl，一條 pushurl 也沒有才落回 url。
// 它的用途是條數核對（`get-url --push --all` 的回答條數理應與它一致）與辨認 insteadOf 改寫過，
// 绝不作为实际发布地址使用。
[[nodiscard]] std::vector<std::wstring> RawConfiguredPushUrls(const PushConfigListing& config,
                                                             std::wstring_view remoteName);

// 一次發布 URL 解析的結論。urls 為空就是「看不清要去哪裡」，failure 給出面向界面的完整說明；
// 這裡永遠不會填入配置裡未經解析的原樣地址。
struct PushUrlResolution {
  std::vector<std::wstring> urls;
  std::wstring failure;
  bool rewritten = false;  // 回答與原樣清單不完全一致：改寫規則生效過
};

// 判讀 `git remote get-url --push --all <遠端>` 的回答：空回答、含控制字符的地址、
// 與配置條目數對不上、非 0 退出、啟動不成、压根沒發問——各自都是失敗，且都拒絕繼續發布。
[[nodiscard]] PushUrlResolution ResolvePushUrls(const GitQueryResult& remoteUrlQuery, bool queryRan,
                                                const PushConfigListing& config,
                                                std::wstring_view remoteName);

// 多個發布地址時必須交代的事（Git 會推給每一個、退出碼是各目標合計、二次改寫的邊界），
// 單一地址但被改寫過時改說改寫。不需要特別說明時返回空字串。
[[nodiscard]] std::wstring DescribePushUrlNote(const std::vector<std::wstring>& urls, bool rewritten,
                                               const std::vector<std::wstring>& rawUrls);

// 會被本次推送就地中和／不受影響的倉庫設定（只影響這一個子進程，不寫任何配置文件）。
// 命令要不要補那幾句 `-c`、確認框要不要說明，全部以這裡的判定為準。
struct PushScopeConfigEffects {
  bool mirrorConfigured = false;
  bool tagOptConfigured = false;
  bool followTagsConfigured = false;
  bool extraPushRefspecsConfigured = false;
  std::wstring pushDefault;  // 僅供展示：顯式 refspec 讓它不参与本次推送
};

[[nodiscard]] PushScopeConfigEffects InspectPushScopeConfig(const PushConfigListing& config,
                                                            std::wstring_view remoteName);

// 「會被本次命令就地中和的設置」那幾句說明。普通推送與首次推送共用同一份措辭與同一批判定：
// 兩處各寫一套就會出現「一個入口說了、另一個入口漏說」的中和項。
[[nodiscard]] std::wstring DescribePushNeutralization(std::wstring_view remoteName,
                                                      const PushScopeConfigEffects& effects);

// ---- 只讀查詢的參數（全部顯式 -C 綁定倉庫根，不依賴進程全局目錄） ----

// 一次問回全部生效配置。--null：條目以 NUL 分隔，值裡的換行不會被誤當成條目邊界。
[[nodiscard]] std::vector<std::wstring> BuildPushConfigListingArguments(
    std::wstring_view repositoryDirectory);
// 由 Git 自己算「推這個遠端時實際去哪裡」的**全部**地址（--push --all：pushurl 優先於 url，
// insteadOf／落回 url 場合的 pushInsteadOf 改寫已疊好，按配置順序逐條回答）。
// remoteName 為空時返回空數組：連目標名字都沒定下來，就沒有可問的 URL。
[[nodiscard]] std::vector<std::wstring> BuildPushRemoteUrlArguments(std::wstring_view repositoryDirectory,
                                                                    std::wstring_view remoteName);
// 點擊「推送」之後在命令窗口裡發出的那條 push。源側是確認那一刻問回的**完整提交 ID**
// （不是會被人挪走的分支名）：sourceObjectId 必須通過 LooksLikeFullObjectId，remoteBranchRef
// 必須是 `refs/` 開頭的完整引用，remoteName 形態合格；任一不合格（空、含 `:`、引號、控制字元）
// 返回空數組：調用方據此拒絕，本函數絕不「湊一個能跑的」出來。
// 兩個 neutralize 標記決定要不要為這一個子進程補 `-c` 覆蓋（倉庫裡確實有那條設定時才傳 true，
// 實測見文件頭：mirror 会让整條命令以 128 失敗，tagOpt/--tags 則是可能把標籤一起帶走的設定）。
[[nodiscard]] std::vector<std::wstring> BuildPushCommandArguments(std::wstring_view remoteName,
                                                                 std::wstring_view sourceObjectId,
                                                                 std::wstring_view remoteBranchRef,
                                                                 bool neutralizeMirror,
                                                                 bool neutralizeTagOpt);
// 推送之後向**發布目標**核對那一条引用：`git ls-remote -- <發布 URL> <遠端引用>`。
// url 為空或引用不合格時返回空數組。-- 之後才放 URL：使用者配的 URL 可能以 `-` 開頭，
// 那種寫法會被 Git 當成選項，隔開之後它只可能是倉庫位址。
[[nodiscard]] std::vector<std::wstring> BuildPushRemoteProbeArguments(std::wstring_view repositoryDirectory,
                                                                      std::wstring_view pushUrl,
                                                                      std::wstring_view remoteBranchRef);

// ---- 方案 ----

// 本地相對「上一次 fetch 讀回的那個位置」領先/落後幾個提交。
// 這個方位感的依據是本地遠端跟蹤引用，與實際發布目標是否同一個地方由 pushTargetIsFetchTarget 決定。
struct PushRelationshipFacts {
  bool known = false;
  long long ahead = 0;   // 只有本地有的提交數（這次要送出去的量）
  long long behind = 0;  // 只有那個引用有的提交數（本地缺的：推送會被非快進拒絕）
  std::wstring detail;   // 問不到時的具體說明
};

struct PushPreflightFacts {
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
  std::wstring upstreamRemote;        // 上游所在遠端（抓取的這一側）
  std::wstring upstreamTrackingRef;   // refs/remotes/origin/main
  std::wstring upstreamRemoteRef;     // refs/heads/main（遠端那邊的分支引用）

  bool trackingRan = false;
  bool trackingResolved = false;
  std::wstring trackingObjectId;
  std::wstring trackingDetail;        // 「本地還沒有這個引用」等可展示說明

  // ---- 發布目標（可能与抓取目標不是同一個地方） ----
  bool configOk = false;
  PushConfigListing config;
  std::wstring pushRemoteName;        // 實際要推的那個遠端名
  std::wstring pushRemoteSource;      // 這個遠端名是哪條配置定的（展示用原文）
  bool pushRemoteExists = false;      // 它在倉庫的遠端清單裡
  std::vector<std::wstring> pushUrls;  // `get-url --push --all` 答出的生效發布 URL（未掩碼，
                                       // 展示前才掩碼）；解析不成時為空，絕不塞原樣地址頂替
  std::vector<std::wstring> rawPushUrls;  // 配置裡的原樣 pushurl／url 清單（用於辨認改寫與條數核對）
  bool pushUrlRewritten = false;      // get-url 的回答與配置原樣清單不完全一致：改寫規則生效過
  std::wstring pushUrlNote;           // 關於發布 URL 還得說一句的話（多目標與二次改寫的邊界）
  bool pushTargetIsFetchTarget = false;  // 發布目標與上游所在的抓取目標是同一個地方
  std::wstring pushUrlFailure;        // 問不出發布 URL 時的具體說明

  // 會被本程式就地中和的設定（只影響這一個子進程，不寫任何配置文件）。
  bool mirrorConfigured = false;
  bool tagOptConfigured = false;
  bool followTagsConfigured = false;
  bool extraPushRefspecsConfigured = false;
  std::wstring pushDefault;           // 僅供展示：顯式 refspec 讓它不参与本次推送

  PushRelationshipFacts relationship;

  // 流程痕迹：推送不動索引與工作區，因此這裡只用於「還有一次合併沒走完」這種老實提醒，
  // 不構成拒絕理由（那種場合 HEAD 上有的東西本來就推得出去）。
  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
};

// 「實際要推給哪個遠端」的裁決結果。優先序照 Git 自己的規矩：
// branch.<分支>.pushRemote > remote.pushDefault > branch.<分支>.remote（也就是上游所在的那個遠端）。
// 本程式把這個名字算出來以後**顯式**寫進命令，於是 Git 內部那條優先序就不再參與——
// 界面上寫的是哪個遠端，命令裡推的就是哪個遠端。
// remoteName 為空表示定不下來（設成了空值、或根本沒有可依據的遠端），source 給出原因。
struct PushRemoteChoice {
  std::wstring remoteName;
  std::wstring source;
};

[[nodiscard]] PushRemoteChoice ResolvePushRemote(const PushConfigListing& config,
                                                 std::wstring_view branchName,
                                                 std::wstring_view upstreamRemote);

// 平台層在中間一步就要先定出遠端名，才能問「這個遠端實際去哪裡」：配置清單判讀與
// get-url 查詢因此分成兩趟，這裡是共用的那份裁決依據（與 InterpretPushPreflight 內部同一個函數）。
[[nodiscard]] PushRemoteChoice ResolvePushRemoteFromQueries(const GitQueryResult& configListing,
                                                            std::wstring_view branchName,
                                                            std::wstring_view upstreamRemote);

// 一次推送預檢的全部只讀查詢結果。哪幾條發了、哪幾條因為前提不成立而根本沒發，用 *Ran 如實帶入，
// 判讀函數自己不猜（與 pull 的 PullTargetQueries 同一套規矩）。
struct PushPreflightQueries {
  GitQueryResult symbolicRef;
  GitQueryResult headObject;
  GitQueryResult upstream;
  bool upstreamRan = false;
  GitQueryResult trackingObject;
  bool trackingRan = false;
  GitQueryResult configListing;
  GitQueryResult remoteUrl;      // `git remote get-url --push --all <遠端>`
  bool remoteUrlRan = false;
  GitQueryResult aheadBehind;
  bool aheadBehindRan = false;
  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
};

[[nodiscard]] PushPreflightFacts InterpretPushPreflight(const PushPreflightQueries& queries);

// ---- 方案：点头之前 ----

enum class PushPlanState {
  blocked = 0,  // 前提不成立：不產生命令
  ready,        // 目標與範圍已定：給確認框（有風險時要求明確點頭）
};

struct PushPlan {
  PushPlanState state = PushPlanState::blocked;
  std::wstring explanation;  // 兩種狀態都要有把話說完的文案（界面原樣展示）

  std::wstring branchName;       // 源分支名
  std::wstring localBranchRef;   // refs/heads/main
  std::wstring remoteBranchRef;  // refs/heads/main（遠端那邊）
  std::wstring remoteName;       // 目標遠端
  std::wstring remoteUrlDisplay;  // 發布目標（已掩碼，逐條列出 get-url --push --all 的全部回答）
  std::wstring remoteNameSource;  // 這個遠端名是誰定的
  std::wstring pushedObjectId;    // 這次要送出去的那一份提交（完整 ID；命令源側釘的就是它）
  std::vector<std::wstring> pushUrls;  // 核實用：與界面展示、命令發布目標同一批已展開位址

  bool requiresForce = false;   // 有風險條目：必須點「仍要推送」
  std::vector<std::wstring> risks;
  std::vector<std::wstring> notes;  // 不構成風險、但界面必須交代的事（方位感來源、中和了哪條配置）

  std::vector<std::wstring> arguments;  // BuildPushCommandArguments 的原樣結果
  std::wstring operationId;             // 執行器操作 ID（純 ASCII：push）
  std::wstring displayName;             // L"推送"
  std::wstring commandLabel;            // 展示用命令（不含任何憑據）
  std::wstring confirmationText;
  std::wstring notice;                  // 隨操作一直顯示的範圍說明
};

// 主入口：核對事實並產出方案。前提不成立一律 blocked（不產生任何命令）。
// repositoryDirectory 只用於確認文字里展示工作目錄，不進任何命令參數。
[[nodiscard]] PushPlan BuildPushPlan(const PushPreflightFacts& facts,
                                     std::wstring_view repositoryDirectory = {});

// ---- 點頭之後、啟動命令之前的執行前復核 ----

// 比對兩份預檢事實：分支、HEAD 那一份提交、上游指向、發布目標（遠端名與 URL 清單）、
// 會被中和的那些設定的有無。任何一處對不上，預檢得出的那份方案就不屬於此刻的倉庫。
// 工作區/索引的變動**不**在比對之內：推送只送已有的提交，不碰工作區，
// 使用者在點頭之後又改了一個沒暫存的文件不該讓一次合法的推送作废。
// 返回空字串表示一致，可以照原方案執行；非空就是要原樣展示的原因。
[[nodiscard]] std::wstring DescribePushChange(const PushPreflightFacts& preflight,
                                              const PushPreflightFacts& latest);

// ---- 推送之後：向發布目標核對那一条引用 ----

// 對一個發布 URL 的核對結果（ls-remote 由平台層執行，這裡只做判讀與文案）。
struct PushTargetCheck {
  std::wstring url;             // 核對的對象（展示前已掩碼）
  bool queried = false;         // 這個目標到底問過沒有
  bool ok = false;              // 問成功了（退出碼 0）
  bool refPresent = false;      // 那個引用在對端存在
  std::wstring remoteObjectId;  // 它在對端停著的那一份提交（完整 ID）
  std::wstring failure;         // 問不到時的具體說明
};

// 把 ls-remote 的原始回答判成一個目標的核對結果：只看引用名完全等於 remoteBranchRef 的那些行
// （ls-remote 的模式匹配是寬鬆的，可能順帶回別的行）。
[[nodiscard]] PushTargetCheck InterpretPushTargetCheck(std::wstring_view url,
                                                      const GitQueryResult& lsRemoteResult,
                                                      std::wstring_view remoteBranchRef);

enum class PushVerificationVerdict {
  nothingChecked = 0,  // 一個目標都沒問成：結論只能以命令窗口的輸出為準
  confirmed,           // 每個問到的目標上那條引用都正是剛推出去的那一份
  mismatched,          // 至少一個目標不是（或還不在）那一份提交
  mixed,               // 有目標核上了、有目標沒問到（多個發布 URL 常見）
};

[[nodiscard]] std::wstring_view PushVerificationVerdictLabel(PushVerificationVerdict verdict) noexcept;

struct PushVerificationReport {
  PushVerificationVerdict verdict = PushVerificationVerdict::nothingChecked;
  std::wstring headline;             // 一句話結論（界面原樣進狀態欄）
  std::vector<std::wstring> lines;   // 逐目標的證據，界面原樣逐行展示
};

// 把「命令窗口的結果」與「向發布目標核到的實況」合成一份結論。
// 兩者哪個都不能少：退出碼 0 只說明 Git 自己認為推成功了，核對才說明對端現在到底是什麼。
// commandSucceeded=false 時本函數絕不寫「推送成功」；commandSucceeded=true 而核對不上時
// 也絕不寫「已核實」。三種口径必須分得開：
//   * 命令成功、某目標核到同一份提交 —— 該目標「已核實」；
//   * 命令成功、某目標核到別的位置（對端又被人推進過、地址根本不對）——「與預期不符」；
//   * 命令成功、某目標根本沒問到（認證、權限、網路、核實被二次改寫）——「已推送但未核實」，
//     這既不是成功也不是失敗，報告逐目標原樣保留，界面據此決定彈不彈窗，全程不自動重推。
[[nodiscard]] PushVerificationReport ComposePushVerification(
    const std::vector<PushTargetCheck>& checks, std::wstring_view expectedObjectId,
    std::wstring_view remoteBranchRef, bool commandSucceeded, std::wstring_view commandConclusion);

}  // namespace gc::git
