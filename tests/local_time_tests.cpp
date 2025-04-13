// 本机时区换算（platform/windows/local_time）的测试。这一层是唯一碰操作系统时区规则的代码，
// 因此断言都按「与另一条独立途径是否一致」来写，而不是把被测实现再抄一遍：
//   * 偏移：与 GetDynamicTimeZoneInformation 的 Bias 独立比对（那条途径完全不经过 TzSpecific…）；
//   * 瞬间：与 GetSystemTimePreciseAsFileTime 的 UTC 秒比对（容几秒时钟推进）；
//   * 不变量：epoch + 偏移×60 == 把墙上时间当 UTC 的秒数（偏移的定义式）；
//   * 反方向：先由系统把某个已知 UTC 瞬间换成本机墙上时间，再交回本层换算，
//     两端应落回同一个瞬间（夏令时重复的那一小时容许差一小时，见用例说明）。
// 用例在本机时区下运行，不断言某个具体偏移值，因此换时区、换机器都仍然成立。
#include <string>
#include <vector>

#include "git/commit_date.h"
#include "platform/windows/local_time.h"
#include "platform/windows/locale_text.h"
#include "support/tiny_test.h"

namespace {

using gc::git::CivilTime;
using gc::platform::LocalInstant;

constexpr CivilTime Time(int year, int month, int day, int hour = 0, int minute = 0,
                         int second = 0) noexcept {
  return CivilTime{year, month, day, hour, minute, second};
}

// 把一段 SYSTEMTIME「按 UTC」解释后的 Unix 秒：独立于被测实现的第二条换算途径（走 FILETIME）。
long long EpochTreatingAsUtc(const SYSTEMTIME& value) {
  FILETIME fileTime{};
  if (::SystemTimeToFileTime(&value, &fileTime) == 0) {
    return -1;
  }
  const unsigned long long raw =
      (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
  constexpr unsigned long long kOffset = 116444736000000000ULL;
  return raw < kOffset ? -1 : static_cast<long long>((raw - kOffset) / 10000000ULL);
}

// 操作系统认为「此刻」相对 UTC 的偏移（分钟）：完全用 Bias 算，不碰 TzSpecific 那一族函数。
int BiasOffsetMinutes() {
  DYNAMIC_TIME_ZONE_INFORMATION info{};
  const LONG state = ::GetDynamicTimeZoneInformation(&info);
  const LONG bias = info.Bias + (state == TIME_ZONE_ID_DAYLIGHT ? info.DaylightBias : info.StandardBias);
  return static_cast<int>(-bias);
}

std::string Describe(const LocalInstant& instant) {
  return "year=" + std::to_string(instant.wall.year) + " utcEpoch=" +
         std::to_string(instant.utcEpochSeconds) + " offset=" +
         std::to_string(instant.offsetMinutes) + " valid=" + (instant.valid ? "1" : "0");
}

GC_TEST(local_time_now_matches_independent_bias_and_clock) {
  SYSTEMTIME systemNow{};
  ::GetLocalTime(&systemNow);
  const LocalInstant now = gc::platform::CurrentLocalInstant();
  GC_REQUIRE_MESSAGE(now.valid, "此刻应当能换算：" + Describe(now));

  // 控件里显示的墙上时间就是系统给出的本机关历时间（只到秒）。
  const CivilTime expectedWall = gc::platform::CivilFromSystemTime(systemNow);
  GC_CHECK(now.wall.year == expectedWall.year && now.wall.month == expectedWall.month &&
               now.wall.day == expectedWall.day,
           "墙上时间的日期部分应与 GetLocalTime 一致");

  // 偏移走另一条途径核对：Bias 与 TzSpecific 的判定必须给出同一个数（本机当前时刻）。
  GC_CHECK_MESSAGE(now.offsetMinutes == BiasOffsetMinutes(),
                   "偏移应与 GetDynamicTimeZoneInformation 的 Bias 一致，实际 " +
                       std::to_string(now.offsetMinutes) + " 对 " +
                       std::to_string(BiasOffsetMinutes()));

  // 瞬间走另一条途径核对：与系统精确 UTC 时钟相差应在几秒内（用例执行本身要花时间）。
  FILETIME utcNow{};
  ::GetSystemTimePreciseAsFileTime(&utcNow);
  SYSTEMTIME utcParts{};
  GC_CHECK(::FileTimeToSystemTime(&utcNow, &utcParts) != 0);
  const long long realUtc = EpochTreatingAsUtc(utcParts);
  const long long delta = now.utcEpochSeconds > realUtc ? now.utcEpochSeconds - realUtc
                                                        : realUtc - now.utcEpochSeconds;
  GC_CHECK_MESSAGE(delta <= 10, "换算出的 UTC 秒应贴近系统时钟，差 " + std::to_string(delta) + " 秒");

  // 偏移的定义式：把墙上时间当 UTC 的秒数，一定等于真正的 UTC 秒加上偏移。
  long long wallAsUtc = 0;
  GC_CHECK(gc::git::CivilTimeToUtcEpoch(now.wall, &wallAsUtc));
  GC_CHECK(wallAsUtc - now.utcEpochSeconds == static_cast<long long>(now.offsetMinutes) * 60);
}

GC_TEST(local_time_round_trips_known_utc_instants) {
  // 几个跨年代的已知瞬间：由系统换成本机墙上时间后交回本层，应落回同一瞬间。
  for (const long long utcEpoch : {0LL, 1000000000LL, 1700000000LL, 2147483647LL, 4102444799LL}) {
    CivilTime utcParts = gc::git::CivilTime{};
    if (!gc::git::UtcEpochToCivilTime(utcEpoch, &utcParts)) {
      continue;  // 落在本层年份界之外的瞬间由下一条用例负责。
    }
    const SYSTEMTIME utc = gc::platform::SystemTimeFromCivil(utcParts);
    SYSTEMTIME local{};
    if (::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) == 0) {
      continue;  // 这台机器的时区规则表达不了这个瞬间（例如 1970 之前的本地时间）。
    }
    const CivilTime wall = gc::platform::CivilFromSystemTime(local);
    if (!gc::git::CivilTimeLooksValid(wall)) {
      continue;  // 偏移把日期推出了 1970—2099 的范围，属于本层声明的支持边界。
    }
    const LocalInstant back = gc::platform::ResolveLocalWallTime(wall);
    GC_REQUIRE_MESSAGE(back.valid, "本机墙上时间应当可换算：" + Describe(back));
    const long long delta = back.utcEpochSeconds > utcEpoch ? back.utcEpochSeconds - utcEpoch
                                                           : utcEpoch - back.utcEpochSeconds;
    // 秋令时「重复的那一小时」里，系统正/反向映射各挑一次时可能差一小时；
    // 本层的规矩是：照实回报系统给出的那个偏移，所以界面显示的与交给 Git 的永远是一对。
    GC_CHECK_MESSAGE(delta <= 3600,
                     "两端应落回同一瞬间（允许夏令时重复的 1 小时），差 " +
                         std::to_string(delta) + " 秒");
    long long wallAsUtc = 0;
    GC_CHECK(gc::git::CivilTimeToUtcEpoch(wall, &wallAsUtc));
    GC_CHECK(wallAsUtc - back.utcEpochSeconds == static_cast<long long>(back.offsetMinutes) * 60);
  }
}

GC_TEST(local_time_rejects_values_outside_supported_range) {
  // 年份界外的输入不该被硬凑成一个瞬间，而要给出可读的原因。
  for (const CivilTime bad : {Time(1969, 12, 31, 23, 59, 59), Time(2100, 1, 1), Time(2026, 2, 30),
                              Time(2026, 13, 1), Time(2026, 1, 1, 24, 0, 0)}) {
    const LocalInstant result = gc::platform::ResolveLocalWallTime(bad);
    GC_CHECK_MESSAGE(!result.valid, "不该接受：" + Describe(result));
    GC_CHECK_MESSAGE(!result.failureReason.empty(), "拒绝时必须给原因");
    GC_CHECK(result.utcEpochSeconds == 0 && result.offsetMinutes == 0,
             "拒绝时不能留下半个换算结果");
  }
}

GC_TEST(local_time_systemtime_and_civil_agree) {
  const CivilTime sample = Time(2026, 10, 1, 12, 34, 56);
  const SYSTEMTIME converted = gc::platform::SystemTimeFromCivil(sample);
  GC_CHECK(converted.wYear == 2026 && converted.wMonth == 10 && converted.wDay == 1 &&
           converted.wHour == 12 && converted.wMinute == 34 && converted.wSecond == 56 &&
           converted.wMilliseconds == 0);
  const CivilTime back = gc::platform::CivilFromSystemTime(converted);
  GC_CHECK(back.year == sample.year && back.month == sample.month && back.day == sample.day &&
           back.hour == sample.hour && back.minute == sample.minute && back.second == sample.second);

  // 控件初值用的「此刻」与本层的「此刻」必须是同一件事，否则界面显示与交给 Git 的值会分家。
  const SYSTEMTIME now = gc::platform::CurrentLocalTime();
  const LocalInstant instant = gc::platform::ResolveLocalWallTime(gc::platform::CivilFromSystemTime(now));
  GC_CHECK(instant.valid);
}

GC_TEST(local_timezone_name_is_printable_text) {
  const std::wstring name = gc::platform::LocalTimeZoneName();
  // 系统时区名取不到时返回空串，界面会退回只显示偏移；这里只断言它不会带进控制字符。
  for (const wchar_t c : name) {
    GC_CHECK(c >= 0x20 && c != 0x7F);
  }
}

}  // namespace
