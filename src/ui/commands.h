#pragma once

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

// 刷新请求合并：连点“刷新”、切换仓库与操作结束这几路触发共用一个定时器，
// 短时间内的多次请求只跑一轮读取，既不让后台队列无限增长，也不会让列表反复闪。
inline constexpr UINT_PTR kRefreshTimer = 0x4714;
inline constexpr UINT kRefreshDebounceMs = 300;

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

}  // namespace gc::ui
