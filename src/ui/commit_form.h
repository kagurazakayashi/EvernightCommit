#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_date.h"
#include "git/commit_message.h"
#include "ui/controls.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 提交表单：标题、描述、作者、合作者列表、作者时间与提交者时间。
// 每个时间由“日期 + 时分秒”两个 DateTimePicker 组成，均可用键盘直接输入。
//
// 本类只管「控件与一份表单数据之间的搬运」，不做任何格式判断：
// 身份解析、校验与消息合成都在 git/commit_identity、git/commit_message 里，
// 因此规则可以完全脱离窗口测试（见 tests/commit_*_tests.cpp）。
// 合作者列表的内部模型与控件行号一一对应（行号 == 下标），与工作区列表同一套约定；
// 显示用的文字就是数据本身，不从单元格文本反解任何东西。
class CommitForm {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void SetTimeZoneText(std::wstring_view text);
  // 程序把两块控件一起设成给出的时间，并撤掉「用户改过时间」的记号（见 TimesUserEdited）。
  void SetTimes(const SYSTEMTIME& authorTime, const SYSTEMTIME& committerTime);
  // 「时间同步修改」勾着时，把提交者那两块控件的显示改成与作者一致。
  // 与 SetTimes 的区别：这是用户改了作者时间引起的联动，不能把「用户改过时间」的记号抹掉。
  void MirrorCommitterTime();

  // ---- 时间控件 ----
  // 「日期」与「时、分、秒」两块控件各管一半，读回来要拼成一段墙上时间：
  // 时间型控件内部虽然也存日期，但用户看不见它，拿它当日期用会把「看不见的那一半」当真。
  [[nodiscard]] git::CivilTime AuthorWallTime() const;
  [[nodiscard]] git::CivilTime CommitterWallTime() const;
  // 收到 DTN_DATETIMECHANGE 时由界面调用：这一下是人改的，不是程序填的。
  void NoteTimesUserEdited() noexcept { timesUserEdited_ = true; }
  // 人有没有亲手改过时间。没人动过时，「创建提交」用提交那一刻的时间，
  // 而不是应用启动时被设进控件的那个时刻。
  [[nodiscard]] bool TimesUserEdited() const noexcept { return timesUserEdited_; }
  // 控件发来的时间变更通知算不算「用户改的」：程序自己 SetTimes 时同样会收到通知，
  // 那一次必须不算，否则「没人动过时间」这件事永远成立不了。
  [[nodiscard]] bool AcceptsTimeNotify() const noexcept { return !settingTimes_; }

  [[nodiscard]] bool TimeSyncChecked() const;
  // 「时间同步修改」的勾选状态。程序改动它时不经过用户点击，因此不需要记成用户编辑。
  void SetTimeSyncChecked(bool checked);
  // 勾选同步时提交者那两块控件置灰：联动规则要看得见，不能只写在说明里。
  void SetCommitterTimeEnabled(bool enabled);

  // ---- 表单数据 ----
  // 读取当前内容：标题、描述、作者取自控件文字，合作者取自内部模型。
  [[nodiscard]] git::CommitFormData Capture() const;
  void SetSummaryText(std::wstring_view text);
  void SetDescriptionText(std::wstring_view text);
  void SetAuthorText(std::wstring_view text);
  // 整份替换合作者列表（增删改都走这里，保证模型与列表同步）；同时切换“暂无合作者”提示。
  void SetCoauthors(std::vector<std::wstring> entries);
  [[nodiscard]] const std::vector<std::wstring>& Coauthors() const noexcept { return coauthors_; }
  [[nodiscard]] std::vector<int> SelectedCoauthorRows() const;
  // 重建列表后恢复选中项（行号会随增删变化，调用方按条目决定要选哪一行）。
  void SelectCoauthorRows(const std::vector<int>& rows);
  // 把整张表单倒回「空」：标题、描述、作者、合作者都清空。时间不动（由 SetTimes 单独控制）。
  void ClearFields();
  // 提交成功后的清理：只清「这次写了什么」（标题、描述、合作者），
  // 作者那一栏是可复用的身份要留着，时间则由调用方按提交那一刻重设。
  void ClearMessageFields();

  [[nodiscard]] HWND SummaryEdit() const noexcept { return summary_; }
  [[nodiscard]] HWND DescriptionEdit() const noexcept { return description_; }
  [[nodiscard]] HWND AuthorEdit() const noexcept { return author_; }
  [[nodiscard]] HWND CoauthorList() const noexcept { return coauthorList_; }

  [[nodiscard]] HWND SyncCheckbox() const noexcept { return timeSync_; }
  [[nodiscard]] HWND TimeResetButton() const noexcept { return timeReset_; }
  [[nodiscard]] HWND AuthorDate() const noexcept { return authorDate_; }
  [[nodiscard]] HWND AuthorClock() const noexcept { return authorClock_; }
  [[nodiscard]] HWND CommitterDate() const noexcept { return committerDate_; }
  [[nodiscard]] HWND CommitterClock() const noexcept { return committerClock_; }
  [[nodiscard]] HWND CoauthorAdd() const noexcept { return coauthorAdd_; }
  [[nodiscard]] HWND CoauthorRemove() const noexcept { return coauthorRemove_; }
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;

private:
  [[nodiscard]] static int MinDescriptionHeight(const UiMetrics& metrics) noexcept;
  [[nodiscard]] static int MinCoauthorHeight(const UiMetrics& metrics) noexcept;
  void RebuildCoauthorList();
  void ApplyCommitterTimeEnabled(bool enabled);

  HWND group_ = nullptr;
  HWND summaryLabel_ = nullptr;
  HWND summary_ = nullptr;
  HWND descriptionLabel_ = nullptr;
  HWND description_ = nullptr;
  HWND authorLabel_ = nullptr;
  HWND author_ = nullptr;
  HWND coauthorLabel_ = nullptr;
  HWND coauthorList_ = nullptr;
  HWND coauthorHint_ = nullptr;
  HWND coauthorAdd_ = nullptr;
  HWND coauthorRemove_ = nullptr;
  HWND authorTimeLabel_ = nullptr;
  HWND authorDate_ = nullptr;
  HWND authorClock_ = nullptr;
  HWND timeZone_ = nullptr;
  HWND committerTimeLabel_ = nullptr;
  HWND committerDate_ = nullptr;
  HWND committerClock_ = nullptr;
  HWND timeSync_ = nullptr;
  HWND timeReset_ = nullptr;
  std::vector<std::wstring> coauthors_;
  // 人有没有亲手改过时间：没人动过时「创建提交」按提交那一刻的时间，
  // 而不是应用启动时设进控件的那个时刻。
  bool timesUserEdited_ = false;
  // 程序自己 SetTimes 时控件同样会发 DTN_DATETIMECHANGE，那一次不能记成用户编辑。
  bool settingTimes_ = false;
};

}  // namespace gc::ui
