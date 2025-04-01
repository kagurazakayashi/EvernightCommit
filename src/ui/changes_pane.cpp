#include "ui/changes_pane.h"

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

}  // namespace

// 分组标题 + 列表表头 + 两行空状态说明 + 边框与留白。与 LayoutList 共用同一组函数，
// 保证窗口缩到最小尺寸时空状态说明仍完整可见。
int ChangesPane::GroupCaption(const UiMetrics& metrics) noexcept { return metrics.LabelHeight() + metrics.Scale(5); }
int ChangesPane::ListHeaderHeight(const UiMetrics& metrics) noexcept { return metrics.LabelHeight() + metrics.Scale(10); }
int ChangesPane::HintTextHeight(const UiMetrics& metrics) noexcept { return 2 * metrics.LabelHeight(); }
int ChangesPane::HintVerticalPadding(const UiMetrics& metrics) noexcept { return metrics.Scale(16); }
int ChangesPane::InnerInset(const UiMetrics& metrics) noexcept { return metrics.Scale(7); }

int ChangesPane::MinimumHeight(const UiMetrics& metrics) noexcept {
  return GroupCaption(metrics) + ListHeaderHeight(metrics) + HintTextHeight(metrics) +
         HintVerticalPadding(metrics) + InnerInset(metrics);
}

void ChangesPane::Create(HWND parent) {
  unstagedGroup_ = CreateGroupBox(parent, L"未暂存的更改", kIdUnstagedGroup);
  unstagedList_ = CreateReportListView(parent, kIdUnstagedList, changeColumns_);
  unstagedHint_ = CreateCenteredLabel(unstagedList_, L"", kIdUnstagedHint);

  stageAdd_ = CreatePushButton(parent, L"加入暂存区 →", kIdStageAddButton);
  stageRemove_ = CreatePushButton(parent, L"← 移出暂存区", kIdStageRemoveButton);

  stagedGroup_ = CreateGroupBox(parent, L"已暂存的更改", kIdStagedGroup);
  stagedList_ = CreateReportListView(parent, kIdStagedList, changeColumns_);
  stagedHint_ = CreateCenteredLabel(stagedList_, L"", kIdStagedHint);

  historyGroup_ = CreateGroupBox(parent, L"最近提交", kIdHistoryGroup);
  historyList_ = CreateReportListView(parent, kIdHistoryList, commitColumns_);
  historyHint_ = CreateCenteredLabel(historyList_, L"", kIdHistoryHint);
}

RECT ChangesPane::InnerRect(const RECT& group, const UiMetrics& metrics) noexcept {
  const int inset = InnerInset(metrics);
  return RECT{group.left + inset, group.top + GroupCaption(metrics), group.right - inset, group.bottom - inset};
}

void ChangesPane::LayoutList(HWND group, HWND list, HWND hint, const RECT& bounds, const UiMetrics& metrics,
                             const std::vector<ListColumn>& columns) {
  Place(group, bounds);
  const RECT inner = InnerRect(bounds, metrics);
  RECT listBefore{};
  ::GetWindowRect(list, &listBefore);   // 必须在移动列表之前取旧位置
  Place(list, inner);
  const int listWidth = inner.right - inner.left;
  ApplyListColumnWidths(list, columns, listWidth);

  // 表头高度按实际控件测量，提示文字在其下方居中，避免与表头或滚动条重叠。
  RECT area{};
  ::GetClientRect(list, &area);
  int headerBottom = ListHeaderHeight(metrics);
  auto header = reinterpret_cast<HWND>(::SendMessageW(list, LVM_GETHEADER, 0, 0));
  if (header != nullptr) {
    RECT headerRect{};
    ::GetWindowRect(header, &headerRect);
    POINT topLeft{headerRect.left, headerRect.top};
    ::ScreenToClient(list, &topLeft);
    RECT headerClient{};
    ::GetClientRect(header, &headerClient);
    headerBottom = topLeft.y + (headerClient.bottom - headerClient.top) + metrics.Scale(2);
  }

  // 提示文字在表头以下的可用区域内居中；列表尺寸变化时整块重绘，
  // 因为 ListView 不会自动擦除子控件旧位置，否则缩放时会留下文字重影。
  const int textHeight = HintTextHeight(metrics);
  const int available = static_cast<int>(area.bottom) - headerBottom - metrics.Scale(4);
  const int top = headerBottom + std::max(0, (available - textHeight) / 2);
  const RECT hintRect{metrics.Scale(3), top, static_cast<int>(area.right) - metrics.Scale(3),
                      top + textHeight + metrics.Scale(2)};
  Place(hint, hintRect);
  if (listBefore.right - listBefore.left != inner.right - inner.left ||
      listBefore.bottom - listBefore.top != inner.bottom - inner.top) {
    ::InvalidateRect(list, nullptr, TRUE);
  }
}

void ChangesPane::Layout(const ChangesColumns& columns, const UiMetrics& metrics) {
  LayoutList(unstagedGroup_, unstagedList_, unstagedHint_, columns.unstaged, metrics, changeColumns_);
  LayoutList(stagedGroup_, stagedList_, stagedHint_, columns.staged, metrics, changeColumns_);
  LayoutList(historyGroup_, historyList_, historyHint_, columns.history, metrics, commitColumns_);

  const int gap = metrics.RowGap();
  const int rowHeight = metrics.ControlHeight();
  const int arrowWidth = columns.arrows.right - columns.arrows.left;
  const int stackHeight = 2 * rowHeight + gap;
  int top = columns.arrows.top + ((columns.arrows.bottom - columns.arrows.top) - stackHeight) / 2;
  top = std::max<int>(columns.arrows.top, top);

  const int addWidth = std::min(arrowWidth, metrics.ButtonWidth(L"加入暂存区 →"));
  const int removeWidth = std::min(arrowWidth, metrics.ButtonWidth(L"← 移出暂存区"));
  Place(stageAdd_, Box(columns.arrows.left + (arrowWidth - addWidth) / 2, top, addWidth, rowHeight));
  top += rowHeight + gap;
  Place(stageRemove_, Box(columns.arrows.left + (arrowWidth - removeWidth) / 2, top, removeWidth, rowHeight));
}

void ChangesPane::ShowWorkspace(const git::WorkspaceModel& model, const git::EmptyStateTexts& texts) {
  // 行内容与模型下标一一对应：状态列与路径列都只是显示文本，
  // 后续步骤按行号回到模型取原始路径与状态，绝不从单元格文字反解 Git 命令参数。
  const auto apply = [](HWND list, HWND hint, const std::vector<git::ChangeItem>& items,
                        const std::wstring& text) {
    ClearListItems(list);
    for (const git::ChangeItem& item : items) {
      AddListRow(list, {item.StatusLabel(), item.PathLabel()});
    }
    SetControlText(hint, text);
    ::ShowWindow(hint, items.empty() ? SW_SHOW : SW_HIDE);
  };
  apply(unstagedList_, unstagedHint_, model.unstaged, texts.unstaged);
  apply(stagedList_, stagedHint_, model.staged, texts.staged);

  // 提交历史仍属后续步骤：git log 未接入，这里只保留说明，不清成“没有提交”。
  ClearListItems(historyList_);
  SetControlText(historyHint_, texts.history);
  ::ShowWindow(historyHint_, model.recentCommits.empty() ? SW_SHOW : SW_HIDE);
}

}  // namespace gc::ui
