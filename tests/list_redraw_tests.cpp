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
