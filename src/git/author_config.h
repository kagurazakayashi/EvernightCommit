#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_identity.h"
#include "git/repository.h"

namespace gc::git {

// 「有效 Git 身份」的查詢與判讀（純邏輯，不碰 Win32、不起子進程）。
//
// 為什麼把「讀哪一個設定檔」這件事完全交給 Git：Git 自己的優先序是
//   工作樹設定（需 extensions.worktreeConfig）→ 倉庫本體 .git/config → 使用者設定
//   （$XDG_CONFIG_HOME/git/config 與 ~/.gitconfig，且受 GIT_CONFIG_GLOBAL 影響）→ 系統設定
// 加上 include / includeIf 的遞迴包含。自己讀 ~/.gitconfig 只會猜錯順序，
// 也看不見倉庫本體與 includeIf。因此這裡只發一條 `git config --get`，
// 由 Git 回答「這個倉庫現在實際用哪個身份」。
//
// 命令形態在本機 Git 2.56.0.windows.1 實測：
//   * `git config --null --get <key>` 成功時輸出「<值>\0」，值的頭尾空白原樣保留
//     （不像 `--get` 那樣以換行分隔，值裡的換行不可能出現，因為設定檔一行一值）；
//   * 同一 key 在多層都有值時只回最優先的一個；同層用 `git config --add` 寫成多值時回最後一個；
//   * key 完全不存在 → 退出碼 1、無輸出（這不是錯誤，是「沒設定」）；
//   * 設定檔語法壞了 → 退出碼 128，stderr 給出 `fatal: bad config line N in file ⋯`；
//   * 在非倉庫目錄裡照樣成功回傳使用者層的身份（退出碼 0），所以「查不查得到」不能當
//     「是不是倉庫」的判據，查詢只在已識別出的工作區根上發起。
//   * `git config --null --get key1 key2`（一次問兩個 key）在 2.56 不成立：第二個參數被當成
//     值樣板，退出碼 1 且無輸出，因此分兩次查詢。
enum class ConfigValueState {
  absent,   // 退出碼 1：這一層也沒有人設過這個 key。
  present,  // 退出碼 0：值有效（可能是空字串，仍算「設過但為空」）。
  failed,   // 啟動失敗、逾時、或其他非 0 退出（設定檔損壞、倉庫不安全等）。
};

// 一次 `git config --null --get <key>` 的判讀結果。
struct ConfigValueRead {
  ConfigValueState state = ConfigValueState::failed;
  std::wstring value;    // present 時的原值（含頭尾空白，由呼叫方決定是否修剪）
  RepoError error = RepoError::none;  // failed 時的歸類
  std::wstring detail;   // failed 時 Git 給出的說明（stderr 首行）
};

// 構造一次身份查詢的参数陣列：顯式 -C 並把工作目錄綁到倉庫根，不使用全域 cd。
// 加 --no-optional-locks：讀設定不該順手寫 index.lock 之類的鎖檔案。
[[nodiscard]] std::vector<std::wstring> BuildIdentityConfigArguments(std::wstring_view repositoryDirectory,
                                                                    std::wstring_view key);

// 判讀一次查詢：把退出碼與輸出分成「沒設過」「設過（值）」「查不到」三種。
// 只有退出碼 0 或 1 才算 Git 正常回答；其餘一律歸為失敗並給出原因。
[[nodiscard]] ConfigValueRead ParseIdentityConfigQuery(const GitQueryResult& result, std::wstring_view key);

// 「提交者身份」的可得性。提交者是 Git 依有效設定（含 GIT_COMMITTER_* 環境變數）自己決定的，
// 表單不提供輸入；界面只能報告它現在能不能用。
enum class CommitterIdentityState {
  unknown,  // 還沒讀到，或讀取失敗：不能斷定。
  available,
  missing,  // user.name / user.email 至少一項缺失或為空。
};

// 一次身份讀取的完整結果。
struct AuthorIdentityConfig {
  ConfigValueRead name;
  ConfigValueRead email;
  [[nodiscard]] CommitterIdentityState CommitterState() const noexcept;
  // 姓名與郵箱都問到且都非空時給出「姓名 <邮箱>」，否則返回空字串。
  [[nodiscard]] std::wstring Identity() const;
  [[nodiscard]] bool ReadFailed() const noexcept {
    return name.state == ConfigValueState::failed || email.state == ConfigValueState::failed;
  }
};

// 用兩次查詢的結果合成 AuthorIdentityConfig（呼叫方負責發起查詢，這裡只判讀）。
[[nodiscard]] AuthorIdentityConfig CombineIdentityConfig(const ConfigValueRead& name,
                                                        const ConfigValueRead& email);

// 面向界面的說明：區分「沒讀到（失敗原因）」與「Git 裡沒有完整身份（要使用者自己填）」。
// 空字串表示讀到了完整身份，界面不需要多說什麼。
[[nodiscard]] std::wstring BuildIdentityConfigNotice(const AuthorIdentityConfig& config);

// 兩個 key 的名字集中在此，避免界面與平台各自拼字串。
inline constexpr std::wstring_view kUserConfigKeys[] = {L"user.name", L"user.email"};

}  // namespace gc::git
