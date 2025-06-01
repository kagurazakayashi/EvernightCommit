// Linux 本机时区换算（linux 组）：Unix 时间、实际 UTC 偏移、夏令时缺失/重复。
// 用 POSIX TZ 规则串固定时区，不依赖发行版是否装了 tzdata 的某个城市文件。
#include <string>

#include "git/commit_date.h"
#include "platform/local_time.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::git::CivilTime;
using gc::platform::LocalInstant;
using gc::test::ScopedEnvironmentVariable;

CivilTime Time(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  return CivilTime{year, month, day, hour, minute, second};
}

}  // namespace

GC_TEST(linux_local_time_utc_roundtrips_unix_epochs) {
  ScopedEnvironmentVariable tz("TZ", std::string("UTC0"));
  const CivilTime wall = Time(2026, 10, 21, 12, 34, 56);
  long long expected = 0;
  GC_REQUIRE_MESSAGE(gc::git::CivilTimeToUtcEpoch(wall, &expected), "墙上午夜应可换算");

  const LocalInstant instant = gc::platform::ResolveLocalWallTime(wall);
  GC_REQUIRE_MESSAGE(instant.valid, gc::platform::WideToUtf8(instant.failureReason));
  GC_CHECK(!instant.ambiguous);
  GC_CHECK(instant.utcEpochSeconds == expected);
  GC_CHECK(instant.offsetMinutes == 0);
  GC_CHECK(gc::platform::FormatLocalEpochSeconds(0) == L"1970-01-01 00:00:00");
  GC_CHECK(gc::platform::FormatLocalEpochSeconds(expected) == L"2026-10-21 12:34:56");

  const LocalInstant now = gc::platform::CurrentLocalInstant();
  GC_CHECK_MESSAGE(now.valid, gc::platform::WideToUtf8(now.failureReason));
  GC_CHECK_MESSAGE(now.offsetMinutes == 0, "UTC0 下此刻偏移应为 0");
}

GC_TEST(linux_local_time_detects_missing_dst_wall_time) {
  ScopedEnvironmentVariable tz("TZ", std::string("EST5EDT,M3.2.0/2,M11.1.0/2"));
  // 2026-03-08 02:30 不存在：当地时钟从 02:00 直接跳到 03:00。
  const LocalInstant gap = gc::platform::ResolveLocalWallTime(Time(2026, 3, 8, 2, 30));
  GC_CHECK_MESSAGE(!gap.valid, "夏令时空洞里的墙上时间不能被猜成一个瞬间");
  GC_CHECK(!gap.ambiguous);
  GC_CHECK_MESSAGE(gap.utcEpochSeconds == 0 && gap.offsetMinutes == 0,
                   "拒绝时不能留下半个换算结果");
  GC_CHECK(!gap.failureReason.empty());
}

GC_TEST(linux_local_time_marks_repeated_dst_wall_time_and_reports_actual_offsets) {
  ScopedEnvironmentVariable tz("TZ", std::string("EST5EDT,M3.2.0/2,M11.1.0/2"));
  // 2026-11-01 01:30 出现两次：先 EDT(-04:00)，后 EST(-05:00)。
  const CivilTime wall = Time(2026, 11, 1, 1, 30);
  long long wallAsUtc = 0;
  GC_REQUIRE_MESSAGE(gc::git::CivilTimeToUtcEpoch(wall, &wallAsUtc), "墙上时间应可换算");

  const LocalInstant instant = gc::platform::ResolveLocalWallTime(wall);
  GC_REQUIRE_MESSAGE(instant.valid, gc::platform::WideToUtf8(instant.failureReason));
  GC_CHECK_MESSAGE(instant.ambiguous, "重复的墙上时间必须被标记");
  GC_CHECK_MESSAGE(instant.offsetMinutes == -240, "固定选较早的 EDT 瞬间");
  GC_CHECK_MESSAGE(instant.alternateOffsetMinutes == -300, "另一个瞬间是 EST");
  GC_CHECK(instant.utcEpochSeconds == wallAsUtc + 240 * 60);
  GC_CHECK(instant.alternateUtcEpochSeconds == wallAsUtc + 300 * 60);
  GC_CHECK(instant.alternateUtcEpochSeconds - instant.utcEpochSeconds == 3600);

  // 交给 Git 的日期串带着实际偏移，而不是一个裸 epoch。
  std::string gitDate;
  std::wstring refusal;
  GC_REQUIRE_MESSAGE(gc::git::FormatGitInternalDate(instant.utcEpochSeconds, instant.offsetMinutes, &gitDate, &refusal),
                     "日期串应可格式化");
  GC_CHECK(gitDate.find(" -0400") != std::string::npos);
}

GC_TEST(linux_local_time_reports_standard_and_daylight_offsets) {
  ScopedEnvironmentVariable tz("TZ", std::string("EST5EDT,M3.2.0/2,M11.1.0/2"));
  const LocalInstant winter = gc::platform::ResolveLocalWallTime(Time(2026, 1, 15, 12, 0));
  const LocalInstant summer = gc::platform::ResolveLocalWallTime(Time(2026, 7, 1, 12, 0));
  GC_REQUIRE_MESSAGE(winter.valid && summer.valid, "冬夏两个时刻都应可换算");
  GC_CHECK(!winter.ambiguous && !summer.ambiguous);
  GC_CHECK_MESSAGE(winter.offsetMinutes == -300, "冬季应是 EST(-05:00)");
  GC_CHECK_MESSAGE(summer.offsetMinutes == -240, "夏季应是 EDT(-04:00)");
  GC_CHECK(winter.utcEpochSeconds != summer.utcEpochSeconds);

  // 偏移定义式：epoch + 偏移 = 把墙上时间当 UTC 的秒数。
  long long wallAsUtc = 0;
  GC_REQUIRE_MESSAGE(gc::git::CivilTimeToUtcEpoch(summer.wall, &wallAsUtc), "夏季墙上时间应可换算");
  GC_CHECK(wallAsUtc - summer.utcEpochSeconds == static_cast<long long>(summer.offsetMinutes) * 60);
}

GC_TEST(linux_local_time_rejects_out_of_range_values) {
  ScopedEnvironmentVariable tz("TZ", std::string("UTC0"));
  for (const CivilTime bad : {Time(1969, 12, 31, 23, 59, 59), Time(2100, 1, 1),
                              Time(2026, 2, 30), Time(2026, 13, 1), Time(2026, 1, 1, 24, 0, 0)}) {
    const LocalInstant result = gc::platform::ResolveLocalWallTime(bad);
    GC_CHECK_MESSAGE(!result.valid, "不该接受越界或不存在的时间");
    GC_CHECK(!result.failureReason.empty());
  }
}

GC_TEST(linux_timezone_name_has_no_replacement_characters) {
  ScopedEnvironmentVariable tz("TZ", std::string("UTC0"));
  const std::wstring name = gc::platform::LocalTimeZoneName();
  for (const wchar_t c : name) {
    GC_CHECK(c >= 0x20 && c != static_cast<wchar_t>(0xFFFD));
  }
}
