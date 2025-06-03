// Linux 交互式终端执行链路（不启动真实 Git）：
//   * 用测试自己的“假终端脚本”接收 xterm 适配器的 -T/-e 参数，再 exec 本程序的辅助入口；
//   * 辅助入口按说明书用 posix_spawn 启动测试自己的假 Git 程序，参数逐项原样送达；
//   * 覆盖 GUI 提前关闭不杀 Git、终端被关闭判为结果未知、辅助入口拒绝越界目录。
// 这一组不启动 Git、不联网、不涉及 push；真实 Git 的终端用例在 linux_terminal_git_tests.cpp。
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/command_window_result.h"
#include "platform/linux/command_window_runner.h"
#include "platform/linux/operation_directory.h"
#include "platform/linux/scratch_files.h"
#include "platform/terminal.h"
#include "platform/text_encoding.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::platform::CommandWindowResult;
using gc::platform::LinuxCommandWindowRunner;
using gc::platform::TerminalAdapterKind;
using gc::platform::TerminalSelection;
using gc::test::PosixTempDirectory;
using gc::test::RequireMakeDirectory;
using gc::test::RequireReadFile;
using gc::test::RequireWriteFile;
using gc::test::ScopedEnvironmentVariable;

constexpr const char* kNonce = "0123456789abcdef";

TerminalSelection FakeTerminalSelection(const std::string& terminalPath) {
  TerminalSelection selection;
  selection.usable = true;
  selection.selected.kind = TerminalAdapterKind::xterm;
  selection.selected.programPath = gc::platform::Utf8ToWide(terminalPath);
  selection.selected.discoveredAs = L"test-fake-terminal";
  return selection;
}

gc::git::CommandWindowOperation MakeOperation(const std::string& program,
                                              std::vector<std::wstring> arguments,
                                              const std::string& repository) {
  gc::git::CommandWindowOperation operation;
  operation.operationId = L"terminal-test";
  operation.displayName = L"终端测试";
  operation.gitExecutable = gc::platform::Utf8ToWide(program);
  operation.repositoryDirectory = gc::platform::Utf8ToWide(repository);
  operation.arguments = std::move(arguments);
  operation.holdTerminalOpen = false;
  return operation;
}

bool WaitForFile(const std::string& path, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (::access(path.c_str(), F_OK) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return ::access(path.c_str(), F_OK) == 0;
}

bool WaitForResult(const LinuxCommandWindowRunner& runner, uint64_t id, CommandWindowResult* result,
                   int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (runner.TakeResult(id, result)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return runner.TakeResult(id, result);
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::string bytes;
  RequireReadFile(path, bytes);
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < bytes.size()) {
    const size_t end = bytes.find('\n', start);
    const size_t stop = end == std::string::npos ? bytes.size() : end;
    std::string line = bytes.substr(start, stop - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

std::string ReadLink(const char* path) {
  char buffer[4096];
  const ssize_t length = ::readlink(path, buffer, sizeof(buffer));
  if (length <= 0) {
    return {};
  }
  return std::string(buffer, static_cast<size_t>(length));
}

void MakeFakeTerminal(const std::string& path) {
  RequireWriteFile(path,
                   "#!/bin/sh\n"
                   "# xterm 适配器参数：-T <title> -e <program> <args...>\n"
                   "program=\"$4\"\n"
                   "shift 4\n"
                   "exec \"$program\" \"$@\"\n",
                   0700);
}

}  // namespace

GC_TEST(linux_terminal_runner_delivers_arguments_without_shell_interpretation) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  RequireMakeDirectory(temp.PathIn("repo"), 0755);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string fakeTerminal = temp.PathIn("fake-terminal");
  const std::string fakeGit = temp.PathIn("fake-git");
  const std::string capture = temp.PathIn("argv.txt");
  MakeFakeTerminal(fakeTerminal);
  RequireWriteFile(fakeGit,
                   "#!/bin/sh\n"
                   "capture=\"$1\"\n"
                   "shift\n"
                   ": > \"$capture\"\n"
                   "for arg in \"$@\"; do printf '%s\\n' \"$arg\" >> \"$capture\"; done\n"
                   "exit 7\n",
                   0700);

  LinuxCommandWindowRunner runner;
  runner.SetTerminalSelectionForTest(FakeTerminalSelection(fakeTerminal));
  runner.SweepStaleOperationDirectories();

  uint64_t id = 0;
  CommandWindowResult failure;
  const gc::git::CommandWindowOperation operation =
      MakeOperation(fakeGit,
                    {gc::platform::Utf8ToWide(capture), L"one two", L"semi;colon",
                     L"$(touch pwned)", L"it's", L"--", L"-leading", L"中文"},
                    temp.PathIn("repo"));
  GC_REQUIRE(runner.Start(operation, gc::platform::TerminalPreferences{}, &id, &failure),
             "终端执行器应接受这份操作：" + gc::platform::WideToUtf8(failure.failureReason));

  CommandWindowResult result;
  GC_REQUIRE(WaitForResult(runner, id, &result, 10000), "应在期限内拿到结果");
  GC_CHECK(result.completion == gc::git::CommandCompletion::finished);
  GC_CHECK(result.exitCode == 7);

  const std::vector<std::string> lines = ReadLines(capture);
  GC_REQUIRE(lines.size() == 7, "假 Git 应收到 7 个参数");
  GC_CHECK(lines[0] == "one two");
  GC_CHECK(lines[1] == "semi;colon");
  GC_CHECK(lines[2] == "$(touch pwned)");
  GC_CHECK(lines[3] == "it's");
  GC_CHECK(lines[4] == "--");
  GC_CHECK(lines[5] == "-leading");
  GC_CHECK(lines[6] == "中文");
  GC_CHECK(::access(temp.PathIn("pwned").c_str(), F_OK) != 0);
  GC_CHECK(::access(temp.PathIn("repo/pwned").c_str(), F_OK) != 0);
}

GC_TEST(linux_terminal_runner_handles_push_shaped_arguments_without_transport) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  RequireMakeDirectory(temp.PathIn("repo"), 0755);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string fakeTerminal = temp.PathIn("fake-terminal");
  const std::string fakeGit = temp.PathIn("fake-git");
  const std::string capture = temp.PathIn("args.txt");
  MakeFakeTerminal(fakeTerminal);
  RequireWriteFile(fakeGit,
                   "#!/bin/sh\n"
                   "capture=\"$1\"\n"
                   "shift\n"
                   "printf '%s\\n' \"$@\" > \"$capture\"\n"
                   "exit 0\n",
                   0700);

  LinuxCommandWindowRunner runner;
  runner.SetTerminalSelectionForTest(FakeTerminalSelection(fakeTerminal));
  uint64_t id = 0;
  CommandWindowResult failure;
  const gc::git::CommandWindowOperation operation =
      MakeOperation(fakeGit,
                    {gc::platform::Utf8ToWide(capture), L"push", L"--", L"origin", L"main"},
                    temp.PathIn("repo"));
  GC_REQUIRE(runner.Start(operation, gc::platform::TerminalPreferences{}, &id, &failure),
             "push 形态的参数应能进入终端执行器");

  CommandWindowResult result;
  GC_REQUIRE(WaitForResult(runner, id, &result, 10000), "应在期限内拿到结果");
  GC_CHECK(result.completion == gc::git::CommandCompletion::finished);
  GC_CHECK(result.exitCode == 0);
  const std::vector<std::string> lines = ReadLines(capture);
  GC_REQUIRE(lines.size() == 4, "假 Git 应收到 push 参数");
  GC_CHECK(lines[0] == "push");
  GC_CHECK(lines[1] == "--");
  GC_CHECK(lines[2] == "origin");
  GC_CHECK(lines[3] == "main");
}

GC_TEST(linux_terminal_runner_shutdown_does_not_kill_the_running_program) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  RequireMakeDirectory(temp.PathIn("repo"), 0755);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string fakeTerminal = temp.PathIn("fake-terminal");
  const std::string fakeGit = temp.PathIn("fake-git");
  const std::string pidFile = temp.PathIn("program.pid");
  MakeFakeTerminal(fakeTerminal);
  RequireWriteFile(fakeGit,
                   "#!/bin/sh\n"
                   "echo \"$$\" > \"$1\"\n"
                   "exec sleep 30\n",
                   0700);

  LinuxCommandWindowRunner runner;
  runner.SetTerminalSelectionForTest(FakeTerminalSelection(fakeTerminal));
  uint64_t id = 0;
  CommandWindowResult failure;
  const gc::git::CommandWindowOperation operation =
      MakeOperation(fakeGit, {gc::platform::Utf8ToWide(pidFile)}, temp.PathIn("repo"));
  GC_REQUIRE(runner.Start(operation, gc::platform::TerminalPreferences{}, &id, &failure),
             "应启动终端链路：" + gc::platform::WideToUtf8(failure.failureReason));
  GC_REQUIRE(WaitForFile(pidFile, 5000), "假程序应写出自己的进程号");

  pid_t programPid = 0;
  {
    std::string text;
    RequireReadFile(pidFile, text);
    programPid = static_cast<pid_t>(std::strtol(text.c_str(), nullptr, 10));
  }
  GC_REQUIRE(programPid > 0, "进程号应有效");
  runner.Shutdown();
  GC_CHECK(::kill(programPid, 0) == 0);  // GUI 关闭不杀正在运行的程序
  static_cast<void>(::kill(programPid, SIGKILL));
  static_cast<void>(::waitpid(programPid, nullptr, 0));
}

GC_TEST(linux_terminal_runner_reports_unknown_when_helper_is_killed) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  RequireMakeDirectory(temp.PathIn("repo"), 0755);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string fakeTerminal = temp.PathIn("fake-terminal");
  const std::string fakeGit = temp.PathIn("fake-git");
  const std::string pidFile = temp.PathIn("program.pid");
  MakeFakeTerminal(fakeTerminal);
  RequireWriteFile(fakeGit,
                   "#!/bin/sh\n"
                   "echo \"$$\" > \"$1\"\n"
                   "exec sleep 30\n",
                   0700);

  LinuxCommandWindowRunner runner;
  runner.SetTerminalSelectionForTest(FakeTerminalSelection(fakeTerminal));
  uint64_t id = 0;
  CommandWindowResult failure;
  const gc::git::CommandWindowOperation operation =
      MakeOperation(fakeGit, {gc::platform::Utf8ToWide(pidFile)}, temp.PathIn("repo"));
  GC_REQUIRE(runner.Start(operation, gc::platform::TerminalPreferences{}, &id, &failure),
             "应启动终端链路：" + gc::platform::WideToUtf8(failure.failureReason));
  GC_REQUIRE(WaitForFile(pidFile, 5000), "假程序应写出自己的进程号");

  const unsigned long long helperPid = runner.LauncherProcessId(id);
  GC_REQUIRE(helperPid != 0, "应记录终端/辅助进程号");
  // 假终端脚本 exec 成辅助入口，因此 launcher pid 就是辅助进程本身。
  static_cast<void>(::kill(static_cast<pid_t>(helperPid), SIGKILL));

  CommandWindowResult result;
  GC_REQUIRE(WaitForResult(runner, id, &result, 8000), "辅助进程被终止后应尽快结案");
  GC_CHECK(result.completion == gc::git::CommandCompletion::terminated);
  GC_CHECK(result.exitCode == 0);
  GC_CHECK(result.failureReason.find(L"终端") != std::wstring::npos ||
           result.failureReason.find(L"辅助进程") != std::wstring::npos);

  std::string pidText;
  RequireReadFile(pidFile, pidText);
  const pid_t programPid = static_cast<pid_t>(std::strtol(pidText.c_str(), nullptr, 10));
  if (programPid > 0) {
    static_cast<void>(::kill(programPid, SIGKILL));
    static_cast<void>(::waitpid(programPid, nullptr, 0));
  }
}

GC_TEST(linux_terminal_helper_refuses_directory_outside_trusted_scratch_root) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  RequireMakeDirectory(temp.PathIn("other-root"), 0700);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string foreign = temp.PathIn("other-root/GcOp1xabcdef0123456789");
  RequireMakeDirectory(foreign, 0700);

  const std::string self = ReadLink("/proc/self/exe");
  GC_REQUIRE(!self.empty(), "应取得测试程序自身路径");
  pid_t pid = ::fork();
  GC_REQUIRE(pid >= 0, "fork 失败");
  if (pid == 0) {
    ::execl(self.c_str(), self.c_str(), "--gc-terminal-helper", foreign.c_str(), kNonce,
            static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  GC_REQUIRE(::waitpid(pid, &status, 0) == pid, "等待辅助入口失败");
  GC_CHECK(WIFEXITED(status));
  GC_CHECK(WEXITSTATUS(status) != 0);
  GC_CHECK(::access((foreign + "/spec.txt").c_str(), F_OK) != 0);
}

GC_TEST(linux_terminal_discovery_reports_missing_terminal_without_hidden_fallback) {
  ScopedEnvironmentVariable path("PATH", std::string());
  const gc::platform::TerminalSelection selection =
      gc::platform::DiscoverAndSelectTerminal(gc::platform::TerminalPreferences{});
  GC_CHECK(!selection.usable);
  GC_CHECK(!selection.failureReason.empty());
  GC_CHECK(!selection.guidance.empty());
  GC_CHECK(selection.guidance.find(L"隐藏执行") != std::wstring::npos ||
           selection.guidance.find(L"终端") != std::wstring::npos);
}

namespace {

int RunHelperForTest(const std::string& operationDirectory) {
  const std::string self = ReadLink("/proc/self/exe");
  GC_REQUIRE(!self.empty(), "应取得测试程序自身路径");
  pid_t pid = ::fork();
  GC_REQUIRE(pid >= 0, "fork 失败");
  if (pid == 0) {
    ::execl(self.c_str(), self.c_str(), "--gc-terminal-helper", operationDirectory.c_str(),
            kNonce, static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  GC_REQUIRE(::waitpid(pid, &status, 0) == pid, "等待辅助入口失败");
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

GC_TEST(linux_terminal_helper_refuses_symlinked_operation_directory) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  RequireMakeDirectory(temp.PathIn("target"), 0700);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string link = temp.PathIn("scratch/GcOp1xabcdef0123456789");
  GC_REQUIRE(::symlink(temp.PathIn("target").c_str(), link.c_str()) == 0, "建符号链接失败");
  GC_CHECK(RunHelperForTest(link) != 0);
  GC_CHECK(::access(temp.PathIn("target/spec.txt").c_str(), F_OK) != 0);
  GC_CHECK(::access(temp.PathIn("target/result.txt").c_str(), F_OK) != 0);
}

GC_TEST(linux_terminal_helper_refuses_operation_directory_open_to_others) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string directory = temp.PathIn("scratch/GcOp1xabcdef0123456789");
  RequireMakeDirectory(directory, 0755);
  GC_CHECK(RunHelperForTest(directory) != 0);
  GC_CHECK(::access((directory + "/spec.txt").c_str(), F_OK) != 0);
}

namespace {

int RunHelperWithExplicitRoot(const std::string& operationDirectory, const std::string& root) {
  const std::string self = ReadLink("/proc/self/exe");
  GC_REQUIRE(!self.empty(), "应取得测试程序自身路径");
  pid_t pid = ::fork();
  GC_REQUIRE(pid >= 0, "fork 失败");
  if (pid == 0) {
    ::execl(self.c_str(), self.c_str(), "--gc-terminal-helper", operationDirectory.c_str(),
            kNonce, root.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  GC_REQUIRE(::waitpid(pid, &status, 0) == pid, "等待辅助入口失败");
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

GC_TEST(linux_terminal_helper_uses_explicit_scratch_root_when_terminal_service_differs) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("inherited-tmp"), 0700);
  RequireMakeDirectory(temp.PathIn("launcher-root"), 0700);
  RequireMakeDirectory(temp.PathIn("repo"), 0755);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("inherited-tmp"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  const std::string fakeGit = temp.PathIn("fake-git");
  const std::string capture = temp.PathIn("capture.txt");
  RequireWriteFile(fakeGit,
                   "#!/bin/sh\n"
                   "capture=\"$1\"\n"
                   "shift\n"
                   "printf '%s\\n' \"$@\" > \"$capture\"\n"
                   "exit 0\n",
                   0700);

  const std::string token = "GcOp1xabcdef0123456789";
  const std::string operationDirectory = temp.PathIn("launcher-root/" + token);
  RequireMakeDirectory(operationDirectory, 0700);

  gc::git::CommandWindowOperation operation;
  operation.operationId = L"explicit-root";
  operation.displayName = L"explicit root";
  operation.gitExecutable = gc::platform::Utf8ToWide(fakeGit);
  operation.repositoryDirectory = gc::platform::Utf8ToWide(temp.PathIn("repo"));
  operation.arguments = {gc::platform::Utf8ToWide(capture), L"status"};
  operation.holdTerminalOpen = false;
  const std::wstring nonce = gc::platform::Utf8ToWide(kNonce);
  std::wstring spec;
  gc::git::CommandPlanReject reject = gc::git::CommandPlanReject::none;
  std::wstring detail;
  GC_REQUIRE(gc::git::BuildCommandWindowSpecText(
                 operation, gc::platform::Utf8ToWide(token), L"explicit root", nonce, &spec,
                 &reject, &detail, gc::git::CommandLineDialect::posixArgv),
             "说明书应可构造：" + gc::platform::WideToUtf8(detail));
  RequireWriteFile(operationDirectory + "/spec.txt", gc::platform::WideToUtf8(spec), 0600);

  GC_CHECK(RunHelperWithExplicitRoot(operationDirectory, temp.PathIn("launcher-root")) == 0);
  std::string resultBytes;
  RequireReadFile(operationDirectory + "/result.txt", resultBytes);
  long exitCode = -1;
  bool launched = false;
  GC_REQUIRE(gc::git::ParseCommandWindowResult(resultBytes, kNonce, &exitCode, &launched),
             "结果行应合格");
  GC_CHECK(launched);
  GC_CHECK(exitCode == 0);
  const std::vector<std::string> lines = ReadLines(capture);
  GC_REQUIRE(lines.size() == 1, "假 Git 应收到一个参数");
  GC_CHECK(lines[0] == "status");
}

GC_TEST(linux_terminal_stale_sweep_preserves_a_live_lease_and_reclaims_after_release) {
  PosixTempDirectory temp;
  GC_REQUIRE(temp.Valid(), temp.FailureReason());
  RequireMakeDirectory(temp.PathIn("scratch"), 0700);
  ScopedEnvironmentVariable tmpdir("TMPDIR", temp.PathIn("scratch"));
  ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  gc::platform::ClaimedOperationDirectory claimed = gc::platform::ClaimOperationDirectoryLinux();
  GC_REQUIRE(claimed.ok, "应能认领操作目录：" + gc::platform::WideToUtf8(claimed.failureReason));
  const std::string path = claimed.pathBytes;
  GC_CHECK(::access(path.c_str(), F_OK) == 0);

  struct timespec oldTimes[2];
  oldTimes[0].tv_sec = ::time(nullptr) - 7200;
  oldTimes[0].tv_nsec = 0;
  oldTimes[1] = oldTimes[0];
  GC_REQUIRE(::utimensat(AT_FDCWD, path.c_str(), oldTimes, 0) == 0, "应能把目录时间改旧");

  LinuxCommandWindowRunner runner;
  runner.SweepStaleOperationDirectories();
  GC_CHECK_MESSAGE(::access(path.c_str(), F_OK) == 0,
                   "租约仍被持有：陈旧清扫不得删除操作目录");

  gc::platform::CloseOperationDirectory(&claimed);
  runner.SweepStaleOperationDirectories();
  GC_CHECK_MESSAGE(::access(path.c_str(), F_OK) != 0,
                   "租约释放且目录陈旧：清扫应回收它");
}
