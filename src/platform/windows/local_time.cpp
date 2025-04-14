#include "platform/windows/local_time.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include "git/commit_date.h"

namespace gc::platform {
namespace {

// 1601-01-01 到 1970-01-01 之间的 100 纳秒间隔数（FILETIME 与 Unix 秒的固定差）。
constexpr unsigned long long kFileTimeUnixEpochOffset = 116444736000000000ULL;

// 把一个 SYSTEMTIME 当成 UTC 时刻换算成 Unix 秒。SystemTimeToFileTime 失败（年份越界等）返回 false。
bool UtcSystemTimeToEpochSeconds(const SYSTEMTIME& value, long long* outEpoch) {
  FILETIME fileTime{};
  if (::SystemTimeToFileTime(&value, &fileTime) == 0) {
    return false;
  }
  const unsigned long long raw =
      (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
  // FILETIME 从 1601-01-01 UTC 起算，早于 Unix 纪元的值不可能是合法提交时间。
  if (raw < kFileTimeUnixEpochOffset) {
    return false;
  }
  *outEpoch = static_cast<long long>((raw - kFileTimeUnixEpochOffset) / 10000000ULL);
  return true;
}

}  // namespace

git::CivilTime CivilFromSystemTime(const SYSTEMTIME& value) noexcept {
  git::CivilTime result;
  result.year = value.wYear;
  result.month = value.wMonth;
  result.day = value.wDay;
  result.hour = value.wHour;
  result.minute = value.wMinute;
  result.second = value.wSecond;
  return result;
}

SYSTEMTIME SystemTimeFromCivil(const git::CivilTime& value) noexcept {
  SYSTEMTIME result{};
  result.wYear = static_cast<WORD>(value.year);
  result.wMonth = static_cast<WORD>(value.month);
  result.wDay = static_cast<WORD>(value.day);
  result.wHour = static_cast<WORD>(value.hour);
  result.wMinute = static_cast<WORD>(value.minute);
  result.wSecond = static_cast<WORD>(value.second);
  result.wMilliseconds = 0;
  // wDayOfWeek 由系统换算，控件与 FILETIME 都不看它；留零即可。
  return result;
}

LocalInstant ResolveLocalWallTime(const git::CivilTime& wall) {
  LocalInstant instant;
  instant.wall = wall;
  if (!git::CivilTimeLooksValid(wall)) {
    instant.failureReason =
        L"这个日期时间不成立（年份要在 1970 到 2099 之间，月日与时分秒也要各自不越界）。";
    return instant;
  }

  // 墙上时间 → UTC 瞬间：交给系统按本机时区规则（含那一天的夏令时）判定。
  // 传 nullptr 表示「本机当前时区」，这是本程序唯一需要的语义。
  const SYSTEMTIME localValue = SystemTimeFromCivil(wall);
  SYSTEMTIME utcValue{};
  if (::TzSpecificLocalTimeToSystemTime(nullptr, &localValue, &utcValue) == 0) {
    instant.failureReason =
        L"本机时区没能把这个日期时间换算成一个时刻（Windows 错误码 " +
        std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return instant;
  }

  // UTC 那一侧的秒数：交给 Git 的 epoch 用它。
  long long utcEpoch = 0;
  if (!UtcSystemTimeToEpochSeconds(utcValue, &utcEpoch)) {
    instant.failureReason = L"换算出来的时刻超出了可以记录的范围（1970 至 2099 年之间）。";
    return instant;
  }

  // 墙上时间当成 UTC 的秒数（纯算法，不涉及时区）：两者之差就是那一刻的实际偏移。
  // 直接用 CivilTimeToUtcEpoch 拿不到这个值——它按本程序的年份界校验，而偏移算式需要的是
  // 「把年月日时分秒当作 UTC」的裸秒数，因此这里用 FILETIME 走同一条换算。
  const SYSTEMTIME wallAsUtc = localValue;  // 字段完全相同，只是按 UTC 解释。
  long long wallEpoch = 0;
  if (!UtcSystemTimeToEpochSeconds(wallAsUtc, &wallEpoch)) {
    instant.failureReason = L"换算出来的时刻超出了可以记录的范围（1970 至 2099 年之间）。";
    return instant;
  }

  const long long offsetSeconds = wallEpoch - utcEpoch;
  // 现实时区都是整分钟；出现非整分钟（罕见的历史秒偏移）时按四舍五入取整并如实显示。
  const long long rounding = offsetSeconds >= 0 ? 30 : -30;
  const long long offsetMinutesValue = (offsetSeconds + rounding) / 60;
  if (offsetMinutesValue < -1024 || offsetMinutesValue > 1024) {
    instant.failureReason = L"本机时区给出的偏移超出现实范围，无法交给 Git。";
    return instant;
  }

  instant.utcEpochSeconds = utcEpoch;
  instant.offsetMinutes = static_cast<int>(offsetMinutesValue);
  instant.valid = true;
  return instant;
}

LocalInstant CurrentLocalInstant() {
  SYSTEMTIME now{};
  ::GetLocalTime(&now);
  return ResolveLocalWallTime(CivilFromSystemTime(now));
}

std::wstring LocalTimeZoneName() {
  DYNAMIC_TIME_ZONE_INFORMATION info{};
  const LONG state = ::GetDynamicTimeZoneInformation(&info);
  const bool daylight = (state == TIME_ZONE_ID_DAYLIGHT);
  const wchar_t* name = daylight ? info.DaylightName : info.StandardName;
  if (name == nullptr || name[0] == L'\0') {
    return {};
  }
  return std::wstring(name);
}

std::wstring FormatLocalEpochSeconds(long long epochSeconds) {
  const std::wstring fallback = std::to_wstring(epochSeconds);
  // Unix 秒 → FILETIME（100 纳秒间隔，1601 起算）。负秒数（1970 之前的提交）先判不越过 FILETIME 零点。
  const __int64 raw = static_cast<__int64>(kFileTimeUnixEpochOffset) +
                      static_cast<__int64>(epochSeconds) * 10000000LL;
  if (raw < 0) {
    return fallback;
  }
  FILETIME utcFileTime{};
  utcFileTime.dwLowDateTime = static_cast<DWORD>(raw & 0xFFFFFFFFULL);
  utcFileTime.dwHighDateTime = static_cast<DWORD>(static_cast<unsigned __int64>(raw) >> 32);
  SYSTEMTIME utcTime{};
  if (::FileTimeToSystemTime(&utcFileTime, &utcTime) == 0) {
    return fallback;
  }
  SYSTEMTIME localTime{};
  if (::SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime) == 0) {
    return fallback;
  }
  const auto pad = [](unsigned long value, unsigned width) {
    std::wstring text = std::to_wstring(value);
    while (text.size() < width && text.size() < 4) {  // 年份为 4 位，其余 2 位。
      text.insert(text.begin(), L'0');
    }
    return text;
  };
  // pad(年份, 4)：四位年份不需要补零，小于 1000 的年份按实际位数显示即可。
  std::wstring year = std::to_wstring(localTime.wYear);
  while (year.size() < 4) {
    year.insert(year.begin(), L'0');
  }
  return year + L"-" + pad(localTime.wMonth, 2) + L"-" + pad(localTime.wDay, 2) + L" " +
         pad(localTime.wHour, 2) + L":" + pad(localTime.wMinute, 2) + L":" + pad(localTime.wSecond, 2);
}

}  // namespace gc::platform
