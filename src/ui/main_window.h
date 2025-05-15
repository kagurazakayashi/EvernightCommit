#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.h"
#include "app/commit_form_session.h"
#include "app/operation_gate.h"
#include "app/task_coordinator.h"
#include "git/commit_message.h"
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
#include "ui/commit_flow.h"
#include "ui/commit_form.h"
#include "ui/conflict_flow.h"
#include "ui/controls.h"
#include "ui/fetch_flow.h"
#include "ui/layout.h"
#include "ui/operation_host.h"
#include "ui/pull_flow.h"
#include "ui/push_flow.h"
#include "ui/repo_bar.h"
#include "ui/splitter.h"
#include "ui/submodule_flow.h"
#include "ui/undo_flow.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

inline constexpr const wchar_t* kMainWindowWindowClass = L"EvernightCommit.MainWindow";
inline constexpr const wchar_t* kWindowTitle = L"Git 提交工具";

// 主窗口：窗口过程分发、子面板装配与布局、基础设施读取（Git 验证/仓库识别/工作区/作者身份）、
// 命令窗口执行器；六个被编排的 Git 操作（创建提交/撤回/fetch/pull/推送/冲突与暂停流程）各由一个操作控制器
// 负责编排，本类通过 OperationHost 接口只提供「呈现确认与结果、启动命令窗口、请求刷新」这些
// 服务，不再在成员里保存这些操作的中间状态。
class MainWindow : private CommitOperationHost {
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
  // status 按钮、“双击查看差异”与六个操作控制器的启动共用同一条路径，入口不同行为完全一致。
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
  // （判定规则与文案在 app/operation_gate；暂存/查看类只走这一层，
  //   六个被编排的操作另有 AdmitGitFlow 的流程互斥裁决。）
  [[nodiscard]] bool RequireWritePrerequisites(HWND window, std::wstring_view actionLabel);
  // 被编排操作的统一入口裁决：共同前提 + 「谁正走在自己的流程里」。拒绝时写好状态栏并返回 false。
  [[nodiscard]] bool AdmitGitFlow(HWND window, app::GitFlow requested, std::wstring_view actionLabel);
  // 把界面上的可变状态在这一刻拷成只读快照，交给操作控制器；控制器全程只认这份快照。
  [[nodiscard]] OperationContext CaptureOperationContext() const;
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

  // ---- 六个被编排操作的按钮入口 ----
  // 每个入口只做三件事：准入裁决（app/operation_gate）→ 采集只读快照 → 把流程交给对应控制器。
  // 之后的预检、确认、复核、启动、完成、验证与清理都在各控制器的文件里。
  void CreateCommit(HWND window);
  void UndoLastCommit(HWND window);
  void RequestFetch(HWND window);
  void RequestPull(HWND window);
  void RequestPush(HWND window);

  // ---- 冲突与暂停流程的三个入口 ----
  // 三个入口共用一台控制器（ui/conflict_flow）与同一份现场读取：
  // 「查看」只读地把现场展示出来；「继续」与「中止」各自规划、预检、确认、复核之后
  // 才把那一条 Git 命令交进命令窗口。没有流程时不生成 --abort，还有未合并文件时不生成 --continue。
  void ShowConflictState(HWND window);
  void ContinueConflictFlow(HWND window);
  void AbortConflictFlow(HWND window);

  // ---- 子模块导航的两个入口 ----
  // 两个入口都做同一套准入（app/DescribeNavigationRefusal：导航不写任何东西，但它会把界面
  // 绑定的仓库整个换掉，因此在途的预检/复核/命令窗口/核实都要先结束），然后交给
  // ui/submodule_flow 那台控制器：进入之前的只读核对、代管父仓库草稿、返回之后核那三份位置。
  void EnterSubmodule(HWND window);
  void ReturnToParent(HWND window);
  // 「恰好选中一条子模块记录」的核对：按钮可用性与点击处理共用这一份判定，
  // 两处各判一次就会出现「按钮开着而点下去说不能进」或反过来。refusal 给完整说法。
  [[nodiscard]] bool CapturedSubmoduleSelection(git::ChangeItem* item,
                                                std::wstring* refusal) const;

  // 时间控件的联动与说明：勾选同步时提交者跟着作者、提交者那两块置灰，
  // 并把「所选作者时间实际生效的 UTC 偏移」写进本机时区那句说明里。
  void RefreshTimeControlsState(HWND window);
  // 恢复当前时间：控件回到此刻、清掉「用户改过时间」的记号（这是那条明确的退路）。
  void ResetCommitTimesToNow(HWND window);

  // ---- OperationHost / CommitOperationHost：控制器对界面唯一的接触面 ----
  // 每个方法都只做「呈现给定的内容」或「对整界面做一次重画」，不返回任何可变引用；
  // 控制器需要的数据一律经 CaptureOperationContext 的快照传入，不从这里回头读窗口状态。
  void SetStatus(std::wstring note) override;
  void SetFormNote(std::wstring note) override;
  void RefreshUi() override;
  bool Confirm(const std::wstring& title, const std::wstring& body, bool warningIcon) override;
  void ShowInfo(const std::wstring& title, const std::wstring& body) override;
  void ShowWarning(const std::wstring& title, const std::wstring& body) override;
  bool RiskConfirm(const std::wstring& title, const std::wstring& mainInstruction,
                   const std::wstring& yesButton, const std::wstring& body) override;
  std::optional<size_t> PromptRemoteChoice(platform::RemoteChoiceSpec spec,
                                           const RemoteChoiceLayoutHints& hints) override;
  // 单行输入框：与「合作者」那一套模态框同一份实现，几何同样按本窗口的 DPI 度量填。
  // 首次推送向导用它问目标分支名（框内的 validate 只做纯形态提示，权威裁定在后台问 Git）。
  std::optional<std::wstring> PromptForText(platform::IdentityPromptSpec spec,
                                            const TextInputLayoutHints& hints) override;
  bool LaunchCommandWindow(const git::CommandWindowOperation& operation,
                           const CommandLaunchOptions& options) override;
  void ScheduleRefresh() override;
  void RememberOperationConclusion(std::wstring_view conclusion) override;
  // 导航用的仓库切换：把「本地仓库」那一栏改到给出的目录，然后走既有的那条识别链路
  // （后台识别 → 绑定 → 作废旧列表 → 重读 → 作者默认值重查）。不另开一套换绑逻辑。
  bool NavigateRepository(std::wstring_view directory, std::wstring_view statusNote) override;
  [[nodiscard]] CommitFormSnapshot CaptureCommitForm() const override;
  [[nodiscard]] bool CommitTimesUserEdited() const override;
  void ApplyDefaultTimesToNow() override;
  void RunFormValidation(std::wstring_view prefixNote) override;
  void ApplyCommittedFormCleanup(const app::CommittedFormCleanup& cleanup) override;
  // 子模块导航的表单代管：界面上有没有用户写的字 / 收起 / 交还（键号与默认值记号都在这一层处理）。
  [[nodiscard]] bool CommitFormHasUserContent() const override;
  void ClearFormForNavigation(std::wstring_view note) override;
  void ApplyHeldFormForNavigation(const app::HeldForm& held, std::wstring_view note) override;

  // 窗口收尾的两阶段：先对所有后台 worker 喊停（在途查询跑完后剩余查询被停止信号短路），
  // 再逐个 Join 等待线程真正退出。总等待按「最长的一条在途查询」计，
  // 不是把每个探测的超时相加；Join 完成前不释放任何还被线程引用的对象。
  void BeginStopAllBackgroundWorkers();
  void JoinAllBackgroundWorkers();

  // 一次在途的外部命令窗口操作：协调器保管“同时只许一个”的规则与结论，
  // 这里只保存它与执行器操作 ID 的对应关系（通知里只带执行器 ID）。
  struct ActiveOperation {
    unsigned long long serial = 0;  // 协调器序号；0 表示没有在途操作
    unsigned long long runnerId = 0;
    std::wstring displayName;
    // 这次查看的范围说明（子模块只给指针差异、二进制不输出内容、大文件只给摘要上限）。
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
    // 这次「创建提交」提交的那一份表单内容：收尾按它逐栏比对，屏幕上已经换成别的内容时不清。
    git::CommitFormData committedForm;
    // 这次是「撤回最近提交」：成功时把恢复线索拼进结果说明（不自动恢复、不删 reflog）。
    bool undoOperation = false;
    std::wstring restoreHint;
    // 这次是「fetch」：终态说明里追加范围承诺；无论成败都不自动重试（失败原因在命令窗口里）。
    bool fetchOperation = false;
    // 这次是「pull 的获取阶段」：终态不是终点——成功要继续问本地与远端的关系，失败就此为止。
    bool pullFetchOperation = false;
    // 这次是「pull 的整合阶段」：非 0 退出时要把现场交回 pull 控制器做后台读取与结案。
    bool pullIntegrateOperation = false;
    // 这次是「推送」：终态之后要把结论交回 push 控制器发起对发布目标的核实。
    bool pushOperation = false;
    // 这次是「首次推送之后的上游写入」第几条 git config（0 = 不是这一步）。
    // 终态交回 push 控制器：成功才发下一条，失败就把「推送已成的那部分」和「配置只写了一半」
    // 分开说完——两条各自有退出码，绝不合并成一句成功。
    int upstreamWriteStep = 0;
    // 这次是「冲突流程 继续」或「冲突流程 中止」（两者互斥）。终态一律交回 conflictFlow_：
    // 成功只追加一句按退出码说话的范围承诺；没做成（含「结果未知」）时把现场读取与结案
    // 交给那台控制器，与 pull 整合失败同一套做法——绝不把「Git 返回非 0」与「没拿到退出码」合并。
    bool conflictContinueOperation = false;
    bool conflictAbortOperation = false;
  };

  platform::UniqueWindow window_;
  platform::GitVerifyWorker gitWorker_;
  platform::RepoDetectWorker repoWorker_;
  platform::WorkspaceStatusWorker workspaceWorker_;
  platform::AuthorConfigWorker authorWorker_;
  // 六个被编排操作的控制器：各自的后台预检器、阶段标记、方案与复核基准都由控制器自己保管。
  CommitFlow commitFlow_;
  UndoFlow undoFlow_;
  FetchFlow fetchFlow_;
  PullFlow pullFlow_;
  PushFlow pushFlow_;
  // 冲突与暂停流程：现场读取、判读成的方案、复核基准与「没做完时把现场读回来补结论」
  // 都在这台控制器里（三个入口共用它，因此同一时刻只有一条在走自己的阶段）。
  ConflictFlow conflictFlow_;
  // 子模块导航：进来路、代管各仓库的表单草稿、两条只读探测都在这台控制器里。
  SubmoduleFlow submoduleFlow_;
  platform::CommandWindowRunner commandRunner_;
  app::TaskCoordinator tasks_;
  ActiveOperation activeOperation_;
  app::AppState state_;
  app::CommitFormSession formSession_;
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
