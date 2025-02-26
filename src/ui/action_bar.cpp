#include "ui/action_bar.h"

#include <algorithm>
#include <string>

#include "ui/commands.h"

namespace gc::ui {
namespace {

void Place(HWND target, const RECT& rect) {
  if (target != nullptr) {
    ::MoveWindow(target, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
  }
}

constexpr RECT Box(int left, int top, int width, int height) noexcept {
  return RECT{left, top, left + width, top + height};
}

}  // namespace

int ActionBar::MinimumHeight(const UiMetrics& metrics) noexcept {
  return metrics.ControlHeight();
}

int ActionBar::MinimumWidth(const UiMetrics& metrics) const {
  const int gap = metrics.ColGap();
  int width = metrics.LabelWidth(GetControlText(status_));
  for (HWND button : {refresh_, createCommit_, undoCommit_, push_}) {
    width += metrics.ButtonWidth(GetControlText(button)) + gap;
  }
  return width + gap;
}

void ActionBar::Create(HWND parent) {
  refresh_ = CreatePushButton(parent, L"刷新", kIdRefreshButton);
  createCommit_ = CreatePushButton(parent, L"创建提交", kIdCreateCommitButton);
  undoCommit_ = CreatePushButton(parent, L"撤回最近提交", kIdUndoCommitButton);
  push_ = CreatePushButton(parent, L"推送", kIdPushButton);
  status_ = CreateLabel(parent, L"未执行任何操作。", kIdBottomStatusLabel);
}

void ActionBar::SetStatus(std::wstring_view text) {
  SetControlText(status_, text);
}

void ActionBar::Layout(const RECT& area, const UiMetrics& metrics) {
  const int rowHeight = metrics.ControlHeight();
  const int gap = metrics.ColGap();
  const int top = area.top + ((area.bottom - area.top) - rowHeight) / 2;
  const int labelTop = area.top + ((area.bottom - area.top) - metrics.LabelHeight()) / 2;

  int x = area.left;
  for (HWND button : {refresh_, createCommit_, undoCommit_, push_}) {
    const int width = metrics.ButtonWidth(GetControlText(button));
    Place(button, Box(x, top, width, rowHeight));
    x += width + gap;
  }

  const int statusWidth = std::max<int>(metrics.Scale(120), area.right - x - gap);
  Place(status_, Box(x + gap, labelTop, statusWidth, metrics.LabelHeight()));
}

}  // namespace gc::ui
