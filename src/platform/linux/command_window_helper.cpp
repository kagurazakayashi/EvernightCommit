#include "platform/linux/command_window_helper.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/command_window.h"
#include "git/shell_text.h"
#include "platform/linux/environment.h"
#include "platform/linux/operation_directory.h"
#include "platform/path.h"
#include "platform/text_encoding.h"

extern char** environ;

namespace gc::platform {
namespace {

constexpr size_t kMaxSpecBytes = 256u * 1024u;
constexpr size_t kMaxResultBytes = 64u * 1024u;

std::wstring ErrnoText(int error) {
  return L"errno " + std::to_wstring(error) + L"（" + Utf8ToWide(std::strerror(error)) + L"）";
}

bool WriteAll(int fd, std::string_view bytes) {
  size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (count == 0) {
      return false;
    }
    written += static_cast<size_t>(count);
  }
  return true;
}

void WriteText(const std::string& text) { static_cast<void>(WriteAll(STDOUT_FILENO, text)); }

void WriteWideText(std::wstring_view text) {
  std::string bytes;
  if (TryWideToUtf8Strict(text, bytes)) {
    WriteText(bytes);
  }
}

// 终端标题只走 OSC 转义序列；标题在说明书解析时已经拒绝控制字符，不会伪造出别的
// 转义序列。非 tty（测试直接启动 helper 时）不写，避免把转义码混进捕获输出。
void ApplyTerminalTitle(std::wstring_view title) {
  if (::isatty(STDOUT_FILENO) == 0) {
    return;
  }
  std::string bytes;
  if (!TryWideToUtf8Strict(title, bytes)) {
    return;
  }
  std::string sequence = "\x1b]0;";
  sequence += bytes;
  sequence.push_back('\a');
  static_cast<void>(WriteAll(STDOUT_FILENO, sequence));
}

bool LoadSpec(int dirFd, std::string_view directoryName, std::wstring_view expectedNonce,
              git::CommandWindowSpec* outSpec, std::wstring* failureReason) {
  std::string bytes;
  bool exists = false;
  if (!ReadOperationFileAt(dirFd, git::kSpecFileName, kMaxSpecBytes, &bytes, &exists,
                           failureReason)) {
    return false;
  }
  if (!exists) {
    *failureReason = L"操作说明书不存在（可能已被清理，或辅助入口收到的目录不属于本次操作）。";
    return false;
  }
  std::wstring text;
  if (!TryUtf8ToWideStrict(bytes, text)) {
    *failureReason = L"操作说明书不是合法的 UTF-8 文本，拒绝执行。";
    return false;
  }
  git::CommandWindowSpec spec;
  std::wstring parseReason;
  if (!git::ParseCommandWindowSpecText(text, &spec, &parseReason)) {
    *failureReason = parseReason;
    return false;
  }
  if (spec.nonce != expectedNonce) {
    *failureReason = L"操作说明书的口令与本次启动不符，拒绝执行。";
    return false;
  }
  if (spec.directoryToken != Utf8ToWide(directoryName)) {
    *failureReason = L"操作说明书声明的目录与它实际所在目录不符，拒绝执行。";
    return false;
  }
  if (spec.gitExecutable.empty() || !IsAbsolutePath(spec.gitExecutable)) {
    *failureReason = L"操作说明书里的 Git 程序不是绝对路径，拒绝执行。";
    return false;
  }
  git::CommandWindowOperation operation;
  operation.operationId = spec.operationId;
  operation.displayName = spec.operationId;
  operation.gitExecutable = spec.gitExecutable;
  operation.repositoryDirectory = spec.workingDirectory;
  operation.arguments = spec.arguments;
  operation.environmentOverrides = spec.environmentOverrides;
  std::wstring detail;
  git::CommandPlanReject reject = git::CommandPlanReject::none;
  std::wstring line;
  if (!git::BuildGitCommandLineForDialect(operation, git::CommandLineDialect::posixArgv, &line,
                                          &reject, &detail)) {
    *failureReason = L"操作说明书的内容没有通过执行边界校验：" + detail;
    return false;
  }
  *outSpec = std::move(spec);
  return true;
}

bool IsTerminalInteractive() { return ::isatty(STDIN_FILENO) != 0; }

// Git 与辅助进程共用一个终端：Git 继承辅助进程的 0/1/2，hooks、SSH 口令、GPG 签名与
// 编辑器都能原生交互。辅助进程忽略 SIGINT/SIGQUIT，这样 Ctrl-C 只结束 Git，辅助进程
// 仍能等到退出码并发布结果；子进程通过 posix_spawn 的 sigdefault 恢复默认动作。
bool InstallHelperSignalPolicy(std::wstring* failureReason) {
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  for (const int signalNumber : {SIGINT, SIGQUIT, SIGPIPE}) {
    if (::sigaction(signalNumber, &ignore, nullptr) != 0) {
      *failureReason = L"无法设置辅助进程信号策略：" + ErrnoText(errno);
      return false;
    }
  }
  return true;
}

struct SpawnedProcess {
  bool started = false;
  pid_t pid = -1;
  std::wstring failureReason;
};

SpawnedProcess SpawnGit(const git::CommandWindowSpec& spec,
                        const std::vector<std::string>& environmentEntries) {
  SpawnedProcess result;
  std::string programBytes;
  if (!TryPathBytesFromWide(spec.gitExecutable, programBytes) ||
      programBytes.find('\0') != std::string::npos) {
    result.failureReason = L"Git 程序路径无法无损编码成 UTF-8 文件名。";
    return result;
  }
  std::vector<std::string> argumentBytes;
  argumentBytes.reserve(spec.arguments.size() + 1);
  for (const std::wstring& argument : spec.arguments) {
    std::string bytes;
    if (!TryWideToUtf8Strict(argument, bytes) || bytes.find('\0') != std::string::npos) {
      result.failureReason = L"Git 参数无法无损编码成 UTF-8。";
      return result;
    }
    argumentBytes.push_back(std::move(bytes));
  }
  std::vector<char*> argv;
  argv.reserve(argumentBytes.size() + 2);
  argv.push_back(programBytes.data());
  for (std::string& argument : argumentBytes) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::vector<char*> envp;
  envp.reserve(environmentEntries.size() + 1);
  for (const std::string& entry : environmentEntries) {
    envp.push_back(const_cast<char*>(entry.c_str()));
  }
  envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
  int actionError = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
  int actionError = 0;
#endif
#else
  int actionError = 0;
#endif

  sigset_t defaultSignals;
  sigemptyset(&defaultSignals);
  for (const int signalNumber : {SIGINT, SIGQUIT, SIGHUP, SIGTERM, SIGPIPE, SIGCHLD}) {
    sigaddset(&defaultSignals, signalNumber);
  }
  sigset_t emptyMask;
  sigemptyset(&emptyMask);
  posix_spawnattr_setsigdefault(&attributes, &defaultSignals);
  posix_spawnattr_setsigmask(&attributes, &emptyMask);
  posix_spawnattr_setflags(&attributes,
                           POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
  if (actionError != 0) {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    result.failureReason = L"无法限制 Git 子进程继承的描述符：" + ErrnoText(actionError);
    return result;
  }

  pid_t pid = -1;
  const int spawnError =
      posix_spawn(&pid, programBytes.c_str(), &actions, &attributes, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (spawnError != 0) {
    result.failureReason = L"Git 进程未能创建：" + ErrnoText(spawnError);
    return result;
  }
  result.started = true;
  result.pid = pid;
  return result;
}

std::wstring DescribeCommand(const git::CommandWindowSpec& spec) {
  const git::ShellCommandText formatted =
      git::FormatShellCommand(spec.gitExecutable, spec.arguments, git::ShellDialect::posixSh);
  if (formatted.expressible) {
    return formatted.text;
  }
  return git::FormatCommandPreview(spec.gitExecutable, spec.arguments);
}

int WaitForProcess(pid_t pid, int* outCode, std::wstring* failureReason) {
  int status = 0;
  for (;;) {
    const pid_t waited = ::waitpid(pid, &status, 0);
    if (waited == pid) {
      break;
    }
    if (waited < 0) {
      if (errno == EINTR) {
        continue;
      }
      *failureReason = L"等待 Git 进程失败：" + ErrnoText(errno);
      return -1;
    }
  }
  if (WIFEXITED(status)) {
    *outCode = WEXITSTATUS(status);
    return 0;
  }
  if (WIFSIGNALED(status)) {
    *outCode = 128 + WTERMSIG(status);
    return 1;
  }
  *outCode = 128;
  return 1;
}

void HoldTerminal(std::wstring_view note) {
  WriteWideText(L"\n[EvernightCommit] ");
  WriteWideText(note);
  WriteWideText(L"\n[EvernightCommit] 窗口保持打开，按 Enter 关闭（不会结束任何仍在运行的进程）。\n");
  if (!IsTerminalInteractive()) {
    return;
  }
  char buffer[256];
  for (;;) {
    const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (got <= 0) {
      if (got < 0 && errno == EINTR) {
        continue;
      }
      return;
    }
    for (ssize_t index = 0; index < got; ++index) {
      if (buffer[index] == '\n') {
        return;
      }
    }
  }
}

int Execute(const std::string& operationDirectory, std::wstring_view nonce,
            std::wstring_view scratchRootOverride) {
  std::wstring directoryPath;
  if (!TryUtf8ToWideStrict(operationDirectory, directoryPath)) {
    WriteWideText(L"[EvernightCommit] 操作目录不是合法的 UTF-8 路径，本次操作未被执行。\n");
    return 1;
  }
  OpenedOperationDirectory directory =
      OpenOperationDirectoryForHelper(directoryPath, scratchRootOverride);
  if (!directory.ok) {
    WriteWideText(L"[EvernightCommit] 本次操作未被执行：" + directory.failureReason + L"\n");
    return 1;
  }
  struct DirectoryGuard {
    OpenedOperationDirectory* directory;
    ~DirectoryGuard() { CloseOpenedOperationDirectory(directory); }
  } directoryGuard{&directory};

  git::CommandWindowSpec spec;
  std::wstring failure;
  if (!LoadSpec(directory.dirFd, directory.name, nonce, &spec, &failure)) {
    WriteWideText(L"[EvernightCommit] 本次操作未被执行：" + failure + L"\n");
    return 1;
  }

  ApplyTerminalTitle(spec.title);
  const std::wstring display = DescribeCommand(spec);
  WriteWideText(display + L"\n");

  // 开始标记 + flock：观察端据此区分“辅助进程已开始执行”和“窗口里什么都没有”。
  // 句柄一直持有到结果发布完成，因此终端被关闭时锁随进程释放，观察端能判出
  // “Git 可能跑到一半被带走”，而不是永远停在执行中。
  const std::string asciiNonce = WideToUtf8(nonce);
  const int startMarkerFd = CreateHeldStartMarkerAt(
      directory.dirFd, git::BuildCommandWindowStartMarkerText(asciiNonce), &failure);
  if (startMarkerFd < 0) {
    WriteWideText(L"[EvernightCommit] 无法写出开始标记，操作未执行：" + failure + L"\n");
    return 2;
  }
  struct StartMarkerGuard {
    int fd;
    ~StartMarkerGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } startMarker{startMarkerFd};

  const auto publish = [&](long exitCode, bool gitLaunched) {
    std::wstring publishFailure;
    const bool published = PublishResultFileAt(
        directory.dirFd, git::kResultTempFileName, git::kResultFileName,
        git::BuildCommandWindowResultText(asciiNonce, exitCode, gitLaunched), &publishFailure);
    if (!published) {
      WriteWideText(L"[EvernightCommit] " + publishFailure + L"\n");
    }
    return published;
  };

  if (!IsExecutableRegularFile(spec.gitExecutable)) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] Git 程序不存在或不可执行：" + spec.gitExecutable + L"\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 未能启动。");
    }
    return 3;
  }
  if (!IsExistingDirectory(spec.workingDirectory)) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] 仓库目录不存在：" + spec.workingDirectory + L"\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 未能启动。");
    }
    return 3;
  }

  const PosixChildEnvironment environment =
      BuildGitChildEnvironment(git::GitRunPurpose::commandWindow, spec.environmentOverrides);
  if (environment.entries.empty()) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] Git 环境装配失败：" + environment.failureReason + L"\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 未能启动。");
    }
    return 3;
  }
  if (!environment.notice.empty()) {
    WriteWideText(L"[EvernightCommit] " + environment.notice + L"\n");
  }

  if (!InstallHelperSignalPolicy(&failure)) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] " + failure + L"\n");
    return 3;
  }

  // 工作目录由参数数组之外的进程属性绑定：先 chdir 再 spawn，不把路径拼进命令行。
  // 这里只在辅助进程自己的副本里 chdir，不影响任何其他进程。
  std::string workingDirectoryBytes;
  if (!TryPathBytesFromWide(spec.workingDirectory, workingDirectoryBytes) ||
      workingDirectoryBytes.find('\0') != std::string::npos) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] 仓库目录无法无损编码成 UTF-8 文件名。\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 未能启动。");
    }
    return 3;
  }
  if (::chdir(workingDirectoryBytes.c_str()) != 0) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] 无法进入仓库目录：" + ErrnoText(errno) + L"\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 未能启动。");
    }
    return 3;
  }

  const SpawnedProcess spawned = SpawnGit(spec, environment.entries);
  if (!spawned.started) {
    static_cast<void>(publish(0, /*gitLaunched=*/false));
    WriteWideText(L"[EvernightCommit] " + spawned.failureReason + L"\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 未能启动。");
    }
    return 3;
  }

  int gitExitCode = 0;
  const int waitResult = WaitForProcess(spawned.pid, &gitExitCode, &failure);
  if (waitResult < 0) {
    // Git 已创建但本进程拿不到退出码：不写“完成”，让观察端按结果缺失处理。
    WriteWideText(L"[EvernightCommit] " + failure + L"\n");
    if (spec.holdTerminalOpen) {
      HoldTerminal(L"Git 结果未知。");
    }
    return 4;
  }
  static_cast<void>(publish(static_cast<long>(gitExitCode), /*gitLaunched=*/true));
  // 结果已发布，开始标记的锁可以放手；观察端的回收探测从此可以正常判断目录是否还在用。
  startMarker.fd = -1;
  ::close(startMarkerFd);

  if (spec.holdTerminalOpen) {
    HoldTerminal(waitResult == 0
                     ? L"Git 已退出，退出码 " + std::to_wstring(gitExitCode) + L"。"
                     : L"Git 被信号终止（按 128+信号号报告为 " + std::to_wstring(gitExitCode) + L"）。");
  }
  return 0;
}

}  // namespace

bool RunTerminalHelperIfRequested(int argc, char** argv, int* exitCode) {
  if (exitCode != nullptr) {
    *exitCode = 0;
  }
  if (argc < 2 || argv == nullptr || std::string_view(argv[1]) != kTerminalHelperSwitch) {
    return false;
  }
  if ((argc != 4 && argc != 5) || argv[2] == nullptr || argv[3] == nullptr ||
      (argc == 5 && argv[4] == nullptr)) {
    if (exitCode != nullptr) {
      *exitCode = 1;
    }
    return true;
  }
  const std::wstring nonce = Utf8ToWide(argv[3]);
  if (!git::IsSafeNonce(nonce)) {
    if (exitCode != nullptr) {
      *exitCode = 1;
    }
    return true;
  }
  std::wstring scratchRootOverride;
  if (argc == 5 && !TryUtf8ToWideStrict(argv[4], scratchRootOverride)) {
    if (exitCode != nullptr) {
      *exitCode = 1;
    }
    return true;
  }
  const int code = Execute(std::string(argv[2]), nonce, scratchRootOverride);
  if (exitCode != nullptr) {
    *exitCode = code;
  }
  return true;
}

}  // namespace gc::platform
