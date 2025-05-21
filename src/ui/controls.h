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

// 摆放子控件：位置与尺寸已经一致时什么都不做。
// 布局函数会被状态刷新反复调用，而无条件的 MoveWindow 会让控件连同父窗口区域一起重绘——
// 文本没变、格子也没动的时候，这份重绘就是用户眼里的闪烁；组合框正在展开的下拉列表更会被直接收起。
void PlaceIfChanged(HWND target, const RECT& rect);

// 报表视图的行读写：行内容一律由调用方给出「单元格文本」，界面不从单元格文本反解操作参数。
// 整列作废只用于“内容确实要全部丢掉”的场合（例如重建合作者清单）。
void ClearListItems(HWND list);
// 就地改行 / 在指定位置插行 / 删行。刷新要保住用户正在看的滚动位置时必须按差异就地更新：
// 报表视图不响应 LVM_SCROLL，整列清空重建之后没有任何消息能把视口放回原处。
void InsertListRow(HWND list, int row, const std::vector<std::wstring>& cells);
void SetListRowCells(HWND list, int row, const std::vector<std::wstring>& cells);
void RemoveListRow(HWND list, int row);
[[nodiscard]] int GetListItemCount(HWND list);

// 视图状态：刷新会就地增删行，行号可能对应到别的条目，因此取的「选中行号」
// 只在更新前短暂有效，调用方必须把它们换算成按条目身份的记号再恢复。
[[nodiscard]] std::vector<int> GetListSelectedRows(HWND list);
// 选中项恢复：只改 LVIS_SELECTED 状态位，不动焦点也不动视口。
// 未列出的行一律取消选中；列表为空时什么都不做。
void RestoreListSelection(HWND list, const std::vector<int>& rows);
// 重建整列期间的重绘暂停：逐行插入的中间状态如果被绘制出来就是可感知的闪烁。
// 作用域结束时一律恢复重绘并整块失效——本类只按「一层作用域」使用，不做嵌套。
class ListRedrawPause {
public:
  explicit ListRedrawPause(HWND list);
  ListRedrawPause(const ListRedrawPause&) = delete;
  ListRedrawPause& operator=(const ListRedrawPause&) = delete;
  ~ListRedrawPause();

private:
  HWND list_ = nullptr;
  bool paused_ = false;
};

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
