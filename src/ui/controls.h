#pragma once

#include <windows.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace gc::ui {

struct ListColumn {
  std::wstring title;
  int percent = 50;  // 该列在列表宽度中所占百分比，随窗口缩放
};

HWND CreateLabel(HWND parent, std::wstring_view text, int id);
HWND CreateCenteredLabel(HWND parent, std::wstring_view text, int id);
HWND CreateGroupBox(HWND parent, std::wstring_view text, int id);
HWND CreateSingleLineEdit(HWND parent, std::wstring_view text, int id);
HWND CreateMultilineEdit(HWND parent, std::wstring_view text, int id);
HWND CreatePushButton(HWND parent, std::wstring_view text, int id);
HWND CreateCheckBox(HWND parent, std::wstring_view text, int id, bool checked);
// 可编辑组合框：既能在下拉列表中挑选候选项，也能直接键入文本。
HWND CreateEditableCombo(HWND parent, int id);
// 重建下拉候选列表并保持编辑框文本不变；候选项按插入顺序显示。
void SetComboCandidates(HWND combo, const std::vector<std::wstring>& items);
HWND CreateReportListView(HWND parent, int id, const std::vector<ListColumn>& columns);
// timeOnly=false 时显示可键盘输入的完整日期，true 时显示时/分/秒。
HWND CreateDateTimePicker(HWND parent, int id, bool timeOnly);

void SetControlText(HWND target, std::wstring_view text);
[[nodiscard]] std::wstring GetControlText(HWND target);

// 列表列宽按百分比在每次布局时重算，保证缩放时不出现横向截断。
void ApplyListColumnWidths(HWND list, const std::vector<ListColumn>& columns, int width);

void ApplyFontToChildTree(HWND parent, HFONT font);

// 为按钮提供“该操作尚未实现”之类的说明；提示文本由本对象持有，控件销毁前保持有效。
class ToolTips {
public:
  ToolTips() = default;
  ToolTips(const ToolTips&) = delete;
  ToolTips& operator=(const ToolTips&) = delete;
  ~ToolTips();

  bool Create(HWND owner, HFONT font);
  void Add(HWND target, std::wstring_view text);
  [[nodiscard]] HWND handle() const noexcept { return window_; }

private:
  HWND window_ = nullptr;
  std::vector<std::wstring> texts_;
};

}  // namespace gc::ui
