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

// 第二行：仓库类型、当前分支与上游，右端是子模块导航的两个按钮。
//
// 操作结论与仓库状态不再在这里重复一份：那类文字统一由窗口底部状态栏（ActionBar 的 status 标签）
// 呈现，两处显示同一句话只会让使用者不知道该看哪一行。
//
// 子模块导航按钮摆在「上游：」这条带上：它换掉的是整个仓库绑定，与「当前分支/上游」是同一件事，
// 因此在仓库信息这一行右对齐，而不挤在变更区的暂存按钮那一栏里。
class RepoInfoBar {
public:
  void Create(HWND parent);
  void Layout(const RECT& area, const UiMetrics& metrics);
  void Refresh(std::wstring type, std::wstring branch, std::wstring upstream);
  [[nodiscard]] static int MinimumHeight(const UiMetrics& metrics) noexcept;
  // 三段单行说明文字与导航按钮都不被裁剪时所需的最小宽度。
  [[nodiscard]] int MinimumWidth(const UiMetrics& metrics) const;

  [[nodiscard]] HWND enterSubmoduleButton() const noexcept { return enterSubmodule_; }
  [[nodiscard]] HWND returnToParentButton() const noexcept { return returnToParent_; }

private:
  HWND type_ = nullptr;
  HWND branch_ = nullptr;
  HWND upstream_ = nullptr;
  HWND enterSubmodule_ = nullptr;
  HWND returnToParent_ = nullptr;
};

}  // namespace gc::ui
