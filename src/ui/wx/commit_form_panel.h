#pragma once

#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "app/commit_form_session.h"
#include "git/commit_date.h"
#include "git/commit_message.h"

class wxButton;
class wxCheckBox;
class wxDatePickerCtrl;
class wxListBox;
class wxStaticText;
class wxTextCtrl;
class wxTimePickerCtrl;

namespace gc::ui::wx {

// 提交表单：标题、多行描述、作者、合作者列表、作者时间与提交者时间。
//
// 只负责控件与一份表单数据之间的搬运与交互；身份解析、校验与消息合成仍由
// git/commit_identity、git/commit_message 决定。日期与时分秒分成两块控件，
// 与合作者列表一样按「行号 == 下标」维护内部模型，绝不从显示文字反解参数。
class CommitFormPanel final : public wxPanel {
public:
  explicit CommitFormPanel(wxWindow* parent);

  // 父窗口用它知道「用户动了哪一栏」，以维护 CommitFormSession 的默认值记号。
  std::function<void(app::CommitFormSession::Field)> onFieldEdited;
  // 时间控件被用户改动（包括取消同步后单独改提交者时间）。
  std::function<void()> onTimesEdited;
  // 作者栏被清空：这是「回到配置里的默认身份」的明确退路。
  std::function<void()> onAuthorCleared;
  // 任一内容或时间变化之后：父窗口重跑校验、刷新偏移说明。
  std::function<void()> onChanged;

  [[nodiscard]] git::CommitFormData Capture() const;
  [[nodiscard]] git::CivilTime AuthorWallTime() const;
  [[nodiscard]] git::CivilTime CommitterWallTime() const;
  [[nodiscard]] bool TimeSyncChecked() const;
  [[nodiscard]] bool TimesUserEdited() const noexcept { return timesUserEdited_; }
  [[nodiscard]] std::wstring AuthorText() const;

  // 程序填写作者默认值：不记成用户编辑。
  void SetAuthorProgrammatically(const std::wstring& identity);
  // 整份替代表单内容（子模块导航交还草稿等）；不改变时间的「用户改过」记号。
  void SetFormData(const git::CommitFormData& data);
  void SetTimeSyncChecked(bool checked);
  void MarkTimesUserEdited() noexcept { timesUserEdited_ = true; }
  // 把两个时间一起设成给出的墙上时间；clearUserEdited=true 表示「恢复当前时间」那条退路。
  void SetTimes(const git::CivilTime& author, const git::CivilTime& committer,
                bool clearUserEdited);
  // 只把提交者控件跟到作者时间（用户改作者时间引起的联动，不抹掉「用户改过时间」的记号）。
  void MirrorCommitterToAuthor();

  // 提交成功后的逐栏清理：作者栏永远留着；时间只在调用方点名时才回到此刻。
  void ClearCommittedFields(bool subject, bool description, bool coauthors);
  // 换仓库且用户选择放弃时整体清空；时间由随后到达的默认值另行设置。
  void ClearAllFields();

  void FocusSubject();

private:
  void BuildUi();
  void BindEvents();
  void ApplyProgrammaticTimes(const git::CivilTime& author, const git::CivilTime& committer);
  void UpdateCommitterEnabled();
  void RefreshOffsetText();
  void AddCoauthor();
  void EditCoauthor();
  void RemoveCoauthors();
  [[nodiscard]] bool PromptCoauthor(bool editing, size_t row, std::wstring* out) const;
  void RebuildCoauthorList(size_t selectedRow = static_cast<size_t>(-1));

  wxTextCtrl* subject_ = nullptr;
  wxTextCtrl* description_ = nullptr;
  wxTextCtrl* author_ = nullptr;
  wxListBox* coauthorList_ = nullptr;
  wxButton* coauthorAdd_ = nullptr;
  wxButton* coauthorEdit_ = nullptr;
  wxButton* coauthorRemove_ = nullptr;
  wxDatePickerCtrl* authorDate_ = nullptr;
  wxTimePickerCtrl* authorClock_ = nullptr;
  wxDatePickerCtrl* committerDate_ = nullptr;
  wxTimePickerCtrl* committerClock_ = nullptr;
  wxCheckBox* timeSync_ = nullptr;
  wxButton* timeReset_ = nullptr;
  wxStaticText* offsetText_ = nullptr;

  std::vector<std::wstring> coauthors_;
  bool settingFields_ = false;
  bool settingTimes_ = false;
  bool timesUserEdited_ = false;
};

}  // namespace gc::ui::wx
