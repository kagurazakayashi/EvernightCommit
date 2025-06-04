#include "ui/wx/main_frame.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dateevt.h>
#include <wx/dirdlg.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include "git/commit_identity.h"
#include "git/repository.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/git_discovery.h"
#include "platform/git_toolchain.h"
#include "platform/local_time.h"
#include "platform/path.h"
#include "platform/scratch_files.h"
#include "ui/wx/wx_text.h"

namespace gc::ui::wx {
namespace {

constexpr uint64_t kGitVerifySource = 1;
constexpr uint64_t kRepoDetectSource = 2;
constexpr uint64_t kWorkspaceSource = 3;
constexpr uint64_t kAuthorConfigSource = 4;
constexpr uint64_t kCommitProbeSource = 5;

constexpr int kRefreshTimerId = wxID_HIGHEST + 101;
constexpr int kOperationTimerId = wxID_HIGHEST + 102;

constexpr unsigned long kGitVerifyTimeoutMs = 3000;
constexpr unsigned long kRepoDetectTimeoutMs = 8000;
constexpr unsigned long kWorkspaceStatusTimeoutMs = 20000;
constexpr unsigned long kAuthorConfigTimeoutMs = 8000;
constexpr unsigned long kRefreshDebounceMs = 200;
constexpr unsigned long kOperationPollMs = 500;

bool ContainsPath(const std::vector<std::wstring>& paths, const std::wstring& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

std::vector<std::wstring> SelectedPaths(const wxListBox& list,
                                        const std::vector<git::ChangeItem>& items) {
  std::vector<std::wstring> paths;
  for (unsigned int row = 0; row < list.GetCount(); ++row) {
    if (list.IsSelected(row) && row < items.size()) {
      paths.push_back(items[row].path);
    }
  }
  return paths;
}

}  // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, FromWide(L"EvernightCommit — 暂存与创建提交"),
              wxDefaultPosition, wxSize(1100, 760)),
      refreshTimer_(this, kRefreshTimerId),
      operationTimer_(this, kOperationTimerId) {
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
  commitFlow_.SetCompletionDispatcher(dispatcher_, kCommitProbeSource);

  commandWindows_ = platform::MakeCommandWindowService();
  commandWindows_->SweepStaleOperationDirectories();

  BuildUi();
  BindEvents();
  SetMinSize(wxSize(900, 700));
  Centre();

  const wxString cwd = wxGetCwd();
  state_.SetRepoPath(ToWide(cwd));
  repoPath_->SetValue(cwd);
  state_.SetStatusNote(L"正在验证 Git 程序并识别当前目录…");
  UpdateUi();

  CallAfter([this] { StartInitialization(); });
}

MainFrame::~MainFrame() {
  refreshTimer_.Stop();
  operationTimer_.Stop();
  if (completionPump_) {
    completionPump_->Stop();
  }
  gitWorker_.BeginStop();
  repoWorker_.BeginStop();
  workspaceWorker_.BeginStop();
  authorWorker_.BeginStop();
  commitFlow_.BeginStop();
  gitWorker_.Join();
  repoWorker_.Join();
  workspaceWorker_.Join();
  authorWorker_.Join();
  commitFlow_.JoinWorkers();
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

  // 主体放进滚动区：864 高的笔记本屏幕上也要能滚到提交表单的最后一行。
  auto* mainScroll =
      new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 200), wxVSCROLL);
  auto* panel = new wxPanel(mainScroll);
  auto* root = new wxBoxSizer(wxVERTICAL);

  auto* repoRow = new wxBoxSizer(wxHORIZONTAL);
  repoRow->Add(new wxStaticText(panel, wxID_ANY, FromWide(L"本地仓库")), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  repoPath_ = new wxTextCtrl(panel, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                             wxTE_PROCESS_ENTER);
  repoRow->Add(repoPath_, 1, wxEXPAND | wxRIGHT, 8);
  browseRepo_ = new wxButton(panel, wxID_ANY, FromWide(L"浏览…"));
  repoRow->Add(browseRepo_, 0, wxRIGHT, 6);
  refreshButton_ = new wxButton(panel, wxID_ANY, FromWide(L"刷新"));
  repoRow->Add(refreshButton_, 0);
  root->Add(repoRow, 0, wxEXPAND | wxALL, 10);

  gitLabel_ = new wxStaticText(panel, wxID_ANY, FromWide(L"Git：正在发现…"));
  root->Add(gitLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  repoSummary_ = new wxStaticText(panel, wxID_ANY, FromWide(L"尚未识别仓库。"), wxDefaultPosition,
                                  wxDefaultSize, wxST_NO_AUTORESIZE);
  root->Add(repoSummary_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  auto* changesRow = new wxBoxSizer(wxHORIZONTAL);
  auto* unstagedColumn = new wxBoxSizer(wxVERTICAL);
  unstagedLabel_ = new wxStaticText(panel, wxID_ANY, FromWide(L"未暂存的更改"));
  unstagedColumn->Add(unstagedLabel_, 0, wxBOTTOM, 4);
  unstagedList_ = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 100), 0, nullptr,
                                wxLB_EXTENDED);
  unstagedColumn->Add(unstagedList_, 1, wxEXPAND);
  unstagedHint_ = new wxStaticText(panel, wxID_ANY, FromWide(L"尚未读取。"), wxDefaultPosition,
                                   wxDefaultSize, wxST_NO_AUTORESIZE);
  unstagedColumn->Add(unstagedHint_, 0, wxTOP | wxEXPAND, 4);
  changesRow->Add(unstagedColumn, 1, wxEXPAND | wxRIGHT, 10);

  auto* stagingButtons = new wxBoxSizer(wxVERTICAL);
  stageAll_ = new wxButton(panel, wxID_ANY, FromWide(L"全部加入 →"));
  stageSelected_ = new wxButton(panel, wxID_ANY, FromWide(L"加入暂存区 →"));
  unstageSelected_ = new wxButton(panel, wxID_ANY, FromWide(L"← 移出暂存区"));
  unstageAll_ = new wxButton(panel, wxID_ANY, FromWide(L"← 全部移出"));
  stagingButtons->AddStretchSpacer();
  stagingButtons->Add(stageAll_, 0, wxEXPAND | wxBOTTOM, 6);
  stagingButtons->Add(stageSelected_, 0, wxEXPAND | wxBOTTOM, 12);
  stagingButtons->Add(unstageSelected_, 0, wxEXPAND | wxBOTTOM, 6);
  stagingButtons->Add(unstageAll_, 0, wxEXPAND);
  stagingButtons->AddStretchSpacer();
  changesRow->Add(stagingButtons, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

  auto* stagedColumn = new wxBoxSizer(wxVERTICAL);
  stagedColumn->Add(new wxStaticText(panel, wxID_ANY, FromWide(L"已暂存的更改")), 0, wxBOTTOM, 4);
  stagedList_ = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 100), 0, nullptr,
                              wxLB_EXTENDED);
  stagedColumn->Add(stagedList_, 1, wxEXPAND);
  stagedHint_ = new wxStaticText(panel, wxID_ANY, FromWide(L"尚未读取。"), wxDefaultPosition,
                                 wxDefaultSize, wxST_NO_AUTORESIZE);
  stagedColumn->Add(stagedHint_, 0, wxTOP | wxEXPAND, 4);
  changesRow->Add(stagedColumn, 1, wxEXPAND);
  root->Add(changesRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

  auto* commitHeader = new wxBoxSizer(wxHORIZONTAL);
  commitHeader->Add(new wxStaticText(panel, wxID_ANY, FromWide(L"提交表单")), 1,
                    wxALIGN_CENTER_VERTICAL);
  createCommit_ = new wxButton(panel, wxID_ANY, FromWide(L"创建提交…"));
  commitHeader->Add(createCommit_, 0);
  root->Add(commitHeader, 0, wxEXPAND | wxALL, 10);

  form_ = new CommitFormPanel(panel);
  root->Add(form_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  panel->SetSizer(root);
  auto* scrollSizer = new wxBoxSizer(wxVERTICAL);
  scrollSizer->Add(panel, 1, wxEXPAND);
  mainScroll->SetSizer(scrollSizer);
  mainScroll->SetScrollRate(0, 12);
  mainScroll->SetMinSize(wxSize(-1, 200));
  frameSizer->Add(mainScroll, 1, wxEXPAND);

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
}

void MainFrame::BindEvents() {
  browseRepo_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxDirDialog dialog(this, FromWide(L"选择本地 Git 仓库"), repoPath_->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
      return;
    }
    repoPath_->SetValue(dialog.GetPath());
    RequestRepoDetection(true);
  });
  repoPath_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { RequestRepoDetection(true); });
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

  stageSelected_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StageSelected(); });
  stageAll_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StageAll(); });
  unstageSelected_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { UnstageSelected(); });
  unstageAll_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { UnstageAll(); });
  createCommit_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CreateCommit(); });
  unstagedList_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
    selectedUnstagedPaths_ = SelectedPaths(*unstagedList_, state_.WorkspaceModel().unstaged);
  });
  stagedList_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
    selectedStagedPaths_ = SelectedPaths(*stagedList_, state_.WorkspaceModel().staged);
  });

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
  Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
}

void MainFrame::StartInitialization() {
  const std::vector<std::wstring> candidates = platform::DiscoverGitCandidates();
  if (candidates.empty()) {
    SetStatus(L"没有在 PATH 上找到 Git 程序。请先安装 Git，或把 Git 加入 PATH 后重新启动本程序。");
    UpdateUi();
    return;
  }
  RequestGitVerification(platform::NormalizeGitExeInput(candidates.front()));
}

void MainFrame::RequestGitVerification(const std::wstring& path) {
  app::GitToolState tool;
  tool.status = app::GitExeStatus::verifying;
  tool.path = path;
  tool.message = L"正在后台执行 git --version 验证…";
  state_.SetGitTool(std::move(tool));
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
  }
}

void MainFrame::OnGitVerificationCompleted(uint64_t serial) {
  platform::GitExeVerification verification;
  if (!gitWorker_.FetchLatest(serial, &verification)) {
    return;
  }
  app::GitToolState tool;
  tool.path = verification.path;
  if (verification.outcome == git::GitProbeOutcome::verified) {
    tool.status = app::GitExeStatus::verified;
    tool.version = verification.version;
    state_.SetGitTool(std::move(tool));
    state_.SetGitExePath(verification.path);
    SetStatus(L"Git 程序可用：" + verification.path + L"（版本 " + verification.version + L"）。");
    repoPath_->SetValue(FromWide(platform::ToAbsolutePath(ToWide(repoPath_->GetValue()))));
    refreshCycleActive_ = true;
    RequestRepoDetection(true);
  } else {
    tool.status = app::GitExeStatus::invalid;
    tool.message = verification.message;
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
  selectedUnstagedPaths_ = SelectedPaths(*unstagedList_, state_.WorkspaceModel().unstaged);
  selectedStagedPaths_ = SelectedPaths(*stagedList_, state_.WorkspaceModel().staged);
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

void MainFrame::OnCommitProbeCompleted(uint64_t serial) {
  commitFlow_.OnProbeCompleted(*this, CaptureOperationContext(), state_.WorkspaceModel(), serial);
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
    const app::OperationOutcome outcome =
        tasks_.ForgetOperation(activeOperationSerial_, L"执行器已不再认识这次操作（结果未知）");
    platform::RemoveNulPathspecFile(activePathspecFile_);
    platform::RemoveCommitMessageFile(activeMessageFile_);
    if (activeCommitOperation_) {
      commitFlow_.OnCommandSettled(*this, false, activeCommittedForm_);
    }
    SetStatus(outcome.note);
    activeOperationSerial_ = 0;
    activeRunnerId_ = 0;
    activePathspecFile_.clear();
    activeMessageFile_.clear();
    activeCommitOperation_ = false;
    activeCommittedForm_ = {};
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
  activeOperationSerial_ = 0;
  activeRunnerId_ = 0;
  activePathspecFile_.clear();
  activeMessageFile_.clear();
  activeCommitOperation_ = false;
  activeCommittedForm_ = {};
  operationTimer_.Stop();

  if (commitOperation) {
    commitFlow_.OnCommandSettled(*this, succeeded, committedForm);
  }

  std::wstring note = outcome.note;
  if (!result.environmentNotice.empty()) {
    note += L"｜" + result.environmentNotice;
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

void MainFrame::ClearWorkspaceState() {
  selectedUnstagedPaths_.clear();
  selectedStagedPaths_.clear();
  state_.SetWorkspace(git::WorkspaceSnapshot{});
  RebuildChangesLists();
}

void MainFrame::RebuildChangesLists() {
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  const std::vector<std::wstring>& keepUnstaged = selectedUnstagedPaths_;
  const std::vector<std::wstring>& keepStaged = selectedStagedPaths_;

  unstagedList_->Clear();
  for (const git::ChangeItem& item : model.unstaged) {
    unstagedList_->Append(FromWide(item.StatusLabel() + L"  " + item.PathLabel()));
  }
  for (size_t row = 0; row < model.unstaged.size(); ++row) {
    if (ContainsPath(keepUnstaged, model.unstaged[row].path)) {
      unstagedList_->SetSelection(static_cast<int>(row));
    }
  }

  stagedList_->Clear();
  for (const git::ChangeItem& item : model.staged) {
    stagedList_->Append(FromWide(item.StatusLabel() + L"  " + item.PathLabel()));
  }
  for (size_t row = 0; row < model.staged.size(); ++row) {
    if (ContainsPath(keepStaged, model.staged[row].path)) {
      stagedList_->SetSelection(static_cast<int>(row));
    }
  }
}

void MainFrame::UpdateUi() {
  if (state_.GitUsable()) {
    gitLabel_->SetLabel(FromWide(L"Git：" + state_.Git().path + L"（" +
                                 (state_.Git().version.empty() ? L"已验证" : state_.Git().version) +
                                 L"）"));
  } else {
    const std::wstring message = state_.Git().message.empty() ? L"正在发现 Git 程序…"
                                                              : state_.Git().message;
    gitLabel_->SetLabel(FromWide(L"Git：" + message));
  }

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
  repoSummary_->SetLabel(FromWide(summary));

  const git::EmptyStateTexts texts = state_.WorkspaceHintTexts();
  if (unstagedList_->GetCount() == 0) {
    unstagedHint_->SetLabel(FromWide(texts.unstaged));
  } else {
    unstagedHint_->SetLabel(
        FromWide(L"共 " + std::to_wstring(unstagedList_->GetCount()) + L" 项；只处理选中的条目"));
  }
  std::wstring submodule = git::SubmoduleExplanationText(state_.WorkspaceModel());
  if (!submodule.empty()) {
    unstagedHint_->SetLabel(unstagedHint_->GetLabel() + FromWide(L" " + submodule));
  }
  if (stagedList_->GetCount() == 0) {
    stagedHint_->SetLabel(FromWide(texts.staged));
  } else {
    stagedHint_->SetLabel(FromWide(L"共 " + std::to_wstring(stagedList_->GetCount()) +
                                   L" 项；提交只来自索引里这一份"));
  }

  const bool busy = tasks_.OperationInFlight();
  const bool repositoryReady = state_.GitUsable() && state_.RepoUsable();
  const bool workspaceReady = repositoryReady &&
                              state_.Workspace().status == git::WorkspaceLoadStatus::loaded;
  refreshButton_->Enable(state_.GitUsable());
  browseRepo_->Enable(!busy);
  const bool stagingReady = workspaceReady && !busy;
  stageSelected_->Enable(stagingReady);
  stageAll_->Enable(stagingReady);
  unstageSelected_->Enable(stagingReady);
  unstageAll_->Enable(stagingReady);
  createCommit_->Enable(workspaceReady && !busy && !commitFlow_.Active());

  statusText_->SetLabel(FromWide(state_.StatusNote()));
  formNoteText_->SetLabel(FromWide(state_.FormNote()));
  const int width = GetClientSize().GetWidth() > 200 ? GetClientSize().GetWidth() - 40 : 900;
  statusText_->Wrap(width);
  formNoteText_->Wrap(width);
  repoSummary_->Wrap(width);
  unstagedHint_->Wrap(width / 2 - 20 > 100 ? width / 2 - 20 : 300);
  stagedHint_->Wrap(width / 2 - 20 > 100 ? width / 2 - 20 : 300);
  Layout();
}

std::vector<int> MainFrame::SelectedRows(const wxListBox& list) const {
  std::vector<int> rows;
  for (unsigned int row = 0; row < list.GetCount(); ++row) {
    if (list.IsSelected(row)) {
      rows.push_back(static_cast<int>(row));
    }
  }
  return rows;
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
  const std::vector<int> rows = SelectedRows(*unstagedList_);
  if (rows.empty()) {
    SetStatus(L"先在“未暂存的更改”里选中文件，再点“加入暂存区”；没有选中时不会暂存全部。");
    UpdateUi();
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  if (unstagedList_->GetCount() != static_cast<unsigned int>(model.unstaged.size())) {
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
  const std::vector<int> rows = SelectedRows(*stagedList_);
  if (rows.empty()) {
    SetStatus(L"先在“已暂存的更改”里选中文件，再点“移出暂存区”；没有选中时不会取消全部。");
    UpdateUi();
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  if (stagedList_->GetCount() != static_cast<unsigned int>(model.staged.size())) {
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
  if (unstagedList_->GetCount() != static_cast<unsigned int>(model.unstaged.size())) {
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
  if (stagedList_->GetCount() != static_cast<unsigned int>(model.staged.size())) {
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

void MainFrame::SetStatus(std::wstring note) {
  state_.SetStatusNote(std::move(note));
  if (statusText_ != nullptr) {
    statusText_->SetLabel(FromWide(state_.StatusNote()));
    const int width = GetClientSize().GetWidth() > 200 ? GetClientSize().GetWidth() - 40 : 900;
    statusText_->Wrap(width);
    Layout();
  }
}

void MainFrame::SetFormNote(std::wstring note) {
  state_.SetFormNote(std::move(note));
  if (formNoteText_ != nullptr) {
    formNoteText_->SetLabel(FromWide(state_.FormNote()));
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
  operationTimer_.Start(static_cast<int>(kOperationPollMs));
  SetStatus(options.startedNote);
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
  refreshTimer_.Stop();
  operationTimer_.Stop();
  if (completionPump_) {
    completionPump_->Stop();
  }
  gitWorker_.BeginStop();
  repoWorker_.BeginStop();
  workspaceWorker_.BeginStop();
  authorWorker_.BeginStop();
  commitFlow_.BeginStop();
  gitWorker_.Join();
  repoWorker_.Join();
  workspaceWorker_.Join();
  authorWorker_.Join();
  commitFlow_.JoinWorkers();
  if (commitFlow_.Active()) {
    commitFlow_.ReleaseAttempt(true);
  }
  if (commandWindows_) {
    commandWindows_->Shutdown();
  }
  if (dispatcher_) {
    dispatcher_->Close();
  }
  event.Skip();
}

}  // namespace gc::ui::wx
