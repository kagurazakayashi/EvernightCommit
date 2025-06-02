// 需要 posix_spawn_file_actions_addclosefrom_np / addchdir_np 与 pipe2。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "platform/linux/subprocess.h"

#include <fcntl.h>
#include <features.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kReadChunkBytes = 64 * 1024;
constexpr int kPollSliceMs = 50;
constexpr unsigned long kKillGraceMs = 5000;
constexpr unsigned long kReaderExitGraceMs = 2000;
constexpr size_t kNoteTextLimit = 200;
// 说明文字会进界面与日志，一律限长，别把整段系统文案摊开。
std::wstring ClampNote(std::wstring text) {
  if (text.size() > kNoteTextLimit) {
    text.resize(kNoteTextLimit);
  }
  return text;
}

std::wstring ErrnoText(int error) {
  const char* text = std::strerror(error);
  std::wstring wide;
  if (text != nullptr) {
    wide = Utf8ToWide(text);
  }
  if (wide.empty()) {
    wide = L"errno " + std::to_wstring(error);
  }
  return ClampNote(std::move(wide));
}

// 只用于诊断展示，不是执行输入：执行始终是 program + 参数数组。
std::wstring QuoteForDisplay(std::wstring_view value) {
  std::wstring quoted = L"'";
  for (const wchar_t c : value) {
    if (c == L'\'') {
      quoted += L"'\\''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted.push_back(L'\'');
  return quoted;
}

std::wstring BuildDisplayCommandLine(const std::wstring& program,
                                     const std::vector<std::wstring>& arguments) {
  std::wstring text = QuoteForDisplay(program);
  for (const std::wstring& argument : arguments) {
    text += L" ";
    text += QuoteForDisplay(argument);
  }
  return text;
}

std::wstring TruncatedNote(size_t maxBytes, size_t observedBytes) {
  return L"输出超过 " + std::to_wstring(maxBytes) + L" 字节上限（本次实际出现 " +
         std::to_wstring(observedBytes) + L" 字节），只保留了开头一段，不能当成完整回答。";
}

std::wstring AbandonedNote() {
  return L"管道在限定时间内没有结束（子进程的子孙可能仍持有写端），后面的内容没有读到。";
}

std::wstring ReadFailedNote(int error) {
  return L"读取管道失败：" + ErrnoText(error);
}

std::wstring ReaderExitNote() {
  return L"读取线程未能在限定时间内结束，这一路输出按不完整处理。";
}

// 一路输出的读取状态。用 shared_ptr 持有：万一读线程最后没能按时结束（调用方已分离它），
// 状态与文件描述符仍由线程自己持有并关闭，调用方不会再碰它。
struct ReaderState {
  int fd = -1;
  std::atomic<bool> abandonRequested{false};
  std::atomic<bool> finished{false};

  // 下面几个字段只由读线程写；调用方只在确认线程结束之后读取。
  std::string bytes;
  size_t observedBytes = 0;
  size_t maxBytes = 0;
  StreamCaptureState state = StreamCaptureState::notStarted;
  std::wstring note;
};

// 非阻塞读 + 有界 poll：既不会因为另一条流没读而堵住子进程，也能在放弃时按轮醒来。
void ReadStreamToEnd(const std::shared_ptr<ReaderState>& state) {
  std::vector<char> buffer(static_cast<size_t>(kReadChunkBytes));
  for (;;) {
    if (state->abandonRequested.load(std::memory_order_acquire)) {
      state->state = StreamCaptureState::abandoned;
      state->note = AbandonedNote();
      ::close(state->fd);
      state->fd = -1;
      state->finished.store(true, std::memory_order_release);
      return;
    }

    pollfd entry{};
    entry.fd = state->fd;
    entry.events = POLLIN | POLLHUP | POLLERR;
    const int ready = ::poll(&entry, 1, kPollSliceMs);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      state->state = StreamCaptureState::readFailed;
      state->note = ReadFailedNote(errno);
      break;
    }
    if (ready == 0) {
      continue;  // 期限内没有新字节也没有 EOF：下一轮再看放弃标记。
    }

    const ssize_t got = ::read(state->fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      state->state = StreamCaptureState::readFailed;
      state->note = ReadFailedNote(errno);
      break;
    }
    if (got == 0) {
      state->state = state->observedBytes > state->maxBytes ? StreamCaptureState::truncated
                                                            : StreamCaptureState::complete;
      if (state->state == StreamCaptureState::truncated) {
        state->note = TruncatedNote(state->maxBytes, state->observedBytes);
      }
      break;
    }
    const size_t count = static_cast<size_t>(got);
    state->observedBytes += count;
    if (state->bytes.size() < state->maxBytes) {
      const size_t room = state->maxBytes - state->bytes.size();
      state->bytes.append(buffer.data(), std::min(room, count));
    }
    // 超过上限后继续排空但不保留：既不无界占内存，也让子进程能正常写完退出。
  }
  ::close(state->fd);
  state->fd = -1;
  state->finished.store(true, std::memory_order_release);
}

// 一路输出的读取线程与其结果。用法固定为 Start → Finish → Take 三步。
class StreamReader {
public:
  explicit StreamReader(int readFd) : state_(std::make_shared<ReaderState>()) {
    state_->fd = readFd;
  }

  StreamReader(const StreamReader&) = delete;
  StreamReader& operator=(const StreamReader&) = delete;

  void Start(size_t maxBytes) {
    state_->maxBytes = maxBytes;
    // 读端设为非阻塞：poll 报告可读后，read 也不会在边界上意外阻塞。
    const int flags = ::fcntl(state_->fd, F_GETFL, 0);
    if (flags >= 0) {
      static_cast<void>(::fcntl(state_->fd, F_SETFL, flags | O_NONBLOCK));
    }
    try {
      thread_ = std::thread([state = state_]() { ReadStreamToEnd(state); });
    } catch (const std::system_error&) {
      state_->state = StreamCaptureState::readFailed;
      state_->note = L"无法创建输出读取线程，这一路按读取失败处理。";
      ::close(state_->fd);
      state_->fd = -1;
      state_->finished.store(true, std::memory_order_release);
      return;
    }
    threadRunning_ = true;
  }

  // 有界收尾：先等管道自然 EOF（把已缓冲的输出排空），超过期限才要求读线程中止；
  // 中止后仍等不到线程结束就分离它并按不完整报告。
  //
  // 完成标志由读线程发布，主线程只读这个原子位；其余捕获字段在 join 之后才读取，
  // 因此不存在“读到一半状态”的竞态。
  void Finish(unsigned long drainMs) {
    if (!threadRunning_) {
      return;
    }
    const auto drainDeadline = Clock::now() + std::chrono::milliseconds(drainMs);
    const auto hardDeadline =
        drainDeadline + std::chrono::milliseconds(kReaderExitGraceMs);
    while (!state_->finished.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      if (now >= drainDeadline) {
        state_->abandonRequested.store(true, std::memory_order_release);
      }
      if (now >= hardDeadline) {
        // 宽限也用尽：不能再 join（那会变成无限等待），交给线程自己收尾。
        detached_ = true;
        thread_.detach();
        threadRunning_ = false;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    thread_.join();
    threadRunning_ = false;
  }

  [[nodiscard]] StreamCapture Take() const {
    StreamCapture capture;
    capture.maxBytes = state_->maxBytes;
    if (detached_) {
      capture.state = StreamCaptureState::abandoned;
      capture.note = ReaderExitNote();
      return capture;
    }
    capture.state = state_->state;
    capture.observedBytes = state_->observedBytes;
    capture.bytes = state_->bytes;
    capture.note = ClampNote(state_->note);
    return capture;
  }

private:
  std::shared_ptr<ReaderState> state_;
  std::thread thread_;
  bool threadRunning_ = false;
  bool detached_ = false;
};

bool SetNonBlockingAndCloseOnExec(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return false;
  }
  const int descriptorFlags = ::fcntl(fd, F_GETFD, 0);
  return descriptorFlags >= 0 && ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0;
}

void CloseIfOpen(int* fd) {
  if (*fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}

bool EncodeProgram(const ProcessRequest& request, std::string* programBytes,
                   std::vector<std::string>* argumentBytes, std::wstring* failureReason) {
  if (request.program.empty()) {
    *failureReason = L"程序路径为空";
    return false;
  }
  if (!TryPathBytesFromWide(request.program, *programBytes)) {
    *failureReason = L"程序路径无法无损编码成 UTF-8：" + request.program;
    return false;
  }
  argumentBytes->clear();
  argumentBytes->reserve(request.arguments.size());
  for (const std::wstring& argument : request.arguments) {
    std::string bytes;
    if (!TryWideToUtf8Strict(argument, bytes)) {
      *failureReason = L"程序参数无法无损编码成 UTF-8：" + argument;
      return false;
    }
    argumentBytes->push_back(std::move(bytes));
  }
  return true;
}

void KillProcessGroup(pid_t pid) {
  // setsid 让子进程成为新会话与进程组的组长（pgid == pid）。先杀组，再补杀进程本身：
  // 子进程若在 setsid 之前就被取消，组还不存在，但进程本身仍在。
  if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) {
    static_cast<void>(::kill(pid, SIGKILL));
  }
  static_cast<void>(::kill(pid, SIGKILL));
}

}  // namespace

ProcessRunResult RunCapturedProcess(const ProcessRequest& request,
                                    const std::vector<std::string>& environmentEntries) {
  ProcessRunResult result;
  result.stdoutCapture.maxBytes = request.maxCaptureBytesPerStream;
  result.stderrCapture.maxBytes = request.maxCaptureBytesPerStream;
  result.commandLine = BuildDisplayCommandLine(request.program, request.arguments);

  std::string programBytes;
  std::vector<std::string> argumentBytes;
  std::wstring encodingFailure;
  if (!EncodeProgram(request, &programBytes, &argumentBytes, &encodingFailure)) {
    result.launchError = EINVAL;
    result.launchErrorText = ClampNote(std::move(encodingFailure));
    return result;
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

  int outRead = -1;
  int outWrite = -1;
  int errRead = -1;
  int errWrite = -1;
  int pipeFds[4] = {-1, -1, -1, -1};
  if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
    result.launchError = errno;
    result.launchErrorText = L"无法创建输出管道：" + ErrnoText(errno);
    return result;
  }
  outRead = pipeFds[0];
  outWrite = pipeFds[1];
  if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
    result.launchError = errno;
    result.launchErrorText = L"无法创建错误输出管道：" + ErrnoText(errno);
    CloseIfOpen(&outRead);
    CloseIfOpen(&outWrite);
    return result;
  }
  errRead = pipeFds[0];
  errWrite = pipeFds[1];
  if (!SetNonBlockingAndCloseOnExec(outRead) || !SetNonBlockingAndCloseOnExec(errRead)) {
    result.launchError = errno;
    result.launchErrorText = L"无法设置输出管道属性：" + ErrnoText(errno);
    CloseIfOpen(&outRead);
    CloseIfOpen(&outWrite);
    CloseIfOpen(&errRead);
    CloseIfOpen(&errWrite);
    return result;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);

  const auto failSetup = [&](int error, std::wstring text) {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    CloseIfOpen(&outRead);
    CloseIfOpen(&outWrite);
    CloseIfOpen(&errRead);
    CloseIfOpen(&errWrite);
    result.launchError = static_cast<unsigned long>(error);
    result.launchErrorText = ClampNote(std::move(text));
    return result;
  };

  // stdin 接 /dev/null：后台查询不等待任何输入（交互式 Git 不走这条路）。
  int actionError = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_adddup2(&actions, outWrite, STDOUT_FILENO);
  }
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_adddup2(&actions, errWrite, STDERR_FILENO);
  }
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addclose(&actions, outRead);
  }
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addclose(&actions, errRead);
  }
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addclose(&actions, outWrite);
  }
  if (actionError == 0) {
    actionError = posix_spawn_file_actions_addclose(&actions, errWrite);
  }
  if (actionError != 0) {
    return failSetup(actionError, L"无法设置子进程输出重定向：" + ErrnoText(actionError));
  }

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
  // 只让子进程拿到 0/1/2：并发任务的管道、GUI 的 X11/socket 等描述符不会漏进去。
  actionError = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (actionError != 0) {
    return failSetup(actionError, L"无法限制子进程继承的描述符：" + ErrnoText(actionError));
  }
#endif
#endif

  if (!request.workingDirectory.empty()) {
    std::string directoryBytes;
    if (!TryPathBytesFromWide(request.workingDirectory, directoryBytes)) {
      return failSetup(EINVAL, L"工作目录无法无损编码成 UTF-8：" + request.workingDirectory);
    }
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 29)
    actionError = posix_spawn_file_actions_addchdir_np(&actions, directoryBytes.c_str());
    if (actionError != 0) {
      return failSetup(actionError, L"无法设置子进程工作目录：" + ErrnoText(actionError));
    }
#else
    return failSetup(ENOTSUP, L"当前 C 运行库不支持在 spawn 时指定工作目录。");
#endif
#else
    return failSetup(ENOTSUP, L"当前 C 运行库不支持在 spawn 时指定工作目录。");
#endif
  }

  sigset_t defaultSignals;
  sigemptyset(&defaultSignals);
  for (const int signalNumber : {SIGPIPE, SIGINT, SIGQUIT, SIGHUP, SIGTERM, SIGCHLD}) {
    sigaddset(&defaultSignals, signalNumber);
  }
  sigset_t emptyMask;
  sigemptyset(&emptyMask);
  posix_spawnattr_setsigdefault(&attributes, &defaultSignals);
  posix_spawnattr_setsigmask(&attributes, &emptyMask);

  short spawnFlags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_SETSID
  spawnFlags |= POSIX_SPAWN_SETSID;
#endif
  posix_spawnattr_setflags(&attributes, spawnFlags);

  pid_t pid = -1;
  const int spawnError = posix_spawn(&pid, programBytes.c_str(), &actions, &attributes,
                                     argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  // 父进程一侧的写端立即关闭：否则管道永远不会 EOF，读取线程无法结束。
  CloseIfOpen(&outWrite);
  CloseIfOpen(&errWrite);
  if (spawnError != 0) {
    CloseIfOpen(&outRead);
    CloseIfOpen(&errRead);
    result.launchError = static_cast<unsigned long>(spawnError);
    result.launchErrorText = L"无法启动进程 " + request.program + L"：" + ErrnoText(spawnError);
    return result;
  }

  result.started = true;

  StreamReader stdoutReader(outRead);
  StreamReader stderrReader(errRead);
  outRead = -1;  // 所有权已交给读取线程
  errRead = -1;
  stdoutReader.Start(request.maxCaptureBytesPerStream);
  stderrReader.Start(request.maxCaptureBytesPerStream);

  const auto start = Clock::now();
  const bool hasTimeout = request.timeoutMilliseconds != 0;
  const auto timeoutDeadline =
      start + std::chrono::milliseconds(hasTimeout ? request.timeoutMilliseconds : 0);

  bool exitObserved = false;
  bool killRequested = false;
  bool cancelObserved = false;
  bool timeoutObserved = false;
  auto killDeadline = start;
  siginfo_t info{};
  for (;;) {
    info = siginfo_t{};
    const int waited = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
    if (waited == 0 && info.si_pid == pid) {
      exitObserved = true;
      break;
    }
    if (waited < 0 && errno == ECHILD) {
      // 子进程已被别人回收：不能再假定它的退出码，也不能再等。
      break;
    }
    if (!killRequested && StopRequested(request.cancel)) {
      cancelObserved = true;
      killRequested = true;
      killDeadline = Clock::now() + std::chrono::milliseconds(kKillGraceMs);
      KillProcessGroup(pid);
    } else if (!killRequested && hasTimeout && Clock::now() >= timeoutDeadline) {
      timeoutObserved = true;
      killRequested = true;
      killDeadline = Clock::now() + std::chrono::milliseconds(kKillGraceMs);
      KillProcessGroup(pid);
    }
    if (killRequested && Clock::now() >= killDeadline) {
      // 终止后仍要有界等待回收；绝大多数情况下一两轮就结束，这里兜住极端不可中断状态。
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  int status = 0;
  bool reaped = false;
  if (exitObserved) {
    // 组长已经退出，但子孙可能还占着管道：在回收组长之前先清理整个会话/进程组。
    // 这一步发生在 waitpid 之前，pid 仍被僵尸占用，不会被别的进程复用。
    static_cast<void>(::kill(-pid, SIGKILL));
    pid_t waitedPid = -1;
    do {
      waitedPid = ::waitpid(pid, &status, 0);
    } while (waitedPid < 0 && errno == EINTR);
    reaped = waitedPid == pid;
  } else if (killRequested) {
    // 终止后仍未观察到退出：先等宽限期，再把回收交给独立线程，避免主调用无限阻塞。
    const auto graceEnd = Clock::now() + std::chrono::milliseconds(kKillGraceMs);
    while (Clock::now() < graceEnd) {
      info = siginfo_t{};
      const int waited = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
      if (waited == 0 && info.si_pid == pid) {
        static_cast<void>(::kill(-pid, SIGKILL));
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        reaped = true;
        break;
      }
      if (waited < 0 && errno == ECHILD) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!reaped) {
      // 结果如实报告「没有确认回收」；后台线程只负责 waitpid，不触碰调用方状态。
      std::thread([pid] {
        int ignored = 0;
        while (::waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {
        }
      }).detach();
    }
  }

  result.reaped = reaped;
  result.exited = reaped;
  if (reaped) {
    if (WIFEXITED(status)) {
      result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.signal = WTERMSIG(status);
      result.exitCode = 128 + result.signal;
    }
  }

  if (cancelObserved) {
    result.termination = ProcessTermination::canceled;
    result.canceled = true;
    result.terminated = true;
    result.timedOut = false;
  } else if (timeoutObserved) {
    result.termination = ProcessTermination::timedOut;
    result.timedOut = true;
    result.terminated = true;
  } else if (!exitObserved && !reaped) {
    result.termination = ProcessTermination::unknown;
  } else if (result.signal != 0) {
    result.termination = ProcessTermination::signaled;
  } else {
    result.termination = ProcessTermination::exited;
  }

  stdoutReader.Finish(request.drainAfterExitMilliseconds);
  stderrReader.Finish(request.drainAfterExitMilliseconds);
  result.stdoutCapture = stdoutReader.Take();
  result.stderrCapture = stderrReader.Take();
  return result;
}

}  // namespace gc::platform
