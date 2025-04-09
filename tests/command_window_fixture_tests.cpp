// 外部命令窗口执行器的集成测试（对应 AGENTS.md：自动化测试只在自己创建的临时仓库里跑 Git）。
// 覆盖：真实退出码（成功/失败）、cmd 退出码与 Git 退出码不混同、特殊路径与参数、
// 受控环境覆盖确实传进命令窗口里的 Git、启动前拒绝、提前关窗不卡死、Shutdown 不阻塞。
// 每个用例会打开真实的 cmd 窗口并在结束后关闭，仅在本机桌面环境可完整验证。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/command_window_runner.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::CommandCompletion;
using gc::git::CommandWindowOperation;
using gc::platform::CommandWindowResult;
using gc::test::GitFixture;

constexpr auto kOperationBudget = std::chrono::seconds(30);

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
}

CommandWindowOperation MakeOperation(const GitFixture& fixture, std::vector<std::wstring> arguments) {
  CommandWindowOperation operation;
  operation.operationId = L"status";
  operation.displayName = L"status";
  operation.gitExecutable = fixture.GitExe();
  operation.repositoryDirectory = fixture.RepoDir();
  operation.arguments = std::move(arguments);
  return operation;
}

// 取证用：把操作目录里的文件清单与内容（含脚本原文）拼成一行诊断文本。
std::string DumpOperationDirectory(const std::wstring& directory) {
  namespace fs = std::filesystem;
  std::string text = " 目录[" + gc::platform::Utf16ToUtf8(directory) + "]";
  std::error_code ec;
  if (!fs::exists(fs::path(directory), ec)) {
    return text + " 不存在";
  }
  for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(directory), ec)) {
    if (entry.is_directory()) {
      text += " {" + gc::platform::Utf16ToUtf8(entry.path().native()) + "/}";
      continue;
    }
    std::string bytes;
    std::ifstream file(entry.path(), std::ios::binary);
    if (file.is_open()) {
      bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    text += " {" + entry.path().string() + " size=" + std::to_string(bytes.size()) + " 内容=" + bytes + "}";
  }
  return text;
}

// 轮询等待结果登记。测试进程没有消息循环（GUI 侧才用 PostMessage 通知），
// 因此这里用 TakeResult 轮询；超时即判定“一直停在执行中”，属于回归。
// 超时时把操作目录（脚本原文 + 标记/结果文件） dump 进诊断信息，便于直接定位卡住的位置。
bool WaitForResult(gc::platform::CommandWindowRunner& runner, uint64_t operationId,
                   CommandWindowResult* out, std::string* timeoutDump) {
  const auto deadline = std::chrono::steady_clock::now() + kOperationBudget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runner.TakeResult(operationId, out)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (timeoutDump != nullptr) {
    *timeoutDump = DumpOperationDirectory(runner.OperationDirectory(operationId));
  }
  return false;
}

// 统一入口：等待终态；超时则把操作目录取证信息带进失败原因。
void ExpectResult(gc::platform::CommandWindowRunner& runner, uint64_t operationId,
                  CommandWindowResult* out, const std::string& label) {
  std::string timeoutDump;
  if (WaitForResult(runner, operationId, out, &timeoutDump)) {
    return;
  }
  GC_REQUIRE_MESSAGE(false, label + "（30 秒内没有终态，一直停在“执行中”属于回归）" + timeoutDump);
}

std::string CompletionName(CommandCompletion completion) {
  switch (completion) {
    case CommandCompletion::launchFailed:
      return "launchFailed";
    case CommandCompletion::launched:
      return "launched";
    case CommandCompletion::running:
      return "running";
    case CommandCompletion::finished:
      return "finished";
    case CommandCompletion::gitNotStarted:
      return "gitNotStarted";
    case CommandCompletion::terminated:
      return "terminated";
    case CommandCompletion::scriptNeverRan:
      return "scriptNeverRan";
    case CommandCompletion::stillUnknown:
      return "stillUnknown";
  }
  return "unknown";
}

std::string Describe(const CommandWindowResult& result) {
  return "completion=" + CompletionName(result.completion) +  //
         " exit=" + std::to_string(result.exitCode) +         //
         " console=" + std::to_string(result.consoleExitCode) +
         " reason=" + gc::platform::Utf16ToUtf8(result.failureReason) +
         " cmd=" + gc::platform::Utf16ToUtf8(result.commandLine);
}

// 用例结束把还开着的命令窗口收掉（Git 已退出，只关窗口，不碰进程），
// 并等 cmd 真正退出：命令窗口的工作目录就是夹具临时仓库，进程不走夹具就删不掉。
struct RunnerGuard {
  gc::platform::CommandWindowRunner* runner = nullptr;
  std::vector<uint64_t>* ids = nullptr;
  ~RunnerGuard() {
    // 每轮都重发关闭请求：窗口可能刚出现、标题尚未写上唯一标记，第一次查找会落空。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      bool allGone = true;
      for (uint64_t id : *ids) {
        runner->CloseOperationWindow(id);
        if (!runner->ConsoleExited(id)) {
          allGone = false;
        }
      }
      if (allGone) {
        // 进程刚退出时句柄释放与目录解绑有极短窗口期，让夹具的删除重试赶在前面。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    runner->Shutdown();
  }
};

}  // namespace

GC_TEST(command_window_reports_real_success_exit_code) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(MakeOperation(fixture, {L"status"}), &id, &failure);
  GC_REQUIRE_MESSAGE(started, "命令窗口启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "成功场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished, Describe(result));
  GC_CHECK_MESSAGE(result.exitCode == 0, Describe(result));
  GC_CHECK(result.Success());
  // 进行中操作清零；结果记录仍可取回。
  GC_CHECK(runner.ActiveCount() == 0);
}

GC_TEST(command_window_reports_real_failure_exit_code) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  // 故意无效参数：必须是 Git 真实返回的非 0 退出码，而不是 cmd 的退出码。
  // 注意不能选 rev-parse —— 它对无法解析的参数会原样打印并以 0 退出（实测踩坑），
  // status 遇到未知选项才确实报错（129）。
  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started =
      runner.Start(MakeOperation(fixture, {L"status", L"--definitely-not-an-option"}), &id, &failure);
  GC_REQUIRE_MESSAGE(started, "命令窗口启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "失败退出码场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished, Describe(result));
  GC_CHECK_MESSAGE(result.exitCode != 0, Describe(result));
  // cmd /k 在脚本执行完后仍存活；进程退出只能由窗口关闭造成，因此 console 退出码
  // 只作诊断，判定成败一律看 result.txt。
  GC_CHECK(result.commandLine.find(L"--definitely-not-an-option") != std::wstring::npos);
}

GC_TEST(command_window_delivers_percent_in_names_to_git) {
  // 引号挡不住 cmd 的百分号展开（实测：`"x%FOO%y.txt"` 在引号内照样被换成变量值，
  // 单个 `%.` 会被当成位置参数引用而整段消失），所以脚本里必须按展开轮数转义。
  // 判据不能靠读窗口输出：--no-index 的「有差异」与「读不到」都是退出码 1。
  // 这里比较**同一个档案两次**：转义正确 -> 没有差异 -> 退出码 0；名字被改写 -> 1。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"100%.txt", "percent dot name\n");           // 落单的 %：与本机环境无关
  fixture.WriteFile(L"x%COMSPEC%y.txt", "percent pair name\n");   // 成对的 %：引用一个必然存在的变量

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  const std::vector<std::wstring> names = {L"100%.txt", L"x%COMSPEC%y.txt"};
  for (const std::wstring& name : names) {
    // 两个路徑指向同一个档案：Git 报告「没有差异」的唯一前提是两个名字都原样送到。
    uint64_t id = 0;
    CommandWindowResult failure;
    const bool started =
        runner.Start(MakeOperation(fixture, {L"diff", L"--no-index", L"--", name, name}), &id, &failure);
    GC_REQUIRE_MESSAGE(started, "命令窗口启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
    opened.push_back(id);

    CommandWindowResult result;
    ExpectResult(runner, id, &result, "含 % 的档名");
    GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished, Describe(result));
    GC_CHECK_MESSAGE(result.exitCode == 0,
                    "档名里的 % 没有原样送達 Git（被 cmd 改写过）：" + Describe(result));
    // 界面展示用的命令行保留原始形态，不显示转义后的模样。
    GC_CHECK_MESSAGE(result.commandLine.find(name) != std::wstring::npos,
                     "展示的命令行应含原始档名：" + gc::platform::Utf16ToUtf8(result.commandLine));
  }
}

GC_TEST(command_window_handles_chinese_and_space_paths) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 目录名同时包含中文、空格与 &：进入 cmd 脚本的路径必须原样工作。
  fixture.InitRepository(L"仓库 & 空格");
  fixture.WriteFile(L"中文 目录/文件 & 名.txt", "内容\n");
  fixture.StageAll();
  fixture.Commit(L"中文提交");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(MakeOperation(fixture, {L"status", L"--porcelain=v1"}), &id, &failure);
  GC_REQUIRE_MESSAGE(started,
                     "中文/空格/& 路径启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "中文/空格路径场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished && result.exitCode == 0, Describe(result));
  GC_CHECK(result.repositoryDirectory == fixture.RepoDir());
}

GC_TEST(command_window_keeps_metacharacter_arguments_literal) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  // 文件名里塞满 cmd 元字符与中文：作为 pathspec 参数进入脚本后必须原样到达 Git。
  // 只能挑 NTFS 合法字符（| < > : " / \ ? * 是非法文件名字符，cmd 也拿不到它们）。
  fixture.WriteFile(L"特殊 &^%!(x) 文件.txt", "hello\n");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  // 只读查询：git status -- <pathspec>。参数含 & ^ % ! 括号与空格，
  // 全部落在引号区域内，cmd 必须按字面量传给 Git；匹配到该文件时退出码为 0。
  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started =
      runner.Start(MakeOperation(fixture, {L"status", L"--porcelain=v1", L"特殊 &^%!(x) 文件.txt"}), &id,
                   &failure);
  GC_REQUIRE_MESSAGE(started, "元字符参数被错误拒绝：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "元字符参数场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished && result.exitCode == 0, Describe(result));
  // 展示用的命令行必须是宽字符原文，不能被码页字节逐字符提升成乱码。
  GC_CHECK_MESSAGE(result.commandLine.find(L"特殊 &^%!(x) 文件.txt") != std::wstring::npos,
                   Describe(result));
}

GC_TEST(command_window_rejects_quote_injection_before_launch) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  // 经典注入样例：把参数写成“闭合引用后再执行 calc”。含双引号的输入在启动 cmd 之前就被拒绝，
  // 因此连命令窗口都不该创建。
  CommandWindowOperation operation = MakeOperation(fixture, {L"status\" & calc"});
  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(operation, &id, &failure);

  GC_CHECK(!started);
  GC_CHECK(id == 0);
  GC_CHECK(failure.completion == CommandCompletion::launchFailed);
  GC_CHECK(!failure.failureReason.empty());
  GC_CHECK(runner.ActiveCount() == 0);
}

GC_TEST(command_window_reports_launch_failure_for_missing_git) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  CommandWindowOperation operation = MakeOperation(fixture, {L"--version"});
  operation.gitExecutable = fixture.PathInRoot(L"不存在的 git.exe");
  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(operation, &id, &failure);

  GC_CHECK(!started);
  GC_CHECK(failure.completion == CommandCompletion::launchFailed);
  // “Git 不存在”必须是启动失败，不能被报告成“执行完成 + 非 0 退出码”。
  GC_CHECK(failure.exitCode == 0);
  GC_CHECK(failure.failureReason.find(L"Git 程序不存在") != std::wstring::npos);
}

GC_TEST(command_window_applies_environment_overrides) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  // 命令窗口子进程继承本进程环境（产品设计：用户的真实 Git 配置与凭据要照常生效），
  // 因此不能用 user.name 之类可能被用户配置填满的键来验证覆盖。
  // 改用 Git 自带的环境注入配置（GIT_CONFIG_COUNT / _KEY_n / _VALUE_n，Git 2.31+）：
  // 基线查一个用户与仓库都不可能有的键 → 退出码 1；注入后再查 → 退出码 0。
  static constexpr const wchar_t* kProbeKey = L"evernightCommitTest.probeValue";
  CommandWindowOperation baseline = MakeOperation(fixture, {L"config", L"--get", std::wstring(kProbeKey)});
  CommandWindowOperation overridden = MakeOperation(fixture, {L"config", L"--get", std::wstring(kProbeKey)});
  overridden.environmentOverrides = {
      gc::git::EnvironmentOverride{L"GIT_CONFIG_COUNT", std::wstring(L"1")},
      gc::git::EnvironmentOverride{L"GIT_CONFIG_KEY_0", std::wstring(kProbeKey)},
      gc::git::EnvironmentOverride{L"GIT_CONFIG_VALUE_0", std::wstring(L"from-override")},
  };

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t baselineId = 0;
  CommandWindowResult failure;
  GC_REQUIRE(runner.Start(baseline, &baselineId, &failure),
             "基线操作启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(baselineId);
  CommandWindowResult baselineResult;
  ExpectResult(runner, baselineId, &baselineResult, "基线操作");
  GC_CHECK_MESSAGE(baselineResult.exitCode != 0,
                   "未注入时该键本就不存在：" + Describe(baselineResult));

  uint64_t overriddenId = 0;
  GC_REQUIRE(runner.Start(overridden, &overriddenId, &failure),
             "环境覆盖场景启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(overriddenId);
  CommandWindowResult overriddenResult;
  ExpectResult(runner, overriddenId, &overriddenResult, "环境覆盖场景");
  GC_CHECK_MESSAGE(overriddenResult.completion == CommandCompletion::finished, Describe(overriddenResult));
  GC_CHECK_MESSAGE(overriddenResult.exitCode == 0,
                   "环境覆盖没有传进命令窗口里的 Git（需 Git 2.31+ 支持 GIT_CONFIG_COUNT）：" +
                       Describe(overriddenResult));
}

GC_TEST(command_window_early_close_never_stays_running) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(MakeOperation(fixture, {L"status"}), &id, &failure);
  GC_REQUIRE_MESSAGE(started, "命令窗口启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  // 立刻关闭窗口：结果要么是 Git 真的跑完了（finished），要么是提前关闭（terminated/scriptNeverRan）。
  // 无论如何都必须“有终态”，不能停在执行中。
  runner.CloseOperationWindow(id);
  CommandWindowResult result;
  ExpectResult(runner, id, &result, "提前关窗场景");
  const bool terminal = result.completion == CommandCompletion::finished ||
                        result.completion == CommandCompletion::terminated ||
                        result.completion == CommandCompletion::scriptNeverRan ||
                        result.completion == CommandCompletion::stillUnknown ||
                        result.completion == CommandCompletion::gitNotStarted;
  GC_CHECK_MESSAGE(terminal, Describe(result));
  if (result.completion == CommandCompletion::terminated ||
      result.completion == CommandCompletion::scriptNeverRan) {
    GC_CHECK(!result.failureReason.empty());
  }
}

GC_TEST(command_window_shutdown_does_not_wait_for_git) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  gc::platform::CommandWindowRunner runner;
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(MakeOperation(fixture, {L"status"}), &id, &failure);
  GC_REQUIRE_MESSAGE(started, "命令窗口启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));

  // Shutdown 只 join 观察线程：必须快速返回，且不终止命令窗口里的 Git。
  const auto begin = std::chrono::steady_clock::now();
  runner.Shutdown();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - begin)
                           .count();
  GC_CHECK_MESSAGE(elapsed < 5000, "Shutdown 耗时过长（观察线程没有及时退出）");
  GC_CHECK(runner.ActiveCount() == 0);
  // 应用退出时未完成的操作必须落成终态：可能已经跑完（finished），也可能记为“结果未知”，
  // 但绝不能留在中间态“执行中/已启动”，也不能没有结果（否则界面会永远显示“执行中”）。
  CommandWindowResult abandoned;
  GC_REQUIRE_MESSAGE(runner.TakeResult(id, &abandoned), "Shutdown 后没有为该操作登记终态");
  const bool terminal = abandoned.completion == CommandCompletion::stillUnknown ||
                        abandoned.completion == CommandCompletion::finished ||
                        abandoned.completion == CommandCompletion::gitNotStarted ||
                        abandoned.completion == CommandCompletion::terminated ||
                        abandoned.completion == CommandCompletion::scriptNeverRan;
  GC_CHECK_MESSAGE(terminal, Describe(abandoned));
  if (abandoned.completion == CommandCompletion::stillUnknown) {
    GC_CHECK(!abandoned.failureReason.empty());
  }
  // Shutdown 只结束观察线程，不杀 Git 也不关窗口：这里显式收起窗口，夹具才能回收临时仓库。
  // 关闭请求反复补发：Shutdown 后没有轮询线程帮忙缓存句柄，窗口可能还要一点时间才写好标题。
  const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < closeDeadline && !runner.ConsoleExited(id)) {
    runner.CloseOperationWindow(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  GC_CHECK_MESSAGE(runner.ConsoleExited(id), "关闭命令窗口后 cmd 进程仍未退出");
}

GC_TEST(command_window_rejects_operations_after_shutdown) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  gc::platform::CommandWindowRunner runner;
  runner.Startup(nullptr);
  runner.Shutdown();

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(MakeOperation(fixture, {L"status"}), &id, &failure);
  GC_CHECK(!started);
  GC_CHECK(failure.completion == CommandCompletion::launchFailed);
}
