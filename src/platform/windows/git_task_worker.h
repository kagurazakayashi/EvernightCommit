#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace gc::platform {

// 后台一次性任务控制器：任务在工作线程执行，GUI 线程不等待、不冻结。
// 每次提交携带递增序号，只有仍属于“最新一次提交”的结果会被 FetchLatest 取回，
// 较慢完成的旧结果自动作废，不会覆盖用户之后的选择。
// body 只在工作线程调用，结果也只在锁保护下写入；UI 线程收到通知后再读取，
// 完成通知通过 PostMessageW 发出（wParam=序号），不直接触碰任何控件。
template <typename TaskRequest, typename TaskResult>
class GitTaskWorker {
public:
  using Body = std::function<TaskResult(const TaskRequest&)>;

  GitTaskWorker() = default;
  GitTaskWorker(const GitTaskWorker&) = delete;
  GitTaskWorker& operator=(const GitTaskWorker&) = delete;
  ~GitTaskWorker() { Shutdown(); }

  // 提交一次任务；工作线程惰性启动。
  void Request(HWND notifyWindow, UINT completionMessage, TaskRequest request, Body body) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !body) {
      return;  // 窗口已销毁，不再接受新任务。
    }
    ++submittedSerial_;
    pending_ = PendingTask{std::move(request), std::move(body), notifyWindow, completionMessage,
                           submittedSerial_};
    if (!thread_.joinable()) {
      thread_ = std::thread([this] { Loop(); });
    }
    condition_.notify_all();
  }

  // UI 线程在收到通知后调用：仅当 completionSerial 仍是最新提交且结果未被消费时返回 true。
  bool FetchLatest(uint64_t completionSerial, TaskResult* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasResult_ || completionSerial != submittedSerial_ || completionSerial != completedSerial_ ||
        consumed_) {
      return false;
    }
    if (out != nullptr) {
      *out = result_;
    }
    consumed_ = true;
    return true;
  }

  // 停止并等待工作线程结束（WM_DESTROY 调用；最长阻塞一次任务的剩余超时时间）。
  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  struct PendingTask {
    TaskRequest request;
    Body body;
    HWND window = nullptr;
    UINT message = 0;
    uint64_t serial = 0;
  };

  void Loop() {
    for (;;) {
      TaskRequest request;
      Body body;
      HWND window = nullptr;
      UINT message = 0;
      uint64_t serial = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
          return stopping_ || (pending_.has_value() && pending_->serial > processedSerial_);
        });
        if (stopping_) {
          return;
        }
        request = pending_->request;
        body = pending_->body;
        window = pending_->window;
        message = pending_->message;
        serial = pending_->serial;
        processedSerial_ = serial;
      }

      TaskResult produced = body(request);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(produced);
        hasResult_ = true;
        completedSerial_ = serial;
        consumed_ = false;
      }
      ::PostMessageW(window, message, static_cast<WPARAM>(serial), 0);
    }
  }

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<PendingTask> pending_;
  uint64_t submittedSerial_ = 0;
  uint64_t processedSerial_ = 0;
  uint64_t completedSerial_ = 0;
  TaskResult result_{};
  bool hasResult_ = false;
  bool consumed_ = true;
  bool stopping_ = false;
};

}  // namespace gc::platform
