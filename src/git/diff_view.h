#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/workspace_model.h"

namespace gc::git {

// 雙擊文件列表「查看差異／查看內容」的可移植決策邏輯（本模組不碰任何 Win32 API）：
//   1) 按「哪一側的列表 + 條目類別」決定比較基準，构造要交給命令窗口執行器的 Git 參數陣列；
//   2) 對未跟蹤檔案做預檢裁決（是否存在、可否讀、是否二進位、是否過大），
//      把「不能直接把位元組當文字輸出」的決定留在開窗口之前；
//   3) 校驗倉庫相對路徑不會逃出工作區（`..`、絕對路徑、盤符、NTFS 數據流）。
//
// 下面這些命令形態都在本機 Git 2.55 實測核對過，不是按記憶推測：
//   * 未暫存側用 `git diff -- <pathspec>`（工作區 vs 索引），已暫存側用 `git diff --cached -- <pathspec>`
//     （索引 vs HEAD）。同一檔案兩側各自成一份差異，不會混成一次「完整 diff」。
//   * `git diff` 本身**不因為「有差異」而非 0 退出**（要 `--exit-code` 才會），
//     但 `git diff --no-index` 找到差異時固定返回 1，讀不到檔案時也返回 1。
//     因此「查看」類操作的退出碼 0/1 都屬正常，界面必須按這個語義解釋。
//   * `--no-index` 前兩条路徑是檔案系統路徑，不是 pathspec；缺第二條是用法錯誤（129），
//     不帶 `--` 終止符時以減號開頭的檔名會被當成選項（實測報 `switch 'l' expects ...`）。
//   * `--no-ext-diff --no-textconv` 確保不调用使用者配置的外部 diff / textconv 工具，
//     輸出一律是 Git 原生生成的差異。
//   * `--ignore-submodules=none --submodule=short`：子模組只顯示父倉庫記錄的提交指標
//     （`Subproject commit A` → `B`，骯髒時帶 `-dirty` 後綴），不遞迴進內部的檔案差異。
//   * 初始提交之前（HEAD 尚不可解析）`git diff --cached` 照常工作：Git 以空樹為基準。
//   * 未合併（衝突）條目在未暫存側會輸出 `diff --cc` 組合差異（含衝突標記），
//     在已暫存側輸出 `* Unmerged path ...`，兩側都不是空輸出。
//   * 暫存的重命名只給新路徑時，Git 把它顯示成「新增檔案」；同時給舊、新兩條路徑才會顯示
//     `rename from/to`，所以重命名/複製條目的 pathspec 必須包含兩個路徑。
//   * pathspec 預設按 glob 解釋，`a[b]c.txt` 這類 Windows 合法檔名會連帶命中 `abc.txt`；
//     `:(literal)` 前綴關閉通配。`--` 之前不允許任何東西被當成選項。

// 差異的比較基準來自哪一側列表：與界面上的兩塊列表一一對應。
enum class ChangeSide {
  unstaged,  // 工作區相對索引
  staged,    // 索引相對 HEAD
};

// 一次查看的類別：決定參數形態、退出碼解釋與界面文案。
enum class DiffViewKind {
  trackedDiff = 0,   // 已跟蹤條目：git diff / git diff --cached
  untrackedContent,  // 未跟蹤檔案：git diff --no-index 對照 /dev/null，等價於直接給出內容
  untrackedSummary,  // 未跟蹤但超過內容上限：同一命令加 --stat，只給規模摘要
  blocked,           // 預檢不通過：不彈命令窗口，由界面把原因講清楚
};

// 未跟蹤檔案的預檢事實，由平台層採集後交來裁決（本模組不讀檔案系統）。
struct WorktreeFileFacts {
  bool probed = false;      // 是否取得到檔案資訊（false 時一律 blocked）
  bool exists = false;      // 路徑現在是否還在
  bool isDirectory = false; // 目錄沒有「內容」可看
  bool readable = false;    // 是否可打開讀取（權限、鎖定等）
  bool containsNullByte = false;  // 前 kBinaryProbeBytes 位元組內出現 NUL：按二進位對待
  unsigned long long sizeBytes = 0;
  std::wstring failureReason;  // 採集失敗的具體原因（含 Windows 錯誤文本）
};

// 構造完成的查看方案。arguments 一項一個元素，直接交給 CommandWindowOperation，
// 界面不拼任何 shell 字串；displayName 進窗口標題與狀態欄，operationId 必須是安全 ASCII。
struct DiffViewPlan {
  DiffViewKind kind = DiffViewKind::blocked;
  std::vector<std::wstring> arguments;
  std::wstring displayName;
  std::wstring operationId;
  // 需要讓使用者知道的範圍限制（子模組只顯示指標、二進位不輸出內容、大文件只給摘要、
  // 或 blocked 時的原因）。blocked 時的理由說明放在 blockedReason。
  std::wstring notice;
  std::wstring blockedReason;
};

// 內容预览上限：實測 2.4 MB / 20 萬行的 --no-index 輸出要 3.8 秒且把控制台淹掉，
// 超過上限改給 --stat 摘要（同一命令，實測 40 毫秒），更大則連摘要都不跑。
inline constexpr unsigned long long kUntrackedContentPreviewLimitBytes = 256ULL * 1024ULL;
inline constexpr unsigned long long kUntrackedSummaryLimitBytes = 64ULL * 1024ULL * 1024ULL;
// 二進位判定的採樣長度：與 Git 自身「內容含 NUL 即視為二進位」的規則同源。
inline constexpr size_t kBinaryProbeBytes = 8192;

[[nodiscard]] std::wstring_view DiffViewKindLabel(DiffViewKind kind) noexcept;

// 字面 pathspec：加 `:(literal)` 前綴，關閉 glob，檔名裡的 [ ] 不再被當字符類。
// 已經帶 magic 前綴或為空的輸入原樣/空串返回，不重複加前綴。
[[nodiscard]] std::wstring MakeLiteralPathspec(std::wstring_view relativePath);

// 未跟踪檔案在 `--no-index` 形態下的第二條路徑佔位：實測 Git 對 /dev/null 與 NUL 都接受，
// 且輸出統一顯示成 `--- /dev/null`，因此固定用它，界面文案也按這個寫。
inline constexpr const wchar_t* kEmptySidePath = L"/dev/null";

// 倉庫相對路徑是否可安全地當成工作區內的路徑使用。拒絕：空、含反斜杠、以斜杠開頭、
// 含盤符或冒號（NTFS 數據流）、任何 `..` 段、含控制字元或雙引號。
[[nodiscard]] bool IsWorktreeRelativePath(std::wstring_view relativePath, std::wstring* reason);

// 工作區根目錄 + 倉庫相對路徑 → 絕對 Windows 路徑（僅供預檢與說明文字使用；
// 送給 Git 的參數仍是倉庫相對路徑，因為子進程的工作目錄已綁定為根目錄）。
[[nodiscard]] std::wstring JoinWorktreeFilePath(std::wstring_view repositoryRoot,
                                                std::wstring_view relativePath);

// 主入口：按側別與條目類別構造方案。facts 只在 kind 為 untracked 時被讀取，
// 其餘條目（含刪除的檔案）不需要檔案存在 —— 刪除項的差異由 Git 自己給出，
// 界面絕不去打開已經不存在的檔案。
[[nodiscard]] DiffViewPlan BuildDiffViewPlan(ChangeSide side, const ChangeItem& item,
                                            const WorktreeFileFacts& facts);

// 人讀的參數摘要（把方案里的 pathspec 換成「檔案」字樣）：用於界面說明，不用於執行。
[[nodiscard]] std::wstring DescribeDiffViewCommand(const DiffViewPlan& plan);

// 查看類操作的退出碼解釋：0/1 都屬正常完成，語義不同，必須講清楚，
// 否則使用者會把「有差異」當成「操作失敗」。
[[nodiscard]] std::wstring DescribeDiffViewExitCode(DiffViewKind kind, long exitCode) noexcept;

}  // namespace gc::git
