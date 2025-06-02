#include "platform/notification.h"

namespace gc::platform {

void PostCompletionNotification(NotificationTarget target, CompletionMessageId message,
                                std::uint64_t serial) {
  // Linux 侧目前没有窗口/事件循环：target 与 message 只可能来自尚未迁移的界面。
  // 后台任务的结果仍留在 worker 里，由 MainThreadDispatcher（wx 计时器 Drain）
  // 或测试主动 FetchLatest 取回；这里不伪造任何投递。
  static_cast<void>(target);
  static_cast<void>(message);
  static_cast<void>(serial);
}

}  // namespace gc::platform
