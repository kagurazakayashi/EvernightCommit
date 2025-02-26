#pragma once

#include <windows.h>

#include <string_view>
#include <vector>

#include "ui/controls.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 提交表单：标题、描述、作者、合作者列表、作者时间与提交者时间。
// 每个时间由“日期 + 时分秒”两个 DateTimePicker 组成，均可用键盘直接输入。
class CommitForm {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void SetTimeZoneText(std::wstring_view text);
  void SetTimes(const SYSTEMTIME& authorTime, const SYSTEMTIME& committerTime);

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
};

}  // namespace gc::ui
