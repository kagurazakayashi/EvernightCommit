// 变更区面板的回归测试：两类缺陷的断言对象都是窗口的真实可见状态，纯逻辑用例碰不到。
// 表现分别是「清单停在上一条内容上（新行一条没插进来）」与「已经藏起来的空状态说明被布局
// 又摆回到行上面（说明文字叠在清单中间）」。
#include "support/tiny_test.h"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

#include "git/workspace_model.h"
#include "ui/changes_pane.h"
#include "ui/layout.h"
#include "ui/ui_metrics.h"

namespace {

using gc::git::ChangeItem;
using gc::git::CommitItem;
using gc::git::EmptyStateTexts;
using gc::git::WorkspaceModel;
using gc::ui::ChangesPane;
using gc::ui::ChangesSpec;
using gc::ui::HistoryColumns;
using gc::ui::StagingColumns;
using gc::ui::UiMetrics;

constexpr wchar_t kHostClassName[] = L"EvernightCommitTestChangesPaneHost";

bool RegisterHostClass() {
  static const bool registered = [] {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    if (::InitCommonControlsEx(&controls) == FALSE) {
      return false;
    }
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = ::DefWindowProcW;
    description.hInstance = ::GetModuleHandleW(nullptr);
    description.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    description.lpszClassName = kHostClassName;
    return ::RegisterClassExW(&description) != 0;
  }();
  return registered;
}

// 摆在屏幕外并带 WS_EX_TOOLWINDOW 的宿主窗口：控件要真的能收到显隐与摆放消息，
// 但用户屏幕上不该闪出一个测试窗口，也不进任务栏与 Alt+Tab。
[[nodiscard]] HWND MakeHost() {
  if (!RegisterHostClass()) {
    return nullptr;
  }
  const HWND window =
      ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClassName, L"", WS_OVERLAPPED | WS_CLIPCHILDREN,
                        -32000, -32000, 900, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                        nullptr);
  ::ShowWindow(window, SW_SHOWNOACTIVATE);
  return window;
}

// 空状态说明是列表唯一的 STATIC 子窗口（表头是 SysHeader32），按类名取回来断言可见性，
// 不为测试把面板的内部句柄开放出去。
[[nodiscard]] HWND HintOf(HWND list) {
  return ::FindWindowExW(list, nullptr, L"STATIC", nullptr);
}

ChangeItem MakeChange(std::wstring_view path) {
  ChangeItem item;
  item.kind = gc::git::ChangeKind::modified;
  item.statusCode = L"M";
  item.path = std::wstring(path);
  return item;
}

CommitItem MakeCommit(std::wstring_view objectId, std::wstring_view summary) {
  CommitItem item;
  item.objectId = std::wstring(objectId);
  item.summary = std::wstring(summary);
  item.author = L"KagurazakaMiyabi";
  item.authoredAt = L"2026-01-01 00:00:00 +0800";
  return item;
}

// 面板只按给出的栏位摆放，测试不重算选项卡内容区，两页共用同一份区域即可。
constexpr RECT kArea{0, 0, 880, 300};

StagingColumns StagingOf() {
  StagingColumns columns{};
  gc::ui::ComputeStagingColumns(kArea, ChangesSpec{}, &columns);
  return columns;
}

HistoryColumns HistoryOf() {
  HistoryColumns columns{};
  gc::ui::ComputeHistoryColumns(kArea, ChangesSpec{}, &columns);
  return columns;
}

}  // namespace

GC_TEST(changes_pane_layout_keeps_hidden_hints_hidden) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证变更区面板");
  ChangesPane pane;
  pane.Create(host);
  const UiMetrics metrics;
  GC_REQUIRE(HintOf(pane.unstagedList()) != nullptr && HintOf(pane.stagedList()) != nullptr &&
                 HintOf(pane.historyList()) != nullptr,
             "列表里没有空状态说明，用例前提已变化");

  WorkspaceModel model;
  model.unstaged.push_back(MakeChange(L"src/a.cpp"));
  model.recentCommits.push_back(MakeCommit(L"1111111111111111111111111111111111111111", L"第一条"));
  const EmptyStateTexts texts{L"没有未暂存的更改", L"没有已暂存的更改", L"提交历史为空"};
  pane.ShowWorkspace(model, texts);

  // 「变化信息」页：摆放与切页都不许改动说明的显隐，它只由「这一列有没有条目」决定。
  pane.LayoutStaging(StagingOf(), metrics);
  pane.LayoutStaging(StagingOf(), metrics);
  GC_CHECK_MESSAGE(::IsWindowVisible(HintOf(pane.unstagedList())) == FALSE,
                   "未暂存列表有条目时，布局不得把空状态说明摆回行上");
  GC_CHECK_MESSAGE(::IsWindowVisible(HintOf(pane.stagedList())) != FALSE,
                   "已暂存列表确实没有条目，说明要照常露出来");

  // 另一页同理：切到「提交历史」时列表是可见的，说明只有在该列真没有条目时才该露出来。
  pane.LayoutHistory(HistoryOf(), metrics);
  GC_CHECK(::IsWindowVisible(pane.historyList()) != FALSE);
  GC_CHECK_MESSAGE(::IsWindowVisible(HintOf(pane.historyList())) == FALSE,
                   "提交历史有条目时，切页布局不得把「提交历史为空」摆回行上");

  ::DestroyWindow(host);
}

GC_TEST(changes_pane_commit_files_rows_land_for_the_selected_commit) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证文件变化清单");
  ChangesPane pane;
  pane.Create(host);
  const HWND hint = HintOf(pane.filesList());
  GC_REQUIRE(hint != nullptr, "「文件变化」列表里没有空状态说明，用例前提已变化");

  const CommitItem commit = MakeCommit(L"2222222222222222222222222222222222222222", L"第二条");
  ChangesPane::CommitFilesView view;
  view.commit = &commit;
  view.changes = {MakeChange(L"README.md"), MakeChange(L"src/b.cpp"), MakeChange(L"src/c.cpp")};
  pane.ShowCommitFiles(view);

  GC_CHECK_MESSAGE(pane.ListRowCount(pane.filesList()) == 3,
                   "落地的清单必须真的换成这条提交的三条文件变化：行没插进来时状态栏却说读到了条目");
  GC_CHECK_MESSAGE(::IsWindowVisible(hint) == FALSE, "有条目时空状态说明必须藏起来");
  GC_CHECK(pane.CommitFilesCount() == pane.ListRowCount(pane.filesList()));

  // 换到一条读不出内容的提交：旧行必须让位，不能停在上一条的清单上冒充现状。
  const CommitItem other = MakeCommit(L"3333333333333333333333333333333333333333", L"第三条");
  ChangesPane::CommitFilesView emptied;
  emptied.commit = &other;
  emptied.note = L"git show --name-status 没有输出条目";
  pane.ShowCommitFiles(emptied);
  GC_CHECK_MESSAGE(pane.ListRowCount(pane.filesList()) == 0,
                   "上一条的清单必须被换掉，否则双击会按旧行发起命令");
  GC_CHECK_MESSAGE(::IsWindowVisible(hint) != FALSE, "空清单要给出这一段的说法而不是留白");

  ::DestroyWindow(host);
}
