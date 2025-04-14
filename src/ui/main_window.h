#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.h"
#include "app/commit_form_session.h"
#include "app/task_coordinator.h"
#include "git/commit_message.h"
#include "git/commit_plan.h"
#include "git/staging_plan.h"
#include "platform/windows/author_config.h"
#include "platform/windows/command_window_runner.h"
#include "platform/windows/git_verify_worker.h"
#include "platform/windows/identity_prompt.h"
#include "platform/windows/raii.h"
#include "platform/windows/repo_detect.h"
#include "platform/windows/workspace_status.h"
#include "ui/action_bar.h"
#include "ui/changes_pane.h"
#include "ui/commit_form.h"
#include "ui/controls.h"
#include "ui/layout.h"
#include "ui/repo_bar.h"
#include "ui/splitter.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

inline constexpr const wchar_t* kMainWindowWindowClass = L"EvernightCommit.MainWindow";
inline constexpr const wchar_t* kWindowTitle = L"Git 提交工具";

// 一次命令窗口操作的发起参数。查看类操作（viewKind 有值）与写操作在“退出码算什么”上
// 语义不同，所以这里带的不是文案，而是判定所需的类别。
struct CommandLaunchOptions {
  app::OperationExitPolicy policy = app::OperationExitPolicy::requireZeroExit;
  std::wstring startedNote;   // 启动成功后立刻写进“任务状态”的说明
  std::wstring scopeNotice;   // 随状态一起显示的范围说明（子模块指针/二进制/摘要上限）
  std::optional<git::DiffViewKind> viewKind;
  // 本次操作独占的路径清单临时文件（git add 用）。Git 还在读它的时候绝不能删，
  // 因此所有权随操作一起交给 ActiveOperation，只在拿到终态或启动失败时回收。
  std::wstring pathspecFile;
  // 本次操作独占的提交信息临时文件（git commit -F 用），回收规则与清单文件同一套。
  std::wstring messageFile;
  // 这次是「创建提交」：只有确认它创建成功，界面才清空已提交的标题/描述/合作者。
  bool commitOperation = false;
};

// 主窗口：只做窗口过程分发、子面板装配与布局调用，业务状态留在 app::AppState。
class MainWindow {
public:
  MainWindow() = default;
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  [[nodiscard]] static bool RegisterWindowClass(HINSTANCE instance);
  static void UnregisterWindowClass(HINSTANCE instance);

  [[nodiscard]] bool Create(HINSTANCE instance, int showCommand);
  [[nodiscard]] HWND handle() const noexcept { return window_.get(); }

private:
  static LRESULT CALLBACK Thunk(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

  // 下面这些辅助函数一律使用传入的窗口句柄：WM_CREATE 期间成员 window_ 尚未赋值。
  void OnCreate(HWND window);
  void DoLayout(HWND window);
  void RefreshTexts(HWND window);
  void UpdateCommandAvailability();
  void RegisterTooltips();
  void ApplyFonts(HWND window);
  void UpdateLayoutSpecs(HWND window);
  [[nodiscard]] SIZE MinimumWindowSize(HWND window) const;
  void OnCommand(HWND window, WPARAM wParam);
  void OnGitComboNotify(HWND window, UINT notifyCode);
  void OnRepoEditNotify(HWND window, UINT notifyCode);
  void BrowseRepoPath(HWND window);
  void BrowseGitPath(HWND window);
  void OnSplitterDragged(HWND window, int splitterId, int parentX);

  // Git 可执行文件发现/选择/验证（步骤 2）。
  void InitializeGitDetection(HWND window);
  void CommitGitInput(HWND window);
  void RequestGitVerification(HWND window, const std::wstring& normalizedPath);
  void OnGitProbeCompleted(HWND window, uint64_t completionSerial);

  // 仓库路径选择与识别（步骤 3）。
  void InitializeRepoInput(HWND window);
  void CommitRepoInput(HWND window);
  // 识别有两种来路：换仓库/换 Git 程序（initial，先清空列表）与刷新（refresh，
  // 保留现有列表与摘要，等新结果回来再就地替换，避免整屏闪一下）。
  enum class RepoDetectMode {
    initial,
    refresh,
  };
  void RequestRepoDetection(HWND window, const std::wstring& normalizedPath, RepoDetectMode mode);
  void SetRepoFailed(HWND window, const std::wstring& normalizedPath, gc::git::RepoError error,
                     std::wstring_view detail);
  void OnRepoDetectCompleted(HWND window, uint64_t completionSerial);
  void ResumeRepoDetectionWhenGitReady(HWND window);

  // 工作区状态解析（步骤 6）与刷新调度（步骤 7）：
  // 一次刷新 = 重新读取仓库摘要 + 重新读取两个文件列表与提交历史，全部走内部只读后台查询，不弹命令窗口。
  // 「最近提交」的 git log 读取挂在同一次工作区读取里（platform::LoadWorkspaceStatus 在 status
  // 成功后追问一条 log），界面不另建触发机制。
  void ScheduleRefresh(HWND window);
  void RunRefreshCycle(HWND window);
  void StartWorkspaceRead(HWND window);
  void ClearWorkspace();
  void ApplyWorkspaceLists();
  void OnWorkspaceLoadCompleted(HWND window, uint64_t completionSerial);

  // 外部命令窗口执行器（步骤 5）：用户主动执行的 Git 操作在 cmd 窗口里运行。
  void InitializeCommandWatching(HWND window);
  // 提交一次命令窗口操作：状态登记、槽位占用与失败结案都在这里，
  // status 按钮与“双击查看差异”共用同一条路径，两种入口的行为完全一致。
  [[nodiscard]] bool LaunchCommandWindowOperation(HWND window,
                                                 const git::CommandWindowOperation& operation,
                                                 const CommandLaunchOptions& options);
  void LaunchStatusOperation(HWND window);

  // “加入暂存区 →”：把未暂存列表里选中的条目交给命令窗口里的 git add。
  // “← 移出暂存区”：把已暂存列表里选中的条目从索引撤回（只写索引），两个方向共用下面这套核对与启动流程。
  // 选择范围在点击瞬间按行号核对后拷成快照，之后的刷新/迟到结果都不会改变本次执行的范围。
  void StageSelectedUnstaged(HWND window);
  void UnstageSelectedStaged(HWND window);

  // 一次「按选中条目执行的暂存方向操作」的界面措辞与形态参数：两个方向只差文案与清单文件名前缀，
  // 共同的流程（拒绝说明、确认框、清单写出、命令窗口启动）只实现一遍，避免两处各自漂移。
  struct StagingLaunchWords {
    std::wstring_view objectLabel;        // 「暂存」/「取消暂存」：前提核对与行数不符时的说明用
    std::wstring_view buttonTitle;        // 行核对失败时提醒用户重新点击的按钮名
    std::wstring_view blockedTitle;       // 方案被拒绝时说明框的标题
    std::wstring_view confirmTitle;       // 确认框标题
    std::wstring_view cancelledHint;      // 用户取消后的状态栏说明
    std::wstring_view pathspecFilePrefix; // 清单文件名开头（gc-add / gc-unstage）
    std::wstring_view rangeVerb;          // 「暂存」/「移出暂存区」：启动说明里对本次改动范围的称呼
  };

  // 写操作共同的执行前提核对：Git 与仓库可用、没有别的命令窗口操作在跑、
  // 界面显示的工作区根仍然是协调器绑定的那一个。不通过时写好状态栏并返回 false。
  [[nodiscard]] bool RequireWritePrerequisites(HWND window, std::wstring_view actionLabel);
  // 把选中的行号逐行核对成条目快照：行数、行号对应的条目、路径与状态都要和刚读回来的模型一致，
  // 任何一处对不上就整份拒绝（*refusal 给出要显示的原因，此时返回空）。
  [[nodiscard]] std::vector<git::ChangeItem> CaptureCheckedSelection(
      HWND list, const std::vector<int>& rows, const std::vector<git::ChangeItem>& items, git::ChangeSide side,
      std::wstring_view objectLabel, std::wstring_view buttonTitle, std::wstring* refusal) const;
  // 按方案的传递形态写出清单文件（需要时）并在命令窗口里执行；拒绝与确认框也在这里处理。
  // 返回 false 表示没有跑起来（仓库未被改动，原因已写进状态栏或说明框）。
  [[nodiscard]] bool RunStagingPlan(HWND window, const git::StagingPlan& plan, const StagingLaunchWords& words);

  // 双击“未暂存的更改/已暂存的更改”的某一行：按所在侧与条目类别构造差异/内容查看命令，
  // 仍走上面的命令窗口执行器（不打开外部编辑器，也不在本进程里静默跑 Git）。
  void OnChangesListDoubleClicked(HWND window, HWND list, int row);

  // 双击“最近提交”的某一行：用条目里保存的完整对象 ID 构造 git show，同样走命令窗口执行器。
  // 列表是只读的：这里不构造任何 checkout/reset/revert，查看详情失败也不会动当前分支。
  void OnHistoryCommitDoubleClicked(HWND window, int row);

  void OnCommandWindowCompleted(HWND window, uint64_t operationId);
  void TickActiveOperations(HWND window);
  void StopOperationWatching();
  [[nodiscard]] std::wstring TaskStatusNote() const;
  [[nodiscard]] std::wstring OperationBanner() const;
  // 关闭前确认：仍有操作在命令窗口里执行时，不静默离开。
  bool ConfirmCloseWithActiveOperations(HWND window);

  // 提交表单（标题/描述/作者/合作者）的数据与校验：只做表单，不创建提交。
  // 格式规则一律在 git/commit_identity 与 git/commit_message 里，这里只负责搬运与显示结论。
  void OnCommitFormEdit(HWND window, int controlId);
  // 依当前表单内容重跑一次校验，并把结论写进「任务状态」。
  // prefixNote 是给同一行的前因（例如「仓库配置里的身份没有覆盖你输入的内容」）：
  // 校验结论与前因必须显示在同一行里，否则用户只来得及看到其中一句。
  void RunFormValidation(HWND window, std::wstring_view prefixNote = {});
  void AddCoauthor(HWND window);
  void RemoveSelectedCoauthors(HWND window);
  void EditCoauthorAt(HWND window, int row);
  // 合作者输入框的尺寸与文案：尺寸全部取自主窗口的 DPI 度量，保持与界面其它地方一致。
  [[nodiscard]] platform::IdentityPromptSpec MakeCoauthorPrompt(const std::wstring& title,
                                                                const std::wstring& initial,
                                                                int editingRow) const;
  // 表单内容属于哪一个工作区（按折叠后的工作区根比较，换 Git 程序不算换仓库）。
  [[nodiscard]] std::wstring FormRepositoryKey() const;
  // 切换仓库时对已有表单内容的处理：有用户内容就问「保留还是放弃」，程序不替人猜。
  void ResolveFormOnRepositorySwitch(HWND window, const std::wstring& key);

  // 「作者」初值：向 Git 问这个仓库的有效身份配置（user.name / user.email），
  // 优先序完全交给 Git；本程序只读，绝不写回任何配置。
  void RequestAuthorConfig(HWND window);
  void OnAuthorConfigCompleted(HWND window, uint64_t completionSerial);
  // 编程式改写「作者」输入框：期间抑制 EN_CHANGE，免得把程序填的值记成用户输入。
  void SetAuthorField(HWND window, const std::wstring& text);

  // ---- 创建提交（本步骤）----
  // 点击「创建提交」：先按界面现有的事实做一轮快速拒绝（表单、时间、暂存内容），
  // 通过了才发起一次只读重读，等仓库现状回来之后再把确认框摆到用户面前。
  // 提交前重新核对是必要的：列表可能是几分钟前读的，期间外部终端完全可能改了同一个仓库。
  void CreateCommit(HWND window);
  // 用刚刚读回的仓库现状合成方案、写提交信息文件、给出确认框；确认后才启动命令窗口。
  void ConfirmAndLaunchCommit(HWND window);
  // 放弃这次提交（核对失败、用户取消、启动失败）：删掉刚写的信息文件并把原因写进状态栏。
  void AbandonCommitAttempt(HWND window, std::wstring_view reason, std::wstring_view messageFile);
  // 把一段墙上时间换算成「交给 Git 的值 + 给人看的说明」；失败时写原因并返回 false。
  [[nodiscard]] bool BuildCommitTimeChoice(const git::CivilTime& wall, git::CommitTimeChoice* out,
                                           std::wstring* refusal) const;
  // 时间控件的联动与说明：勾选同步时提交者跟着作者、提交者那两块置灰，
  // 并把「所选作者时间实际生效的 UTC 偏移」写进本机时区那句说明里。
  void RefreshTimeControlsState(HWND window);
  // 恢复当前时间：控件回到此刻、清掉「用户改过时间」的记号（这是那条明确的退路）。
  void ResetCommitTimesToNow(HWND window);
  // 提交创建成功后的表单收尾：清正文、留作者、时间回到此刻。
  void AfterCommitSucceeded(HWND window);
  // 不再等这次重读了（仓库被换掉、核对失败）：丢弃点击瞬间记下的那份摘要。
  void ClearPendingCommitRead() noexcept { pendingCommit_.waitingForRead = false; }

  // 一次在途的外部命令窗口操作：协调器保管“同时只许一个”的规则与结论，
  // 这里只保存它与执行器操作 ID 的对应关系（通知里只带执行器 ID）。
  struct ActiveOperation {
    unsigned long long serial = 0;  // 协调器序号；0 表示没有在途操作
    unsigned long long runnerId = 0;
    std::wstring displayName;
    // 这次查看的范围说明（子模块只给指针差异、二进制不输出内容、大文件只给摘要）。
    // 命令窗口打开期间一直跟着状态一起显示，否则用户只剩一句“执行中”可看。
    std::wstring scopeNotice;
    // 查看类操作的退出码含义由 git::DescribeDiffViewExitCode 解释（协调器只判定成败）。
    std::optional<git::DiffViewKind> viewKind;
    // 本次操作独占的路径清单临时文件（写操作把选中的路径交给它）。
    // 只能在拿到终态之后删除：命令窗口里的 Git 可能还在读它。
    std::wstring pathspecFile;
    // 本次操作独占的提交信息临时文件（git commit -F 的那个文件）。同上。
    std::wstring messageFile;
    // 这次是「创建提交」：终态是成功才做表单收尾（见 OnCommandWindowCompleted）。
    bool commitOperation = false;
  };

  // 一次「等待重读后再确认」的创建提交：点击瞬间把界面摘要留在这儿，
  // 重读回来后与它对比；两者不一致时，确认框必须说明以刚读回的为准（不假装旧状态还成立）。
  struct PendingCommitRead {
    bool waitingForRead = false;
    git::CapturedSnapshot captured;
  };

  platform::UniqueWindow window_;
  platform::GitVerifyWorker gitWorker_;
  platform::RepoDetectWorker repoWorker_;
  platform::WorkspaceStatusWorker workspaceWorker_;
  platform::AuthorConfigWorker authorWorker_;
  platform::CommandWindowRunner commandRunner_;
  app::TaskCoordinator tasks_;
  ActiveOperation activeOperation_;
  app::AppState state_;
  app::CommitFormSession formSession_;
  PendingCommitRead pendingCommit_;
  UiMetrics metrics_;
  std::wstring programInfo_;
  bool suppressRepoEditNotify_ = false;  // 程序改写输入框时不再触发一次识别
  bool suppressCommitFormNotify_ = false;  // 程序改写表单文字时不算作用户编辑
  bool refreshCycleActive_ = false;      // 本次识别/读取属于刷新，不重置列表
  unsigned long long authorReadSerial_ = 0;  // 作者身份查询的序号：迟到的旧结果按它作废

  BandSpec bandSpec_{};
  ChangesSpec changesSpec_{};
  RECT changesArea_{};

  RepoBar repoBar_;
  RepoInfoBar infoBar_;
  ChangesPane changesPane_;
  CommitForm commitForm_;
  ActionBar actionBar_;
  Splitter leftSplitter_;
  Splitter rightSplitter_;
  ToolTips tooltips_;
};

}  // namespace gc::ui
