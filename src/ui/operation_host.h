#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/commit_form_session.h"
#include "app/submodule_journey.h"  // HeldForm：子模块导航交还表单时要倒回的那一份
#include "app/task_coordinator.h"
#include "git/author_config.h"
#include "git/commit_date.h"
#include "git/commit_message.h"
#include "git/diff_view.h"
#include "git/repository.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/identity_prompt.h"
#include "platform/windows/remote_choice_dialog.h"

namespace gc::ui {

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
  // 这次「创建提交」真正提交的那一份表单内容（标题/描述/合作者）。命令窗口跑的那几分钟里
  // 用户完全可能又打了新东西，收尾时只有「屏幕上还是这一份」的栏目才允许清空。
  git::CommitFormData committedForm;
  // 这次是「撤回最近提交」：成功时把恢复线索（原提交完整 ID）拼进结果说明。
  bool undoOperation = false;
  std::wstring restoreHint;
  // 这次是「fetch」：结论里追加一句范围承诺（只更新了远端跟踪引用；失败时不自动重试）。
  bool fetchOperation = false;
  // 这次是「pull 的第一步：获取」。它的完成不是终态——还要接着做阶段二预检并再问一次才整合，
  // 因此界面要在终态里认出「这一步属于哪一次 pull」。
  bool pullFetchOperation = false;
  // 这次是「pull 的第二步：整合」。非 0 退出时界面要把现场读回来如实说（冲突文件、卡在哪一步）。
  bool pullIntegrateOperation = false;
  // 这次是「推送」：终态之后还要向发布目标做一次只读核对，成功与否以那份实况参与结论。
  bool pushOperation = false;
  // 这次是「首次推送之后的上游写入」：第几条 git config（1 = branch.<分支>.remote，
  // 2 = branch.<分支>.merge）。0 表示不是这一步。终态要交回推送控制器决定下一条与最终结论：
  // 「推送成功」与「配置写成没有」是两件事，各自有退出码，绝不合并成一句「推送并设置上游成功」。
  int upstreamWriteStep = 0;
  // 这次是「冲突流程 继续」（某一种流程的 --continue）或「冲突流程 中止」（对应的 --abort）。
  // 两者都必须把终态交回 ConflictFlow：成功时只追加一句按退出码说话的范围承诺；
  // 没做成时要把现场交回控制器做后台读取与结案（「结果未知」与「Git 返回非 0」不合并成一句话）。
  // 两个位互斥：一次操作只能是其中一种，控制器据此认出该用哪一种措辞补完结论。
  bool conflictContinueOperation = false;
  bool conflictAbortOperation = false;
};

// 决策点一次性取用的仓库与界面只读快照。操作控制器全程只能用这里的值——不允许回头读
// 可变窗口状态：这样「预检回来时界面早已换了仓库」一定能被 detection.root 对比出来，
// 复核的两份事实也都确定属于同一个落点。通知分派时由主窗口现场采集（每个异步事件一份）。
struct OperationContext {
  HWND notifyWindow = nullptr;      // 后台完成通知的投递目标（主窗口）
  std::wstring gitExecutable;       // 已验证可用的 git.exe
  git::RepoDetection detection;     // 仓库识别结果：工作区根、Git 目录、HEAD 状态、分支……
  bool repoUsable = false;          // 此刻仍有可用工作区（识别失败/裸仓库时控制器据此作废旧结果）
  // 「提交者」身份在当前有效配置下的可得性：表单校验拿它当前提（取自界面刚读回的那份，
  // 预检回来之后一律改以预检事实里的提交者结论为准，两者不混用）。
  git::CommitterIdentityState committerState = git::CommitterIdentityState::unknown;
  git::WorkspaceLoadStatus workspaceStatus = git::WorkspaceLoadStatus::unloaded;
  std::wstring workspaceMessage;    // 工作区读取的当前结论（成功摘要或失败原因）
  std::size_t stagedItemCount = 0;  // 界面当前「已暂存的更改」条目数（点击瞬间摘要留档用）
};

// 「要点列表让用户单选」那次弹窗的布局参数（字号、边距这些一律由主窗口按 DPI 度量填，
// 控制器只给出内容与行数）。
struct RemoteChoiceLayoutHints {
  int labelRows = 3;       // 「为什么要在这里选」那句说明留几行
  int contentWidth = 420;  // 对话框内容宽度（DIP）
  int listHeight = 140;    // 列表区高度（DIP）
};

// 「问一个字串」那次弹窗的布局参数，与单选列表框同一套规矩：几何由主窗口按 DPI 填。
struct TextInputLayoutHints {
  int labelRows = 3;       // 输入框上方那句说明的行数
  int noteRows = 2;        // 框内校验说明预留的行数
  int contentWidth = 420;  // 对话框内容宽度（DIP）
};

// 操作控制器需要的界面服务：呈现确认与结果、启动命令窗口操作、请求刷新。
// 由主窗口实现；控制器不持有 HWND，也不得经这里回头读任何可变窗口状态——
// 每个方法要么展示调用方给出的内容，要么对整界面做一次「重画」。
class OperationHost {
public:
  OperationHost() = default;
  OperationHost(const OperationHost&) = delete;
  OperationHost& operator=(const OperationHost&) = delete;
  virtual ~OperationHost() = default;

  // 写「任务状态」并重画（控制器放弃/拒绝/状态推进都走这一句）。
  virtual void SetStatus(std::wstring note) = 0;
  // 写表单自己的说明通道并重画（与「任务状态」分开：一句是仓库怎么样，一句是用户正在做的事）。
  virtual void SetFormNote(std::wstring note) = 0;
  // 按钮可用性与整界面重画（操作槽位、流程状态变化之后）。
  virtual void RefreshUi() = 0;

  // 确认框：true = 用户点「确定」。warningIcon 区分「改动类操作请确认」（警告图标）
  // 与「只读范围请确认」（提示图标）两种既有形态。
  virtual bool Confirm(const std::wstring& title, const std::wstring& body, bool warningIcon) = 0;
  // 只有「确定」的说明框：拒绝、结果解释走这一条。
  virtual void ShowInfo(const std::wstring& title, const std::wstring& body) = 0;
  virtual void ShowWarning(const std::wstring& title, const std::wstring& body) = 0;
  // 风险确认框（TaskDialog 自定义按钮 + 打开失败时的退路）：「强制/仍要」只越过本程序的
  // 风险提示，命令一个字都不加——按钮文字由调用方给出，两种形态都要点明哪个按钮是什么。
  virtual bool RiskConfirm(const std::wstring& title, const std::wstring& mainInstruction,
                           const std::wstring& yesButton, const std::wstring& body) = 0;
  // 单选列表框（fetch 选远端、pull 选整合方式共用）：返回选中下标；取消返回空。
  virtual std::optional<size_t> PromptRemoteChoice(platform::RemoteChoiceSpec spec,
                                                   const RemoteChoiceLayoutHints& hints) = 0;
  // 单行文本输入框（与「合作者」那一套模态输入同一份实现，几何同样由主窗口按 DPI 填）：
  // 确定返回输入值，取消返回空。validate 只在框内做纯形态提示——这里绝不能同步等子进程，
  // 权威裁定由控制器在输入之后于后台问回来（例如首次推送的 git check-ref-format）。
  virtual std::optional<std::wstring> PromptForText(platform::IdentityPromptSpec spec,
                                                    const TextInputLayoutHints& hints) = 0;

  // 占用命令窗口单槽并启动一次操作；false = 没跑起来（原因已被执行路径写进状态栏）。
  virtual bool LaunchCommandWindow(const git::CommandWindowOperation& operation,
                                   const CommandLaunchOptions& options) = 0;
  // 请求一次后台重读（合并去抖由主窗口的刷新调度负责）。
  virtual void ScheduleRefresh() = 0;
  // 把一段结论交给协调器保管：紧随其后的自动刷新会把它和仓库现状并排显示在同一行里。
  virtual void RememberOperationConclusion(std::wstring_view conclusion) = 0;

  // 导航用的仓库切换：把界面绑定的仓库整个换到给出的目录，走的就是既有的那条链路
  // （后台识别 → 绑定 → 作废旧列表 → 重读 → 作者默认值重查），不另开一套。
  // 控制器只决定「去哪儿」与「这句状态怎么写」；换绑定的全部后果由实现承担。
  // 返回 false = 没有切换（原因已由实现写进状态栏）。
  virtual bool NavigateRepository(std::wstring_view directory, std::wstring_view statusNote) = 0;
};

// 「创建提交」额外需要的表单桥：内容重采集与收尾落地都在提交表单控件里，
// 控制器只决定「比对结果与要不要清」，怎么清由主窗口执行（表单会话记号也在那里维护）。
struct CommitFormSnapshot {
  git::CommitFormData data;
  git::CivilTime authorWall{};
  git::CivilTime committerWall{};
  bool timesSynced = false;
};

class CommitOperationHost : public OperationHost {
public:
  // 按屏幕「此刻」的内容重新采集一份（预检回来时表单可能已被用户改过，必须重新核对）。
  [[nodiscard]] virtual CommitFormSnapshot CaptureCommitForm() const = 0;
  // 「用户亲手改过时间」的记号：没人改过时，提交用「提交那一刻」，控件要先回到此刻。
  [[nodiscard]] virtual bool CommitTimesUserEdited() const = 0;
  // 把两个时间控件改回此刻并跟上联动（「默认用提交时的当前时间」那条规则的落地）。
  virtual void ApplyDefaultTimesToNow() = 0;
  // 依当前表单内容重跑一次校验并把结论写进界面（prefixNote 是同行的前因）。
  virtual void RunFormValidation(std::wstring_view prefixNote) = 0;
  // 提交创建成功的收尾：按判定清空对应栏目；resetTimes 时把两个时间回到此刻并跟上联动。
  virtual void ApplyCommittedFormCleanup(const app::CommittedFormCleanup& cleanup) = 0;

  // ---- 子模块导航的表单代管 ----
  // 界面上现在有没有「用户写的东西」。交还代管草稿之前必须先问这一句：
  // 两份内容都可能是要的，程序无权替人取舍。
  [[nodiscard]] virtual bool CommitFormHasUserContent() const = 0;
  // 离开这个仓库：整张表单倒空，并撤掉「这是用户写的」那些记号（内容已由导航收进代管）。
  // note 写进「表单自己的说明」那条通道，让人看得见那些字去了哪里。
  virtual void ClearFormForNavigation(std::wstring_view note) = 0;
  // 回到这个仓库：把代管的那一份原样倒回表单（含两个时间与同步勾选）。
  // 交还的作者栏是用户当时写的字，必须记成用户内容：随后到达的「这个仓库的默认作者」
  // 不能把它悄悄盖掉——那正是身份串用的来路。
  virtual void ApplyHeldFormForNavigation(const app::HeldForm& held, std::wstring_view note) = 0;
};

}  // namespace gc::ui
