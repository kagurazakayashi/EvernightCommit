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

// 程序写时间控件期间的「这不是用户改的」标记，出作用域自动复位。
class ProgrammaticTimeWrite {
public:
  explicit ProgrammaticTimeWrite(bool& flag) : flag_(flag) { flag_ = true; }
  ProgrammaticTimeWrite(const ProgrammaticTimeWrite&) = delete;
  ProgrammaticTimeWrite& operator=(const ProgrammaticTimeWrite&) = delete;
  ~ProgrammaticTimeWrite() { flag_ = false; }

private:
  bool& flag_;
};

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

// 日期与时间的可选界：与 git/commit_date 的年份界同一个来源。
// 不设这两块控件时，用户能敲到 1601 或 30827 这类 Git 根本收不下的年份；
// 控件里就拦掉，比提交时再拒绝友好得多。
void ApplyDateRange(HWND picker) {
  SYSTEMTIME bounds[2]{};
  bounds[0] = {};
  bounds[0].wYear = static_cast<WORD>(git::kCommitYearLowerBound);
  bounds[0].wMonth = 1;
  bounds[0].wDay = 1;
  bounds[1] = {};
  bounds[1].wYear = static_cast<WORD>(git::kCommitYearUpperBound);
  bounds[1].wMonth = 12;
  bounds[1].wDay = 31;
  ::SendMessageW(picker, DTM_SETRANGE, 0, reinterpret_cast<LPARAM>(bounds));
}

// 把「日期控件 + 时分秒控件」拼成一段墙上时间：日期取前者、时分秒取后者。
// 时间型控件内部也存着日期，但那是用户看不见的一半，不能拿它当真。
git::CivilTime ReadCombinedWallTime(HWND datePicker, HWND clockPicker) {
  SYSTEMTIME dateValue{};
  SYSTEMTIME clockValue{};
  const LRESULT dateState =
      ::SendMessageW(datePicker, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&dateValue));
  const LRESULT clockState =
      ::SendMessageW(clockPicker, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&clockValue));
  if (dateState != GDT_VALID || clockState != GDT_VALID) {
    return git::CivilTime{};  // 控件没设时间（本程序没用 DTS_SHOWNONE，正常不会走到这里）。
  }
  git::CivilTime value;
  value.year = dateValue.wYear;
  value.month = dateValue.wMonth;
  value.day = dateValue.wDay;
  value.hour = clockValue.wHour;
  value.minute = clockValue.wMinute;
  value.second = clockValue.wSecond;
  return value;
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
  timeReset_ = CreatePushButton(parent, L"恢复当前时间", kIdTimeResetButton);

  // 四块控件的可选范围收到 Git 收得下的那一段年份里（见 git/commit_date 的实测边界）。
  for (HWND picker : {authorDate_, authorClock_, committerDate_, committerClock_}) {
    ApplyDateRange(picker);
  }
  // 「时间同步修改」默认勾上：提交者那两块控件此时是跟着作者时间走的，直接置灰，
  // 免得用户改了半天才发现那一半根本不作数。
  ApplyCommitterTimeEnabled(false);
}

void CommitForm::ApplyCommitterTimeEnabled(bool enabled) {
  for (HWND picker : {committerDate_, committerClock_}) {
    ::EnableWindow(picker, enabled ? TRUE : FALSE);
  }
}

void CommitForm::SetTimeZoneText(std::wstring_view text) {
  SetControlText(timeZone_, text);
}

void CommitForm::SetTimes(const SYSTEMTIME& authorTime, const SYSTEMTIME& committerTime) {
  // 程序写入同样会触发 DTN_DATETIMECHANGE：这一阵子里来的通知一律不算用户编辑。
  const ProgrammaticTimeWrite guard(settingTimes_);
  timesUserEdited_ = false;
  SetDate(authorDate_, authorTime);
  SetDate(authorClock_, authorTime);
  SetDate(committerDate_, committerTime);
  SetDate(committerClock_, committerTime);
}

void CommitForm::MirrorCommitterTime() {
  SYSTEMTIME authorValue{};
  if (::SendMessageW(authorDate_, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&authorValue)) !=
      GDT_VALID) {
    return;  // 作者那半边读不出值（本程序没用 DTS_SHOWNONE，正常不会走到这里）：不动提交者。
  }
  const ProgrammaticTimeWrite guard(settingTimes_);
  SetDate(committerDate_, authorValue);
  SetDate(committerClock_, authorValue);
}

git::CivilTime CommitForm::AuthorWallTime() const {
  return ReadCombinedWallTime(authorDate_, authorClock_);
}

git::CivilTime CommitForm::CommitterWallTime() const {
  return ReadCombinedWallTime(committerDate_, committerClock_);
}

bool CommitForm::TimeSyncChecked() const {
  return ::SendMessageW(timeSync_, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void CommitForm::SetTimeSyncChecked(bool checked) {
  ::SendMessageW(timeSync_, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

void CommitForm::SetCommitterTimeEnabled(bool enabled) { ApplyCommitterTimeEnabled(enabled); }

git::CommitFormData CommitForm::Capture() const {
  git::CommitFormData data;
  data.subject = GetControlText(summary_);
  data.description = GetControlText(description_);
  data.author = GetControlText(author_);
  // 合作者一律取内部模型，不从单元格文本反解：控件里的文字只是它的显示形态。
  data.coauthors = coauthors_;
  return data;
}

void CommitForm::SetSummaryText(std::wstring_view text) { SetControlText(summary_, text); }

void CommitForm::SetDescriptionText(std::wstring_view text) { SetControlText(description_, text); }

void CommitForm::SetAuthorText(std::wstring_view text) { SetControlText(author_, text); }

void CommitForm::ClearFields() {
  SetControlText(summary_, L"");
  SetControlText(description_, L"");
  SetControlText(author_, L"");
  SetCoauthors({});
}

void CommitForm::ClearMessageFields() {
  // 提交成功之后清掉「这次写了什么」，作者那一栏是可复用的身份，留着下次接着用。
  SetControlText(summary_, L"");
  SetControlText(description_, L"");
  SetCoauthors({});
}

void CommitForm::SetCoauthors(std::vector<std::wstring> entries) {
  coauthors_ = std::move(entries);
  RebuildCoauthorList();
}

std::vector<int> CommitForm::SelectedCoauthorRows() const { return GetListSelectedRows(coauthorList_); }

void CommitForm::SelectCoauthorRows(const std::vector<int>& rows) {
  RestoreListSelection(coauthorList_, rows);
}

void CommitForm::RebuildCoauthorList() {
  const ListRedrawPause pause(coauthorList_);
  ClearListItems(coauthorList_);
  int row = 0;
  for (const std::wstring& entry : coauthors_) {
    InsertListRow(coauthorList_, row, {entry});
    ++row;
  }
  // 「暂无合作者」只是占位说明：有条目时必须藏起来，否则提示会压在列表上。
  ::ShowWindow(coauthorHint_, coauthors_.empty() ? SW_SHOW : SW_HIDE);
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
  const int pickerEnd = contentLeft + dateWidth + clockWidth + 2 * metrics.ColGap();

  Place(authorTimeLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(authorDate_, Box(contentLeft, y, dateWidth, rowHeight));
  Place(authorClock_, Box(contentLeft + dateWidth + metrics.ColGap(), y, clockWidth, rowHeight));
  // 「恢复当前时间」靠右摆在本机时区那句说明之后：控件永远看得见，窗口极窄时被压缩的是
  // 一句说明文字（它只是提示，裁掉不影响操作），而不是按钮本身。
  const int resetWidth = metrics.ButtonWidth(L"恢复当前时间");
  const int resetLeft = std::max(pickerEnd, right - resetWidth);
  Place(timeReset_, Box(resetLeft, y, std::max(metrics.Scale(0), right - resetLeft), rowHeight));
  const int timeZoneWidth = std::max(metrics.Scale(60), resetLeft - metrics.ColGap() - pickerEnd);
  Place(timeZone_, Box(pickerEnd, y + labelTopOffset, timeZoneWidth, metrics.LabelHeight()));
  y += rowHeight + gap;

  Place(committerTimeLabel_, Box(left, y + labelTopOffset, labelWidth, metrics.LabelHeight()));
  Place(committerDate_, Box(contentLeft, y, dateWidth, rowHeight));
  Place(committerClock_, Box(contentLeft + dateWidth + metrics.ColGap(), y, clockWidth, rowHeight));
  const int syncWidth = std::min(std::max(metrics.Scale(60), right - pickerEnd),
                                 metrics.LabelWidth(L"时间同步修改") + metrics.Scale(22));
  Place(timeSync_, Box(pickerEnd, y + labelTopOffset, syncWidth, rowHeight));
}

}  // namespace gc::ui
