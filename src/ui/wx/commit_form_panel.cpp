#include "ui/wx/commit_form_panel.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timectrl.h>

#include "git/commit_identity.h"
#include "platform/local_time.h"
#include "ui/wx/wx_text.h"

namespace gc::ui::wx {
namespace {

wxDateTime ToWxDateTime(const git::CivilTime& value) {
  const auto month = static_cast<wxDateTime::Month>(
      std::clamp(value.month, 1, 12) - 1);
  return wxDateTime(static_cast<wxDateTime::wxDateTime_t>(std::max(value.day, 1)), month, value.year,
                    static_cast<wxDateTime::wxDateTime_t>(std::clamp(value.hour, 0, 23)),
                    static_cast<wxDateTime::wxDateTime_t>(std::clamp(value.minute, 0, 59)),
                    static_cast<wxDateTime::wxDateTime_t>(std::clamp(value.second, 0, 59)));
}

// 合作者输入框：格式不对、或与列表里另一位是同一个人时，框不关闭，把原因写在框里。
// 这样「重填」不需要重新打开对话框，也避免程序静默去重。
class CoauthorDialog final : public wxDialog {
public:
  CoauthorDialog(wxWindow* parent, const std::wstring& initial,
                 std::function<std::wstring(std::wstring_view)> validator)
      : wxDialog(parent, wxID_ANY, FromWide(L"合作者"), wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        validator_(std::move(validator)) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(this, wxID_ANY,
                               FromWide(L"格式：姓名 <邮箱>（例如 张三 <zhangsan@example.com>）")),
              0, wxALL, 10);
    edit_ = new wxTextCtrl(this, wxID_ANY, FromWide(initial), wxDefaultPosition, wxSize(460, -1),
                           wxTE_PROCESS_ENTER);
    root->Add(edit_, 0, wxLEFT | wxRIGHT | wxEXPAND, 10);
    error_ = new wxStaticText(this, wxID_ANY, wxString());
    error_->SetForegroundColour(*wxRED);
    root->Add(error_, 0, wxLEFT | wxRIGHT | wxTOP, 10);

    auto* buttons = new wxStdDialogButtonSizer();
    auto* ok = new wxButton(this, wxID_OK, FromWide(L"确定"));
    buttons->AddButton(ok);
    buttons->AddButton(new wxButton(this, wxID_CANCEL, FromWide(L"取消")));
    buttons->Realize();
    root->Add(buttons, 0, wxALL | wxEXPAND, 10);
    SetSizerAndFit(root);
    SetMinSize(wxSize(520, 180));

    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Accept(); });
    edit_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Accept(); });
    edit_->SetFocus();
    edit_->SelectAll();
  }

  [[nodiscard]] std::wstring Value() const { return ToWide(edit_->GetValue()); }

private:
  void Accept() {
    const std::wstring text = ToWide(edit_->GetValue());
    const std::wstring reason = validator_(text);
    if (!reason.empty()) {
      error_->SetLabel(FromWide(reason));
      Layout();
      edit_->SetFocus();
      return;
    }
    EndModal(wxID_OK);
  }

  wxTextCtrl* edit_ = nullptr;
  wxStaticText* error_ = nullptr;
  std::function<std::wstring(std::wstring_view)> validator_;
};

std::wstring OffsetFor(const git::CivilTime& wall) {
  const platform::LocalInstant instant = platform::ResolveLocalWallTime(wall);
  if (!instant.valid) {
    return L"该墙上时间在本机时区不存在（" + instant.failureReason +
           L"）。请另选一个时间，否则创建提交会被拒绝。";
  }
  std::wstring text = L"UTC" + git::FormatOffsetText(instant.offsetMinutes).substr(3);
  if (instant.ambiguous) {
    text += L"；这个时间在夏令时回拨中出现两次，将采用较早的瞬间，另一个是 UTC" +
            git::FormatOffsetText(instant.alternateOffsetMinutes).substr(3);
  }
  return text;
}

}  // namespace

CommitFormPanel::CommitFormPanel(wxWindow* parent) : wxPanel(parent) {
  BuildUi();
  BindEvents();
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const git::CivilTime wall = now.valid ? now.wall : git::CivilTime{};
  ApplyProgrammaticTimes(wall, wall);
  RefreshOffsetText();
}

void CommitFormPanel::BuildUi() {
  auto* root = new wxBoxSizer(wxVERTICAL);

  auto* subjectRow = new wxBoxSizer(wxHORIZONTAL);
  subjectRow->Add(new wxStaticText(this, wxID_ANY, FromWide(L"标题")), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  subject_ = new wxTextCtrl(this, wxID_ANY);
  subject_->SetHint(FromWide(L"必填，单行；不检查 Conventional Commits 前缀"));
  subjectRow->Add(subject_, 1, wxEXPAND);
  root->Add(subjectRow, 0, wxEXPAND | wxBOTTOM, 6);

  root->Add(new wxStaticText(this, wxID_ANY,
                             FromWide(L"描述（可多行；行尾统一为 LF，首尾空行去掉，正文不润色）")),
            0, wxBOTTOM, 2);
  description_ = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition, wxSize(-1, 72),
                                wxTE_MULTILINE | wxTE_RICH2);
  root->Add(description_, 0, wxEXPAND | wxBOTTOM, 6);

  auto* authorRow = new wxBoxSizer(wxHORIZONTAL);
  authorRow->Add(new wxStaticText(this, wxID_ANY, FromWide(L"作者")), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  author_ = new wxTextCtrl(this, wxID_ANY);
  author_->SetHint(FromWide(L"姓名 <邮箱>；初值来自这个仓库的有效 Git 配置，手动修改只作用于本次提交"));
  authorRow->Add(author_, 1, wxEXPAND);
  root->Add(authorRow, 0, wxEXPAND | wxBOTTOM, 6);

  auto* coauthorRow = new wxBoxSizer(wxHORIZONTAL);
  auto* coauthorLeft = new wxBoxSizer(wxVERTICAL);
  coauthorLeft->Add(new wxStaticText(this, wxID_ANY, FromWide(L"合作者")), 0, wxBOTTOM, 2);
  coauthorList_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 50), 0, nullptr,
                                wxLB_EXTENDED);
  coauthorLeft->Add(coauthorList_, 1, wxEXPAND);
  coauthorRow->Add(coauthorLeft, 1, wxEXPAND);
  auto* coauthorButtons = new wxBoxSizer(wxVERTICAL);
  coauthorAdd_ = new wxButton(this, wxID_ANY, FromWide(L"添加…"));
  coauthorEdit_ = new wxButton(this, wxID_ANY, FromWide(L"编辑…"));
  coauthorRemove_ = new wxButton(this, wxID_ANY, FromWide(L"删除"));
  coauthorButtons->Add(coauthorAdd_, 0, wxEXPAND | wxBOTTOM, 4);
  coauthorButtons->Add(coauthorEdit_, 0, wxEXPAND | wxBOTTOM, 4);
  coauthorButtons->Add(coauthorRemove_, 0, wxEXPAND);
  coauthorRow->Add(coauthorButtons, 0, wxLEFT | wxEXPAND, 8);
  coauthorEdit_->Disable();
  coauthorRemove_->Disable();
  root->Add(coauthorRow, 0, wxEXPAND | wxBOTTOM, 6);

  auto* timeHeader = new wxBoxSizer(wxHORIZONTAL);
  timeHeader->Add(new wxStaticText(this, wxID_ANY,
                                   FromWide(L"作者时间与提交者时间（各带明确时区偏移；控件年份 1970–2099）")),
                  1, wxALIGN_CENTER_VERTICAL);
  timeSync_ = new wxCheckBox(this, wxID_ANY, FromWide(L"时间同步修改"));
  timeSync_->SetValue(true);
  timeHeader->Add(timeSync_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  timeReset_ = new wxButton(this, wxID_ANY, FromWide(L"恢复当前时间"));
  timeHeader->Add(timeReset_, 0, wxALIGN_CENTER_VERTICAL);
  root->Add(timeHeader, 0, wxEXPAND | wxBOTTOM, 4);

  auto* timeGrid = new wxFlexGridSizer(2, 4, 6, 8);
  timeGrid->AddGrowableCol(1, 1);
  timeGrid->AddGrowableCol(3, 1);
  timeGrid->Add(new wxStaticText(this, wxID_ANY, FromWide(L"作者")), 0, wxALIGN_CENTER_VERTICAL);
  auto* authorTimeRow = new wxBoxSizer(wxHORIZONTAL);
  authorDate_ = new wxDatePickerCtrl(this, wxID_ANY, wxDefaultDateTime,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxDP_DEFAULT | wxDP_SHOWCENTURY);
  authorClock_ = new wxTimePickerCtrl(this, wxID_ANY, wxDefaultDateTime);
  authorTimeRow->Add(authorDate_, 2, wxEXPAND | wxRIGHT, 6);
  authorTimeRow->Add(authorClock_, 1, wxEXPAND);
  timeGrid->Add(authorTimeRow, 1, wxEXPAND);
  timeGrid->Add(new wxStaticText(this, wxID_ANY, FromWide(L"提交者")), 0, wxALIGN_CENTER_VERTICAL);
  auto* committerTimeRow = new wxBoxSizer(wxHORIZONTAL);
  committerDate_ = new wxDatePickerCtrl(this, wxID_ANY, wxDefaultDateTime,
                                        wxDefaultPosition, wxDefaultSize,
                                        wxDP_DEFAULT | wxDP_SHOWCENTURY);
  committerClock_ = new wxTimePickerCtrl(this, wxID_ANY, wxDefaultDateTime);
  committerTimeRow->Add(committerDate_, 2, wxEXPAND | wxRIGHT, 6);
  committerTimeRow->Add(committerClock_, 1, wxEXPAND);
  timeGrid->Add(committerTimeRow, 1, wxEXPAND);
  root->Add(timeGrid, 0, wxEXPAND | wxBOTTOM, 4);

  const wxDateTime lower(1, wxDateTime::Jan, 1970, 0, 0, 0);
  const wxDateTime upper(31, wxDateTime::Dec, 2099, 23, 59, 59);
  authorDate_->SetRange(lower, upper);
  committerDate_->SetRange(lower, upper);

  offsetText_ = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                                 wxST_NO_AUTORESIZE);
  root->Add(offsetText_, 0, wxEXPAND);

  SetSizer(root);
}

void CommitFormPanel::BindEvents() {
  subject_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
    if (settingFields_) {
      return;
    }
    if (onFieldEdited) {
      onFieldEdited(app::CommitFormSession::Field::subject);
    }
    if (onChanged) {
      onChanged();
    }
  });
  description_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
    if (settingFields_) {
      return;
    }
    if (onFieldEdited) {
      onFieldEdited(app::CommitFormSession::Field::description);
    }
    if (onChanged) {
      onChanged();
    }
  });
  author_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
    if (settingFields_) {
      return;
    }
    if (author_->GetValue().empty()) {
      if (onAuthorCleared) {
        onAuthorCleared();
      }
    } else if (onFieldEdited) {
      onFieldEdited(app::CommitFormSession::Field::author);
    }
    if (onChanged) {
      onChanged();
    }
  });

  authorDate_->Bind(wxEVT_DATE_CHANGED, [this](wxDateEvent&) {
    if (settingTimes_) {
      return;
    }
    timesUserEdited_ = true;
    if (timeSync_->GetValue()) {
      MirrorCommitterToAuthor();
    }
    RefreshOffsetText();
    if (onTimesEdited) {
      onTimesEdited();
    }
    if (onChanged) {
      onChanged();
    }
  });
  authorClock_->Bind(wxEVT_TIME_CHANGED, [this](wxDateEvent&) {
    if (settingTimes_) {
      return;
    }
    timesUserEdited_ = true;
    if (timeSync_->GetValue()) {
      MirrorCommitterToAuthor();
    }
    RefreshOffsetText();
    if (onTimesEdited) {
      onTimesEdited();
    }
    if (onChanged) {
      onChanged();
    }
  });
  committerDate_->Bind(wxEVT_DATE_CHANGED, [this](wxDateEvent&) {
    if (settingTimes_) {
      return;
    }
    timesUserEdited_ = true;
    RefreshOffsetText();
    if (onTimesEdited) {
      onTimesEdited();
    }
    if (onChanged) {
      onChanged();
    }
  });
  committerClock_->Bind(wxEVT_TIME_CHANGED, [this](wxDateEvent&) {
    if (settingTimes_) {
      return;
    }
    timesUserEdited_ = true;
    RefreshOffsetText();
    if (onTimesEdited) {
      onTimesEdited();
    }
    if (onChanged) {
      onChanged();
    }
  });

  timeSync_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    if (timeSync_->GetValue()) {
      MirrorCommitterToAuthor();
    }
    UpdateCommitterEnabled();
    RefreshOffsetText();
    if (onChanged) {
      onChanged();
    }
  });
  timeReset_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const platform::LocalInstant now = platform::CurrentLocalInstant();
    if (!now.valid) {
      wxMessageBox(FromWide(L"无法取得本机当前时间：" + now.failureReason), FromWide(L"恢复当前时间"),
                   wxOK | wxICON_ERROR, this);
      return;
    }
    ApplyProgrammaticTimes(now.wall, now.wall);
    timesUserEdited_ = false;
    RefreshOffsetText();
    if (onChanged) {
      onChanged();
    }
  });

  coauthorAdd_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddCoauthor(); });
  coauthorEdit_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditCoauthor(); });
  coauthorRemove_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveCoauthors(); });
  coauthorList_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { EditCoauthor(); });
  auto updateCoauthorButtons = [this]() {
    size_t selected = 0;
    for (unsigned int index = 0; index < coauthorList_->GetCount(); ++index) {
      if (coauthorList_->IsSelected(index)) {
        ++selected;
      }
    }
    coauthorEdit_->Enable(selected == 1);
    coauthorRemove_->Enable(selected >= 1);
  };
  coauthorList_->Bind(wxEVT_LISTBOX, [updateCoauthorButtons](wxCommandEvent&) {
    updateCoauthorButtons();
  });
}

git::CommitFormData CommitFormPanel::Capture() const {
  git::CommitFormData data;
  data.subject = ToWide(subject_->GetValue());
  data.description = ToWide(description_->GetValue());
  data.author = ToWide(author_->GetValue());
  data.coauthors = coauthors_;
  return data;
}

git::CivilTime CommitFormPanel::AuthorWallTime() const {
  const wxDateTime date = authorDate_->GetValue();
  const wxDateTime clock = authorClock_->GetValue();
  return git::CivilTime{date.GetYear(), static_cast<int>(date.GetMonth()) + 1, date.GetDay(),
                        clock.GetHour(), clock.GetMinute(), clock.GetSecond()};
}

git::CivilTime CommitFormPanel::CommitterWallTime() const {
  if (timeSync_->GetValue()) {
    return AuthorWallTime();
  }
  const wxDateTime date = committerDate_->GetValue();
  const wxDateTime clock = committerClock_->GetValue();
  return git::CivilTime{date.GetYear(), static_cast<int>(date.GetMonth()) + 1, date.GetDay(),
                        clock.GetHour(), clock.GetMinute(), clock.GetSecond()};
}

bool CommitFormPanel::TimeSyncChecked() const { return timeSync_->GetValue(); }

std::wstring CommitFormPanel::AuthorText() const { return ToWide(author_->GetValue()); }

void CommitFormPanel::SetAuthorProgrammatically(const std::wstring& identity) {
  settingFields_ = true;
  author_->SetValue(FromWide(identity));
  settingFields_ = false;
}

void CommitFormPanel::SetTimes(const git::CivilTime& author, const git::CivilTime& committer,
                               bool clearUserEdited) {
  ApplyProgrammaticTimes(author, committer);
  if (clearUserEdited) {
    timesUserEdited_ = false;
  }
  RefreshOffsetText();
  if (onChanged) {
    onChanged();
  }
}

void CommitFormPanel::SetFormData(const git::CommitFormData& data) {
  settingFields_ = true;
  subject_->SetValue(FromWide(data.subject));
  description_->SetValue(FromWide(data.description));
  author_->SetValue(FromWide(data.author));
  coauthors_ = data.coauthors;
  RebuildCoauthorList();
  settingFields_ = false;
}

void CommitFormPanel::SetTimeSyncChecked(bool checked) {
  timeSync_->SetValue(checked);
  UpdateCommitterEnabled();
  RefreshOffsetText();
}

void CommitFormPanel::FocusSubject() {
  if (subject_ != nullptr) {
    subject_->SetFocus();
  }
}

void CommitFormPanel::MirrorCommitterToAuthor() {
  const git::CivilTime author = AuthorWallTime();
  const wxDateTime value = ToWxDateTime(author);
  settingTimes_ = true;
  committerDate_->SetValue(value);
  committerClock_->SetValue(value);
  settingTimes_ = false;
}

void CommitFormPanel::ClearCommittedFields(bool subject, bool description, bool coauthors) {
  settingFields_ = true;
  if (subject) {
    subject_->Clear();
  }
  if (description) {
    description_->Clear();
  }
  if (coauthors) {
    coauthors_.clear();
    RebuildCoauthorList();
  }
  settingFields_ = false;
}

void CommitFormPanel::ClearAllFields() {
  settingFields_ = true;
  subject_->Clear();
  description_->Clear();
  author_->Clear();
  coauthors_.clear();
  RebuildCoauthorList();
  settingFields_ = false;
}

void CommitFormPanel::ApplyProgrammaticTimes(const git::CivilTime& author,
                                             const git::CivilTime& committer) {
  const wxDateTime authorValue = ToWxDateTime(author);
  const wxDateTime committerValue = ToWxDateTime(committer);
  settingTimes_ = true;
  authorDate_->SetValue(authorValue);
  authorClock_->SetValue(authorValue);
  committerDate_->SetValue(committerValue);
  committerClock_->SetValue(committerValue);
  settingTimes_ = false;
  UpdateCommitterEnabled();
}

void CommitFormPanel::UpdateCommitterEnabled() {
  const bool enabled = !timeSync_->GetValue();
  committerDate_->Enable(enabled);
  committerClock_->Enable(enabled);
}

void CommitFormPanel::RefreshOffsetText() {
  const std::wstring zone = platform::LocalTimeZoneName();
  std::wstring text = zone.empty() ? L"本机时区未知" : L"本机时区：" + zone;
  text += L"；所选作者时间 " + OffsetFor(AuthorWallTime());
  if (!timeSync_->GetValue()) {
    text += L"；所选提交者时间 " + OffsetFor(CommitterWallTime());
  } else {
    text += L"；提交者时间随作者时间同步。";
  }
  offsetText_->SetLabel(FromWide(text));
  offsetText_->Wrap(GetClientSize().GetWidth() > 100 ? GetClientSize().GetWidth() : 900);
  Layout();
}

void CommitFormPanel::AddCoauthor() {
  std::wstring value;
  if (!PromptCoauthor(false, 0, &value)) {
    return;
  }
  coauthors_.push_back(value);
  RebuildCoauthorList(coauthors_.size() - 1);
  if (onFieldEdited) {
    onFieldEdited(app::CommitFormSession::Field::coauthors);
  }
  if (onChanged) {
    onChanged();
  }
}

void CommitFormPanel::EditCoauthor() {
  int selection = coauthorList_->GetSelection();
  if (selection == wxNOT_FOUND) {
    return;
  }
  std::wstring value;
  if (!PromptCoauthor(true, static_cast<size_t>(selection), &value)) {
    return;
  }
  coauthors_[static_cast<size_t>(selection)] = value;
  RebuildCoauthorList(static_cast<size_t>(selection));
  if (onFieldEdited) {
    onFieldEdited(app::CommitFormSession::Field::coauthors);
  }
  if (onChanged) {
    onChanged();
  }
}

void CommitFormPanel::RemoveCoauthors() {
  std::vector<size_t> rows;
  for (unsigned int index = 0; index < coauthorList_->GetCount(); ++index) {
    if (coauthorList_->IsSelected(index)) {
      rows.push_back(index);
    }
  }
  if (rows.empty()) {
    return;
  }
  std::sort(rows.rbegin(), rows.rend());
  for (const size_t row : rows) {
    if (row < coauthors_.size()) {
      coauthors_.erase(coauthors_.begin() + static_cast<std::ptrdiff_t>(row));
    }
  }
  RebuildCoauthorList();
  if (onFieldEdited) {
    onFieldEdited(app::CommitFormSession::Field::coauthors);
  }
  if (onChanged) {
    onChanged();
  }
}

bool CommitFormPanel::PromptCoauthor(bool editing, size_t row, std::wstring* out) const {
  const std::wstring initial = editing && row < coauthors_.size() ? coauthors_[row] : std::wstring();
  CoauthorDialog dialog(const_cast<CommitFormPanel*>(this), initial,
                        [this, editing, row](std::wstring_view text) -> std::wstring {
                          std::wstring reason = git::ValidateGitIdentity(text, L"合作者");
                          if (!reason.empty()) {
                            return reason;
                          }
                          const std::wstring key = git::CanonicalIdentityKey(text);
                          for (size_t index = 0; index < coauthors_.size(); ++index) {
                            if (editing && index == row) {
                              continue;
                            }
                            if (git::CanonicalIdentityKey(coauthors_[index]) == key) {
                              return L"这一位已经在合作者列表里（姓名与邮箱都相同）。";
                            }
                          }
                          return {};
                        });
  if (dialog.ShowModal() != wxID_OK) {
    return false;
  }
  *out = dialog.Value();
  return true;
}

void CommitFormPanel::RebuildCoauthorList(size_t selectedRow) {
  const int keep = selectedRow == static_cast<size_t>(-1) ? wxNOT_FOUND
                                                          : static_cast<int>(selectedRow);
  coauthorList_->Clear();
  for (const std::wstring& entry : coauthors_) {
    coauthorList_->Append(FromWide(entry));
  }
  if (keep != wxNOT_FOUND && static_cast<unsigned int>(keep) < coauthorList_->GetCount()) {
    coauthorList_->SetSelection(keep);
  }
  const size_t selected = coauthorList_->GetSelection() == wxNOT_FOUND ? 0 : 1;
  coauthorEdit_->Enable(selected == 1);
  coauthorRemove_->Enable(selected == 1);
}

}  // namespace gc::ui::wx
