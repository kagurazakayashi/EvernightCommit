#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace gc::platform {

// 后台任务完成通知的主线程调度边界。
//
// 工作线程只把「哪个任务源的第几号提交完成了」放进线程安全队列，不直接触碰任何控件，
// 也不持有业务对象。界面线程（Win32 消息循环或 wxWidgets 事件/计时器）在可以安全更新
// 界面的时刻调用 Drain()，再按 sourceId 找到对应 worker、用 serial 调 FetchLatest。
//
// 这样做的两个直接后果：
//   * 关闭顺序可证明：先对所有 worker BeginStop，再 Join，最后 Close 调度器；
//     队列里只剩下整数 ID，不会再回调任何已被销毁的对象。
//   * 过期结果可丢弃：serial 与仓库绑定/代次一起判定，旧仓库或旧任务的结果不会落地。
class MainThreadDispatcher {
public:
  struct Notice {
    // 调用方给每个 worker 分配的稳定编号（同一界面生命周期内唯一）。
    uint64_t sourceId = 0;
    // worker 的提交序号：只有仍是最新一次提交时才应被取用。
    uint64_t serial = 0;
  };

  virtual ~MainThreadDispatcher() = default;

  // 可在任意线程调用。调度器已关闭时返回 false，通知被丢弃。
  virtual bool Post(Notice notice) = 0;

  // 只允许界面线程调用：取走当前已排队的通知。关闭后返回空。
  virtual std::vector<Notice> Drain() = 0;

  // 停止接受新通知并丢弃尚未取走的内容。应在所有 worker Join 之后调用。
  virtual void Close() = 0;

  [[nodiscard]] virtual bool Closed() const = 0;
};

// 进程内队列实现。wxWidgets 侧可用 wxTimer 周期性调用 Drain()，Win32 侧可用自定义消息；
// 本类本身不依赖任何窗口系统。
[[nodiscard]] std::shared_ptr<MainThreadDispatcher> MakeQueuedMainThreadDispatcher();

}  // namespace gc::platform
