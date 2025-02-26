#pragma once

#include <windows.h>

#include <string_view>

#include "ui/controls.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 底部操作栏：刷新、创建提交、撤回最近提交、推送，以及右侧的状态说明。
class ActionBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void SetStatus(std::wstring_view text);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 四个按钮与状态说明都不被裁剪时所需的最小宽度。
  [[nodiscard]] int MinimumWidth(const UiMetrics& metrics) const;

  [[nodiscard]] HWND refreshButton() const noexcept { return refresh_; }
  [[nodiscard]] HWND createCommitButton() const noexcept { return createCommit_; }
  [[nodiscard]] HWND undoCommitButton() const noexcept { return undoCommit_; }
  [[nodiscard]] HWND pushButton() const noexcept { return push_; }

private:
  HWND refresh_ = nullptr;
  HWND createCommit_ = nullptr;
  HWND undoCommit_ = nullptr;
  HWND push_ = nullptr;
  HWND status_ = nullptr;
};

}  // namespace gc::ui
