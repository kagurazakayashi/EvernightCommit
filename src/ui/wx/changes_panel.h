#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <wx/listctrl.h>
#include <wx/panel.h>

#include "app/list_view_memory.h"
#include "git/diff_view.h"
#include "git/workspace_model.h"

class wxButton;
class wxNotebook;
class wxScrolledWindow;
class wxSplitterWindow;
class wxStaticText;
class wxTextCtrl;

namespace gc::ui::wx {

class CommitFormPanel;

// 「变化信息 / 提交历史」两页面板（Windows 与 Linux 共用）。
//
// 对应 Win32 过渡实现的 ui/changes_pane.*：同样的两个选项卡、同样的列与百分比列宽、
// 同样「按条目身份就地增删行」的刷新方式（绝不整列清空——清空会把报表视图的滚动位置
// 退回顶部且救不回来，见 app/list_view_memory.h）。行号与条目的对应关系只在
// 本面板最后一次落地的内容上成立：双击与暂存都按行号经这里取回条目再与模型核对，
// 绝不从单元格的显示文本反解路径或对象 ID。
class ChangesPanel final : public wxPanel {
public:
  explicit ChangesPanel(wxWindow* parent);

  // 提交表单由本面板创建在「变化信息」页下半区的滚动窗里；主窗口取用这个指针挂回调。
  [[nodiscard]] CommitFormPanel* form() const noexcept { return form_; }

  // ---- 回调（由主窗口挂接；重建进行中不会触发） ----
  std::function<void()> onStagingSelectionChanged;
  std::function<void()> onHistorySelectionChanged;
  std::function<void(git::ChangeSide side, int row)> onChangesDoubleClicked;
  std::function<void(int row)> onHistoryDoubleClicked;
  std::function<void(int row)> onFileDoubleClicked;
  std::function<void()> onCopyDetail;

  // ---- 「变化信息」页：两个更改列表 + 最近提交，一次落地 ----
  // 选中项按条目身份跨刷新保留（app/list_view_memory），滚动位置靠就地增删保留。
  void ShowWorkspace(const git::WorkspaceModel& model, const git::EmptyStateTexts& texts);

  // ---- 「提交历史」页右侧「文件变化」清单的落点（语义同 Win32 ChangesPane） ----
  struct CommitFilesView {
    bool hasCommit = false;
    git::CommitItem commit;
    bool loading = false;  // 后台读取进行中（changes 必为空）
    std::wstring error;    // 非空 = 这次读取失败（changes 必为空）
    std::wstring note;     // 成功摘要 / 失败原因 / 引导语，进空状态说明
    std::vector<git::ChangeItem> changes;
  };
  void ShowCommitFiles(const CommitFilesView& view);

  // ---- 「提交历史」页底部「提交详情」带 ----
  struct CommitDetailView {
    bool hasCommit = false;
    git::CommitItem commit;
    bool loading = false;  // 后台读取进行中（text 与 error 都忽略）
    std::wstring text;     // 成功时那份可复制文本
    std::wstring error;    // 非空 = 这一条没能读出来
  };
  void ShowCommitDetail(const CommitDetailView& view);
  // 只有真读到一条提交的详情时才非空；引导语、进行中与失败说明都不算可复制内容。
  [[nodiscard]] const std::wstring& CommitDetailText() const noexcept { return detailText_; }
  [[nodiscard]] const std::wstring& CommitDetailCommitId() const noexcept {
    return detailCommitId_;
  }

  // ---- 行号 ↔ 条目（只对最后一次落地的内容成立） ----
  [[nodiscard]] const git::ChangeItem* ItemAt(git::ChangeSide side, int row) const;
  [[nodiscard]] const git::CommitItem* CommitItemAt(int row) const;
  [[nodiscard]] bool CommitFileAt(int row, const git::CommitItem** commit,
                                  const git::ChangeItem** change) const;
  [[nodiscard]] int UnstagedRowCount() const noexcept;
  [[nodiscard]] int StagedRowCount() const noexcept;
  [[nodiscard]] int HistoryRowCount() const noexcept;
  [[nodiscard]] int FilesRowCount() const noexcept;
  [[nodiscard]] const std::wstring& CommitFilesCommitId() const noexcept {
    return filesCommitId_;
  }
  [[nodiscard]] bool CommitFilesLoading() const noexcept { return filesLoading_; }
  [[nodiscard]] int CommitFilesCount() const noexcept { return static_cast<int>(shownFiles_.size()); }
  // 重建期间为真：主窗口要忽略重建过程中间态发出的选中通知，结束后统一重对一次。
  [[nodiscard]] bool UpdatingLists() const noexcept { return updatingLists_; }

  [[nodiscard]] std::vector<int> SelectedUnstagedRows() const;
  [[nodiscard]] std::vector<int> SelectedStagedRows() const;
  [[nodiscard]] int SelectedHistoryRow() const;  // 没有选中返回 -1

  [[nodiscard]] int ActiveTab() const;

  // ---- 控件句柄（主窗口负责挂工具提示与可用性） ----
  [[nodiscard]] wxButton* stageAllButton() const noexcept { return stageAll_; }
  [[nodiscard]] wxButton* stageSelectedButton() const noexcept { return stageSelected_; }
  [[nodiscard]] wxButton* unstageSelectedButton() const noexcept { return unstageSelected_; }
  [[nodiscard]] wxButton* unstageAllButton() const noexcept { return unstageAll_; }
  [[nodiscard]] wxButton* copyDetailButton() const noexcept { return detailCopy_; }
  [[nodiscard]] wxStaticText* unstagedHint() const noexcept { return unstagedHint_; }
  [[nodiscard]] wxStaticText* stagedHint() const noexcept { return stagedHint_; }

private:
  void BuildChangesPage();
  void BuildHistoryPage();
  void BindEvents();

  // 就地差量重建一张有键列表：删中段、插中段、逐行改文本，选中按条目身份恢复。
  void RebuildChangeList(wxListCtrl* list, std::vector<git::ChangeItem>& shown,
                         const std::vector<git::ChangeItem>& items);
  void RebuildCommitList(const std::vector<git::CommitItem>& items);
  void ApplyColumnPercents(wxListCtrl* list, const std::vector<int>& percents);

  wxNotebook* notebook_ = nullptr;
  // 变化信息页
  wxSplitterWindow* changesSplitter_ = nullptr;
  wxScrolledWindow* formScroll_ = nullptr;
  CommitFormPanel* form_ = nullptr;
  wxListCtrl* unstagedList_ = nullptr;
  wxListCtrl* stagedList_ = nullptr;
  wxStaticText* unstagedHint_ = nullptr;
  wxStaticText* stagedHint_ = nullptr;
  wxButton* stageAll_ = nullptr;
  wxButton* stageSelected_ = nullptr;
  wxButton* unstageSelected_ = nullptr;
  wxButton* unstageAll_ = nullptr;
  // 提交历史页
  wxSplitterWindow* historySplitter_ = nullptr;
  wxListCtrl* historyList_ = nullptr;
  wxListCtrl* filesList_ = nullptr;
  wxStaticText* historyHint_ = nullptr;
  wxStaticText* filesHint_ = nullptr;
  wxTextCtrl* detailEdit_ = nullptr;
  wxButton* detailCopy_ = nullptr;

  // 上一次落地的内容：行号与条目身份的对应关系只在这里成立。
  std::vector<git::ChangeItem> shownUnstaged_;
  std::vector<git::ChangeItem> shownStaged_;
  std::vector<git::CommitItem> shownCommits_;
  git::CommitItem filesCommit_;
  std::vector<git::ChangeItem> shownFiles_;
  std::wstring filesCommitId_;
  std::wstring detailText_;
  std::wstring detailCommitId_;
  bool filesLoading_ = false;
  bool updatingLists_ = false;
};

}  // namespace gc::ui::wx
