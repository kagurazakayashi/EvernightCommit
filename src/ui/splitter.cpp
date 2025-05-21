#include "ui/splitter.h"

#include <windowsx.h>

#include "ui/commands.h"
#include "ui/controls.h"

namespace gc::ui {
namespace {

[[nodiscard]] Splitter* Self(HWND window) noexcept {
  return reinterpret_cast<Splitter*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

void PaintGripper(HWND window, HDC dc) {
  RECT area{};
  ::GetClientRect(window, &area);
  const int width = area.right - area.left;
  const int height = area.bottom - area.top;
  if (width <= 0 || height <= 0) {
    return;
  }

  ::FillRect(dc, &area, ::GetSysColorBrush(COLOR_BTNFACE));
  const RECT leftEdge{area.left, area.top, area.left + 1, area.bottom};
  const RECT rightEdge{area.right - 1, area.top, area.right, area.bottom};
  ::FillRect(dc, &leftEdge, ::GetSysColorBrush(COLOR_3DSHADOW));
  ::FillRect(dc, &rightEdge, ::GetSysColorBrush(COLOR_3DHILIGHT));

  const int centerX = area.left + width / 2;
  const int centerY = area.top + height / 2;
  const int dotLeft = centerX - 1;
  const int dotRight = centerX + 1;
  const RECT upper{dotLeft, centerY - 5, dotRight, centerY - 3};
  const RECT middle{dotLeft, centerY - 1, dotRight, centerY + 1};
  const RECT lower{dotLeft, centerY + 3, dotRight, centerY + 5};
  ::FillRect(dc, &upper, ::GetSysColorBrush(COLOR_3DDKSHADOW));
  ::FillRect(dc, &middle, ::GetSysColorBrush(COLOR_3DDKSHADOW));
  ::FillRect(dc, &lower, ::GetSysColorBrush(COLOR_3DDKSHADOW));
}

}  // namespace

bool Splitter::RegisterWindowClass(HINSTANCE instance) {
  WNDCLASSEXW existing{sizeof(existing)};
  if (::GetClassInfoExW(instance, kSplitterWindowClass, &existing) != 0) {
    return true;  // 同一进程内重复创建时直接使用已注册的类。
  }
  WNDCLASSEXW description{};
  description.cbSize = sizeof(description);
  description.style = CS_HREDRAW | CS_VREDRAW;
  description.lpfnWndProc = &Splitter::WndProc;
  description.hInstance = instance;
  description.hCursor = ::LoadCursorW(nullptr, IDC_SIZEWE);
  description.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
  description.lpszClassName = kSplitterWindowClass;
  return ::RegisterClassExW(&description) != 0;
}

void Splitter::UnregisterWindowClass(HINSTANCE instance) {
  ::UnregisterClassW(kSplitterWindowClass, instance);
}

bool Splitter::Create(HWND parent, int id) {
  id_ = id;
  const auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
  window_ = ::CreateWindowExW(0, kSplitterWindowClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
  return window_ != nullptr;
}

void Splitter::SetBounds(const RECT& bounds) {
  PlaceIfChanged(window_, bounds);
}

LRESULT CALLBACK Splitter::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_NCCREATE: {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
      ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      break;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = ::BeginPaint(window, &paint);
      PaintGripper(window, dc);
      ::EndPaint(window, &paint);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      if (Splitter* self = Self(window); self != nullptr) {
        self->dragging_ = true;
        ::SetCapture(window);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      Splitter* self = Self(window);
      if (self == nullptr || !self->dragging_) {
        return 0;
      }
      POINT point{GET_X_LPARAM(lParam), 0};
      ::ClientToScreen(window, &point);
      HWND parent = ::GetParent(window);
      if (parent != nullptr) {
        ::ScreenToClient(parent, &point);
        ::SendMessageW(parent, kSplitterDragged, static_cast<WPARAM>(self->id_), static_cast<LPARAM>(point.x));
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (Splitter* self = Self(window); self != nullptr && self->dragging_) {
        self->dragging_ = false;
        ::ReleaseCapture();
      }
      return 0;
    }
    case WM_CAPTURECHANGED: {
      if (Splitter* self = Self(window); self != nullptr) {
        self->dragging_ = false;
      }
      return 0;
    }
    case WM_SETCURSOR: {
      ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
      return TRUE;
    }
    default:
      break;
  }
  return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace gc::ui
