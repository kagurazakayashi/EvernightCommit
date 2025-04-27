#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/repository.h"

namespace gc::git {

// 「一次抓取的範圍」策略——界面 fetch 按鈕與 pull 第一步**共用同一份**參數生成、
// 配置判讀、範圍驗證與確認文字（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）。
//
// 產品承諾（這句話就是本模組要負責到底的內容）：
//   這次抓取只更新**明確選中的那個遠端**在受允許命名空間（refs/remotes/<遠端名>/ 之下）
//   的遠端跟踪引用；不刪除任何引用、不寫本地分支與本地標籤、不遞歸子模組。
//   要擴大這個範圍必須另有明確的產品授權，不能靠改確認框的措辭把保護抹掉。
//
// 為什麼「只寫 --recurse-submodules=no」遠遠不夠（本机 Git 2.53.0.windows.3 實測，
// 語義依據是 Git 自己的 git-fetch(1)/git-config(7) 文件）：
//   * fetch.prune 與 remote.<遠端>.prune：文件寫明「等同在命令行上給了 --prune」，會**刪除**
//     過期的遠端跟踪引用。實測把任一條設成 true 之後 `git fetch --dry-run -v <遠端>` 就報出
//     `[deleted] …`，加上 --no-prune 之後不再報 → 命令行明示可以中和，本模組每次都寫。
//   * fetch.pruneTags 與 remote.<遠端>.pruneTags：文件寫明它「等同於另外聲明
//     refs/tags/*:refs/tags/*」，動的是**本地標籤**。實測 stale-tag 會被列進刪除，
//     加 --no-prune-tags（或 --no-prune）後不再出現 → 兩個都寫：--no-prune-tags 管的是
//     「那句標籤映射根本不被加入」，不只是「別刪」。
//   * Git 預設會自動跟隨標籤（抓下來的對象所指的標籤寫進 refs/tags/）：--no-tags 關掉。
//     remote.<遠端>.tagOpt 就是這個預設值，文件對它的說明寫著「Passing these flags directly
//     to git-fetch(1) can override this setting」，Git 1.7.2.3 的發布說明也確認了命令行優先。
//   * remote.<遠端>.fetch（Effective fetch refspec）才是「哪些本地引用會被寫」的總閘門：
//     命令行上沒有任何參數能覆蓋它，而一旦在命令行上改給 refspec，使用者自己配的分支過濾與
//     上游映射就會被一起替換掉。所以這條只能**驗證**不能**中和**：映射落在 refs/remotes/<遠端>/
//     之外的（refs/heads/、refs/tags/、別的遠端命名空間、鏡像倉庫那種 +refs/*:refs/*）
//     一律執行前拒絕並給出具體那一句配置，本程序不改寫使用者配置、也不替它猜一個映射。
//     合法用法必須原樣放行：`+refs/heads/main:refs/remotes/origin/main` 這種單分支過濾、
//     以及 `^refs/heads/secret` 這種負 refspec（文件寫明負 refspec 只排除、不含目標端）。
//   * remote.<遠端>.mirror=true 只影響 push（文件明示），對 fetch 不構成擴大；鏡像克隆真正
//     越界的是它那條 +refs/*:refs/* 映射，由上面那條檢查抓到。
//   * fetch.all=true 會被「命令行上顯式點名遠端」覆蓋（文件明示），本模組每次都只點名一個遠端；
//     remotes.<組> 只在 --all / --multiple / `git remote update` 裡展開（實測：普通
//     `git fetch <組名>` 把組名當成倉庫名而直接失敗），這三種形態本程序都不發。
//   * remote.<遠端>.url 多個地址時文件寫明「the first is used for fetching」，其餘地址不參與
//     這次抓取，因此只作披露、不构成範圍擴大。
//   * `.git/branches/<名>` 那種 Git 正在淘汰的檔案形態，文件寫明它的 fetch 映射是
//     `refs/heads/<head>:refs/heads/<分支>`——直接寫本地分支。它根本不會出現在 `git remote`
//     的清單裡，而本程序的候選遠端一律來自 `git remote`，因此不可能被選中。
//
// 查詢形態：兩條 `git config --null --get-regexp`（一次問遠端級、一次問全局級），把上面這些
// 鍵一次讀回。記錄約定與 git/push_plan.h 對 `config --list --null` 的判讀一致——本機實測為
// `鍵<換行>值<NUL>`；**布林鍵省略取值時**（`[fetch]` 段落裡單獨一行 `prune`）實測只輸出
// `鍵<NUL>`，那種寫法在 Git 的布林語法裡就是真，因此判讀函數把它如實記成 hasValue=false，
// 而不是「讀到半條」。鍵的歸屬按「第一個點與最後一個點」切：section 與變數名不含點、
// 遠端名可以含點（實測 `git remote add my.remote` 合法），這樣切才不會把
// `remote.my.remote.prune` 誤讀成遠端「my」的「remote.prune」。
// 輸出必須先過 NulRecordsAreComplete 的記錄邊界門檻；讀不全、認不出的記錄一律整份拒絕——
// 看不全配置就谈不上「這次抓取不會越界」。
//
// 各查詢全部帶 `--no-optional-locks`；`--get-regexp` 沒有任何匹配時以退出碼 1 + 空輸出作答
// （「確實沒有這類配置」是明確答案，不是失敗），與 ReadUndoQuery 的 noResult 同一套判讀。

// 這次抓取在命令行上寫死的中和項。兩個入口都只能經本模組取參數，不準各拼一套。
inline constexpr std::wstring_view kFetchRecurseSubmodulesOff = L"--recurse-submodules=no";
inline constexpr std::wstring_view kFetchPruneOff = L"--no-prune";
inline constexpr std::wstring_view kFetchPruneTagsOff = L"--no-prune-tags";
inline constexpr std::wstring_view kFetchTagsOff = L"--no-tags";

// 一條生效配置記錄。hasValue 為 false 是 Git 的「鍵寫了、取值省略」形態（布林意義上的真）。
struct FetchScopeRecord {
  std::wstring key;    // Git 原樣給出的鍵（section 與變數名已小寫化，遠端名保留大小寫）
  std::wstring value;  // hasValue 為 false 時為空
  bool hasValue = true;
};

// 平台層按順序執行兩條配置查詢後原樣交進判讀函數。
struct FetchScopeQueries {
  GitQueryResult remoteConfig;  // remote.<名>.{fetch,url,prune,pruneTags,tagOpt,mirror}
  GitQueryResult globalConfig;  // fetch.{prune,pruneTags,all,recurseSubmodules} 與 submodule.recurse
};

// 某個遠端身上會影響抓取範圍的配置（按 Git 給出的順序原樣保留，重複項就是多值鍵）。
struct FetchRemoteScope {
  std::vector<std::wstring> fetchRefspecs;
  std::vector<std::wstring> urls;
  std::vector<FetchScopeRecord> prune;
  std::vector<FetchScopeRecord> pruneTags;
  std::vector<FetchScopeRecord> tagOpt;
  std::vector<FetchScopeRecord> mirror;
};

struct FetchScopeFacts {
  bool readOk = false;
  std::wstring readFailure;  // readOk 為 false 時面向界面的完整說明

  std::vector<FetchScopeRecord> globalPrune;
  std::vector<FetchScopeRecord> globalPruneTags;
  std::vector<FetchScopeRecord> globalFetchAll;
  std::vector<FetchScopeRecord> globalRecurse;      // fetch.recurseSubmodules
  std::vector<FetchScopeRecord> submoduleRecurse;   // submodule.recurse（fetch 的預設來源之一）
  std::vector<FetchRemoteScope> remotes;            // 與 remoteNames 一一對應
  std::vector<std::wstring> remoteNames;            // 按首次出現順序去重的遠端名
};

// ---- 只讀查詢的參數（全部顯式 -C 綁定倉庫根，不依賴進程全局目錄） ----

[[nodiscard]] std::vector<std::wstring> BuildFetchScopeRemoteConfigArguments(
    std::wstring_view repositoryDirectory);
[[nodiscard]] std::vector<std::wstring> BuildFetchScopeGlobalConfigArguments(
    std::wstring_view repositoryDirectory);

// 把兩條配置查詢的回答判讀成事實。純函數：輸入全部是平台層帶回的結果，可用樁完整測試。
[[nodiscard]] FetchScopeFacts InterpretFetchScope(const FetchScopeQueries& queries);

// ---- 範圍決策：參數生成 + 驗證 + 可展示的範圍文字 ----

struct FetchScopeDecision {
  bool allowed = false;
  std::wstring refusal;  // allowed 為 false 時把話說完的原因（界面原樣展示，不產生命令）

  std::vector<std::wstring> arguments;  // {fetch, 四個中和項…, <遠端名>}；拒絕時為空
  std::wstring commandLabel;            // 展示用的完整命令

  // 確認框共用的正文：範圍承諾 + 會變化的東西 + 這次被中和的配置逐條 + 「這不是事務隔離」。
  // 兩個入口把這一段原樣放進自己的確認框，不得各自改寫其中任何一句。
  std::wstring scopeParagraph;
  // 隨操作一直顯示的範圍說明正文（同樣兩個入口共用，各自再加自己的前綴）。
  std::wstring noticeCore;
  std::vector<std::wstring> disclosures;  // 中和/覆蓋項原文（已限長、URL 已做憑據掩碼）
};

// 核心入口：核對事實里這個遠端的抓取範圍，能承諾就給出參數與文字，不能承諾就拒絕。
// remoteName 必須是 `git remote` 帶回的那個名字（區分大小寫）；配置裡完全沒有它的記錄時
// 按 Git 文檔的預設映射（refs/heads/*:refs/remotes/<遠端名>/*）判定，那是允許的形態。
[[nodiscard]] FetchScopeDecision DecideFetchScope(const FetchScopeFacts& facts,
                                                  std::wstring_view remoteName);

// 抓取確認文字的共同骨架：目標、目標来历、將要執行的命令、範圍正文、認證與失敗邊界。
// 兩個入口的差別只用兩段可選文字表達，其餘每一句都出自本函數與 decision：
//   leadingText —— 排在「抓取目標」之前（pull 用它擺出本地分支與上游那一對，fetch 按鈕傳空）；
//   stageText   —— 排在範圍正文之後、認證與失敗說明之前（pull 用它交代「這只是第一步，
//                  整合稍後再問」與策略配置；fetch 按鈕傳空）。
// sourceSentence 由呼叫方說明「這個目標是怎麼定下來的」（三個場合的来历各不相同）。
[[nodiscard]] std::wstring BuildFetchConfirmationText(const FetchScopeDecision& decision,
                                                      std::wstring_view remoteName,
                                                      std::wstring_view remoteUrlForDisplay,
                                                      std::wstring_view sourceSentence,
                                                      std::wstring_view repositoryDirectory,
                                                      std::wstring_view leadingText = {},
                                                      std::wstring_view stageText = {});

}  // namespace gc::git
