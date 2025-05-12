#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/repository.h"       // GitQueryResult / RepoDetection / PathsEqualFolded
#include "git/undo_commit_plan.h"  // 复用 UndoQueryRead / ReadUndoQuery 这一套三态判读
#include "git/workspace_model.h"   // ChangeItem / SubmoduleState（porcelain v2 的 <sub> 三态）

namespace gc::git {

// 「从父仓库进入子模块、再回到父仓库更新指针」的可移植決策邏輯
// （本模組不碰任何 Win32 API、不起子進程、不讀檔案系統；路徑存在性與子進程一律由平台層帶回）：
//   1) 構造那幾條只讀查詢的參數（父索引裡的那條 gitlink、父提交裡記著的那一份、子模块自己的 HEAD）；
//   2) 把平台層帶回的結果判讀成「這個目錄到底是不是那個子模組、現在能不能進去」；
//   3) 返回父倉庫時，把三份位置合成一句話說清：差在哪一層、下一步該由誰做。
//
// 為什麼導航本身不寫任何东西（本任務的硬性邊界）：
//   * 進入/返回做的只有「問」與「換綁定的目錄」：父索引一個字節不動、不產生提交、不訪問遠端；
//   * 指針暫存（git add）、子模組內部檔案的暫存、初始化／更新（git submodule update --init，
//     那要聯網並改動工作區）都不在導航裡隱式觸發——界面只把「可以做哪一步、該由你自己點哪一個按鈕」
//     說清楚，做不做由使用者決定（既有約束：父倉庫只暫存 gitlink，絕不遞歸提交子模組內部的檔案）。
//
// 「不猜」的界線（逐條與實作對應）：
//   * 點中的條目必須真是子模組條目（kind==submodule，來自 porcelain v2 的 <sub> 記錄），
//     而且要與剛讀回來的模型逐行核對——列表行號與條目的對應關係只在顯示落地那一刻成立。
//   * 路徑是資料：一律走既有的「倉庫相對路徑」形態判定（拒絕空、絕對路徑、`..` 段、盤符/冒號、
//     反斜杠、控制字符與雙引號），拼出的絕對目錄只用於展示與識別；送給 Git 的仍是相對路徑本體。
//   * 子模組身份由 Git 自己回答：識別那個目錄，要求 kind 是子模組形態，而且
//     `--show-superproject-working-tree` 答出的父倉庫工作區就是當前界面綁定的這一個。
//     答不出父倉庫、或答的是另一個地方（移動過的倉庫、獨立倉庫、來歷不明的目錄）一律拒絕並點名，
//     不「看起來像就進去」。
//   * 「未初始化」與「這不是倉庫」要分得開：父索引裡那一條記錄確實是 160000 gitlink、
//     而那個目錄存在卻識別不出倉庫時，說的是「子模組還沒有初始化」（裡面沒有 .git）；
//     目錄根本不存在時說的是「路徑已不見」。兩種都是拒絕導航，但給的下一步不一樣，
//     而且都不在這一筆點擊裡做初始化（那要聯網、會改動工作區）。
//   * 位置判讀一律以完整對象 ID 為準，形態不合格（半截 ID、空、混著別的輸出）就当沒問出來，
//     不拿它算差異。`ls-files -s` 帶 `-z`：不帶時 Git 會把含空格與非 ASCII 的路徑按 C 引號轉寫，
//     那種回答不是原樣路徑，拿它比對會把一個合法子模組說成「路徑對不上」。
//   * 父索引裡那條記錄的 mode 不是 160000（普通檔案被換了、或路徑撞名）時明確拒絕：
//     界面此刻看到的「子模組」與索引裡的事實已經不是一件事。
//
// 三份位置的讀法（返回父倉庫時的那句提示就按這個矩陣說話）：
//   索引 = `git ls-files -s -z -- :(literal)<路徑>` 裡那條 160000 的對象 ID
//   提交 = `git rev-parse --verify --quiet HEAD:<路徑>`（父倉庫 HEAD 樹裡記著的那一份）
//   子模組 = 在子模組目錄裡 `git rev-parse --verify --quiet HEAD`
//   三者一致 ⇒ 父倉庫這一條顯示乾淨（但乾淨不等於子模組那份提交已推出去、已發佈）；
//   索引==子模組!=提交 ⇒ 指針已經暫存，等的是父倉庫創建提交；
//   索引!=子模組 ⇒ 指針還沒進索引，該由使用者自己選那一條點「加入暫存區」；
//   子模組 HEAD 解析不出 ⇒ 說「問不出子模組現在在哪一份提交」，不猜、也不說「乾淨」。

// ---- 只讀查詢的參數（全部顯式 -C 綁定倉庫根，不依賴進程全局目錄） ----

// 父索引裡那條 gitlink：`-C <父根> --no-optional-locks ls-files -s -z -- :(literal)<相對路徑>`。
// 相對路徑不合格（見上面那條界線）時返回空數組：調用方據此拒絕，本函數不「修一個能用的」。
[[nodiscard]] std::vector<std::wstring> BuildSubmoduleIndexPointerArguments(
    std::wstring_view repositoryRoot, std::wstring_view relativePath);

// 父倉庫 HEAD 樹裡為這個路徑記著的那一份提交：`rev-parse --verify --quiet HEAD:<相對路徑>`。
// HEAD 還不可解析（倉庫沒有提交）時 Git 以退出碼 1 + 空輸出回答，那是明確答案不是錯誤。
[[nodiscard]] std::vector<std::wstring> BuildSubmoduleHeadPointerArguments(
    std::wstring_view repositoryRoot, std::wstring_view relativePath);

// 子模組自己現在的 HEAD（工作目錄換成子模組目錄；形態與 pull/push 預檢同一條）。
[[nodiscard]] std::vector<std::wstring> BuildSubmoduleOwnHeadArguments(
    std::wstring_view submoduleDirectory);

// ---- 父索引記錄的判讀 ----

struct GitlinkIndexEntry {
  bool readOk = false;      // 這一条查詢問成了、輸出也符合 NUL 記錄約定
  std::wstring readFailure;  // readOk 為 false 時的具體說明
  bool recordFound = false;  // 回答裡有那一条與詢問路徑一致的記錄（空輸出 = 沒有這條記錄）
  bool isGitlink = false;   // 命中且 mode 是 160000
  std::wstring objectId;   // isGitlink 時的完整對象 ID
  std::wstring mode;        // 命中但不是 160000 時的原樣 mode（進拒絕文案點名用）
  std::wstring recordedPath;  // Git 答出的路徑原文（用來核對「問的」與「答的」是同一條）
};

// 判讀 `git ls-files -s -z` 的回答。記錄形如 `<mode><空格><oid><空格><stage>\t<路徑><NUL>`；
// 只採認路徑與詢問時完全一致的那一條，出現多條或一條也沒有都如實回答（不是「幹淨」）。
[[nodiscard]] GitlinkIndexEntry ParseGitlinkIndexEntry(const GitQueryResult& listing,
                                                       std::wstring_view expectedRelativePath);

// ---- 三份位置的合成 ----

enum class SubmodulePointerState {
  unknown = 0,        // 問不成（查詢失敗、輸出不合約定）：不能談差異
  consistentClean,    // 子模組 == 索引 == 父提交：父倉庫這一條顯示乾淨
  pointerStaged,      // 子模組 == 索引 != 父提交：指針已暫存，等父倉庫創建提交
  pointerUnstaged,    // 子模組 != 索引：指針沒進索引，該由使用者決定要不要暫存
  submoduleHeadUnknown,  // 子模組那边的 HEAD 問不出提交（尚無提交/引用壞了/問不成）
  notGitlinkInIndex,  // 父索引裡這個路徑不是 160000 記錄（已被換掉、移走或撞名）
};

[[nodiscard]] std::wstring_view SubmodulePointerStateLabel(SubmodulePointerState state) noexcept;

struct SubmodulePointerQueries {
  GitQueryResult indexListing;   // BuildSubmoduleIndexPointerArguments 的那一條
  GitQueryResult headPointer;    // BuildSubmoduleHeadPointerArguments
  GitQueryResult submoduleHead;  // BuildSubmoduleOwnHeadArguments
  std::wstring relativePath;     // 三條查詢問的是同一個路徑，判讀據此核對
};

struct SubmodulePointerFacts {
  SubmodulePointerState state = SubmodulePointerState::unknown;
  std::wstring detail;            // unknown/異常形態時面向界面的具體說明
  GitlinkIndexEntry index;        // 父索引記著哪一份
  std::wstring headObjectId;      // 父提交記著哪一份（問不出為空）
  std::wstring submoduleHeadId;   // 子模組現在在哪一份（問不出為空）
  bool headQueried = false;
  bool submoduleHeadQueried = false;
};

[[nodiscard]] SubmodulePointerFacts InterpretSubmodulePointer(const SubmodulePointerQueries& queries);

// 返回父仓库时摆给使用者的那段話：三份位置、下一步該由誰做、以及兩條硬性提醒
// （父倉庫顯示乾淨不證明子模組那次提交已發佈；導航本身不動父索引、不提交、不聯網）。
// 子模組內部還有沒有已跟蹤改動或未跟蹤檔案，看的是父倉庫列表裡那一條自己的狀態——
// 這裡不帶那份標記進來：指針判讀用的是三條剛問回來的只讀查詢，混進一次可能過期的
// status 快照只會讓兩處說法不一。
[[nodiscard]] std::wstring ComposeSubmodulePointerReport(const SubmodulePointerFacts& facts,
                                                          std::wstring_view submodulePath);

// ---- 進入子模組的裁決 ----

// 那個目錄在檔面上的形態（由平台層的只讀探測帶回，判讀層自己不猜）。
enum class SubmodulePathPresence {
  unknown = 0,      // 沒去探（或探不了）
  missing,          // 目錄不存在：父倉庫裡/git 目錄裡都不見了
  existsDirectory,  // 是一個存在的目錄
  existsNotDirectory,  // 同一個名字上是檔案或別的東西（不是目錄）
};

struct SubmoduleEntryFacts {
  // 界面點中的那一條（已與模型逐行核對過才填進來）。
  bool itemIsSubmodule = false;
  std::wstring relativePath;
  SubmoduleState flags;

  // 路徑形態與拼接結果（resolvedDirectory 由平台層用既有路徑工具算出，只供展示與識別）。
  bool pathShapeOk = false;
  std::wstring pathShapeProblem;
  std::wstring resolvedDirectory;
  SubmodulePathPresence presence = SubmodulePathPresence::unknown;

  // 當前界面綁定的父倉庫根（身份核對的基準）。
  std::wstring parentRoot;

  // 父索引裡那條記錄（同一次導航裡問回來的，判讀据此決定「未初始化」還是「不是 gitlink」）。
  GitlinkIndexEntry index;

  // 對那個目錄跑一次倉庫識別的結果（childProbed 為 false 表示压根沒問成）。
  bool childProbed = false;
  RepoDetection child;
};

enum class SubmoduleEntryDecision {
  enter = 0,      // 可以進入：路徑、身份、初始化狀態都由 Git 的回答確認過
  refuse,        // 明確拒絕（reason 把是哪一条不過、以及下一步該由使用者做什麼說清）
};

struct SubmoduleEntryPlan {
  SubmoduleEntryDecision decision = SubmoduleEntryDecision::refuse;
  std::wstring reason;  // 兩種裁決都要有把話說完的文案（界面原樣展示）
  std::wstring disclosure;  // 進入時要一起講清的事實：內部還有已跟蹤改動/未跟蹤檔案、指針歸父倉庫
  std::wstring targetDirectory;  // decision==enter 時有效（等於 facts.resolvedDirectory）
};

// 主入口：按上面的界線逐條核對，任何一條不過就 refuse 並給出具體原因。
// 這裡不產生任何命令：導航只是換綁定的目錄，add/commit/push/init 都不在這一條鏈路上。
[[nodiscard]] SubmoduleEntryPlan BuildSubmoduleEntryPlan(const SubmoduleEntryFacts& facts);

// ---- 返回父倉庫的准入 ----

// 當前界面綁定的根是不是「栈頂記下的那個子模組」：不是就不許返回——
// 把父倉庫的草稿交回一個不相干的倉庫，比不返回更糟。
[[nodiscard]] bool IsJourneyChildRoot(std::wstring_view recordedChildRoot,
                                     std::wstring_view currentRoot);

}  // namespace gc::git
