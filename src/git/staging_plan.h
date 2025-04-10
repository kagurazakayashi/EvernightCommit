#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/workspace_model.h"

namespace gc::git {

// 「加入暫存區」的可移植決策邏輯（本模組不碰任何 Win32 API，也不碰檔案系統）：
//   1) 只從「使用者選中的未暫存條目」導出 pathspec 清單，絕不用 `.`、`-A`、`-u` 之類
//      全倉庫形態，因此「沒有選擇」永遠不會被解釋成「暫存全部」；
//   2) 校驗每條倉庫相對路徑，任一不合約定就整份拒絕，不交出一半的清單；
//   3) 按 Git 能力選擇傳遞方式：路徑清單臨時檔（NUL 分隔）或字面 pathspec 參數；
//   4) 把必須讓使用者知道的語義差別湊成一句確認文案（未合併條目、子模組指標）。
//
// 下面這些命令形態都在本機 Git 2.56 與自建臨時倉庫實測核對過，不是按記憶推測、也不照抄文档：
//   * `git add --pathspec-from-file=<file> --pathspec-file-nul`：清單以 NUL 分隔。
//     **實測要點**：`-z` 只關閉「引號／C 轉義」這一層，pathspec 的 glob 魔法仍然生效 ——
//     清單裡直接寫 `brk[x].txt` 會連帶命中同目錄的 `brkx.txt`（`[x]` 被當成字符類）。
//     所以每條路徑都要帶 `:(literal)` 前綴，實測加前綴之後只剩自己那一条。
//     `#` 開頭不為註釋、`!` 開頭不為取反、`%` 原樣保留、前導 `-` 不會被當成選項（均在清單裡實測）。
//     該接口自 Git 2.25 提供，舊版本一律退回參數形態。
//   * 清單路徑相對子進程工作目錄（即工作區根），與界面用的倉庫相對路徑同源；
//     清單檔本身可放在倉庫之外（實測用絕對路徑可用）。
//   * `git add` 對「工作區已刪除但索引還在」的路徑會如實記下刪除（無需 `-A`），
//     對「索引沒有、工作區也沒有」的路徑報 `fatal: pathspec ... did not match any files`
//     且退出碼 128 —— 實測此時整條命令一個字元都沒進索引，失敗是全有或全無的。
//   * 同一檔案已暫存一版後再次 `git add`，索引內容更新為工作區當前內容（部分暫存可重複）。
//   * 未合併（衝突）條目被 `git add` 後即記為已解決：索引寫入的是工作區當前位元組，
//     實測連 `<<<<<<<` 標記都會一起進索引。程序不判斷衝突是否真的解決，只把這句話交給使用者。
//   * 子模組在父倉庫只有一條 gitlink：指標未移動時 `git add sub` 是無操作（退出碼 0，
//     內部髒檔案一個都不進父索引）；指標已移動時只記錄新的提交 ID。父倉庫永不遞迴暫存內部檔案。
//   * `git mv` 之後在工作區改檔名，porcelain v2 給的是「.D 舊路徑」與「?? 新路徑」兩條
//     未暫存記錄，不是一條重命名；同時選中兩條即等價於暫存這次重命名（實測索引記成 R100）。

// 傳遞選中條目的方式。界面據此決定要不要先寫清單臨時檔。
enum class StagingDelivery {
  blocked = 0,      // 不執行任何命令，由界面把原因講清楚
  pathspecFile,     // 清單寫進 NUL 分隔的臨時檔，命令裡只出現檔名
  inlinePathspec,   // 不支援清單接口的舊 Git：`:(literal)` 路徑直接進參數陣列
};

// 構造方案的輸入條件。
struct StagingPlanOptions {
  // 當前 Git 程序是否支援 `--pathspec-from-file` + `--pathspec-file-nul`。
  // 由界面用 `git --version` 的回報值判定（見 SupportsPathspecFileDelivery），不猜。
  bool pathspecFileSupported = true;
  // 未暫存那一側的條目總數，只用於說明「另有幾項未選中，本次不會動它們」。
  size_t totalUnstagedItems = 0;
};

struct StagingPlan {
  StagingDelivery delivery = StagingDelivery::blocked;
  std::wstring blockedReason;                 // delivery 為 blocked 時的完整說明
  // 去重後的 pathspec 元素：每條都已帶 `:(literal)` 字面前綴，
  // 既是清單檔案的內容，也是參數形態下要進命令列的那一项。
  std::vector<std::wstring> pathspecEntries;
  // inlinePathspec 時已是完整參數；pathspecFile 時尚未帶清單檔路徑（由界面寫好檔案後補上）。
  std::vector<std::wstring> arguments;
  std::wstring operationId;   // 執行器操作 ID（純 ASCII）
  std::wstring displayName;   // 命令窗口標題與狀態欄用的操作名
  std::wstring notice;        // 隨操作一直顯示的範圍說明
  std::wstring confirmationText;  // 非空 → 界面必須先讓使用者確認再執行
  std::wstring selectionSummary;  // 本次涉及的條目摘要（狀態欄用，已截斷）
  size_t selectedItems = 0;       // 選中的未暫存條目數
  size_t pathspecCount = 0;       // pathspecEntries 的條數（重命名會多一條）
};

// Git 版本是否支援 NUL 分隔的路徑清單文件接口（2.25 起）。
// 解析不出版本號時一律回 false：退回參數形態在舊新版本上都合法，
// 絕不因為「說不準」而改成 `git add .` 這種更大範圍的命令。
[[nodiscard]] bool SupportsPathspecFileDelivery(std::wstring_view versionText);

// 把清單檔路徑拼成 `git add` 的兩项尾參（`--pathspec-from-file=<path>` 與 `--pathspec-file-nul`）。
// 只做字符串拼接與安全性複核：路徑為空、含雙引號或控制字元時回 false，界面據此不彈窗口。
[[nodiscard]] bool AppendPathspecFileOptions(std::vector<std::wstring>* arguments,
                                             std::wstring_view pathspecFilePath);

// 主入口：由選中的未暫存條目構造一次 `git add` 方案。
// selected 必須是點擊瞬間從模型拷貝的快照（按列表行序），函式本身不讀界面狀態。
[[nodiscard]] StagingPlan BuildStagingAddPlan(const std::vector<ChangeItem>& selected,
                                              const StagingPlanOptions& options);

}  // namespace gc::git
