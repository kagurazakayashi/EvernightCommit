#pragma once

#include <windows.h>

#include <array>
#include <string_view>

#include "ui/controls.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 底部操作栏：刷新、创建提交、撤回最近提交、推送、冲突与暂停流程的三个入口，
// 右侧的持久化开关区（保存记录 / 保存草稿 / 清除已存记录）与状态说明。
// 三个冲突入口排在最后并彼此相邻：它们做的是同一件事的三种看法
// （看清现场 / 把 Git 的 --continue 交出去 / 把 --abort 交出去），可用性还额外取决于
// 仓库里此刻有没有流程停着，与前面那四个「随时可用」的按钮不是一类。
// 持久化区与 Git 操作不是一类：它不碰仓库、不需要 Git 可用，任何时候都可点。
class ActionBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void SetStatus(std::wstring_view text);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 全部按钮、持久化区与状态说明都不被裁剪时所需的最小宽度。
  [[nodiscard]] int MinimumWidth(const UiMetrics& metrics) const;

  [[nodiscard]] HWND refreshButton() const noexcept { return refresh_; }
  [[nodiscard]] HWND createCommitButton() const noexcept { return createCommit_; }
  [[nodiscard]] HWND undoCommitButton() const noexcept { return undoCommit_; }
  [[nodiscard]] HWND pushButton() const noexcept { return push_; }
  [[nodiscard]] HWND conflictViewButton() const noexcept { return conflictView_; }
  [[nodiscard]] HWND conflictContinueButton() const noexcept { return conflictContinue_; }
  [[nodiscard]] HWND conflictAbortButton() const noexcept { return conflictAbort_; }
  [[nodiscard]] HWND persistRecordsCheck() const noexcept { return persistRecords_; }
  [[nodiscard]] HWND persistDraftsCheck() const noexcept { return persistDrafts_; }
  [[nodiscard]] HWND clearPrefsButton() const noexcept { return clearPrefs_; }

  // 程序改写开关状态（首次说明的选择、读回来的记录）：BM_SETCHECK 不会发 BN_CLICKED，
  // 不会被当成用户又点了一次。
  void SetPreferenceChecks(bool records, bool drafts);

private:
  // 单行摆放的全部操作按钮（顺序即从左到右的显示顺序）：MinimumWidth 与 Layout 共用这一份，
  // 免得两处各自维护一份列表而漏掉新按钮（新按钮因此不会在最小宽度里凭空少算一格）。
  [[nodiscard]] std::array<HWND, 7> Buttons() const noexcept;
  // 持久化区的三个控件，同样只在这一处列名单。
  [[nodiscard]] std::array<HWND, 3> PreferenceControls() const noexcept;
  // 单个持久化控件的应占宽度（复选框按文字加方框、按钮按按钮宽度）。
  [[nodiscard]] int PreferenceControlWidth(HWND control, const UiMetrics& metrics) const;

  HWND refresh_ = nullptr;
  HWND createCommit_ = nullptr;
  HWND undoCommit_ = nullptr;
  HWND push_ = nullptr;
  HWND conflictView_ = nullptr;
  HWND conflictContinue_ = nullptr;
  HWND conflictAbort_ = nullptr;
  HWND persistRecords_ = nullptr;
  HWND persistDrafts_ = nullptr;
  HWND clearPrefs_ = nullptr;
  HWND status_ = nullptr;
};

}  // namespace gc::ui
