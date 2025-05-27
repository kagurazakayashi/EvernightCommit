#include "ui/repo_bar.h"

#include <algorithm>
#include <string>

#include "ui/commands.h"
#include "ui/controls.h"

namespace gc::ui {
namespace {

void Place(HWND target, const RECT& rect) { PlaceIfChanged(target, rect); }

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
  // 导航按钮比单行文字高：这一带的高度按按钮算，否则按钮会溢出到变更区上去。
  return std::max(metrics.LabelHeight() + metrics.Scale(4), metrics.ControlHeight() + metrics.Scale(2));
}

int RepoInfoBar::MinimumWidth(const UiMetrics& metrics) const {
  const int gap = metrics.ColGap() * 2;
  const int buttons = metrics.ButtonWidth(L"进入子模块") + metrics.ButtonWidth(L"返回父仓库") +
                      metrics.ColGap();
  return metrics.LabelWidth(GetControlText(type_)) + gap + metrics.LabelWidth(GetControlText(branch_)) +
         gap + metrics.LabelWidth(GetControlText(upstream_)) + gap + buttons;
}

void RepoInfoBar::Create(HWND parent) {
  type_ = CreateLabel(parent, L"仓库类型：", kIdRepoTypeLabel);
  branch_ = CreateLabel(parent, L"当前分支：", kIdBranchLabel);
  upstream_ = CreateLabel(parent, L"上游：", kIdUpstreamLabel);
  enterSubmodule_ = CreatePushButton(parent, L"进入子模块", kIdEnterSubmoduleButton);
  returnToParent_ = CreatePushButton(parent, L"返回父仓库", kIdReturnToParentButton);
}

void RepoInfoBar::Refresh(std::wstring type, std::wstring branch, std::wstring upstream) {
  SetControlText(type_, std::move(type));
  SetControlText(branch_, std::move(branch));
  SetControlText(upstream_, std::move(upstream));
}

void RepoInfoBar::Layout(const RECT& area, const UiMetrics& metrics) {
  const int gap = metrics.ColGap() * 2;
  const int height = metrics.LabelHeight();
  const int top = area.top + ((area.bottom - area.top) - height) / 2;

  const int typeWidth = metrics.LabelWidth(GetControlText(type_));
  const int branchWidth = metrics.LabelWidth(GetControlText(branch_));
  const int upstreamWidth = metrics.LabelWidth(GetControlText(upstream_));

  int x = area.left;
  Place(type_, Row(x, top, typeWidth, height));
  x += typeWidth + gap;
  Place(branch_, Row(x, top, branchWidth, height));
  x += branchWidth + gap;
  Place(upstream_, Row(x, top, upstreamWidth, height));

  // 导航按钮在这一行右对齐（从左到右：进入子模块、返回父仓库）。
  // 条带不够宽时它们会往左挤到上游文字上——那种尺寸已经被 MinimumWidth 挡在最小窗口之外。
  const int rowHeight = metrics.ControlHeight();
  const int buttonTop = area.top + ((area.bottom - area.top) - rowHeight) / 2;
  const int returnWidth = metrics.ButtonWidth(L"返回父仓库");
  const int enterWidth = metrics.ButtonWidth(L"进入子模块");
  int right = area.right - returnWidth;
  Place(returnToParent_, Row(right, buttonTop, returnWidth, rowHeight));
  right -= metrics.ColGap();
  right -= enterWidth;
  Place(enterSubmodule_, Row(right, buttonTop, enterWidth, rowHeight));
}

}  // namespace gc::ui
