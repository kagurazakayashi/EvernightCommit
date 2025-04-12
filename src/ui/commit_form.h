#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

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
  void SetTimes(const SYSTEMTIME& authorTime, const SYSTEMTIME& committerTime);

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

  [[nodiscard]] HWND SummaryEdit() const noexcept { return summary_; }
  [[nodiscard]] HWND DescriptionEdit() const noexcept { return description_; }
  [[nodiscard]] HWND AuthorEdit() const noexcept { return author_; }
  [[nodiscard]] HWND CoauthorList() const noexcept { return coauthorList_; }

  [[nodiscard]] HWND SyncCheckbox() const noexcept { return timeSync_; }
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
  std::vector<std::wstring> coauthors_;
};

}  // namespace gc::ui
