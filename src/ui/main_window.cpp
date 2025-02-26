#include "ui/main_window.h"

#include <commctrl.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

#include "git/workspace_model.h"
#include "platform/windows/locale_text.h"
#include "platform/windows/path_picker.h"
#include "platform/windows/utf_text.h"
#include "ui/commands.h"

namespace gc::ui {
namespace {

constexpr int kSplitterLeftId = 901;
constexpr int kSplitterRightId = 902;

constexpr int kInitialWindowWidth = 1100;
constexpr int kInitialWindowHeight = 780;

constexpr std::wstring_view kTipBrowseRepo = L"选择本地仓库目录。仅记录路径，仓库识别与 Git 操作尚未接入。";
constexpr std::wstring_view kTipBrowseGit = L"选择 git.exe 程序路径。仅记录路径，Git 可用性检测尚未接入。";
constexpr std::wstring_view kTipFetch = L"该仓库级操作尚未实现，按钮保持禁用。对应命令：git fetch";
constexpr std::wstring_view kTipPull = L"该仓库级操作尚未实现，按钮保持禁用。对应命令：git pull";
constexpr std::wstring_view kTipStatus = L"该仓库级操作尚未实现，按钮保持禁用。对应命令：git status";
constexpr std::wstring_view kTipStageAdd = L"该操作尚未实现，按钮保持禁用。对应命令：git add <所选文件>";
constexpr std::wstring_view kTipStageRemove =
    L"该操作尚未实现，按钮保持禁用。对应命令：git restore --staged <所选文件>";
constexpr std::wstring_view kTipRefresh = L"该操作尚未实现，按钮保持禁用。刷新会读取 git status。";
constexpr std::wstring_view kTipCreateCommit = L"该操作尚未实现，按钮保持禁用。对应命令：git commit";
constexpr std::wstring_view kTipUndoCommit = L"该操作尚未实现，按钮保持禁用。对应命令：git reset --soft HEAD^";
constexpr std::wstring_view kTipPush = L"该操作尚未实现，按钮保持禁用。对应命令：git push";
constexpr std::wstring_view kTipCoauthor = L"合作者条目的增删尚未实现，按钮保持禁用。";
constexpr std::wstring_view kTipDateInput = L"可用键盘直接输入年、月、日。";
constexpr std::wstring_view kTipClockInput = L"可用键盘直接输入时、分、秒；本机时区见右侧说明。";
constexpr std::wstring_view kTipTimeSync = L"勾选后，创建提交时以作者时间同步提交者时间。提交功能尚未接入。";
constexpr std::wstring_view kPendingNotice = L"提示：Git 功能尚未接入，所有 Git 操作按钮均处于禁用状态。";

constexpr std::wstring_view kPickRepoTitle = L"选择本地仓库目录";
constexpr std::wstring_view kPickGitTitle = L"选择 Git 程序（git.exe）";

}  // namespace

bool MainWindow::RegisterWindowClass(HINSTANCE instance) {
  WNDCLASSEXW existing{sizeof(existing)};
  if (::GetClassInfoExW(instance, kMainWindowWindowClass, &existing) != 0) {
    return true;
  }
  WNDCLASSEXW description{};
  description.cbSize = sizeof(description);
  description.style = CS_HREDRAW | CS_VREDRAW;
  description.lpfnWndProc = &MainWindow::Thunk;
  description.hInstance = instance;
  description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  description.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
  description.lpszClassName = kMainWindowWindowClass;
  return ::RegisterClassExW(&description) != 0;
}

void MainWindow::UnregisterWindowClass(HINSTANCE instance) {
  ::UnregisterClassW(kMainWindowWindowClass, instance);
}

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
  if (!RegisterWindowClass(instance) || !Splitter::RegisterWindowClass(instance)) {
    return false;
  }
  HWND created = ::CreateWindowExW(0, kMainWindowWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                   CW_USEDEFAULT, metrics_.Scale(kInitialWindowWidth),
                                   metrics_.Scale(kInitialWindowHeight), nullptr, nullptr, instance, this);
  if (created == nullptr) {
    return false;
  }
  window_.Reset(created);
  ::ShowWindow(created, showCommand);
  ::UpdateWindow(created);
  return true;
}

void MainWindow::OnCreate(HWND window) {
  metrics_.UpdateForDpi(DpiForWindowOrSystem(window));
  ::SetWindowPos(window, nullptr, 0, 0, metrics_.Scale(kInitialWindowWidth), metrics_.Scale(kInitialWindowHeight),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  repoBar_.Create(window);
  infoBar_.Create(window);
  changesPane_.Create(window);
  leftSplitter_.Create(window, kSplitterLeftId);
  rightSplitter_.Create(window, kSplitterRightId);
  commitForm_.Create(window);
  actionBar_.Create(window);

  programInfo_ = L"EvernightCommit 界面骨架 v" + platform::Utf8ToUtf16(GC_VERSION_STRING) + L"（" +
                 platform::Utf8ToUtf16(GC_BUILD_TYPE) + L"）";
  commitForm_.SetTimeZoneText(platform::LocalTimeZoneLabel());
  const SYSTEMTIME now = platform::CurrentLocalTime();
  commitForm_.SetTimes(now, now);

  ApplyFonts(window);
  tooltips_.Create(window, metrics_.Font());
  RegisterTooltips();
  UpdateCommandAvailability();
  RefreshTexts(window);
}

void MainWindow::RegisterTooltips() {
  tooltips_.Add(repoBar_.repoBrowse(), kTipBrowseRepo);
  tooltips_.Add(repoBar_.gitBrowse(), kTipBrowseGit);
  tooltips_.Add(repoBar_.fetchButton(), kTipFetch);
  tooltips_.Add(repoBar_.pullButton(), kTipPull);
  tooltips_.Add(repoBar_.statusButton(), kTipStatus);
  tooltips_.Add(changesPane_.stageAddButton(), kTipStageAdd);
  tooltips_.Add(changesPane_.stageRemoveButton(), kTipStageRemove);
  tooltips_.Add(commitForm_.CoauthorAdd(), kTipCoauthor);
  tooltips_.Add(commitForm_.CoauthorRemove(), kTipCoauthor);
  tooltips_.Add(commitForm_.SyncCheckbox(), kTipTimeSync);
  tooltips_.Add(commitForm_.AuthorDate(), kTipDateInput);
  tooltips_.Add(commitForm_.AuthorClock(), kTipClockInput);
  tooltips_.Add(commitForm_.CommitterDate(), kTipDateInput);
  tooltips_.Add(commitForm_.CommitterClock(), kTipClockInput);
  tooltips_.Add(actionBar_.refreshButton(), kTipRefresh);
  tooltips_.Add(actionBar_.createCommitButton(), kTipCreateCommit);
  tooltips_.Add(actionBar_.undoCommitButton(), kTipUndoCommit);
  tooltips_.Add(actionBar_.pushButton(), kTipPush);
}

void MainWindow::UpdateCommandAvailability() {
  const BOOL gitReady = app::AppState::kGitAccessConnected ? TRUE : FALSE;
  for (HWND button : {repoBar_.fetchButton(), repoBar_.pullButton(), repoBar_.statusButton(),
                      changesPane_.stageAddButton(), changesPane_.stageRemoveButton(), commitForm_.CoauthorAdd(),
                      commitForm_.CoauthorRemove(), actionBar_.refreshButton(), actionBar_.createCommitButton(),
                      actionBar_.undoCommitButton(), actionBar_.pushButton()}) {
    ::EnableWindow(button, gitReady);
  }
}

void MainWindow::UpdateLayoutSpecs(HWND /*window*/) {
  bandSpec_ = BandSpec{};
  bandSpec_.repoBarHeight = RepoBar::MinimumHeight(metrics_);
  bandSpec_.infoRowHeight = RepoInfoBar::MinimumHeight(metrics_);
  bandSpec_.actionRowHeight = ActionBar::MinimumHeight(metrics_);
  bandSpec_.gap = metrics_.RowGap();
  bandSpec_.minChangesHeight = ChangesPane::MinimumHeight(metrics_);
  bandSpec_.minFormHeight = CommitForm::MinimumHeight(metrics_);
  bandSpec_.minWidth = std::max({repoBar_.MinimumWidth(metrics_), infoBar_.MinimumWidth(metrics_),
                                 actionBar_.MinimumWidth(metrics_)});

  // 分隔条比例是用户状态，只更新与 DPI 相关的尺寸，不重置比例。
  changesSpec_.splitterWidth = metrics_.SplitterWidth();
  changesSpec_.arrowColumnWidth = metrics_.ArrowColumnWidth();
  changesSpec_.minListWidth = metrics_.MinListWidth();
  changesSpec_.minArrowColumnWidth = metrics_.MinArrowColumnWidth();
  changesSpec_.gap = metrics_.ColGap();
}

void MainWindow::RefreshTexts(HWND window) {
  infoBar_.Refresh(L"当前分支：" + state_.BranchDisplay(), L"上游：" + state_.UpstreamDisplay(),
                   L"任务状态：" + state_.StatusNote(), programInfo_);
  changesPane_.ShowWorkspace(state_.Workspace(), git::NotLoadedTexts());
  if (app::AppState::kGitAccessConnected) {
    actionBar_.SetStatus(state_.StatusNote());
  } else {
    actionBar_.SetStatus(kPendingNotice);
  }
  DoLayout(window);
}

void MainWindow::ApplyFonts(HWND window) {
  ApplyFontToChildTree(window, metrics_.Font());
  if (tooltips_.handle() != nullptr) {
    ::SendMessageW(tooltips_.handle(), WM_SETFONT, reinterpret_cast<WPARAM>(metrics_.Font()), TRUE);
  }
}

void MainWindow::DoLayout(HWND window) {
  UpdateLayoutSpecs(window);

  RECT client{};
  if (::GetClientRect(window, &client) == 0) {
    return;
  }
  const int margin = metrics_.Margin();
  client.left += margin;
  client.top += margin;
  client.right -= margin;
  client.bottom -= margin;

  Bands bands{};
  ComputeBands(client, bandSpec_, &bands);

  repoBar_.Layout(bands.repoBar, metrics_);
  infoBar_.Layout(bands.infoRow, metrics_);

  changesArea_ = bands.changes;
  ChangesColumns columns{};
  ComputeChangesColumns(bands.changes, changesSpec_, &columns);
  leftSplitter_.SetBounds(columns.splitterLeft);
  rightSplitter_.SetBounds(columns.splitterRight);
  changesPane_.Layout(columns, metrics_);

  commitForm_.Layout(bands.commitForm, metrics_);
  actionBar_.Layout(bands.actions, metrics_);
}

SIZE MainWindow::MinimumWindowSize(HWND window) const {
  const MinimumClientSize minimum = MinimumClientFor(bandSpec_, changesSpec_);
  const int margin = metrics_.Margin();
  RECT frame{0, 0, minimum.width + 2 * margin, minimum.height + 2 * margin};
  ::AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE)), metrics_.Dpi());
  return SIZE{frame.right - frame.left, frame.bottom - frame.top};
}

void MainWindow::OnCommand(HWND window, int commandId) {
  switch (commandId) {
    case kIdRepoBrowse:
      BrowseRepoPath(window);
      break;
    case kIdGitBrowse:
      BrowseGitPath(window);
      break;
    default:
      break;  // 其余按钮保持禁用，不会收到命令通知。
  }
}

void MainWindow::BrowseRepoPath(HWND window) {
  std::wstring start = GetControlText(repoBar_.repoEdit());
  if (start.empty()) {
    start = platform::CurrentWorkingDirectory();
  }
  const auto picked = platform::BrowseForFolder(window, kPickRepoTitle, start);
  // 用户取消，或结果不是可用的文件系统路径时，不改动状态、也不给出成功提示。
  if (!picked.has_value() || picked->empty()) {
    return;
  }
  SetControlText(repoBar_.repoEdit(), *picked);
  state_.SetRepoPath(*picked);
  state_.SetStatusNote(L"已记录本地仓库路径；仓库识别与 Git 操作尚未接入。");
  RefreshTexts(window);
}

void MainWindow::BrowseGitPath(HWND window) {
  std::wstring start = GetControlText(repoBar_.gitEdit());
  if (start.empty()) {
    start = platform::CurrentWorkingDirectory();
  }
  const auto picked = platform::BrowseForExecutable(window, kPickGitTitle, start);
  if (!picked.has_value() || picked->empty()) {
    return;
  }
  SetControlText(repoBar_.gitEdit(), *picked);
  state_.SetGitExePath(*picked);
  state_.SetStatusNote(L"已记录 Git 程序路径；Git 可用性检测尚未接入。");
  RefreshTexts(window);
}

void MainWindow::OnSplitterDragged(HWND window, int splitterId, int parentX) {
  const int index = (splitterId == kSplitterLeftId) ? 0 : 1;
  RatioFromMouseX(changesArea_, changesSpec_, index, parentX, &changesSpec_.leftRatio, &changesSpec_.middleRatio);
  DoLayout(window);
}

LRESULT CALLBACK MainWindow::Thunk(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  MainWindow* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<MainWindow*>(create->lpCreateParams);
    ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (self == nullptr) {
    return ::DefWindowProcW(window, message, wParam, lParam);
  }
  try {
    return self->HandleMessage(window, message, wParam, lParam);
  } catch (...) {
    // 异常不能越过系统回调；退回到默认处理并保持消息循环继续运行。
    return ::DefWindowProcW(window, message, wParam, lParam);
  }
}

LRESULT MainWindow::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      OnCreate(window);
      return 0;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) {
        DoLayout(window);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
      const SIZE minimum = MinimumWindowSize(window);
      info->ptMinTrackSize.x = minimum.cx;
      info->ptMinTrackSize.y = minimum.cy;
      return 0;
    }
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      ::SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      metrics_.UpdateForDpi(LOWORD(wParam));
      ApplyFonts(window);
      DoLayout(window);
      return 0;
    }
    case WM_COMMAND:
      OnCommand(window, LOWORD(wParam));
      return 0;
    case kSplitterDragged:
      OnSplitterDragged(window, static_cast<int>(wParam), static_cast<int>(lParam));
      return 0;
    case WM_CLOSE:
      ::DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      window_.Disown();
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, message, wParam, lParam);
  }
}

}  // namespace gc::ui
