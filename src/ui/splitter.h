#pragma once

#include <windows.h>

namespace gc::ui {

inline constexpr const wchar_t* kSplitterWindowClass = L"EvernightCommit.Splitter";

// 可拖动的竖向分隔条。它只处理鼠标捕获与绘制，并把父窗口客户区横坐标通知给父窗口；
// 宽度与比例的换算统一由 ui/layout.cpp 负责。
class Splitter {
public:
  [[nodiscard]] static bool RegisterWindowClass(HINSTANCE instance);
  static void UnregisterWindowClass(HINSTANCE instance);

  bool Create(HWND parent, int id);
  void SetBounds(const RECT& bounds);

  [[nodiscard]] HWND handle() const noexcept { return window_; }
  [[nodiscard]] int id() const noexcept { return id_; }

private:
  static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

  HWND window_ = nullptr;
  int id_ = 0;
  bool dragging_ = false;
};

}  // namespace gc::ui
