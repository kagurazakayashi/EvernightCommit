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
#include "git/diff_view.h"
#include "git/staging_plan.h"
#include "git/workspace_model.h"
#include "platform/author_config.h"
#include "platform/command_window_service.h"
#include "platform/commit_changes.h"
#include "platform/commit_detail.h"
#include "platform/commit_probe.h"
#include "platform/git_task_worker.h"
#include "platform/git_verify_worker.h"
#include "platform/main_thread_dispatch.h"
#include "platform/operation_history_store.h"
#include "platform/repo_detect.h"
#include "platform/workspace_status.h"
#include "ui/commit_flow.h"
#include "ui/operation_host.h"
#include "ui/restore_flow.h"
#include "ui/undo_flow.h"
#include "ui/wx/changes_panel.h"
#include "ui/wx/commit_form_panel.h"
#include "ui/wx/completion_pump.h"

class wxButton;
class wxCheckBox;
class wxComboBox;
class wxStaticText;
class wxTextCtrl;

namespace gc::ui::wx {

// Windows 与 Linux 共用的 wxWidgets 主窗口（统一界面）。
//
// 与 Win32 过渡实现（ui/main_window.*）的对应关系：仓库栏（目录 + Git 程序组合框 +
// 浏览 + 验证状态）、两页变化区（ui/wx/changes_panel）、后台状态刷新与提交历史读取、
// 提交详情/文件变化带、差异查看（命令窗口里的 git diff / git show）、暂存与创建提交、
// 撤回最近提交、操作历史窗口与按记录恢复。
// 尚未迁移的写操作入口（fetch、pull、推送、冲突处理、子模块导航、持久化开关）
// 在这里以禁用态呈现并说明开发状态，不接空实现。
// 所有业务语义走既有共用代码（git/、app/、platform/ 契约、ui/ 控制器），
// 界面只做搬运与显示，没有第二套业务流程。
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
  void SetGitCandidates(const std::vector<std::wstring>& candidates);

  // ---- 后台任务 ----
  void RequestGitVerification(const std::wstring& path);
  void CommitGitInput();
  void RequestRepoDetection(bool initial);
  void RequestWorkspaceRead();
  void RequestAuthorConfig();
  void RequestCommitChanges(int row);
  void RequestCommitDetail(int row);
  void ReconcileSelectedCommit();
  void ScheduleRefreshSoon();
  void HandleNotice(const platform::MainThreadDispatcher::Notice& notice);
  void OnGitVerificationCompleted(uint64_t serial);
  void OnRepoDetectionCompleted(uint64_t serial);
  void OnWorkspaceReadCompleted(uint64_t serial);
  void OnAuthorConfigCompleted(uint64_t serial);
  void OnCommitProbeCompleted(uint64_t serial);
  void OnCommitChangesCompleted(uint64_t serial);
  void OnCommitDetailCompleted(uint64_t serial);
  void OnUndoProbeCompleted(uint64_t serial);
  void OnUndoRecheckCompleted(uint64_t serial);
  void OnRestoreProbeCompleted(uint64_t serial);
  void OnRestoreRecheckCompleted(uint64_t serial);
  void OnRefreshTimer(wxTimerEvent& event);
  void OnOperationTimer(wxTimerEvent& event);
  void OnGitVerifyDebounce(wxTimerEvent& event);
  void OnRepoDetectDebounce(wxTimerEvent& event);
  void OnHistorySaveTimer(wxTimerEvent& event);
  void FinishActiveOperation(const platform::CommandWindowResult& result);
  [[nodiscard]] OperationContext CaptureOperationContext() const;
  [[nodiscard]] app::GitFlowActivity CurrentFlowActivity() const;
  [[nodiscard]] bool AdmitFlow(app::GitFlow requested, std::wstring_view actionLabel);

  // ---- 操作历史（内存 + 平台存储契约） ----
  void InitializeOperationHistory();
  [[nodiscard]] bool HistoryRecordingEnabled() const;
  [[nodiscard]] std::wstring HistoryStorageLine() const;
  [[nodiscard]] std::wstring NextHistoryId();
  void ScheduleHistorySave();
  void RunHistorySave();
  std::wstring RecordHistory(const app::HistoryCapture& capture,
                             const app::HistoryTerminalInfo& terminal,
                             const std::wstring& existingId, std::wstring* refusal);
  std::wstring BeginInProgressHistory(const app::HistoryCapture& capture, long long startedEpoch,
                                      std::wstring* refusal);
  void MarkHistoryRecordUnknown(const std::wstring& recordId, std::wstring_view why);
  void SettleUnfinishedHistoryOnShutdown();
  void ShowOperationHistory();
  void BeginRestoreFromRecord(const app::OperationRecord& record);
  void CopyHistoryRestoreText(const app::OperationRecord& record);

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

  // ---- 撤回入口 ----
  void UndoLastCommit();

  // ---- 查看入口（命令窗口里的只读 Git） ----
  void OnChangesDoubleClicked(git::ChangeSide side, int row);
  void OnHistoryDoubleClicked(int row);
  void OnFileDoubleClicked(int row);
  void LaunchStatusOperation();
  void CopyCommitDetailToClipboard();
  void OpenTerminalDialog();
  // 三处双击共用的发起前核对：不通过时把原因写进状态栏并返回 false。
  [[nodiscard]] bool RequireViewPrerequisites(std::wstring_view actionLabel);

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
  void StopWorkers();

  // ---- 控件 ----
  wxTextCtrl* repoPath_ = nullptr;
  wxButton* browseRepo_ = nullptr;
  wxComboBox* gitCombo_ = nullptr;
  wxButton* browseGit_ = nullptr;
  wxButton* terminalButton_ = nullptr;
  wxButton* statusButton_ = nullptr;
  wxButton* fetchButton_ = nullptr;
  wxButton* pullButton_ = nullptr;
  wxButton* refreshButton_ = nullptr;
  wxStaticText* gitLabel_ = nullptr;
  wxStaticText* repoSummary_ = nullptr;
  // 尚未迁移的入口（禁用 + 说明开发状态）：
  wxButton* undoCommitButton_ = nullptr;
  wxButton* pushButton_ = nullptr;
  wxButton* conflictViewButton_ = nullptr;
  wxButton* conflictContinueButton_ = nullptr;
  wxButton* conflictAbortButton_ = nullptr;
  wxButton* enterSubmoduleButton_ = nullptr;
  wxButton* returnToParentButton_ = nullptr;
  wxButton* historyBrowseButton_ = nullptr;
  wxCheckBox* persistRecordsCheck_ = nullptr;
  wxCheckBox* persistDraftsCheck_ = nullptr;
  wxCheckBox* historyRecordCheck_ = nullptr;
  wxButton* clearPrefsButton_ = nullptr;
  ChangesPanel* panel_ = nullptr;
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
  platform::CommitChangesWorker commitChangesWorker_;
  platform::CommitDetailWorker commitDetailWorker_;
  ui::CommitFlow commitFlow_;
  ui::UndoFlow undoFlow_;
  ui::RestoreFlow restoreFlow_;
  std::unique_ptr<platform::CommandWindowService> commandWindows_;
  wxTimer refreshTimer_;
  wxTimer operationTimer_;
  wxTimer gitVerifyTimer_;
  wxTimer repoDetectTimer_;
  wxTimer historySaveTimer_;

  bool refreshCycleActive_ = false;
  bool refreshPending_ = false;
  bool closing_ = false;
  bool settingGitCombo_ = false;  // 程序回填组合框时抑制「键入即验证」回环
  unsigned long long commitFilesSerial_ = 0;
  unsigned long long commitDetailSerial_ = 0;
  git::CommitItem pendingFilesCommit_;
  git::CommitItem pendingDetailCommit_;
  unsigned long long activeOperationSerial_ = 0;
  uint64_t activeRunnerId_ = 0;
  std::wstring activePathspecFile_;
  std::wstring activeMessageFile_;
  bool activeCommitOperation_ = false;
  git::CommitFormData activeCommittedForm_;
  std::optional<git::DiffViewKind> activeViewKind_;
  bool activeUndoOperation_ = false;
  std::wstring activeRestoreHint_;
  bool activeRestoreOperation_ = false;
  app::HistoryCapture activeHistoryCapture_;
  long long activeHistoryStartedEpoch_ = 0;
  std::wstring activeHistoryRecordId_;
  bool activeHistoryRecording_ = false;

  // 本窗口的这一份操作历史与写入意图。数据语义在 app/operation_history；
  // 落盘只在 platform::DescribeHistoryStorageAvailability().usable 时尝试，
  // Linux 在步骤 11 前明确只保留本会话内存记录，绝不假装已经保存。
  platform::HistoryStorageAvailability historyStorage_;
  platform::HistoryStorePaths historyPaths_;
  app::OperationLog historyLog_;
  bool historyWritable_ = false;
  bool historySavePending_ = false;
  app::HistoryWriteIntents historyIntents_;
  unsigned long long historyRecordCounter_ = 0;
  std::wstring historyLoadNote_;
};

}  // namespace gc::ui::wx
