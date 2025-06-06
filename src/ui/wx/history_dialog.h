#pragma once

#include <string>
#include <vector>

#include <wx/dialog.h>

#include "app/operation_history.h"

class wxButton;
class wxListCtrl;
class wxListEvent;
class wxTextCtrl;

namespace gc::ui::wx {

enum class HistoryDialogAction {
  none = 0,
  restore,
  copyRestore,
};

// 统一的操作历史窗口：左侧是记录列表（最新在前），右侧是这条记录的完整元数据与
// 「可恢复范围」。它只展示与收集意图，不执行任何 Git 命令；点「恢复」只把选中的记录 ID
// 交回主窗口，由主窗口重新做仓库绑定检查并交给 RestoreFlow（预检 → 强制确认 → 复核 → 命令窗口）。
//
// 与 Win32 过渡实现的区别：这里把记录详情、逐目标核实与复核证据一并摆在同一个窗口里，
// 不再用一串模态选择框；但恢复语义没有变——unknown / inProgress 也允许选中查看，
// 真正的执行仍要重新预检，记录本身永远不是执行依据。
class HistoryDialog final : public wxDialog {
public:
  HistoryDialog(wxWindow* parent, std::vector<app::OperationRecord> records,
                std::wstring storageLine, std::wstring currentWorkTreeRoot);

  [[nodiscard]] HistoryDialogAction action() const noexcept { return action_; }
  [[nodiscard]] const std::wstring& selectedRecordId() const noexcept { return selectedId_; }

private:
  void SelectFirstRow();
  void OnSelectionChanged(wxListEvent& event);
  void OnRestoreClicked(wxCommandEvent& event);
  void OnCopyClicked(wxCommandEvent& event);
  void UpdateDetailsAndButtons();
  [[nodiscard]] const app::OperationRecord* SelectedRecord() const;
  [[nodiscard]] std::wstring BuildDetails(const app::OperationRecord& record) const;

  std::vector<app::OperationRecord> records_;  // 最新在前：与列表行一一对应
  std::wstring storageLine_;
  std::wstring currentWorkTreeRoot_;
  wxListCtrl* list_ = nullptr;
  wxTextCtrl* details_ = nullptr;
  wxButton* restoreButton_ = nullptr;
  wxButton* copyButton_ = nullptr;
  HistoryDialogAction action_ = HistoryDialogAction::none;
  std::wstring selectedId_;
};

}  // namespace gc::ui::wx
