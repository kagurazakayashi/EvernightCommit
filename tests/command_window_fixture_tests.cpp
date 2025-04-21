// 外部命令窗口执行器的集成测试（对应 AGENTS.md：自动化测试只在自己创建的临时仓库里跑 Git）。
// 覆盖：真实退出码（成功/失败）、命令窗口自身的退出码与 Git 退出码不混同、特殊路径与参数、
// 受控环境覆盖确实传进命令窗口里的 Git、启动前拒绝、提前关窗不卡死、Shutdown 不阻塞，
// 兼容性修复的三条主判据（非 ASCII 临时目录、原样送达的文件名、辅助入口的边界），
// 以及操作目录的生命周期：每次操作的目录名互不相同且用完即回收、
// 还在进行的操作被下一次清扫误不掉（用只读的 `git hash-object --stdin` 造长任务，不用提交拖时间）、
// 标记与结果文件在磁盘上的实际形态带着本次口令。
// 每个用例会打开真实的命令窗口并在结束后关闭，仅在本机桌面环境可完整验证。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/command_window_helper.h"
#include "platform/windows/command_window_runner.h"
#include "platform/windows/raii.h"
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

// 操作目录里某个文件的完整路径（测试自己造的目录也要按 Unicode 拼）。
std::wstring JoinForTest(std::wstring_view base, std::wstring_view relative) {
  std::wstring result(base);
  if (!result.empty() && result.back() != L'\\') {
    result.push_back(L'\\');
  }
  result.append(relative);
  return result;
}

// 命令行整体加引号：与执行器交给辅助进程的那一条同形态（测试目录不含引号）。
std::wstring QuoteArgumentForTest(std::wstring_view value) {
  return L"\"" + std::wstring(value) + L"\"";
}

// 文件名常量在 git 层是 ASCII 字节（观察端按字符串比对），拼 Unicode 路径时逐字符提升即可。
std::wstring WideForTest(std::string_view asciiName) {
  std::wstring result;
  result.reserve(asciiName.size());
  for (const char c : asciiName) {
    result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return result;
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
    case CommandCompletion::helperNeverStarted:
      return "helperNeverStarted";
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

bool ReadFileBytes(const std::wstring& path, std::string& outBytes) {
  outBytes.clear();
  gc::platform::UniqueHandle handle(::CreateFileW(path.c_str(), GENERIC_READ,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                                  nullptr));
  if (!handle) {
    return false;
  }
  char buffer[1024];
  for (;;) {
    DWORD got = 0;
    if (::ReadFile(handle.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr) == 0) {
      return false;
    }
    if (got == 0) {
      return true;
    }
    outBytes.append(buffer, got);
  }
}

// 把活动操作所在的目录推到“已经超过回收阈值”的过去，模拟上一次会话留下的残骸形态。
// 只用 FILE_WRITE_ATTRIBUTES：只改这一个测试自己的目录的最后写入时间。
bool MakeDirectoryLookAncient(const std::wstring& directory) {
  gc::platform::UniqueHandle handle(::CreateFileW(
      directory.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle) {
    return false;
  }
  FILETIME now{};
  ::GetSystemTimeAsFileTime(&now);
  const ULONGLONG back =
      ((static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime) -
      static_cast<ULONGLONG>(gc::platform::kStaleOperationDirectoryAgeTicks) -
      120ULL * 10000000ULL;  // 阈值之外再多留两分钟
  FILETIME stale{};
  stale.dwLowDateTime = static_cast<DWORD>(back & 0xFFFFFFFFu);
  stale.dwHighDateTime = static_cast<DWORD>(back >> 32);
  return ::SetFileTime(handle.get(), nullptr, nullptr, &stale) != 0;
}

bool DirectoryExists(const std::wstring& path) {
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::path(path), ec);
}

// 等到某个操作目录消失（正常结束的收尾是异步的：观察线程落账之后才回收）。
bool WaitForDirectoryGone(const std::wstring& directory) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!DirectoryExists(directory)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !DirectoryExists(directory);
}

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

  // 故意无效参数：必须是 Git 真实返回的非 0 退出码，而不是命令窗口自身的退出码。
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
  // 命令窗口在 Git 结束后仍存活（辅助进程把窗口交给 cmd /k）；进程退出只能由关窗造成，
  // 因此 console 退出码只作诊断，判定成败一律看 result.txt。
  GC_CHECK(result.commandLine.find(L"--definitely-not-an-option") != std::wstring::npos);
}

GC_TEST(command_window_delivers_names_to_git_verbatim) {
  // 本轮修复的主判据：进入命令窗口的路径与参数必须是“数据”，既不能被代码页改写，
  // 也不能因为“本机装不下”而拒绝一次合法操作。
  // 判据不靠读窗口输出，也不只判断“窗口出现了”：用只读的
  //   git ls-files --error-unmatch -- ":(literal)<名字>"
  // 只有名字一个字节都不差地送到 Git、并且工作目录也正确时才返回 0；
  // 名字被改写（例如 %FOO% 被环境变量替换、?/最佳匹配替换）会是非 0，仓库不对则 128。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"仓库 & 空格");
  const std::vector<std::wstring> names = {
      L"中文 目录/文件 & 名.txt",  // 中文 + 空格 + &
      L"100%.txt",                 // 落单的 %：会被当成位置参数引用的形态
      L"x%COMSPEC%y.txt",          // 成对的 %：引用一个必然存在的变量
      L"bang!(x)^& 与括号.txt",     // ! ^ & 与括号
      L"Ж ж emoji 😀 超代码页.txt",  // 西里尔 + emoji：传统代码页装不下
  };
  for (const std::wstring& name : names) {
    fixture.WriteFile(name, "内容\n");
  }
  fixture.StageAll();

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  for (const std::wstring& name : names) {
    uint64_t id = 0;
    CommandWindowResult failure;
    const std::wstring pathspec = L":(literal)" + name;  // 关掉 glob 语义，只留“原样这个名字”
    const bool started = runner.Start(
        MakeOperation(fixture, {L"ls-files", L"--error-unmatch", L"--", pathspec}), &id, &failure);
    GC_REQUIRE_MESSAGE(started, "含中文/元字符的名字被错误拒绝：" +
                                    gc::platform::Utf16ToUtf8(failure.failureReason));
    opened.push_back(id);

    CommandWindowResult result;
    ExpectResult(runner, id, &result, "名字送达校验：" + gc::platform::Utf16ToUtf8(name));
    GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished, Describe(result));
    GC_CHECK_MESSAGE(result.exitCode == 0,
                     "名字没有原样送到 Git（被改写或工作目录不符）：" + Describe(result));
    // 界面展示用的命令行是宽字符原文，不显示任何转义后的模样。
    GC_CHECK_MESSAGE(result.commandLine.find(name) != std::wstring::npos,
                     "展示的命令行应含原始名字：" + gc::platform::Utf16ToUtf8(result.commandLine));
  }
}

GC_TEST(command_window_handles_chinese_and_space_paths) {
  // 仓库目录名同时包含中文、空格与 &：说明书、工作目录与标题都必须原样工作。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"仓库 & 空格");
  fixture.WriteFile(L"中文 目录/文件 & 名.txt", "内容\n");
  fixture.StageAll();

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(
      MakeOperation(fixture,
                    {L"ls-files", L"--error-unmatch", L"--", L":(literal)中文 目录/文件 & 名.txt"}),
      &id, &failure);
  GC_REQUIRE_MESSAGE(started,
                     "中文/空格/& 路径启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "中文/空格路径场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished && result.exitCode == 0,
                   Describe(result));
  GC_CHECK(result.repositoryDirectory == fixture.RepoDir());
  // 窗口标题带中文与非 ASCII 前缀，同时必须保留唯一标记（执行器靠它定位窗口）。
  GC_CHECK(result.displayName == L"status");
}

GC_TEST(command_window_keeps_metacharacter_arguments_literal) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  // 文件名里塞满 shell 元字符与中文：作为参数交给辅助进程时必须原样到达 Git。
  // 只能挑 NTFS 合法字符（| < > : " / \ ? * 是非法文件名字符）。
  const std::wstring name = L"特殊 &^%!(x) 文件.txt";
  fixture.WriteFile(name, "hello\n");
  fixture.StageAll();

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  // 只读查询：git ls-files --error-unmatch -- ":(literal)<名字>"。
  // 名字里任何一个字符被改写都会让 pathspec 落空，退出码立刻变成非 0。
  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(
      MakeOperation(fixture, {L"ls-files", L"--error-unmatch", L"--", L":(literal)" + name}), &id,
      &failure);
  GC_REQUIRE_MESSAGE(started, "元字符参数被错误拒绝：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "元字符参数场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished && result.exitCode == 0, Describe(result));
  // 展示用的命令行必须是宽字符原文，不能被任何编码形态改写。
  GC_CHECK_MESSAGE(result.commandLine.find(name) != std::wstring::npos, Describe(result));
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

  // 立刻关闭窗口：结果要么是 Git 真的跑完了（finished），要么是提前关闭（terminated/helperNeverStarted）。
  // 无论如何都必须“有终态”，不能停在执行中。
  runner.CloseOperationWindow(id);
  CommandWindowResult result;
  ExpectResult(runner, id, &result, "提前关窗场景");
  const bool terminal = result.completion == CommandCompletion::finished ||
                        result.completion == CommandCompletion::terminated ||
                        result.completion == CommandCompletion::helperNeverStarted ||
                        result.completion == CommandCompletion::stillUnknown ||
                        result.completion == CommandCompletion::gitNotStarted;
  GC_CHECK_MESSAGE(terminal, Describe(result));
  if (result.completion == CommandCompletion::terminated ||
      result.completion == CommandCompletion::helperNeverStarted) {
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
                        abandoned.completion == CommandCompletion::helperNeverStarted;
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

GC_TEST(command_window_works_with_non_ascii_temp_directory) {
  // 验收项 11 的回归：中文用户名会把 %TEMP% 变成非 ASCII 路径，命令窗口照样要能用。
  // 旧实现要求操作目录能编码成纯 ASCII 字节（标记文件靠 cmd 的重定向创建），
  // 于是“中文用户名的机器一律打不开命令窗口”。现在标记与结果文件都由 Unicode API 读写，
  // 这个前提已经不存在 —— 本用例把本进程的临时目录指向夹具里的中文目录来实测它。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();

  std::error_code ec;
  const std::wstring chineseTemp = fixture.PathInRoot(L"中文 临时目录");
  GC_REQUIRE(std::filesystem::create_directories(chineseTemp, ec) && !ec,
             "创建中文临时目录失败：" + gc::platform::Utf16ToUtf8(chineseTemp));

  // 环境变量只改本进程（不碰系统设置、不碰注册表），用例结束立刻还原。
  class TempOverride {
  public:
    TempOverride() {
      Save(L"TEMP");
      Save(L"TMP");
    }
    ~TempOverride() {
      for (const auto& entry : saved_) {
        static_cast<void>(::SetEnvironmentVariableW(entry.first.c_str(), entry.second.c_str()));
      }
    }
    void Apply(std::wstring_view name, std::wstring_view value) {
      static_cast<void>(::SetEnvironmentVariableW(std::wstring(name).c_str(),
                                                 std::wstring(value).c_str()));
    }

  private:
    void Save(std::wstring_view name) {
      std::wstring buffer(4096, L'\0');
      const DWORD length = ::GetEnvironmentVariableW(std::wstring(name).c_str(), buffer.data(),
                                                    static_cast<DWORD>(buffer.size()));
      if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        saved_.emplace_back(std::wstring(name), buffer);
      } else {
        saved_.emplace_back(std::wstring(name), std::wstring());
      }
    }
    std::vector<std::pair<std::wstring, std::wstring>> saved_;
  } tempOverride;
  tempOverride.Apply(L"TEMP", chineseTemp);
  tempOverride.Apply(L"TMP", chineseTemp);

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started = runner.Start(MakeOperation(fixture, {L"status", L"--porcelain=v1"}), &id,
                                   &failure);
  GC_REQUIRE_MESSAGE(started,
                     "中文临时目录被错误拒绝：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  CommandWindowResult result;
  ExpectResult(runner, id, &result, "中文临时目录场景");
  GC_CHECK_MESSAGE(result.completion == CommandCompletion::finished && result.exitCode == 0,
                   Describe(result));
  // 操作目录确实落在中文根之下：证明没有悄悄退回别的根目录来“绕过”这个问题。
  GC_CHECK_MESSAGE(result.directory.find(L"中文 临时目录") != std::wstring::npos,
                   "操作目录不在中文临时根下：" + gc::platform::Utf16ToUtf8(result.directory));
}

GC_TEST(command_window_operation_directories_are_unique_and_reclaimed) {
  // 每次操作都要有一个猜不到的目录，用完还要收干净：
  // 名字能被猜到 → 别人可以抢先占住它，甚至往里放一条“旧的成功”；
  // 用完不收 → %TEMP% 里堆满残骸，回收就成了唯一的清道夫。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  std::wstring dirA;
  std::wstring dirB;
  {
    gc::platform::CommandWindowRunner runner;
    std::vector<uint64_t> opened;
    RunnerGuard guard{&runner, &opened};
    runner.Startup(nullptr);

    uint64_t first = 0;
    CommandWindowResult failure;
    GC_REQUIRE_MESSAGE(runner.Start(MakeOperation(fixture, {L"--version"}), &first, &failure),
                       gc::platform::Utf16ToUtf8(failure.failureReason));
    opened.push_back(first);
    uint64_t second = 0;
    GC_REQUIRE_MESSAGE(runner.Start(MakeOperation(fixture, {L"--version"}), &second, &failure),
                       gc::platform::Utf16ToUtf8(failure.failureReason));
    opened.push_back(second);

    dirA = runner.OperationDirectory(first);
    dirB = runner.OperationDirectory(second);
    GC_REQUIRE(!dirA.empty() && !dirB.empty(), "操作目录没被登记");
    GC_CHECK_MESSAGE(dirA != dirB, "两次操作共用了同一个目录：" + gc::platform::Utf16ToUtf8(dirA));
    const std::wstring leafA = dirA.substr(dirA.find_last_of(L"\\") + 1);
    const std::wstring leafB = dirB.substr(dirB.find_last_of(L"\\") + 1);
    GC_CHECK(gc::git::IsSafeOperationDirectoryName(leafA));
    GC_CHECK(gc::git::IsSafeOperationDirectoryName(leafB));
    // 分隔 x 之后的随机段必须不同：同一进程里连续两次操作靠它区分，序号已经不参与命名。
    const size_t splitA = leafA.find(L'x');
    const size_t splitB = leafB.find(L'x');
    GC_REQUIRE(splitA != std::wstring::npos && splitB != std::wstring::npos, "目录名形态不对");
    GC_CHECK(leafA.substr(splitA) != leafB.substr(splitB));

    CommandWindowResult resultA;
    CommandWindowResult resultB;
    ExpectResult(runner, first, &resultA, "第一次 --version");
    ExpectResult(runner, second, &resultB, "第二次 --version");
    GC_CHECK_MESSAGE(resultA.completion == CommandCompletion::finished && resultA.exitCode == 0,
                     Describe(resultA));
    GC_CHECK_MESSAGE(resultB.completion == CommandCompletion::finished && resultB.exitCode == 0,
                     Describe(resultB));
  }  // RunnerGuard 析构：关窗 + Shutdown

  GC_CHECK_MESSAGE(WaitForDirectoryGone(dirA),
                   "正常结束后没有回收自己的目录：" + gc::platform::Utf16ToUtf8(dirA));
  GC_CHECK_MESSAGE(WaitForDirectoryGone(dirB),
                   "正常结束后没有回收自己的目录：" + gc::platform::Utf16ToUtf8(dirB));
}

GC_TEST(command_window_active_operation_survives_stale_sweep) {
  // 验收项 2 与 7 合起来的真实形态：一次还在跑的操作，目录时间被推到超过阈值之后，
  // 另一个执行器实例来清扫也必须拿它没办法；等它自己结束才被回收。
  // 长任务用只读的 `git hash-object --stdin`：它停在控制台输入上等 EOF，
  // 既没有任何副作用，也不需要造一次提交来拖时间（本用例全程不 commit、不 push）。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  uint64_t id = 0;
  CommandWindowResult failure;
  const bool started =
      runner.Start(MakeOperation(fixture, {L"hash-object", L"--stdin"}), &id, &failure);
  GC_REQUIRE_MESSAGE(started, "命令窗口启动失败：" + gc::platform::Utf16ToUtf8(failure.failureReason));
  opened.push_back(id);

  // 等到“执行中”：这要求开始标记里的口令在生产链路上核对成功（读不出归属只会停在“已启动”）。
  std::wstring status;
  const auto runningDeadline = std::chrono::steady_clock::now() + kOperationBudget;
  while (std::chrono::steady_clock::now() < runningDeadline) {
    runner.DescribeOperation(id, &status, nullptr);
    if (status == L"执行中") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  GC_REQUIRE_MESSAGE(status == L"执行中",
                     "活动操作没进入“执行中”，开始标记的归属核对可能失效：" +
                         gc::platform::Utf16ToUtf8(status));

  const std::wstring directory = runner.OperationDirectory(id);
  GC_REQUIRE(!directory.empty(), "取不到活动操作的目录");
  GC_REQUIRE_MESSAGE(MakeDirectoryLookAncient(directory), "把活动目录的时间推到过去失败");

  gc::platform::CommandWindowRunner other;
  other.Startup(nullptr);  // 启动即清扫一次
  other.SweepStaleOperationDirectories();
  GC_CHECK_MESSAGE(DirectoryExists(directory),
                   "还在进行的操作的目录被下一次清扫误删了：" + gc::platform::Utf16ToUtf8(directory));
  GC_CHECK(runner.ActiveCount() == 1);
  runner.DescribeOperation(id, &status, nullptr);
  GC_CHECK_MESSAGE(status == L"执行中",
                   "清扫之后活动任务的状态被改写：" + gc::platform::Utf16ToUtf8(status));
  CommandWindowResult early;
  GC_CHECK_MESSAGE(!runner.TakeResult(id, &early),
                   "活动任务还没结束就有结果，疑似把旧结果当成了新的");

  // 保留必须留下记录，而不是“静默没删”。
  bool recorded = false;
  for (const std::wstring& entry : other.PreservedOperationDirectories()) {
    if (entry.find(directory) != std::wstring::npos) {
      recorded = true;
    }
  }
  GC_CHECK_MESSAGE(recorded, "活动目录被保留时没有记下原因");

  // 收起命令窗口：辅助进程与它握着的开始标记一起结束，操作才有终态、目录才可被回收。
  other.Shutdown();
  const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < closeDeadline && !runner.ConsoleExited(id)) {
    runner.CloseOperationWindow(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  CommandWindowResult result;
  ExpectResult(runner, id, &result, "关窗后的终态");
  const bool terminal = result.completion != CommandCompletion::launched &&
                        result.completion != CommandCompletion::running &&
                        result.completion != CommandCompletion::launchFailed;
  GC_CHECK_MESSAGE(terminal, Describe(result));
  GC_CHECK_MESSAGE(WaitForDirectoryGone(directory),
                   "操作结束后目录没被回收：" + gc::platform::Utf16ToUtf8(directory));
}

GC_TEST(command_window_helper_refuses_foreign_requests) {
  // 辅助入口不能沦为“绕过界面确认的通用命令执行通道”。这里绕开执行器，直接按那个命令行
  // 形态启动自己：伪造口令、伪造目录名两种花样，都要求它拒绝执行 ——
  // 既不写 start.txt（说明它没打算执行任何操作），也不写 result.txt（更没有 Git 的退出码）。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  wchar_t tempRoot[MAX_PATH + 4]{};
  const DWORD rootLength = ::GetTempPathW(static_cast<DWORD>(MAX_PATH), tempRoot);
  GC_REQUIRE(rootLength > 0 && rootLength < MAX_PATH, "取不到系统临时目录");
  // 目录名必须是执行器会生成的形态，否则第一步就被拒（那种拒绝不针对本用例要验的绑定）。
  static constexpr std::wstring_view kDirectoryName = L"GcOp900001xdeadbeef1234";
  const std::wstring directory = std::wstring(tempRoot, rootLength) + kDirectoryName.data();
  // “目录已存在”不是所有权证明：存在即判为前置失败，绝不往别人的目录里写东西。
  const BOOL created = ::CreateDirectoryW(directory.c_str(), nullptr);
  GC_REQUIRE(created != 0, "伪造操作目录已存在或被占用，本用例需要在独占目录里取证");

  class DirectoryGuard {
  public:
    explicit DirectoryGuard(std::wstring path) : path_(std::move(path)) {}
    ~DirectoryGuard() {
      for (std::string_view name : gc::git::kOperationFileNames) {
        static_cast<void>(::DeleteFileW(JoinForTest(path_, WideForTest(name)).c_str()));
      }
      static_cast<void>(::RemoveDirectoryW(path_.c_str()));
    }

  private:
    std::wstring path_;
  } directoryGuard(directory);

  wchar_t modulePath[32768]{};
  const DWORD moduleLength = ::GetModuleFileNameW(nullptr, modulePath, 32768);
  GC_REQUIRE(moduleLength > 0 && moduleLength < 32768, "取不到测试可执行文件路径");
  const std::wstring executable(modulePath, moduleLength);

  // 说明书由生产代码同一份序列化函数生成，之后只改“要它拒绝”的那一个字段。
  gc::git::CommandWindowOperation operation;
  operation.operationId = L"status";
  operation.displayName = L"status";
  operation.gitExecutable = fixture.GitExe();
  operation.repositoryDirectory = fixture.RepoDir();
  operation.arguments = {L"--version"};  // 只读：万一“竟然通过了校验”，也不会改动任何仓库
  const std::wstring title =
      gc::git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", L"status", kDirectoryName);

  const auto writeSpec = [&](std::wstring_view nonce, std::wstring_view token) {
    std::wstring specText;
    std::wstring detail;
    const bool built = gc::git::BuildCommandWindowSpecText(operation, token, title, nonce,
                                                           &specText, nullptr, &detail);
    GC_REQUIRE_MESSAGE(built, gc::platform::Utf16ToUtf8(detail));
    std::string bytes;
    GC_REQUIRE(gc::platform::TryUtf16ToUtf8Strict(specText, bytes), "说明书编码失败");
    const std::wstring specPath = JoinForTest(directory, WideForTest(gc::git::kSpecFileName));
    const HANDLE raw = ::CreateFileW(specPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    GC_REQUIRE(raw != INVALID_HANDLE_VALUE, "写说明书失败");
    gc::platform::UniqueHandle handle(raw);
    DWORD written = 0;
    GC_REQUIRE(::WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                           nullptr) != 0 &&
                   written == bytes.size(),
               "写说明书不完整");
  };

  const auto runHelper = [&](std::wstring_view nonce) {
    const std::wstring commandLine = QuoteArgumentForTest(executable) + L" " +
                                     QuoteArgumentForTest(gc::platform::kCommandWindowHelperSwitch) + L" " +
                                     QuoteArgumentForTest(directory) + L" " +
                                     QuoteArgumentForTest(nonce);
    std::wstring mutableCommandLine = commandLine;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    // 不给 CREATE_NEW_CONSOLE：辅助进程自己 FreeConsole + AllocConsole，
    // 所以这里的启动形态与正式程序完全一致。
    const BOOL launched =
        ::CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                         CREATE_UNICODE_ENVIRONMENT, nullptr, fixture.RepoDir().c_str(), &startup,
                         &information);
    GC_REQUIRE_MESSAGE(launched != 0, "无法启动辅助进程做边界取证");
    gc::platform::UniqueHandle process(information.hProcess);
    gc::platform::UniqueHandle thread(information.hThread);
    // 拒绝路径应当在秒级内结束：它不启动 Git，也不会把窗口交给 cmd。
    const DWORD wait = ::WaitForSingleObject(process.get(), 20000);
    unsigned long exitCode = 0;
    static_cast<void>(::GetExitCodeProcess(process.get(), &exitCode));
    GC_REQUIRE_MESSAGE(wait == WAIT_OBJECT_0, "辅助进程在拒绝路径上没有退出（等了 20 秒）");
    return exitCode;
  };

  // ① 说明书里的口令与被启动时传来的不符：两者不是同一件事。
  writeSpec(L"aaaaaaaaaaaaaaaa", kDirectoryName);
  const unsigned long wrongNonceExit = runHelper(L"bbbbbbbbbbbbbbbb");
  GC_CHECK_MESSAGE(wrongNonceExit != 0, "口令不符时辅助入口竟然执行了");
  GC_CHECK(::GetFileAttributesW(
               JoinForTest(directory, WideForTest(gc::git::kStartMarkerFileName)).c_str()) ==
           INVALID_FILE_ATTRIBUTES);
  GC_CHECK(::GetFileAttributesW(
               JoinForTest(directory, WideForTest(gc::git::kResultFileName)).c_str()) ==
           INVALID_FILE_ATTRIBUTES);

  // ② 口令相符，但说明书声明的目录名与实际所在目录不符。
  writeSpec(L"aaaaaaaaaaaaaaaa", L"GcOp1xabcd1234");
  const unsigned long wrongTokenExit = runHelper(L"aaaaaaaaaaaaaaaa");
  GC_CHECK_MESSAGE(wrongTokenExit != 0, "目录名不符时辅助入口竟然执行了");
  GC_CHECK(::GetFileAttributesW(
               JoinForTest(directory, WideForTest(gc::git::kStartMarkerFileName)).c_str()) ==
           INVALID_FILE_ATTRIBUTES);

  // ③ 正向路径（同一个入口，口令与目录都对得上）：它照说明书执行 Git，并把执行痕迹写成
  //    “带本次口令、完整发布”的形态 —— 观察端只认这一份，别人留下的旧记录一概不算成功。
  //    窗口按设计交给 cmd /k 保留，用例最后按标题里的唯一标记把它收起。
  const std::string nonceBytes(16, 'a');  // 与下面 writeSpec 用的 L"aaaaaaaaaaaaaaaa" 同一个值
  writeSpec(L"aaaaaaaaaaaaaaaa", kDirectoryName);
  const std::wstring positiveArgument = QuoteArgumentForTest(executable) + L" " +
                                        QuoteArgumentForTest(gc::platform::kCommandWindowHelperSwitch) +
                                        L" " + QuoteArgumentForTest(directory) + L" " +
                                        QuoteArgumentForTest(L"aaaaaaaaaaaaaaaa");
  std::wstring positiveCommandLine = positiveArgument;
  STARTUPINFOW positiveStartup{};
  positiveStartup.cb = sizeof(positiveStartup);
  PROCESS_INFORMATION positiveInfo{};
  const BOOL positiveLaunched =
      ::CreateProcessW(executable.c_str(), positiveCommandLine.data(), nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, nullptr, fixture.RepoDir().c_str(),
                       &positiveStartup, &positiveInfo);
  GC_REQUIRE_MESSAGE(positiveLaunched != 0, "正向路径没能启动辅助进程");
  gc::platform::UniqueHandle positiveProcess(positiveInfo.hProcess);
  gc::platform::UniqueHandle positiveThread(positiveInfo.hThread);

  const std::wstring markerPath = JoinForTest(directory, WideForTest(gc::git::kStartMarkerFileName));
  const std::wstring resultPath = JoinForTest(directory, WideForTest(gc::git::kResultFileName));
  const std::wstring tempResultPath =
      JoinForTest(directory, WideForTest(gc::git::kResultTempFileName));
  std::string markerBytes;
  std::string resultBytes;
  const auto recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < recordDeadline) {
    static_cast<void>(ReadFileBytes(markerPath, markerBytes));
    static_cast<void>(ReadFileBytes(resultPath, resultBytes));
    if (!markerBytes.empty() && !resultBytes.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  GC_REQUIRE_MESSAGE(!markerBytes.empty() && !resultBytes.empty(),
                     "正向路径没写出标记或结果：" + DumpOperationDirectory(directory));
  GC_CHECK_MESSAGE(markerBytes == "start\t" + nonceBytes + "\r\n",
                   "开始标记不是约定的形态：" + markerBytes);
  GC_CHECK(gc::git::ParseCommandWindowStartMarker(markerBytes, nonceBytes));
  GC_CHECK(!gc::git::ParseCommandWindowStartMarker(markerBytes, "bbbbbbbbbbbbbbbb"));
  long gitExit = -1;
  GC_CHECK_MESSAGE(gc::git::ParseCommandWindowResult(resultBytes, nonceBytes, &gitExit),
                   "结果行没能按本次口令解析：" + resultBytes);
  GC_CHECK_MESSAGE(gitExit == 0, "git --version 应当以 0 退出，实际 " + std::to_string(gitExit));
  GC_CHECK(!gc::git::ParseCommandWindowResult(resultBytes, "bbbbbbbbbbbbbbbb", &gitExit));
  // 结果由临时名改名发布：正式名一出现内容就是完整的，暂存名不该留下任何痕迹。
  GC_CHECK_MESSAGE(::GetFileAttributesW(tempResultPath.c_str()) == INVALID_FILE_ATTRIBUTES,
                   "结果发布后留下了临时名：" + DumpOperationDirectory(directory));

  HWND consoleWindow = nullptr;
  const auto findDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < findDeadline) {
    consoleWindow = ::FindWindowW(nullptr, title.c_str());
    if (consoleWindow != nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  GC_REQUIRE_MESSAGE(consoleWindow != nullptr,
                     "正向路径的命令窗口没能按标题找到，用例无法收尾：" +
                         gc::platform::Utf16ToUtf8(title));
  static_cast<void>(::PostMessageW(consoleWindow, WM_CLOSE, 0, 0));
  static_cast<void>(::WaitForSingleObject(positiveProcess.get(), 20000));
}
