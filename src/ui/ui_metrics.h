#pragma once

#include <windows.h>

#include <string_view>

#include "platform/windows/raii.h"

namespace gc::ui {

// 与 DPI 相关的所有尺寸只在这里换算一次，界面代码不直接写像素常量。
// 96 DPI 基准值取自 Windows 桌面应用的常规间距习惯。
class UiMetrics {
public:
  UiMetrics() = default;

  void UpdateForDpi(UINT dpi);

  [[nodiscard]] HFONT Font() const noexcept { return font_.get(); }
  [[nodiscard]] UINT Dpi() const noexcept { return dpi_; }
  [[nodiscard]] int Scale(int base96) const noexcept;

  [[nodiscard]] int Margin() const noexcept { return Scale(9); }
  [[nodiscard]] int RowGap() const noexcept { return Scale(6); }
  [[nodiscard]] int ColGap() const noexcept { return Scale(6); }
  [[nodiscard]] int ControlHeight() const noexcept { return Scale(24); }
  [[nodiscard]] int LabelHeight() const noexcept { return fontHeight_; }
  [[nodiscard]] int SplitterWidth() const noexcept { return Scale(7); }
  [[nodiscard]] int ArrowColumnWidth() const noexcept { return Scale(118); }
  [[nodiscard]] int MinListWidth() const noexcept { return Scale(168); }
  [[nodiscard]] int MinArrowColumnWidth() const noexcept { return Scale(96); }
  [[nodiscard]] int StatusRowHeight() const noexcept { return Scale(20); }

  // 按当前字体测量文本，用于给按钮和标签一个不会截断的宽度。
  [[nodiscard]] int TextWidth(std::wstring_view text) const;
  [[nodiscard]] int ButtonWidth(std::wstring_view text) const;
  [[nodiscard]] int LabelWidth(std::wstring_view text) const;

private:
  platform::OwnedFont font_;
  UINT dpi_ = 96;
  int fontHeight_ = 16;
};

// 取窗口当前的 DPI；无法查询时退回系统 DPI。
[[nodiscard]] UINT DpiForWindowOrSystem(HWND hwnd) noexcept;

}  // namespace gc::ui
