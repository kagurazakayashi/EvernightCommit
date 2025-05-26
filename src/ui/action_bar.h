#pragma once

#include <windows.h>

#include <array>
#include <string_view>

#include "ui/controls.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 底部操作栏，两行：
// 上一行是 Git 操作按钮——刷新、创建提交、撤回最近提交、推送，以及冲突与暂停流程的三个入口，
// 后面跟着状态说明。三个冲突入口排在最后并彼此相邻：它们做的是同一件事的三种看法
// （看清现场 / 把 Git 的 --continue 交出去 / 把 --abort 交出去），可用性还额外取决于
// 仓库里此刻有没有流程停着，与前面那四个「随时可用」的按钮不是一类。
// 下一行从「保存记录」起，是不碰仓库、不需要 Git 可用、任何时候都可点的持久化区：记录开关
// 与清除入口为一组，操作历史开关与其查看入口为另一组，两组之间留出比单个控件间隔更宽的缝。
class ActionBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void SetStatus(std::wstring_view text);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 两行都不被裁剪时所需的最小宽度：Git 操作按钮加状态说明，与整条持久化区取较宽的一行。
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
  // 操作历史（可选功能）：总开关与「操作历史…」入口。它不碰仓库、不需要 Git 可用，任何时候都可点。
  [[nodiscard]] HWND historyCheck() const noexcept { return historyCheck_; }
  [[nodiscard]] HWND historyBrowseButton() const noexcept { return historyBrowse_; }

  // 程序改写开关状态（首次说明的选择、读回来的记录）：BM_SETCHECK 不会发 BN_CLICKED，
  // 不会被当成用户又点了一次。
  void SetPreferenceChecks(bool records, bool drafts);

private:
  // 单行摆放的全部操作按钮（顺序即从左到右的显示顺序）：MinimumWidth 与 Layout 共用这一份，
  // 免得两处各自维护一份列表而漏掉新按钮（新按钮因此不会在最小宽度里凭空少算一格）。
  [[nodiscard]] std::array<HWND, 7> Buttons() const noexcept;
  // 持久化区的两个分组（顺序即显示顺序）：记录开关 + 清除入口，操作历史开关 + 查看入口。
  // 与 Buttons() 同样只在这一处列名单，MinimumWidth 与 Layout 共用，新控件不会凭空少算一格。
  [[nodiscard]] std::array<HWND, 3> RecordControls() const noexcept;
  [[nodiscard]] std::array<HWND, 2> HistoryControls() const noexcept;
  // 单个持久化控件的应占宽度（复选框按文字加方框、按钮按按钮宽度）。
  [[nodiscard]] int PreferenceControlWidth(HWND control, const UiMetrics& metrics) const;
  // 分组之间的间隔：明显宽于控件间隔，才看得出「这是两组」。
  [[nodiscard]] static int GroupGap(const UiMetrics& metrics) noexcept;

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
  HWND historyCheck_ = nullptr;
  HWND historyBrowse_ = nullptr;
  HWND status_ = nullptr;
};

}  // namespace gc::ui
