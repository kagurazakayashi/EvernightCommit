#include "support/tiny_test.h"

#include <windows.h>

#include <string>

#include "ui/controls.h"

// 界面「空闲时反复重画」这一类缺陷只能在真实窗口上验证：它们的表现全在消息层面——
// 同样文本的 WM_SETTEXT、位置没变的 MoveWindow、以及关掉却再也没打开的重绘。
// 这里用自定义窗口类把这三类消息计数记录下来，作为「不该再发生」的可观察证据。

namespace {

struct Probe {
  int disableCalls = 0;
  int enableCalls = 0;
  LRESULT firstDisableReturn = -1;
  int posChanges = 0;
  int setTextCalls = 0;
};

Probe* g_probe = nullptr;

LRESULT CALLBACK ProbeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_SETREDRAW) {
    const LRESULT result = ::DefWindowProcW(window, message, wParam, lParam);
    if (g_probe != nullptr) {
      if (wParam == FALSE) {
        ++g_probe->disableCalls;
        if (g_probe->disableCalls == 1) {
          g_probe->firstDisableReturn = result;
        }
      } else {
        ++g_probe->enableCalls;
      }
    }
    return result;
  }
  if (message == WM_WINDOWPOSCHANGING && g_probe != nullptr) {
    ++g_probe->posChanges;
  }
  if (message == WM_SETTEXT && g_probe != nullptr) {
    ++g_probe->setTextCalls;
  }
  return ::DefWindowProcW(window, message, wParam, lParam);
}

constexpr wchar_t kProbeClassName[] = L"EvernightCommitTestRedrawProbe";

bool RegisterProbeClass() {
  static const bool registered = [] {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &ProbeProc;
    description.hInstance = ::GetModuleHandleW(nullptr);
    description.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    description.lpszClassName = kProbeClassName;
    return ::RegisterClassExW(&description) != 0;
  }();
  return registered;
}

// 不 ShowWindow 的顶层窗口：消息照样能送进去，用户屏幕上也不会闪出一个测试窗口。
[[nodiscard]] HWND MakeProbeWindow(DWORD style, HWND parent) {
  if (!RegisterProbeClass()) {
    return nullptr;
  }
  return ::CreateWindowExW(0, kProbeClassName, L"", style, 10, 10, 50, 20, parent, nullptr,
                           ::GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

GC_TEST(list_redraw_pause_re_enables_drawing_even_when_setredraw_reports_zero) {
  const HWND window = MakeProbeWindow(WS_OVERLAPPED, nullptr);
  GC_REQUIRE(window != nullptr, "测试窗口创建失败，无法验证重绘暂停");
  Probe probe;
  g_probe = &probe;

  {
    gc::ui::ListRedrawPause pause(window);
    GC_CHECK_MESSAGE(probe.disableCalls == 1, "构造时应当只关掉一次重绘");
    // 前提：重绘原本开着时，WM_SETREDRAW(FALSE) 并不返回 TRUE。旧实现正是误信了返回值才永不恢复。
    GC_CHECK_MESSAGE(probe.firstDisableReturn != TRUE,
                     "前提已变化：WM_SETREDRAW 现在会报告原状态，本用例的判断依据需要重新核对");
    GC_CHECK_MESSAGE(probe.enableCalls == 0, "作用域内不应提前恢复重绘");
  }

  GC_CHECK_MESSAGE(probe.enableCalls == 1, "作用域结束必须把重绘打开（不能依赖返回值）");
  RECT update{};
  GC_CHECK_MESSAGE(::GetUpdateRect(window, &update, FALSE) != 0,
                   "恢复重绘后必须留下待重绘区域，否则画面还是旧的那一帧");
  g_probe = nullptr;
  ::DestroyWindow(window);
}

GC_TEST(list_redraw_pause_on_null_handle_does_nothing) {
  Probe probe;
  g_probe = &probe;
  {
    gc::ui::ListRedrawPause pause(nullptr);
  }
  GC_CHECK(probe.disableCalls == 0 && probe.enableCalls == 0);
  g_probe = nullptr;
}

GC_TEST(place_if_changed_skips_move_when_geometry_is_identical) {
  const HWND parent = MakeProbeWindow(WS_OVERLAPPED, nullptr);
  const HWND child = MakeProbeWindow(WS_CHILD | WS_VISIBLE, parent);
  GC_REQUIRE(parent != nullptr && child != nullptr, "测试父/子窗口创建失败，无法验证布局去重");
  Probe probe;
  g_probe = &probe;

  // 创建时摆在 10,10,50x20；第一次挪到别处，第二次用同一份矩形——第二次才是「空闲刷新」的形状。
  const RECT target{30, 20, 90, 45};
  gc::ui::PlaceIfChanged(child, target);
  GC_CHECK_MESSAGE(probe.posChanges >= 1, "第一次摆放必须真的移动控件");

  probe.posChanges = 0;
  gc::ui::PlaceIfChanged(child, target);
  GC_CHECK_MESSAGE(probe.posChanges == 0, "位置尺寸都没变时不应再移动：这就是空闲时反复重画的来源");

  probe.posChanges = 0;
  gc::ui::PlaceIfChanged(child, RECT{30, 20, 100, 45});  // 只有宽度变了
  GC_CHECK_MESSAGE(probe.posChanges >= 1, "尺寸变化必须照常移动");

  g_probe = nullptr;
  ::DestroyWindow(child);
  ::DestroyWindow(parent);
}

GC_TEST(set_control_text_skips_identical_text) {
  const HWND parent = MakeProbeWindow(WS_OVERLAPPED, nullptr);
  const HWND child = MakeProbeWindow(WS_CHILD | WS_VISIBLE, parent);
  GC_REQUIRE(parent != nullptr && child != nullptr, "测试父/子窗口创建失败，无法验证文本去重");
  Probe probe;
  g_probe = &probe;

  gc::ui::SetControlText(child, L"当前分支：main");
  GC_CHECK_MESSAGE(probe.setTextCalls >= 1, "第一次设置文本必须真的发下去");
  GC_CHECK(gc::ui::GetControlText(child) == L"当前分支：main");

  probe.setTextCalls = 0;
  gc::ui::SetControlText(child, L"当前分支：main");
  GC_CHECK_MESSAGE(probe.setTextCalls == 0, "同样的文本重设一遍就是可看见的闪烁，不该发出 WM_SETTEXT");

  probe.setTextCalls = 0;
  gc::ui::SetControlText(child, L"当前分支：dev");
  GC_CHECK_MESSAGE(probe.setTextCalls >= 1, "文本变了必须设置");

  g_probe = nullptr;
  ::DestroyWindow(child);
  ::DestroyWindow(parent);
}

// ---------------------------------------------------------------------------
// 残影：控件在窗口里挪动位置或改了尺寸之后，它原来占的那块格子必须有人重画。父窗口带
// WS_CLIPCHILDREN，被分组框占住的那部分不归父窗口擦；而分组框在视觉样式下只画边框和标题，
// 内部一格都不填。两边一凑，字段移动后留下的旧像素谁都不管，用户看到的就是一整块残影。
// 下面三条分别钉住这条链路的三个环节：失效要传到盖着的分组框、分组框要填自己的内部、
// 分组框必须待在兄弟 z 序最底（不然填内部反而把字段糊掉）。
//
// 这一组与前两条不同，必须真的把窗口显示出来：待重绘区域只对可见窗口记账（隐形窗口上
// InvalidateRect 与 RedrawWindow 都不留任何区域，本机实测），而本用例断言的正是那块区域。
// 窗口摆在 -32000,-32000 并带 WS_EX_TOOLWINDOW，用户屏幕上看不见，也不进任务栏与 Alt+Tab。
// ---------------------------------------------------------------------------

[[nodiscard]] HWND MakeOffscreenParent() {
  if (!RegisterProbeClass()) {
    return nullptr;
  }
  const HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW, kProbeClassName, L"",
                                        WS_OVERLAPPED | WS_CLIPCHILDREN, -32000, -32000, 300, 200, nullptr,
                                        nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) {
    return nullptr;
  }
  ::ShowWindow(window, SW_SHOWNOACTIVATE);
  ::UpdateWindow(window);
  ::ValidateRect(window, nullptr);
  return window;
}

GC_TEST(place_if_changed_repaints_the_group_box_under_the_old_position) {
  const HWND parent = MakeOffscreenParent();
  const HWND group = gc::ui::CreateGroupBox(parent, L"提交信息", 1);
  const HWND field = gc::ui::CreateLabel(parent, L"标题：", 2);
  GC_REQUIRE(parent != nullptr && group != nullptr && field != nullptr,
             "测试窗口创建失败，无法验证残影链路");
  gc::ui::SendToBack(group);
  ::MoveWindow(group, 0, 0, 300, 200, TRUE);
  ::MoveWindow(field, 20, 20, 120, 24, TRUE);
  ::UpdateWindow(parent);
  ::ValidateRect(parent, nullptr);
  ::ValidateRect(group, nullptr);

  RECT cleared{};
  GC_REQUIRE_MESSAGE(::GetUpdateRect(group, &cleared, FALSE) == 0,
                     "前提已变化：摆放完成后分组框就不该留有待重绘区域");

  gc::ui::PlaceIfChanged(field, RECT{20, 90, 140, 114});  // 往下挪：旧位置仍然落在分组框范围内

  RECT groupUpdate{};
  GC_CHECK_MESSAGE(::GetUpdateRect(group, &groupUpdate, FALSE) != 0,
                   "字段移动后，盖在它旧位置上的分组框必须收到失效：父窗口带 WS_CLIPCHILDREN，那块擦不到它头上");
  // 分组框自己带 WS_CLIPSIBLINGS，新位置那一段归字段画，所以对它只剩「旧位置必须被覆盖」这一条要求。
  GC_CHECK_MESSAGE(groupUpdate.left <= 20 && groupUpdate.top <= 20 && groupUpdate.right >= 140 &&
                       groupUpdate.bottom >= 44,
                   "分组框的待重绘区域必须覆盖字段留下的旧位置");
  RECT fieldUpdate{};
  GC_CHECK_MESSAGE(::GetUpdateRect(field, &fieldUpdate, FALSE) != 0, "移动后的字段本身也要重画");

  // 空闲刷新（位置与尺寸都没变）不能再留下任何待重绘区域。
  ::ValidateRect(parent, nullptr);
  ::ValidateRect(group, nullptr);
  gc::ui::PlaceIfChanged(field, RECT{20, 90, 140, 114});
  GC_CHECK_MESSAGE(::GetUpdateRect(group, nullptr, FALSE) == 0, "位置没变时不该再失效分组框");

  ::DestroyWindow(field);
  ::DestroyWindow(group);
  ::DestroyWindow(parent);
}

GC_TEST(group_box_erases_its_interior_with_the_parent_background) {
  const HWND parent = MakeProbeWindow(WS_OVERLAPPED | WS_CLIPCHILDREN, nullptr);
  const HWND group = gc::ui::CreateGroupBox(parent, L"提交信息", 1);
  GC_REQUIRE(parent != nullptr && group != nullptr, "测试窗口创建失败");
  ::MoveWindow(group, 0, 0, 200, 120, FALSE);

  const HDC screen = ::GetDC(nullptr);
  const HDC memory = screen != nullptr ? ::CreateCompatibleDC(screen) : nullptr;
  const HBITMAP bitmap = memory != nullptr ? ::CreateCompatibleBitmap(screen, 200, 120) : nullptr;
  GC_REQUIRE(memory != nullptr && bitmap != nullptr, "测试用内存 DC 创建失败");
  const HGDIOBJ previous = ::SelectObject(memory, bitmap);

  // 先整块涂成一个绝不该出现在背景上的颜色，再让分组框自己擦一次。
  const HBRUSH sentinel = ::CreateSolidBrush(RGB(1, 2, 3));
  const RECT area{0, 0, 200, 120};
  ::FillRect(memory, &area, sentinel);
  ::DeleteObject(sentinel);
  GC_CHECK(::GetPixel(memory, 100, 60) == RGB(1, 2, 3));

  const LRESULT erased = ::SendMessageW(group, WM_ERASEBKGND, reinterpret_cast<WPARAM>(memory), 0);
  GC_CHECK_MESSAGE(erased != 0,
                   "分组框必须自己处理 WM_ERASEBKGND：视觉样式下它只画边框与标题，内部一格都不填");
  GC_CHECK_MESSAGE(::GetPixel(memory, 100, 60) == ::GetSysColor(COLOR_BTNFACE),
                   "分组框内部要填上父窗口底色，否则挪走的字段旧像素永远留在原地");

  ::SelectObject(memory, previous);
  ::DeleteObject(bitmap);
  ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  ::DestroyWindow(group);
  ::DestroyWindow(parent);
}

GC_TEST(send_to_back_puts_the_group_box_below_its_fields) {
  const HWND parent = MakeProbeWindow(WS_OVERLAPPED | WS_CLIPCHILDREN, nullptr);
  const HWND group = gc::ui::CreateGroupBox(parent, L"提交信息", 1);
  const HWND field = gc::ui::CreateLabel(parent, L"标题：", 2);
  GC_REQUIRE(parent != nullptr && group != nullptr && field != nullptr, "测试窗口创建失败");
  // 新建的子窗口默认排在兄弟 z 序最前面：先创建的分组框反而压在字段之上。
  GC_REQUIRE_MESSAGE(::GetWindow(group, GW_HWNDNEXT) == field &&
                         ::GetWindow(group, GW_HWNDPREV) == nullptr,
                     "前提已变化：子窗口的默认 z 序不再是「先创建的在最上面」");

  gc::ui::SendToBack(group);
  GC_CHECK_MESSAGE(::GetWindow(group, GW_HWNDNEXT) == nullptr &&
                       ::GetWindow(group, GW_HWNDPREV) == field,
                   "分组框必须压到兄弟 z 序最底，否则它填内部时会把字段整块糊掉");

  ::DestroyWindow(field);
  ::DestroyWindow(group);
  ::DestroyWindow(parent);
}
