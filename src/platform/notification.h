#pragma once

#include <cstdint>

namespace gc::platform {

// ---------- 后台完成通知的平台无关契约 ----------
//
// 共用层（操作控制器、各探测 worker）只知道两件事：往哪个目标投递、投到哪一条通道。
// 目标取 `void*`：各平台的原生窗口句柄（Win32 的 `HWND__*`、wxWidgets 的 `wxEvtHandler*`）
// 是指向对象类型的指针，能隐式转进来，而反向必须显式 cast——于是「解释这个地址」的代码
// 只可能出现在平台实现里，共用层拿到的只是一个动不了的地址。nullptr = 不投递
// （测试与「结果由界面主动取回」的场合就这么用）。
using NotificationTarget = void*;

// 完成通道号。Windows 过渡实现把它映射成 WM_APP+<n>，wxWidgets 实现映射成自定义事件类型；
// 通道本身的语义（哪一类后台读取回来了）由界面层的通知表决定，与操作语义无关。
using CompletionMessageId = std::uint32_t;

// 把「serial 号后台任务已完成」投递给 target。由当前平台的通知实现提供
// （Windows：src/platform/windows/notification.cpp 里的 PostMessageW；Linux/wx 侧后续提供同名实现）。
// target 为 nullptr 时什么都不投递——任务结果仍然留在 worker 里，由界面按既有方式取回。
// 注意：这条函数只携带序号，负载一律由 worker 自己保管，因此窗口销毁后残留的
// 通知不会指向已经失效的数据。
void PostCompletionNotification(NotificationTarget target, CompletionMessageId message,
                                std::uint64_t serial);

}  // namespace gc::platform
