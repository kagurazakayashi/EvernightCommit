#pragma once

namespace gc::ui {

// 子控件命令 ID。仅用于 WM_COMMAND / WM_NOTIFY 路由，值本身无外部含义。
enum ControlId : int {
  kIdNone = 0,

  kIdRepoLabel = 101,
  kIdRepoEdit = 102,
  kIdRepoBrowse = 103,
  kIdGitLabel = 104,
  kIdGitEdit = 105,
  kIdGitBrowse = 106,

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

}  // namespace gc::ui
