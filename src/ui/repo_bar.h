#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ui/ui_metrics.h"

namespace gc::ui {

// 顶部“本地仓库 / Git 程序”两行输入，以及右侧的仓库级操作按钮（fetch、pull、status）。
class RepoBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 两行输入 + 仓库工具栏都保持可见时所需的最小宽度。
  [[nodiscard]] int MinimumWidth(const UiMetrics& metrics) const;

  // 填充“Git 程序”下拉候选列表（完整路径），保持当前文本。
  void SetGitCandidates(const std::vector<std::wstring>& candidates);

  [[nodiscard]] HWND repoEdit() const noexcept { return repoEdit_; }
  [[nodiscard]] HWND gitCombo() const noexcept { return gitCombo_; }
  [[nodiscard]] HWND repoBrowse() const noexcept { return repoBrowse_; }
  [[nodiscard]] HWND gitBrowse() const noexcept { return gitBrowse_; }
  [[nodiscard]] HWND fetchButton() const noexcept { return fetch_; }
  [[nodiscard]] HWND pullButton() const noexcept { return pull_; }
  [[nodiscard]] HWND statusButton() const noexcept { return status_; }

private:
  [[nodiscard]] int ToolbarWidth(const UiMetrics& metrics) const;

  HWND repoLabel_ = nullptr;
  HWND repoEdit_ = nullptr;
  HWND repoBrowse_ = nullptr;
  HWND gitLabel_ = nullptr;
  HWND gitCombo_ = nullptr;
  HWND gitBrowse_ = nullptr;
  HWND fetch_ = nullptr;
  HWND pull_ = nullptr;
  HWND status_ = nullptr;
};

// 第二行：仓库类型、当前分支、上游、任务状态与程序信息。
class RepoInfoBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void Refresh(std::wstring type, std::wstring branch, std::wstring upstream, std::wstring task,
               std::wstring program);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 五段单行说明文字都不被裁剪时所需的最小宽度。
  [[nodiscard]] int MinimumWidth(const UiMetrics& metrics) const;

private:
  HWND type_ = nullptr;
  HWND branch_ = nullptr;
  HWND upstream_ = nullptr;
  HWND task_ = nullptr;
  HWND program_ = nullptr;
};

}  // namespace gc::ui
