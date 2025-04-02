// 刷新与任务调度的纯逻辑用例：用“可控的异步返回”模拟乱序完成、通知丢失与并发触发。
// 这里不碰任何窗口，也不依赖 Git——被测的是「哪一次结果可以落地、什么时候可以发起操作」。
// 真实仓库驱动的读取见 refresh_repository_tests.cpp。
#include <string>
#include <string_view>
#include <vector>

#include "app/task_coordinator.h"
#include "git/command_window.h"
#include "support/tiny_test.h"

namespace {

using gc::app::OperationOutcome;
using gc::app::ReadDisposition;
using gc::app::RefreshSchedule;
using gc::app::TaskCoordinator;

constexpr std::wstring_view kGitA = L"C:\\Program Files\\Git\\bin\\git.exe";
constexpr std::wstring_view kRepoA = L"D:\\项目\\仓库甲";
constexpr std::wstring_view kRepoB = L"D:\\项目\\仓库乙";

// 走完一次正常读取：绑定 + 请求 + 取回凭证 + 采纳。
void LoadOnce(TaskCoordinator& tasks, std::wstring_view git, std::wstring_view root) {
  static_cast<void>(tasks.BindRepository(git, root));
  GC_REQUIRE_MESSAGE(tasks.RequestRefresh() == RefreshSchedule::start, "刷新请求应当被允许发起读取");
  const gc::app::ReadTicket ticket = tasks.BeginRead();
  GC_REQUIRE_MESSAGE(tasks.CompleteRead(ticket.serial) == ReadDisposition::accepted,
                     "同一身份下的读取结果应当被采纳");
}

}  // namespace

GC_TEST(coordinator_refuses_refresh_without_repository_identity) {
  TaskCoordinator tasks;
  // 还没有可用仓库：没有任何安全的查询落点，宁可不读也不能凭空发一条命令。
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::ignored);
  GC_CHECK(tasks.BeginRead().serial == 0);
  GC_CHECK(!tasks.Identity().Valid());

  LoadOnce(tasks, kGitA, kRepoA);
  GC_CHECK(tasks.Identity().Valid());
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
}

GC_TEST(coordinator_merges_repeated_refresh_into_exactly_one_followup) {
  TaskCoordinator tasks;
  static_cast<void>(tasks.BindRepository(kGitA, kRepoA));
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket inFlight = tasks.BeginRead();

  // 连点“刷新”、几个来源同时到达：都并入这次在途读取，绝不排队累积。
  for (int attempt = 0; attempt < 5; ++attempt) {
    GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::merged);
  }
  GC_CHECK(tasks.ReadInFlight());
  GC_CHECK(tasks.RefreshStillQueued());

  GC_CHECK(tasks.CompleteRead(inFlight.serial) == ReadDisposition::accepted);
  GC_CHECK(!tasks.ReadInFlight());
  // 合并出来的补读只有一次；补读做完若没有新请求，就不会再自己发起第三轮。
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket followup = tasks.BeginRead();
  GC_CHECK(!tasks.RefreshStillQueued());
  GC_CHECK(tasks.CompleteRead(followup.serial) == ReadDisposition::accepted);
  GC_CHECK(!tasks.RefreshStillQueued());
}

GC_TEST(coordinator_discards_result_after_switching_repository) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  const unsigned long long generationOfA = tasks.Identity().generation;

  // 仓库甲的读取仍在途时用户切到仓库乙：甲的结果必须作废。
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket stale = tasks.BeginRead();
  GC_CHECK(tasks.BindRepository(kGitA, kRepoB));
  GC_CHECK(tasks.Identity().generation != generationOfA);
  GC_CHECK(tasks.CompleteRead(stale.serial) == ReadDisposition::staleRepository);
  // 同一序号的重复通知也一律作废，不会把上个仓库的列表塞回来。
  GC_CHECK(tasks.CompleteRead(stale.serial) == ReadDisposition::staleRepository);
  GC_CHECK(!tasks.ReadInFlight());

  // 新身份仍可正常读取。
  const gc::app::ReadTicket fresh = tasks.BeginRead();
  GC_CHECK(fresh.serial != stale.serial);
  GC_CHECK(tasks.CompleteRead(fresh.serial) == ReadDisposition::accepted);
}

GC_TEST(coordinator_discards_result_after_switching_git_executable) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket stale = tasks.BeginRead();

  // 只换 Git 程序、仓库路径没变，也算身份变化：旧 Git 读回来的摘要与列表都不能再用。
  GC_CHECK(tasks.BindRepository(L"C:\\Users\\me\\git\\bin\\git.exe", kRepoA));
  GC_CHECK(tasks.CompleteRead(stale.serial) == ReadDisposition::staleRepository);
}

GC_TEST(coordinator_adopts_result_when_refresh_rebinds_same_identity) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  const unsigned long long generation = tasks.Identity().generation;
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket ticket = tasks.BeginRead();

  // 刷新会重新识别仓库；同一个仓库（即使写成不同的大小或斜杠）不该被当成“换仓库”，
  // 否则每次刷新都会把自己正在等的结果判成过期。
  GC_CHECK(!tasks.BindRepository(kGitA, L"d:/项目/仓库甲"));
  GC_CHECK(tasks.Identity().generation == generation);
  GC_CHECK(tasks.CompleteRead(ticket.serial) == ReadDisposition::accepted);
}

GC_TEST(coordinator_ignores_unknown_read_serials) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket ticket = tasks.BeginRead();
  GC_CHECK(tasks.CompleteRead(0) == ReadDisposition::notPending);
  GC_CHECK(tasks.CompleteRead(ticket.serial + 999) == ReadDisposition::notPending);
  // 上面这些不该结束在途状态：否则真正的结果回来时会被当成无主通知。
  GC_CHECK(tasks.ReadInFlight());
  GC_CHECK(tasks.CompleteRead(ticket.serial) == ReadDisposition::accepted);
  GC_CHECK(tasks.CompleteRead(ticket.serial) == ReadDisposition::notPending);
}

GC_TEST(coordinator_admits_only_one_operation_at_a_time) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);

  unsigned long long first = 0;
  GC_CHECK(tasks.BeginOperation(L"status", &first));
  GC_CHECK(tasks.OperationInFlight());
  unsigned long long second = 0;
  GC_CHECK(!tasks.BeginOperation(L"push", &second));  // 防重：第二个写操作根本不发起。

  const OperationOutcome done =
      tasks.FinishOperation(first, gc::git::CommandCompletion::finished, 0, std::wstring_view{});
  GC_CHECK(done.recognised);
  GC_CHECK(!tasks.OperationInFlight());
  GC_CHECK(tasks.BeginOperation(L"push", &second));
  GC_CHECK(second != 0);
}

GC_TEST(operation_success_requires_zero_exit_code) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  unsigned long long serial = 0;
  GC_CHECK(tasks.BeginOperation(L"status", &serial));

  // 窗口起得来不等于操作成功：只有拿到 Git 退出码且为 0 才算成功。
  const OperationOutcome launched =
      tasks.FinishOperation(serial, gc::git::CommandCompletion::launched, 0, std::wstring_view{});
  GC_CHECK_MESSAGE(!launched.succeeded, "命令窗口启动成功不能当作操作成功");
}

GC_TEST(operation_outcomes_follow_git_exit_code) {
  struct Expectation {
    gc::git::CommandCompletion completion;
    long exitCode;
    bool succeeded;
  };
  const std::vector<Expectation> cases{
      {gc::git::CommandCompletion::finished, 0, true},
      {gc::git::CommandCompletion::finished, 1, false},
      {gc::git::CommandCompletion::gitNotStarted, 9009, false},
      {gc::git::CommandCompletion::terminated, 0, false},
      {gc::git::CommandCompletion::launchFailed, 0, false},
  };
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  for (const Expectation& item : cases) {
    unsigned long long serial = 0;
    GC_REQUIRE_MESSAGE(tasks.BeginOperation(L"status", &serial), "首次发起操作应成功占用槽位");
    const OperationOutcome outcome = tasks.FinishOperation(serial, item.completion, item.exitCode, L"演示原因");
    GC_CHECK_MESSAGE(outcome.recognised, "结案的序号应当就是刚发起的那次操作");
    GC_CHECK_MESSAGE(outcome.succeeded == item.succeeded, "成敗判定只以 Git 退出码为准");
    // 失败的操作也可能已经改动仓库，所以成敗都要安排一次刷新。
    GC_CHECK_MESSAGE(outcome.refreshRequested, "操作结束后都要重读一次仓库");
    GC_CHECK(!outcome.note.empty());
    GC_CHECK(!tasks.OperationInFlight());
  }
}

GC_TEST(operation_conclusion_untouched_by_other_operation_ids) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  unsigned long long serial = 0;
  GC_REQUIRE_MESSAGE(tasks.BeginOperation(L"status", &serial), "首次发起操作应成功占用槽位");
  // 迟到或无主的序号不能替当前操作结案，否则槽位会被别人的通知释放。
  const OperationOutcome unknown =
      tasks.FinishOperation(serial + 1, gc::git::CommandCompletion::finished, 0, std::wstring_view{});
  GC_CHECK(!unknown.recognised);
  GC_CHECK(tasks.OperationInFlight());
}

GC_TEST(lost_completion_notification_releases_slot_and_refreshes) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  unsigned long long serial = 0;
  GC_REQUIRE_MESSAGE(tasks.BeginOperation(L"status", &serial), "首次发起操作应成功占用槽位");

  // 执行器已经不认识这个操作（通知丢失）：界面不能永远显示“执行中”。
  const OperationOutcome forgotten = tasks.ForgetOperation(serial, L"没有收到完成通知");
  GC_CHECK(forgotten.recognised);
  GC_CHECK(!forgotten.succeeded);
  GC_CHECK(forgotten.refreshRequested);
  GC_CHECK(!tasks.OperationInFlight());
  unsigned long long again = 0;
  GC_CHECK(tasks.BeginOperation(L"status", &again));  // 一个保留下来的窗口或丢失的通知都不该锁住界面
}

GC_TEST(operation_conclusion_shows_with_the_refresh_it_triggered) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  unsigned long long serial = 0;
  GC_REQUIRE_MESSAGE(tasks.BeginOperation(L"push", &serial), "首次发起操作应成功占用槽位");
  const OperationOutcome outcome =
      tasks.FinishOperation(serial, gc::git::CommandCompletion::finished, 1, std::wstring_view{});
  tasks.RememberOperationConclusion(outcome.note);

  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket ticket = tasks.BeginRead();
  GC_CHECK(tasks.CompleteRead(ticket.serial) == ReadDisposition::accepted);
  // 状态栏一行里同时看得到“刚才的操作怎么样”和“仓库现在怎么样”。
  const std::wstring line = tasks.ReadFinishedText(L"工作区已读取：未暂存 2 项、已暂存 0 项。");
  GC_CHECK_MESSAGE(line.find(outcome.note) != std::wstring::npos, "操作结论要跟随它触发的刷新一起显示");
  GC_CHECK(line.find(L"工作区已读取") != std::wstring::npos);

  // 结论只用一次：之后的手动刷新不该再重复上次那个操作。
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket next = tasks.BeginRead();
  GC_CHECK(tasks.CompleteRead(next.serial) == ReadDisposition::accepted);
  GC_CHECK_MESSAGE(tasks.ReadFinishedText(L"工作区已读取：未暂存 2 项、已暂存 0 项。").find(L"push") ==
                       std::wstring::npos,
                   "旧的结论不该在之后的刷新里反复出现");
}

GC_TEST(switching_repository_clears_pending_operation_conclusion) {
  TaskCoordinator tasks;
  LoadOnce(tasks, kGitA, kRepoA);
  unsigned long long serial = 0;
  GC_REQUIRE_MESSAGE(tasks.BeginOperation(L"commit", &serial), "首次发起操作应成功占用槽位");
  const OperationOutcome outcome =
      tasks.FinishOperation(serial, gc::git::CommandCompletion::finished, 0, std::wstring_view{});
  tasks.RememberOperationConclusion(outcome.note);

  // 用户先切了仓库：上个仓库的操作结论不该跟着新仓库的摘要一起显示。
  GC_CHECK(tasks.BindRepository(kGitA, kRepoB));
  GC_CHECK(tasks.RequestRefresh() == RefreshSchedule::start);
  const gc::app::ReadTicket ticket = tasks.BeginRead();
  GC_CHECK(tasks.CompleteRead(ticket.serial) == ReadDisposition::accepted);
  GC_CHECK_MESSAGE(tasks.ReadFinishedText(L"工作区已读取：未暂存 0 项、已暂存 0 项。").find(L"commit") ==
                       std::wstring::npos,
                   "换仓库后不再重复上个仓库的操作结论");
}
