#pragma once

#include <wx/timer.h>

#include <functional>
#include <memory>
#include <utility>

#include "platform/main_thread_dispatch.h"

namespace gc::ui::wx {

// 把后台完成通知从线程安全队列搬进 wxWidgets 主线程的计时器。
//
// 用法（wxApp 内）：
//   dispatcher_ = platform::MakeQueuedMainThreadDispatcher();
//   pump_ = std::make_unique<CompletionPump>(dispatcher_, [this](const Notice& n) { ... });
//   pump_->Start(25);  // 25ms 轮询；只在有通知时执行回调
//
// 关闭顺序：先对所有 GitTaskWorker 调 BeginStop，再逐个 Join，最后 dispatcher_->Close()
// 并 Stop() 计时器。队列里只有 sourceId/serial 两个整数，不会回调已销毁的 worker 或控件。
class CompletionPump final : public wxTimer {
public:
  using Notice = platform::MainThreadDispatcher::Notice;
  using Handler = std::function<void(const Notice&)>;

  CompletionPump(std::shared_ptr<platform::MainThreadDispatcher> dispatcher, Handler handler)
      : dispatcher_(std::move(dispatcher)), handler_(std::move(handler)) {}

  void Notify() override {
    if (!dispatcher_ || !handler_) {
      return;
    }
    for (const Notice& notice : dispatcher_->Drain()) {
      handler_(notice);
    }
    if (dispatcher_->Closed()) {
      Stop();
    }
  }

private:
  std::shared_ptr<platform::MainThreadDispatcher> dispatcher_;
  Handler handler_;
};

}  // namespace gc::ui::wx
