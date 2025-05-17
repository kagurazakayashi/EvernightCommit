#pragma once

#include <windows.h>

#include "platform/windows/command_window_runner.h"  // 只用它的 kCompletionMessage 做一句编译期防撞

namespace gc::ui {

// 子控件命令 ID。仅用于 WM_COMMAND / WM_NOTIFY 路由，值本身无外部含义。
enum ControlId : int {
  kIdNone = 0,

  kIdRepoLabel = 101,
  kIdRepoEdit = 102,
  kIdRepoBrowse = 103,
  kIdGitLabel = 104,
  kIdGitCombo = 105,
  kIdGitBrowse = 106,

  kIdRepoTypeLabel = 107,
  kIdBranchLabel = 110,
  kIdUpstreamLabel = 111,
  kIdTaskLabel = 112,
  kIdAppInfoLabel = 113,

  kIdFetchButton = 120,
  kIdPullButton = 121,
  kIdStatusButton = 122,

  kIdUnstagedGroup = 130,
  kIdUnstagedList = 131,
  kIdUnstagedHint = 132,
  kIdStagedGroup = 140,
  kIdStagedList = 141,
  kIdStagedHint = 142,
  kIdHistoryGroup = 150,
  kIdHistoryList = 151,
  kIdHistoryHint = 152,

  kIdStageAddButton = 160,
  kIdStageRemoveButton = 161,

  kIdCommitGroup = 170,
  kIdSummaryLabel = 171,
  kIdSummaryEdit = 172,
  kIdDescriptionLabel = 173,
  kIdDescriptionEdit = 174,
  kIdAuthorLabel = 175,
  kIdAuthorEdit = 176,
  kIdCoauthorLabel = 177,
  kIdCoauthorList = 178,
  kIdCoauthorAdd = 179,
  kIdCoauthorRemove = 180,
  kIdCoauthorHint = 181,
  kIdAuthorTimeLabel = 182,
  kIdAuthorDate = 183,
  kIdAuthorClock = 188,
  kIdCommitterTimeLabel = 184,
  kIdCommitterDate = 185,
  kIdCommitterClock = 189,
  kIdTimeSyncCheck = 186,
  kIdTimeZoneLabel = 187,

  kIdRefreshButton = 190,
  kIdCreateCommitButton = 191,
  kIdUndoCommitButton = 192,
  kIdPushButton = 193,
  kIdBottomStatusLabel = 194,
  kIdTimeResetButton = 195,
  // 子模块导航的两个按钮：只对状态列写着「子模块」的那一条起作用，返回的可用性由导航栈决定。
  kIdEnterSubmoduleButton = 196,
  kIdReturnToParentButton = 197,
  // 冲突与暂停流程的三个入口：查看（只读展示现场）、继续（--continue，由 Git 建立提交）、
  // 中止（--abort，会改动工作区）。三个入口共用一台控制器，可用性由现场痕迹决定。
  kIdConflictViewButton = 198,
  kIdConflictContinueButton = 199,
  kIdConflictAbortButton = 200,
  // 持久化（可选功能）：两个开关与一个清除入口。「保存记录」是总开关（最近仓库/Git 程序/
  // 窗口布局/草稿），「保存草稿」只管提交表单草稿；这两个开关的状态连同记录内容都保存在
  // 当前用户应用数据目录的记录文件里，不碰 Git 配置。「清除已存记录」删掉全部用户内容记录。
  kIdPersistRecordsCheck = 201,
  kIdPersistDraftsCheck = 202,
  kIdClearPrefsButton = 203,
  // 操作历史（可选功能）：「记录操作历史」总开关 + 「操作历史…」查看/导出/清除/恢复入口。
  // 与「保存记录」各自独立：历史是只追加、按条保留、含引用级恢复线索的事件流。
  kIdHistoryCheck = 204,
  kIdHistoryBrowseButton = 205,
};

inline constexpr UINT kSplitterDragged = WM_APP + 1;
// Git --version 后台验证完成通知；wParam 为请求序号。
inline constexpr UINT kGitProbeCompleted = WM_APP + 2;
// 仓库识别（只读 Git 查询）后台完成通知；wParam 为请求序号。
inline constexpr UINT kRepoDetectCompleted = WM_APP + 3;
// 外部命令窗口操作的观察通知（轮询进行中的操作状态）；无参数。
inline constexpr UINT kGitOperationTick = WM_APP + 4;
inline constexpr UINT_PTR kGitOperationTimer = 0x4713;
inline constexpr UINT kGitOperationTickMs = 500;
// 工作区状态（git status）后台读取完成通知；wParam 为请求序号。
inline constexpr UINT kWorkspaceStatusCompleted = WM_APP + 5;
// 作者默认身份（git config --get user.name/user.email）后台读取完成通知；wParam 为请求序号。
inline constexpr UINT kAuthorConfigCompleted = WM_APP + 6;
// 撤回最近提交的预检（HEAD/分支/父提交/远端跟踪引用/工作区状态，全部只读）完成通知；wParam 为请求序号。
inline constexpr UINT kUndoProbeCompleted = WM_APP + 7;
// fetch 目标的只读预检（当前分支/分支配置的远端/远端清单，全部只读）完成通知；wParam 为请求序号。
inline constexpr UINT kFetchProbeCompleted = WM_APP + 8;
// pull 的只读预检（分支/HEAD/上游/策略配置/现状，以及抓取后的关系与冲突预演）完成通知；
// wParam 为请求序号。抓取与整合两步本身都在命令窗口里跑，不走这条通知。
inline constexpr UINT kPullProbeCompleted = WM_APP + 9;
// push 的只读预检（分支/HEAD/上游/生效配置/发布 URL/领先落后）完成通知；wParam 为请求序号。
// 推送本身在命令窗口里跑，不走这条通知。
inline constexpr UINT kPushProbeCompleted = WM_APP + 10;
// 推送之后向发布目标核对（只读 ls-remote）的完成通知；wParam 为请求序号。
inline constexpr UINT kPushVerifyCompleted = WM_APP + 11;
// 「创建提交」绑定的那组身份事实（工作区根与 Git 目录／完整分支引用／HEAD 完整对象 ID／
// 索引内容标识／流程痕迹／提交者身份配置）的后台查询完成通知；wParam 为请求序号。
// 确认框之前的预检与点头之后的执行前复核共用这一条，回来给谁用由界面的阶段标记分辨。
inline constexpr UINT kCommitProbeCompleted = WM_APP + 12;
// 「撤回最近提交」点头之后的 HEAD/分支后台复核完成通知；wParam 为请求序号。
// 这一步原来是 GUI 线程上的同步子进程调用，走后台之后窗口不再冻结；没回来之前不发命令。
inline constexpr UINT kUndoRecheckCompleted = WM_APP + 13;
// pull 整合失败后的「现场读取」（流程痕迹／未合并条目／HEAD 位置，全部只读）完成通知；
// wParam 为请求序号。读回来才补完那句结论并请求重读。
inline constexpr UINT kPullAftermathCompleted = WM_APP + 14;
// 「首次推送」向导第一步：仓库里有哪些远端、各自实际发布地址（config --list 与逐远端
// get-url --push --all，全部本地只读）完成通知；wParam 为请求序号。
inline constexpr UINT kFirstPushTargetsCompleted = WM_APP + 15;
// 「首次推送」向导第二步：选定目标之后的只读预检（分支／HEAD／上游还是没有／引用名裁定／
// 逐发布地址那条引用在不在／配置文件落点）完成通知；wParam 为请求序号。
// 确认框之前的预检与点头之后的执行前复核共用这一条，回来给谁用由推送控制器的阶段标记分辨。
// 注意这一步里的 ls-remote 会访问远端（只读），与「不接触任何远端」的本地预检不同。
inline constexpr UINT kFirstPushProbeCompleted = WM_APP + 16;
// 「进入子模块」之前的只读探测（目录存在性 + 那次仓库识别 + 父索引里那条 gitlink）完成通知；
// wParam 为请求序号。导航本身不改父索引、不提交、不访问远端，切换动作在通知回来之后才发起。
inline constexpr UINT kSubmoduleEntryProbeCompleted = WM_APP + 17;
// 「返回父仓库」之后那三份位置的只读核对（父索引 / 父提交 / 子模块 HEAD）完成通知；wParam 为请求序号。
inline constexpr UINT kSubmodulePointerProbeCompleted = WM_APP + 18;
// 「冲突与暂停流程」的现场读取（Git 目录里的痕迹 + 未合并条目 + 当前分支/HEAD，全部只读）
// 完成通知；wParam 为请求序号。「查看」的展示、「继续」与「中止」的预检共用这一条，
// 回来给谁用由冲突控制器自己的阶段标记分辨。命令本身在命令窗口里跑，不走这条通知。
inline constexpr UINT kConflictProbeCompleted = WM_APP + 19;
// 「首次使用」持久化说明的弹出通知：WM_CREATE 里不弹模态框，创建完成后由这条消息补上。
inline constexpr UINT kPersistentConsentNotice = WM_APP + 23;
// 「按记录恢复引用」的只读预检（分支现值／要挪回的对象可达性／有没有流程停着，全部本地只读）
// 完成通知；wParam 为请求序号。命令本身在命令窗口里跑，不走这条通知。
inline constexpr UINT kRestoreProbeCompleted = WM_APP + 24;
// 恢复「点头之后、发命令之前」的分支现值复核完成通知；wParam 为请求序号。走后台，没回来不发命令。
inline constexpr UINT kRestoreRecheckCompleted = WM_APP + 25;
// 注意：WM_APP + 20 归命令窗口执行器的完成通知（platform::CommandWindowRunner::kCompletionMessage），
// 它不在本文件里定义，历史上就差点与这里的序号撞车（撞了会表现为「终态回调把预检通知当成
// 操作完成」这类极难查的错乱）。因此这一段从 19 直接跳到 21，文件末尾有一条 static_assert 兜底。
// 点头之后、发出 --continue/--abort 之前的执行前复核完成通知；wParam 为请求序号。
// 这一核对走后台（GUI 线程绝不同步等子进程）：没回来之前不发命令。
inline constexpr UINT kConflictRecheckCompleted = WM_APP + 21;
// 那条流程命令没做成之后的「现场读取」完成通知；wParam 为请求序号。
// 读回来才补完那句结论并请求重读——与 pull 整合失败那一步同一套做法。
inline constexpr UINT kConflictAftermathCompleted = WM_APP + 22;

// 刷新请求合并：连点“刷新”、切换仓库与操作结束这几路触发共用一个定时器，
// 短时间内的多次请求只跑一轮读取，既不让后台队列无限增长，也不会让列表反复闪。
inline constexpr UINT_PTR kRefreshTimer = 0x4714;
inline constexpr UINT kRefreshDebounceMs = 300;

// 持久化保存的防抖：草稿编辑、开关切换、布局改动都只重排这一个定时器；
// 到点在本线程做一次「拿锁→重读→合并→原子写」的小文件写入（不启动任何子进程）。
inline constexpr UINT_PTR kPrefsSaveTimer = 0x4717;
inline constexpr UINT kPrefsSaveDebounceMs = 1500;

// 操作历史落账的防抖：一次操作终态、逐目标核实追加、开关切换都只重排这一个定时器；
// 到点在本线程做一次「拿锁→重读→按 ID 合并→原子写」的小文件写入（不启动任何子进程）。
inline constexpr UINT_PTR kHistorySaveTimer = 0x4718;
inline constexpr UINT kHistorySaveDebounceMs = 1200;

// “Git 程序”输入防抖：连续键入只在停顿后验证一次。
inline constexpr UINT_PTR kGitVerifyTimer = 0x4711;
inline constexpr UINT kGitVerifyDebounceMs = 500;
inline constexpr unsigned long kGitProbeTimeoutMs = 3000;

// “本地仓库”输入防抖：连续键入只在停顿后发起一次识别，避免每敲一键启动一批 Git 进程。
inline constexpr UINT_PTR kRepoDetectTimer = 0x4712;
inline constexpr UINT kRepoDetectDebounceMs = 500;
// 识别是纯本地只读查询；放宽到 8 秒以容纳慢盘与大型仓库。
inline constexpr unsigned long kRepoDetectTimeoutMs = 8000;
// 工作区读取同样是本地只读查询，但 --untracked-files=all 要把未跟踪目录展开到每个文件，
// 大型仓库首次读取明显更慢；放宽到 20 秒，超时后界面报告“Git 查询超时”而不是无限等待。
inline constexpr unsigned long kWorkspaceStatusTimeoutMs = 20000;
// 读作者默认身份同样是本地只读查询（而且一次刷新里要问两句），沿用识别的 8 秒放宽值。
inline constexpr unsigned long kAuthorConfigTimeoutMs = 8000;
// 撤回预检同样是本地只读查询，但里面包含最慢的 git status，沿用工作区读取的 20 秒放宽值。
inline constexpr unsigned long kUndoProbeTimeoutMs = 20000;
// 确认框点头之后、启动命令窗口之前的 HEAD/分支复核只有两条毫秒级查询：现在走后台任务，
// 超时即按「复核不过」取消本次执行（宁可不撤，也不对已经变了的 HEAD 盲目动引用），
// 因此用短超时；GUI 线程不等它，冻结问题已不存在。
inline constexpr unsigned long kUndoRecheckTimeoutMs = 3000;
// fetch 目标预检同样是本地只读查询（symbolic-ref / config / remote -v 三条都是毫秒级），
// 沿用识别的 8 秒放宽值以容纳慢盘；超时按「查询失败」展示，绝不自动重试。
inline constexpr unsigned long kFetchProbeTimeoutMs = 8000;
// pull 预检里最慢的一条是 git status（要展开未跟踪目录），与工作区读取同源，
// 因此沿用它的 20 秒放宽值；抓取之后那一趟还要多问关系、带入清单与冲突预演，都在这一次里。
inline constexpr unsigned long kPullProbeTimeoutMs = 20000;
// 确认框点头之后、启动整合命令之前的同步复核只有三条毫秒级查询（分支 / HEAD / 跟踪引用）：
// 超时即按「复核不过」放弃本次执行，因此用短超时、绝不长等。
inline constexpr unsigned long kPullRecheckTimeoutMs = 3000;
// push 预检最慢的一条是 `git config --list`（按 include 叠出全部生效配置），同样是本地只读查询，
// 沿用 20 秒放宽值容纳慢盘与巨型配置；超时按「查询失败」拒绝这次推送，绝不带着猜的目标上线。
inline constexpr unsigned long kPushProbeTimeoutMs = 20000;
// 推送之后向发布目标的那一次核对要走网络（ls-remote），凭据由 Git 自己的认证方式处理：
// 给 30 秒，超时只说明「这一处没能核实」，不据此断言推送失败，也不自动重试。
// 程序退出时这一步有整体退出策略：worker 的停止信号让剩余目标不再发起 ls-remote，
// WM_DESTROY 的等待因此只按「当前在途的那一条」计，而不是全部目标逐个把超时排完。
inline constexpr unsigned long kPushVerifyTimeoutMs = 30000;
// 「首次推送」向导第一步全是本地只读查询（config --list 与逐远端 get-url），
// 沿用推送预检的 20 秒放宽值；问不成的远端逐个带着原因交回界面，不自动重试。
inline constexpr unsigned long kFirstPushTargetsTimeoutMs = 20000;
// 「首次推送」向导第二步里含一条要访问远端的只读 ls-remote，与推送后的核实同一档：
// 30 秒。超时只落成「对端现状没能问出来」，不据此断言任何一边被改过，也不自动重问。
inline constexpr unsigned long kFirstPushProbeTimeoutMs = 30000;
// 子模块导航的探测沿用仓库识别那一档（识别本身最慢的一条是几条毫秒级只读查询，
// 放宽到 8 秒容纳慢盘与大型仓库）。两条都不联网：目录属性、识别、ls-files / rev-parse 都是本地。
inline constexpr unsigned long kSubmoduleProbeTimeoutMs = 8000;
// 「创建提交」的身份预检里最慢的一条是 git write-tree（大仓库要现算若干棵树），量级与 git status
// 相当，沿用工作区读取的 20 秒放宽值。超时一律按「这份事实没读回来」放弃这次提交：确认框不弹、
// 命令不发，绝不退回用早前那一份事实继续，也不自动重试。
inline constexpr unsigned long kCommitProbeTimeoutMs = 20000;
// 「冲突与暂停流程」的现场读取里最慢的一条是 `git diff --name-only -z --diff-filter=U`
// （大仓库要扫完整棵树），与 git status 同一量级，沿用工作区读取的 20 秒放宽值。
// 「点头之后、发命令之前」的复核原样重发同一组查询，因此用同一个超时——两面对得上，
// 才允许把「复核不过」解释成「现场变了」而不是「这一轮没读完」。超时一律按「这份现场没读回来」
// 放弃这次执行：确认框不重弹、命令不发，也绝不退回用早前那一份事实继续。
inline constexpr unsigned long kConflictProbeTimeoutMs = 20000;

// 「按记录恢复引用」的预检与复核都只有两条毫秒级本地只读查询（rev-parse --verify --quiet），
// 沿用撤回复核那一档短超时：超时即按「读不回来 → 不发命令」收场，绝不对已变的现状盲目动引用。
inline constexpr unsigned long kRestoreProbeTimeoutMs = 3000;
inline constexpr unsigned long kRestoreRecheckTimeoutMs = 3000;

// 编译期防撞：本文件里定义的每一条完成通知都不许等于执行器那条完成消息。
// 新增通知时先核对这一段，别指望 switch 的重复 case 一定会报错（两条消息分属两个函数时就漏了）。
static_assert(kSplitterDragged != platform::CommandWindowRunner::kCompletionMessage);
static_assert(kConflictProbeCompleted != platform::CommandWindowRunner::kCompletionMessage);
static_assert(kConflictRecheckCompleted != platform::CommandWindowRunner::kCompletionMessage);
static_assert(kConflictAftermathCompleted != platform::CommandWindowRunner::kCompletionMessage);
static_assert(kPersistentConsentNotice != platform::CommandWindowRunner::kCompletionMessage);
static_assert(kRestoreProbeCompleted != platform::CommandWindowRunner::kCompletionMessage);
static_assert(kRestoreRecheckCompleted != platform::CommandWindowRunner::kCompletionMessage);

}  // namespace gc::ui
