#include "ui/wx/main_frame.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choicdlg.h>
#include <wx/clipbrd.h>
#include <wx/combobox.h>
#include <wx/dataobj.h>
#include <wx/dateevt.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include "git/commit_changes.h"
#include "git/commit_history.h"
#include "git/commit_identity.h"
#include "git/repository.h"
#include "git/shell_text.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/git_discovery.h"
#include "platform/git_toolchain.h"
#include "platform/local_time.h"
#include "platform/path.h"
#include "platform/scratch_files.h"
#include "platform/shell_dialect.h"
#include "platform/terminal.h"
#include "ui/wx/app_icon.h"
#include "ui/wx/history_dialog.h"
#include "ui/wx/wx_text.h"

namespace gc::ui::wx {
namespace {

constexpr uint64_t kGitVerifySource = 1;
constexpr uint64_t kRepoDetectSource = 2;
constexpr uint64_t kWorkspaceSource = 3;
constexpr uint64_t kAuthorConfigSource = 4;
constexpr uint64_t kCommitProbeSource = 5;
constexpr uint64_t kCommitChangesSource = 6;
constexpr uint64_t kCommitDetailSource = 7;
constexpr uint64_t kUndoProbeSource = 8;
constexpr uint64_t kUndoRecheckSource = 9;
constexpr uint64_t kRestoreProbeSource = 10;
constexpr uint64_t kRestoreRecheckSource = 11;

constexpr int kRefreshTimerId = wxID_HIGHEST + 101;
constexpr int kOperationTimerId = wxID_HIGHEST + 102;
constexpr int kGitVerifyTimerId = wxID_HIGHEST + 103;
constexpr int kRepoDetectTimerId = wxID_HIGHEST + 104;
constexpr int kHistorySaveTimerId = wxID_HIGHEST + 105;

// 超时与防抖取值与 Win32 过渡实现（ui/commands.h）逐项对齐：那是业务承诺，不是实现细节。
constexpr unsigned long kGitVerifyTimeoutMs = 3000;
constexpr unsigned long kRepoDetectTimeoutMs = 8000;
constexpr unsigned long kWorkspaceStatusTimeoutMs = 20000;
constexpr unsigned long kAuthorConfigTimeoutMs = 8000;
constexpr unsigned long kCommitChangesTimeoutMs = 20000;
constexpr unsigned long kCommitDetailTimeoutMs = 8000;
constexpr unsigned long kRefreshDebounceMs = 200;
constexpr unsigned long kInputDebounceMs = 500;
constexpr unsigned long kOperationPollMs = 500;
// 历史落账防抖与 Win32 过渡实现同值：一次终态、逐目标核实追加、开关切换都只重排这一个定时器。
constexpr unsigned long kHistorySaveDebounceMs = 1200;

// 尚未迁移到统一界面的入口共用的说明（不接空实现，点了也不会有任何动作）。
constexpr const wchar_t* kNotMigratedSuffix =
    L"｜该入口尚未迁移到统一界面（开发中）：这里只呈现禁用态，不接任何空实现；"
    L"Windows 过渡期的 Win32 界面（EvernightCommit.exe）仍保留完整入口供行为对照。";

// 标签只在内容真的变了才重设：空闲刷新不该反复触发重画（wx 各端口对同文本 SetLabel 的
// 重画代价不一致，这里统一在界面层挡掉）。
void SetLabelIfChanged(wxStaticText* text, const wxString& value) {
  if (text->GetLabel() != value) {
    text->SetLabel(value);
  }
}

}  // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, FromWide(L"Git 提交工具"), wxDefaultPosition, wxSize(1100, 760)),
      refreshTimer_(this, kRefreshTimerId),
      operationTimer_(this, kOperationTimerId),
      gitVerifyTimer_(this, kGitVerifyTimerId),
      repoDetectTimer_(this, kRepoDetectTimerId),
      historySaveTimer_(this, kHistorySaveTimerId) {
  const wxIcon icon = MakeAppIcon();
  if (icon.IsOk()) {
    SetIcon(icon);
  }
  dispatcher_ = platform::MakeQueuedMainThreadDispatcher();
  completionPump_ = std::make_unique<CompletionPump>(
      dispatcher_, [this](const platform::MainThreadDispatcher::Notice& notice) {
        HandleNotice(notice);
      });
  completionPump_->Start(25);

  gitWorker_.SetCompletionDispatcher(dispatcher_, kGitVerifySource);
  repoWorker_.SetCompletionDispatcher(dispatcher_, kRepoDetectSource);
  workspaceWorker_.SetCompletionDispatcher(dispatcher_, kWorkspaceSource);
  authorWorker_.SetCompletionDispatcher(dispatcher_, kAuthorConfigSource);
  commitChangesWorker_.SetCompletionDispatcher(dispatcher_, kCommitChangesSource);
  commitDetailWorker_.SetCompletionDispatcher(dispatcher_, kCommitDetailSource);
  commitFlow_.SetCompletionDispatcher(dispatcher_, kCommitProbeSource);
  undoFlow_.SetCompletionDispatcher(dispatcher_, kUndoProbeSource, kUndoRecheckSource);
  restoreFlow_.SetCompletionDispatcher(dispatcher_, kRestoreProbeSource, kRestoreRecheckSource);

  commandWindows_ = platform::MakeCommandWindowService();
  commandWindows_->SweepStaleOperationDirectories();

  BuildUi();
  BindEvents();
  InitializeOperationHistory();
  SetMinSize(wxSize(900, 700));
  Centre();

  const wxString cwd = wxGetCwd();
  state_.SetRepoPath(ToWide(cwd));
  repoPath_->ChangeValue(cwd);
  state_.SetStatusNote(L"正在验证 Git 程序并识别当前目录…");
  UpdateUi();

  CallAfter([this] { StartInitialization(); });
}

MainFrame::~MainFrame() {
  StopWorkers();
}

void MainFrame::StopWorkers() {
  refreshTimer_.Stop();
  operationTimer_.Stop();
  gitVerifyTimer_.Stop();
  repoDetectTimer_.Stop();
  historySaveTimer_.Stop();
  if (completionPump_) {
    completionPump_->Stop();
  }
  gitWorker_.BeginStop();
  repoWorker_.BeginStop();
  workspaceWorker_.BeginStop();
  authorWorker_.BeginStop();
  commitChangesWorker_.BeginStop();
  commitDetailWorker_.BeginStop();
  commitFlow_.BeginStop();
  undoFlow_.BeginStop();
  restoreFlow_.BeginStop();
  gitWorker_.Join();
  repoWorker_.Join();
  workspaceWorker_.Join();
  authorWorker_.Join();
  commitChangesWorker_.Join();
  commitDetailWorker_.Join();
  commitFlow_.JoinWorkers();
  undoFlow_.JoinWorkers();
  restoreFlow_.JoinWorkers();
  if (commitFlow_.Active()) {
    commitFlow_.ReleaseAttempt(true);
  }
  if (commandWindows_) {
    commandWindows_->Shutdown();
  }
  if (dispatcher_) {
    dispatcher_->Close();
  }
}

void MainFrame::BuildUi() {
  auto* frameSizer = new wxBoxSizer(wxVERTICAL);

  auto* topPanel = new wxPanel(this);
  auto* top = new wxBoxSizer(wxVERTICAL);

  auto* repoRow = new wxBoxSizer(wxHORIZONTAL);
  repoRow->Add(new wxStaticText(topPanel, wxID_ANY, FromWide(L"本地仓库：")), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  repoPath_ = new wxTextCtrl(topPanel, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                             wxTE_PROCESS_ENTER);
  repoRow->Add(repoPath_, 1, wxEXPAND | wxRIGHT, 6);
  browseRepo_ = new wxButton(topPanel, wxID_ANY, FromWide(L"浏览…"));
  repoRow->Add(browseRepo_, 0, wxRIGHT, 12);
  repoRow->Add(new wxStaticText(topPanel, wxID_ANY, FromWide(L"Git 程序：")), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  gitCombo_ = new wxComboBox(topPanel, wxID_ANY, wxString(), wxDefaultPosition, wxSize(260, -1), 0,
                             nullptr);
  repoRow->Add(gitCombo_, 0, wxRIGHT, 6);
  browseGit_ = new wxButton(topPanel, wxID_ANY, FromWide(L"浏览…"));
  repoRow->Add(browseGit_, 0, wxRIGHT, 12);
  terminalButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"终端…"));
  repoRow->Add(terminalButton_, 0, wxRIGHT, 6);
  fetchButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"fetch"));
  repoRow->Add(fetchButton_, 0, wxRIGHT, 6);
  pullButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"pull"));
  repoRow->Add(pullButton_, 0, wxRIGHT, 6);
  statusButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"status"));
  repoRow->Add(statusButton_, 0);
  top->Add(repoRow, 0, wxEXPAND | wxALL, 10);

  gitLabel_ = new wxStaticText(topPanel, wxID_ANY, FromWide(L"Git：正在发现…"));
  top->Add(gitLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

  auto* summaryRow = new wxBoxSizer(wxHORIZONTAL);
  repoSummary_ = new wxStaticText(topPanel, wxID_ANY, FromWide(L"尚未识别仓库。"),
                                  wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
  summaryRow->Add(repoSummary_, 1, wxEXPAND | wxRIGHT, 8);
  enterSubmoduleButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"进入子模块"));
  summaryRow->Add(enterSubmoduleButton_, 0, wxRIGHT, 6);
  returnToParentButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"返回父仓库"));
  summaryRow->Add(returnToParentButton_, 0);
  top->Add(summaryRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

  auto* actionRow = new wxBoxSizer(wxHORIZONTAL);
  refreshButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"刷新"));
  actionRow->Add(refreshButton_, 0, wxRIGHT, 6);
  createCommit_ = new wxButton(topPanel, wxID_ANY, FromWide(L"创建提交…"));
  actionRow->Add(createCommit_, 0, wxRIGHT, 6);
  undoCommitButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"撤回最近提交"));
  actionRow->Add(undoCommitButton_, 0, wxRIGHT, 6);
  pushButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"推送"));
  actionRow->Add(pushButton_, 0, wxRIGHT, 6);
  conflictViewButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"查看冲突状态"));
  actionRow->Add(conflictViewButton_, 0, wxRIGHT, 6);
  conflictContinueButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"继续该流程"));
  actionRow->Add(conflictContinueButton_, 0, wxRIGHT, 6);
  conflictAbortButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"中止该流程"));
  actionRow->Add(conflictAbortButton_, 0, wxRIGHT, 6);
  historyBrowseButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"操作历史…"));
  actionRow->Add(historyBrowseButton_, 0);
  top->Add(actionRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

  auto* prefsRow = new wxBoxSizer(wxHORIZONTAL);
  persistRecordsCheck_ = new wxCheckBox(topPanel, wxID_ANY, FromWide(L"保存记录"));
  prefsRow->Add(persistRecordsCheck_, 0, wxRIGHT, 10);
  persistDraftsCheck_ = new wxCheckBox(topPanel, wxID_ANY, FromWide(L"保存草稿"));
  prefsRow->Add(persistDraftsCheck_, 0, wxRIGHT, 10);
  clearPrefsButton_ = new wxButton(topPanel, wxID_ANY, FromWide(L"清除已存记录"));
  prefsRow->Add(clearPrefsButton_, 0, wxRIGHT, 16);
  historyRecordCheck_ = new wxCheckBox(topPanel, wxID_ANY, FromWide(L"记录操作历史"));
  prefsRow->Add(historyRecordCheck_, 0);
  top->Add(prefsRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // 尚未迁移的入口：一律禁用并说明开发状态（点了不会有动作，也不接空实现）。
  const wxString notMigrated = FromWide(kNotMigratedSuffix);
  fetchButton_->Enable(false);
  fetchButton_->SetToolTip(FromWide(
                               L"fetch 尚未迁移到统一界面（开发中）。\r\n") +
                           notMigrated);
  pullButton_->Enable(false);
  pullButton_->SetToolTip(FromWide(L"pull 尚未迁移到统一界面（开发中）。\r\n") + notMigrated);
  pushButton_->Enable(false);
  pushButton_->SetToolTip(FromWide(L"推送尚未迁移到统一界面（开发中）。\r\n") + notMigrated);
  conflictViewButton_->Enable(false);
  conflictContinueButton_->Enable(false);
  conflictAbortButton_->Enable(false);
  conflictViewButton_->SetToolTip(
      FromWide(L"冲突与暂停流程的三个入口尚未迁移到统一界面（开发中）。\r\n") + notMigrated);
  conflictContinueButton_->SetToolTip(conflictViewButton_->GetToolTip());
  conflictAbortButton_->SetToolTip(conflictViewButton_->GetToolTip());
  enterSubmoduleButton_->Enable(false);
  returnToParentButton_->Enable(false);
  enterSubmoduleButton_->SetToolTip(
      FromWide(L"子模块导航尚未迁移到统一界面（开发中）。\r\n") + notMigrated);
  returnToParentButton_->SetToolTip(enterSubmoduleButton_->GetToolTip());
  historyBrowseButton_->Enable(true);
  historyBrowseButton_->SetToolTip(FromWide(
      L"操作历史：查看本程序做过的写操作（含启动与终态、逐目标核实、引用级恢复线索）。\r\n"
      L"选中一条「引用级可回退」的记录可重新预检现状后确认恢复：只移动那一个本地分支引用，"
      L"索引与工作区一字不动，远端不受影响；绝不自动 force push / hard reset / clean。"));
  undoCommitButton_->SetToolTip(FromWide(
      L"把当前分支的最新一次提交挪回它的第一父提交（软撤回）：只移动那一个分支引用，"
      L"索引与工作区一个字节都不动，原提交的改动会留在「已暂存的更改」里。\r\n"
      L"不是 revert，也不是 reset --hard；绝不触碰远端。点击后先只读预检，确认后再复核一次。"));
  persistRecordsCheck_->Enable(false);
  persistDraftsCheck_->Enable(false);
  clearPrefsButton_->Enable(false);
  persistRecordsCheck_->SetToolTip(FromWide(
      L"持久化（最近仓库/Git 程序/布局/草稿）尚未迁移到统一界面（开发中）：\r\n"
      L"这里不读写任何用户数据文件。\r\n") + notMigrated);
  persistDraftsCheck_->SetToolTip(persistRecordsCheck_->GetToolTip());
  clearPrefsButton_->SetToolTip(persistRecordsCheck_->GetToolTip());
  historyRecordCheck_->SetToolTip(FromWide(
      L"记录本程序启动的写操作（含启动与终态、逐目标核实与引用级恢复线索）。\r\n"
      L"默认关闭；关闭时一个字节都不写。Windows 落在当前用户的应用数据目录；"
      L"Linux 在后续步骤接入持久化之前只保存在本次运行的内存里，关闭界面即丢失。"));

  topPanel->SetSizer(top);
  frameSizer->Add(topPanel, 0, wxEXPAND);

  // 两页变化区（变化信息 / 提交历史）与提交表单（表单由面板创建在变化信息页下半区）。
  panel_ = new ChangesPanel(this);
  form_ = panel_->form();
  frameSizer->Add(panel_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

  // 两条说明通道固定在窗口底部：仓库结论与表单自己的话都不会被滚动区带走。
  auto* statusPanel = new wxPanel(this);
  statusPanel->SetMinSize(wxSize(-1, 56));
  auto* statusSizer = new wxBoxSizer(wxVERTICAL);
  statusText_ = new wxStaticText(statusPanel, wxID_ANY, FromWide(state_.StatusNote()),
                                 wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
  formNoteText_ = new wxStaticText(statusPanel, wxID_ANY, FromWide(state_.FormNote()),
                                   wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
  formNoteText_->SetForegroundColour(wxColour(70, 70, 70));
  statusSizer->Add(statusText_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
  statusSizer->Add(formNoteText_, 0, wxEXPAND | wxALL, 8);
  statusPanel->SetSizer(statusSizer);
  frameSizer->Add(statusPanel, 0, wxEXPAND | wxBOTTOM, 4);

  SetSizer(frameSizer);

  // 工具提示：文案与 Win32 过渡实现逐条对齐（迁移的是说明，不是新写的文案）。
  repoPath_->SetToolTip(
      FromWide(L"输入本地仓库目录（也可以是其子目录）；识别在后台只读执行，不会改动仓库。"));
  browseRepo_->SetToolTip(FromWide(
      L"选择本地仓库目录；也可在输入框直接键入路径（停顿后自动识别）。"
      L"支持仓库的子目录，识别时会上溯到工作区根。"));
  browseGit_->SetToolTip(
      FromWide(L"浏览选择 git 程序；选定后立即在后台运行 git --version 验证。"));
  gitCombo_->SetToolTip(FromWide(
      L"Git 程序路径：下拉里是 PATH 上发现的候选，也可以直接键入或浏览选择。\r\n"
      L"任何一条都要经过后台 git --version 验证才算可用。"));
  terminalButton_->SetToolTip(FromWide(
      L"选择命令窗口用哪个终端模拟器（Linux）：命令窗口里的 Git 由选中的终端承载。\r\n"
      L"选择只对本次运行生效；终端设置的持久化尚未迁移。"));
  statusButton_->SetToolTip(FromWide(
      L"在新命令窗口里执行 git status：显示真实命令与输出，Git 结束窗口仍保留；"
      L"本程序通过结果文件获知退出码。"));
  refreshButton_->SetToolTip(FromWide(
      L"重新读取当前仓库的摘要、两个更改列表与提交历史：只在本程序后台执行只读 Git 查询，"
      L"不弹出命令窗口。\r\n在外部终端里改过仓库、或命令窗口里的 Git 已结束，都可以点这里恢复真实状态。"));
  createCommit_->SetToolTip(FromWide(
      L"把「已暂存的更改」里现在的索引内容提交出去：在命令窗口里执行 git commit，"
      L"显示真实命令与输出，执行结束后窗口保留，本程序自动重读仓库状态。\r\n"
      L"点击后先做一次只读重读，确认框里摆出的是刚刚读回的现状；只提交索引里那一份，绝不带 -a。\r\n"
      L"命令里没有 --no-verify：仓库的 hooks 与签名设置照常生效。"));
  panel_->unstagedHint()->SetToolTip(FromWide(
      L"双击某一行可在命令窗口里查看它的差异（未跟踪文件按其内容显示）；只读，不改动仓库。"));
  panel_->stagedHint()->SetToolTip(panel_->unstagedHint()->GetToolTip());
  panel_->stageAllButton()->SetToolTip(FromWide(
      L"「全部加入 →」：把「未暂存的更改」里此刻全部条目一次交给 git 原生的批量命令 git add .：\r\n"
      L"它不看选中项；只想暂存其中几项，请用下面的「加入暂存区 →」。"));
  panel_->stageSelectedButton()->SetToolTip(FromWide(
      L"把「未暂存的更改」里选中的条目交给 git add：在命令窗口里显示真实命令与完整输出。\r\n"
      L"只处理选中的行（可多选）：没有选中就是「什么都不做」，绝不会代为暂存全部改动。"));
  panel_->unstageSelectedButton()->SetToolTip(FromWide(
      L"把「已暂存的更改」里选中的条目从索引撤回：这一步只写索引，绝不改写工作区。\r\n"
      L"只处理选中的行（可多选）：没有选中就是「什么都不做」。"));
  panel_->unstageAllButton()->SetToolTip(FromWide(
      L"「← 全部移出」：把「已暂存的更改」里此刻全部条目从索引撤回（只写索引）。\r\n"
      L"仓库里只要还有未合并（冲突）条目，这个按钮会整份拒绝并说明原因。"));
}

void MainFrame::BindEvents() {
  browseRepo_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxDirDialog dialog(this, FromWide(L"选择本地 Git 仓库"), repoPath_->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
      return;  // 取消或空结果：不改任何状态。
    }
    repoPath_->ChangeValue(dialog.GetPath());
    RequestRepoDetection(true);
  });
  repoPath_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { RequestRepoDetection(true); });
  repoPath_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
    repoDetectTimer_.StartOnce(static_cast<int>(kInputDebounceMs));
  });

  gitCombo_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
    if (settingGitCombo_) {
      return;
    }
    gitVerifyTimer_.StartOnce(static_cast<int>(kInputDebounceMs));
  });
  gitCombo_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
    if (settingGitCombo_) {
      return;
    }
    gitVerifyTimer_.Stop();
    CommitGitInput();  // 从下拉里选定：不再防抖，立即验证。
  });
  gitCombo_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
    if (!settingGitCombo_) {
      gitVerifyTimer_.Stop();
      CommitGitInput();  // 失焦立即提交；与当前路径相同时 CommitGitInput 自己会跳过。
    }
    event.Skip();
  });
  browseGit_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog dialog(this, FromWide(L"选择 Git 程序"), wxString(), L"git",
                        FromWide(L"Git 程序|git.exe;git|所有文件|*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
      return;
    }
    gitVerifyTimer_.Stop();
    const std::wstring normalized = platform::NormalizeGitExeInput(ToWide(dialog.GetPath()));
    settingGitCombo_ = true;
    gitCombo_->SetValue(FromWide(normalized));
    settingGitCombo_ = false;
    RequestGitVerification(normalized);
  });
  terminalButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenTerminalDialog(); });
  statusButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LaunchStatusOperation(); });
  refreshButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (!state_.GitUsable()) {
      SetStatus(L"Git 程序还不能用，没法刷新。请确认本机已安装 Git 并在 PATH 上。");
      return;
    }
    SetStatus(L"正在后台刷新：重新识别仓库并读取 git status（只读查询，不弹命令窗口）…");
    if (refreshCycleActive_) {
      refreshPending_ = true;
    } else {
      refreshCycleActive_ = true;
      RequestRepoDetection(false);
    }
  });

  panel_->stageSelectedButton()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StageSelected(); });
  panel_->stageAllButton()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StageAll(); });
  panel_->unstageSelectedButton()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { UnstageSelected(); });
  panel_->unstageAllButton()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { UnstageAll(); });
  createCommit_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CreateCommit(); });
  undoCommitButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { UndoLastCommit(); });
  historyBrowseButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowOperationHistory(); });
  historyRecordCheck_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
    historyLog_.historyEnabled = event.IsChecked();
    if (historyWritable_) {
      historyIntents_.policy = true;
      ScheduleHistorySave();
      SetFormNote(event.IsChecked()
                      ? L"已开始记录操作历史：" + HistoryStorageLine()
                      : L"已停止记录操作历史：不再新增记录，已有的保留不动。");
    } else {
      SetFormNote(event.IsChecked()
                      ? L"已开始记录操作历史，但只保存在本次运行的内存里：" +
                            historyStorage_.reason + L"（开关与已有记录都不会写盘。）"
                      : L"已停止记录操作历史：本次运行不再新增内存记录。");
    }
    UpdateUi();
  });

  panel_->onStagingSelectionChanged = [this] {
    if (panel_->UpdatingLists()) {
      return;
    }
    // 点击瞬间按行号把选中条目拷成快照（行数与模型不符就整份拒绝），这里只维护显示语义。
  };
  panel_->onHistorySelectionChanged = [this] {
    if (panel_->UpdatingLists()) {
      return;
    }
    ReconcileSelectedCommit();
  };
  panel_->onChangesDoubleClicked = [this](git::ChangeSide side, int row) {
    OnChangesDoubleClicked(side, row);
  };
  panel_->onHistoryDoubleClicked = [this](int row) { OnHistoryDoubleClicked(row); };
  panel_->onFileDoubleClicked = [this](int row) { OnFileDoubleClicked(row); };
  panel_->onCopyDetail = [this] { CopyCommitDetailToClipboard(); };

  form_->onFieldEdited = [this](app::CommitFormSession::Field field) {
    formSession_.NoteUserEdit(field);
    if (field == app::CommitFormSession::Field::author) {
      SetFormNote(L"作者栏已手动修改：只作用于本次提交，不会写回任何 Git 配置。");
    }
    RunFormValidation({});
  };
  form_->onAuthorCleared = [this] { HandleAuthorCleared(); };
  form_->onTimesEdited = [this] {
    SetFormNote(L"时间已手动选择：创建提交会使用这里显示的时刻与偏移，不再自动改成当前时间。"
                L"要退回此刻请点“恢复当前时间”。");
    RunFormValidation({});
  };
  form_->onChanged = [this] { RunFormValidation({}); };

  refreshTimer_.Bind(wxEVT_TIMER, &MainFrame::OnRefreshTimer, this, kRefreshTimerId);
  operationTimer_.Bind(wxEVT_TIMER, &MainFrame::OnOperationTimer, this, kOperationTimerId);
  gitVerifyTimer_.Bind(wxEVT_TIMER, &MainFrame::OnGitVerifyDebounce, this, kGitVerifyTimerId);
  repoDetectTimer_.Bind(wxEVT_TIMER, &MainFrame::OnRepoDetectDebounce, this, kRepoDetectTimerId);
  historySaveTimer_.Bind(wxEVT_TIMER, &MainFrame::OnHistorySaveTimer, this, kHistorySaveTimerId);
  Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
}

void MainFrame::StartInitialization() {
  const std::vector<std::wstring> candidates = platform::DiscoverGitCandidates();
  SetGitCandidates(candidates);
  if (candidates.empty()) {
    SetStatus(L"没有在 PATH 上找到 Git 程序。请先安装 Git，或在“Git 程序”里手动输入路径、"
              L"用“浏览…”选择。");
    app::GitToolState tool;
    tool.status = app::GitExeStatus::unverified;
    tool.message = L"当前进程 PATH 中未找到 git 程序，请手动输入路径或用“浏览…”选择。";
    state_.SetGitTool(std::move(tool));
    UpdateUi();
    return;
  }
  RequestGitVerification(platform::NormalizeGitExeInput(candidates.front()));
}

void MainFrame::SetGitCandidates(const std::vector<std::wstring>& candidates) {
  wxArrayString items;
  for (const std::wstring& candidate : candidates) {
    items.Add(FromWide(candidate));
  }
  const wxString keep = gitCombo_->GetValue();
  settingGitCombo_ = true;
  gitCombo_->Clear();
  gitCombo_->Append(items);
  gitCombo_->SetValue(keep);
  settingGitCombo_ = false;
}

void MainFrame::CommitGitInput() {
  const std::wstring raw = ToWide(gitCombo_->GetValue());
  const std::wstring normalized = platform::NormalizeGitExeInput(raw);
  if (normalized.empty()) {
    app::GitToolState tool;
    tool.status = app::GitExeStatus::unverified;
    tool.message = L"未选择 Git 程序。";
    state_.SetGitTool(std::move(tool));
    SetStatus(L"Git 未设置：请输入 git 程序路径或用“浏览…”选择。");
    UpdateUi();
    return;
  }
  const app::GitToolState& current = state_.Git();
  if (normalized == current.path &&
      (current.status == app::GitExeStatus::verifying ||
       current.status == app::GitExeStatus::verified)) {
    return;  // 程序回填文本回环触发的重复提交：跳过。
  }
  if (normalized != raw) {
    settingGitCombo_ = true;
    gitCombo_->SetValue(FromWide(normalized));
    settingGitCombo_ = false;
  }
  RequestGitVerification(normalized);
}

void MainFrame::OnGitVerifyDebounce(wxTimerEvent&) {
  if (!closing_) {
    CommitGitInput();
  }
}

void MainFrame::OnRepoDetectDebounce(wxTimerEvent&) {
  if (closing_) {
    return;
  }
  const std::wstring text = ToWide(repoPath_->GetValue());
  if (text.empty()) {
    tasks_.UnbindRepository();
    ClearWorkspaceState();
    app::RepoState repo;
    repo.status = app::RepoLoadStatus::unloaded;
    state_.SetRepo(std::move(repo));
    SetStatus(L"本地仓库路径为空，没有发起识别。");
    UpdateUi();
    return;
  }
  refreshCycleActive_ = true;
  RequestRepoDetection(true);
}

void MainFrame::RequestGitVerification(const std::wstring& path) {
  app::GitToolState tool;
  tool.status = app::GitExeStatus::verifying;
  tool.path = path;
  tool.message = L"正在后台验证 Git 程序（git --version）…";
  state_.SetGitTool(std::move(tool));
  state_.SetGitExePath(path);
  SetStatus(L"正在验证 Git 程序：" + path);
  UpdateUi();
  gitWorker_.Request(nullptr, 0, path, [](const std::wstring& exePath) {
    return platform::VerifyGitExe(exePath, kGitVerifyTimeoutMs);
  });
}

void MainFrame::HandleNotice(const platform::MainThreadDispatcher::Notice& notice) {
  if (notice.sourceId == kGitVerifySource) {
    OnGitVerificationCompleted(notice.serial);
  } else if (notice.sourceId == kRepoDetectSource) {
    OnRepoDetectionCompleted(notice.serial);
  } else if (notice.sourceId == kWorkspaceSource) {
    OnWorkspaceReadCompleted(notice.serial);
  } else if (notice.sourceId == kAuthorConfigSource) {
    OnAuthorConfigCompleted(notice.serial);
  } else if (notice.sourceId == kCommitProbeSource) {
    OnCommitProbeCompleted(notice.serial);
  } else if (notice.sourceId == kCommitChangesSource) {
    OnCommitChangesCompleted(notice.serial);
  } else if (notice.sourceId == kCommitDetailSource) {
    OnCommitDetailCompleted(notice.serial);
  } else if (notice.sourceId == kUndoProbeSource) {
    OnUndoProbeCompleted(notice.serial);
  } else if (notice.sourceId == kUndoRecheckSource) {
    OnUndoRecheckCompleted(notice.serial);
  } else if (notice.sourceId == kRestoreProbeSource) {
    OnRestoreProbeCompleted(notice.serial);
  } else if (notice.sourceId == kRestoreRecheckSource) {
    OnRestoreRecheckCompleted(notice.serial);
  }
}

void MainFrame::OnGitVerificationCompleted(uint64_t serial) {
  platform::GitExeVerification verification;
  if (!gitWorker_.FetchLatest(serial, &verification)) {
    return;
  }
  // 换 Git 程序同样是一次身份变化：旧 Git 读回来的摘要与列表都要作废。
  const std::wstring previousGitPath = state_.Git().path;
  app::GitToolState tool;
  tool.path = verification.path;
  tool.message = verification.message;
  if (verification.outcome == git::GitProbeOutcome::verified) {
    tool.status = app::GitExeStatus::verified;
    tool.version = verification.version;
    state_.SetGitTool(std::move(tool));
    state_.SetGitExePath(verification.path);
    SetStatus(L"Git 程序可用：" + verification.path + L"（版本 " + verification.version + L"）。");
    settingGitCombo_ = true;
    gitCombo_->SetValue(FromWide(verification.path));
    settingGitCombo_ = false;
    repoPath_->ChangeValue(FromWide(platform::ToAbsolutePath(ToWide(repoPath_->GetValue()))));
    const bool switchedGit = !previousGitPath.empty() && previousGitPath != verification.path &&
                             !state_.Info().repoPath.empty();
    refreshCycleActive_ = true;
    RequestRepoDetection(true);
    if (switchedGit) {
      SetStatus(L"已改用另一个 Git 程序重新识别当前仓库（在途的旧结果按身份作废）。");
    }
  } else {
    tool.status = app::GitExeStatus::invalid;
    state_.SetGitTool(std::move(tool));
    SetStatus(L"Git 程序不可用：" + verification.message);
    UpdateUi();
  }
}

void MainFrame::RequestRepoDetection(bool initial) {
  if (!state_.GitUsable()) {
    SetStatus(L"Git 程序尚不可用，无法识别仓库。");
    refreshCycleActive_ = false;
    UpdateUi();
    return;
  }
  const std::wstring input = ToWide(repoPath_->GetValue());
  const std::wstring directory = platform::ToAbsolutePath(input);
  if (directory.empty()) {
    SetStatus(L"本地仓库路径为空，没有发起识别。");
    refreshCycleActive_ = false;
    UpdateUi();
    return;
  }
  state_.SetRepoPath(directory);
  repoPath_->ChangeValue(FromWide(directory));
  app::RepoState repo;
  repo.status = app::RepoLoadStatus::detecting;
  state_.SetRepo(std::move(repo));
  if (initial) {
    ClearWorkspaceState();
  }
  SetStatus(L"正在后台识别仓库（只读 Git 查询，不弹命令窗口）：" + directory);

  platform::RepoDetectRequest request;
  request.exePath = state_.Git().path;
  request.directory = directory;
  request.timeoutMilliseconds = kRepoDetectTimeoutMs;
  repoWorker_.Request(nullptr, 0, std::move(request),
                      [](const platform::RepoDetectRequest& pending) {
                        return platform::RunRepositoryDetection(pending);
                      });
  UpdateUi();
}

void MainFrame::OnRepoDetectionCompleted(uint64_t serial) {
  git::RepoDetection detection;
  if (!repoWorker_.FetchLatest(serial, &detection)) {
    return;
  }
  if (detection.kind == git::RepoKind::failed) {
    tasks_.UnbindRepository();
    app::RepoState repo;
    repo.status = app::RepoLoadStatus::failed;
    repo.detection = detection;
    state_.SetRepo(std::move(repo));
    ClearWorkspaceState();
    git::WorkspaceSnapshot failed;
    failed.status = git::WorkspaceLoadStatus::failed;
    failed.error = detection.error;
    failed.message = detection.message;
    state_.SetWorkspace(std::move(failed));
    SetStatus(detection.message);
    refreshCycleActive_ = false;
    if (commitFlow_.Active()) {
      commitFlow_.OnWorkspaceLoaded(*this, CaptureOperationContext(), false);
    }
    UpdateUi();
    if (refreshPending_) {
      refreshPending_ = false;
      ScheduleRefreshSoon();
    }
    return;
  }

  const bool workspace = git::KindHasWorkspace(detection.kind);
  if (!workspace) {
    tasks_.UnbindRepository();
    app::RepoState repo;
    repo.status = app::RepoLoadStatus::loaded;
    repo.detection = detection;
    state_.SetRepo(std::move(repo));
    ClearWorkspaceState();
    git::WorkspaceSnapshot failed;
    failed.status = git::WorkspaceLoadStatus::failed;
    failed.error = git::RepoError::inputNotDirectory;
    failed.message = detection.message;
    state_.SetWorkspace(std::move(failed));
    SetStatus(detection.message);
    refreshCycleActive_ = false;
    if (commitFlow_.Active()) {
      commitFlow_.OnWorkspaceLoaded(*this, CaptureOperationContext(), false);
    }
    UpdateUi();
    return;
  }

  if (!ApplyDetectedRepository(detection)) {
    refreshCycleActive_ = false;
    UpdateUi();
    return;
  }

  const bool identityChanged = tasks_.BindRepository(state_.Git().path, detection.root);
  app::RepoState repo;
  repo.status = app::RepoLoadStatus::loaded;
  repo.detection = detection;
  state_.SetRepo(std::move(repo));
  state_.SetRepoPath(detection.root);
  repoPath_->ChangeValue(FromWide(detection.root));
  if (identityChanged) {
    ClearWorkspaceState();
    app::AuthorState author;
    author.status = app::AuthorLoadStatus::unloaded;
    state_.SetAuthor(std::move(author));
  }
  if (identityChanged || state_.Author().status == app::AuthorLoadStatus::unloaded ||
      state_.Author().status == app::AuthorLoadStatus::failed) {
    RequestAuthorConfig();
  }
  RequestWorkspaceRead();
  UpdateUi();
}

bool MainFrame::ApplyDetectedRepository(const git::RepoDetection& detection) {
  const std::wstring key = git::CanonicalPathKey(detection.root);
  if (formSession_.NeedsSwitchDecision(key)) {
    const std::wstring previous =
        state_.Repo().detection.root.empty() ? formSession_.BoundRepository()
                                             : state_.Repo().detection.root;
    wxMessageDialog dialog(
        this,
        FromWide(L"提交表单里已有你输入的内容，而这次要换到另一个工作区：\n\n旧仓库：" + previous +
                 L"\n新仓库：" + detection.root +
                 L"\n\n“保留”会把现有内容带到新仓库；“放弃”会清空标题、描述、合作者与时间。"),
        FromWide(L"切换仓库前请确认"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
    dialog.SetYesNoCancelLabels(FromWide(L"保留"), FromWide(L"放弃"), FromWide(L"取消"));
    const int answer = dialog.ShowModal();
    if (answer == wxID_CANCEL) {
      if (!previous.empty()) {
        repoPath_->ChangeValue(FromWide(previous));
        state_.SetRepoPath(previous);
      }
      SetStatus(L"已取消切换仓库：仓库、索引、工作区与表单内容都没有改动。");
      return false;
    }
    if (answer == wxID_YES) {
      formSession_.ChooseKeep(key);
      SetFormNote(L"已保留表单内容用于新仓库；旧仓库默认作者从此按用户内容对待，不会被新仓库的配置覆盖。");
    } else {
      formSession_.ChooseDiscard(key);
      form_->ClearAllFields();
      const platform::LocalInstant now = platform::CurrentLocalInstant();
      if (now.valid) {
        form_->SetTimes(now.wall, now.wall, true);
      }
      SetFormNote(L"已放弃旧仓库的表单内容：标题、描述、作者、合作者与时间都重置了。");
    }
  } else {
    formSession_.BindRepository(key);
  }
  return true;
}

void MainFrame::RequestWorkspaceRead() {
  if (!tasks_.Identity().Valid()) {
    git::WorkspaceSnapshot failed = state_.Workspace();
    failed.status = git::WorkspaceLoadStatus::failed;
    failed.message = L"还没有绑定可用的工作区根目录，没有发起 git status。";
    state_.SetWorkspace(std::move(failed));
    if (commitFlow_.Active()) {
      commitFlow_.OnWorkspaceLoaded(*this, CaptureOperationContext(), false);
    }
    refreshCycleActive_ = false;
    UpdateUi();
    return;
  }
  const app::RefreshSchedule schedule = tasks_.RequestRefresh();
  if (schedule == app::RefreshSchedule::ignored) {
    refreshCycleActive_ = false;
    UpdateUi();
    return;
  }
  if (schedule == app::RefreshSchedule::merged) {
    refreshPending_ = true;
    return;
  }
  const app::ReadTicket ticket = tasks_.BeginRead();
  if (ticket.serial == 0) {
    refreshCycleActive_ = false;
    UpdateUi();
    return;
  }
  git::WorkspaceSnapshot loading = state_.Workspace();
  loading.status = git::WorkspaceLoadStatus::loading;
  state_.SetWorkspace(std::move(loading));
  RebuildChangesLists();

  platform::WorkspaceStatusRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = tasks_.Identity().workTreeRoot;
  request.timeoutMilliseconds = kWorkspaceStatusTimeoutMs;
  request.repositoryHasCommits = state_.Repo().detection.headResolved;
  request.readSerial = ticket.serial;
  request.bindingGeneration = ticket.generation;
  workspaceWorker_.Request(nullptr, 0, std::move(request),
                           [](const platform::WorkspaceStatusRequest& pending) {
                             return platform::RunWorkspaceStatusLoad(pending);
                           });
  UpdateUi();
}

void MainFrame::OnWorkspaceReadCompleted(uint64_t serial) {
  platform::WorkspaceLoadOutcome outcome;
  if (!workspaceWorker_.FetchLatest(serial, &outcome)) {
    return;
  }
  const app::ReadDisposition disposition = tasks_.CompleteRead(outcome.readSerial);
  if (disposition != app::ReadDisposition::accepted) {
    return;
  }
  const bool succeeded = outcome.snapshot.status == git::WorkspaceLoadStatus::loaded;
  state_.SetWorkspace(outcome.snapshot);
  refreshCycleActive_ = false;
  RebuildChangesLists();

  std::wstring summary = outcome.snapshot.message;
  if (summary.empty() && succeeded) {
    summary = git::BuildWorkspaceSummary(outcome.snapshot.model);
  }
  SetStatus(tasks_.ReadFinishedText(summary));

  commitFlow_.OnWorkspaceLoaded(*this, CaptureOperationContext(), succeeded);
  UpdateUi();
  ReconcileSelectedCommit();  // 重建结束后按新列表统一重对一次（与 Win32 同一收口）。
  if (tasks_.RefreshStillQueued() || refreshPending_) {
    refreshPending_ = false;
    ScheduleRefreshSoon();
  }
}

void MainFrame::RequestAuthorConfig() {
  if (!tasks_.Identity().Valid()) {
    return;
  }
  platform::AuthorConfigRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = tasks_.Identity().workTreeRoot;
  request.timeoutMilliseconds = kAuthorConfigTimeoutMs;
  authorWorker_.Request(nullptr, 0, std::move(request),
                        [](const platform::AuthorConfigRequest& pending) {
                          return platform::RunAuthorIdentityLoad(pending);
                        });
}

void MainFrame::OnAuthorConfigCompleted(uint64_t serial) {
  platform::AuthorConfigOutcome outcome;
  if (!authorWorker_.FetchLatest(serial, &outcome)) {
    return;
  }
  if (!tasks_.Identity().Valid() ||
      !git::PathsEqualFolded(outcome.repositoryDirectory, tasks_.Identity().workTreeRoot)) {
    return;
  }
  app::AuthorState author;
  author.status = outcome.config.ReadFailed() ? app::AuthorLoadStatus::failed
                                              : app::AuthorLoadStatus::loaded;
  author.config = outcome.config;
  state_.SetAuthor(std::move(author));
  ApplyAuthorDefaultIfAllowed();
  const std::wstring notice = git::BuildIdentityConfigNotice(outcome.config);
  if (!notice.empty()) {
    SetFormNote(notice);
  } else {
    RunFormValidation({});
  }
  UpdateUi();
}

void MainFrame::RequestCommitChanges(int row) {
  // 只读查询，但有一个硬前提：仓库身份还成立。不成立时不动清单（宁可停在引导语上）。
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  if (!state_.GitUsable() || !state_.RepoUsable() || repositoryRoot.empty() ||
      !git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot) ||
      state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    return;
  }
  if (panel_->HistoryRowCount() !=
      static_cast<int>(state_.WorkspaceModel().recentCommits.size())) {
    return;  // 显示的行数与已读取的历史对不上：宁可不读。
  }
  const git::CommitItem* item = panel_->CommitItemAt(row);
  if (item == nullptr) {
    return;
  }
  platform::CommitChangesRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = repositoryRoot;
  request.objectId = item->objectId;
  request.timeoutMilliseconds = kCommitChangesTimeoutMs;
  request.readSerial = ++commitFilesSerial_;
  request.bindingGeneration = tasks_.Identity().generation;
  // 请求发出时就把这条提交完整记下来：回来时按它核对，绝不用「现在恰好选中的那一条」认领。
  pendingFilesCommit_ = *item;

  ChangesPanel::CommitFilesView loading;
  loading.hasCommit = true;
  loading.commit = pendingFilesCommit_;
  loading.loading = true;
  panel_->ShowCommitFiles(loading);

  commitChangesWorker_.Request(nullptr, 0, std::move(request),
                               [](const platform::CommitChangesRequest& pending) {
                                 return platform::RunCommitChangesLoad(pending);
                               });
}

void MainFrame::OnCommitChangesCompleted(uint64_t serial) {
  platform::CommitChangesOutcome outcome;
  if (!commitChangesWorker_.FetchLatest(serial, &outcome)) {
    return;  // 期间又读了更晚的一条（用户换了提交）：这份结果不再有意义。
  }
  // 序号只保证「最后一次请求」；仓库身份与那条提交还要各核一次。
  if (outcome.bindingGeneration != tasks_.Identity().generation ||
      outcome.objectId != pendingFilesCommit_.objectId ||
      panel_->CommitFilesCommitId() != outcome.objectId) {
    return;
  }
  ChangesPanel::CommitFilesView view;
  view.hasCommit = true;
  view.commit = pendingFilesCommit_;
  view.note = outcome.message;
  if (outcome.error != git::RepoError::none) {
    view.error = outcome.message;  // 失败不留半份清单。
  } else {
    view.changes = std::move(outcome.changes);
  }
  panel_->ShowCommitFiles(view);
  SetStatus(outcome.message);  // 只读仓库：不改操作结论。
  UpdateUi();
}

void MainFrame::RequestCommitDetail(int row) {
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  if (!state_.GitUsable() || !state_.RepoUsable() || repositoryRoot.empty() ||
      !git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot) ||
      state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    return;
  }
  if (panel_->HistoryRowCount() !=
      static_cast<int>(state_.WorkspaceModel().recentCommits.size())) {
    return;
  }
  const git::CommitItem* item = panel_->CommitItemAt(row);
  if (item == nullptr) {
    return;
  }
  platform::CommitDetailRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = repositoryRoot;
  request.objectId = item->objectId;
  request.timeoutMilliseconds = kCommitDetailTimeoutMs;
  request.readSerial = ++commitDetailSerial_;
  request.bindingGeneration = tasks_.Identity().generation;
  pendingDetailCommit_ = *item;

  ChangesPanel::CommitDetailView loading;
  loading.hasCommit = true;
  loading.commit = pendingDetailCommit_;
  loading.loading = true;
  panel_->ShowCommitDetail(loading);

  commitDetailWorker_.Request(nullptr, 0, std::move(request),
                              [](const platform::CommitDetailRequest& pending) {
                                return platform::RunCommitDetailLoad(pending);
                              });
}

void MainFrame::OnCommitDetailCompleted(uint64_t serial) {
  platform::CommitDetailOutcome outcome;
  if (!commitDetailWorker_.FetchLatest(serial, &outcome)) {
    return;
  }
  if (outcome.bindingGeneration != tasks_.Identity().generation ||
      outcome.objectId != pendingDetailCommit_.objectId ||
      panel_->CommitDetailCommitId() != outcome.objectId) {
    return;
  }
  ChangesPanel::CommitDetailView view;
  view.hasCommit = true;
  view.commit = pendingDetailCommit_;
  if (outcome.error != git::RepoError::none) {
    view.error = outcome.message;  // 失败不留上一条的文本冒充现状。
  } else {
    view.text = outcome.text;
  }
  panel_->ShowCommitDetail(view);
  SetStatus(outcome.message);
  UpdateUi();
}

void MainFrame::ReconcileSelectedCommit() {
  // 「当前选中的那一条」是唯一判据：列表刚刷新、选项卡刚切回、用户点了另一行，
  // 三种来源都只问这一个问题，不会出现「列表已换而清单还是上一条」的窗口期。
  const int row = panel_->SelectedHistoryRow();
  const git::CommitItem* item = row >= 0 ? panel_->CommitItemAt(row) : nullptr;
  if (item == nullptr) {
    panel_->ShowCommitFiles(ChangesPanel::CommitFilesView{});
    panel_->ShowCommitDetail(ChangesPanel::CommitDetailView{});
    return;
  }
  if (!(panel_->CommitFilesLoading() && panel_->CommitFilesCommitId() == item->objectId)) {
    RequestCommitChanges(row);
  }
  if (panel_->CommitDetailCommitId() != item->objectId) {
    RequestCommitDetail(row);
  }
}

void MainFrame::OnCommitProbeCompleted(uint64_t serial) {
  commitFlow_.OnProbeCompleted(*this, CaptureOperationContext(), state_.WorkspaceModel(), serial);
  UpdateUi();
}

void MainFrame::OnUndoProbeCompleted(uint64_t serial) {
  undoFlow_.OnProbeCompleted(*this, CaptureOperationContext(), serial);
  UpdateUi();
}

void MainFrame::OnUndoRecheckCompleted(uint64_t serial) {
  undoFlow_.OnRecheckCompleted(*this, CaptureOperationContext(), serial);
  UpdateUi();
}

void MainFrame::OnRestoreProbeCompleted(uint64_t serial) {
  restoreFlow_.OnProbeCompleted(*this, CaptureOperationContext(), serial);
  UpdateUi();
}

void MainFrame::OnRestoreRecheckCompleted(uint64_t serial) {
  restoreFlow_.OnRecheckCompleted(*this, CaptureOperationContext(), serial);
  UpdateUi();
}

void MainFrame::ApplyAuthorDefaultIfAllowed() {
  if (!formSession_.AcceptsAuthorDefault()) {
    return;
  }
  const std::wstring identity = state_.Author().config.Identity();
  if (identity.empty()) {
    return;
  }
  if (formSession_.AuthorDefaultApplied() && formSession_.AppliedAuthorDefault() == identity &&
      form_->AuthorText() == identity) {
    return;
  }
  form_->SetAuthorProgrammatically(identity);
  formSession_.NoteAuthorDefaultApplied(identity);
}

void MainFrame::HandleAuthorCleared() {
  formSession_.NoteAuthorCleared();
  ApplyAuthorDefaultIfAllowed();
  if (form_->AuthorText().empty()) {
    SetFormNote(L"作者栏已清空；这个仓库的有效 Git 配置里没有完整身份，请手动填写“姓名 <邮箱>”。"
                L"本程序不会改写任何 Git 配置。");
  } else {
    SetFormNote(L"作者栏已回到这个仓库有效 Git 配置里的默认身份；手动修改只作用于本次提交。");
  }
  RunFormValidation({});
}

void MainFrame::ScheduleRefreshSoon() {
  if (closing_) {
    return;
  }
  refreshTimer_.StartOnce(static_cast<int>(kRefreshDebounceMs));
}

void MainFrame::OnRefreshTimer(wxTimerEvent&) {
  if (closing_) {
    return;
  }
  if (refreshCycleActive_) {
    refreshPending_ = true;
    return;
  }
  if (!state_.GitUsable()) {
    SetStatus(L"Git 程序还不能用，没法刷新。");
    return;
  }
  refreshCycleActive_ = true;
  RequestRepoDetection(false);
}

void MainFrame::OnOperationTimer(wxTimerEvent&) {
  if (activeRunnerId_ == 0) {
    operationTimer_.Stop();
    return;
  }
  platform::CommandWindowResult result;
  if (commandWindows_->TakeResult(activeRunnerId_, &result)) {
    FinishActiveOperation(result);
    return;
  }
  std::wstring status;
  platform::CommandWindowResult snapshot;
  if (!commandWindows_->DescribeOperation(activeRunnerId_, &status, &snapshot)) {
    // 执行器已经不认识这次操作：完成通知/结果都不可能再回来。按「结果未知」释放槽位，
    // 绝不自动重发，也绝不把未知说成成功或失败。
    const bool restoreStepInProgress = activeRestoreOperation_;
    const std::wstring inFlightHistoryId =
        activeHistoryRecording_ ? activeHistoryRecordId_ : std::wstring();
    const app::OperationOutcome outcome =
        tasks_.ForgetOperation(activeOperationSerial_, L"执行器已不再认识这次操作（结果未知）");
    platform::RemoveNulPathspecFile(activePathspecFile_);
    platform::RemoveCommitMessageFile(activeMessageFile_);
    if (activeCommitOperation_) {
      commitFlow_.OnCommandSettled(*this, false, activeCommittedForm_);
    }
    if (restoreStepInProgress) {
      // 恢复的那条 update-ref 没拿到终态就是「结果未知」：控制器放开流程状态，
      // 既不说成功也不说失败，更不自动重发。
      restoreFlow_.Forget();
    }
    SetStatus(outcome.note);
    // 同一条在途记录补成「结果未知」：不是成功、不是失败，也不是「已撤销」。
    // 排在结论写进状态之后，补记失败那句会附在它后面并列。
    MarkHistoryRecordUnknown(inFlightHistoryId, outcome.note);
    activeOperationSerial_ = 0;
    activeRunnerId_ = 0;
    activePathspecFile_.clear();
    activeMessageFile_.clear();
    activeCommitOperation_ = false;
    activeCommittedForm_ = {};
    activeViewKind_.reset();
    activeUndoOperation_ = false;
    activeRestoreHint_.clear();
    activeRestoreOperation_ = false;
    activeHistoryCapture_ = {};
    activeHistoryStartedEpoch_ = 0;
    activeHistoryRecordId_.clear();
    activeHistoryRecording_ = false;
    operationTimer_.Stop();
    UpdateUi();
    ScheduleRefreshSoon();
    return;
  }
  if (!status.empty()) {
    SetStatus(status);
  }
}

void MainFrame::FinishActiveOperation(const platform::CommandWindowResult& result) {
  const app::OperationOutcome outcome = tasks_.FinishOperation(
      activeOperationSerial_, result.completion, result.exitCode, result.failureReason);
  const bool succeeded = outcome.succeeded;

  // 文件所有权随操作一起；只有拿到终态（或执行器明确不再认识它）才回收。
  platform::RemoveNulPathspecFile(activePathspecFile_);
  platform::RemoveCommitMessageFile(activeMessageFile_);

  const bool commitOperation = activeCommitOperation_;
  const git::CommitFormData committedForm = activeCommittedForm_;
  const std::optional<git::DiffViewKind> viewKind = activeViewKind_;
  const bool undoOperation = activeUndoOperation_;
  const std::wstring restoreHint = activeRestoreHint_;
  const bool restoreOperation = activeRestoreOperation_;
  const app::HistoryCapture historyCapture = activeHistoryCapture_;
  const long long historyStartedEpoch = activeHistoryStartedEpoch_;
  const std::wstring historyRecordId = activeHistoryRecordId_;
  const bool historyRecording = activeHistoryRecording_;

  activeOperationSerial_ = 0;
  activeRunnerId_ = 0;
  activePathspecFile_.clear();
  activeMessageFile_.clear();
  activeCommitOperation_ = false;
  activeCommittedForm_ = {};
  activeViewKind_.reset();
  activeUndoOperation_ = false;
  activeRestoreHint_.clear();
  activeRestoreOperation_ = false;
  activeHistoryCapture_ = {};
  activeHistoryStartedEpoch_ = 0;
  activeHistoryRecordId_.clear();
  activeHistoryRecording_ = false;
  operationTimer_.Stop();

  if (commitOperation) {
    commitFlow_.OnCommandSettled(*this, succeeded, committedForm);
  }

  std::wstring note = outcome.note;
  if (viewKind.has_value() && result.completion == git::CommandCompletion::finished) {
    // 查看类操作的退出码语义在这里补完整：`git diff` 有差异时也是 0，
    // `git diff --no-index` 则用 1 表示「有差异」。不解释就会被读成「操作失败」。
    note += git::DescribeDiffViewExitCode(*viewKind, result.exitCode);
  }
  // 撤回成功才追加恢复线索（原提交完整 ID + 找回方式）：失败/结果未知时不说「撤回了」。
  if (undoOperation && succeeded && !restoreHint.empty()) {
    note += L"｜" + restoreHint;
  }
  if (!result.environmentNotice.empty()) {
    note += L"｜" + result.environmentNotice;
  }

  // 历史落账排在「记住结论」之前：被拒的那句要并进结论本身，再交给紧随其后的刷新显示。
  // 启动与终态用同一个操作 ID；unknown / launchFailed / gitNotStarted 各有自己的终态档，
  // 绝不因为命令窗口曾经打开就把这一条写成成功。
  if (historyRecording) {
    app::HistoryTerminalInfo terminal;
    terminal.outcome = app::MapCommandCompletionToHistoryOutcome(result.completion, succeeded);
    terminal.startedEpoch = historyStartedEpoch;
    const platform::LocalInstant now = platform::CurrentLocalInstant();
    terminal.terminalEpoch = now.valid ? now.utcEpochSeconds : 0;
    terminal.exitCodeKnown = result.completion == git::CommandCompletion::finished;
    terminal.exitCode = result.exitCode;
    terminal.completionLabel = std::wstring(git::CommandCompletionLabel(result.completion));
    terminal.conclusion = note;
    std::wstring refusal;
    RecordHistory(historyCapture, terminal, historyRecordId, &refusal);
    if (!refusal.empty()) {
      note += L"｜" + refusal;
    }
  }

  if (restoreOperation && restoreFlow_.Active()) {
    // 按记录恢复的终态交回控制器结案：它先说清「只移动了那一个本地引用」或「没有成功」，
    // 再请求刷新。这里不替它把未知判定成成功，也不自动重试或回退。
    tasks_.RememberOperationConclusion(note);
    restoreFlow_.OnCommandSettled(*this, succeeded, note);
    UpdateUi();
    ScheduleRefreshSoon();
    return;
  }

  SetStatus(note);
  tasks_.RememberOperationConclusion(note);
  UpdateUi();
  ScheduleRefreshSoon();
}

OperationContext MainFrame::CaptureOperationContext() const {
  OperationContext ctx;
  ctx.notify = nullptr;
  ctx.gitExecutable = state_.Git().path;
  ctx.detection = state_.Repo().detection;
  ctx.repoUsable = state_.RepoUsable();
  ctx.repositoryIdentity = tasks_.Identity();
  ctx.committerState = state_.Author().config.CommitterState();
  ctx.workspaceStatus = state_.Workspace().status;
  ctx.workspaceMessage = state_.Workspace().message;
  ctx.stagedItemCount = state_.WorkspaceModel().staged.size();
  return ctx;
}

// ============================================================================
// 操作历史（内存记录 + 平台存储契约）
// ============================================================================

void MainFrame::InitializeOperationHistory() {
  historyStorage_ = platform::DescribeHistoryStorageAvailability();
  historyPaths_ = platform::ResolveHistoryStorePaths();
  historyWritable_ = historyStorage_.usable && historyPaths_.valid;
  historyLoadNote_.clear();

  if (historyStorage_.crossSessionPersistence && historyWritable_) {
    const platform::HistoryReadResult read =
        platform::ReadHistoryFile(historyPaths_.historyFile);
    switch (read.kind) {
      case platform::HistoryReadKind::absent:
        break;  // 首次使用：开关默认关闭。
      case platform::HistoryReadKind::unreadable:
        historyWritable_ = false;  // 读不成不等于没有：这次不使用也不覆盖。
        historyLoadNote_ = read.detail + L"本次运行不使用也不会覆盖这份历史文件。";
        break;
      case platform::HistoryReadKind::oversized:
      case platform::HistoryReadKind::invalidUtf8:
        historyLoadNote_ = read.detail + L"下一次保存时会把原件改名保留在旁边，再重新开始记录。";
        break;
      case platform::HistoryReadKind::loaded: {
        const app::HistoryLoadResult parsed = app::ParseOperationLog(read.text);
        switch (parsed.status) {
          case app::HistoryLoadStatus::empty:
            break;
          case app::HistoryLoadStatus::corrupt:
            historyLoadNote_ = L"历史文件读不成立（" + parsed.reason +
                               L"）。下一次保存时会把原件改名保留，再从头开始。";
            break;
          case app::HistoryLoadStatus::tooNew:
            historyWritable_ = false;
            historyLoadNote_ = L"盘上的历史文件由更新版本的程序写出（版本 " +
                               std::to_wstring(parsed.detectedVersion) +
                               L"），本次不读取、不覆盖。";
            break;
          case app::HistoryLoadStatus::loaded:
            historyLog_ = parsed.log;
            break;
        }
        break;
      }
    }
  } else if (!historyStorage_.reason.empty()) {
    historyLoadNote_ = historyStorage_.reason;
  }

  if (historyRecordCheck_ != nullptr) {
    historyRecordCheck_->SetValue(historyLog_.historyEnabled);
    if (!historyWritable_) {
      // 限制要一眼看得见，而不是只藏在工具提示里：不落盘时把这句话写进控件标题。
      historyRecordCheck_->SetLabel(FromWide(L"记录操作历史（仅本次运行，不落盘）"));
    }
  }
}

bool MainFrame::HistoryRecordingEnabled() const { return historyLog_.historyEnabled; }

std::wstring MainFrame::HistoryStorageLine() const {
  if (historyStorage_.crossSessionPersistence) {
    if (historyWritable_ && historyPaths_.valid) {
      return app::DescribeHistoryStoragePath(historyPaths_.directory,
                                             platform::kHistoryFileName);
    }
    if (!historyStorage_.reason.empty()) {
      return L"不可写：" + historyStorage_.reason;
    }
    if (!historyPaths_.failureReason.empty()) {
      return L"不可写：" + historyPaths_.failureReason;
    }
    if (!historyLoadNote_.empty()) {
      return L"不可写：" + historyLoadNote_;
    }
    return L"不可写：这次没有可用的历史位置。";
  }
  return L"仅本次运行的内存（不落盘）：" +
         (historyStorage_.reason.empty()
              ? std::wstring(L"本平台的操作历史持久化尚未接入，关闭界面即丢失。")
              : historyStorage_.reason);
}

std::wstring MainFrame::NextHistoryId() {
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  ++historyRecordCounter_;
  return L"op-" + std::to_wstring(epoch < 0 ? 0 : epoch) + L"-" +
         std::to_wstring(static_cast<unsigned long>(wxGetProcessId())) + L"-" +
         std::to_wstring(historyRecordCounter_);
}

void MainFrame::ScheduleHistorySave() {
  if (!historyWritable_) {
    return;
  }
  historyIntents_.append = true;
  historySavePending_ = true;
  historySaveTimer_.StartOnce(static_cast<int>(kHistorySaveDebounceMs));
}

void MainFrame::OnHistorySaveTimer(wxTimerEvent&) {
  if (!closing_) {
    RunHistorySave();
  }
}

void MainFrame::RunHistorySave() {
  historySavePending_ = false;
  historySaveTimer_.Stop();
  if (!historyWritable_) {
    return;
  }
  app::HistoryWriteIntents intents = historyIntents_;
  historyIntents_ = app::HistoryWriteIntents{};  // 先清；写不成再原样放回，脏意不丢
  if (!(intents.policy || intents.append || intents.replaceAll)) {
    return;
  }
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  const platform::HistorySaveOutcome outcome =
      platform::SaveOperationHistory(historyPaths_, historyLog_, intents, epoch);
  const auto requeue = [&]() {
    historyIntents_.append = historyIntents_.append || intents.append;
    historyIntents_.policy = historyIntents_.policy || intents.policy;
    historyIntents_.replaceAll = historyIntents_.replaceAll || intents.replaceAll;
  };
  switch (outcome.status) {
    case platform::HistorySaveStatus::saved:
    case platform::HistorySaveStatus::recoveredCorrupt:
      historyLog_ = outcome.merged;  // 盘上现在就是这一份：下一次合并以它为基准
      if (!outcome.report.keptFromDiskStrongerTerminal.empty()) {
        SetFormNote(L"另一个窗口已记下某些操作更确定的结果（" +
                    std::to_wstring(outcome.report.keptFromDiskStrongerTerminal.size()) +
                    L" 条）；本次以先记录到终态的那份为准，没有把较弱的信息盖上去。");
      }
      if (!outcome.detail.empty()) {
        SetStatus(state_.StatusNote() + L"｜" + outcome.detail);
      }
      return;
    case platform::HistorySaveStatus::busy:
      requeue();
      ScheduleHistorySave();
      return;
    case platform::HistorySaveStatus::refusedTooNew:
      historyWritable_ = false;
      SetStatus(state_.StatusNote() + L"｜" + outcome.detail);
      return;
    case platform::HistorySaveStatus::failed:
      // 写不进去不等于 Git 那一步没做成：两句分开说，脏意留回队列稍后再试一次。
      requeue();
      ScheduleHistorySave();
      SetStatus(state_.StatusNote() + L"｜操作历史这次没写成功（" + outcome.detail +
                L"）；Git 那一步的结果不受影响。");
      return;
  }
}

std::wstring MainFrame::RecordHistory(const app::HistoryCapture& capture,
                                      const app::HistoryTerminalInfo& terminal,
                                      const std::wstring& existingId, std::wstring* refusal) {
  if (!capture.record) {
    return {};  // 这次操作本来就不落历史。
  }
  const std::wstring id = existingId.empty() ? NextHistoryId() : existingId;
  app::OperationRecord record =
      app::ComposeHistoryRecord(id, capture.workTreeRoot, capture, terminal);
  std::wstring reason;
  if (!app::AppendHistoryRecord(&historyLog_, std::move(record), &reason)) {
    if (refusal != nullptr) {
      *refusal = L"操作历史没有记录：" + reason;
    }
    return {};
  }
  ScheduleHistorySave();
  return id;
}

std::wstring MainFrame::BeginInProgressHistory(const app::HistoryCapture& capture,
                                                long long startedEpoch,
                                                std::wstring* refusal) {
  app::HistoryTerminalInfo terminal;
  terminal.outcome = app::HistoryOutcome::inProgress;
  terminal.startedEpoch = startedEpoch;
  terminal.terminalEpoch = 0;  // 还没有终态时刻：这一条只说明「发出去了」
  terminal.completionLabel = L"执行中";
  terminal.conclusion = L"命令窗口已经启动，本程序还没有拿到它的结果。";
  const std::wstring id = RecordHistory(capture, terminal, std::wstring(), refusal);
  if (id.empty()) {
    return {};
  }
  if (historyWritable_) {
    // 当场写盘，不等防抖：这条记录的意义正是「进程此刻没了也还查得到」。
    historySavePending_ = false;
    historySaveTimer_.Stop();
    RunHistorySave();
  }
  return id;
}

void MainFrame::MarkHistoryRecordUnknown(const std::wstring& recordId,
                                         std::wstring_view why) {
  if (recordId.empty() || !HistoryRecordingEnabled()) {
    return;
  }
  bool held = false;
  for (const app::OperationRecord& existing : historyLog_.records) {
    if (existing.id != recordId) {
      continue;
    }
    held = true;
    if (app::TerminalHasSettled(existing.terminal)) {
      return;  // 已有更确定的结论：不降级、不改写。
    }
    break;
  }
  // 这条在途记录已经不在了（被保留策略裁掉，或当场就没落下来）：不拿一次补记新建孤儿行。
  if (!held) {
    return;
  }
  app::OperationRecord unknown;
  unknown.id = recordId;
  unknown.terminal = app::HistoryTerminal::unknown;
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  unknown.terminalEpoch = now.valid ? now.utcEpochSeconds : 0;
  unknown.completionLabel = L"没有收到完成通知";
  unknown.outcomeNote = std::wstring(why);
  std::wstring reason;
  if (!app::AppendHistoryRecord(&historyLog_, std::move(unknown), &reason)) {
    SetStatus(state_.StatusNote() + L"｜另外：这条在途操作的历史没能补成「结果未知」（" + reason +
              L"）；Git 那一步的结果不受影响。");
    return;
  }
  ScheduleHistorySave();
}

void MainFrame::SettleUnfinishedHistoryOnShutdown() {
  if (!activeHistoryRecording_ || activeHistoryRecordId_.empty()) {
    return;
  }
  MarkHistoryRecordUnknown(
      activeHistoryRecordId_,
      L"界面在关闭前没有收到「" +
          (activeHistoryCapture_.operationLabel.empty()
               ? std::wstring(L"这次操作")
               : activeHistoryCapture_.operationLabel) +
          L"」的完成通知。按你的选择，命令窗口里的 Git 没有被中断、也没有被重放或撤销，"
          L"本程序不再读取它的结果：这条操作的结果未知，请以仓库现状核实。");
}

void MainFrame::ShowOperationHistory() {
  HistoryDialog dialog(this, historyLog_.records, HistoryStorageLine(),
                       state_.Repo().detection.root);
  if (dialog.ShowModal() != wxID_OK) {
    return;
  }
  const std::wstring id = dialog.selectedRecordId();
  const auto found = std::find_if(historyLog_.records.begin(), historyLog_.records.end(),
                                  [&](const app::OperationRecord& record) {
                                    return record.id == id;
                                  });
  if (found == historyLog_.records.end()) {
    SetStatus(L"选中的那条记录已经不在当前历史里（可能刚被保留策略裁掉），本次没有执行任何操作。");
    UpdateUi();
    return;
  }
  if (dialog.action() == HistoryDialogAction::restore) {
    BeginRestoreFromRecord(*found);
  } else if (dialog.action() == HistoryDialogAction::copyRestore) {
    CopyHistoryRestoreText(*found);
  }
}

void MainFrame::BeginRestoreFromRecord(const app::OperationRecord& record) {
  if (!AdmitFlow(app::GitFlow::restore, L"按记录恢复")) {
    return;
  }
  const OperationContext ctx = CaptureOperationContext();
  if (!ctx.repoUsable) {
    SetStatus(L"当前没有可用的工作区仓库，无法按记录恢复；请先识别一个可用的仓库。");
    UpdateUi();
    return;
  }
  // 记录属于另一个仓库时绝不恢复：同一份判定也供 Win32 过渡界面使用，
  // 内容包括 refMove 档位、仓库规范键、引用名与对象 ID 形状。
  const app::HistoryRestoreOffer offer =
      app::DescribeHistoryRestoreOffer(record, ctx.detection.root);
  if (!offer.offered) {
    ShowInfo(L"无法按记录恢复", L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + offer.refusal);
    SetStatus(L"未执行恢复：" + offer.refusal);
    UpdateUi();
    return;
  }
  restoreFlow_.Start(*this, ctx, offer.clues);
  UpdateUi();
}

void MainFrame::CopyHistoryRestoreText(const app::OperationRecord& record) {
  std::wstring payload;
  std::wstring note;
  if (record.restoreKind == app::HistoryRestoreKind::refMove) {
    // 复制不要求当前仓库就是记录所属仓库：线索本身足够复现命令。但线索的形状检查
    // 与恢复链路共用同一份（以记录自己的工作区根做归属比较）。
    const app::HistoryRestoreOffer offer =
        app::DescribeHistoryRestoreOffer(record, record.workTreeRoot);
    if (!offer.offered) {
      SetStatus(L"这条记录的恢复线索不成立，没有可复制的命令：" + offer.refusal);
      UpdateUi();
      return;
    }
    const git::RestoreCopyText copy =
        git::DescribeRestoreCopy(offer.clues, platform::DefaultShellDialect());
    if (!copy.cluesUsable) {
      SetStatus(L"这条记录的恢复线索不成立，没有可复制的命令：" + copy.refusal);
      UpdateUi();
      return;
    }
    if (!copy.copyableCommand.empty()) {
      payload = copy.copyableCommand;
      note = L"已复制" + copy.dialectLabel +
             L"的一行恢复命令（未执行，也不代表已恢复远端）。" + copy.copyNote +
             L"粘贴前请核对目标与风险；换到别的 shell 就要重新核对这一行的写法。";
    } else {
      payload = copy.structuredFacts;
      note = L"这一串参数不能安全地粘成一行（" + copy.copyRefusal +
             L"），所以复制的是逐段列出的字段清单，不是命令。本程序没有替换任何字符；"
             L"要执行请在终端里自己按段输入。";
    }
  } else {
    payload = record.restoreNote;
    if (payload.empty()) {
      SetStatus(L"这条记录没有可复制的恢复说明。");
      UpdateUi();
      return;
    }
    note = L"已复制这条记录的恢复说明（一段说明文字，不是可直接执行的命令，也没有执行任何东西）。";
    if (record.restoreKind == app::HistoryRestoreKind::manualRemote) {
      note += L"涉及远端历史的回退要你自己判断协作影响；本程序不提供 force push 这类命令。";
    }
  }

  bool copied = false;
  if (wxTheClipboard->Open()) {
    copied = wxTheClipboard->SetData(new wxTextDataObject(FromWide(payload)));
    wxTheClipboard->Close();
  }
  if (!copied) {
    SetStatus(L"复制到剪贴板失败。恢复线索仍可在操作历史窗口里选中阅读。");
    UpdateUi();
    return;
  }
  SetStatus(note);
  UpdateUi();
}

void MainFrame::ClearWorkspaceState() {
  state_.SetWorkspace(git::WorkspaceSnapshot{});
  RebuildChangesLists();
}

void MainFrame::RebuildChangesLists() {
  panel_->ShowWorkspace(state_.WorkspaceModel(), state_.WorkspaceHintTexts());
}

void MainFrame::UpdateUi() {
  if (state_.GitUsable()) {
    SetLabelIfChanged(gitLabel_, FromWide(L"Git：" + state_.Git().path + L"（" +
                                          (state_.Git().version.empty() ? L"已验证"
                                                                         : state_.Git().version) +
                                          L"）"));
  } else {
    const std::wstring message = state_.Git().message.empty() ? L"正在发现 Git 程序…"
                                                              : state_.Git().message;
    SetLabelIfChanged(gitLabel_, FromWide(L"Git：" + message));
  }

  SetRepositorySummary();

  const git::EmptyStateTexts texts = state_.WorkspaceHintTexts();
  std::wstring unstagedNote;
  if (panel_->UnstagedRowCount() == 0) {
    unstagedNote = texts.unstaged;
  } else {
    unstagedNote = L"共 " + std::to_wstring(panel_->UnstagedRowCount()) +
                   L" 项；只处理选中的条目；双击可看差异";
  }
  const std::wstring submodule = git::SubmoduleExplanationText(state_.WorkspaceModel());
  if (!submodule.empty()) {
    unstagedNote += L" " + submodule;
  }
  SetLabelIfChanged(panel_->unstagedHint(), FromWide(unstagedNote));
  if (panel_->StagedRowCount() == 0) {
    SetLabelIfChanged(panel_->stagedHint(), FromWide(texts.staged));
  } else {
    SetLabelIfChanged(panel_->stagedHint(), FromWide(L"共 " +
                                                     std::to_wstring(panel_->StagedRowCount()) +
                                                     L" 项；提交只来自索引里这一份；双击可看差异"));
  }

  const bool busy = tasks_.OperationInFlight();
  const bool repositoryReady = state_.GitUsable() && state_.RepoUsable();
  const bool workspaceReady =
      repositoryReady && state_.Workspace().status == git::WorkspaceLoadStatus::loaded;
  const bool flowBusy = commitFlow_.Active() || undoFlow_.Active() || restoreFlow_.Active();
  refreshButton_->Enable(state_.GitUsable());
  browseRepo_->Enable(!busy);
  statusButton_->Enable(repositoryReady && !busy);
  const bool stagingReady = workspaceReady && !busy;
  panel_->stageSelectedButton()->Enable(stagingReady);
  panel_->stageAllButton()->Enable(stagingReady);
  panel_->unstageSelectedButton()->Enable(stagingReady);
  panel_->unstageAllButton()->Enable(stagingReady);
  createCommit_->Enable(workspaceReady && !busy && !flowBusy);
  undoCommitButton_->Enable(workspaceReady && !busy && !flowBusy);
  historyBrowseButton_->Enable(true);
  historyRecordCheck_->Enable(true);

  SetLabelIfChanged(statusText_, FromWide(state_.StatusNote()));
  SetLabelIfChanged(formNoteText_, FromWide(state_.FormNote()));
  const int width = GetClientSize().GetWidth() > 200 ? GetClientSize().GetWidth() - 40 : 900;
  statusText_->Wrap(width);
  formNoteText_->Wrap(width);
  repoSummary_->Wrap(width);
  panel_->unstagedHint()->Wrap(width / 2 - 20 > 100 ? width / 2 - 20 : 300);
  panel_->stagedHint()->Wrap(width / 2 - 20 > 100 ? width / 2 - 20 : 300);
  Layout();
}

void MainFrame::SetRepositorySummary() {
  const git::RepoDetection& detection = state_.Repo().detection;
  std::wstring summary;
  switch (state_.Repo().status) {
    case app::RepoLoadStatus::unloaded:
      summary = L"尚未识别仓库。";
      break;
    case app::RepoLoadStatus::detecting:
      summary = L"正在后台识别仓库（只读查询，不弹命令窗口）…";
      break;
    case app::RepoLoadStatus::failed:
      summary = L"仓库识别失败：" + detection.message;
      break;
    case app::RepoLoadStatus::loaded:
      summary = detection.message;
      if (git::KindHasWorkspace(detection.kind)) {
        summary += L"\n分支：" + state_.BranchDisplay() + L"；上游：" + state_.UpstreamDisplay();
      }
      break;
  }
  SetLabelIfChanged(repoSummary_, FromWide(summary));
}

bool MainFrame::RequireStagingPrerequisites(std::wstring_view actionLabel) {
  app::WritePrerequisites prerequisites;
  prerequisites.gitUsable = state_.GitUsable();
  prerequisites.repoUsable = state_.RepoUsable();
  prerequisites.commandWindowBusy = tasks_.OperationInFlight();
  prerequisites.workspaceStillBound =
      !state_.Repo().detection.root.empty() &&
      git::PathsEqualFolded(state_.Repo().detection.root, tasks_.Identity().workTreeRoot);
  const std::wstring refusal = app::DescribeWritePrerequisiteRefusal(prerequisites, actionLabel);
  if (!refusal.empty()) {
    SetStatus(refusal);
    UpdateUi();
    return false;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    SetStatus(L"还没读到这个仓库的工作区状态，无法确定暂存范围。请先点“刷新”。");
    UpdateUi();
    return false;
  }
  return true;
}

void MainFrame::StageSelected() {
  if (!RequireStagingPrerequisites(L"暂存")) {
    return;
  }
  const std::vector<int> rows = panel_->SelectedUnstagedRows();
  if (rows.empty()) {
    SetStatus(L"先在“未暂存的更改”里选中文件，再点“加入暂存区”；没有选中时不会暂存全部。");
    UpdateUi();
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  if (panel_->UnstagedRowCount() != static_cast<int>(model.unstaged.size())) {
    SetStatus(L"列表行数与已读取的仓库状态对不上，为避免暂存错文件，本次没有执行任何命令。点“刷新”后重试。");
    UpdateUi();
    return;
  }
  std::vector<git::ChangeItem> selection;
  selection.reserve(rows.size());
  for (const int row : rows) {
    if (row < 0 || static_cast<size_t>(row) >= model.unstaged.size()) {
      SetStatus(L"选中的行已经不在当前列表里，请重新选择后再点一次。");
      UpdateUi();
      return;
    }
    selection.push_back(model.unstaged[static_cast<size_t>(row)]);
  }

  git::StagingPlanOptions options;
  options.pathspecFileSupported = git::SupportsPathspecFileDelivery(state_.Git().version);
  options.totalUnstagedItems = model.unstaged.size();
  const git::StagingPlan plan = git::BuildStagingAddPlan(selection, options);
  RunStagingPlan(plan, L"暂存", L"暂存", L"gc-add", L"无法加入暂存区", L"加入暂存区前请确认");
}

void MainFrame::UnstageSelected() {
  if (!RequireStagingPrerequisites(L"取消暂存")) {
    return;
  }
  const std::vector<int> rows = panel_->SelectedStagedRows();
  if (rows.empty()) {
    SetStatus(L"先在“已暂存的更改”里选中文件，再点“移出暂存区”；没有选中时不会取消全部。");
    UpdateUi();
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  if (panel_->StagedRowCount() != static_cast<int>(model.staged.size())) {
    SetStatus(L"列表行数与已读取的仓库状态对不上，为避免取消错文件，本次没有执行任何命令。点“刷新”后重试。");
    UpdateUi();
    return;
  }
  std::vector<git::ChangeItem> selection;
  selection.reserve(rows.size());
  for (const int row : rows) {
    if (row < 0 || static_cast<size_t>(row) >= model.staged.size()) {
      SetStatus(L"选中的行已经不在当前列表里，请重新选择后再点一次。");
      UpdateUi();
      return;
    }
    selection.push_back(model.staged[static_cast<size_t>(row)]);
  }

  git::StagingPlanOptions options;
  options.pathspecFileSupported = git::SupportsPathspecFileDelivery(state_.Git().version);
  options.totalStagedItems = model.staged.size();
  options.repositoryHasHead = state_.Repo().detection.headResolved;
  options.restoreCommandSupported = git::SupportsRestoreCommand(state_.Git().version);
  const git::StagingPlan plan = git::BuildStagingUnstagePlan(selection, options);
  RunStagingPlan(plan, L"取消暂存", L"移出暂存区", L"gc-unstage", L"无法移出暂存区",
                 L"移出暂存区前请确认");
}

void MainFrame::StageAll() {
  if (!RequireStagingPrerequisites(L"全部暂存")) {
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  if (panel_->UnstagedRowCount() != static_cast<int>(model.unstaged.size())) {
    SetStatus(L"列表行数与已读取的仓库状态对不上，为避免按过期状态解释这次操作，本次没有执行任何命令。"
              L"点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const git::StagingPlan plan = git::BuildStagingAddAllPlan(model.unstaged);
  RunStagingPlan(plan, L"暂存", L"暂存", L"", L"无法全部加入暂存区", L"全部加入暂存区前请确认");
}

void MainFrame::UnstageAll() {
  if (!RequireStagingPrerequisites(L"全部取消暂存")) {
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  if (panel_->StagedRowCount() != static_cast<int>(model.staged.size())) {
    SetStatus(L"列表行数与已读取的仓库状态对不上，为避免按过期状态解释这次操作，本次没有执行任何命令。"
              L"点“刷新”后重试。");
    UpdateUi();
    return;
  }
  git::StagingPlanOptions options;
  options.repositoryHasHead = state_.Repo().detection.headResolved;
  options.restoreCommandSupported = git::SupportsRestoreCommand(state_.Git().version);
  for (const git::ChangeItem& item : model.unstaged) {
    if (item.kind == git::ChangeKind::conflicted) {
      options.unmergedPaths.push_back(item.path);
    }
  }
  const git::StagingPlan plan = git::BuildStagingUnstageAllPlan(model.staged, options);
  RunStagingPlan(plan, L"取消暂存", L"移出暂存区", L"", L"无法全部移出暂存区",
                 L"全部移出暂存区前请确认");
}

void MainFrame::RunStagingPlan(const git::StagingPlan& plan, std::wstring_view objectLabel,
                               std::wstring_view rangeVerb, std::wstring_view pathspecPrefix,
                               std::wstring_view blockedTitle, std::wstring_view confirmTitle) {
  if (plan.delivery == git::StagingDelivery::blocked) {
    ShowInfo(std::wstring(blockedTitle),
             L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason);
    SetStatus(L"未执行 " + plan.commandLabel + L"：" + plan.blockedReason);
    return;
  }
  if (!plan.confirmationText.empty()) {
    const std::wstring body = L"这次" + std::wstring(objectLabel) +
                              L"涉及需要特别说明的条目：\n\n" + plan.confirmationText;
    if (!Confirm(std::wstring(confirmTitle), body, true)) {
      SetStatus(L"已取消：没有打开命令窗口，也没有执行任何 Git 命令。");
      return;
    }
  }

  std::vector<std::wstring> arguments = plan.arguments;
  std::wstring pathspecFile;
  if (plan.delivery == git::StagingDelivery::pathspecFile) {
    const platform::PathspecFileWrite written =
        platform::WriteNulPathspecFile(plan.pathspecEntries, pathspecPrefix);
    if (!written.written) {
      SetStatus(L"没有打开命令窗口，也没有对仓库做任何改动。写路径清单失败：" + written.failureReason);
      return;
    }
    pathspecFile = written.path;
    if (!git::AppendPathspecFileOptions(&arguments, pathspecFile)) {
      platform::RemoveNulPathspecFile(pathspecFile);
      SetStatus(L"没有打开命令窗口：清单文件的路径无法安全交给命令窗口（含引号或控制字符）。");
      return;
    }
  }

  git::CommandWindowOperation operation =
      git::MakeCommandWindowOperation(plan.operationId, plan.displayName, state_.Git().path,
                                      state_.Repo().detection.root, std::move(arguments));
  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（" +
                        state_.Repo().detection.root + L"），本次" + std::wstring(rangeVerb) + L" " +
                        std::to_wstring(plan.selectedItems) + L" 项，等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pathspecFile = pathspecFile;
  if (!LaunchCommandWindow(operation, options)) {
    SetStatus(L"这次" + std::wstring(rangeVerb) +
              L"没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    return;
  }
  SetStatus(options.startedNote);
}

void MainFrame::CreateCommit() {
  app::GitFlowActivity activity;
  activity.commit = commitFlow_.Active();
  app::WritePrerequisites prerequisites;
  prerequisites.gitUsable = state_.GitUsable();
  prerequisites.repoUsable = state_.RepoUsable();
  prerequisites.commandWindowBusy = tasks_.OperationInFlight();
  prerequisites.workspaceStillBound =
      !state_.Repo().detection.root.empty() &&
      git::PathsEqualFolded(state_.Repo().detection.root, tasks_.Identity().workTreeRoot);
  const std::wstring refusal =
      app::AdmitWriteFlow(app::GitFlow::commit, activity, prerequisites, L"创建提交");
  if (!refusal.empty()) {
    SetStatus(refusal);
    UpdateUi();
    return;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    SetStatus(L"还没读到这个仓库的工作区状态，无法确定要提交什么。请先点“刷新”。");
    UpdateUi();
    return;
  }
  commitFlow_.Start(*this, CaptureOperationContext(), state_.WorkspaceModel());
  UpdateUi();
}

app::GitFlowActivity MainFrame::CurrentFlowActivity() const {
  app::GitFlowActivity activity;
  activity.commit = commitFlow_.Active();
  activity.undo = undoFlow_.Active();
  activity.restore = restoreFlow_.Active();
  // fetch / pull / push / conflict 还没有统一界面入口：它们不会在这里占路，
  // 但命令窗口单槽仍由 tasks_.OperationInFlight() 在共同前提里兜底。
  return activity;
}

bool MainFrame::AdmitFlow(app::GitFlow requested, std::wstring_view actionLabel) {
  app::WritePrerequisites prerequisites;
  prerequisites.gitUsable = state_.GitUsable();
  prerequisites.repoUsable = state_.RepoUsable();
  prerequisites.commandWindowBusy = tasks_.OperationInFlight();
  prerequisites.workspaceStillBound =
      !state_.Repo().detection.root.empty() &&
      git::PathsEqualFolded(state_.Repo().detection.root, tasks_.Identity().workTreeRoot);
  const std::wstring refusal =
      app::AdmitWriteFlow(requested, CurrentFlowActivity(), prerequisites, actionLabel);
  if (refusal.empty()) {
    return true;
  }
  SetStatus(refusal);
  UpdateUi();
  return false;
}

void MainFrame::UndoLastCommit() {
  if (!AdmitFlow(app::GitFlow::undo, L"撤回最近提交")) {
    return;
  }
  // 撤回只移动当前分支引用；预检、确认、执行前复核与命令窗口都在 UndoFlow 里，
  // 界面不在这里替它判断「能不能撤」，也不直接拼任何 Git 命令。
  undoFlow_.Start(*this, CaptureOperationContext());
  UpdateUi();
}

bool MainFrame::RequireViewPrerequisites(std::wstring_view actionLabel) {
  static_cast<void>(actionLabel);
  if (!state_.GitUsable() || !state_.RepoUsable()) {
    SetStatus(L"Git 或仓库当前不可用，无法查看。请先确认路径并点“刷新”。");
    UpdateUi();
    return false;
  }  if (tasks_.OperationInFlight()) {
    // 命令窗口操作是单槽的：并发发起两次会让「哪个窗口对应哪一行」变得含糊。
    SetStatus(L"已有一个命令窗口操作在进行，请等它结束后再双击查看。");
    UpdateUi();
    return false;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  if (repositoryRoot.empty() ||
      !git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot)) {
    // 界面显示的条目属于「已经绑定过的那个工作区」：根与协调器身份不一致时，
    // 哪怕只读命令也不能发出去，否则可能把上一个仓库的路径用到新仓库上。
    SetStatus(L"仓库工作区已改变，请先点“刷新”再查看。");
    UpdateUi();
    return false;
  }
  return true;
}

void MainFrame::OnChangesDoubleClicked(git::ChangeSide side, int row) {
  if (!RequireViewPrerequisites(L"查看差异")) {
    return;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    SetStatus(L"这一轮仓库状态没有读成功，列表不可信。请点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const git::ChangeItem* shown = panel_->ItemAt(side, row);
  if (shown == nullptr) {
    SetStatus(L"这一行已不在当前列表里（内容刚被刷新），请重新选择后再双击。");
    UpdateUi();
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  const std::vector<git::ChangeItem>& items =
      side == git::ChangeSide::staged ? model.staged : model.unstaged;
  const int rowCount =
      side == git::ChangeSide::staged ? panel_->StagedRowCount() : panel_->UnstagedRowCount();
  if (rowCount != static_cast<int>(items.size()) ||
      static_cast<size_t>(row) >= items.size()) {
    SetStatus(L"列表行数与已读取的仓库状态对不上，为避免看错文件，本次没有执行任何命令。点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const git::ChangeItem& item = items[static_cast<size_t>(row)];
  if (item.path != shown->path || item.kind != shown->kind) {
    SetStatus(L"这一行已经换成另一个文件，为避免看错文件，本次没有执行任何命令。请重新双击。");
    UpdateUi();
    return;
  }

  // 未跟踪文件没有「正常差异」，要先把档案本身问清楚（存在？可读？二进制？多大？）再决定显示什么。
  // 已跟踪条目（含删除项）不需要这一步：差异由 Git 自己给出，程序不会去打开已不存在的路径。
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  git::WorktreeFileFacts facts;
  if (item.kind == git::ChangeKind::untracked) {
    facts = platform::ProbeWorktreeFileForPreview(
        git::JoinWorktreeFilePath(repositoryRoot, item.path));
  }
  const git::DiffViewPlan plan = git::BuildDiffViewPlan(side, item, facts);
  if (plan.kind == git::DiffViewKind::blocked) {
    const std::wstring message =
        L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason + L"\n\n条目：" +
        item.PathLabel();
    ShowInfo(L"无法查看该条目", message);
    SetStatus(L"未查看 " + item.PathLabel() + L"：" + plan.blockedReason);
    UpdateUi();
    return;
  }

  git::CommandWindowOperation operation =
      git::MakeCommandWindowOperation(plan.operationId, plan.displayName, state_.Git().path,
                                      repositoryRoot, plan.arguments);
  CommandLaunchOptions options;
  options.policy = app::OperationExitPolicy::readOnlyView;
  options.viewKind = plan.kind;
  options.scopeNotice = plan.notice;
  options.startedNote = L"已在命令窗口执行 " + std::wstring(git::DiffViewKindLabel(plan.kind)) +
                        L"（" + repositoryRoot + L"）：" + item.PathLabel() + L"，等待 Git 退出码…";
  if (!LaunchCommandWindow(operation, options)) {
    SetStatus(L"这次查看没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    UpdateUi();
  }
}

void MainFrame::OnHistoryDoubleClicked(int row) {
  if (!RequireViewPrerequisites(L"查看提交")) {
    return;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    SetStatus(L"这一轮仓库状态没有读成功，提交历史不可用。请点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const std::vector<git::CommitItem>& commits = state_.WorkspaceModel().recentCommits;
  const git::CommitItem* shown = panel_->CommitItemAt(row);
  if (panel_->HistoryRowCount() != static_cast<int>(commits.size()) || shown == nullptr) {
    SetStatus(L"列表行数与已读取的提交历史对不上，为避免看错提交，本次没有执行任何命令。点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const git::CommitItem& item = commits[static_cast<size_t>(row)];
  // 两头核对：显示的那一条和模型的那一条必须是同一个对象 ID；命令只用完整 ID。
  if (shown->objectId != item.objectId) {
    SetStatus(L"这一行已经换成另一条提交，为避免看错提交，本次没有执行任何命令。请重新双击。");
    UpdateUi();
    return;
  }
  const git::CommitShowPlan plan = git::BuildCommitShowPlan(item);
  if (!plan.allowed) {
    const std::wstring message =
        L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.refusalReason +
        L"\n\n提交：" + git::ShortObjectId(item.objectId) + L" " + item.summary;
    ShowInfo(L"无法查看该提交", message);
    SetStatus(L"未查看提交 " + git::ShortObjectId(item.objectId) + L"：对象 ID 不符合约定。");
    UpdateUi();
    return;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  git::CommandWindowOperation operation =
      git::MakeCommandWindowOperation(plan.operationId, plan.displayName, state_.Git().path,
                                      repositoryRoot, plan.arguments);
  CommandLaunchOptions options;
  // 退出码按常规判定：git show 读到对象就是 0，报错误非 0——不像 --no-index 那样有「1 也算正常」的语义。
  options.scopeNotice = plan.notice;
  options.startedNote = L"已在命令窗口执行 git show（" + repositoryRoot + L"）：查看提交 " +
                        git::ShortObjectId(item.objectId) + L"「" + item.summary +
                        L"」，等待 Git 退出码…";
  if (!LaunchCommandWindow(operation, options)) {
    SetStatus(L"这次查看没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    UpdateUi();
  }
}

void MainFrame::OnFileDoubleClicked(int row) {
  if (!RequireViewPrerequisites(L"查看差异")) {
    return;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    SetStatus(L"这一轮仓库状态没有读成功，提交历史不可用。请点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const git::CommitItem* commit = nullptr;
  const git::ChangeItem* change = nullptr;
  if (!panel_->CommitFileAt(row, &commit, &change)) {
    SetStatus(L"这一行已不在当前「文件变化」清单里（清单刚被刷新），请重新选择提交后再双击。");
    UpdateUi();
    return;
  }
  // 两头核对：显示行数与已落地清单条数、清单所属提交，都必须是同一份。
  if (panel_->FilesRowCount() != panel_->CommitFilesCount() ||
      commit->objectId != panel_->CommitFilesCommitId()) {
    SetStatus(L"「文件变化」的显示行数与已读取的内容对不上，为避免看错文件，本次没有执行任何命令。"
              L"点“刷新”后重试。");
    UpdateUi();
    return;
  }
  const git::CommitFileDiffPlan plan = git::BuildCommitFileDiffPlan(*commit, *change);
  if (!plan.allowed) {
    const std::wstring message =
        L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.refusalReason + L"\n\n条目：" +
        change->PathLabel();
    ShowInfo(L"无法查看该文件", message);
    SetStatus(L"未查看 " + change->PathLabel() + L"：" + plan.refusalReason);
    UpdateUi();
    return;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  git::CommandWindowOperation operation =
      git::MakeCommandWindowOperation(plan.operationId, plan.displayName, state_.Git().path,
                                      repositoryRoot, plan.arguments);
  CommandLaunchOptions options;
  // 与双击提交同一条退出口径：git show 读到对象就是 0，报错误非 0。
  options.scopeNotice = plan.notice;
  options.startedNote = L"已在命令窗口执行 git show（" + repositoryRoot + L"）：查看提交 " +
                        git::ShortObjectId(commit->objectId) + L" 里的 " + change->PathLabel() +
                        L"，等待 Git 退出码…";
  if (!LaunchCommandWindow(operation, options)) {
    SetStatus(L"这次查看没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    UpdateUi();
  }
}

void MainFrame::LaunchStatusOperation() {
  if (!state_.GitUsable() || !state_.RepoUsable()) {
    return;
  }
  if (tasks_.OperationInFlight()) {
    SetStatus(L"已有一个命令窗口操作在进行，请等它结束。");
    UpdateUi();
    return;
  }
  const std::wstring gitExe = state_.Git().path;
  std::wstring repository = state_.Repo().detection.root;
  if (repository.empty()) {
    repository = state_.Info().repoPath;
  }
  // 只读地查看工作区状态；参数按数组提交，不进任何 shell 字符串。
  git::CommandWindowOperation operation = git::MakeCommandWindowOperation(
      L"status", L"status", gitExe, repository, {L"status"});
  CommandLaunchOptions options;
  options.startedNote =
      L"已在命令窗口启动 git status（" + repository + L"），等待 Git 退出码…";
  static_cast<void>(LaunchCommandWindow(operation, options));
}

void MainFrame::CopyCommitDetailToClipboard() {
  const std::wstring text = panel_->CommitDetailText();
  if (text.empty()) {
    SetStatus(L"现在没有可复制的提交详情：请先在「最近提交」里选中一条，并等它读出来。");
    UpdateUi();
    return;
  }
  bool copied = false;
  if (wxTheClipboard->Open()) {
    copied = wxTheClipboard->SetData(new wxTextDataObject(FromWide(text)));
    wxTheClipboard->Close();
  }
  if (!copied) {
    SetStatus(L"没能复制到剪贴板。 面板上的文字仍可以直接选中后用 Ctrl+C。");
    UpdateUi();
    return;
  }
  SetStatus(L"已把这条提交的完整信息复制到剪贴板（只是复制，没有执行任何命令）。");
  UpdateUi();
}

void MainFrame::OpenTerminalDialog() {
  const platform::TerminalSelection current = commandWindows_->DescribeTerminalSelection();
  if (current.candidates.size() <= 1) {
    // Windows 只有控制台辅助入口一种候选：没有可选项，如实说明，不摆一个点了没反应的框。
    ShowInfo(L"终端选择",
             current.guidance.empty()
                 ? L"本平台的命令窗口固定使用控制台辅助入口，没有可替换的终端模拟器实现。"
                 : current.guidance);
    return;
  }

  wxDialog dialog(this, wxID_ANY, FromWide(L"命令窗口终端选择"), wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* sizer = new wxBoxSizer(wxVERTICAL);
  wxString choiceHint;
  if (!current.usable) {
    choiceHint = FromWide(L"当前没有可用终端：" +
                          (current.failureReason.empty() ? current.guidance : current.failureReason));
  } else {
    choiceHint = FromWide(L"当前使用：" + current.selected.programPath);
  }
  sizer->Add(new wxStaticText(&dialog, wxID_ANY, choiceHint), 0, wxALL, 10);
  if (!current.note.empty()) {
    sizer->Add(new wxStaticText(&dialog, wxID_ANY, FromWide(current.note)), 0, wxLEFT | wxRIGHT, 10);
  }
  wxArrayString labels;
  for (const platform::TerminalCandidate& candidate : current.candidates) {
    std::wstring label = std::wstring(platform::TerminalAdapterDisplayName(candidate.kind));
    label += L" — " + candidate.programPath;
    if (!candidate.discoveredAs.empty() && candidate.discoveredAs != candidate.programPath) {
      label += L"（经 " + candidate.discoveredAs + L"）";
    }
    labels.Add(FromWide(label));
  }
  auto* radio = new wxRadioBox(&dialog, wxID_ANY, FromWide(L"显式选择终端"), wxDefaultPosition,
                               wxDefaultSize, labels, 1, wxRA_SPECIFY_COLS);
  sizer->Add(radio, 1, wxEXPAND | wxALL, 10);
  auto* fallback = new wxCheckBox(&dialog, wxID_ANY,
                                  FromWide(L"显式选择不可用时回退到其他终端"));
  fallback->SetValue(commandWindows_->GetTerminalPreferences().allowFallback);
  sizer->Add(fallback, 0, wxLEFT | wxRIGHT, 10);
  sizer->Add(
      new wxStaticText(
          &dialog, wxID_ANY,
          FromWide(L"选择只对本次运行生效；终端设置的持久化尚未迁移到统一界面。")),
      0, wxALL, 10);
  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  buttons->AddStretchSpacer();
  buttons->Add(new wxButton(&dialog, wxID_OK, FromWide(L"确定")), 0, wxRIGHT, 6);
  buttons->Add(new wxButton(&dialog, wxID_CANCEL, FromWide(L"取消")), 0);
  sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);
  dialog.SetSizerAndFit(sizer);
  if (current.usable) {
    for (size_t i = 0; i < current.candidates.size(); ++i) {
      if (current.candidates[i].programPath == current.selected.programPath) {
        radio->SetSelection(static_cast<int>(i));
        break;
      }
    }
  }
  if (dialog.ShowModal() != wxID_OK) {
    return;
  }
  const int selection = radio->GetSelection();
  if (selection < 0 || static_cast<size_t>(selection) >= current.candidates.size()) {
    return;
  }
  const platform::TerminalCandidate& chosen = current.candidates[static_cast<size_t>(selection)];
  platform::TerminalPreferences prefs;
  prefs.preferredKind = chosen.kind;
  prefs.preferredProgramPath = chosen.programPath;
  prefs.allowFallback = fallback->GetValue();
  std::wstring refusal;
  if (!commandWindows_->SetTerminalPreferences(prefs, &refusal)) {
    ShowWarning(L"终端选择", refusal);
    return;
  }
  SetStatus(L"终端选择已更新（本会话生效）：" + chosen.programPath +
            L"；下一次命令窗口操作起使用。终端设置的持久化尚未迁移。");
  UpdateUi();
}

void MainFrame::SetStatus(std::wstring note) {
  state_.SetStatusNote(std::move(note));
  if (statusText_ != nullptr) {
    SetLabelIfChanged(statusText_, FromWide(state_.StatusNote()));
    const int width = GetClientSize().GetWidth() > 200 ? GetClientSize().GetWidth() - 40 : 900;
    statusText_->Wrap(width);
    Layout();
  }
}

void MainFrame::SetFormNote(std::wstring note) {
  state_.SetFormNote(std::move(note));
  if (formNoteText_ != nullptr) {
    SetLabelIfChanged(formNoteText_, FromWide(state_.FormNote()));
    const int width = GetClientSize().GetWidth() > 200 ? GetClientSize().GetWidth() - 40 : 900;
    formNoteText_->Wrap(width);
    Layout();
  }
}

void MainFrame::RefreshUi() { UpdateUi(); }

bool MainFrame::Confirm(const std::wstring& title, const std::wstring& body, bool warningIcon) {
  wxMessageDialog dialog(this, FromWide(body), FromWide(title),
                         wxOK | wxCANCEL |
                             (warningIcon ? wxICON_WARNING : wxICON_INFORMATION));
  dialog.SetOKCancelLabels(FromWide(L"确定"), FromWide(L"取消"));
  return dialog.ShowModal() == wxID_OK;
}

void MainFrame::ShowInfo(const std::wstring& title, const std::wstring& body) {
  wxMessageBox(FromWide(body), FromWide(title), wxOK | wxICON_INFORMATION, this);
}

void MainFrame::ShowWarning(const std::wstring& title, const std::wstring& body) {
  wxMessageBox(FromWide(body), FromWide(title), wxOK | wxICON_WARNING, this);
}

bool MainFrame::RiskConfirm(const std::wstring& title, const std::wstring& mainInstruction,
                            const std::wstring& yesButton, const std::wstring& body) {
  wxMessageDialog dialog(this, FromWide(mainInstruction + L"\n\n" + body), FromWide(title),
                         wxYES_NO | wxICON_WARNING);
  dialog.SetYesNoLabels(FromWide(yesButton), FromWide(L"取消"));
  return dialog.ShowModal() == wxID_YES;
}

std::optional<size_t> MainFrame::PromptSingleChoice(platform::SingleChoiceRequest request) {
  wxArrayString choices;
  for (const platform::ChoiceItem& item : request.items) {
    choices.Add(FromWide(item.detail.empty() ? item.label : item.label + L" — " + item.detail));
  }
  wxSingleChoiceDialog dialog(this, FromWide(request.label), FromWide(request.title), choices);
  if (!choices.IsEmpty()) {
    dialog.SetSelection(0);
  }
  if (dialog.ShowModal() != wxID_OK) {
    return std::nullopt;
  }
  const int selection = dialog.GetSelection();
  if (selection < 0 || static_cast<size_t>(selection) >= request.items.size()) {
    return std::nullopt;
  }
  return static_cast<size_t>(selection);
}

std::optional<std::wstring> MainFrame::PromptForText(platform::TextPromptRequest request) {
  std::wstring initial = request.initialValue;
  for (;;) {
    wxTextEntryDialog dialog(this, FromWide(request.label), FromWide(request.title),
                             FromWide(initial));
    if (dialog.ShowModal() != wxID_OK) {
      return std::nullopt;
    }
    const std::wstring value = ToWide(dialog.GetValue());
    const std::wstring reason = request.validate ? request.validate(value) : std::wstring();
    if (reason.empty()) {
      return value;
    }
    wxMessageBox(FromWide(reason), FromWide(request.title), wxOK | wxICON_WARNING, this);
    initial = value;
  }
}

bool MainFrame::LaunchCommandWindow(const git::CommandWindowOperation& operation,
                                    const CommandLaunchOptions& options) {
  const std::wstring refusal = git::DescribeCommandWindowOperationRefusal(operation);
  if (!refusal.empty()) {
    platform::RemoveNulPathspecFile(options.pathspecFile);
    platform::RemoveCommitMessageFile(options.messageFile);
    SetStatus(L"没有启动命令窗口：" + refusal);
    UpdateUi();
    return false;
  }
  // 公共边界上先核对「这次要落的那条历史记录本身成立不成立」：漏了仓库工作区根或没说清
  // 是哪类操作，会等真正写盘时才被整条拒掉——那等于一件做过的操作在历史里查无此事。
  // 历史开关没开时一律放行（不记录就没有可核对的记录）。
  const std::wstring historyRefusal =
      app::DescribeHistoryCaptureRefusal(options.history, HistoryRecordingEnabled());
  if (!historyRefusal.empty()) {
    platform::RemoveNulPathspecFile(options.pathspecFile);
    platform::RemoveCommitMessageFile(options.messageFile);
    SetStatus(historyRefusal + L"（没有启动命令窗口，仓库没有被改动。）");
    UpdateUi();
    return false;
  }
  unsigned long long serial = 0;
  if (!tasks_.BeginOperation(operation.displayName, &serial, options.policy)) {
    platform::RemoveNulPathspecFile(options.pathspecFile);
    platform::RemoveCommitMessageFile(options.messageFile);
    UpdateUi();
    return false;
  }
  uint64_t runnerId = 0;
  platform::CommandWindowResult failure;
  if (!commandWindows_->Start(operation, &runnerId, &failure)) {
    const app::OperationOutcome outcome =
        tasks_.FinishOperation(serial, failure.completion, failure.exitCode, failure.failureReason);
    platform::RemoveNulPathspecFile(options.pathspecFile);
    platform::RemoveCommitMessageFile(options.messageFile);
    std::wstring note = outcome.note;
    if (!failure.environmentNotice.empty()) {
      note += L"｜" + failure.environmentNotice;
    }
    // 启动失败本身也是一种终态：命令窗口没打开、Git 一个字都没跑。先写 Git 那一步的结论，
    // 再把落历史被拒的那句并列附上；谁也不盖谁。
    std::wstring launchHistoryRefusal;
    if (options.history.record && HistoryRecordingEnabled()) {
      const platform::LocalInstant now = platform::CurrentLocalInstant();
      const long long epoch = now.valid ? now.utcEpochSeconds : 0;
      app::HistoryTerminalInfo terminal;
      terminal.outcome = app::HistoryOutcome::launchFailed;
      terminal.startedEpoch = epoch;
      terminal.terminalEpoch = epoch;
      terminal.completionLabel = std::wstring(git::CommandCompletionLabel(failure.completion));
      terminal.conclusion = note;
      RecordHistory(options.history, terminal, std::wstring(), &launchHistoryRefusal);
    }
    if (!launchHistoryRefusal.empty()) {
      note += L"｜" + launchHistoryRefusal;
    }
    SetStatus(note);
    UpdateUi();
    ScheduleRefreshSoon();
    return false;
  }

  activeOperationSerial_ = serial;
  activeRunnerId_ = runnerId;
  activePathspecFile_ = options.pathspecFile;
  activeMessageFile_ = options.messageFile;
  activeCommitOperation_ = options.commitOperation;
  activeCommittedForm_ = options.committedForm;
  activeViewKind_ = options.viewKind;
  activeUndoOperation_ = options.undoOperation;
  activeRestoreHint_ = options.restoreHint;
  activeRestoreOperation_ = options.restoreOperation;
  activeHistoryCapture_ = options.history;
  activeHistoryStartedEpoch_ = 0;
  activeHistoryRecordId_.clear();
  activeHistoryRecording_ = false;
  operationTimer_.Start(static_cast<int>(kOperationPollMs));
  SetStatus(options.startedNote);

  if (options.history.record && HistoryRecordingEnabled()) {
    const platform::LocalInstant now = platform::CurrentLocalInstant();
    activeHistoryStartedEpoch_ = now.valid ? now.utcEpochSeconds : 0;
    std::wstring inFlightRefusal;
    activeHistoryRecordId_ = BeginInProgressHistory(
        activeHistoryCapture_, activeHistoryStartedEpoch_, &inFlightRefusal);
    activeHistoryRecording_ = !activeHistoryRecordId_.empty();
    if (!inFlightRefusal.empty()) {
      // Git 已经在跑，记录只是它旁边的附属信息：把这句附在启动结论后面，不盖掉它。
      SetStatus(state_.StatusNote() + L"｜" + inFlightRefusal +
                L"（Git 这一步照常执行，但这次之后查不到它的线索。）");
    }
  }
  UpdateUi();
  return true;
}

void MainFrame::RememberOperationConclusion(std::wstring_view conclusion) {
  tasks_.RememberOperationConclusion(conclusion);
}

void MainFrame::RecordPushVerification(std::wstring_view verificationSummary) {
  static_cast<void>(verificationSummary);
}

bool MainFrame::NavigateRepository(std::wstring_view directory, std::wstring_view statusNote) {
  if (directory.empty()) {
    SetStatus(L"导航没有发起：目标目录为空。");
    return false;
  }
  repoPath_->ChangeValue(FromWide(directory));
  state_.SetRepoPath(std::wstring(directory));
  SetStatus(std::wstring(statusNote));
  refreshCycleActive_ = true;
  refreshPending_ = false;
  RequestRepoDetection(true);
  return true;
}

CommitFormSnapshot MainFrame::CaptureCommitForm() const {
  CommitFormSnapshot snapshot;
  snapshot.data = form_->Capture();
  snapshot.authorWall = form_->AuthorWallTime();
  snapshot.committerWall = form_->CommitterWallTime();
  snapshot.timesSynced = form_->TimeSyncChecked();
  return snapshot;
}

bool MainFrame::CommitTimesUserEdited() const { return form_->TimesUserEdited(); }

void MainFrame::ApplyDefaultTimesToNow() {
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  if (!now.valid) {
    SetStatus(L"无法取得本机当前时间：" + now.failureReason);
    return;
  }
  form_->SetTimes(now.wall, now.wall, true);
  UpdateUi();
}

void MainFrame::RunFormValidation(std::wstring_view prefixNote) {
  const git::CommitFormValidity validity =
      git::ValidateCommitForm(form_->Capture(), state_.Author().config.CommitterState());
  std::wstring text;
  if (!prefixNote.empty()) {
    text = std::wstring(prefixNote) + L"｜";
  }
  text += validity.StatusText();
  SetFormNote(std::move(text));
}

void MainFrame::ApplyCommittedFormCleanup(const app::CommittedFormCleanup& cleanup) {
  form_->ClearCommittedFields(cleanup.clearSubject, cleanup.clearDescription, cleanup.clearCoauthors);
  if (cleanup.resetTimes) {
    ApplyDefaultTimesToNow();
  }
  formSession_.NoteCommitted(cleanup.clearSubject, cleanup.clearDescription,
                             cleanup.clearCoauthors);
  SetFormNote(cleanup.note);
  UpdateUi();
}

bool MainFrame::CommitFormHasUserContent() const { return formSession_.HasUserContent(); }

void MainFrame::ClearFormForNavigation(std::wstring_view note) {
  form_->ClearAllFields();
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  if (now.valid) {
    form_->SetTimes(now.wall, now.wall, true);
  }
  formSession_.Reset();
  SetFormNote(std::wstring(note));
  UpdateUi();
}

void MainFrame::ApplyHeldFormForNavigation(const app::HeldForm& held, std::wstring_view note) {
  form_->SetFormData(held.form);
  form_->SetTimes(held.authorWall, held.committerWall, true);
  form_->SetTimeSyncChecked(held.timesSynced);
  if (held.timesUserEdited) {
    form_->MarkTimesUserEdited();
  }
  using Field = app::CommitFormSession::Field;
  if (!held.form.subject.empty()) {
    formSession_.NoteUserEdit(Field::subject);
  }
  if (!held.form.description.empty()) {
    formSession_.NoteUserEdit(Field::description);
  }
  if (!held.form.coauthors.empty()) {
    formSession_.NoteUserEdit(Field::coauthors);
  }
  if (!held.form.author.empty()) {
    formSession_.NoteUserEdit(Field::author);
  }
  SetFormNote(std::wstring(note));
  RunFormValidation({});
  UpdateUi();
}

void MainFrame::OnClose(wxCloseEvent& event) {
  if (activeRunnerId_ != 0) {
    wxMessageDialog dialog(
        this,
        FromWide(L"命令窗口里的 Git 仍可能在运行。关闭本界面不会终止那个 Git 进程，"
                 L"但本窗口之后无法再显示它的退出码与结果。\n\n仍要关闭界面吗？"),
        FromWide(L"仍有操作在进行"), wxYES_NO | wxICON_WARNING);
    dialog.SetYesNoLabels(FromWide(L"关闭界面"), FromWide(L"留下"));
    if (dialog.ShowModal() != wxID_YES) {
      event.Veto();
      return;
    }
  }
  if (closing_) {
    event.Skip();
    return;
  }
  closing_ = true;
  // 关闭前把还没结案的在途记录补成「结果未知」；这一刻是最后能写盘的机会，
  // 因此紧接着同步执行一次保存，不依赖那个会被 StopWorkers 停掉的防抖定时器。
  SettleUnfinishedHistoryOnShutdown();
  RunHistorySave();
  StopWorkers();
  event.Skip();
}

}  // namespace gc::ui::wx
