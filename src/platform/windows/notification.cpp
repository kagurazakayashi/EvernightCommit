#include "platform/notification.h"

#include <windows.h>

namespace gc::platform {

// Windows 过渡实现：完成通知就是发给主窗口的那条 WM_APP 消息，wParam 携带序号。
// 载荷不进消息，因此窗口销毁后残留的通知只是一串没人认领的序号，不会指向已失效的数据。
// 这里的 reinterpret_cast 是整套界面代码里唯一把 NotificationTarget 当句柄解释的地方。
void PostCompletionNotification(NotificationTarget target, CompletionMessageId message,
                                std::uint64_t serial) {
  if (target == nullptr) {
    return;
  }
  ::PostMessageW(reinterpret_cast<HWND>(target), static_cast<UINT>(message),
                 static_cast<WPARAM>(serial), 0);
}

}  // namespace gc::platform
