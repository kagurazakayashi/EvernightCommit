#include "platform/terminal.h"

#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "git/git_locator.h"
#include "platform/path.h"
#include "platform/terminal_adapters.h"
#include "platform/text_encoding.h"

extern char** environ;

namespace gc::platform {
namespace {

struct KnownTerminal {
  const wchar_t* executableName;
  TerminalAdapterKind kind;
};

// 顺序就是默认优先级：xterm 的 -e 是“选项之后全部按 argv 启动”，最容易证明不会被 shell
// 二次解析；其余适配器只在对应程序确实存在时才成为候选。x-terminal-emulator 是 Debian
// 的 alternatives 入口，必须解析到已知实现后复用该实现的明确参数，不能把它当万能终端。
constexpr KnownTerminal kKnownTerminals[] = {
    {L"xterm", TerminalAdapterKind::xterm},
    {L"xfce4-terminal", TerminalAdapterKind::xfce4Terminal},
    {L"konsole", TerminalAdapterKind::konsole},
    {L"gnome-terminal", TerminalAdapterKind::gnomeTerminal},
    {L"alacritty", TerminalAdapterKind::alacritty},
    {L"kitty", TerminalAdapterKind::kitty},
};

git::FsProbe MakeFsProbe() {
  return git::FsProbe{
      [](const std::wstring& path) { return IsExistingRegularFile(path); },
      [](const std::wstring& path) { return ToAbsolutePath(path); },
      [](const std::wstring& path) { return IsExecutableRegularFile(path); },
  };
}

std::wstring BaseName(std::wstring_view path) {
  const size_t slash = path.find_last_of(L'/');
  return slash == std::wstring_view::npos ? std::wstring(path) : std::wstring(path.substr(slash + 1));
}

bool ClassifyProgramName(std::wstring_view baseName, TerminalAdapterKind* out) {
  // xfce4-terminal.wrapper 是 Debian alternatives 的转接脚本；它最终 exec 的是
  // xfce4-terminal。这里只识别名字，真正启动时使用直接发现的 xfce4-terminal 可执行文件。
  std::wstring_view name = baseName;
  constexpr std::wstring_view kWrapperSuffix = L".wrapper";
  if (name.size() > kWrapperSuffix.size() &&
      name.compare(name.size() - kWrapperSuffix.size(), kWrapperSuffix.size(), kWrapperSuffix) == 0) {
    name.remove_suffix(kWrapperSuffix.size());
  }
  if (name == L"xterm") {
    *out = TerminalAdapterKind::xterm;
    return true;
  }
  if (name == L"xfce4-terminal") {
    *out = TerminalAdapterKind::xfce4Terminal;
    return true;
  }
  if (name == L"gnome-terminal") {
    *out = TerminalAdapterKind::gnomeTerminal;
    return true;
  }
  if (name == L"konsole") {
    *out = TerminalAdapterKind::konsole;
    return true;
  }
  if (name == L"alacritty") {
    *out = TerminalAdapterKind::alacritty;
    return true;
  }
  if (name == L"kitty") {
    *out = TerminalAdapterKind::kitty;
    return true;
  }
  return false;
}

bool ContainsCandidate(const std::vector<TerminalCandidate>& candidates, TerminalAdapterKind kind,
                       std::wstring_view programPath) {
  for (const TerminalCandidate& candidate : candidates) {
    if (candidate.kind == kind && candidate.programPath == programPath) {
      return true;
    }
  }
  return false;
}

bool EncodePath(std::wstring_view path, std::string* bytes) {
  return TryPathBytesFromWide(path, *bytes) && bytes->find('\0') == std::string::npos;
}

std::wstring ErrnoText(int error) {
  return Utf8ToWide(std::strerror(error));
}

}  // namespace

std::vector<TerminalCandidate> DiscoverTerminalCandidates() {
  std::vector<TerminalCandidate> candidates;
  const std::wstring pathVariable = GetPathVariable();
  const git::FsProbe probe = MakeFsProbe();

  const auto findDirect = [&](std::wstring_view name) -> std::wstring {
    const std::vector<std::wstring> found =
        git::SearchPathForExecutable(pathVariable, name, probe, git::PathFlavor::posix);
    return found.empty() ? std::wstring() : found.front();
  };

  for (const KnownTerminal& known : kKnownTerminals) {
    const std::wstring program = findDirect(known.executableName);
    if (program.empty()) {
      continue;
    }
    TerminalCandidate candidate;
    candidate.kind = known.kind;
    candidate.programPath = program;
    candidate.discoveredAs = known.executableName;
    if (!ContainsCandidate(candidates, candidate.kind, candidate.programPath)) {
      candidates.push_back(std::move(candidate));
    }
  }

  // Debian 的 x-terminal-emulator 只是 alternatives 入口：解析到已知实现后，
  // 以该实现自己的参数形态加入候选；解析不出已知实现就明确不采用，绝不猜参数。
  const std::wstring alternatives = findDirect(L"x-terminal-emulator");
  if (!alternatives.empty()) {
    char resolved[PATH_MAX];
    const std::string alternativesBytes = WideToUtf8(alternatives);
    if (::realpath(alternativesBytes.c_str(), resolved) != nullptr) {
      std::wstring resolvedWide;
      if (TryWidePathFromBytes(resolved, resolvedWide)) {
        TerminalAdapterKind kind = TerminalAdapterKind::unknown;
        const std::wstring base = BaseName(resolvedWide);
        if (ClassifyProgramName(base, &kind) && IsKnownTerminalAdapter(kind)) {
          // 转接脚本本身可能不认识目标程序的参数；优先使用按名字直接发现的真实程序。
          std::wstring program = findDirect(TerminalAdapterKindId(kind));
          if (program.empty()) {
            program = resolvedWide;
          }
          if (IsExecutableRegularFile(program) &&
              !ContainsCandidate(candidates, kind, program)) {
            TerminalCandidate candidate;
            candidate.kind = kind;
            candidate.programPath = program;
            candidate.discoveredAs =
                std::wstring(L"x-terminal-emulator（→ ") + base + L"）";
            candidate.viaAlternatives = true;
            candidates.push_back(std::move(candidate));
          }
        }
      }
    }
  }

  return candidates;
}

TerminalSelection DiscoverAndSelectTerminal(const TerminalPreferences& preferences) {
  std::vector<TerminalCandidate> candidates = DiscoverTerminalCandidates();
  const char* display = std::getenv("DISPLAY");
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  const bool hasX = display != nullptr && display[0] != '\0';
  const bool hasWayland = wayland != nullptr && wayland[0] != '\0';

  if (!hasX && !hasWayland) {
    TerminalSelection selection;
    selection.candidates = candidates;
    selection.failureReason = L"当前进程没有可用的图形显示会话（DISPLAY / WAYLAND_DISPLAY 均为空）。";
    selection.guidance =
        L"交互式 Git 必须在你自己的终端窗口里运行。请在图形桌面会话中启动本程序，"
        L"或在能打开终端的环境里重新登录后重试；本程序不会把写操作改成隐藏执行。";
    return selection;
  }

  if (!hasX && hasWayland) {
    // Wayland 会话里只有原生 Wayland 终端能直接工作；xterm/xfce4-terminal 等 X11 程序
    // 通常依赖 XWayland（会设置 DISPLAY）。没有 DISPLAY 时不把它们当作可用候选。
    std::erase_if(candidates, [](const TerminalCandidate& candidate) {
      return candidate.kind != TerminalAdapterKind::gnomeTerminal &&
             candidate.kind != TerminalAdapterKind::kitty;
    });
  }

  TerminalSelection selection = SelectTerminalFromCandidates(candidates, preferences);
  if (!selection.usable && selection.guidance.empty()) {
    selection.guidance =
        L"请安装 xterm、xfce4-terminal、gnome-terminal、konsole、alacritty 或 kitty 之一，"
        L"或在设置里指定终端程序。";
  }
  return selection;
}

TerminalLaunchResult LaunchTerminal(const TerminalLaunchPlan& plan) {
  TerminalLaunchResult result;
  result.kind = plan.kind;
  if (!plan.failureReason.empty()) {
    result.failureReason = plan.failureReason;
    return result;
  }
  if (plan.program.empty()) {
    result.failureReason = L"终端程序路径为空。";
    return result;
  }

  std::string programBytes;
  if (!EncodePath(plan.program, &programBytes)) {
    result.failureReason = L"终端程序路径无法无损编码成 UTF-8 文件名：" + plan.program;
    return result;
  }
  std::vector<std::string> argumentBytes;
  argumentBytes.reserve(plan.arguments.size());
  for (const std::wstring& argument : plan.arguments) {
    std::string bytes;
    if (!TryWideToUtf8Strict(argument, bytes) || bytes.find('\0') != std::string::npos) {
      result.failureReason = L"终端参数无法无损编码成 UTF-8：" + argument;
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

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);

  const auto failSetup = [&](int error, std::wstring reason) {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    result.failureReason = std::move(reason);
    if (error != 0) {
      result.failureReason += L"（" + ErrnoText(error) + L"）";
    }
    return result;
  };

  // 终端客户端不需要继承本进程的输入输出：GUI 可能没有控制台，测试运行器也可能
  // 正在收集管道。三个标准流都接 /dev/null，终端的交互全部发生在它自己创建的 pty 里。
  int actionError = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  }
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  }
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  }
#endif
#endif
  if (actionError != 0) {
    return failSetup(actionError, L"无法设置终端客户端的标准流。");
  }

  sigset_t defaultSignals;
  sigemptyset(&defaultSignals);
  for (const int signalNumber : {SIGINT, SIGQUIT, SIGHUP, SIGTERM, SIGPIPE, SIGCHLD}) {
    sigaddset(&defaultSignals, signalNumber);
  }
  sigset_t emptyMask;
  sigemptyset(&emptyMask);
  posix_spawnattr_setsigdefault(&attributes, &defaultSignals);
  posix_spawnattr_setsigmask(&attributes, &emptyMask);

  short spawnFlags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_SETSID
  // 新会话：GUI 退出或它自己的终端关闭时，不会把仍在运行的终端窗口一起带走。
  spawnFlags |= POSIX_SPAWN_SETSID;
#endif
  posix_spawnattr_setflags(&attributes, spawnFlags);

  pid_t pid = -1;
  const int spawnError =
      posix_spawn(&pid, programBytes.c_str(), &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (spawnError != 0) {
    result.failureReason = L"无法启动终端程序 " + plan.program + L"：" + ErrnoText(spawnError);
    if (spawnError == ENOENT) {
      result.guidance = L"终端程序在启动前消失或 PATH 已变化；请重新选择终端。";
    } else if (spawnError == EACCES) {
      result.guidance = L"终端程序没有执行权限；请检查安装与权限。";
    }
    return result;
  }

  result.started = true;
  result.launcherProcessId = static_cast<std::uint64_t>(pid);

  // 终端客户端可能很快退出（交给已有终端服务），也可能一直等到窗口关闭。这里只回收
  // 僵尸进程，不把它的退出码写进任何结果字段——它不是 Git 的退出码。
  try {
    std::thread([pid] {
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }).detach();
  } catch (...) {
    // 无法创建回收线程不影响终端已经启动；最坏情况只留下一个僵尸，进程退出时由系统回收。
  }
  return result;
}

}  // namespace gc::platform
