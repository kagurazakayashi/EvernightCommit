#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/repository.h"
#include "git/workspace_model.h"

namespace gc::git {

// 本模組只負責「怎麼問 Git 要工作區狀態、怎麼把回答拆成條目」，不碰任何 Win32 API：
// 子進程執行由平台層以 GitQueryRunner 注入，因此參數構造與解析都能用樁輸出完整測試。
// 進入本模組的文本已在平台邊界完成 UTF-8→UTF-16 解碼，這裡只處理寬字符。
//
// 查詢參數（--porcelain=v2 -z --untracked-files=all --ignore-submodules=none）的選定依據，
// 來自 Git 官方 porcelain v2 規範並在本機 Git 2.55 實測核對：
//   --porcelain=v2      機器可讀格式。索引側與工作區側分兩欄（XY）給出，同一檔案的兩側狀態互不覆蓋；
//                       重命名/複製記錄自帶來源路徑與相似度，未合併記錄自帶三個階段的模式，
//                       子模組以 <sub> 欄位回報內部狀態。
//   -z                  記錄以 NUL 結尾，且路徑原樣輸出、不做任何引號轉義
//                       （因此不依賴 core.quotepath，也不依賴本地代碼頁；含空格、&、^、% 與中文的路徑可原樣還原）。
//   --untracked-files=all
//                       未跟蹤目錄展開成一條條可單獨選擇的檔案，而非只列目錄。
//   --ignore-submodules=none
//                       覆蓋用戶的 submodule.*.ignore 與 diff.ignoreSubmodules 配置，子模組狀態一律如實回報。
// 不請求 --ignored：被 Git 忽略的條目不進列表，也不使用任何強制加入手段。
[[nodiscard]] std::vector<std::wstring> BuildWorkspaceStatusArguments(
    std::wstring_view repositoryDirectory);

struct WorkspaceStatusParseResult {
  WorkspaceModel model;
  // 非空表示輸出不符合 porcelain v2 約定。寧可整體報告讀取失敗，也不能把一條沒解析出來的
  // 變化悄悄丟掉——那會讓使用者在看不見的檔案上做出提交決定。
  std::wstring error;
  // 認識但刻意不進列表的記錄數（# 頭部記錄、! 被忽略條目）。
  size_t skippedRecords = 0;
};

// 解析 --porcelain=v2 -z 的 NUL 分隔輸出。
// 條目歸屬：X 欄非 '.' 的進入 staged，Y 欄非 '.' 的進入 unstaged；
// 未合併（u 記錄）作為一條衝突條目進入 unstaged（在工作區解決，解決手段即暫存）；
// 未跟蹤（? 記錄）進入 unstaged。
WorkspaceStatusParseResult ParseWorkspacePorcelainV2(std::wstring_view nulSeparatedOutput);

// 一次工作區讀取的完整結果，供應用狀態直接持有（界面只讀這裡，不直接訪問 Git）。
struct WorkspaceSnapshot {
  WorkspaceLoadStatus status = WorkspaceLoadStatus::unloaded;
  RepoError error = RepoError::none;  // status 為 failed 時的歸類原因
  WorkspaceModel model;
  std::wstring message;   // 面向界面的完整說明（成功摘要或失敗原因）
  size_t skippedRecords = 0;
};

// 讀取成功後寫進界面的摘要：兩側條目數與衝突提示。
[[nodiscard]] std::wstring BuildWorkspaceSummary(const WorkspaceModel& model);

// 讀取失敗說明：歸類標籤 + Git 給出的細節 + 一句「只讀、未改動倉庫」的安心話。
[[nodiscard]] std::wstring BuildWorkspaceFailureMessage(RepoError error, std::wstring_view detail);

}  // namespace gc::git
