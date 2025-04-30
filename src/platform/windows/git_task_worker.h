#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace gc::platform {

// 后台任务的停止信号。程序退出时 WM_DESTROY 要等工作线程收尾：线程在“下一条查询”
// 之前看到这一位被置起，就把剩下的查询按“没有执行”收场，不再逐个启动子进程把各自的
// 超时耗完。共享的是同一份标志：worker 持有并翻牌，请求携带引用，任务体逐条核对。
// 空指针（测试夹具直接调用 Collect*/Load* 的场合）表示“无人喊停”，行为与从前一致。
using StopFlag = std::shared_ptr<std::atomic<bool>>;

[[nodiscard]] inline bool StopRequested(const StopFlag& flag) noexcept {
  return flag != nullptr && flag->load(std::memory_order_relaxed);
}

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
    // 请求类型带 stopFlag 字段时（生产用的探测请求都带）把本 worker 的停止信号挂上去：
    // 任务体在每一条查询之前核对它，退出时的收尾因此只等“当前这一条”，不等剩余的全部。
    if constexpr (requires { request.stopFlag; }) {
      request.stopFlag = stopFlag_;
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

  // 第一阶段：只喊停，不等待。置 stopping_（不再接受新任务）并翻起停止信号
  // （当前任务体在下一条查询前收场），唤醒等待中的线程让它立刻退出。
  void BeginStop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    stopFlag_->store(true, std::memory_order_relaxed);
    condition_.notify_all();
  }

  // 第二阶段：等待工作线程结束。窗口收尾的用法是先对所有 worker 各调一次 BeginStop，
  // 再逐个 Join——各线程同时收场，总等待按“最长的一条在途查询”计，
  // 而不是十个探测超时相加。
  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // 停止并等待工作线程结束（析构与不需要两阶段的场合调用；
  // 最长等当前这一条查询的剩余时间，后续查询会被停止信号短路）。
  void Shutdown() {
    BeginStop();
    Join();
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
  StopFlag stopFlag_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace gc::platform
