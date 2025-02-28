#include "platform/windows/git_verify_worker.h"

#include <utility>

namespace gc::platform {

GitVerifyWorker::~GitVerifyWorker() { Shutdown(); }

void GitVerifyWorker::RequestVerify(HWND notifyWindow, UINT completionMessage, std::wstring exePath,
                                    unsigned long timeoutMilliseconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) {
    return;  // 窗口已销毁，不再接受新任务。
  }
  ++submittedSerial_;
  pending_ = PendingRequest{std::move(exePath), notifyWindow, completionMessage, timeoutMilliseconds,
                            submittedSerial_};
  if (!thread_.joinable()) {
    thread_ = std::thread([this] { Loop(); });
  }
  condition_.notify_all();
}

bool GitVerifyWorker::FetchLatest(uint64_t completionSerial, GitExeVerification* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!completed_ || completionSerial != submittedSerial_ || completionSerial != completed_->first || consumed_) {
    return false;
  }
  if (out != nullptr) {
    *out = completed_->second;
  }
  consumed_ = true;
  return true;
}

void GitVerifyWorker::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void GitVerifyWorker::Loop() {
  for (;;) {
    PendingRequest request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] {
        return stopping_ || (pending_.has_value() && pending_->serial > processedSerial_);
      });
      if (stopping_) {
        return;
      }
      request = *pending_;
      processedSerial_ = request.serial;
    }

    GitExeVerification verification = VerifyGitExe(request.path, request.timeoutMilliseconds);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      completed_ = std::make_pair(request.serial, std::move(verification));
      consumed_ = false;
    }
    ::PostMessageW(request.window, request.message, static_cast<WPARAM>(request.serial), 0);
  }
}

}  // namespace gc::platform
