#pragma once

#include <windows.h>

#include <vector>

#include "app/list_view_memory.h"
#include "git/workspace_model.h"
#include "ui/controls.h"
#include "ui/layout.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 中部三块列表：未暂存的更改 / 已暂存的更改 / 最近提交，以及中间的移动文件按钮。
class ChangesPane {
public:
  void Create(HWND parent);
  void Layout(const ChangesColumns& columns, const UiMetrics& metrics);
  // 模型为空时显示空状态说明；列表本身保持可见以保留表头。
  // 重建整列会丢失选中行与滚动位置，因此这里按条目身份（相对路径）记忆并在重建后恢复：
  // 刷新只是让内容变新，不该把用户已经选好的文件清空。
  void ShowWorkspace(const git::WorkspaceModel& model, const git::EmptyStateTexts& texts);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;

  [[nodiscard]] HWND stageAddButton() const noexcept { return stageAdd_; }
  [[nodiscard]] HWND stageRemoveButton() const noexcept { return stageRemove_; }
  // 两个更改列表本身：主窗口只取句柄挂说明性工具提示，不直接操作条目内容。
  [[nodiscard]] HWND unstagedList() const noexcept { return unstagedList_; }
  [[nodiscard]] HWND stagedList() const noexcept { return stagedList_; }

private:
  [[nodiscard]] static RECT InnerRect(const RECT& group, const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int GroupCaption(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int ListHeaderHeight(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int HintTextHeight(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int HintVerticalPadding(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int InnerInset(const UiMetrics& metrics) noexcept;
  void LayoutList(HWND group, HWND list, HWND hint, const RECT& bounds, const UiMetrics& metrics,
                  const std::vector<ListColumn>& columns);
  // 取出当前列表的视图状态（行号按上一次显示的模型换算成条目身份）。
  [[nodiscard]] static app::ListViewMemory CaptureMemory(HWND list,
                                                         const std::vector<git::ChangeItem>& shown);
  // 按差异就地更新一列：删掉已不存在的行、就地改文本、需要时插入新行，绝不整列清空。
  // 清空重建会把报表视图的滚动位置退回顶部（LVM_SCROLL 在报表视图无效，救不回来）。
  static void RebuildList(HWND list, HWND hint, const std::vector<git::ChangeItem>& shown,
                          const app::ListViewMemory& memory, const std::vector<git::ChangeItem>& items,
                          const std::wstring& hintText);

  HWND unstagedGroup_ = nullptr;
  HWND unstagedList_ = nullptr;
  HWND unstagedHint_ = nullptr;
  HWND stagedGroup_ = nullptr;
  HWND stagedList_ = nullptr;
  HWND stagedHint_ = nullptr;
  HWND historyGroup_ = nullptr;
  HWND historyList_ = nullptr;
  HWND historyHint_ = nullptr;
  HWND stageAdd_ = nullptr;
  HWND stageRemove_ = nullptr;

  std::vector<ListColumn> changeColumns_{{L"状态", 20}, {L"相对路径", 80}};
  std::vector<ListColumn> commitColumns_{{L"提交", 16}, {L"标题", 44}, {L"作者", 22}, {L"时间", 18}};
  // 上一次显示到列表里的内容：行号与条目身份（相对路径）的对应关系只有在这里才成立，
  // 因此记忆选中项时必须用它换算，而不是去读单元格里拼好的显示文本。
  git::WorkspaceModel shown_;
};

}  // namespace gc::ui
