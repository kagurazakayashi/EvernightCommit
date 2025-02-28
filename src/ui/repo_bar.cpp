#include "ui/repo_bar.h"

#include <algorithm>
#include <string>

#include "ui/commands.h"
#include "ui/controls.h"

namespace gc::ui {
namespace {

void Place(HWND target, const RECT& rect) {
  if (target != nullptr) {
    ::MoveWindow(target, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
  }
}

constexpr RECT Row(int left, int top, int width, int height) noexcept {
  return RECT{left, top, left + width, top + height};
}

}  // namespace

int RepoBar::MinimumHeight(const UiMetrics& metrics) noexcept {
  return 2 * metrics.ControlHeight() + metrics.RowGap();
}

int RepoBar::MinimumWidth(const UiMetrics& metrics) const {
  const int colGap = metrics.ColGap();
  const int labelWidth =
      std::max(metrics.LabelWidth(L"本地仓库："), metrics.LabelWidth(L"Git 程序："));
  const int browseWidth = std::max(metrics.ButtonWidth(L"浏览…"), metrics.Scale(72));
  return labelWidth + colGap + metrics.Scale(90) + colGap + browseWidth + colGap + ToolbarWidth(metrics);
}

void RepoBar::Create(HWND parent) {
  repoLabel_ = CreateLabel(parent, L"本地仓库：", kIdRepoLabel);
  repoEdit_ = CreateSingleLineEdit(parent, L"", kIdRepoEdit);
  repoBrowse_ = CreatePushButton(parent, L"浏览…", kIdRepoBrowse);

  gitLabel_ = CreateLabel(parent, L"Git 程序：", kIdGitLabel);
  gitCombo_ = CreateEditableCombo(parent, kIdGitCombo);
  gitBrowse_ = CreatePushButton(parent, L"浏览…", kIdGitBrowse);

  fetch_ = CreatePushButton(parent, L"fetch", kIdFetchButton);
  pull_ = CreatePushButton(parent, L"pull", kIdPullButton);
  status_ = CreatePushButton(parent, L"status", kIdStatusButton);
}

int RepoBar::ToolbarWidth(const UiMetrics& metrics) const {
  const int width = metrics.ButtonWidth(L"fetch") + metrics.ButtonWidth(L"pull") + metrics.ButtonWidth(L"status");
  return width + 2 * metrics.ColGap();
}

void RepoBar::SetGitCandidates(const std::vector<std::wstring>& candidates) {
  SetComboCandidates(gitCombo_, candidates);
}

void RepoBar::Layout(const RECT& area, const UiMetrics& metrics) {
  const int gap = metrics.RowGap();
  const int colGap = metrics.ColGap();
  const int rowHeight = metrics.ControlHeight();
  const int totalHeight = area.bottom - area.top;

  const int toolbarWidth = ToolbarWidth(metrics);
  const int leftWidth = std::max<int>(metrics.Scale(220), (area.right - area.left) - toolbarWidth - colGap);

  const int labelWidth =
      std::max(metrics.LabelWidth(L"本地仓库："), metrics.LabelWidth(L"Git 程序："));
  const int browseWidth = std::max(metrics.ButtonWidth(L"浏览…"), metrics.Scale(72));
  const int editWidth =
      std::max(metrics.Scale(90), leftWidth - labelWidth - browseWidth - 2 * colGap);

  const int firstTop = area.top;
  const int secondTop = firstTop + rowHeight + gap;
  const int x = area.left;

  Place(repoLabel_, Row(x, firstTop + (rowHeight - metrics.LabelHeight()) / 2, labelWidth, metrics.LabelHeight()));
  Place(repoEdit_, Row(x + labelWidth + colGap, firstTop, editWidth, rowHeight));
  Place(repoBrowse_, Row(x + labelWidth + colGap + editWidth + colGap, firstTop, browseWidth, rowHeight));

  Place(gitLabel_, Row(x, secondTop + (rowHeight - metrics.LabelHeight()) / 2, labelWidth, metrics.LabelHeight()));
  // 组合框窗口高度包含下拉列表区域：给足高度，长路径候选也能完整滚动查看。
  Place(gitCombo_, Row(x + labelWidth + colGap, secondTop, editWidth, metrics.Scale(240)));
  Place(gitBrowse_, Row(x + labelWidth + colGap + editWidth + colGap, secondTop, browseWidth, rowHeight));

  int toolbarX = x + leftWidth + colGap;
  const int buttonTop = area.top + (totalHeight - rowHeight) / 2;
  for (HWND button : {fetch_, pull_, status_}) {
    const int width = metrics.ButtonWidth(GetControlText(button));
    Place(button, Row(toolbarX, buttonTop, width, rowHeight));
    toolbarX += width + colGap;
  }
}

int RepoInfoBar::MinimumHeight(const UiMetrics& metrics) noexcept {
  return metrics.LabelHeight() + metrics.Scale(4);
}

int RepoInfoBar::MinimumWidth(const UiMetrics& metrics) const {
  const int gap = metrics.ColGap() * 2;
  return metrics.LabelWidth(GetControlText(branch_)) + gap + metrics.LabelWidth(GetControlText(upstream_)) + gap +
         metrics.LabelWidth(GetControlText(task_)) + gap + metrics.LabelWidth(GetControlText(program_));
}

void RepoInfoBar::Create(HWND parent) {
  branch_ = CreateLabel(parent, L"当前分支：", kIdBranchLabel);
  upstream_ = CreateLabel(parent, L"上游：", kIdUpstreamLabel);
  task_ = CreateLabel(parent, L"任务状态：", kIdTaskLabel);
  program_ = CreateLabel(parent, L"", kIdAppInfoLabel);
}

void RepoInfoBar::Refresh(std::wstring branch, std::wstring upstream, std::wstring task, std::wstring program) {
  SetControlText(branch_, std::move(branch));
  SetControlText(upstream_, std::move(upstream));
  SetControlText(task_, std::move(task));
  SetControlText(program_, std::move(program));
}

void RepoInfoBar::Layout(const RECT& area, const UiMetrics& metrics) {
  const int gap = metrics.ColGap() * 2;
  const int height = metrics.LabelHeight();
  const int top = area.top + ((area.bottom - area.top) - height) / 2;

  const int branchWidth = metrics.LabelWidth(GetControlText(branch_));
  const int upstreamWidth = metrics.LabelWidth(GetControlText(upstream_));
  const int programWidth = metrics.LabelWidth(GetControlText(program_));

  int x = area.left;
  Place(branch_, Row(x, top, branchWidth, height));
  x += branchWidth + gap;
  Place(upstream_, Row(x, top, upstreamWidth, height));
  x += upstreamWidth + gap;
  Place(program_, Row(area.right - programWidth, top, programWidth, height));
  const int taskWidth = std::max<int>(metrics.Scale(120), (area.right - programWidth - gap) - x);
  Place(task_, Row(x, top, taskWidth, height));
}

}  // namespace gc::ui
