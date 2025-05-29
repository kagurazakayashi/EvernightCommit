// 「时间同步修改」联动的回归测试。缺陷表现是勾着同步时提交者那一行仍显示另一个时刻，
// 而这类表现只在真实控件上存在：DateTimePicker 内部永远存着完整的日期与时分秒，
// 显示出来的只有一半，另一半看不见却会被整份读写带进带出。
#include "support/tiny_test.h"

#include <windows.h>
#include <commctrl.h>

#include "git/commit_date.h"
#include "ui/commit_form.h"

namespace {

using gc::git::CivilTime;
using gc::ui::CommitForm;

constexpr wchar_t kHostClassName[] = L"EvernightCommitTestCommitFormHost";

bool RegisterHostClass() {
  static const bool registered = [] {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    // 与变更区用例同一组类别。注意不要加 ICC_STANDARD_CLASSES：测试二进制没有 comctl32 v6 清单，
    // 该标志在本机实测返回 FALSE（其余标志均为 TRUE），会让整个宿主窗口建不出来。
    controls.dwICC = ICC_DATE_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
      return false;
    }
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = ::DefWindowProcW;
    description.hInstance = ::GetModuleHandleW(nullptr);
    description.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    description.lpszClassName = kHostClassName;
    return ::RegisterClassExW(&description) != 0;
  }();
  return registered;
}

[[nodiscard]] HWND MakeHost() {
  if (!RegisterHostClass()) {
    return nullptr;
  }
  return ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClassName, L"", WS_OVERLAPPED | WS_CLIPCHILDREN,
                           -32000, -32000, 900, 600, nullptr, nullptr,
                           ::GetModuleHandleW(nullptr), nullptr);
}

CivilTime MakeCivil(int year, int month, int day, int hour, int minute, int second) {
  CivilTime value;
  value.year = year;
  value.month = month;
  value.day = day;
  value.hour = hour;
  value.minute = minute;
  value.second = second;
  return value;
}

bool SameCivil(const CivilTime& left, const CivilTime& right) {
  return left.year == right.year && left.month == right.month && left.day == right.day &&
         left.hour == right.hour && left.minute == right.minute && left.second == right.second;
}

SYSTEMTIME MakeSystemTime(const CivilTime& value) {
  SYSTEMTIME time{};
  time.wYear = static_cast<WORD>(value.year);
  time.wMonth = static_cast<WORD>(value.month);
  time.wDay = static_cast<WORD>(value.day);
  time.wHour = static_cast<WORD>(value.hour);
  time.wMinute = static_cast<WORD>(value.minute);
  time.wSecond = static_cast<WORD>(value.second);
  return time;
}

// 模拟用户敲控件：DTM_SETSYSTEMTIME 改哪一块，那一块的可见与不可见两半都按给的值走，
// 而另一块控件不受影响——这正是「看不见的那一半」会与可见部分分道扬镳的原因。
void Pick(HWND picker, const CivilTime& value) {
  const SYSTEMTIME time = MakeSystemTime(value);
  ::SendMessageW(picker, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&time));
}

}  // namespace

GC_TEST(commit_form_mirror_uses_visible_author_clock_not_date_controls_hidden_time) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证时间联动");
  CommitForm form;
  form.Create(host);

  // 程序先把两边都设成「此刻」：四块控件内部的隐藏那一半也一起是 22:18:11。
  const CivilTime launch = MakeCivil(2026, 10, 15, 22, 18, 11);
  form.SetTimes(MakeSystemTime(launch), MakeSystemTime(launch));

  // 用户挑了一个过去的时刻：日期块改日期，时钟块改时分秒。
  // 改完之后，日期控件内部那份看不见的时分秒仍然是控件建立时的那一刻。
  const CivilTime picked = MakeCivil(2025, 5, 29, 20, 18, 11);
  Pick(form.AuthorDate(), MakeCivil(2025, 5, 29, 22, 18, 11));
  Pick(form.AuthorClock(), MakeCivil(2026, 10, 15, 20, 18, 11));
  form.NoteTimesUserEdited();

  GC_REQUIRE_MESSAGE(SameCivil(form.AuthorWallTime(), picked),
                     "前提已变化：作者两半拼回来的墙上时间与所挑时刻不符");

  form.MirrorCommitterTime();

  // 旧实现整份抄日期控件，提交者时钟于是停在 22:18:11——屏幕上就是两个不一样的时刻。
  GC_CHECK_MESSAGE(SameCivil(form.CommitterWallTime(), picked),
                   "同步联动必须把作者「看得见」的时分秒带过去，而不是日期控件内部那份看不见的");
  GC_CHECK_MESSAGE(form.TimesUserEdited(),
                   "联动是用户改作者时间引起的，不能把「用户改过时间」的记号抹掉");

  ::DestroyWindow(host);
}

GC_TEST(commit_form_mirror_keeps_both_committer_pickers_in_step) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证时间联动");
  CommitForm form;
  form.Create(host);

  const CivilTime first = MakeCivil(2024, 3, 1, 9, 5, 7);
  Pick(form.AuthorDate(), first);
  Pick(form.AuthorClock(), first);
  form.MirrorCommitterTime();
  GC_CHECK_MESSAGE(SameCivil(form.CommitterWallTime(), first), "首次联动应等于作者时间");

  const CivilTime second = MakeCivil(2023, 12, 31, 23, 59, 58);
  Pick(form.AuthorDate(), second);
  Pick(form.AuthorClock(), second);
  form.MirrorCommitterTime();
  GC_CHECK_MESSAGE(SameCivil(form.CommitterWallTime(), second),
                   "作者时间再改一次，提交者两块控件要一起跟上，不能只挪日期");

  ::DestroyWindow(host);
}
