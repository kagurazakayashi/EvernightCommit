// 主线程调度边界与后台任务代次的纯逻辑测试：
// 不创建窗口、不启动进程；只验证通知队列、sourceId 分发、序号作废与关闭顺序。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "platform/git_task_worker.h"
#include "platform/main_thread_dispatch.h"
#include "support/tiny_test.h"

namespace {

using gc::platform::GitTaskWorker;
using gc::platform::MainThreadDispatcher;

std::vector<MainThreadDispatcher::Notice> WaitForNotices(
    const std::shared_ptr<MainThreadDispatcher>& dispatcher, size_t expected) {
  std::vector<MainThreadDispatcher::Notice> notices;
  for (int attempt = 0; attempt < 200 && notices.size() < expected; ++attempt) {
    std::vector<MainThreadDispatcher::Notice> drained = dispatcher->Drain();
    notices.insert(notices.end(), drained.begin(), drained.end());
    if (notices.size() < expected) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  return notices;
}

}  // namespace

GC_TEST(main_thread_dispatcher_queues_across_threads_and_closes) {
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  GC_REQUIRE_MESSAGE(dispatcher != nullptr, "调度器应能创建");

  std::thread producer([dispatcher] {
    for (uint64_t index = 1; index <= 3; ++index) {
      static_cast<void>(dispatcher->Post(MainThreadDispatcher::Notice{7, index}));
    }
  });
  producer.join();

  const std::vector<MainThreadDispatcher::Notice> notices = dispatcher->Drain();
  GC_REQUIRE(notices.size() == 3, "三条通知都应排在主线程队列里");
  GC_CHECK(notices[0].sourceId == 7 && notices[0].serial == 1);
  GC_CHECK(notices[1].serial == 2);
  GC_CHECK(notices[2].serial == 3);
  GC_CHECK(dispatcher->Drain().empty());

  dispatcher->Close();
  GC_CHECK(dispatcher->Closed());
  GC_CHECK_MESSAGE(!dispatcher->Post(MainThreadDispatcher::Notice{7, 4}),
                   "关闭后的调度器必须拒绝新通知");
  GC_CHECK(dispatcher->Drain().empty());
}

GC_TEST(git_task_worker_delivers_through_dispatcher_with_source_id) {
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  GitTaskWorker<int, int> worker;
  worker.SetCompletionDispatcher(dispatcher, 42);
  worker.Request(nullptr, 0, 5, [](const int& value) { return value * 2; });

  const std::vector<MainThreadDispatcher::Notice> notices = WaitForNotices(dispatcher, 1);
  GC_REQUIRE_MESSAGE(notices.size() == 1, "工作线程应把完成通知投进主线程队列");
  GC_CHECK(notices[0].sourceId == 42);
  GC_CHECK(notices[0].serial == 1);

  int result = 0;
  GC_CHECK(worker.FetchLatest(notices[0].serial, &result));
  GC_CHECK(result == 10);
  GC_CHECK_MESSAGE(!worker.FetchLatest(notices[0].serial, &result), "同一结果只能取用一次");
  worker.BeginStop();
  worker.Join();
  dispatcher->Close();
}

GC_TEST(git_task_worker_request_does_not_block_the_caller) {
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  GitTaskWorker<int, int> worker;
  worker.SetCompletionDispatcher(dispatcher, 3);
  const auto start = std::chrono::steady_clock::now();
  worker.Request(nullptr, 0, 1, [](const int& value) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return value;
  });
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  GC_CHECK_MESSAGE(elapsed < 100, "提交后台任务不能阻塞界面线程，实际耗时 " +
                                      std::to_string(elapsed) + "ms");
  worker.BeginStop();
  worker.Join();
  dispatcher->Close();
}

GC_TEST(git_task_worker_late_result_from_older_request_is_not_accepted) {
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  GitTaskWorker<int, int> worker;
  worker.SetCompletionDispatcher(dispatcher, 9);
  worker.Request(nullptr, 0, 1, [](const int& value) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return value;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  worker.Request(nullptr, 0, 2, [](const int& value) { return value; });

  // 两条都可能完成，但只有最新提交的序号能落地。
  const std::vector<MainThreadDispatcher::Notice> notices = WaitForNotices(dispatcher, 2);
  GC_REQUIRE(notices.size() == 2, "两次提交都应完成并各发一条通知");
  int result = 0;
  GC_CHECK_MESSAGE(!worker.FetchLatest(1, &result), "旧序号的结果必须被丢弃");
  GC_CHECK(worker.FetchLatest(2, &result));
  GC_CHECK(result == 2);
  worker.BeginStop();
  worker.Join();
  dispatcher->Close();
}

GC_TEST(git_task_worker_completion_during_shutdown_is_discarded) {
  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  GitTaskWorker<int, int> worker;
  worker.SetCompletionDispatcher(dispatcher, 1);
  worker.Request(nullptr, 0, 1, [](const int& value) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return value;
  });

  worker.BeginStop();  // 关闭从结果产生之前开始
  worker.Join();
  dispatcher->Close();  // 先 Join 再 Close：队列里只剩整数编号，不会回调已销毁对象

  int result = 0;
  GC_CHECK_MESSAGE(!worker.FetchLatest(1, &result),
                   "开始收尾后完成的结果不能再被界面采用");
  GC_CHECK(dispatcher->Drain().empty());
}
