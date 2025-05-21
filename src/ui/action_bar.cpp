#include "ui/action_bar.h"

#include <algorithm>
#include <string>

#include "ui/commands.h"
#include "ui/controls.h"

namespace gc::ui {
namespace {

void Place(HWND target, const RECT& rect) { PlaceIfChanged(target, rect); }

constexpr RECT Box(int left, int top, int width, int height) noexcept {
  return RECT{left, top, left + width, top + height};
}

}  // namespace

std::array<HWND, 7> ActionBar::Buttons() const noexcept {
  return {refresh_, createCommit_, undoCommit_, push_, conflictView_, conflictContinue_, conflictAbort_};
}

std::array<HWND, 5> ActionBar::PreferenceControls() const noexcept {
  return {persistRecords_, persistDrafts_, historyCheck_, clearPrefs_, historyBrowse_};
}

int ActionBar::PreferenceControlWidth(HWND control, const UiMetrics& metrics) const {
  if (control == nullptr) {
    return 0;
  }
  const std::wstring text = GetControlText(control);
  // 复选框没有现成的「整控件宽度」度量：按「方框 + 文字」估算，与 Create 给的标题一致即可；
  // 「清除已存记录」与「操作历史…」是按钮，走与其余按钮同一套字体度量。
  const bool button = control == clearPrefs_ || control == historyBrowse_;
  return button ? metrics.ButtonWidth(text) : metrics.Scale(20) + metrics.LabelWidth(text);
}

int ActionBar::MinimumHeight(const UiMetrics& metrics) noexcept {
  return metrics.ControlHeight();
}

int ActionBar::MinimumWidth(const UiMetrics& metrics) const {
  const int gap = metrics.ColGap();
  int width = metrics.LabelWidth(GetControlText(status_));
  for (HWND button : Buttons()) {
    width += metrics.ButtonWidth(GetControlText(button)) + gap;
  }
  for (HWND control : PreferenceControls()) {
    width += PreferenceControlWidth(control, metrics) + gap;
  }
  return width + gap;
}

void ActionBar::Create(HWND parent) {
  refresh_ = CreatePushButton(parent, L"刷新", kIdRefreshButton);
  createCommit_ = CreatePushButton(parent, L"创建提交", kIdCreateCommitButton);
  undoCommit_ = CreatePushButton(parent, L"撤回最近提交", kIdUndoCommitButton);
  push_ = CreatePushButton(parent, L"推送", kIdPushButton);
  conflictView_ = CreatePushButton(parent, L"查看冲突状态", kIdConflictViewButton);
  conflictContinue_ = CreatePushButton(parent, L"继续该流程", kIdConflictContinueButton);
  conflictAbort_ = CreatePushButton(parent, L"中止该流程", kIdConflictAbortButton);
  // 持久化区：两个开关的初值在 OnCreate 里按读回来的记录/首次说明改（Create 时先给默认勾上，
  // 首帧不被旧数据牵引；「清除已存记录」任何时刻可点，点了才会有动作）。
  persistRecords_ = CreateCheckBox(parent, L"保存记录", kIdPersistRecordsCheck, true);
  persistDrafts_ = CreateCheckBox(parent, L"保存草稿", kIdPersistDraftsCheck, true);
  clearPrefs_ = CreatePushButton(parent, L"清除已存记录", kIdClearPrefsButton);
  // 操作历史区（可选功能）：总开关默认关——没勾之前一个字节都不写；「操作历史…」是查看/导出/清除/恢复入口。
  historyCheck_ = CreateCheckBox(parent, L"记录操作历史", kIdHistoryCheck, false);
  historyBrowse_ = CreatePushButton(parent, L"操作历史…", kIdHistoryBrowseButton);
  status_ = CreateLabel(parent, L"未执行任何操作。", kIdBottomStatusLabel);
}

void ActionBar::SetPreferenceChecks(bool records, bool drafts) {
  ::SendMessageW(persistRecords_, BM_SETCHECK, records ? BST_CHECKED : BST_UNCHECKED, 0);
  ::SendMessageW(persistDrafts_, BM_SETCHECK, drafts ? BST_CHECKED : BST_UNCHECKED, 0);
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
  for (HWND button : Buttons()) {
    const int width = metrics.ButtonWidth(GetControlText(button));
    Place(button, Box(x, top, width, rowHeight));
    x += width + gap;
  }
  for (HWND control : PreferenceControls()) {
    const int width = PreferenceControlWidth(control, metrics);
    Place(control, Box(x, top, width, rowHeight));
    x += width + gap;
  }

  const int statusWidth = std::max<int>(metrics.Scale(120), area.right - x - gap);
  Place(status_, Box(x + gap, labelTop, statusWidth, metrics.LabelHeight()));
}

}  // namespace gc::ui
