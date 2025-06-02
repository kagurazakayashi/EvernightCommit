#include "platform/main_thread_dispatch.h"

#include <mutex>
#include <utility>

namespace gc::platform {
namespace {

class QueuedMainThreadDispatcher final : public MainThreadDispatcher {
public:
  bool Post(Notice notice) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return false;
    }
    queue_.push_back(notice);
    return true;
  }

  std::vector<Notice> Drain() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      queue_.clear();
      return {};
    }
    std::vector<Notice> drained;
    drained.swap(queue_);
    return drained;
  }

  void Close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    queue_.clear();
  }

  [[nodiscard]] bool Closed() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<Notice> queue_;
  bool closed_ = false;
};

}  // namespace

std::shared_ptr<MainThreadDispatcher> MakeQueuedMainThreadDispatcher() {
  return std::make_shared<QueuedMainThreadDispatcher>();
}

}  // namespace gc::platform
