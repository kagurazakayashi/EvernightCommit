#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/repository.h"
#include "git/undo_commit_plan.h"  // 复用同一套 GitQueryResult 判读（ReadUndoQuery）

namespace gc::git {

// 「fetch」的可移植決策邏輯（本模組不碰任何 Win32 API、不起子進程、不讀檔案系統）：
//   1) 構造點擊瞬間要發起的幾條只讀查詢的參數（當前分支／分支配置的遠端／遠端清單）；
//   2) 把平台層帶回的 GitQueryResult 判讀成一份乾淨的事實（FetchTargetFacts）；
//   3) 把事實核對成「目標已定／要用户選／無從執行」，並湊出送進命令窗口的參數與確認文字。
//
// fetch 的邊界就是本步驟的範圍承諾：
//   * 只更新遠端跟蹤引用（refs/remotes/...）：HEAD、本地分支、索引與工作區一個字節都不動，
//     也不順帶 merge（那是 pull 的事，不屬於這一步）；
//   * 目標優先取當前分支明確配置的遠端（branch.<分支名>.remote）。沒有這種配置時
//     **不猜 origin**：列出 `git remote` 的既有遠端由用户選，一個也沒有就老實說沒有，
//     本程序絕不替你創建遠端、也不刪改任何配置；
//   * 不隱式 prune（參數里根本沒有 --prune）、不 --all、不遞歸子模組——
//     遞歸選項以 `--recurse-submodules=no` **明確寫死**，用戶配置的 submodule.recurse
//     不會把這一步的范围悄悄擴大（實測該選項可覆蓋配置項）。
//
// 各查詢全部帶 `--no-optional-locks`；symbolic-ref 帶 `--quiet`（不在分支上以退出碼 1 +
// 空輸出作答），config --get 未設時同樣退出碼 1 + 空輸出——那種「沒有」是明確答案，
// 不是錯誤；`git remote` 的輸出按「一行一個名字」解析，遠端名不會含換行。

// 一個已配置遠端的清單條目：名字與 fetch URL（僅供展示；命令只傳名字，
// URL 由 Git 自己按配置解析，本程序不把 URL 拼回命令行）。
struct FetchRemoteEntry {
  std::wstring name;
  std::wstring fetchUrl;  // `git remote -v` 的 (fetch) 行；同名多個 fetch URL 時取第一条
};

// ---- 只讀查詢的參數（全部顯式 -C 綁定倉庫根，不依賴進程全局目錄） ----

[[nodiscard]] std::vector<std::wstring> BuildFetchSymbolicRefArguments(std::wstring_view repositoryDirectory);
// branchName 為空時返回空數組：不在分支上就根本沒有「分支配置的遠端」可問。
[[nodiscard]] std::vector<std::wstring> BuildFetchBranchRemoteArguments(
    std::wstring_view repositoryDirectory, std::wstring_view branchName);
[[nodiscard]] std::vector<std::wstring> BuildFetchRemotesArguments(std::wstring_view repositoryDirectory);

// ---- 查詢結果的判讀 ----

// 平台層按順序執行只讀查詢後原樣交給判讀函數；branchRemoteRan 如實標記
// 「分支配置的遠端」那條到底發沒發（不在分支上時根本不發），判讀函數自己不猜。
struct FetchTargetQueries {
  GitQueryResult symbolicRef;
  GitQueryResult branchRemote;
  bool branchRemoteRan = false;
  GitQueryResult remotes;
};

struct FetchTargetFacts {
  bool queryOk = false;
  std::wstring queryFailure;  // queryOk 為 false 時面向界面的完整說明

  bool onBranch = false;
  std::wstring branchName;    // refs/heads/ 之後的部分
  std::wstring configuredRemote;  // branch.<分支名>.remote 的值；未配置/不在分支上為空

  // `git remote` 的既有遠端（按 Git 給出的順序）。remoteListOk 區分
  // 「查到了，確實一個也沒有」與「這條查詢本身失敗」。
  bool remoteListOk = false;
  std::vector<FetchRemoteEntry> remotes;
  std::wstring remoteListDetail;  // 解析不合約定時的說明（fetch URL 缺行等）
};

// 把一組原始查詢判讀成事實。純函數：所有輸入都是平台層帶回的结果，可用樁輸出完整測試。
// remotes 那條的原始輸出按「git remote -v」解析（name TAB url (fetch)/(push) 行），
// 只有 (fetch) 行提供 URL；純名字行（`git remote`）同樣接受，URL 留空。
[[nodiscard]] FetchTargetFacts InterpretFetchTarget(const FetchTargetQueries& queries);

// ---- fetch 方案 ----

enum class FetchPlanState {
  blocked = 0,   // 無從執行（查詢失敗 / 根本沒有遠端）：不產生命令
  chooseRemote,  // 目標定不下來：列出既有遠端請用户明確選擇
  ready,         // 目標已由分支配置確定：給確認框，點頭後進命令窗口
};

struct FetchPlan {
  FetchPlanState state = FetchPlanState::blocked;
  std::wstring explanation;  // 三種狀態都要有把話說完的文案（界面原樣展示）

  std::vector<FetchRemoteEntry> candidates;  // chooseRemote 時給選擇界面
  std::wstring remoteName;                   // ready 時有效
  std::wstring remoteUrl;                    // ready 時展示用（可能為空：遠端沒有 fetch URL）

  std::vector<std::wstring> arguments;  // ready: {fetch, --recurse-submodules=no, <remote>}
  std::wstring operationId;             // 執行器操作 ID（純 ASCII：fetch）
  std::wstring displayName;             // L"fetch"
  std::wstring commandLabel;            // 展示用命令，如 L"git fetch --recurse-submodules=no origin"
  std::wstring confirmationText;        // ready 時確認框正文
  std::wstring notice;                  // 隨操作一直顯示的範圍說明
};

// 主入口：核對事實並產出方案。目標只認兩樣東西——分支明確配置的遠端、
// `git remote` 列出的既有遠端；此外一概不猜。
// repositoryDirectory 只用於確認文字里展示工作目錄，不進任何命令參數。
[[nodiscard]] FetchPlan BuildFetchPlan(const FetchTargetFacts& facts,
                                       std::wstring_view repositoryDirectory = {});

// 用户在選擇界面点了某個既有遠端：按同一套文案湊出 ready 方案。
// 名字不在 facts.remotes 裡（列表讀回來後倉庫又被外部改過等）一律 blocked，不將就執行。
[[nodiscard]] FetchPlan ChooseFetchRemote(const FetchTargetFacts& facts, std::wstring_view remoteName,
                                          std::wstring_view repositoryDirectory = {});

}  // namespace gc::git
