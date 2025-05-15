#pragma once

#include <windows.h>

#include <array>
#include <string_view>

#include "ui/controls.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

// 底部操作栏：刷新、创建提交、撤回最近提交、推送、冲突与暂停流程的三个入口，
// 以及右侧的状态说明。三个冲突入口排在最后并彼此相邻：它们做的是同一件事的三种看法
// （看清现场 / 把 Git 的 --continue 交出去 / 把 --abort 交出去），可用性还额外取决于
// 仓库里此刻有没有流程停着，与前面那四个「随时可用」的按钮不是一类。
class ActionBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void SetStatus(std::wstring_view text);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 七个按钮与状态说明都不被裁剪时所需的最小宽度。
  [[nodiscard]] int MinimumWidth(const UiMetrics& metrics) const;

  [[nodiscard]] HWND refreshButton() const noexcept { return refresh_; }
  [[nodiscard]] HWND createCommitButton() const noexcept { return createCommit_; }
  [[nodiscard]] HWND undoCommitButton() const noexcept { return undoCommit_; }
  [[nodiscard]] HWND pushButton() const noexcept { return push_; }
  [[nodiscard]] HWND conflictViewButton() const noexcept { return conflictView_; }
  [[nodiscard]] HWND conflictContinueButton() const noexcept { return conflictContinue_; }
  [[nodiscard]] HWND conflictAbortButton() const noexcept { return conflictAbort_; }

private:
  // 单行摆放的全部按钮（顺序即从左到右的显示顺序）：MinimumWidth 与 Layout 共用这一份，
  // 免得两处各自维护一份列表而漏掉新按钮（新按钮因此不会在最小宽度里凭空少算一格）。
  [[nodiscard]] std::array<HWND, 7> Buttons() const noexcept;

  HWND refresh_ = nullptr;
  HWND createCommit_ = nullptr;
  HWND undoCommit_ = nullptr;
  HWND push_ = nullptr;
  HWND conflictView_ = nullptr;
  HWND conflictContinue_ = nullptr;
  HWND conflictAbort_ = nullptr;
  HWND status_ = nullptr;
};

}  // namespace gc::ui
