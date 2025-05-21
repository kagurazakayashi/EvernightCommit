#include "ui/changes_pane.h"

#include <commctrl.h>

#include <algorithm>
#include <string>

#include "git/commit_history.h"
#include "ui/commands.h"
#include "ui/controls.h"

namespace gc::ui {
namespace {

void Place(HWND target, const RECT& rect) { PlaceIfChanged(target, rect); }

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

int ChangesPane::ButtonStackHeight(const UiMetrics& metrics) noexcept {
  // 四行按钮 + 三个行距 + 一个组间隔（暂存那两个与导航那两个之间留一组，看得出是两件事）。
  return 4 * metrics.ControlHeight() + 3 * metrics.RowGap() + metrics.RowGap() * 2;
}

int ChangesPane::MinimumHeight(const UiMetrics& metrics) noexcept {
  const int listNeed = GroupCaption(metrics) + ListHeaderHeight(metrics) + HintTextHeight(metrics) +
                       HintVerticalPadding(metrics) + InnerInset(metrics);
  // 中间那一栏的四个按钮是同一栏里的竖排：窗口缩到最小时，按钮列比列表列更需要这点高度，
  // 否则最后那一个按钮会被摆到表单那一带上去。
  return std::max(listNeed, ButtonStackHeight(metrics));
}

void ChangesPane::Create(HWND parent) {
  unstagedGroup_ = CreateGroupBox(parent, L"未暂存的更改", kIdUnstagedGroup);
  unstagedList_ = CreateReportListView(parent, kIdUnstagedList, changeColumns_);
  unstagedHint_ = CreateCenteredLabel(unstagedList_, L"", kIdUnstagedHint);

  stageAdd_ = CreatePushButton(parent, L"加入暂存区 →", kIdStageAddButton);
  stageRemove_ = CreatePushButton(parent, L"← 移出暂存区", kIdStageRemoveButton);
  enterSubmodule_ = CreatePushButton(parent, L"进入子模块", kIdEnterSubmoduleButton);
  returnToParent_ = CreatePushButton(parent, L"返回父仓库", kIdReturnToParentButton);

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
  // 四行：加入暂存区 / 移出暂存区 /（一组间隔）/ 进入子模块 / 返回父仓库。
  // 间隔让「动索引的那两个」与「只换绑定仓库的那两个」在同一栏里也看得出是两类事。
  const int stackHeight = ChangesPane::ButtonStackHeight(metrics);
  int top = columns.arrows.top + ((columns.arrows.bottom - columns.arrows.top) - stackHeight) / 2;
  top = std::max<int>(columns.arrows.top, top);

  const int addWidth = std::min(arrowWidth, metrics.ButtonWidth(L"加入暂存区 →"));
  const int removeWidth = std::min(arrowWidth, metrics.ButtonWidth(L"← 移出暂存区"));
  const int enterWidth = std::min(arrowWidth, metrics.ButtonWidth(L"进入子模块"));
  const int returnWidth = std::min(arrowWidth, metrics.ButtonWidth(L"返回父仓库"));
  Place(stageAdd_, Box(columns.arrows.left + (arrowWidth - addWidth) / 2, top, addWidth, rowHeight));
  top += rowHeight + gap;
  Place(stageRemove_,
        Box(columns.arrows.left + (arrowWidth - removeWidth) / 2, top, removeWidth, rowHeight));
  top += rowHeight + gap * 3;  // 组间隔（与 ButtonStackHeight 里多留的那两行距一致）
  Place(enterSubmodule_,
        Box(columns.arrows.left + (arrowWidth - enterWidth) / 2, top, enterWidth, rowHeight));
  top += rowHeight + gap;
  Place(returnToParent_,
        Box(columns.arrows.left + (arrowWidth - returnWidth) / 2, top, returnWidth, rowHeight));
}

template <typename Item>
app::ListViewMemory ChangesPane::CaptureMemory(HWND list, const std::vector<Item>& shown) {
  app::ListViewMemory memory;
  for (const int row : GetListSelectedRows(list)) {
    if (row >= 0 && static_cast<size_t>(row) < shown.size()) {
      memory.selectedKeys.push_back(app::ViewKeyForItem(shown[static_cast<size_t>(row)]));
    }
  }
  return memory;
}

std::vector<std::wstring> ChangesPane::CellsFor(const git::ChangeItem& item) {
  return {item.StatusLabel(), item.PathLabel()};
}

std::vector<std::wstring> ChangesPane::CellsFor(const git::CommitItem& item) {
  // 提交列展示短 ID（只是前缀，供阅读），一切命令都用条目里的完整对象 ID。
  return {git::ShortObjectId(item.objectId), item.summary, item.author, item.authoredAt};
}

template <typename Item>
void ChangesPane::RebuildList(HWND list, HWND hint, const std::vector<Item>& shown,
                              const app::ListViewMemory& memory, const std::vector<Item>& items,
                              const std::wstring& hintText) {
  // 按差异就地更新，不整列清空：报表视图不响应 LVM_SCROLL，一旦清空重建，
  // 用户正在看的滚动位置就再也放不回去，只剩顶部一个选择。
  // 行与条目的对应关系由下面这份 keys 维护，全程不从单元格文本反查。
  const ListRedrawPause pause(list);  // 中间态不绘制，更新完一次性重绘，避免闪一下。
  std::vector<std::wstring> keys;
  keys.reserve(shown.size());
  for (const Item& item : shown) {
    keys.push_back(app::ViewKeyForItem(item));
  }
  std::vector<std::wstring> wanted;
  wanted.reserve(items.size());
  for (const Item& item : items) {
    wanted.push_back(app::ViewKeyForItem(item));
  }

  // 先认头部与尾部的公共段：绝大多数刷新只是中间有一两条进出，两端根本不用动。
  size_t head = 0;
  while (head < keys.size() && head < wanted.size() && keys[head] == wanted[head]) {
    ++head;
  }
  size_t oldTail = keys.size();
  size_t newTail = wanted.size();
  while (oldTail > head && newTail > head && keys[oldTail - 1] == wanted[newTail - 1]) {
    --oldTail;
    --newTail;
  }
  // 中段：旧的多余行自后往前删掉（删除不会让前面已算好的行号失准）。
  for (int row = static_cast<int>(oldTail) - 1; row >= static_cast<int>(head); --row) {
    RemoveListRow(list, row);
  }
  keys.erase(keys.begin() + static_cast<ptrdiff_t>(head), keys.begin() + static_cast<ptrdiff_t>(oldTail));
  // 中段：新的条目按顺序插在各自的位置上。
  for (size_t index = head; index < newTail; ++index) {
    InsertListRow(list, static_cast<int>(index), CellsFor(items[index]));
    keys.insert(keys.begin() + static_cast<ptrdiff_t>(index), wanted[index]);
  }
  // 结构对齐以后，逐行刷新显示文本：同一条目的内容可能已经变了（状态字改变、时间重算）。
  for (size_t index = 0; index < items.size(); ++index) {
    SetListRowCells(list, static_cast<int>(index), CellsFor(items[index]));
  }
  // 选中项按条目身份恢复：行可能已经上移、下移或整条消失，消失的那条不会凭空选到别人。
  RestoreListSelection(list, app::MapListViewMemory(items, memory).selectedRows);

  // 空状态说明只在确实没有条目时占用列表区域；有条目时隐藏，避免叠在行上。
  SetControlText(hint, hintText);
  ::ShowWindow(hint, items.empty() ? SW_SHOW : SW_HIDE);
}

void ChangesPane::ShowWorkspace(const git::WorkspaceModel& model, const git::EmptyStateTexts& texts) {

  // 行内容与模型下标一一对应：单元格列都只是显示文本，
  // 后续步骤按行号回到模型取原始路径/对象 ID 与状态，绝不从单元格文字反解 Git 命令参数。
  // 记忆必须在改动行之前取：行号一旦移动，原来的选择就对不上条目了。
  const app::ListViewMemory unstagedMemory = CaptureMemory(unstagedList_, shown_.unstaged);
  const app::ListViewMemory stagedMemory = CaptureMemory(stagedList_, shown_.staged);
  const app::ListViewMemory historyMemory = CaptureMemory(historyList_, shown_.recentCommits);

  RebuildList(unstagedList_, unstagedHint_, shown_.unstaged, unstagedMemory, model.unstaged, texts.unstaged);
  RebuildList(stagedList_, stagedHint_, shown_.staged, stagedMemory, model.staged, texts.staged);
  // 提交历史同一套就地更新：刷新后仍存在的提交保持选中，新提交插入到应有的位置。
  RebuildList(historyList_, historyHint_, shown_.recentCommits, historyMemory, model.recentCommits,
              texts.history);
  shown_ = model;
}

const git::ChangeItem* ChangesPane::ItemAt(HWND list, int row, git::ChangeSide* side) const {
  const std::vector<git::ChangeItem>* items = nullptr;
  if (list == unstagedList_) {
    items = &shown_.unstaged;
    if (side != nullptr) {
      *side = git::ChangeSide::unstaged;
    }
  } else if (list == stagedList_) {
    items = &shown_.staged;
    if (side != nullptr) {
      *side = git::ChangeSide::staged;
    }
  } else {
    return nullptr;  // 提交历史或陌生句柄：历史条目经 CommitItemAt 按行号取，类型不同不从这里走。
  }
  if (row < 0 || static_cast<size_t>(row) >= items->size()) {
    return nullptr;  // 行已不在当前显示内容里（外部改动后被刷新移除）。
  }
  return &(*items)[static_cast<size_t>(row)];
}

const git::CommitItem* ChangesPane::CommitItemAt(int row) const {
  if (row < 0 || static_cast<size_t>(row) >= shown_.recentCommits.size()) {
    return nullptr;  // 与 ItemAt 同一约定：行号只对最后一次落地的内容有效。
  }
  return &shown_.recentCommits[static_cast<size_t>(row)];
}

int ChangesPane::ListRowCount(HWND list) const {
  if (list != unstagedList_ && list != stagedList_ && list != historyList_) {
    return -1;
  }
  return GetListItemCount(list);
}

std::vector<int> ChangesPane::SelectedUnstagedRows() const { return GetListSelectedRows(unstagedList_); }

std::vector<int> ChangesPane::SelectedStagedRows() const { return GetListSelectedRows(stagedList_); }

}  // namespace gc::ui
