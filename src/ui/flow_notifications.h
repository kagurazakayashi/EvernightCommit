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

// 「创建提交」的身份预检里最慢的一条是 git write-tree；超时一律按「这份事实没读回来」
// 放弃这次提交，绝不退回用早前那一份继续。
inline constexpr unsigned long kCommitProbeQueryTimeoutMs = 20000;

}  // namespace gc::ui
