#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>

#include "app/app_state.h"
#include "app/commit_form_session.h"
#include "app/operation_gate.h"
#include "app/submodule_journey.h"
#include "app/task_coordinator.h"
#include "git/commit_plan.h"
#include "git/staging_plan.h"
#include "platform/author_config.h"
#include "platform/command_window_service.h"
#include "platform/commit_probe.h"
#include "platform/git_task_worker.h"
#include "platform/git_verify_worker.h"
#include "platform/main_thread_dispatch.h"
#include "platform/repo_detect.h"
#include "platform/workspace_status.h"
#include "ui/commit_flow.h"
#include "ui/operation_host.h"
#include "ui/wx/commit_form_panel.h"
#include "ui/wx/completion_pump.h"

class wxButton;
class wxListBox;
class wxStaticText;
class wxTextCtrl;

namespace gc::ui::wx {

// Windows 与 Linux 共用的 wxWidgets 主窗口。
//
// 本步骤只接通「暂存/取消暂存」与「创建提交」：仓库识别、刷新、工作区列表、完整提交表单、
// 后台预检与执行前复核全部复用既有业务层（git/staging_plan、ui/commit_flow、
// platform/*_probe），不重写任何 Git 语义。命令窗口执行器由平台服务提供，
// 界面只轮询终态；交互式 Git 仍在这个程序之外的终端里运行。
class MainFrame final : public wxFrame, private ui::CommitOperationHost {
public:
  MainFrame();
  ~MainFrame() override;

private:
  // ---- 界面装配与状态 ----
  void BuildUi();
  void BindEvents();
  void StartInitialization();
  void UpdateUi();
  void ClearWorkspaceState();
  void RebuildChangesLists();
  void SetRepositorySummary();
  [[nodiscard]] std::vector<int> SelectedRows(const wxListBox& list) const;

  // ---- 后台任务 ----
  void RequestGitVerification(const std::wstring& path);
  void RequestRepoDetection(bool initial);
  void RequestWorkspaceRead();
  void RequestAuthorConfig();
  void ScheduleRefreshSoon();
  void HandleNotice(const platform::MainThreadDispatcher::Notice& notice);
  void OnGitVerificationCompleted(uint64_t serial);
  void OnRepoDetectionCompleted(uint64_t serial);
  void OnWorkspaceReadCompleted(uint64_t serial);
  void OnAuthorConfigCompleted(uint64_t serial);
  void OnCommitProbeCompleted(uint64_t serial);
  void OnRefreshTimer(wxTimerEvent& event);
  void OnOperationTimer(wxTimerEvent& event);
  void FinishActiveOperation(const platform::CommandWindowResult& result);
  [[nodiscard]] OperationContext CaptureOperationContext() const;

  // ---- 仓库绑定与表单默认值 ----
  [[nodiscard]] bool ApplyDetectedRepository(const git::RepoDetection& detection);
  void ApplyAuthorDefaultIfAllowed();
  void HandleAuthorCleared();

  // ---- 暂存入口 ----
  void StageSelected();
  void UnstageSelected();
  void StageAll();
  void UnstageAll();
  void RunStagingPlan(const git::StagingPlan& plan, std::wstring_view objectLabel,
                      std::wstring_view rangeVerb, std::wstring_view pathspecPrefix,
                      std::wstring_view blockedTitle, std::wstring_view confirmTitle);
  [[nodiscard]] bool RequireStagingPrerequisites(std::wstring_view actionLabel);

  // ---- 提交入口 ----
  void CreateCommit();

  // ---- OperationHost / CommitOperationHost ----
  void SetStatus(std::wstring note) override;
  void SetFormNote(std::wstring note) override;
  void RefreshUi() override;
  bool Confirm(const std::wstring& title, const std::wstring& body, bool warningIcon) override;
  void ShowInfo(const std::wstring& title, const std::wstring& body) override;
  void ShowWarning(const std::wstring& title, const std::wstring& body) override;
  bool RiskConfirm(const std::wstring& title, const std::wstring& mainInstruction,
                   const std::wstring& yesButton, const std::wstring& body) override;
  std::optional<size_t> PromptSingleChoice(platform::SingleChoiceRequest request) override;
  std::optional<std::wstring> PromptForText(platform::TextPromptRequest request) override;
  bool LaunchCommandWindow(const git::CommandWindowOperation& operation,
                           const CommandLaunchOptions& options) override;
  void ScheduleRefresh() override { ScheduleRefreshSoon(); }
  void RememberOperationConclusion(std::wstring_view conclusion) override;
  void RecordPushVerification(std::wstring_view verificationSummary) override;
  bool NavigateRepository(std::wstring_view directory, std::wstring_view statusNote) override;

  [[nodiscard]] CommitFormSnapshot CaptureCommitForm() const override;
  [[nodiscard]] bool CommitTimesUserEdited() const override;
  void ApplyDefaultTimesToNow() override;
  void RunFormValidation(std::wstring_view prefixNote) override;
  void ApplyCommittedFormCleanup(const app::CommittedFormCleanup& cleanup) override;
  [[nodiscard]] bool CommitFormHasUserContent() const override;
  void ClearFormForNavigation(std::wstring_view note) override;
  void ApplyHeldFormForNavigation(const app::HeldForm& held, std::wstring_view note) override;

  void OnClose(wxCloseEvent& event);

  // ---- 控件 ----
  wxTextCtrl* repoPath_ = nullptr;
  wxButton* browseRepo_ = nullptr;
  wxButton* refreshButton_ = nullptr;
  wxStaticText* gitLabel_ = nullptr;
  wxStaticText* repoSummary_ = nullptr;
  wxStaticText* unstagedLabel_ = nullptr;
  wxListBox* unstagedList_ = nullptr;
  wxStaticText* unstagedHint_ = nullptr;
  wxListBox* stagedList_ = nullptr;
  wxStaticText* stagedHint_ = nullptr;
  wxButton* stageSelected_ = nullptr;
  wxButton* stageAll_ = nullptr;
  wxButton* unstageSelected_ = nullptr;
  wxButton* unstageAll_ = nullptr;
  wxButton* createCommit_ = nullptr;
  CommitFormPanel* form_ = nullptr;
  wxStaticText* statusText_ = nullptr;
  wxStaticText* formNoteText_ = nullptr;

  // ---- 状态与后台对象 ----
  app::AppState state_;
  app::CommitFormSession formSession_;
  app::TaskCoordinator tasks_;
  std::shared_ptr<platform::MainThreadDispatcher> dispatcher_;
  std::unique_ptr<CompletionPump> completionPump_;
  platform::GitVerifyWorker gitWorker_;
  platform::RepoDetectWorker repoWorker_;
  platform::WorkspaceStatusWorker workspaceWorker_;
  platform::AuthorConfigWorker authorWorker_;
  ui::CommitFlow commitFlow_;
  std::unique_ptr<platform::CommandWindowService> commandWindows_;
  wxTimer refreshTimer_;
  wxTimer operationTimer_;

  bool refreshCycleActive_ = false;
  bool refreshPending_ = false;
  bool closing_ = false;
  std::vector<std::wstring> selectedUnstagedPaths_;
  std::vector<std::wstring> selectedStagedPaths_;
  unsigned long long activeOperationSerial_ = 0;
  uint64_t activeRunnerId_ = 0;
  std::wstring activePathspecFile_;
  std::wstring activeMessageFile_;
  bool activeCommitOperation_ = false;
  git::CommitFormData activeCommittedForm_;
};

}  // namespace gc::ui::wx
