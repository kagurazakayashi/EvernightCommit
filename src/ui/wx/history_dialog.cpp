#include "ui/wx/history_dialog.h"

#include <algorithm>
#include <utility>

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "platform/local_time.h"
#include "ui/wx/wx_text.h"

namespace gc::ui::wx {
namespace {

std::wstring FormatHistoryEpoch(long long epoch) {
  if (epoch <= 0) {
    return L"时间未知";
  }
  return platform::FormatLocalEpochSeconds(epoch);
}

std::wstring ObjectIdDisplay(std::wstring_view id) {
  return id.empty() ? std::wstring(L"（无）") : std::wstring(id);
}

std::wstring TargetCheckLine(const app::HistoryTargetCheck& check) {
  std::wstring line = L"・" + (check.maskedUrl.empty() ? std::wstring(L"（目标地址未记录）")
                                                        : check.maskedUrl) +
                      L"：";
  if (!check.queried) {
    line += L"未核实（这次没有问成，不能当成「对端没有」）";
    if (!check.failure.empty()) {
      line += L"；原因：" + check.failure;
    }
    return line;
  }
  if (!check.ok) {
    line += L"核实失败";
    if (!check.failure.empty()) {
      line += L"；原因：" + check.failure;
    }
    return line;
  }
  if (!check.refPresent) {
    line += L"对端没有这条引用";
    return line;
  }
  line += L"对端存在这条引用";
  if (!check.remoteObjectId.empty()) {
    line += L"，对象 " + check.remoteObjectId;
  }
  return line;
}

}  // namespace

HistoryDialog::HistoryDialog(wxWindow* parent, std::vector<app::OperationRecord> records,
                             std::wstring storageLine, std::wstring currentWorkTreeRoot)
    : wxDialog(parent, wxID_ANY, FromWide(L"操作历史"), wxDefaultPosition, wxSize(1080, 680),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      records_(std::move(records)),
      storageLine_(std::move(storageLine)),
      currentWorkTreeRoot_(std::move(currentWorkTreeRoot)) {
  std::reverse(records_.begin(), records_.end());  // 最新在前

  auto* root = new wxBoxSizer(wxVERTICAL);
  auto* storageText = new wxStaticText(
      this, wxID_ANY, FromWide(L"位置与保存范围：" + storageLine_));
  storageText->Wrap(1020);
  root->Add(storageText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
  auto* explanation = new wxStaticText(
      this, wxID_ANY,
      FromWide(L"这里只展示元数据（操作、时间、引用与对象 ID、终态、逐目标核实与复核线索），"
               L"不包含文件内容、提交正文或任何凭据。记录本身不是执行依据："
               L"选中一条「引用级可回退」的记录后，恢复仍会重新只读预检现状并让你确认。"));
  explanation->Wrap(1020);
  root->Add(explanation, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

  auto* body = new wxBoxSizer(wxHORIZONTAL);
  list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                         wxLC_REPORT | wxLC_SINGLE_SEL);
  list_->InsertColumn(0, FromWide(L"时间"));
  list_->InsertColumn(1, FromWide(L"操作"));
  list_->InsertColumn(2, FromWide(L"终态"));
  list_->InsertColumn(3, FromWide(L"引用"));
  for (size_t i = 0; i < records_.size(); ++i) {
    const app::OperationRecord& record = records_[i];
    const long row = list_->InsertItem(static_cast<long>(i),
                                       FromWide(FormatHistoryEpoch(record.startedEpoch)));
    list_->SetItem(row, 1, FromWide(std::wstring(app::HistoryFlowLabel(record.flow))));
    list_->SetItem(row, 2, FromWide(std::wstring(app::HistoryTerminalLabel(record.terminal))));
    list_->SetItem(row, 3, FromWide(record.restoreBranchRef.empty() ? record.sourceRef
                                                                    : record.restoreBranchRef));
  }
  list_->SetColumnWidth(0, 150);
  list_->SetColumnWidth(1, 150);
  list_->SetColumnWidth(2, 270);
  list_->SetColumnWidth(3, 260);

  details_ = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                            wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
  body->Add(list_, 3, wxEXPAND | wxALL, 10);
  body->Add(details_, 4, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 10);
  root->Add(body, 1, wxEXPAND);

  if (records_.empty()) {
    details_->SetValue(FromWide(
        L"还没有任何记录。勾选主窗口的「记录操作历史」之后，由本程序启动的写操作才会被记录；"
        L"这条开关默认关闭，不会在未勾选时写任何东西。"));
  }

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  buttons->Add(new wxStaticText(
                   this, wxID_ANY,
                   FromWide(L"「恢复」只移动记录里那一条本地分支引用；索引与工作区一字不动，"
                            L"远端不受影响。")),
               1, wxALIGN_CENTER_VERTICAL);
  restoreButton_ = new wxButton(this, wxID_ANY, FromWide(L"恢复所选记录…"));
  copyButton_ = new wxButton(this, wxID_ANY, FromWide(L"复制恢复线索"));
  buttons->Add(restoreButton_, 0, wxRIGHT, 6);
  buttons->Add(copyButton_, 0, wxRIGHT, 6);
  buttons->Add(new wxButton(this, wxID_CANCEL, FromWide(L"关闭")), 0);
  root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  SetSizer(root);
  SetMinSize(wxSize(900, 560));

  list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& event) {
    OnSelectionChanged(event);
  });
  restoreButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { OnRestoreClicked(event); });
  copyButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { OnCopyClicked(event); });

  SelectFirstRow();
  UpdateDetailsAndButtons();
  CentreOnParent();
}

void HistoryDialog::SelectFirstRow() {
  if (records_.empty() || list_ == nullptr) {
    return;
  }
  list_->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                      wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
  list_->EnsureVisible(0);
}

const app::OperationRecord* HistoryDialog::SelectedRecord() const {
  if (list_ == nullptr) {
    return nullptr;
  }
  const long row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (row < 0 || static_cast<size_t>(row) >= records_.size()) {
    return nullptr;
  }
  return &records_[static_cast<size_t>(row)];
}

void HistoryDialog::OnSelectionChanged(wxListEvent& event) {
  event.Skip();
  UpdateDetailsAndButtons();
}

void HistoryDialog::UpdateDetailsAndButtons() {
  const app::OperationRecord* record = SelectedRecord();
  if (record == nullptr) {
    if (records_.empty()) {
      restoreButton_->Enable(false);
      copyButton_->Enable(false);
      return;
    }
    details_->SetValue(FromWide(L"先在左侧选中一条记录。"));
    restoreButton_->Enable(false);
    copyButton_->Enable(false);
    return;
  }
  details_->SetValue(FromWide(BuildDetails(*record)));
  const app::HistoryRestoreOffer offer =
      app::DescribeHistoryRestoreOffer(*record, currentWorkTreeRoot_);
  restoreButton_->Enable(offer.offered);
  restoreButton_->SetToolTip(FromWide(
      offer.offered
          ? L"重新只读预检这条分支的现值、引用形态与占用、流程痕迹，确认后才在命令窗口里执行"
            L"带预期旧值的 git update-ref；不会自动恢复、不会重发、不触碰远端。"
          : L"这条记录当前不能走引用级恢复：" + offer.refusal));
  copyButton_->Enable(true);
}

std::wstring HistoryDialog::BuildDetails(const app::OperationRecord& record) const {
  std::wstring text;
  const auto line = [&text](std::wstring_view value) {
    text += value;
    text.push_back(L'\n');
  };

  line(L"记录 ID：" + record.id);
  line(L"操作：" + std::wstring(app::HistoryFlowLabel(record.flow)) +
       (record.operationLabel.empty() ? std::wstring() : L"（" + record.operationLabel + L"）"));
  line(L"终态：" + std::wstring(app::HistoryTerminalLabel(record.terminal)));
  if (!record.completionLabel.empty()) {
    line(L"终态来源：" + record.completionLabel);
  }
  if (record.exitCodeKnown) {
    line(L"Git 退出码：" + std::to_wstring(record.exitCode));
  } else {
    line(L"Git 退出码：未知（没有拿到 Git 自己的退出码）");
  }
  line(L"启动时刻：" + FormatHistoryEpoch(record.startedEpoch));
  line(L"终态时刻：" + FormatHistoryEpoch(record.terminalEpoch));
  if (record.terminal == app::HistoryTerminal::inProgress ||
      record.terminal == app::HistoryTerminal::unknown) {
    line(L"结果说明：这一条没有确定的成败；可能已经生效，也可能没有。"
         L"恢复入口不会把这条记录当成执行依据，而会重新预检当前引用与对象。");
  }
  line(L"仓库：" + (record.workTreeRoot.empty() ? std::wstring(L"（未记录）")
                                                : record.workTreeRoot));
  if (!record.repositoryKey.empty()) {
    line(L"规范键：" + record.repositoryKey);
  }

  if (!record.sourceRef.empty() || !record.sourceObjectId.empty() ||
      !record.targetRef.empty() || !record.targetObjectId.empty()) {
    line(L"");
    line(L"确认过的引用与对象：");
    if (!record.sourceRef.empty()) {
      line(L"  源引用：" + record.sourceRef);
    }
    if (!record.sourceObjectId.empty()) {
      line(L"  源对象：" + ObjectIdDisplay(record.sourceObjectId));
    }
    if (!record.targetRef.empty()) {
      line(L"  目标引用：" + record.targetRef);
    }
    if (!record.targetObjectId.empty()) {
      line(L"  目标对象：" + ObjectIdDisplay(record.targetObjectId));
    }
    if (!record.remoteName.empty()) {
      line(L"  发布远端：" + record.remoteName);
    }
  }

  if (!record.targetChecks.empty()) {
    line(L"");
    size_t queried = 0;
    size_t refPresent = 0;
    for (const app::HistoryTargetCheck& check : record.targetChecks) {
      if (check.queried && check.ok) {
        ++queried;
        if (check.refPresent) {
          ++refPresent;
        }
      }
    }
    line(L"发布目标核实：共 " + std::to_wstring(record.targetChecks.size()) + L" 项；已核实 " +
         std::to_wstring(queried) + L" 项，其中对端存在引用 " + std::to_wstring(refPresent) +
         L" 项；其余「未核实」不等于「没有」，见逐项说明。");
    if (record.terminal == app::HistoryTerminal::succeeded && queried != record.targetChecks.size()) {
      line(L"整体判定：命令终态为成功，但逐目标核实没有全部完成——这是「部分完成/未能完全核实」，"
           L"不能据此断言所有目标都收到了那一份引用。");
    }
    for (const app::HistoryTargetCheck& check : record.targetChecks) {
      line(TargetCheckLine(check));
    }
  }

  if (!record.reviews.empty()) {
    line(L"");
    line(L"后续追加的复核证据（只追加，不改写已落定的终态）：");
    for (const app::HistoryReview& review : record.reviews) {
      line(L"・" + FormatHistoryEpoch(review.epoch) + L"：" + review.text);
    }
  }

  line(L"");
  line(L"可恢复范围：" + std::wstring(app::HistoryRestoreKindLabel(record.restoreKind)));
  if (record.restoreKind == app::HistoryRestoreKind::refMove &&
      record.terminal != app::HistoryTerminal::succeeded) {
    line(L"注意：这条记录的操作终态不是成功（" +
         std::wstring(app::HistoryTerminalLabel(record.terminal)) +
         L"）。它仍可作为恢复线索，但恢复会重新预检当前引用；"
         L"若分支并不在记录的位置，会被明确拒绝，不会照旧命令执行。");
  }
  if (!record.restoreBranchRef.empty()) {
    line(L"  引用：" + record.restoreBranchRef);
  }
  if (!record.restoreExpectedCurrentId.empty()) {
    line(L"  操作后该分支应指（恢复命令的预期旧值）：" + record.restoreExpectedCurrentId);
  }
  if (!record.restoreUndoToObjectId.empty()) {
    line(L"  要挪回去的对象：" + record.restoreUndoToObjectId);
  }
  if (record.restoreIsRoot) {
    line(L"  恢复形态：删除该分支引用（回到该分支本不存在的状态）");
  }
  if (!record.restoreNote.empty()) {
    line(L"  线索：" + record.restoreNote);
  }

  const app::HistoryRestoreOffer offer =
      app::DescribeHistoryRestoreOffer(record, currentWorkTreeRoot_);
  line(L"本窗口的恢复入口：" +
       (offer.offered ? std::wstring(L"可以开始（仍会重新预检并确认）")
                      : L"不可用——" + offer.refusal));

  if (!record.outcomeNote.empty()) {
    line(L"");
    line(L"终态结论：" + record.outcomeNote);
  }
  return text;
}

void HistoryDialog::OnRestoreClicked(wxCommandEvent& event) {
  static_cast<void>(event);
  const app::OperationRecord* record = SelectedRecord();
  if (record == nullptr) {
    return;
  }
  const app::HistoryRestoreOffer offer =
      app::DescribeHistoryRestoreOffer(*record, currentWorkTreeRoot_);
  if (!offer.offered) {
    wxMessageBox(FromWide(L"没有开始恢复，也没有对仓库做任何改动。\n\n" + offer.refusal),
                 FromWide(L"这条记录不能按引用级恢复"), wxOK | wxICON_INFORMATION, this);
    return;
  }
  selectedId_ = record->id;
  action_ = HistoryDialogAction::restore;
  EndModal(wxID_OK);
}

void HistoryDialog::OnCopyClicked(wxCommandEvent& event) {
  static_cast<void>(event);
  const app::OperationRecord* record = SelectedRecord();
  if (record == nullptr) {
    return;
  }
  selectedId_ = record->id;
  action_ = HistoryDialogAction::copyRestore;
  EndModal(wxID_OK);
}

}  // namespace gc::ui::wx
