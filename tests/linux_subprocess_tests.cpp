// Linux 后台执行器的真实集成测试（linux 组）：
// 只启动测试自己选定的系统程序（/bin/sh、/bin/true、/usr/bin/env 等），
// 不启动 Git、不联网、不涉及 push。所有临时文件都落在用例自有的临时根内。
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform/linux/subprocess.h"
#include "platform/text_encoding.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::platform::ProcessRequest;
using gc::platform::ProcessRunResult;
using gc::platform::ProcessTermination;
using gc::platform::StreamCaptureState;
using gc::test::PosixTempDirectory;
using gc::test::RequireReadFile;
using gc::test::RequireWriteFile;
using gc::test::ScopedEnvironmentVariable;

std::vector<std::string> BaseEnvironment() {
  return {"PATH=/usr/local/bin:/usr/bin:/bin", "LC_ALL=C", "TZ=UTC0"};
}

ProcessRequest MakeRequest(std::wstring program, std::vector<std::wstring> arguments,
                           unsigned long timeoutMs = 5000) {
  ProcessRequest request;
  request.program = std::move(program);
  request.arguments = std::move(arguments);
  request.timeoutMilliseconds = timeoutMs;
  return request;
}

long long ElapsedMilliseconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

GC_TEST(linux_subprocess_separates_streams_and_reports_exit) {
  ProcessRequest request = MakeRequest(L"/bin/sh", {L"-c", L"printf 'out'; printf 'err' >&2; exit 7"});
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/sh");
  GC_CHECK(run.termination == ProcessTermination::exited);
  GC_CHECK(run.exitCode == 7);
  GC_CHECK(run.signal == 0);
  GC_CHECK(run.reaped);
  GC_CHECK(run.stdoutCapture.state == StreamCaptureState::complete);
  GC_CHECK(run.stderrCapture.state == StreamCaptureState::complete);
  GC_CHECK(run.stdoutCapture.bytes == "out");
  GC_CHECK(run.stderrCapture.bytes == "err");
  GC_CHECK(run.AllStreamsComplete());
}

GC_TEST(linux_subprocess_caps_output_but_keeps_draining_both_streams) {
  ProcessRequest request = MakeRequest(
      L"/bin/sh", {L"-c", L"/usr/bin/head -c 5000 /dev/zero; /usr/bin/head -c 3000 /dev/zero >&2"});
  request.maxCaptureBytesPerStream = 1000;
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/sh");
  GC_CHECK(run.stdoutCapture.state == StreamCaptureState::truncated);
  GC_CHECK(run.stderrCapture.state == StreamCaptureState::truncated);
  GC_CHECK(run.stdoutCapture.bytes.size() == 1000);
  GC_CHECK(run.stderrCapture.bytes.size() == 1000);
  GC_CHECK(run.stdoutCapture.observedBytes == 5000);
  GC_CHECK(run.stderrCapture.observedBytes == 3000);
  GC_CHECK_MESSAGE(!run.AllStreamsComplete(), "截断的两路都不能当成完整回答");
}

GC_TEST(linux_subprocess_timeout_is_not_a_normal_exit) {
  ProcessRequest request = MakeRequest(L"/bin/sh", {L"-c", L"sleep 10"}, 200);
  const auto start = std::chrono::steady_clock::now();
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  const long long elapsed = ElapsedMilliseconds(start);
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/sh");
  GC_CHECK(run.timedOut);
  GC_CHECK(run.terminated);
  GC_CHECK(run.termination == ProcessTermination::timedOut);
  GC_CHECK(!run.canceled);
  GC_CHECK_MESSAGE(elapsed < 5000, "超时后必须有界返回，实际耗时 " + std::to_string(elapsed) + "ms");
}

GC_TEST(linux_subprocess_cancel_only_kills_its_own_process_group) {
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  ProcessRequest cancelled = MakeRequest(L"/bin/sh", {L"-c", L"sleep 10"}, 10000);
  cancelled.cancel = cancel;

  std::thread trigger([cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel->store(true, std::memory_order_relaxed);
  });

  // 并发启动的另一个任务不受前一个任务的取消影响。
  ProcessRequest independent = MakeRequest(L"/bin/sh", {L"-c", L"sleep 0.3; exit 3"}, 10000);
  ProcessRunResult independentRun;
  std::thread other([&independent, &independentRun] {
    independentRun = gc::platform::RunCapturedProcess(independent, BaseEnvironment());
  });

  const auto start = std::chrono::steady_clock::now();
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(cancelled, BaseEnvironment());
  const long long elapsed = ElapsedMilliseconds(start);
  other.join();
  trigger.join();

  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动被取消的 /bin/sh");
  GC_CHECK(run.canceled);
  GC_CHECK(run.terminated);
  GC_CHECK(run.termination == ProcessTermination::canceled);
  GC_CHECK_MESSAGE(elapsed < 5000, "取消后必须有界返回，实际耗时 " + std::to_string(elapsed) + "ms");
  GC_CHECK_MESSAGE(independentRun.started && independentRun.termination == ProcessTermination::exited &&
                       independentRun.exitCode == 3,
                   "取消一个任务不能误杀并发启动的另一个任务");
}

GC_TEST(linux_subprocess_distinguishes_signal_termination) {
  ProcessRequest request = MakeRequest(L"/bin/sh", {L"-c", L"kill -TERM $$"});
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/sh");
  GC_CHECK(run.termination == ProcessTermination::signaled);
  GC_CHECK(run.signal == SIGTERM);
  GC_CHECK(!run.timedOut);
  GC_CHECK(!run.canceled);
}

GC_TEST(linux_subprocess_fast_exit_keeps_empty_streams_complete) {
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(MakeRequest(L"/bin/true", {}), BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/true");
  GC_CHECK(run.termination == ProcessTermination::exited);
  GC_CHECK(run.CleanExit());
  GC_CHECK(run.stdoutCapture.bytes.empty());
  GC_CHECK(run.stderrCapture.bytes.empty());
  GC_CHECK(run.stdoutCapture.state == StreamCaptureState::complete);
  GC_CHECK(run.stderrCapture.state == StreamCaptureState::complete);
}

GC_TEST(linux_subprocess_launch_failure_is_not_an_exit_code) {
  const ProcessRunResult missing = gc::platform::RunCapturedProcess(
      MakeRequest(L"/definitely/not/here/git", {}), BaseEnvironment());
  GC_CHECK(!missing.started);
  GC_CHECK(missing.termination == ProcessTermination::notStarted);
  GC_CHECK(!missing.exited);
  GC_CHECK(!missing.timedOut);
  GC_CHECK(missing.launchError != 0);
  GC_CHECK(!missing.launchErrorText.empty());

  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string plain = temp.PathIn("not-executable");
  RequireWriteFile(plain, "#!/bin/sh\nexit 0\n", 0644);
  const ProcessRunResult denied = gc::platform::RunCapturedProcess(
      MakeRequest(gc::platform::Utf8ToWide(plain), {}), BaseEnvironment());
  GC_CHECK(!denied.started);
  GC_CHECK(denied.termination == ProcessTermination::notStarted);
  GC_CHECK(denied.launchError == EACCES);
}

GC_TEST(linux_subprocess_never_waits_for_stdin) {
  ProcessRequest request = MakeRequest(L"/bin/cat", {}, 2000);
  const auto start = std::chrono::steady_clock::now();
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/cat");
  GC_CHECK_MESSAGE(ElapsedMilliseconds(start) < 1500,
                   "stdin 必须接 /dev/null，不能停在看不见的输入上");
  GC_CHECK(run.termination == ProcessTermination::exited);
  GC_CHECK(run.exitCode == 0);
}

GC_TEST(linux_subprocess_reaps_descendants_holding_the_pipe) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string pidFile = temp.PathIn("descendant.pid");
  auto environment = BaseEnvironment();
  environment.push_back("PIDFILE=" + pidFile);

  ProcessRequest request = MakeRequest(
      L"/bin/sh", {L"-c", L"sleep 30 & printf '%s' \"$!\" > \"$PIDFILE\"; exit 0"});
  request.drainAfterExitMilliseconds = 200;
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, environment);
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/sh");
  GC_CHECK(run.termination == ProcessTermination::exited);
  GC_CHECK(run.reaped);

  std::string pidText;
  RequireReadFile(pidFile, pidText);
  GC_REQUIRE_MESSAGE(!pidText.empty(), "子进程应把后代 PID 写进文件");
  const pid_t descendant = static_cast<pid_t>(std::stol(pidText));
  GC_REQUIRE_MESSAGE(descendant > 0, "后代 PID 必须有效");

  // 组长退出后执行器会在回收前清理整个进程组；后代不能继续活着。
  bool gone = false;
  for (int attempt = 0; attempt < 50 && !gone; ++attempt) {
    if (::kill(descendant, 0) != 0 && errno == ESRCH) {
      gone = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!gone) {
    static_cast<void>(::kill(descendant, SIGKILL));
  }
  GC_CHECK_MESSAGE(gone, "组长退出后，持有管道的后代必须被进程组清理掉");
  GC_CHECK_MESSAGE(run.AllStreamsComplete(), "清掉后代后管道应能正常读到 EOF");
}

GC_TEST(linux_subprocess_marks_escaped_descendant_as_abandoned) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string pidFile = temp.PathIn("escaped.pid");
  auto environment = BaseEnvironment();
  environment.push_back("PIDFILE=" + pidFile);

  // 后代用 setsid 逃出本任务的进程组，并继续持有 stdout 写端：执行器无法替它收尾，
  // 只能在有界期限后把这一路标成 abandoned，而不是谎称完整。
  ProcessRequest request = MakeRequest(
      L"/bin/sh",
      {L"-c",
       L"/usr/bin/setsid /bin/sh -c 'exec sleep 30' & desc=$!; sleep 0.3; "
       L"printf '%s' \"$desc\" > \"$PIDFILE\"; exit 0"});
  request.drainAfterExitMilliseconds = 150;
  const auto start = std::chrono::steady_clock::now();
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, environment);
  const long long elapsed = ElapsedMilliseconds(start);
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/sh");
  GC_CHECK_MESSAGE(elapsed < 5000, "放弃收尾必须有界，实际耗时 " + std::to_string(elapsed) + "ms");
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::abandoned,
                   "逃出进程组的后代仍持有写端时，这一路必须标成 abandoned，实际 " +
                       gc::platform::WideToUtf8(run.stdoutCapture.StateLabel()));

  std::string pidText;
  RequireReadFile(pidFile, pidText);
  if (!pidText.empty()) {
    const pid_t escaped = static_cast<pid_t>(std::stol(pidText));
    if (escaped > 0) {
      static_cast<void>(::kill(escaped, SIGKILL));
    }
  }
}

GC_TEST(linux_subprocess_environment_is_explicit_and_not_inherited) {
  ScopedEnvironmentVariable parentOnly("GC_PARENT_ONLY_SENTINEL", "must-not-leak");
  auto environment = BaseEnvironment();
  environment.push_back("GC_CHILD_ONLY=child-value");
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(MakeRequest(L"/usr/bin/env", {}), environment);
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /usr/bin/env");
  GC_REQUIRE_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete, "标准输出应完整读回");
  const std::string out = run.stdoutCapture.bytes;
  GC_CHECK_MESSAGE(out.find("GC_CHILD_ONLY=child-value") != std::string::npos,
                   "显式给出的环境项必须原样交给子进程");
  GC_CHECK_MESSAGE(out.find("GC_PARENT_ONLY_SENTINEL") == std::string::npos,
                   "没有出现在 envp 里的父进程变量不能被子进程继承");
}

GC_TEST(linux_subprocess_binds_working_directory_without_changing_parent) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  char before[4096]{};
  GC_REQUIRE_MESSAGE(::getcwd(before, sizeof(before)) != nullptr, "读取父进程当前目录失败");

  ProcessRequest request = MakeRequest(L"/usr/bin/pwd", {});
  request.workingDirectory = temp.Path();
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /usr/bin/pwd");
  GC_REQUIRE_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete, "标准输出应完整读回");
  GC_CHECK_MESSAGE(run.stdoutCapture.bytes == temp.PathBytes() + "\n",
                   "子进程工作目录应是显式给出的路径，实际 " + run.stdoutCapture.bytes);

  char after[4096]{};
  GC_REQUIRE_MESSAGE(::getcwd(after, sizeof(after)) != nullptr, "读取父进程当前目录失败");
  GC_CHECK_MESSAGE(std::string(before) == std::string(after),
                   "执行子进程不能改变父进程的当前目录");
}

GC_TEST(linux_subprocess_passes_arguments_verbatim_without_shell) {
  ProcessRequest request = MakeRequest(
      L"/usr/bin/printf", {L"%s\n", L"a b", L"-n", L"c'd", L"$HOME", L"; echo injected"});
  const ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, BaseEnvironment());
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /usr/bin/printf");
  GC_REQUIRE_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete, "标准输出应完整读回");
  GC_CHECK_MESSAGE(run.stdoutCapture.bytes == "a b\n-n\nc'd\n$HOME\n; echo injected\n",
                   "参数必须原样到达程序，不能被 shell 解释");
}

GC_TEST(linux_subprocess_closes_inherited_descriptors) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string file = temp.PathIn("leak-check");
  RequireWriteFile(file, "x", 0600);
  const int raw = ::open(file.c_str(), O_RDONLY);
  GC_REQUIRE_MESSAGE(raw >= 0, "无法打开测试文件");
  const int high = ::fcntl(raw, F_DUPFD, 9);
  ::close(raw);
  GC_REQUIRE_MESSAGE(high >= 9, "无法把测试描述符移到高位");

  const ProcessRunResult run = gc::platform::RunCapturedProcess(
      MakeRequest(L"/bin/ls", {L"-1", L"/proc/self/fd"}), BaseEnvironment());
  ::close(high);
  GC_REQUIRE_MESSAGE(run.started, "执行器应能启动 /bin/ls");
  GC_REQUIRE_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete, "标准输出应完整读回");

  const std::string expected = std::to_string(high);
  bool leaked = false;
  std::string line;
  for (const char c : run.stdoutCapture.bytes) {
    if (c == '\n') {
      if (line == expected) {
        leaked = true;
      }
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  GC_CHECK_MESSAGE(!leaked, "父进程的高位描述符不能漏进后台子进程");
}
