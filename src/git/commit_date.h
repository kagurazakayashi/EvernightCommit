#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gc::git {

// 提交時間（GIT_AUTHOR_DATE / GIT_COMMITTER_DATE）的可移植邏輯（本模組不碰任何 Win32 API）：
// 把「本機牆上時間 + 那個時刻實際生效的 UTC 偏移」變成 Git 認得的日期字串，以及給人看的說明。
//
// 為什麼交給界面的是「牆上時間＋偏移」兩個分量，而不是一個 epoch：
//   * 使用者在日期時間控件裡改的是本機墙上時間，控件也只认這個形态；
//   * 同一個墙上時間在夏令時前後對應兩個不同的瞬間，偏移必須按那一天實際的時區規則算，
//     這件事只有操作系統知道（見 platform/windows/local_time），算完的偏移要一起顯示出去；
//   * Git 收下的值裡必須帶明確的時區，否則它會按「讀這個環境變數的那個進程的當前時區」補預設值，
//     使用者挑的時刻就會被挪幾個小時。
//
// 本機 Git 2.53 與自建臨時倉庫實測（不是照抄文件）：
//   * `GIT_AUTHOR_DATE="1759219200 +0800"`（十位秒數＋偏移）被接受，寫進對象的就是這一对；
//   * 但「秒數不足 9 位」的同形态一律被拒：`0 +0800`、`10000000 +0800` → `fatal: invalid date format`
//     （退出碼 128）。加上 `@` 前綴就沒有這個位數門檻：`@0 +0800`、`@1 +0000` 都正常寫入。
//     因此本模組一律用 `@<epoch> <±hhmm>` 的内部形态。
//   * 負數一律拒收（`-1 +0800`、`-100000000 +0800` → invalid date format），
//     所以 1970-01-01 UTC 之前根本提交不了 —— 控件的選取範圍就按這個事實收緊。
//   * `4294967295 +0800`（2106-02-07）與更长的数字同样被拒；上界取到 2099 年足够，
//     也把「+1400 这类偏移把日期推到 2100」的餘量留了出來。
//   * 偏移寫法 `+0530`、`-1200`、`+1400` 都如實保留（`%ad` 顯示同一個偏移）。

// 一段牆上時間：年月日時分秒。不含任何時區信息，偏移由呼叫方另帶。
struct CivilTime {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

// 提交時間可選取年份的上下界。下界是 Git 自己的底（負 epoch 一律拒收），
// 上界留在 2099：再往後就贴近 Git 拒收的 32 位秒數邊界，沒有意義。
inline constexpr int kCommitYearLowerBound = 1970;
inline constexpr int kCommitYearUpperBound = 2099;

// 該年份是否為閏年（格里高利曆規則，1900 因此是平年）。
[[nodiscard]] bool IsLeapYear(int year) noexcept;

// 欄位取值看起來成立嗎：年份在可選範圍內、月日時分秒各自不越界、並且不超過當月天數。
// 只做形態檢查，不管時區、不管夏令時跳躍（那種時刻由操作系統給出實際歸屬）。
[[nodiscard]] bool CivilTimeLooksValid(const CivilTime& value) noexcept;

// 把牆上時間「當作 UTC」換算成秒數（proleptic 格里高利曆，1970-01-01T00:00:00Z 為 0）。
// 取值不成立或超出可表示範圍時返回 false 並清空 *outEpoch。
// 本函數與 UtcEpochToCivilTime() 互为逆運算，是偏移計算的依據：
// 偏移分鐘 = （把牆上時間當 UTC 得到的秒數 − 真正的 UTC 秒數）/ 60。
[[nodiscard]] bool CivilTimeToUtcEpoch(const CivilTime& value, long long* outEpoch) noexcept;

// 把 UTC 秒數換回牆上時間（按 UTC 解碼，不做任何時區換算）。
[[nodiscard]] bool UtcEpochToCivilTime(long long epochSeconds, CivilTime* out) noexcept;

// 那個 epoch 是否落在 Git 收得下的範圍內（0 到 2099 年底，留給偏移的餘量）。
[[nodiscard]] bool IsStorableCommitEpoch(long long epochSeconds) noexcept;

// 拼成交給 GIT_AUTHOR_DATE / GIT_COMMITTER_DATE 的值：`@<epoch> <±hhmm>`。
// 結果一定是純 ASCII、不含空格以外的分隔字元，因此可以安全放進環境塊。
// 任一輸入不合格時返回 false 並在 *refusal 寫入面向使用者的原因（可為空指針）。
[[nodiscard]] bool FormatGitInternalDate(long long epochSeconds, int offsetMinutes, std::string* out,
                                         std::wstring* refusal);

// 偏移的說法：`UTC+08:00` / `UTC-05:00` / `UTC+00:00`。偏移不合法（绝对值超過 14 小時或非整分鐘）
// 時返回空字串，界面據此顯示「偏移未知」而不是憑空編一個。
[[nodiscard]] std::wstring FormatOffsetText(int offsetMinutes) noexcept;

// 給人看的完整時間：`2026-09-30 16:00:00（UTC+08:00）`。取值不成立時返回空字串。
[[nodiscard]] std::wstring FormatCommitTimeText(const CivilTime& value, int offsetMinutes) noexcept;

}  // namespace gc::git
