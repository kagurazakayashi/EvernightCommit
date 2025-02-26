#include "ui/ui_metrics.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace gc::ui {
namespace {

class ScreenDc {
public:
  ScreenDc() : dc_(::GetDC(nullptr)) {}
  ScreenDc(const ScreenDc&) = delete;
  ScreenDc& operator=(const ScreenDc&) = delete;
  ~ScreenDc() {
    if (dc_ != nullptr) {
      ::ReleaseDC(nullptr, dc_);
    }
  }
  [[nodiscard]] HDC get() const noexcept { return dc_; }

private:
  HDC dc_ = nullptr;
};

class SelectedGdiObject {
public:
  SelectedGdiObject(HDC dc, HGDIOBJ object) : dc_(dc), previous_(::SelectObject(dc, object)) {}
  SelectedGdiObject(const SelectedGdiObject&) = delete;
  SelectedGdiObject& operator=(const SelectedGdiObject&) = delete;
  ~SelectedGdiObject() {
    if (previous_ != nullptr) {
      ::SelectObject(dc_, previous_);
    }
  }

private:
  HDC dc_ = nullptr;
  HGDIOBJ previous_ = nullptr;
};

}  // namespace

void UiMetrics::UpdateForDpi(UINT dpi) {
  dpi_ = (dpi == 0) ? 96u : dpi;

  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  LOGFONTW font{};
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0) {
    font = metrics.lfMessageFont;
  } else {
    font.lfHeight = -12;
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
  }
  font.lfHeight = ::MulDiv(font.lfHeight, static_cast<int>(dpi_), 96);
  font.lfCharSet = DEFAULT_CHARSET;

  font_.Reset(::CreateFontIndirectW(&font));
  fontHeight_ = std::abs(font.lfHeight) + ::MulDiv(2, static_cast<int>(dpi_), 96);
}

int UiMetrics::Scale(int base96) const noexcept {
  return ::MulDiv(base96, static_cast<int>(dpi_), 96);
}

int UiMetrics::TextWidth(std::wstring_view text) const {
  if (text.empty()) {
    return 0;
  }
  std::wstring buffer(text);
  ScreenDc dc;
  if (dc.get() == nullptr) {
    return static_cast<int>(buffer.size()) * fontHeight_ / 2;
  }
  const SelectedGdiObject selected(dc.get(), font_.get());
  SIZE size{};
  if (::GetTextExtentPoint32W(dc.get(), buffer.c_str(), static_cast<int>(buffer.size()), &size) == 0) {
    return static_cast<int>(buffer.size()) * fontHeight_ / 2;
  }
  return size.cx;
}

int UiMetrics::ButtonWidth(std::wstring_view text) const {
  // BS_PUSHBUTTON 自身留约 2*系统边框边距；再加少量余量避免中文被截断。
  return TextWidth(text) + Scale(26);
}

int UiMetrics::LabelWidth(std::wstring_view text) const {
  // 静态控件按矩形裁剪文字，末尾字形的右侧悬垂需要额外余量。
  return TextWidth(text) + Scale(4);
}

UINT DpiForWindowOrSystem(HWND hwnd) noexcept {
  if (hwnd != nullptr) {
    const UINT dpi = ::GetDpiForWindow(hwnd);
    if (dpi != 0) {
      return dpi;
    }
  }
  HDC dc = ::GetDC(nullptr);
  const UINT dpi = (dc != nullptr) ? static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX)) : 96u;
  if (dc != nullptr) {
    ::ReleaseDC(nullptr, dc);
  }
  return dpi == 0 ? 96u : dpi;
}

}  // namespace gc::ui
