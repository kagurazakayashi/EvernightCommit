#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "app/list_view_memory.h"
#include "git/diff_view.h"
#include "git/workspace_model.h"
#include "ui/controls.h"
#include "ui/layout.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 变更区：一个选项卡容器，两个选项卡。
//   变化信息 —— 未暂存的更改 / 暂存方向的两个按钮 / 已暂存的更改；
//   提交历史 —— 最近提交 / 文件变化。
// 选项卡标签条由 SysTabControl32 画，页面内容仍是父窗口上的普通子控件：
// 列表句柄、选中行、工具提示都不跨页搬家，主窗口在 WM_NOTIFY 里仍按 idFrom 认领通知。
// 两个选项卡各自只有一条分隔条，因此分隔条由主窗口持有、按当前选项卡摆放那一条。
class ChangesPane {
public:
  void Create(HWND parent);
  // 摆放选项卡控件本身，返回两个选项卡共用的页面内容区（已扣掉标签条）。
  [[nodiscard]] RECT PlaceTabs(const RECT& bounds);
  // 按「变化信息」的栏位摆放该页控件，并隐去另一页的控件。
  void LayoutStaging(const StagingColumns& columns, const UiMetrics& metrics);
  // 按「提交历史」的栏位摆放该页控件，并隐去另一页的控件。
  void LayoutHistory(const HistoryColumns& columns, const UiMetrics& metrics);
  // 当前选中的选项卡：0 = 变化信息，1 = 提交历史；控件还没建好时返回 0。
  [[nodiscard]] int ActiveTab() const noexcept;

  // 「变化信息」页中间那一栏两个按钮竖排所需的高度。
  [[nodiscard]] static int ButtonStackHeight(const UiMetrics& metrics) noexcept;
  // 内容区（含标签条）不可压缩时所需的高度。
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;

  // 模型为空时显示空状态说明；列表本身保持可见以保留表头。
  // 重建整列会丢失选中行与滚动位置，因此这里按条目身份（相对路径）记忆并在重建后恢复：
  // 刷新只是让内容变新，不该把用户已经选好的文件清空。
  void ShowWorkspace(const git::WorkspaceModel& model, const git::EmptyStateTexts& texts);

  // 「提交历史」页右边那份「文件变化」清单的落点。
  struct CommitFilesView {
    // 这份清单属于哪条提交。nullptr = 还没选中任何提交（列表给出引导语）。
    const git::CommitItem* commit = nullptr;
    bool loading = false;  // 后台读取进行中（changes 必为空）
    std::wstring error;    // 非空 = 这次读取失败，note 是原因（changes 必为空）
    std::wstring note;     // 成功摘要 / 失败原因 / 引导语，进空状态说明
    std::vector<git::ChangeItem> changes;
  };
  // 落地一份「文件变化」视图。视图的 commit 与 changes 一起换掉：
  // 行号与条目的对应关系只在最后一次落地的内容上成立，因此双击必须同时经这里取回提交与文件。
  void ShowCommitFiles(const CommitFilesView& view);

  // 「文件变化」页现在展示的是哪条提交的清单（完整对象 ID）；没有时为空。
  [[nodiscard]] const std::wstring& CommitFilesCommitId() const noexcept { return filesCommitId_; }
  [[nodiscard]] bool CommitFilesLoading() const noexcept { return filesLoading_; }
  // 已落地的清单条数：与 ListRowCount(filesList()) 成对核对，两者不一致就不发任何命令。
  [[nodiscard]] int CommitFilesCount() const noexcept { return static_cast<int>(shownFiles_.size()); }

  // 列表正在就地重建（ShowWorkspace 内部）。主窗口收到重建中发出的选中项恢复通知时要忽略它：
  // 那些行号只是“插入/删除过程中的中间态”，按它去发后台查询会读到认不准的那一条；
  // 重建结束后由调用方按新列表重对一次（ReconcileCommitFilesSelection）。
  [[nodiscard]] bool UpdatingLists() const noexcept { return updatingLists_; }

  [[nodiscard]] HWND tabControl() const noexcept { return tabs_; }
  [[nodiscard]] HWND stageAddButton() const noexcept { return stageAdd_; }
  [[nodiscard]] HWND stageRemoveButton() const noexcept { return stageRemove_; }
  // 两个更改列表本身：主窗口只取句柄挂说明性工具提示，不直接操作条目内容。
  [[nodiscard]] HWND unstagedList() const noexcept { return unstagedList_; }
  [[nodiscard]] HWND stagedList() const noexcept { return stagedList_; }
  // 提交历史列表句柄：双击通知按 idFrom 认领，主窗口经 CommitItemAt 取条目。
  [[nodiscard]] HWND historyList() const noexcept { return historyList_; }
  // 「文件变化」列表句柄：选中一条提交后由主窗口在后台读取它的文件变化。
  [[nodiscard]] HWND filesList() const noexcept { return filesList_; }

  // 按列表句柄与行号取回"当前显示的那一条"条目，并给出它属于哪一侧。
  // 行号与条目的对应关系只在本面板最后一次 ShowWorkspace 落地的内容上成立，
  // 因此"双击某行去查看差异"这类按行发起的操作必须经这里取条目，
  // 绝不从单元格的显示文本反解路径（重命名行是"旧 → 新"，反解必然出错）。
  // 句柄不是两块更改列表之一、或行号越界时返回 nullptr。
  [[nodiscard]] const git::ChangeItem* ItemAt(HWND list, int row, git::ChangeSide* side) const;
  // 提交历史那一行的条目：同一套「只认最后落地内容」的约定，按行号取，越界返回 nullptr。
  [[nodiscard]] const git::CommitItem* CommitItemAt(int row) const;
  // 「文件变化」那一行：提交与文件一起取回，两者都只对最后一次 ShowCommitFiles 落地的内容成立。
  // 没有正在展示的提交、或行号越界时返回 false（两个出参都不动）。
  [[nodiscard]] bool CommitFileAt(int row, const git::CommitItem** commit,
                                  const git::ChangeItem** change) const;
  // 该列表当前的行数：用于核对"显示的行数"与"模型条数"是否还一致。
  [[nodiscard]] int ListRowCount(HWND list) const;
  // “未暂存的更改”里当前选中的行号（升序，可多选）。
  // 只给行号，不给条目：调用方必须逐行经 ItemAt 取条目，再与模型核对，
  // 这样“点击瞬间的选择范围”与“界面显示的那一条”是同一件事，不会拿到已被刷新换掉的行。
  [[nodiscard]] std::vector<int> SelectedUnstagedRows() const;
  // “已暂存的更改”里当前选中的行号，约定与上面完全相同（“← 移出暂存区”按这一侧发起）。
  [[nodiscard]] std::vector<int> SelectedStagedRows() const;
  // “最近提交”里当前选中的第一条行号；没有选中时返回 -1。
  // 只给行号：调用方必须经 CommitItemAt 取回条目并与模型核对，不从显示文本反解对象 ID。
  [[nodiscard]] int SelectedHistoryRow() const;

private:
  [[nodiscard]] static RECT InnerRect(const RECT& group, const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int GroupCaption(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int ListHeaderHeight(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int HintTextHeight(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int HintVerticalPadding(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int InnerInset(const UiMetrics& metrics) noexcept;
  void LayoutList(HWND group, HWND list, HWND hint, const RECT& bounds, const UiMetrics& metrics,
                  const std::vector<ListColumn>& columns);
  // 隐去/显示一页的全部控件。ShowWindow 在状态没变时是空操作，不会闪。
  static void ShowControls(bool visible, std::initializer_list<HWND> controls);
  // 取出当前列表的视图状态（行号按上一次显示的条目换算成条目身份；更改/历史列表同一条约定）。
  template <typename Item>
  [[nodiscard]] static app::ListViewMemory CaptureMemory(HWND list, const std::vector<Item>& shown);
  // 按差异就地更新一列：删掉已不存在的行、就地改文本、需要时插入新行，绝不整列清空。
  // 清空重建会把报表视图的滚动位置退回顶部（LVM_SCROLL 在报表视图无效，救不回来）。
  // 行与条目的对应关系由「条目身份」维护：更改列表按相对路径、历史列表按完整对象 ID，
  // 全程不从单元格的显示文本反解。
  template <typename Item>
  static void RebuildList(HWND list, HWND hint, const std::vector<Item>& shown,
                          const app::ListViewMemory& memory, const std::vector<Item>& items,
                          const std::wstring& hintText);
  // 一条条目对应的单元格文本（列数与 changeColumns_/commitColumns_ 一致）。
  [[nodiscard]] static std::vector<std::wstring> CellsFor(const git::ChangeItem& item);
  [[nodiscard]] static std::vector<std::wstring> CellsFor(const git::CommitItem& item);

  HWND tabs_ = nullptr;
  HWND unstagedGroup_ = nullptr;
  HWND unstagedList_ = nullptr;
  HWND unstagedHint_ = nullptr;
  HWND stagedGroup_ = nullptr;
  HWND stagedList_ = nullptr;
  HWND stagedHint_ = nullptr;
  HWND stageAdd_ = nullptr;
  HWND stageRemove_ = nullptr;
  HWND historyGroup_ = nullptr;
  HWND historyList_ = nullptr;
  HWND historyHint_ = nullptr;
  HWND filesGroup_ = nullptr;
  HWND filesList_ = nullptr;
  HWND filesHint_ = nullptr;

  std::vector<ListColumn> changeColumns_{{L"状态", 20}, {L"相对路径", 80}};
  std::vector<ListColumn> commitColumns_{{L"提交", 16}, {L"标题", 44}, {L"作者", 22}, {L"时间", 18}};
  // 上一次显示到列表里的内容：行号与条目身份（更改列表按相对路径、历史列表按完整对象 ID）
  // 的对应关系只有在这里才成立，因此记忆选中项时必须用它换算，
  // 而不是去读单元格里拼好的显示文本。
  git::WorkspaceModel shown_;
  // 「文件变化」页当前展示的那条提交与其清单：提交是完整的 CommitItem
  // （双击某一文件时要拿它的完整对象 ID 去构造 git show，绝不用「提交」列的短 ID）。
  git::CommitItem filesCommit_{};
  std::vector<git::ChangeItem> shownFiles_;
  std::wstring filesCommitId_;  // 空 = 还没有选中的提交（或清空过）
  std::wstring filesHintText_;  // 文件变化列表的空状态说明
  bool filesLoading_ = false;
  bool updatingLists_ = false;  // ShowWorkspace / ShowCommitFiles 运作期间为真
};

}  // namespace gc::ui
