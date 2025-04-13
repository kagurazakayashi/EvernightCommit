// 提交时间换算与格式化的纯逻辑测试：不起 Git、不开窗口，只断言「墙上时间／epoch／偏移 → 交给 Git 的值」。
// 覆盖：历法边界（闰年、当月天数、年份上下界）、epoch 与墙上时间互为逆运算、
// Git 内部日期形态 `@<秒> <±hhmm>` 的拼法与拒绝条件（含负数秒与偏移非法）、
// 偏移文本与现实时区（+05:45、+14:00、-12:00）的写法，以及给人看的那句「时间（偏移）」。
//
// 期望的秒数一律取自 GNU date（`date -u -d "..." +%s`），与本模块的历法算法互为实现，
// 因此这里比的是「两套独立算法是否给出同一个瞬间」，不是把被测代码再抄一遍。
#include <string>
#include <string_view>

#include "git/commit_date.h"
#include "support/tiny_test.h"

namespace {

using gc::git::CivilTime;

constexpr CivilTime Time(int year, int month, int day, int hour = 0, int minute = 0,
                         int second = 0) noexcept {
  return CivilTime{year, month, day, hour, minute, second};
}

long long Epoch(const CivilTime& value) {
  long long epoch = -1;
  const bool ok = gc::git::CivilTimeToUtcEpoch(value, &epoch);
  return ok ? epoch : -1;
}

std::string GitDate(long long epoch, int offsetMinutes) {
  std::string out;
  std::wstring refusal;
  return gc::git::FormatGitInternalDate(epoch, offsetMinutes, &out, &refusal) ? out : std::string();
}

CivilTime FromEpoch(long long epoch) {
  CivilTime out{};
  static_cast<void>(gc::git::UtcEpochToCivilTime(epoch, &out));
  return out;
}

GC_TEST(civil_time_validates_days_per_gregorian_month) {
  GC_CHECK(gc::git::IsLeapYear(2000));
  GC_CHECK(!gc::git::IsLeapYear(1900));
  GC_CHECK(gc::git::IsLeapYear(2096));
  GC_CHECK(!gc::git::IsLeapYear(2099));

  GC_CHECK(gc::git::CivilTimeLooksValid(Time(2024, 2, 29)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2023, 2, 29)));
  GC_CHECK(gc::git::CivilTimeLooksValid(Time(2026, 4, 30)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 4, 31)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 13, 1)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 0, 1)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 1, 0)));
  GC_CHECK(gc::git::CivilTimeLooksValid(Time(2026, 12, 31, 23, 59, 59)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 12, 31, 24, 0, 0)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 12, 31, 23, 60, 0)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2026, 12, 31, 23, 59, 60)));
}

GC_TEST(civil_time_year_bounds_match_what_git_accepts) {
  // 实测：Git 对负秒数一律 `fatal: invalid date format`（-1 与 -100000000 都是退出码 128），
  // 所以 1970-01-01 UTC 之前提交不了；控件的年份下界按这条事实收紧。
  GC_CHECK(gc::git::CivilTimeLooksValid(Time(1970, 1, 1)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(1969, 12, 31)));
  // 上界 2099：再往后就贴近 Git 实测拒收的 32 位秒数地带（4294967295 +0800 已被拒）。
  GC_CHECK(gc::git::CivilTimeLooksValid(Time(2099, 12, 31, 23, 59, 59)));
  GC_CHECK(!gc::git::CivilTimeLooksValid(Time(2100, 1, 1)));
}

GC_TEST(civil_to_epoch_matches_independent_reference) {
  GC_CHECK_MESSAGE(Epoch(Time(1970, 1, 1)) == 0, "纪元原点应为 0，实际 " + std::to_string(Epoch(Time(1970, 1, 1))));
  GC_CHECK_MESSAGE(Epoch(Time(1970, 1, 1, 8, 0, 0)) == 28800, "1970-01-01 08:00 UTC 应为 28800");
  GC_CHECK_MESSAGE(Epoch(Time(2000, 2, 29, 12, 34, 56)) == 951827696,
                   "闰日 2000-02-29 12:34:56 应为 951827696");
  GC_CHECK_MESSAGE(Epoch(Time(2024, 2, 29)) == 1709164800, "2024-02-29 00:00 应为 1709164800");
  GC_CHECK_MESSAGE(Epoch(Time(2026, 9, 30, 16, 0, 0)) == 1790784000,
                   "2026-09-30 16:00 应为 1790784000");
  GC_CHECK_MESSAGE(Epoch(Time(2038, 1, 19, 3, 14, 7)) == 2147483647,
                   "2038 问题那一秒应为 2147483647");
  GC_CHECK_MESSAGE(Epoch(Time(2099, 12, 31, 23, 59, 59)) == 4102444799,
                   "可选上界应为 4102444799");
  GC_CHECK_MESSAGE(Epoch(Time(1999, 12, 31, 23, 59, 59)) == 946684799, "千年前夜应为 946684799");

  long long untouched = 12345;
  GC_CHECK(!gc::git::CivilTimeToUtcEpoch(Time(2026, 2, 30), &untouched));
  GC_CHECK_MESSAGE(untouched == 0, "换算失败时必须把出参清零，不能留下上一次的秒数");
  GC_CHECK(!gc::git::CivilTimeToUtcEpoch(Time(2026, 1, 1), nullptr));
}

GC_TEST(epoch_and_civil_are_inverse_round_trip) {
  // 逐日跨过 1970 起的一段区间（含闰年、世纪年）往返比对，抓「差一天」这类错误。
  for (int year = 1970; year <= 2099; year += 7) {
    for (int month = 1; month <= 12; ++month) {
      for (int day : {1, 14, 28}) {
        const CivilTime original = Time(year, month, day, 7, 8, 9);
        if (!gc::git::CivilTimeLooksValid(original)) {
          continue;  // 例如 2 月 28 之外的非法组合（这里只取一定存在的三天）。
        }
        const long long epoch = Epoch(original);
        const CivilTime back = FromEpoch(epoch);
        GC_CHECK_MESSAGE(back.year == original.year && back.month == original.month &&
                             back.day == original.day && back.hour == original.hour &&
                             back.minute == original.minute && back.second == original.second,
                         "往返不一致：" + std::to_string(year) + "-" + std::to_string(month) +
                             "-" + std::to_string(day));
      }
    }
  }
  GC_CHECK_MESSAGE(FromEpoch(0).year == 1970 && FromEpoch(0).month == 1 && FromEpoch(0).day == 1,
                   "0 应解回纪元原点");
  CivilTime refused{};
  GC_CHECK(!gc::git::UtcEpochToCivilTime(-1, &refused));
  GC_CHECK(gc::git::UtcEpochToCivilTime(1790784000, nullptr) == false);  // 出参可空，判定不变。
}

GC_TEST(epoch_storability_boundaries) {
  GC_CHECK(!gc::git::IsStorableCommitEpoch(-1));
  GC_CHECK(gc::git::IsStorableCommitEpoch(0));
  GC_CHECK(gc::git::IsStorableCommitEpoch(4102444799));
  GC_CHECK(!gc::git::IsStorableCommitEpoch(4102444800));
  GC_CHECK(!gc::git::IsStorableCommitEpoch(4294967295));  // 实测 Git 也拒这个值。
  CivilTime out{};
  GC_CHECK(!gc::git::UtcEpochToCivilTime(4102444800LL, &out));
}

GC_TEST(format_git_internal_date_builds_internal_form) {
  // `@` 前缀是必需的：实测 Git 只认「至少 9 位数」的裸秒数，`0 +0800`、`10000000 +0800`
  // 都会被判成 invalid date format 让整条命令 128 失败；`@0 +0800` 才正常写入。
  GC_CHECK_MESSAGE(GitDate(0, 480) == "@0 +0800", "纪元原点应拼成 @0 +0800");
  GC_CHECK(GitDate(100, 480) == "@100 +0800");
  GC_CHECK(GitDate(1790784000, 480) == "@1790784000 +0800");
  GC_CHECK(GitDate(1700000000, -300) == "@1700000000 -0500");
  GC_CHECK(GitDate(1700000000, 0) == "@1700000000 +0000");
  // 现实里的非整点时区：尼泊尔 +05:45、查塔姆夏令时 +12:45、基里巴斯 +14:00。
  GC_CHECK(GitDate(1700000000, 345) == "@1700000000 +0545");
  GC_CHECK(GitDate(1700000000, 765) == "@1700000000 +1245");
  GC_CHECK(GitDate(1700000000, 840) == "@1700000000 +1400");
  GC_CHECK(GitDate(1700000000, -720) == "@1700000000 -1200");
}

GC_TEST(format_git_internal_date_rejects_unstorable_values) {
  std::wstring refusal;

  std::string out;
  refusal.clear();
  GC_CHECK(!gc::git::FormatGitInternalDate(-1, 480, &out, &refusal));
  GC_CHECK_MESSAGE(out.empty(), "拒绝时不能交出一个半成品日期");
  GC_CHECK(!refusal.empty());

  refusal.clear();
  GC_CHECK(!gc::git::FormatGitInternalDate(4294967295LL, 480, &out, &refusal));
  GC_CHECK(!refusal.empty());

  // 偏移不是 15 分钟的整数倍（现实时区没有这种东西）→ 拒绝，绝不静默取整。
  refusal.clear();
  GC_CHECK(!gc::git::FormatGitInternalDate(1790784000, 7, &out, &refusal));
  GC_CHECK(!refusal.empty());

  refusal.clear();
  GC_CHECK(!gc::git::FormatGitInternalDate(1790784000, 900, &out, &refusal));  // +15:00 不存在。
  GC_CHECK(!gc::git::FormatGitInternalDate(1790784000, -780, &out, &refusal));  // -13:00 不存在。

  // 出参为空指针也要给出原因（调用方漏了容器属于程序错误，不该被当成成功）。
  GC_CHECK(!gc::git::FormatGitInternalDate(1790784000, 480, nullptr, &refusal));
  GC_CHECK(!refusal.empty());
  // 原因可以不要（调用方只问成不成），但判定本身不能因此改变。
  std::string valid;
  GC_CHECK(gc::git::FormatGitInternalDate(1790784000, 480, &valid, nullptr));
  GC_CHECK_MESSAGE(valid == "@1790784000 +0800", "合法取值即使不带出参原因也要成功");
  valid.clear();
  GC_CHECK(!gc::git::FormatGitInternalDate(-5, 480, &valid, nullptr));
  GC_CHECK_MESSAGE(valid.empty(), "拒绝时不能交出一个半成品日期");
}

GC_TEST(offset_text_uses_utc_colon_form) {
  GC_CHECK(gc::git::FormatOffsetText(480) == L"UTC+08:00");
  GC_CHECK(gc::git::FormatOffsetText(-300) == L"UTC-05:00");
  GC_CHECK(gc::git::FormatOffsetText(0) == L"UTC+00:00");
  GC_CHECK(gc::git::FormatOffsetText(345) == L"UTC+05:45");
  GC_CHECK(gc::git::FormatOffsetText(-720) == L"UTC-12:00");
  // 非法偏移返回空串：界面要显示「偏移未知」，不能凭 0 冒充 UTC。
  GC_CHECK(gc::git::FormatOffsetText(7).empty());
  GC_CHECK(gc::git::FormatOffsetText(900).empty());
}

GC_TEST(commit_time_text_shows_wall_clock_with_offset) {
  const std::wstring text = gc::git::FormatCommitTimeText(Time(2026, 9, 30, 16, 5, 7), 480);
  GC_CHECK_MESSAGE(text == L"2026-09-30 16:05:07（UTC+08:00）",
                   "实际是 " + std::string(text.begin(), text.end()));
  GC_CHECK(gc::git::FormatCommitTimeText(Time(1970, 1, 1), 0) == L"1970-01-01 00:00:00（UTC+00:00）");
  // 偏移算不出来时如实标注，不把「未知」写成 +00:00。
  GC_CHECK(gc::git::FormatCommitTimeText(Time(2026, 1, 1, 1, 2, 3), 7) ==
           L"2026-01-01 01:02:03（偏移未知）");
  GC_CHECK(gc::git::FormatCommitTimeText(Time(2026, 2, 30), 480).empty());
}

}  // namespace
