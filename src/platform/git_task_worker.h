#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "platform/main_thread_dispatch.h"
#include "platform/notification.h"
#include "platform/process.h"

namespace gc::platform {

// ---------- 后台任务与任务代次（平台无关） ----------
//
// 本文件原来在 src/platform/windows/ 下，唯一与 Win32 有关的一行是 PostMessageW。
// 「序号作废 + 工作线程 + 两阶段停止」这套代次规则与操作系统无关，因此整份提到共用层：
// 投递方式改为调用 platform::PostCompletionNotification，由当前平台的实现负责
// （Windows 用 WM_APP，后续 wxWidgets 用自定义事件），业务语义一个字没变。

// 后台任务的停止信号定义已移到 platform/process.h：worker 与执行器共享同一个类型，
// 请求携带它时，当前后台子进程会被终止，后续查询不再发起；交互式 Git 不使用它。

// 后台一次性任务控制器：任务在工作线程执行，界面线程不等待、不冻结。
// 每次提交携带递增序号，只有仍属于“最新一次提交”的结果会被 FetchLatest 取回，
// 较慢完成的旧结果自动作废，不会覆盖用户之后的选择。
// body 只在工作线程调用，结果也只在锁保护下写入；界面线程收到通知后再读取，
// 完成通知按 (投递目标, 通道号, 序号) 发出，不直接触碰任何控件。
template <typename TaskRequest, typename TaskResult>
class GitTaskWorker {
public:
  using Body = std::function<TaskResult(const TaskRequest&)>;

  GitTaskWorker() = default;
  GitTaskWorker(const GitTaskWorker&) = delete;
  GitTaskWorker& operator=(const GitTaskWorker&) = delete;
  ~GitTaskWorker() { Shutdown(); }

  // 提交一次任务；工作线程惰性启动。
  void Request(NotificationTarget notifyTarget, CompletionMessageId completionMessage,
               TaskRequest request, Body body) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !body) {
      return;  // 界面已结束，不再接受新任务。
    }
    // 请求类型带 stopFlag 字段时（生产用的探测请求都带）把本 worker 的停止信号挂上去：
    // 任务体在每一条查询之前核对它，退出时的收尾因此只等“当前这一条”，不等剩余的全部。
    if constexpr (requires { request.stopFlag; }) {
      request.stopFlag = stopFlag_;
    }
    ++submittedSerial_;
    pending_ = PendingTask{std::move(request), std::move(body), notifyTarget, completionMessage,
                           submittedSerial_};
    if (!thread_.joinable()) {
      thread_ = std::thread([this] { Loop(); });
    }
    condition_.notify_all();
  }

  // 把完成通知切到主线程调度器。sourceId 在同一界面生命周期内唯一，Drain 后据此找到本 worker。
  // 未设置时保持原有 PostCompletionNotification 投递（Win32 过渡界面）。
  void SetCompletionDispatcher(std::shared_ptr<MainThreadDispatcher> dispatcher, uint64_t sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = std::move(dispatcher);
    dispatcherSourceId_ = sourceId;
  }

  // 界面线程在收到通知后调用：仅当 completionSerial 仍是最新提交且结果未被消费时返回 true。
  // 已经开始收尾（BeginStop）后不再交付任何结果：关闭期间完成的任务随窗口一起作废。
  bool FetchLatest(uint64_t completionSerial, TaskResult* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !hasResult_ || completionSerial != submittedSerial_ ||
        completionSerial != completedSerial_ || consumed_) {
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

  // 第二阶段：等待工作线程结束。界面收尾的用法是先对所有 worker 各调一次 BeginStop，
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
    NotificationTarget target = nullptr;
    CompletionMessageId message = 0;
    uint64_t serial = 0;
  };

  void Loop() {
    for (;;) {
      TaskRequest request;
      Body body;
      NotificationTarget target = nullptr;
      CompletionMessageId message = 0;
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
        target = pending_->target;
        message = pending_->message;
        serial = pending_->serial;
        processedSerial_ = serial;
      }

      TaskResult produced = body(request);
      std::shared_ptr<MainThreadDispatcher> dispatcher;
      uint64_t dispatcherSourceId = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(produced);
        hasResult_ = true;
        completedSerial_ = serial;
        consumed_ = false;
        dispatcher = dispatcher_;
        dispatcherSourceId = dispatcherSourceId_;
      }
      // 通知只携带整数编号；即使结果已完成时界面正在关闭，也不会调用任何已失效对象。
      if (dispatcher) {
        static_cast<void>(
            dispatcher->Post(MainThreadDispatcher::Notice{dispatcherSourceId, serial}));
      } else {
        PostCompletionNotification(target, message, serial);
      }
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
  std::shared_ptr<MainThreadDispatcher> dispatcher_;
  uint64_t dispatcherSourceId_ = 0;
  StopFlag stopFlag_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace gc::platform
