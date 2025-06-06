#pragma once

#include <cstdint>

#include "platform/notification.h"

namespace gc::ui {

// 共用界面控制器的完成通道与超时取值（不含任何 Win32 类型）。
//
// Win32 过渡界面仍按 WM_APP+n 投递；统一 wxWidgets 界面走 MainThreadDispatcher，
// 通道号只作为 GitTaskWorker::Request 的必填参数出现。两边的编号保持一致，
// 这样过渡界面与 wx 界面可以在同一份控制器代码上对照行为。
inline constexpr platform::CompletionMessageId kCommitProbeCompletionId = 0x800C;  // WM_APP + 12
// 撤回最近提交：点击之后的只读预检，以及点头之后、发命令之前的 HEAD/分支复核。
inline constexpr platform::CompletionMessageId kUndoProbeCompletionId = 0x8007;   // WM_APP + 7
inline constexpr platform::CompletionMessageId kUndoRecheckCompletionId = 0x800D;  // WM_APP + 13
// 按记录恢复：选中记录之后的只读预检，以及点头之后、发命令之前的现场复核。
inline constexpr platform::CompletionMessageId kRestoreProbeCompletionId = 0x8018;   // WM_APP + 24
inline constexpr platform::CompletionMessageId kRestoreRecheckCompletionId = 0x8019;  // WM_APP + 25

// 「创建提交」的身份预检里最慢的一条是 git write-tree；超时一律按「这份事实没读回来」
// 放弃这次提交，绝不退回用早前那一份继续。
inline constexpr unsigned long kCommitProbeQueryTimeoutMs = 20000;
// 撤回预检里最慢的一条同样是 git status，沿用工作区读取的 20 秒。
inline constexpr unsigned long kUndoProbeQueryTimeoutMs = 20000;
// 确认之后、发命令之前的复核只有毫秒级查询：超时即按「复核不过」放弃，宁可不撤。
inline constexpr unsigned long kUndoRecheckQueryTimeoutMs = 3000;
// 按记录恢复的预检与复核都只有毫秒级本地查询，沿用短超时：
// 超时即按「读不回来 → 不发命令」收场，绝不对已变的现状盲目动引用。
inline constexpr unsigned long kRestoreProbeQueryTimeoutMs = 3000;
inline constexpr unsigned long kRestoreRecheckQueryTimeoutMs = 3000;

}  // namespace gc::ui
