#include "ui/commit_form.h"

#include <commctrl.h>

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

const std::vector<ListColumn>& CoauthorColumns() {
  static const std::vector<ListColumn> kColumns{{L"合作者", 100}};
  return kColumns;
}

int LabelColumnWidth(const UiMetrics& metrics) {
  int width = 0;
  for (std::wstring_view label :
       {L"标题：", L"描述：", L"作者：", L"合作者：", L"作者时间：", L"提交者时间："}) {
    width = std::max(width, metrics.LabelWidth(label));
  }
  return width;
}

void SetDate(HWND picker, const SYSTEMTIME& time) {
  ::SendMessageW(picker, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&time));
}

}  // namespace

int CommitForm::MinDescriptionHeight(const UiMetrics& metrics) noexcept {
  return metrics.ControlHeight() + metrics.Scale(14);
}

int CommitForm::MinCoauthorHeight(const UiMetrics& metrics) noexcept {
  return 2 * metrics.ControlHeight();
}

int CommitForm::MinimumHeight(const UiMetrics& metrics) noexcept {
  const int fixedRows = 4 * metrics.ControlHeight();  // 标题、作者、作者时间、提交者时间
  return metrics.LabelHeight() + metrics.Scale(8) + fixedRows + MinDescriptionHeight(metrics) +
         MinCoauthorHeight(metrics) + 5 * metrics.RowGap();
}

void CommitForm::Create(HWND parent) {
  group_ = CreateGroupBox(parent, L"提交信息", kIdCommitGroup);

  // 分组框之后的控件在 z 序上位于其上方，因此用父窗口坐标摆放即可，且不会被分组框背景擦除。
  summaryLabel_ = CreateLabel(parent, L"标题：", kIdSummaryLabel);
  summary_ = CreateSingleLineEdit(parent, L"", kIdSummaryEdit);
  descriptionLabel_ = CreateLabel(parent, L"描述：", kIdDescriptionLabel);
  description_ = CreateMultilineEdit(parent, L"", kIdDescriptionEdit);
  authorLabel_ = CreateLabel(parent, L"作者：", kIdAuthorLabel);
  author_ = CreateSingleLineEdit(parent, L"", kIdAuthorEdit);

  coauthorLabel_ = CreateLabel(parent, L"合作者：", kIdCoauthorLabel);
  coauthorList_ = CreateReportListView(parent, kIdCoauthorList, CoauthorColumns());
  coauthorHint_ = CreateCenteredLabel(coauthorList_, L"暂无合作者。", kIdCoauthorHint);
  coauthorAdd_ = CreatePushButton(parent, L"添加", kIdCoauthorAdd);
  coauthorRemove_ = CreatePushButton(parent, L"删除", kIdCoauthorRemove);

  authorTimeLabel_ = CreateLabel(parent, L"作者时间：", kIdAuthorTimeLabel);
  authorDate_ = CreateDateTimePicker(parent, kIdAuthorDate, false);
  authorClock_ = CreateDateTimePicker(parent, kIdAuthorClock, true);
  timeZone_ = CreateLabel(parent, L"本机时区：", kIdTimeZoneLabel);

  committerTimeLabel_ = CreateLabel(parent, L"提交者时间：", kIdCommitterTimeLabel);
  committerDate_ = CreateDateTimePicker(parent, kIdCommitterDate, false);
  committerClock_ = CreateDateTimePicker(parent, kIdCommitterClock, true);
  timeSync_ = CreateCheckBox(parent, L"时间同步修改", kIdTimeSyncCheck, true);
}

void CommitForm::SetTimeZoneText(std::wstring_view text) {
  SetControlText(timeZone_, text);
}

void CommitForm::SetTimes(const SYSTEMTIME& authorTime, const SYSTEMTIME& committerTime) {
  SetDate(authorDate_, authorTime);
  SetDate(authorClock_, authorTime);
  SetDate(committerDate_, committerTime);
  SetDate(committerClock_, committerTime);
}

void CommitForm::Layout(const RECT& area, const UiMetrics& metrics) {
  Place(group_, area);

  const int inset = metrics.Scale(9);
  const int caption = metrics.LabelHeight() + metrics.Scale(4);
  const int gap = metrics.RowGap();
  const int rowHeight = metrics.ControlHeight();
  const int labelWidth = LabelColumnWidth(metrics);
  const int left = area.left + inset;
  const int right = area.right - inset;
  const int contentLeft = left + labelWidth + metrics.ColGap();
  const int contentWidth = std::max(metrics.Scale(80), right - contentLeft);
  const int labelTopOffset = (rowHeight - metrics.LabelHeight()) / 2;

  int y = area.top + caption;

  Place(summaryLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(summary_, Box(contentLeft, y, contentWidth, rowHeight));
  y += rowHeight + gap;

  const int bottomLimit = area.bottom - inset;
  const int remaining = bottomLimit - y;
  // 固定行：作者、作者时间、提交者时间，以及它们各自的间隔。
  const int fixedRows = 3 * rowHeight + 3 * gap;
  const int flexible = std::max(0, remaining - fixedRows);
  const int minimumDescription = MinDescriptionHeight(metrics);
  const int minimumCoauthor = MinCoauthorHeight(metrics);
  int descriptionHeight = (flexible * 45) / 100;
  descriptionHeight = std::clamp(descriptionHeight, minimumDescription,
                                 std::max(minimumDescription, flexible - minimumCoauthor));
  const int coauthorHeight = std::max(minimumCoauthor, flexible - descriptionHeight);

  Place(descriptionLabel_, Box(left, y + (descriptionHeight - metrics.LabelHeight()) / 2, labelWidth,
                               metrics.LabelHeight()));
  Place(description_, Box(contentLeft, y, contentWidth, descriptionHeight));
  y += descriptionHeight + gap;

  Place(authorLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(author_, Box(contentLeft, y, contentWidth, rowHeight));
  y += rowHeight + gap;

  const int buttonWidth = metrics.ButtonWidth(L"添加合作者");
  const int listWidth = std::max(metrics.Scale(90), contentWidth - 2 * (buttonWidth + metrics.ColGap()));
  Place(coauthorLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(coauthorList_, Box(contentLeft, y, listWidth, coauthorHeight));
  ApplyListColumnWidths(coauthorList_, CoauthorColumns(), listWidth);
  const int hintTop = metrics.LabelHeight() + metrics.Scale(4);
  const RECT hint{metrics.Scale(3), hintTop, std::max(metrics.Scale(4), listWidth - metrics.Scale(6)),
                  std::max(hintTop + 1, coauthorHeight - metrics.Scale(6))};
  Place(coauthorHint_, hint);
  Place(coauthorAdd_, Box(contentLeft + listWidth + metrics.ColGap(), y, buttonWidth, rowHeight));
  Place(coauthorRemove_, Box(contentLeft + listWidth + 2 * metrics.ColGap() + buttonWidth, y, buttonWidth, rowHeight));
  y += coauthorHeight + gap;

  const int dateWidth = metrics.Scale(124);
  const int clockWidth = metrics.Scale(96);
  const int pickerLeft = contentLeft + dateWidth + clockWidth + 2 * metrics.ColGap();

  Place(authorTimeLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(authorDate_, Box(contentLeft, y, dateWidth, rowHeight));
  Place(authorClock_, Box(contentLeft + dateWidth + metrics.ColGap(), y, clockWidth, rowHeight));
  const int timeZoneWidth = std::max(metrics.Scale(60), right - pickerLeft);
  Place(timeZone_, Box(pickerLeft, y + labelTopOffset, timeZoneWidth, metrics.LabelHeight()));
  y += rowHeight + gap;

  Place(committerTimeLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(committerDate_, Box(contentLeft, y, dateWidth, rowHeight));
  Place(committerClock_, Box(contentLeft + dateWidth + metrics.ColGap(), y, clockWidth, rowHeight));
  const int syncWidth = std::min(std::max(metrics.Scale(60), right - pickerLeft),
                                 metrics.LabelWidth(L"时间同步修改") + metrics.Scale(22));
  Place(timeSync_, Box(pickerLeft, y + labelTopOffset, syncWidth, rowHeight));
}

}  // namespace gc::ui
