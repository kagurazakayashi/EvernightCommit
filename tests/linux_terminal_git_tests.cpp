// 真实 Git 经由已验证的 Linux 终端适配器（xterm）执行：
//   * 每个用例使用 LinuxGitFixture 的隔离临时仓库与配置；
//   * 终端里运行的是本程序自己的辅助入口，Git 参数按 argv 直接送达；
//   * 只做 status / add / commit，绝不 push、不联网、不碰用户真实仓库。
// 需要可用的 X11 显示会话（xterm 依赖 DISPLAY）；没有时本组用例会明确报前置失败，
// 不会把“没有运行”当成通过。
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "platform/command_window_result.h"
#include "platform/linux/command_window_runner.h"
#include "platform/terminal.h"
#include "platform/text_encoding.h"
#include "support/linux_git_fixture.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::git::EnvironmentOverride;
using gc::platform::CommandWindowResult;
using gc::platform::LinuxCommandWindowRunner;
using gc::platform::TerminalAdapterKind;
using gc::platform::TerminalPreferences;
using gc::test::LinuxGitFixture;

std::vector<EnvironmentOverride> IsolatedOverrides(const LinuxGitFixture& fixture) {
  const std::wstring root = fixture.Root();
  return {
      EnvironmentOverride{L"HOME", root + L"/home"},
      EnvironmentOverride{L"XDG_CONFIG_HOME", root + L"/home/.config"},
      EnvironmentOverride{L"GIT_CONFIG_GLOBAL", root + L"/home/empty.gitconfig"},
      EnvironmentOverride{L"GIT_CONFIG_NOSYSTEM", std::wstring(L"1")},
      EnvironmentOverride{L"GIT_TEMPLATE_DIR", root + L"/git-template"},
      EnvironmentOverride{L"GIT_ALLOW_PROTOCOL", std::wstring(L"file")},
      EnvironmentOverride{L"TZ", std::wstring(L"UTC0")},
      EnvironmentOverride{L"LC_ALL", std::wstring(L"C.UTF-8")},
      EnvironmentOverride{L"GIT_AUTHOR_NAME", std::wstring(L"终端测试作者")},
      EnvironmentOverride{L"GIT_AUTHOR_EMAIL", std::wstring(L"terminal-author@example.invalid")},
      EnvironmentOverride{L"GIT_COMMITTER_NAME", std::wstring(L"终端测试提交者")},
      EnvironmentOverride{L"GIT_COMMITTER_EMAIL",
                          std::wstring(L"terminal-committer@example.invalid")},
  };
}

gc::git::CommandWindowOperation MakeOperation(const LinuxGitFixture& fixture,
                                              std::vector<std::wstring> arguments,
                                              std::wstring displayName) {
  gc::git::CommandWindowOperation operation;
  operation.operationId = L"terminal-git";
  operation.displayName = std::move(displayName);
  operation.gitExecutable = fixture.GitExe();
  operation.repositoryDirectory = fixture.RepoDir();
  operation.arguments = std::move(arguments);
  operation.environmentOverrides = IsolatedOverrides(fixture);
  operation.holdTerminalOpen = false;
  return operation;
}

TerminalPreferences XtermPreference() {
  TerminalPreferences preferences;
  preferences.preferredKind = TerminalAdapterKind::xterm;
  preferences.allowFallback = false;
  return preferences;
}

void RequireGraphicalSessionForXterm() {
  const char* display = std::getenv("DISPLAY");
  GC_REQUIRE_MESSAGE(display != nullptr && display[0] != '\0',
                     "没有 DISPLAY：xterm 终端集成测试未运行（这不是通过；"
                     "请在图形会话中重跑，或明确记录为未执行）");
  const gc::platform::TerminalSelection selection =
      gc::platform::DiscoverAndSelectTerminal(XtermPreference());
  GC_REQUIRE(selection.usable, "没有找到已验证的 xterm：" +
                                   gc::platform::WideToUtf8(selection.failureReason));
}

bool WaitForResult(const LinuxCommandWindowRunner& runner, uint64_t id, CommandWindowResult* result,
                   int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (runner.TakeResult(id, result)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return runner.TakeResult(id, result);
}

CommandWindowResult RunInXterm(LinuxGitFixture& fixture,
                               const gc::git::CommandWindowOperation& operation) {
  RequireGraphicalSessionForXterm();
  const std::string scratch = gc::platform::WideToUtf8(fixture.Root() + L"/scratch");
  gc::test::RequireMakeDirectory(scratch, 0700);
  gc::test::ScopedEnvironmentVariable tmpdir("TMPDIR", scratch);
  gc::test::ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);
  LinuxCommandWindowRunner runner;
  runner.SweepStaleOperationDirectories();
  uint64_t id = 0;
  CommandWindowResult failure;
  GC_REQUIRE(runner.Start(operation, XtermPreference(), &id, &failure),
             "xterm 启动失败：" + gc::platform::WideToUtf8(failure.failureReason) + " " +
                 gc::platform::WideToUtf8(failure.commandLine));
  CommandWindowResult result;
  GC_REQUIRE(WaitForResult(runner, id, &result, 20000),
             "xterm 中的 Git 未在期限内结案：" + gc::platform::WideToUtf8(result.failureReason));
  return result;
}

}  // namespace

GC_TEST(linux_terminal_xterm_runs_real_git_readonly_status) {
  LinuxGitFixture fixture;
  std::string prepareFailure;
  GC_REQUIRE(fixture.Prepare(prepareFailure), prepareFailure);
  fixture.InitRepository();
  fixture.WriteFile(L"hello.txt", "hello\n", false);

  const CommandWindowResult result =
      RunInXterm(fixture, MakeOperation(fixture, {L"status", L"--porcelain=v1"}, L"status"));
  GC_CHECK(result.completion == gc::git::CommandCompletion::finished);
  GC_CHECK(result.exitCode == 0);
}

GC_TEST(linux_terminal_xterm_delivers_metacharacter_pathspec_verbatim) {
  LinuxGitFixture fixture;
  std::string prepareFailure;
  GC_REQUIRE(fixture.Prepare(prepareFailure), prepareFailure);
  fixture.InitRepository();
  fixture.WriteFile(L"weird;name $(touch pwned).txt", "data\n", false);

  const CommandWindowResult result = RunInXterm(
      fixture,
      MakeOperation(fixture, {L"add", L"--", L"weird;name $(touch pwned).txt"}, L"add"));
  GC_CHECK(result.completion == gc::git::CommandCompletion::finished);
  GC_CHECK(result.exitCode == 0);

  const gc::test::LinuxGitRun listed = fixture.RunInRepo({L"ls-files", L"-z"});
  GC_REQUIRE(listed.Success(), "ls-files 应成功");
  GC_CHECK(listed.out.find(L"weird;name $(touch pwned).txt") != std::wstring::npos);
  GC_CHECK(::access(gc::platform::WideToUtf8(fixture.Root() + L"/pwned").c_str(), F_OK) != 0);
}

GC_TEST(linux_terminal_xterm_runs_real_git_commit) {
  LinuxGitFixture fixture;
  std::string prepareFailure;
  GC_REQUIRE(fixture.Prepare(prepareFailure), prepareFailure);
  fixture.InitRepository();
  fixture.WriteFile(L"file.txt", "content\n", true);

  const CommandWindowResult result =
      RunInXterm(fixture,
                 MakeOperation(fixture, {L"commit", L"-m", L"terminal commit"}, L"commit"));
  GC_CHECK(result.completion == gc::git::CommandCompletion::finished);
  GC_CHECK(result.exitCode == 0);
  GC_CHECK(fixture.HeadSubject() == L"terminal commit");
}
