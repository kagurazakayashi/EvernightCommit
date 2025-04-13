#include "git/commit_date.h"

#include <algorithm>
#include <array>

namespace gc::git {
namespace {

// 格里高利曆日序（以 1970-01-01 為 0）與逆運算：proleptic 算法，公元 1582 年以前的改歷
// 不在考慮範圍內（本模組的年份界已遠在其後）。用整數運算，不依賴任何平台曆庫，
// 因此「把牆上時間當 UTC 的秒數」在每一台機器上都得到同一個數值。
constexpr long long DaysFromCivil(int year, int month, int day) noexcept {
  const int shiftedYear = month <= 2 ? year - 1 : year;
  const int era = (shiftedYear >= 0 ? shiftedYear : shiftedYear - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(shiftedYear - era * 400);  // [0, 399]
  const unsigned yearOfMonth = static_cast<unsigned>(month + (month > 2 ? -3 : 9));  // [0, 11]
  const unsigned dayOfYear = (153u * yearOfMonth + 2u) / 5u + static_cast<unsigned>(day) - 1u;  // [0, 365]
  const unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
  return static_cast<long long>(era) * 146097ll + static_cast<long long>(dayOfEra) - 719468ll;
}

constexpr void CivilFromDays(long long days, int* year, int* month, int* day) noexcept {
  const long long shifted = days + 719468ll;
  const long long era = (shifted >= 0 ? shifted : shifted - 146096ll) / 146097ll;
  const unsigned dayOfEra = static_cast<unsigned>(shifted - era * 146097ll);  // [0, 146096]
  const unsigned yearOfEra =
      (dayOfEra - dayOfEra / 1460u + dayOfEra / 36524u - dayOfEra / 146096u) / 365u;  // [0, 399]
  const int shiftedYear = static_cast<int>(yearOfEra + era * 400ll);
  const unsigned dayOfYear = dayOfEra - (365u * yearOfEra + yearOfEra / 4u - yearOfEra / 100u);  // [0, 365]
  const unsigned marchMonth = (5u * dayOfYear + 2u) / 153u;  // [0, 11]
  const unsigned dayOfMonth = dayOfYear - (153u * marchMonth + 2u) / 5u + 1u;  // [1, 31]
  const unsigned monthOfYear = (marchMonth < 10u) ? marchMonth + 3u : marchMonth - 9u;  // [1, 12]
  *year = (monthOfYear <= 2u) ? shiftedYear + 1 : shiftedYear;
  *month = static_cast<int>(monthOfYear);
  *day = static_cast<int>(dayOfMonth);
}

// 平年各月天数（2 月由 IsLeapYear 单独加一天）。
constexpr std::array<int, 12> kCommonMonthDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

constexpr int DaysInMonth(int year, int month) noexcept {
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return kCommonMonthDays[static_cast<size_t>(month - 1)];
}

// 偏移的合法界：真實時區從 UTC-12:00 到 UTC+14:00，分鐘部分只可能是 00/15/30/45。
// Git 的內部形态接受任何 ±HHMM，但「+14:30」這種不存在的時區不該被界面交出去。
constexpr bool OffsetLooksValid(int offsetMinutes) noexcept {
  if (offsetMinutes < -720 || offsetMinutes > 840) {
    return false;
  }
  const int absolute = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  return (absolute % 15) == 0;
}

// 兩位補零。
std::wstring Pad2(int value) {
  const int absolute = value < 0 ? -value : value;
  std::wstring text = std::to_wstring(absolute);
  if (text.size() < 2) {
    text.insert(text.begin(), L'0');
  }
  return text;
}

}  // namespace

bool IsLeapYear(int year) noexcept {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool CivilTimeLooksValid(const CivilTime& value) noexcept {
  if (value.year < kCommitYearLowerBound || value.year > kCommitYearUpperBound) {
    return false;
  }
  if (value.month < 1 || value.month > 12) {
    return false;
  }
  if (value.day < 1 || value.day > DaysInMonth(value.year, value.month)) {
    return false;
  }
  if (value.hour < 0 || value.hour > 23) {
    return false;
  }
  if (value.minute < 0 || value.minute > 59) {
    return false;
  }
  // 秒只到 59：控件給不出 60，閏秒也不屬於本程式要管的形態。
  return value.second >= 0 && value.second <= 59;
}

bool CivilTimeToUtcEpoch(const CivilTime& value, long long* outEpoch) noexcept {
  if (outEpoch == nullptr) {
    return false;
  }
  *outEpoch = 0;
  if (!CivilTimeLooksValid(value)) {
    return false;
  }
  const long long days = DaysFromCivil(value.year, value.month, value.day);
  *outEpoch = days * 86400ll + static_cast<long long>(value.hour) * 3600ll +
              static_cast<long long>(value.minute) * 60ll + static_cast<long long>(value.second);
  return true;
}

bool UtcEpochToCivilTime(long long epochSeconds, CivilTime* out) noexcept {
  if (out == nullptr) {
    return false;
  }
  *out = CivilTime{};
  if (!IsStorableCommitEpoch(epochSeconds)) {
    return false;
  }
  const long long days = epochSeconds / 86400ll;  // epoch 已保證非負，這裡不會出現向零取整的偏差。
  const long long secondOfDay = epochSeconds % 86400ll;
  CivilTime value{};
  CivilFromDays(days, &value.year, &value.month, &value.day);
  value.hour = static_cast<int>(secondOfDay / 3600ll);
  value.minute = static_cast<int>(secondOfDay % 3600ll / 60ll);
  value.second = static_cast<int>(secondOfDay % 60ll);
  // 解碼結果必須落回可選取範圍：超出（例如 2099-12-31 最後一秒加上偏移推進到 2100）時拒絕，
  // 絕不把一個界面根本顯示不出來的時間交給使用者確認。
  if (!CivilTimeLooksValid(value)) {
    *out = CivilTime{};
    return false;
  }
  *out = value;
  return true;
}

bool IsStorableCommitEpoch(long long epochSeconds) noexcept {
  // 下界：Git 對負 epoch 一律報 invalid date format（實測 -1 與 -100000000 都是 128）。
  if (epochSeconds < 0) {
    return false;
  }
  // 上界：2099-12-31T23:59:59Z（= 4102444799 秒）。再往後就進入 Git 拒收的 32 位秒數地帶，
  // 而且界面的年份界也到不了那裡。界值由同一套曆法運算算出來，不寫死字面量，
  // 免得兩處各改各的。
  const CivilTime upperBound{2099, 12, 31, 23, 59, 59};
  long long upper = 0;
  static_cast<void>(CivilTimeToUtcEpoch(upperBound, &upper));
  return epochSeconds <= upper;
}

bool FormatGitInternalDate(long long epochSeconds, int offsetMinutes, std::string* out,
                           std::wstring* refusal) {
  const auto refuse = [&](std::wstring_view reason) {
    if (refusal != nullptr) {
      *refusal = std::wstring(reason);
    }
    return false;
  };
  if (out == nullptr) {
    return refuse(L"内部错误：没有存放日期值的容器。");
  }
  out->clear();
  if (!IsStorableCommitEpoch(epochSeconds)) {
    return refuse(L"这个时间落在 Git 能记录的区间之外（Git 实测拒绝 1970-01-01 之前与 2106 年前后的"
                  L"秒数）。请把日期改回到 1970 到 2099 年之间。");
  }
  if (!OffsetLooksValid(offsetMinutes)) {
    return refuse(L"这个时刻的 UTC 偏移无法表示（操作系统给出的偏移超出了现实时区范围）。");
  }
  const int absolute = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  const char sign = offsetMinutes < 0 ? '-' : '+';
  const int hours = absolute / 60;
  const int minutes = absolute % 60;
  const auto pad2 = [](int value) {
    std::string text = std::to_string(value);
    if (text.size() < 2) {
      text.insert(text.begin(), '0');
    }
    return text;
  };
  // `@` 前綴強制走內部解析形態：沒有它時，Git 只認「至少 9 位數」的裸秒數，
  // 1970-01-01 到 1973-03-03 之間的時間會被誤判成別的日期形態而整條命令失敗。
  *out = "@" + std::to_string(epochSeconds) + " ";
  out->push_back(sign);
  *out += pad2(hours) + pad2(minutes);
  return true;
}

std::wstring FormatOffsetText(int offsetMinutes) noexcept {
  if (!OffsetLooksValid(offsetMinutes)) {
    return {};
  }
  const int absolute = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  std::wstring text = L"UTC";
  text += (offsetMinutes < 0 ? L'-' : L'+');
  text += Pad2(absolute / 60) + L':' + Pad2(absolute % 60);
  return text;
}

std::wstring FormatCommitTimeText(const CivilTime& value, int offsetMinutes) noexcept {
  if (!CivilTimeLooksValid(value)) {
    return {};
  }
  // 年份用四位，月日與時分秒補零；偏移非法時如實寫「偏移未知」，不憑空填一個 +00:00。
  std::wstring text = std::to_wstring(value.year) + L'-' + Pad2(value.month) + L'-' +
                      Pad2(value.day) + L' ' + Pad2(value.hour) + L':' + Pad2(value.minute) +
                      L':' + Pad2(value.second);
  const std::wstring offset = FormatOffsetText(offsetMinutes);
  text += L'（' + (offset.empty() ? std::wstring(L"偏移未知") : offset) + L'）';
  return text;
}

}  // namespace gc::git
