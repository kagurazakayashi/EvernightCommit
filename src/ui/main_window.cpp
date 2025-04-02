#include "ui/main_window.h"

#include <commctrl.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/workspace_model.h"
#include "platform/windows/git_toolchain.h"
#include "platform/windows/locale_text.h"
#include "platform/windows/path_picker.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"
#include "ui/commands.h"
#include "ui/resource_ids.h"
namespace gc::ui {
namespace {

constexpr int kSplitterLeftId = 901;
constexpr int kSplitterRightId = 902;

constexpr int kInitialWindowWidth = 1100;
constexpr int kInitialWindowHeight = 780;

constexpr std::wstring_view kTipBrowseRepo =
    L"选择本地仓库目录；也可在输入框直接键入路径（停顿后自动识别）。支持仓库的子目录，识别时会上溯到工作区根。";
constexpr std::wstring_view kTipBrowseGit = L"浏览选择 git.exe；选定后立即在后台运行 git --version 验证。";
constexpr std::wstring_view kTipFetch = L"该仓库级操作尚未实现，按钮保持禁用。对应命令：git fetch";
constexpr std::wstring_view kTipPull = L"该仓库级操作尚未实现，按钮保持禁用。对应命令：git pull";
constexpr std::wstring_view kTipStatus =
    L"在新命令窗口里执行 git status：显示真实命令与输出，Git 结束窗口仍保留；本程序通过结果文件获知退出码。";
constexpr std::wstring_view kTipStageAdd = L"该操作尚未实现，按钮保持禁用。对应命令：git add <所选文件>";
constexpr std::wstring_view kTipStageRemove =
    L"该操作尚未实现，按钮保持禁用。对应命令：git restore --staged <所选文件>";
constexpr std::wstring_view kTipRefresh =
    L"重新读取当前仓库的摘要与两个更改列表：只在本程序后台执行只读 Git 查询，不弹出命令窗口。\r\n"
    L"在外部终端里改过仓库、或命令窗口里的 Git 已结束，都可以点这里恢复真实状态。";
constexpr std::wstring_view kTipCreateCommit = L"该操作尚未实现，按钮保持禁用。对应命令：git commit";
constexpr std::wstring_view kTipUndoCommit = L"该操作尚未实现，按钮保持禁用。对应命令：git reset --soft HEAD^";
constexpr std::wstring_view kTipPush = L"该操作尚未实现，按钮保持禁用。对应命令：git push";
constexpr std::wstring_view kTipUnstagedList =
    L"未暂存的更改来自只读的 git status（porcelain v2，机器可读格式）：\r\n"
    L"包含已跟踪文件的修改/删除/重命名、未跟踪文件（目录已展开为单个文件），以及待解决的冲突项。\r\n"
    L"被 Git 忽略的文件不列出，也不会被强制加入。\r\n"
    L"同一文件可以同时出现在左右两侧：暂存一次修改后继续编辑，两侧各显示自己的状态。\r\n"
    L"“子模块”条目只表示父仓库记录的提交指针与子模块内部状态；在父仓库暂存子模块不会提交它内部的任何文件。";
constexpr std::wstring_view kTipStagedList =
    L"已暂存的更改是索引相对最近一次提交的差异，同样来自只读的 git status。\r\n"
    L"创建提交时只会写入这里的内容；未暂存那一侧的改动仍留在工作区。\r\n"
    L"“重命名”条目会显示“旧路径 → 新路径”，内部同时保留两个原始路径。";
constexpr std::wstring_view kTipCoauthor = L"合作者条目的增删尚未实现，按钮保持禁用。";
constexpr std::wstring_view kTipDateInput = L"可用键盘直接输入年、月、日。";
constexpr std::wstring_view kTipClockInput = L"可用键盘直接输入时、分、秒；本机时区见右侧说明。";
constexpr std::wstring_view kTipTimeSync = L"勾选后，创建提交时以作者时间同步提交者时间。提交功能尚未接入。";
constexpr std::wstring_view kPendingNotice =
    L"提示：status 已接入外部命令窗口执行器（在新窗口里执行并保留输出）；"
    L"未暂存/已暂存列表已按只读 git status 填充，“刷新”可随时重读且不弹命令窗口，"
    L"命令窗口里的 Git 结束后也会自动重读一次；"
    L"暂存/提交/撤回/fetch/pull/push 将在后续步骤接入，按钮当前保持禁用。";

constexpr std::wstring_view kRepoInputPlaceholder = L"（未设置本地仓库路径）";

constexpr std::wstring_view kPickRepoTitle = L"选择本地仓库目录";
constexpr std::wstring_view kPickGitTitle = L"选择 Git 程序（git.exe）";

// 从本程序资源中加载应用图标。窗口类在创建窗口之前注册，此时拿不到窗口 DPI，
// 因此按系统度量取尺寸（大图标取 SM_CXICON，小图标取 SM_CXSMICON）。
HICON LoadAppIcon(HINSTANCE instance, bool largest) {
  const int cx = largest ? 0 : ::GetSystemMetrics(SM_CXSMICON);
  const int cy = largest ? 0 : ::GetSystemMetrics(SM_CYSMICON);
  const UINT flags = largest ? (LR_DEFAULTSIZE | LR_DEFAULTCOLOR) : LR_DEFAULTCOLOR;
  return static_cast<HICON>(
      ::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, cx, cy, flags));
}

// 程序改写“本地仓库”输入框时抑制 EN_CHANGE 通知，避免自己触发的文本变更又被当成一次新的用户选择。
// 标志恒为“作用域内为 true、出作用域复位”，中途抛异常也不会泄漏成永久抑制。
class SuppressRepoEditNotify {
public:
  explicit SuppressRepoEditNotify(bool& flag) : flag_(flag) { flag_ = true; }
  SuppressRepoEditNotify(const SuppressRepoEditNotify&) = delete;
  SuppressRepoEditNotify& operator=(const SuppressRepoEditNotify&) = delete;
  ~SuppressRepoEditNotify() { flag_ = false; }

private:
  bool& flag_;
};

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
  // 窗口与任务栏图标；资源缺失时 hIcon/hIconSm 为 nullptr，系统回退到默认图标。
  description.hIcon = LoadAppIcon(instance, /*largest=*/true);
  description.hIconSm = LoadAppIcon(instance, /*largest=*/false);
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
  InitializeRepoInput(window);
  RefreshTexts(window);
  ApplyWorkspaceLists();  // 首屏的空状态说明：列表不在 RefreshTexts 里重建，这里显式填一次。
  InitializeGitDetection(window);
  InitializeCommandWatching(window);
}

void MainWindow::RegisterTooltips() {
  tooltips_.Add(repoBar_.repoEdit(), L"输入本地仓库目录（也可以是其子目录）；识别在后台只读执行，不会改动仓库。");
  tooltips_.Add(repoBar_.repoBrowse(), kTipBrowseRepo);
  tooltips_.Add(repoBar_.gitBrowse(), kTipBrowseGit);
  tooltips_.Add(repoBar_.fetchButton(), kTipFetch);
  tooltips_.Add(repoBar_.pullButton(), kTipPull);
  tooltips_.Add(repoBar_.statusButton(), kTipStatus);
  tooltips_.Add(changesPane_.stageAddButton(), kTipStageAdd);
  tooltips_.Add(changesPane_.stageRemoveButton(), kTipStageRemove);
  tooltips_.Add(changesPane_.unstagedList(), kTipUnstagedList);
  tooltips_.Add(changesPane_.stagedList(), kTipStagedList);
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
  // 依赖 Git 的控件要同时满足：功能已接通（后续步骤）、Git 程序验证可用、仓库已识别为可用工作区；
  // 任一条件失效（改正路径、切换仓库、识别失败、裸仓库）都会立即重新禁用。
  // 写操作还要再加一条：同一工作区同时只允许一个由本程序发起的操作，否则会互相抢仓库锁。
  const BOOL gitReady =
      (app::AppState::kGitOperationsImplemented && state_.GitUsable() && state_.RepoUsable() &&
       !tasks_.OperationInFlight())
          ? TRUE
          : FALSE;
  for (HWND button : {repoBar_.fetchButton(), repoBar_.pullButton(),
                      changesPane_.stageAddButton(), changesPane_.stageRemoveButton(), commitForm_.CoauthorAdd(),
                      commitForm_.CoauthorRemove(), actionBar_.createCommitButton(),
                      actionBar_.undoCommitButton(), actionBar_.pushButton()}) {
    ::EnableWindow(button, gitReady);
  }
  // 刷新是内部只读重读，不占用命令窗口、也不写仓库，因此在有操作在跑时依然可用：
  // 外部终端改了仓库、或某个操作的输出看不清时，用户总要能恢复真实状态。
  // 上一次识别失败也保持可用：仓库在外部被修好（或改回原样）后，点刷新就能恢复，
  // 不必先把路径清空再重选。
  const BOOL refreshReady = (state_.GitUsable() && !state_.Info().repoPath.empty()) ? TRUE : FALSE;
  ::EnableWindow(actionBar_.refreshButton(), refreshReady);
  // status 是首个接入命令窗口执行器的操作：除上述条件外，同一时刻只允许一个操作在跑，
  // 避免并发提交让“哪个窗口对应哪次操作”变得含糊。
  const BOOL statusReady =
      (state_.GitUsable() && state_.RepoUsable() && !tasks_.OperationInFlight()) ? TRUE : FALSE;
  ::EnableWindow(repoBar_.statusButton(), statusReady);
}

std::wstring MainWindow::TaskStatusNote() const {
  // 进行中/已完成的操作说明优先于常规任务状态：用户必须能看到“命令窗口里正在跑什么”。
  if (activeOperation_.serial != 0) {
    std::wstring status;
    if (commandRunner_.DescribeOperation(activeOperation_.runnerId, &status, nullptr)) {
      return L"命令窗口操作：" + status;
    }
    // 执行器已经不认识这个 ID：完成通知丢了或被别处回收。这里只说明状态无法确认，
    // 真正的结案（释放槽位 + 安排刷新）由 TickActiveOperations 负责。
    return L"命令窗口操作：" + activeOperation_.displayName + L" 的结果已无法确认，将重新读取仓库状态…";
  }
  if (tasks_.ReadInFlight()) {
    return tasks_.ReadStartedText();
  }
  return state_.StatusNote();
}

std::wstring MainWindow::OperationBanner() const {
  // 底部状态条：有任务在进行时用实时状态取代“功能未接入”的固定说明。
  if (activeOperation_.serial != 0 || tasks_.ReadInFlight()) {
    return TaskStatusNote();
  }
  // 读到子模块变化时，这条说明比固定提示更有价值：父仓库暂存的范围必须当场讲清楚。
  const std::wstring submoduleNote = state_.WorkspaceBanner();
  if (!submoduleNote.empty()) {
    return submoduleNote;
  }
  if (app::AppState::kGitOperationsImplemented) {
    return state_.StatusNote();
  }
  return std::wstring(kPendingNotice);
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
  infoBar_.Refresh(L"仓库类型：" + state_.RepoTypeDisplay(), L"当前分支：" + state_.BranchDisplay(),
                   L"上游：" + state_.UpstreamDisplay(), L"任务状态：" + TaskStatusNote(), programInfo_);
  // 两个列表不在这里重建：本函数每 500 毫秒的状态轮询也会被调用，
  // 逐次清空再填入会让列表反复闪、选中项被冲掉。内容真正变化时由 ApplyWorkspaceLists 落地。
  actionBar_.SetStatus(OperationBanner());
  DoLayout(window);
}

void MainWindow::ApplyWorkspaceLists() {
  changesPane_.ShowWorkspace(state_.WorkspaceModel(), state_.WorkspaceHintTexts());
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

void MainWindow::OnCommand(HWND window, WPARAM wParam) {
  const int commandId = LOWORD(wParam);
  const UINT notifyCode = HIWORD(wParam);
  switch (commandId) {
    case kIdRepoBrowse:
      BrowseRepoPath(window);
      break;
    case kIdGitBrowse:
      BrowseGitPath(window);
      break;
    case kIdGitCombo:
      OnGitComboNotify(window, notifyCode);
      break;
    case kIdRepoEdit:
      OnRepoEditNotify(window, notifyCode);
      break;
    case kIdStatusButton:
      if (notifyCode == BN_CLICKED) {
        LaunchStatusOperation(window);
      }
      break;
    case kIdRefreshButton:
      if (notifyCode == BN_CLICKED) {
        ScheduleRefresh(window);
      }
      break;
    default:
      break;  // 其余按钮保持禁用，不会收到命令通知。
  }
}

void MainWindow::OnGitComboNotify(HWND window, UINT notifyCode) {
  switch (notifyCode) {
    case CBN_EDITCHANGE:
      // 键入逐字符通知：只重排防抖定时器，停顿后统一验证，避免每敲一键启动一个子进程。
      ::KillTimer(window, kGitVerifyTimer);
      ::SetTimer(window, kGitVerifyTimer, kGitVerifyDebounceMs, nullptr);
      break;
    case CBN_SELCHANGE:
    case CBN_KILLFOCUS:
      ::KillTimer(window, kGitVerifyTimer);
      CommitGitInput(window);
      break;
    default:
      break;
  }
}

void MainWindow::OnRepoEditNotify(HWND window, UINT notifyCode) {
  if (suppressRepoEditNotify_) {
    return;  // 程序改写输入框（绝对化、浏览回填）不是用户的新选择。
  }
  switch (notifyCode) {
    case EN_CHANGE:
      // 键入逐字符通知：只重排防抖定时器，停顿后统一识别，避免每敲一键发起一批 Git 查询。
      ::KillTimer(window, kRepoDetectTimer);
      ::SetTimer(window, kRepoDetectTimer, kRepoDetectDebounceMs, nullptr);
      break;
    case EN_KILLFOCUS:
      ::KillTimer(window, kRepoDetectTimer);
      CommitRepoInput(window);
      break;
    default:
      break;
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
  ::KillTimer(window, kRepoDetectTimer);  // 明确选择不再防抖。
  {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), *picked);
  }
  CommitRepoInput(window);
}

void MainWindow::InitializeRepoInput(HWND /*window*/) {
  // 初值取应用启动时的工作目录（只读取，不改变进程工作目录）。
  const std::wstring startupDirectory = platform::CurrentWorkingDirectory();
  const std::wstring absolute = platform::ToAbsolutePath(startupDirectory);
  const std::wstring initial = absolute.empty() ? startupDirectory : absolute;
  {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), initial);
  }
  state_.SetRepoPath(initial);

  app::RepoState repo;
  repo.status = app::RepoLoadStatus::unloaded;
  if (initial.empty()) {
    repo.detection.error = git::RepoError::inputEmpty;
    state_.SetRepo(std::move(repo));
    state_.SetStatusNote(std::wstring(kRepoInputPlaceholder) + L"。" +
                         git::BuildRepoErrorDetail(repo.detection.error, {}));
    return;
  }
  // 识别要等 Git 程序验证通过才能开始（见 OnGitProbeCompleted），这里只挂起等待状态；
  // 不能假装“正在识别”，否则没有任何查询会把它结束掉。
  repo.detection.error = git::RepoError::gitUnavailable;
  state_.SetRepo(std::move(repo));
  state_.SetStatusNote(L"等待 Git 程序验证通过后识别仓库：" + initial);
}

void MainWindow::BrowseGitPath(HWND window) {
  std::wstring start = GetControlText(repoBar_.gitCombo());
  if (start.empty()) {
    start = platform::CurrentWorkingDirectory();
  }
  const auto picked = platform::BrowseForExecutable(window, kPickGitTitle, start);
  if (!picked.has_value() || picked->empty()) {
    return;
  }
  ::KillTimer(window, kGitVerifyTimer);  // 明确选择不再防抖。
  const std::wstring normalized = platform::NormalizeGitExeInput(*picked);
  SetControlText(repoBar_.gitCombo(), normalized);
  RequestGitVerification(window, normalized);
}

void MainWindow::InitializeGitDetection(HWND window) {
  // 启动即按当前进程 PATH 发现候选（等价 `where git` 的搜索意图，不扫盘）。
  const std::vector<std::wstring> candidates = platform::DiscoverGitCandidates();
  repoBar_.SetGitCandidates(candidates);
  if (candidates.empty()) {
    const std::wstring message = L"当前进程 PATH 中未找到 git.exe，请手动输入路径或用“浏览…”选择。";
    app::GitToolState tool;
    tool.status = app::GitExeStatus::unverified;
    tool.message = message;
    state_.SetGitTool(std::move(tool));
    // 没有可用的 Git 就无从识别仓库；置为“Git 程序不可用”，用户补好路径后会自动补做识别。
    SetRepoFailed(window, state_.Info().repoPath, git::RepoError::gitUnavailable, message);
    return;
  }
  SetControlText(repoBar_.gitCombo(), candidates.front());
  RequestGitVerification(window, candidates.front());
}

void MainWindow::CommitGitInput(HWND window) {
  const std::wstring raw = GetControlText(repoBar_.gitCombo());
  const std::wstring normalized = platform::NormalizeGitExeInput(raw);
  if (normalized.empty()) {
    app::GitToolState tool;
    tool.status = app::GitExeStatus::unverified;
    tool.message = L"未选择 Git 程序。";
    state_.SetGitTool(std::move(tool));
    state_.SetStatusNote(L"Git 未设置：请输入 git.exe 路径或用“浏览…”选择。");
    UpdateCommandAvailability();
    RefreshTexts(window);
    return;
  }
  const app::GitToolState& current = state_.Git();
  if (normalized == current.path &&
      (current.status == app::GitExeStatus::verifying || current.status == app::GitExeStatus::verified)) {
    return;  // 编程式改写文本回环触发的重复提交：跳过。
  }
  if (normalized != raw) {
    SetControlText(repoBar_.gitCombo(), normalized);
  }
  RequestGitVerification(window, normalized);
}

void MainWindow::RequestGitVerification(HWND window, const std::wstring& normalizedPath) {
  app::GitToolState tool;
  tool.status = app::GitExeStatus::verifying;
  tool.path = normalizedPath;
  tool.message = L"正在后台验证 Git 程序（git --version）…";
  state_.SetGitTool(std::move(tool));
  state_.SetGitExePath(normalizedPath);
  state_.SetStatusNote(L"正在验证 Git 程序：" + normalizedPath);
  UpdateCommandAvailability();
  RefreshTexts(window);
  // 任务体只在工作线程执行；序号由控制器管理，旧结果按序号作废。
  gitWorker_.Request(window, kGitProbeCompleted, normalizedPath, [timeout = kGitProbeTimeoutMs](const std::wstring& exe) {
    return platform::VerifyGitExe(exe, timeout);
  });
}

void MainWindow::OnGitProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::GitExeVerification verification;
  if (!gitWorker_.FetchLatest(completionSerial, &verification)) {
    return;  // 较慢完成的旧结果：已被更新的选择取代，丢弃。
  }
  // 换 Git 程序同样是一次身份变化：旧 Git 读回来的摘要与列表都要作废。
  const std::wstring previousGitPath = state_.Git().path;
  app::GitToolState tool;
  tool.path = verification.path;
  tool.version = verification.version;
  tool.message = verification.message;
  tool.status = verification.outcome == git::GitProbeOutcome::verified ? app::GitExeStatus::verified
                                                                       : app::GitExeStatus::invalid;
  state_.SetGitTool(std::move(tool));
  state_.SetStatusNote(verification.message);
  UpdateCommandAvailability();
  const bool switchedGit = verification.outcome == git::GitProbeOutcome::verified &&
                           !previousGitPath.empty() && previousGitPath != verification.path &&
                           !state_.Info().repoPath.empty();
  if (switchedGit) {
    // 用另一个 Git 程序重新识别同一个仓库路径：识别成功后会接着重读工作区，
    // 在途的旧结果由身份版本判为过期。
    RequestRepoDetection(window, state_.Info().repoPath, RepoDetectMode::initial);
  } else {
    // Git 一开始不可用、随后才验证通过的场合：自动补一次仓库识别，用户无需重选路径。
    ResumeRepoDetectionWhenGitReady(window);
  }
  RefreshTexts(window);
}

void MainWindow::CommitRepoInput(HWND window) {
  const std::wstring raw = git::TrimWide(GetControlText(repoBar_.repoEdit()));
  if (raw.empty()) {
    SetRepoFailed(window, {}, git::RepoError::inputEmpty, {});
    return;
  }
  // 相对路径按启动工作目录展开；展示与内部状态都用绝对路径（仓库识别只读，不改任何东西）。
  const std::wstring absolute = platform::ToAbsolutePath(raw);
  const std::wstring normalized = absolute.empty() ? raw : absolute;
  if (normalized != raw) {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), normalized);
  }
  state_.SetRepoPath(normalized);
  if (state_.Repo().status == app::RepoLoadStatus::detecting) {
    return;  // 上一次识别还在进行：它会以最新一次提交为准，无需重复排队。
  }
  RequestRepoDetection(window, normalized, RepoDetectMode::initial);
}

void MainWindow::RequestRepoDetection(HWND window, const std::wstring& normalizedPath, RepoDetectMode mode) {
  if (!state_.GitUsable()) {
    SetRepoFailed(window, normalizedPath, git::RepoError::gitUnavailable, {});
    return;
  }
  if (mode == RepoDetectMode::initial) {
    app::RepoState repo;
    repo.status = app::RepoLoadStatus::detecting;
    state_.SetRepo(std::move(repo));
    // 换仓库的第一步就是作废旧身份并清空旧列表：识别还没回来时宁可看到“正在读取”，
    // 也不能让上一个仓库的未暂存/已暂存条目留在屏上被当成当前状态。
    refreshCycleActive_ = false;
    tasks_.UnbindRepository();
    ClearWorkspace();
    state_.SetStatusNote(L"正在后台识别仓库（只读查询，不会改动仓库）：" + normalizedPath);
  } else {
    // 刷新：仓库身份没变，摘要与列表都留在原位，等新结果回来再就地替换，
    // 免得每点一次刷新就整屏空一下、选中项也没了。
    refreshCycleActive_ = true;
    state_.SetStatusNote(tasks_.ReadStartedText());
  }
  UpdateCommandAvailability();
  RefreshTexts(window);

  platform::RepoDetectRequest request;
  request.exePath = state_.Git().path;
  request.directory = normalizedPath;
  request.timeoutMilliseconds = kRepoDetectTimeoutMs;
  repoWorker_.Request(window, kRepoDetectCompleted, std::move(request),
                      [](const platform::RepoDetectRequest& pending) {
                        return platform::RunRepositoryDetection(pending);
                      });
}

void MainWindow::SetRepoFailed(HWND window, const std::wstring& normalizedPath, git::RepoError error,
                               std::wstring_view detail) {
  const std::wstring message = git::BuildRepoErrorDetail(error, detail);
  app::RepoState repo;
  repo.status = (error == git::RepoError::inputEmpty) ? app::RepoLoadStatus::unloaded
                                                      : app::RepoLoadStatus::failed;
  repo.detection.kind = (error == git::RepoError::notRepository) ? git::RepoKind::notRepository
                                                                 : git::RepoKind::failed;
  repo.detection.error = error;
  repo.detection.root = normalizedPath;
  repo.detection.message = message;
  state_.SetRepo(std::move(repo));
  // 没有可用的工作区身份，就没有任何安全的查询落点：在途结果一律作废，列表清空。
  tasks_.UnbindRepository();
  refreshCycleActive_ = false;
  ClearWorkspace();
  // 失败原因写进任务状态；若尚未选择仓库则给出占位说明而不是错误。
  state_.SetStatusNote(normalizedPath.empty() ? std::wstring(kRepoInputPlaceholder) + L"。" + message
                                              : message);
  UpdateCommandAvailability();
  RefreshTexts(window);
}

void MainWindow::OnRepoDetectCompleted(HWND window, uint64_t completionSerial) {
  git::RepoDetection detection;
  if (!repoWorker_.FetchLatest(completionSerial, &detection)) {
    return;  // 较慢完成的旧结果：已被更新的仓库选择取代，丢弃。
  }
  const bool wasRefresh = refreshCycleActive_;
  app::RepoState repo;
  const bool answeredByGit =
      detection.error == git::RepoError::none || detection.kind == git::RepoKind::notRepository;
  repo.status = answeredByGit ? app::RepoLoadStatus::loaded : app::RepoLoadStatus::failed;
  repo.detection = std::move(detection);
  state_.SetRepo(std::move(repo));

  if (!state_.RepoUsable()) {
    // 识别失败，或形态根本没有工作区（裸仓库、.git 内部、非仓库）：
    // 旧身份必须作废，否则之前发出的读取结果会被当成当前状态留在屏上。
    state_.SetStatusNote(state_.Repo().detection.message);
    tasks_.UnbindRepository();
    refreshCycleActive_ = false;
    ClearWorkspace();
    UpdateCommandAvailability();
    RefreshTexts(window);
    return;
  }

  // 刷新链路：识别成功后接着读工作区；识别本身失败时不会走到这里。
  const bool identityChanged = tasks_.BindRepository(state_.Git().path, state_.Repo().detection.root);
  if (identityChanged) {
    // 工作区根与上次不同（选了另一个仓库，或同一 Git 程序解析出不同的根）：
    // 上一个仓库的条目一律作废，等新的读取结果重新填。
    state_.SetStatusNote(state_.Repo().detection.message);
    ClearWorkspace();
  } else if (!wasRefresh) {
    state_.SetStatusNote(state_.Repo().detection.message);
  }
  UpdateCommandAvailability();
  StartWorkspaceRead(window);
  RefreshTexts(window);
}

void MainWindow::ClearWorkspace() {
  // 未发起读取或读取前提消失：状态回到 unloaded，模型为空，列表显示“尚未选择可用仓库”。
  state_.SetWorkspace(git::WorkspaceSnapshot{});
  ApplyWorkspaceLists();
}

void MainWindow::ScheduleRefresh(HWND window) {
  if (!state_.GitUsable() || state_.Info().repoPath.empty()) {
    return;  // 连要刷新哪个仓库都不知道（此时“刷新”按钮也已禁用）。
  }
  // 短时间内的多次触发——连点“刷新”、几个操作接连结束——共用同一个定时器，到点只跑一轮。
  ::KillTimer(window, kRefreshTimer);
  ::SetTimer(window, kRefreshTimer, kRefreshDebounceMs, nullptr);
  state_.SetStatusNote(tasks_.ReadStartedText());
  RefreshTexts(window);
}

void MainWindow::RunRefreshCycle(HWND window) {
  if (!state_.GitUsable() || state_.Info().repoPath.empty()) {
    refreshCycleActive_ = false;
    return;
  }
  // 一次刷新同时覆盖顶部摘要与两个列表：在外部终端里切分支、提交、拉取之后，
  // 分支与上游也一样会变，只重读 git status 会留下半新半旧的界面。
  // 上一次识别失败时，这一趟同时也是“外部把仓库修好了”之后的恢复入口。
  RequestRepoDetection(window, state_.Info().repoPath,
                       state_.RepoUsable() ? RepoDetectMode::refresh : RepoDetectMode::initial);
}

void MainWindow::StartWorkspaceRead(HWND window) {
  if (tasks_.RequestRefresh() != app::RefreshSchedule::start) {
    return;  // merged：已有读取在途，读完会补这一次；ignored：身份不再可用。
  }
  const app::ReadTicket ticket = tasks_.BeginRead();
  if (ticket.serial == 0) {
    refreshCycleActive_ = false;
    return;
  }
  platform::WorkspaceStatusRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kWorkspaceStatusTimeoutMs;
  // 凭据随请求交给工作线程，完成通知原样带回：界面只看它来决定这份结果还算不算数。
  request.readSerial = ticket.serial;
  request.bindingGeneration = ticket.generation;

  if (!refreshCycleActive_) {
    // 换仓库或首次识别：先把列表置成“正在读取”，旧仓库的行不留。
    git::WorkspaceSnapshot loading;
    loading.status = git::WorkspaceLoadStatus::loading;
    state_.SetWorkspace(std::move(loading));
    ApplyWorkspaceLists();
  }
  state_.SetStatusNote(tasks_.ReadStartedText());
  workspaceWorker_.Request(window, kWorkspaceStatusCompleted, std::move(request),
                           [](const platform::WorkspaceStatusRequest& pending) {
                             return platform::RunWorkspaceStatusLoad(pending);
                           });
}

void MainWindow::OnWorkspaceLoadCompleted(HWND window, uint64_t completionSerial) {
  platform::WorkspaceLoadOutcome outcome;
  if (!workspaceWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间已提交更晚的读取，这份结果不再有意义。
  }
  refreshCycleActive_ = false;
  if (tasks_.CompleteRead(outcome.readSerial) != app::ReadDisposition::accepted) {
    // 期间切换了仓库或换了 Git 程序：属于旧身份的快照绝不落地，
    // 当前列表保持原样，等属于新身份的那一次读取来填。
    return;
  }
  state_.SetWorkspace(std::move(outcome.snapshot));
  state_.SetStatusNote(tasks_.ReadFinishedText(state_.Workspace().message));
  // 成功摘要与非 0 失败原因都原样带上：失败时列表回到“读取失败 + 原因”，
  // 而不是保留一份看不准的旧内容；用户改正情况后再点刷新即可恢复。
  ApplyWorkspaceLists();
  UpdateCommandAvailability();
  RefreshTexts(window);
  if (tasks_.RefreshStillQueued()) {
    ScheduleRefresh(window);  // 在途期间又请求过刷新：合并成这一趟补读。
  }
}

void MainWindow::ResumeRepoDetectionWhenGitReady(HWND window) {
  // Git 刚刚可用时补上没做成的识别：包含首次启动（等待 Git 验证）与用户改正路径两种情况。
  if (!state_.GitUsable()) {
    return;
  }
  const app::RepoState& repo = state_.Repo();
  const std::wstring& path = state_.Info().repoPath;
  if (path.empty()) {
    return;
  }
  const bool waitingForGit = repo.detection.error == git::RepoError::gitUnavailable &&
                             (repo.status == app::RepoLoadStatus::unloaded ||
                              repo.status == app::RepoLoadStatus::failed);
  if (!waitingForGit) {
    return;  // 只补因为“Git 不可用”而没能识别的场合，其他失败原因不该被反复重试。
  }
  RequestRepoDetection(window, path, RepoDetectMode::initial);
}

void MainWindow::InitializeCommandWatching(HWND window) {
  commandRunner_.Startup(window);
  ::SetTimer(window, kGitOperationTimer, kGitOperationTickMs, nullptr);
}

void MainWindow::LaunchStatusOperation(HWND window) {
  if (!state_.GitUsable() || !state_.RepoUsable()) {
    return;
  }
  unsigned long long serial = 0;
  if (!tasks_.BeginOperation(L"status", &serial)) {
    return;  // 同一时刻只跑一个命令窗口操作（按钮此时也已禁用）。
  }
  const std::wstring gitExe = state_.Git().path;
  std::wstring repository = state_.Repo().detection.root;
  if (repository.empty()) {
    repository = state_.Info().repoPath;
  }

  git::CommandWindowOperation operation;
  operation.operationId = L"status";
  operation.displayName = L"status";
  operation.gitExecutable = gitExe;
  operation.repositoryDirectory = repository;
  // 只读地查看工作区状态；参数按数组提交，不进任何 shell 字符串。
  operation.arguments = {L"status"};

  uint64_t operationId = 0;
  platform::CommandWindowResult failure;
  if (!commandRunner_.Start(operation, &operationId, &failure)) {
    // 连 Git 都没启动起来：立刻释放槽位，别让一次没跑起来的启动把界面永久锁住。
    const app::OperationOutcome outcome =
        tasks_.FinishOperation(serial, failure.completion, failure.exitCode, failure.failureReason);
    state_.SetStatusNote(outcome.note);
    UpdateCommandAvailability();
    RefreshTexts(window);
    return;
  }
  activeOperation_ = ActiveOperation{serial, operationId, operation.displayName};
  state_.SetStatusNote(L"已在命令窗口启动 git status（" + repository + L"），等待 Git 退出码…");
  UpdateCommandAvailability();
  RefreshTexts(window);
}

void MainWindow::OnCommandWindowCompleted(HWND window, uint64_t operationId) {
  if (activeOperation_.serial == 0 || activeOperation_.runnerId != operationId) {
    return;  // 不是本窗口当前拥有的操作（保留的旧窗口、或已按通知丢失结案的迟到通知）。
  }
  platform::CommandWindowResult result;
  if (!commandRunner_.TakeResult(operationId, &result)) {
    return;  // 理论上不会发生：结果在通知之前登记；这里不删除 ID，等下一次通知或退出确认。
  }
  const unsigned long long serial = activeOperation_.serial;
  activeOperation_ = ActiveOperation{};
  const app::OperationOutcome outcome =
      tasks_.FinishOperation(serial, result.completion, result.exitCode, result.failureReason);
  if (!outcome.recognised) {
    return;
  }
  // 结论交给协调器保管：紧随其后的自动刷新会把它和仓库现状并排显示在同一行里。
  tasks_.RememberOperationConclusion(outcome.note);
  // 已完成但保留的窗口不影响后续操作，只清理已取回的结果记录。
  commandRunner_.ClearAllResults();
  UpdateCommandAvailability();
  // 无论成功还是失败都要重读一次：失败的操作同样可能已经改动仓库
  // （提交到一半、push 被拒、合并留下冲突），只有退出码决定要不要报成功。
  ScheduleRefresh(window);
  RefreshTexts(window);
}

void MainWindow::TickActiveOperations(HWND window) {
  // 轮询只负责把“执行中”刷成最新可见文本，并兜住完成通知丢失的场合
  // （真正的完成判定来自观察线程的通知，不靠这里的文案匹配）。
  if (activeOperation_.serial != 0 &&
      !commandRunner_.DescribeOperation(activeOperation_.runnerId, nullptr, nullptr)) {
    const unsigned long long serial = activeOperation_.serial;
    const std::wstring name = activeOperation_.displayName;
    activeOperation_ = ActiveOperation{};
    // 执行器已经不记得这个操作：按“结果未知”结案并释放槽位，
    // 否则一个再也等不到通知的操作会把后续写操作永久锁住。
    const app::OperationOutcome outcome =
        tasks_.ForgetOperation(serial, L"本程序没有收到「" + name + L"」的完成通知");
    if (outcome.recognised) {
      tasks_.RememberOperationConclusion(outcome.note);
      state_.SetStatusNote(outcome.note);
      UpdateCommandAvailability();
      ScheduleRefresh(window);  // 结果未知时更要重读：仓库究竟被改到什么程度只有 Git 自己知道。
    }
  }
  RefreshTexts(window);
}

void MainWindow::StopOperationWatching() {
  if (window_.get() != nullptr) {
    ::KillTimer(window_.get(), kGitOperationTimer);
    ::KillTimer(window_.get(), kRefreshTimer);
  }
  activeOperation_ = ActiveOperation{};
  commandRunner_.Shutdown();
}

bool MainWindow::ConfirmCloseWithActiveOperations(HWND window) {
  const std::vector<std::wstring> active = commandRunner_.ActiveOperationNames();
  if (active.empty()) {
    return true;
  }
  std::wstring message =
      L"以下 Git 操作仍在命令窗口中执行：\n";
  for (const std::wstring& name : active) {
    message += L"  • " + name + L"\n";
  }
  message += L"\n关闭本程序不会中断它们，也不会替你读取结果。\n是否仍要关闭？";
  const int answer = ::MessageBoxW(window, message.c_str(), L"Git 操作仍在进行",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  return answer == IDYES;
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
      OnCommand(window, wParam);
      return 0;
    case WM_TIMER:
      if (wParam == kGitVerifyTimer) {
        ::KillTimer(window, kGitVerifyTimer);
        CommitGitInput(window);
      } else if (wParam == kRepoDetectTimer) {
        ::KillTimer(window, kRepoDetectTimer);
        CommitRepoInput(window);
      } else if (wParam == kGitOperationTimer) {
        TickActiveOperations(window);
      } else if (wParam == kRefreshTimer) {
        ::KillTimer(window, kRefreshTimer);
        RunRefreshCycle(window);
      }
      return 0;
    case kGitProbeCompleted:
      OnGitProbeCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kRepoDetectCompleted:
      OnRepoDetectCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kWorkspaceStatusCompleted:
      OnWorkspaceLoadCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case platform::CommandWindowRunner::kCompletionMessage:
      OnCommandWindowCompleted(
          window, static_cast<uint64_t>(static_cast<uint32_t>(wParam)) |
                      (static_cast<uint64_t>(static_cast<int64_t>(lParam)) << 32));
      return 0;
    case kSplitterDragged:
      OnSplitterDragged(window, static_cast<int>(wParam), static_cast<int>(lParam));
      return 0;
    case WM_CLOSE:
      if (ConfirmCloseWithActiveOperations(window)) {
        ::DestroyWindow(window);
      }
      return 0;
    case WM_DESTROY:
      // 先停掉后台线程，再交还窗口所有权；旧线程不会再向已销毁窗口发通知。
      gitWorker_.Shutdown();
      repoWorker_.Shutdown();
      workspaceWorker_.Shutdown();
      StopOperationWatching();
      ::KillTimer(window, kGitVerifyTimer);
      ::KillTimer(window, kRepoDetectTimer);
      ::KillTimer(window, kGitOperationTimer);
      window_.Disown();
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, message, wParam, lParam);
  }
}

}  // namespace gc::ui
