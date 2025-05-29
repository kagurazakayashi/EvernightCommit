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

// 带详情带的「提交历史」栏位：ChangesSpec 默认的 detailHeight 是 0，那条带会被整页压成空矩形，
// 那时编辑框虽然「可见」（WS_VISIBLE 与尺寸无关）却没有任何面积，断言摆放就没有意义了。
HistoryColumns HistoryWithDetailBand(const UiMetrics& metrics) {
  ChangesSpec spec{};
  spec.detailHeight = ChangesPane::DetailHeight(metrics);
  HistoryColumns columns{};
  gc::ui::ComputeHistoryColumns(kArea, spec, &columns);
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

// 底部「提交详情」带：落地的是哪一份文本、什么时候才算「有东西可复制」。
// 断言对象是编辑框的真实窗口文本与复制按钮的真实可用状态——纯逻辑用例碰不到这两件事。
GC_TEST(changes_pane_detail_band_only_offers_copy_for_a_real_detail) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证详情带");
  ChangesPane pane;
  pane.Create(host);
  const UiMetrics metrics;
  pane.LayoutHistory(HistoryWithDetailBand(metrics), metrics);

  const HWND edit = pane.commitDetailEdit();
  const HWND copyButton = pane.commitDetailCopyButton();
  GC_REQUIRE(edit != nullptr && copyButton != nullptr, "详情带里没有编辑框与复制按钮，用例前提已变化");
  GC_CHECK_MESSAGE(::IsWindowVisible(edit) != FALSE, "提交历史那一页必须把详情带摆出来");
  RECT editRect{};
  RECT bandRect{};
  ::GetWindowRect(edit, &editRect);
  ::GetWindowRect(pane.commitDetailCopyButton(), &bandRect);
  GC_CHECK_MESSAGE(editRect.right - editRect.left > 0 && editRect.bottom - editRect.top > 0,
                   "详情带的编辑框要有真实面积，否则用户看到的是没有内容的一条空带");
  GC_CHECK(bandRect.right - bandRect.left > 0);

  // 首屏（没有选中提交）：显示引导语，但没有可复制内容，按钮是灰的。
  GC_CHECK(pane.CommitDetailText().empty());
  GC_CHECK(::IsWindowVisible(copyButton) != FALSE);
  GC_CHECK_MESSAGE(::IsWindowEnabled(copyButton) == FALSE,
                   "引导语不是提交信息，不能被复制走");
  std::wstring greeting(GetWindowTextLengthW(edit), L'\0');
  ::GetWindowTextW(edit, greeting.data(), static_cast<int>(greeting.size()) + 1);
  GC_CHECK_MESSAGE(greeting.find(L"选中「最近提交」里的某一条") != std::wstring::npos,
                   "首屏要给出引导语，不能留一块空白让人以为这条提交没内容");

  // 真读到一条提交：文本原样落地，可复制内容就是这一份。
  const CommitItem commit = MakeCommit(L"4444444444444444444444444444444444444444", L"第四条");
  ChangesPane::CommitDetailView read;
  read.commit = &commit;
  read.text = L"提交哈希：4444\r\n标题：第四条";
  pane.ShowCommitDetail(read);
  std::wstring shown(GetWindowTextLengthW(edit), L'\0');
  ::GetWindowTextW(edit, shown.data(), static_cast<int>(shown.size()) + 1);
  GC_CHECK(shown == read.text);
  GC_CHECK(pane.CommitDetailText() == read.text);
  GC_CHECK(pane.CommitDetailCommitId() == commit.objectId);
  GC_CHECK_MESSAGE(::IsWindowEnabled(copyButton) != FALSE, "读到了详情，复制按钮要能用");

  // 读失败：说清原因，且把可复制内容收回去——上一条的详情不能留着冒充现状。
  ChangesPane::CommitDetailView failed;
  failed.commit = &commit;
  failed.error = L"Git 查询超时";
  pane.ShowCommitDetail(failed);
  std::wstring failureText(GetWindowTextLengthW(edit), L'\0');
  ::GetWindowTextW(edit, failureText.data(), static_cast<int>(failureText.size()) + 1);
  GC_CHECK_MESSAGE(failureText.find(L"Git 查询超时") != std::wstring::npos, "失败原因要写到面板上");
  GC_CHECK(pane.CommitDetailText().empty());
  GC_CHECK(::IsWindowEnabled(copyButton) == FALSE);

  ::DestroyWindow(host);
}

// 切到「变化信息」那一页时，属于提交历史的控件（含详情带那三件）必须一起藏起来。
// 只藏两份列表的话，详情编辑框会留在选项卡区域里压住表单上方那一片。
GC_TEST(changes_pane_staging_page_hides_the_detail_band) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证切页显隐");
  ChangesPane pane;
  pane.Create(host);
  const UiMetrics metrics;

  pane.LayoutHistory(HistoryWithDetailBand(metrics), metrics);
  GC_CHECK(::IsWindowVisible(pane.commitDetailEdit()) != FALSE);

  pane.LayoutStaging(StagingOf(), metrics);
  GC_CHECK_MESSAGE(::IsWindowVisible(pane.commitDetailEdit()) == FALSE,
                   "「变化信息」那一页不许露出提交详情的编辑框");
  GC_CHECK(::IsWindowVisible(pane.commitDetailCopyButton()) == FALSE);

  // 来回切页之后仍要能重新摆回详情带的栏位（不是藏了就搬不回来）。
  pane.LayoutHistory(HistoryWithDetailBand(metrics), metrics);
  GC_CHECK(::IsWindowVisible(pane.commitDetailEdit()) != FALSE);

  ::DestroyWindow(host);
}

// 断言信息要带上真实窗口文本，而测试框架的说明参数是窄字符串（现有用例也各自备一份这个小转换）。
[[nodiscard]] std::string Utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string narrow(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size,
                        nullptr, nullptr);
  return narrow;
}

// 中间那一栏现在是四个按钮一条竖列，自上而下：「全部加入 →」、「加入暂存区 →」、
// 「← 移出暂存区」、「← 全部移出」——两个批量按钮就是把逐条那一对向上下各撑开一格。
// 断言对象是四个按钮的真实窗口文本与屏幕矩形：纯逻辑用例碰不到「按钮到底摆在哪一行」，
// 而「批量的在上面/下面」正是这一栏唯一的功能边界（点错一行就会执行另一种范围的命令）。
GC_TEST(changes_pane_layout_places_four_staging_buttons_in_order) {
  const HWND host = MakeHost();
  GC_REQUIRE(host != nullptr, "测试宿主窗口创建失败，无法验证四个暂存按钮的摆放");
  ChangesPane pane;
  pane.Create(host);
  const UiMetrics metrics;

  const HWND buttons[] = {pane.stageAddAllButton(), pane.stageAddButton(), pane.stageRemoveButton(),
                          pane.stageRemoveAllButton()};
  const wchar_t* const labels[] = {L"全部加入 →", L"加入暂存区 →", L"← 移出暂存区", L"← 全部移出"};
  for (int i = 0; i < 4; ++i) {
    GC_REQUIRE(buttons[i] != nullptr, "变更区少了暂存方向的按钮，用例前提已变化");
    std::wstring text(::GetWindowTextLengthW(buttons[i]), L'\0');
    ::GetWindowTextW(buttons[i], text.data(), static_cast<int>(text.size()) + 1);
    GC_CHECK_MESSAGE(text == labels[i],
                     "按钮文字与它执行的命令必须对得上，实际是：" + Utf8(text));
  }

  const StagingColumns columns = StagingOf();
  pane.LayoutStaging(columns, metrics);

  RECT rects[4]{};
  for (int i = 0; i < 4; ++i) {
    ::GetWindowRect(buttons[i], &rects[i]);
    // 屏幕矩形换算成宿主窗口的客户区坐标，才能与 ComputeStagingColumns 给出的栏位对比。
    ::MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<LPPOINT>(&rects[i]), 2);
    GC_CHECK_MESSAGE(::IsWindowVisible(buttons[i]) != FALSE, "「变化信息」那一页要露出四个按钮");
    GC_CHECK_MESSAGE(rects[i].right - rects[i].left > 0 && rects[i].bottom - rects[i].top > 0,
                     "按钮要有真实面积，否则用户看到的是压扁的一条");
    GC_CHECK_MESSAGE(rects[i].bottom - rects[i].top == metrics.ControlHeight(),
                     "每个按钮各占一行，行高必须一致");
  }

  // 自上而下的次序与行距：四个按钮一行一个，中间不许多出一格空行。
  const int step = metrics.ControlHeight() + metrics.RowGap();
  for (int i = 1; i < 4; ++i) {
    GC_CHECK_MESSAGE(rects[i].top > rects[i - 1].top,
                     "四个按钮的上下次序被摆错了：「全部加入 →」必须在最上、「← 全部移出」在最下");
    GC_CHECK(rects[i].top - rects[i - 1].top == step);
  }

  // 横向都落在箭头那一栏里，且中线一致：批量按钮文字更短、宽度各按自己的文字量算，
  // 但一栏按钮左右错开就不是一条竖列了（允许 1 像素的整数除法误差）。
  const int center = rects[0].left + (rects[0].right - rects[0].left) / 2;
  for (int i = 0; i < 4; ++i) {
    GC_CHECK(rects[i].left >= columns.arrows.left && rects[i].right <= columns.arrows.right);
    const int own = rects[i].left + (rects[i].right - rects[i].left) / 2;
    GC_CHECK(center > own ? center - own <= 1 : own - center <= 1);
  }

  // 整列高度按四行算：窗口缩到最小时够不够高，判据就是这一个函数。
  GC_CHECK(ChangesPane::ButtonStackHeight(metrics) ==
           4 * metrics.ControlHeight() + 3 * metrics.RowGap());

  // 切到「提交历史」：四个按钮一起藏起来，不许盖在详情带上。
  pane.LayoutHistory(HistoryWithDetailBand(metrics), metrics);
  for (const HWND button : buttons) {
    GC_CHECK_MESSAGE(::IsWindowVisible(button) == FALSE, "「提交历史」那一页不许露出暂存按钮");
  }

  // 切回来仍要能重新摆回那条竖列（不是藏了就搬不回来）。
  pane.LayoutStaging(columns, metrics);
  for (const HWND button : buttons) {
    GC_CHECK(::IsWindowVisible(button) != FALSE);
  }

  ::DestroyWindow(host);
}
