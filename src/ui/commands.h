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

// “Git 程序”输入防抖：连续键入只在停顿后验证一次。
inline constexpr UINT_PTR kGitVerifyTimer = 0x4711;
inline constexpr UINT kGitVerifyDebounceMs = 500;
inline constexpr unsigned long kGitProbeTimeoutMs = 3000;

// “本地仓库”输入防抖：连续键入只在停顿后发起一次识别，避免每敲一键启动一批 Git 进程。
inline constexpr UINT_PTR kRepoDetectTimer = 0x4712;
inline constexpr UINT kRepoDetectDebounceMs = 500;
// 识别是纯本地只读查询；放宽到 8 秒以容纳慢盘与大型仓库。
inline constexpr unsigned long kRepoDetectTimeoutMs = 8000;

}  // namespace gc::ui
