#include "ui/wx/changes_panel.h"

#include <algorithm>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "app/list_rebuild.h"
#include "git/commit_history.h"
#include "ui/wx/commit_form_panel.h"
#include "ui/wx/wx_text.h"

namespace gc::ui::wx {
namespace {

constexpr int kChangesTabId = wxID_HIGHEST + 201;
constexpr int kHistoryTabId = wxID_HIGHEST + 202;
constexpr int kUnstagedListId = wxID_HIGHEST + 203;
constexpr int kStagedListId = wxID_HIGHEST + 204;
constexpr int kHistoryListId = wxID_HIGHEST + 205;
constexpr int kFilesListId = wxID_HIGHEST + 206;

constexpr const wchar_t* kDetailGuide =
    L"选中「最近提交」里的某一条，这里显示它的完整信息"
    L"（哈希、标题、作者与邮箱、作者时间、提交者时间、描述）。";
constexpr const wchar_t* kFilesGuide =
    L"选择一条提交查看它的文件变化\n"
    L"选中「最近提交」里的某一条，这里会列出它改动的文件；双击文件可查看详细差异。";

std::wstring TwoLine(std::wstring_view first, std::wstring_view second) {
  std::wstring text(first);
  text += L'\n';
  text += second;
  return text;
}

// 当前选中行的条目身份键（行号只对上一次落地的内容成立，越界的行直接忽略）。
template <typename Item>
app::ListViewMemory CaptureMemory(wxListCtrl* list, const std::vector<Item>& shown) {
  app::ListViewMemory memory;
  const long count = list->GetItemCount();
  for (long row = 0; row < count; ++row) {
    if ((list->GetItemState(row, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED) != 0 &&
        static_cast<size_t>(row) < shown.size()) {
      memory.selectedKeys.push_back(app::ViewKeyForItem(shown[static_cast<size_t>(row)]));
    }
  }
  return memory;
}

template <typename Item>
void RestoreSelection(wxListCtrl* list, const std::vector<Item>& items,
                      const app::ListViewMemory& memory) {
  const app::RestoredListView restored = app::MapListViewMemory(items, memory);
  for (const int row : restored.selectedRows) {
    list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
  }
}

}  // namespace

ChangesPanel::ChangesPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
  auto* sizer = new wxBoxSizer(wxVERTICAL);
  notebook_ = new wxNotebook(this, wxID_ANY);
  BuildChangesPage();
  BuildHistoryPage();
  sizer->Add(notebook_, 1, wxEXPAND);
  SetSizer(sizer);
  BindEvents();
}

void ChangesPanel::BuildChangesPage() {
  auto* page = new wxPanel(notebook_, kChangesTabId);
  auto* pageSizer = new wxBoxSizer(wxVERTICAL);
  changesSplitter_ = new wxSplitterWindow(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxSP_LIVE_UPDATE | wxSP_3DSASH);
  changesSplitter_->SetMinimumPaneSize(140);

  auto* listsPanel = new wxPanel(changesSplitter_, wxID_ANY);
  auto* lists = new wxBoxSizer(wxHORIZONTAL);

  auto* unstagedColumn = new wxBoxSizer(wxVERTICAL);
  unstagedColumn->Add(new wxStaticText(listsPanel, wxID_ANY, FromWide(L"未暂存的更改")), 0,
                      wxBOTTOM, 4);
  unstagedList_ = new wxListCtrl(listsPanel, kUnstagedListId, wxDefaultPosition, wxSize(-1, 120),
                                 wxLC_REPORT);
  unstagedList_->InsertColumn(0, FromWide(L"状态"));
  unstagedList_->InsertColumn(1, FromWide(L"相对路径"));
  unstagedColumn->Add(unstagedList_, 1, wxEXPAND);
  unstagedHint_ = new wxStaticText(listsPanel, wxID_ANY, FromWide(L"尚未读取。"), wxDefaultPosition,
                                   wxDefaultSize, wxST_NO_AUTORESIZE);
  unstagedColumn->Add(unstagedHint_, 0, wxTOP | wxEXPAND, 4);
  lists->Add(unstagedColumn, 1, wxEXPAND | wxRIGHT, 8);

  auto* buttons = new wxBoxSizer(wxVERTICAL);
  stageAll_ = new wxButton(listsPanel, wxID_ANY, FromWide(L"全部加入 →"));
  stageSelected_ = new wxButton(listsPanel, wxID_ANY, FromWide(L"加入暂存区 →"));
  unstageSelected_ = new wxButton(listsPanel, wxID_ANY, FromWide(L"← 移出暂存区"));
  unstageAll_ = new wxButton(listsPanel, wxID_ANY, FromWide(L"← 全部移出"));
  // 按钮列给一个最小宽度：布局初帧在隐藏页上以极小宽度分配会刷一屏
  // GTK「Negative content width」告警，最小宽度让分配退到滚动而不是压扁按钮。
  for (wxButton* button : {stageAll_, stageSelected_, unstageSelected_, unstageAll_}) {
    button->SetMinSize(wxSize(132, -1));
  }
  buttons->AddStretchSpacer();
  buttons->Add(stageAll_, 0, wxEXPAND | wxBOTTOM, 6);
  buttons->Add(stageSelected_, 0, wxEXPAND | wxBOTTOM, 12);
  buttons->Add(unstageSelected_, 0, wxEXPAND | wxBOTTOM, 6);
  buttons->Add(unstageAll_, 0, wxEXPAND);
  buttons->AddStretchSpacer();
  lists->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

  auto* stagedColumn = new wxBoxSizer(wxVERTICAL);
  stagedColumn->Add(new wxStaticText(listsPanel, wxID_ANY, FromWide(L"已暂存的更改")), 0,
                    wxBOTTOM, 4);
  stagedList_ = new wxListCtrl(listsPanel, kStagedListId, wxDefaultPosition, wxSize(-1, 120),
                               wxLC_REPORT);
  stagedList_->InsertColumn(0, FromWide(L"状态"));
  stagedList_->InsertColumn(1, FromWide(L"相对路径"));
  stagedColumn->Add(stagedList_, 1, wxEXPAND);
  stagedHint_ = new wxStaticText(listsPanel, wxID_ANY, FromWide(L"尚未读取。"), wxDefaultPosition,
                                 wxDefaultSize, wxST_NO_AUTORESIZE);
  stagedColumn->Add(stagedHint_, 0, wxTOP | wxEXPAND, 4);
  lists->Add(stagedColumn, 1, wxEXPAND);

  listsPanel->SetSizer(lists);

  // 表单放进滚动区：小屏幕上也要能滚到最后一行（与迁移前同一约定）。
  formScroll_ = new wxScrolledWindow(changesSplitter_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxVSCROLL);
  formScroll_->SetScrollRate(0, 12);
  // 必须显式设最小高度：wxScrolledWindow 默认把整份表单的最佳尺寸报成自己的最小尺寸，
  // 小屏幕上会把整个窗口顶出屏幕并被裁掉顶部。
  formScroll_->SetMinSize(wxSize(-1, 200));
  auto* formSizer = new wxBoxSizer(wxVERTICAL);
  form_ = new CommitFormPanel(formScroll_);
  formSizer->Add(form_, 1, wxEXPAND);
  formScroll_->SetSizer(formSizer);

  // 上下分割（列表区在上、表单在下）：wxSplitterWindow 的命名里 Horizontally 指横条上下。
  changesSplitter_->SplitHorizontally(listsPanel, formScroll_, 380);
  changesSplitter_->SetSashGravity(0.6);
  pageSizer->Add(changesSplitter_, 1, wxEXPAND);
  page->SetSizer(pageSizer);
  notebook_->AddPage(page, FromWide(L"变化信息"), true);
}

void ChangesPanel::BuildHistoryPage() {
  auto* page = new wxPanel(notebook_, kHistoryTabId);
  auto* pageSizer = new wxBoxSizer(wxVERTICAL);
  historySplitter_ = new wxSplitterWindow(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxSP_LIVE_UPDATE | wxSP_3DSASH);
  historySplitter_->SetMinimumPaneSize(120);

  auto* listsPanel = new wxPanel(historySplitter_, wxID_ANY);
  auto* lists = new wxBoxSizer(wxHORIZONTAL);

  auto* historyColumn = new wxBoxSizer(wxVERTICAL);
  historyColumn->Add(new wxStaticText(listsPanel, wxID_ANY, FromWide(L"最近提交")), 0, wxBOTTOM, 4);
  historyList_ = new wxListCtrl(listsPanel, kHistoryListId, wxDefaultPosition, wxSize(-1, 120),
                                wxLC_REPORT);
  historyList_->InsertColumn(0, FromWide(L"提交"));
  historyList_->InsertColumn(1, FromWide(L"标题"));
  historyList_->InsertColumn(2, FromWide(L"作者"));
  historyList_->InsertColumn(3, FromWide(L"时间"));
  historyColumn->Add(historyList_, 1, wxEXPAND);
  historyHint_ = new wxStaticText(listsPanel, wxID_ANY, FromWide(L"尚未读取。"), wxDefaultPosition,
                                  wxDefaultSize, wxST_NO_AUTORESIZE);
  historyColumn->Add(historyHint_, 0, wxTOP | wxEXPAND, 4);
  lists->Add(historyColumn, 1, wxEXPAND | wxRIGHT, 8);

  auto* filesColumn = new wxBoxSizer(wxVERTICAL);
  filesColumn->Add(new wxStaticText(listsPanel, wxID_ANY, FromWide(L"文件变化")), 0, wxBOTTOM, 4);
  filesList_ = new wxListCtrl(listsPanel, kFilesListId, wxDefaultPosition, wxSize(-1, 120),
                              wxLC_REPORT);
  filesList_->InsertColumn(0, FromWide(L"状态"));
  filesList_->InsertColumn(1, FromWide(L"相对路径"));
  filesColumn->Add(filesList_, 1, wxEXPAND);
  filesHint_ = new wxStaticText(listsPanel, wxID_ANY, FromWide(kFilesGuide), wxDefaultPosition,
                                wxDefaultSize, wxST_NO_AUTORESIZE);
  filesColumn->Add(filesHint_, 0, wxTOP | wxEXPAND, 4);
  lists->Add(filesColumn, 1, wxEXPAND);
  listsPanel->SetSizer(lists);

  auto* detailPanel = new wxPanel(historySplitter_, wxID_ANY);
  auto* detailSizer = new wxBoxSizer(wxVERTICAL);
  auto* detailHeader = new wxBoxSizer(wxHORIZONTAL);
  detailHeader->Add(new wxStaticText(detailPanel, wxID_ANY, FromWide(L"提交详情")), 1,
                    wxALIGN_CENTER_VERTICAL);
  detailCopy_ = new wxButton(detailPanel, wxID_ANY, FromWide(L"复制详情"));
  detailCopy_->Enable(false);
  detailHeader->Add(detailCopy_, 0);
  detailSizer->Add(detailHeader, 0, wxEXPAND | wxBOTTOM, 4);
  detailEdit_ = new wxTextCtrl(detailPanel, wxID_ANY, FromWide(kDetailGuide), wxDefaultPosition,
                               wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
  detailSizer->Add(detailEdit_, 1, wxEXPAND);
  detailPanel->SetSizer(detailSizer);

  historySplitter_->SplitHorizontally(listsPanel, detailPanel, 300);
  historySplitter_->SetSashGravity(0.7);
  pageSizer->Add(historySplitter_, 1, wxEXPAND);
  page->SetSizer(pageSizer);
  notebook_->AddPage(page, FromWide(L"提交历史"));
}

void ChangesPanel::BindEvents() {
  const auto selectionRouter = [this](wxListEvent& event) {
    if (updatingLists_) {
      return;  // 重建中间态的行号不作数：结束后由主窗口统一重对一次。
    }
    const int id = event.GetId();
    if (id == kUnstagedListId || id == kStagedListId) {
      if (onStagingSelectionChanged) {
        onStagingSelectionChanged();
      }
    } else if (id == kHistoryListId) {
      if (onHistorySelectionChanged) {
        onHistorySelectionChanged();
      }
    }
  };
  for (wxListCtrl* list : {unstagedList_, stagedList_, historyList_, filesList_}) {
    list->Bind(wxEVT_LIST_ITEM_SELECTED, selectionRouter);
    list->Bind(wxEVT_LIST_ITEM_DESELECTED, selectionRouter);
  }
  unstagedList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& event) {
    if (onChangesDoubleClicked) {
      onChangesDoubleClicked(git::ChangeSide::unstaged, static_cast<int>(event.GetIndex()));
    }
  });
  stagedList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& event) {
    if (onChangesDoubleClicked) {
      onChangesDoubleClicked(git::ChangeSide::staged, static_cast<int>(event.GetIndex()));
    }
  });
  historyList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& event) {
    if (onHistoryDoubleClicked) {
      onHistoryDoubleClicked(static_cast<int>(event.GetIndex()));
    }
  });
  filesList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& event) {
    if (onFileDoubleClicked) {
      onFileDoubleClicked(static_cast<int>(event.GetIndex()));
    }
  });
  detailCopy_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (onCopyDetail) {
      onCopyDetail();
    }
  });

  // 列宽按百分比随控件宽度重算（与 Win32 过渡实现同一规则：状态20|路径80，提交16|标题44|作者22|时间18）。
  const auto applyPercents = [](wxListCtrl* list, std::vector<int> percents) {
    list->Bind(wxEVT_SIZE, [list, percents](wxSizeEvent& event) {
      const int width = list->GetSize().GetWidth();
      if (width > 0) {
        int used = 0;
        for (size_t col = 0; col < percents.size(); ++col) {
          int w = std::max(56, width * percents[col] / 100);
          if (col + 1 == percents.size()) {
            w = std::max(56, width - used);
          }
          list->SetColumnWidth(static_cast<int>(col), w);
          used += w;
        }
      }
      event.Skip();
    });
  };
  applyPercents(unstagedList_, {20, 80});
  applyPercents(stagedList_, {20, 80});
  applyPercents(filesList_, {20, 80});
  applyPercents(historyList_, {16, 44, 22, 18});
}

void ChangesPanel::RebuildChangeList(wxListCtrl* list, std::vector<git::ChangeItem>& shown,
                                     const std::vector<git::ChangeItem>& items) {
  const app::ListViewMemory memory = CaptureMemory(list, shown);
  std::vector<std::wstring> oldKeys;
  oldKeys.reserve(shown.size());
  for (const git::ChangeItem& item : shown) {
    oldKeys.push_back(app::ViewKeyForItem(item));
  }
  std::vector<std::wstring> newKeys;
  newKeys.reserve(items.size());
  for (const git::ChangeItem& item : items) {
    newKeys.push_back(app::ViewKeyForItem(item));
  }
  const app::KeyedRebuildPlan plan = app::PlanKeyedRebuild(oldKeys, newKeys);
  for (size_t i = 0; i < plan.removedRows; ++i) {
    list->DeleteItem(static_cast<long>(plan.commonPrefix));
  }
  for (size_t i = 0; i < plan.insertedRows; ++i) {
    list->InsertItem(static_cast<long>(plan.commonPrefix + i), wxString());
  }
  for (size_t row = 0; row < items.size(); ++row) {
    list->SetItem(static_cast<long>(row), 0, FromWide(items[row].StatusLabel()));
    list->SetItem(static_cast<long>(row), 1, FromWide(items[row].PathLabel()));
  }
  shown = items;
  RestoreSelection(list, items, memory);
}

void ChangesPanel::RebuildCommitList(const std::vector<git::CommitItem>& items) {
  const app::ListViewMemory memory = CaptureMemory(historyList_, shownCommits_);
  std::vector<std::wstring> oldKeys;
  oldKeys.reserve(shownCommits_.size());
  for (const git::CommitItem& item : shownCommits_) {
    oldKeys.push_back(app::ViewKeyForItem(item));
  }
  std::vector<std::wstring> newKeys;
  newKeys.reserve(items.size());
  for (const git::CommitItem& item : items) {
    newKeys.push_back(app::ViewKeyForItem(item));
  }
  const app::KeyedRebuildPlan plan = app::PlanKeyedRebuild(oldKeys, newKeys);
  for (size_t i = 0; i < plan.removedRows; ++i) {
    historyList_->DeleteItem(static_cast<long>(plan.commonPrefix));
  }
  for (size_t i = 0; i < plan.insertedRows; ++i) {
    historyList_->InsertItem(static_cast<long>(plan.commonPrefix + i), wxString());
  }
  for (size_t row = 0; row < items.size(); ++row) {
    const git::CommitItem& item = items[row];
    historyList_->SetItem(static_cast<long>(row), 0, FromWide(git::ShortObjectId(item.objectId)));
    historyList_->SetItem(static_cast<long>(row), 1, FromWide(item.summary));
    historyList_->SetItem(static_cast<long>(row), 2, FromWide(item.author));
    historyList_->SetItem(static_cast<long>(row), 3, FromWide(item.authoredAt));
  }
  shownCommits_ = items;
  RestoreSelection(historyList_, items, memory);
}

void ChangesPanel::ShowWorkspace(const git::WorkspaceModel& model,
                                 const git::EmptyStateTexts& texts) {
  updatingLists_ = true;
  RebuildChangeList(unstagedList_, shownUnstaged_, model.unstaged);
  RebuildChangeList(stagedList_, shownStaged_, model.staged);
  RebuildCommitList(model.recentCommits);
  // 空状态说明只在列表真的没有条目时出现（与 Win32 一致）；有条目时不留一句自相矛盾的话。
  historyHint_->SetLabel(FromWide(model.recentCommits.empty() ? texts.history : std::wstring()));
  updatingLists_ = false;
  Layout();
}

void ChangesPanel::ShowCommitFiles(const CommitFilesView& view) {
  updatingLists_ = true;
  filesLoading_ = view.loading;
  if (!view.hasCommit) {
    filesCommitId_.clear();
    RebuildChangeList(filesList_, shownFiles_, {});
    filesHint_->SetLabel(FromWide(kFilesGuide));
  } else {
    filesCommit_ = view.commit;
    filesCommitId_ = view.commit.objectId;
    if (view.loading) {
      RebuildChangeList(filesList_, shownFiles_, {});
      filesHint_->SetLabel(FromWide(TwoLine(L"正在读取这条提交的文件变化",
                                            L"后台执行 git show --name-status…")));
    } else if (!view.error.empty()) {
      RebuildChangeList(filesList_, shownFiles_, {});
      filesHint_->SetLabel(FromWide(TwoLine(L"文件变化读取失败", view.error)));
    } else {
      RebuildChangeList(filesList_, shownFiles_, view.changes);
      if (view.changes.empty()) {
        filesHint_->SetLabel(FromWide(TwoLine(L"这条提交没有文件变化", view.note)));
      } else {
        filesHint_->SetLabel(FromWide(view.note));
      }
    }
  }
  updatingLists_ = false;
  Layout();
}

void ChangesPanel::ShowCommitDetail(const CommitDetailView& view) {
  if (!view.hasCommit) {
    detailCommitId_.clear();
    detailText_.clear();
    detailEdit_->SetValue(FromWide(kDetailGuide));
    detailCopy_->Enable(false);
    return;
  }
  detailCommitId_ = view.commit.objectId;
  if (view.loading) {
    detailText_.clear();
    detailEdit_->SetValue(FromWide(TwoLine(L"正在读取这条提交的完整信息",
                                           L"后台执行 git log -n 1 --format=…，只读，不改动仓库。")));
    detailCopy_->Enable(false);
  } else if (!view.error.empty()) {
    detailText_.clear();
    detailEdit_->SetValue(FromWide(
        TwoLine(L"这条提交的完整信息没能读出来", view.error)));
    detailCopy_->Enable(false);
  } else {
    detailText_ = view.text;
    detailEdit_->SetValue(FromWide(view.text));
    detailCopy_->Enable(!view.text.empty());
  }
}

const git::ChangeItem* ChangesPanel::ItemAt(git::ChangeSide side, int row) const {
  const std::vector<git::ChangeItem>& items =
      side == git::ChangeSide::staged ? shownStaged_ : shownUnstaged_;
  if (row < 0 || static_cast<size_t>(row) >= items.size()) {
    return nullptr;
  }
  return &items[static_cast<size_t>(row)];
}

const git::CommitItem* ChangesPanel::CommitItemAt(int row) const {
  if (row < 0 || static_cast<size_t>(row) >= shownCommits_.size()) {
    return nullptr;
  }
  return &shownCommits_[static_cast<size_t>(row)];
}

bool ChangesPanel::CommitFileAt(int row, const git::CommitItem** commit,
                                const git::ChangeItem** change) const {
  if (filesCommitId_.empty() || row < 0 || static_cast<size_t>(row) >= shownFiles_.size()) {
    return false;
  }
  *commit = &filesCommit_;
  *change = &shownFiles_[static_cast<size_t>(row)];
  return true;
}

int ChangesPanel::UnstagedRowCount() const noexcept {
  return static_cast<int>(unstagedList_->GetItemCount());
}
int ChangesPanel::StagedRowCount() const noexcept {
  return static_cast<int>(stagedList_->GetItemCount());
}
int ChangesPanel::HistoryRowCount() const noexcept {
  return static_cast<int>(historyList_->GetItemCount());
}
int ChangesPanel::FilesRowCount() const noexcept {
  return static_cast<int>(filesList_->GetItemCount());
}

std::vector<int> ChangesPanel::SelectedUnstagedRows() const {
  std::vector<int> rows;
  for (long row = unstagedList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
       row >= 0; row = unstagedList_->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
    rows.push_back(static_cast<int>(row));
  }
  return rows;
}

std::vector<int> ChangesPanel::SelectedStagedRows() const {
  std::vector<int> rows;
  for (long row = stagedList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); row >= 0;
       row = stagedList_->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
    rows.push_back(static_cast<int>(row));
  }
  return rows;
}

int ChangesPanel::SelectedHistoryRow() const {
  const long row = historyList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  return row < 0 ? -1 : static_cast<int>(row);
}

int ChangesPanel::ActiveTab() const { return notebook_->GetSelection(); }

}  // namespace gc::ui::wx
