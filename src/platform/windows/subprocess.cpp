#include "platform/windows/subprocess.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "platform/windows/raii.h"

namespace gc::platform {
namespace {

// 单参数引用规则：仅当参数为空、含空格/制表/引号时才加引号；
// 引号前的连续反斜杠须加倍，嵌入的引号写作 \"，结尾的连续反斜杠须加倍。
void AppendArgument(std::wstring& commandLine, std::wstring_view argument, bool alwaysQuote) {
  const bool needsQuote =
      alwaysQuote || argument.empty() ||
      argument.find_first_of(L" \t\"") != std::wstring_view::npos;
  if (!needsQuote) {
    commandLine.append(argument);
    return;
  }
  commandLine.push_back(L'"');
  size_t pendingBackslashes = 0;
  for (const wchar_t c : argument) {
    if (c == L'\\') {
      ++pendingBackslashes;
      continue;
    }
    if (c == L'"') {
      commandLine.append(pendingBackslashes * 2 + 1, L'\\');
      commandLine.push_back(c);
    } else {
      commandLine.append(pendingBackslashes, L'\\');
      commandLine.push_back(c);
    }
    pendingBackslashes = 0;
  }
  commandLine.append(pendingBackslashes * 2, L'\\');
  commandLine.push_back(L'"');
}

constexpr unsigned long kKillGraceMs = 5000;
// 超过收尾期限后，读线程仍未结束的宽限：到点这里就不再等它（分离线程并报告不完整），
// 保证关闭过程有界。
constexpr unsigned long kReaderExitGraceMs = 2000;
// 收尾轮询切片。要求中止之后的取消要按轮重复投递：读线程可能正好卡在
// 「已决定要读但还没进入 ReadFile」的缝隙里，那一次取消会落空；下一轮它必然已经阻塞，取消就生效。
constexpr unsigned long kJoinSliceMs = 100;
constexpr size_t kReadChunkBytes = 64u * 1024u;
// 说明文字会进界面与日志，一律限长，别把整段系统文案或整份输出摊开。
constexpr size_t kNoteTextLimit = 200;

std::wstring FormatErrorText(unsigned long errorCode) {
  LPWSTR buffer = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD written =
      ::FormatMessageW(flags, nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring text;
  if (written != 0 && buffer != nullptr) {
    text.assign(buffer, written);
    ::LocalFree(buffer);
  }
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
    text.pop_back();
  }
  if (text.empty()) {
    text = L"Windows 错误码 " + std::to_wstring(errorCode);
  }
  if (text.size() > kNoteTextLimit) {
    text.resize(kNoteTextLimit);
  }
  return text;
}

std::wstring TruncatedNote(size_t maxBytes, size_t observedBytes) {
  return L"输出超过 " + std::to_wstring(maxBytes) + L" 字节上限（本次实际出现 " +
         std::to_wstring(observedBytes) +
         L" 字节），只保留了开头一段，不能当成完整回答。";
}

std::wstring AbandonedNote() {
  return L"管道在限定时间内没有结束（子进程的子孙可能仍持有写端），后面的内容没有读到。";
}

std::wstring ReaderExitNote() {
  return L"读取线程未能在限定时间内结束，这一路输出按不完整处理。";
}

std::wstring ClampNote(std::wstring text) {
  if (text.size() > kNoteTextLimit) {
    text.resize(kNoteTextLimit);
  }
  return text;
}

// 一路输出的读取状态。用 shared_ptr 持有：万一读线程最后没能按时结束（调用方已分离它），
// 状态与句柄仍由线程自己持有并关闭，调用方不会再碰它，不会出现悬空引用或句柄泄漏。
struct ReaderState {
  UniqueHandle readHandle;
  std::atomic<bool> abandonRequested{false};

  // 下面几个字段只由读线程写。调用方只允许在 Finish() 确认线程结束之后读取
  // （线程结束本身构成可见性边界）；线程没能结束时走 abandoned，一个字节都不读。
  std::string bytes;
  size_t observedBytes = 0;
  size_t maxBytes = 0;
  StreamCaptureState state = StreamCaptureState::notStarted;
  std::wstring note;
};

// 持续 ReadFile，直到写端全部关闭（EOF）、读失败，或被收尾逻辑要求中止。
// 独立线程保证父进程在等进程退出的同时管道不会写满，也就不会把子进程堵死。
void ReadStreamToEnd(const std::shared_ptr<ReaderState>& state) {
  std::vector<char> buffer(kReadChunkBytes);
  for (;;) {
    if (state->abandonRequested.load(std::memory_order_acquire)) {
      state->state = StreamCaptureState::abandoned;
      state->note = AbandonedNote();
      return;
    }
    DWORD got = 0;
    if (::ReadFile(state->readHandle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                   nullptr) == 0) {
      const unsigned long error = ::GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        // 所有写端都已关闭：这一路到此为止，前面读到的就是全部。
        state->state = state->observedBytes > state->maxBytes ? StreamCaptureState::truncated
                                                              : StreamCaptureState::complete;
        if (state->state == StreamCaptureState::truncated) {
          state->note = TruncatedNote(state->maxBytes, state->observedBytes);
        }
        return;
      }
      if (error == ERROR_OPERATION_ABORTED || state->abandonRequested.load(std::memory_order_acquire)) {
        state->state = StreamCaptureState::abandoned;
        state->note = AbandonedNote();
        return;
      }
      state->state = StreamCaptureState::readFailed;
      state->note = L"读取管道失败：" + FormatErrorText(error);
      return;
    }
    if (got == 0) {
      continue;
    }
    state->observedBytes += static_cast<size_t>(got);
    if (state->bytes.size() < state->maxBytes) {
      const size_t room = state->maxBytes - state->bytes.size();
      state->bytes.append(buffer.data(),
                          room < static_cast<size_t>(got) ? room : static_cast<size_t>(got));
    }
    // 超过上限后继续排空但不保留：既不无界占内存，也让子进程能正常写完退出。
  }
}

// 一路输出的读取线程与其结果。用法固定为 Start → Finish → Take 三步，Take 之前必须调过 Finish。
class StreamReader {
public:
  explicit StreamReader(UniqueHandle readHandle) : state_(std::make_shared<ReaderState>()) {
    state_->readHandle = std::move(readHandle);
  }

  StreamReader(const StreamReader&) = delete;
  StreamReader& operator=(const StreamReader&) = delete;

  // 启动读取线程。线程建不起来时这一路判为读取失败（不是「没有输出」）；
  // 管道句柄交给 state_ 析构关闭。
  void Start(size_t maxBytes) {
    state_->maxBytes = maxBytes;
    try {
      thread_ = std::thread([state = state_]() { ReadStreamToEnd(state); });
    } catch (const std::system_error&) {
      state_->state = StreamCaptureState::readFailed;
      state_->note = L"无法创建输出读取线程，这一路按读取失败处理。";
      return;
    }
    threadRunning_ = true;
  }

  // 有界收尾：先等管道自然 EOF（把已缓冲的输出排空），超过期限才要求读线程中止；
  // 中止后仍等不到线程结束就分离它并按不完整报告。既不无限 join，也不靠「一次取消定成败」。
  void Finish(unsigned long drainMs) {
    if (!threadRunning_) {
      return;
    }
    const ULONGLONG drainDeadline = ::GetTickCount64() + drainMs;
    const ULONGLONG exitDeadline = drainDeadline + kReaderExitGraceMs;
    for (;;) {
      const ULONGLONG now = ::GetTickCount64();
      // 期限之前绝不投递取消：读线程此刻正阻塞等子进程写，取消它就把「还没结束」
      // 误报成「读不到结尾」（第一版就踩过这个坑：收尾只要几十毫秒就先取消了）。
      const bool pastDrainDeadline = now >= drainDeadline;
      if (pastDrainDeadline) {
        state_->abandonRequested.store(true, std::memory_order_release);
      }
      const ULONGLONG hardRemaining = now < exitDeadline ? exitDeadline - now : 0ULL;
      const DWORD slice =
          static_cast<DWORD>(std::min<ULONGLONG>(hardRemaining == 0ULL ? kJoinSliceMs : hardRemaining,
                                                 kJoinSliceMs));
      if (::WaitForSingleObject(thread_.native_handle(), slice) == WAIT_OBJECT_0) {
        thread_.join();
        threadRunning_ = false;
        return;
      }
      if (pastDrainDeadline) {
        // 要求中止之后才取消：读线程可能正好卡在「已决定要读但还没进入 ReadFile」的缝隙里，
        // 那一次取消落空无害，下一轮它必然已经阻塞，取消就生效。
        ::CancelSynchronousIo(thread_.native_handle());
      }
      if (hardRemaining == 0ULL) {
        // 宽限也用尽：不能再 join（那会变成无限等待），交给线程自己收尾。
        detached_ = true;
        thread_.detach();
        threadRunning_ = false;
        return;
      }
    }
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

// 建立一条读端不继承的管道；写端供子进程继承，父进程在启动调用返回后立即关闭。
bool OpenCapturedPipe(UniqueHandle& read, UniqueHandle& write, SECURITY_ATTRIBUTES& inheritable,
                      std::wstring& errorText, unsigned long& errorCode) {
  HANDLE rawRead = nullptr;
  HANDLE rawWrite = nullptr;
  if (::CreatePipe(&rawRead, &rawWrite, &inheritable, static_cast<DWORD>(kReadChunkBytes * 4)) == 0) {
    errorCode = ::GetLastError();
    errorText = L"无法创建输出管道：" + FormatErrorText(errorCode);
    return false;
  }
  read.Reset(rawRead);
  write.Reset(rawWrite);
  if (::SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
    errorCode = ::GetLastError();
    errorText = L"管道句柄设置失败：" + FormatErrorText(errorCode);
    return false;
  }
  return true;
}

// 可继承句柄白名单（PROC_THREAD_ATTRIBUTE_HANDLE_LIST）。
// 只用 bInheritHandles=TRUE 的话，本进程里所有「标记为可继承」的句柄都会跟着进新进程：
// 两个后台查询并发启动时，先建好的那一对管道写端会被后启动的子进程继承，于是那一头的写端
// 要等无关进程退出才关闭，本进程永远等不到 EOF。白名单让子进程只拿到属于它的那两个写端。
// 资源责任：列表缓冲区与属性表都由本对象持有，属性表必须在 CreateProcessW 返回之后才释放，
// 所以活到调用点之外（局部对象，随 RunHiddenCaptured 一起析构）；句柄本身的所有权仍在
// 管道两端与读取线程手里，这里只是引用。
class InheritedHandleList {
public:
  InheritedHandleList() = default;
  InheritedHandleList(const InheritedHandleList&) = delete;
  InheritedHandleList& operator=(const InheritedHandleList&) = delete;

  ~InheritedHandleList() {
    if (list_ != nullptr) {
      ::DeleteProcThreadAttributeList(list_);
    }
  }

  // 建表并挂上唯一的句柄列表属性；失败时给出可展示的说明（不退回「全都继承」那条老路）。
  bool Build(const std::vector<HANDLE>& handles, std::wstring& errorText) {
    handles_ = handles;
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) {
      errorText = L"无法查询进程属性表大小：" + FormatErrorText(::GetLastError());
      return false;
    }
    storage_ = std::vector<uint8_t>(size);
    auto* rawList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (::InitializeProcThreadAttributeList(rawList, 1, 0, &size) == 0) {
      errorText = L"无法创建进程属性表：" + FormatErrorText(::GetLastError());
      return false;
    }
    list_ = rawList;
    if (::UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
                                    handles_.size() * sizeof(HANDLE), nullptr, nullptr) == 0) {
      errorText = L"无法设置继承句柄白名单：" + FormatErrorText(::GetLastError());
      return false;
    }
    return true;
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return list_; }

private:
  std::vector<uint8_t> storage_;
  std::vector<HANDLE> handles_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

}  // namespace

std::wstring_view StreamCapture::StateLabel() const noexcept {
  switch (state) {
    case StreamCaptureState::complete:
      return L"完整";
    case StreamCaptureState::truncated:
      return L"超过上限被截断";
    case StreamCaptureState::readFailed:
      return L"读取失败";
    case StreamCaptureState::abandoned:
      return L"未能读到结尾";
    case StreamCaptureState::notStarted:
      return L"未读取";
  }
  return L"未读取";
}

std::wstring QuoteArgument(std::wstring_view value) {
  std::wstring segment;
  AppendArgument(segment, value, /*alwaysQuote=*/true);
  return segment;
}

std::wstring BuildCommandLine(std::wstring_view program, const std::vector<std::wstring>& arguments) {
  std::wstring commandLine;
  // 程序路径一律加引号：路径几乎总可能含空格。
  AppendArgument(commandLine, program, /*alwaysQuote=*/true);
  for (const std::wstring& argument : arguments) {
    commandLine.push_back(L' ');
    AppendArgument(commandLine, argument, /*alwaysQuote=*/false);
  }
  return commandLine;
}

SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                      const std::vector<std::wstring>& arguments,
                                      std::wstring_view workingDirectory,
                                      unsigned long timeoutMilliseconds, const wchar_t* environmentBlock,
                                      size_t maxCaptureBytesPerStream,
                                      unsigned long drainAfterExitMilliseconds) {
  SubprocessRunResult result;
  result.stdoutCapture.maxBytes = maxCaptureBytesPerStream;
  result.stderrCapture.maxBytes = maxCaptureBytesPerStream;
  result.commandLine = BuildCommandLine(program, arguments);
  if (program.empty()) {
    result.launchError = ERROR_INVALID_NAME;
    result.launchErrorText = L"程序路径为空";
    return result;
  }

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  UniqueHandle stdoutRead;
  UniqueHandle stdoutWrite;
  UniqueHandle stderrRead;
  UniqueHandle stderrWrite;
  std::wstring setupError;
  unsigned long setupErrorCode = 0;
  if (!OpenCapturedPipe(stdoutRead, stdoutWrite, inheritable, setupError, setupErrorCode) ||
      !OpenCapturedPipe(stderrRead, stderrWrite, inheritable, setupError, setupErrorCode)) {
    result.launchError = setupErrorCode;
    result.launchErrorText = setupError;
    return result;
  }

  // 白名单只放行这两个写端；两个读端留在本进程，且已显式取消继承标记。
  InheritedHandleList attributeList;
  if (!attributeList.Build({stdoutWrite.get(), stderrWrite.get()}, setupError)) {
    result.launchError = ::GetLastError();
    result.launchErrorText = setupError;
    return result;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullptr;  // 内部查询命令不需要交互输入。
  startup.StartupInfo.hStdOutput = stdoutWrite.get();
  startup.StartupInfo.hStdError = stderrWrite.get();
  startup.lpAttributeList = attributeList.Get();

  PROCESS_INFORMATION processInformation{};
  std::wstring mutableCommand = result.commandLine;  // CreateProcessW 要求可写缓冲。
  const std::wstring programString(program);
  const std::wstring workDir(workingDirectory);

  // CreateProcessW 的 lpEnvironment 形参是 LPVOID/TCHAR*，语义上只读；去 const 仅限本调用。
  void* environment = const_cast<void*>(static_cast<const void*>(environmentBlock));
  const BOOL created =
      ::CreateProcessW(programString.c_str(), mutableCommand.data(), nullptr, nullptr,
                       /*bInheritHandles=*/TRUE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                       environment, workDir.empty() ? nullptr : workDir.c_str(),
                       reinterpret_cast<LPSTARTUPINFOW>(&startup), &processInformation);
  // 父进程一侧的写端立即关闭：否则管道永远不会 EOF，读取线程无法结束。
  stdoutWrite.Reset();
  stderrWrite.Reset();
  if (created == 0) {
    result.launchError = ::GetLastError();
    result.launchErrorText = FormatErrorText(result.launchError);
    return result;
  }
  result.started = true;
  UniqueHandle processHandle(processInformation.hProcess);
  UniqueHandle threadHandle(processInformation.hThread);

  // 两条流各用独立线程抽取：单线程轮读会在其中一条写满时死锁。
  StreamReader stdoutReader(std::move(stdoutRead));
  StreamReader stderrReader(std::move(stderrRead));
  stdoutReader.Start(maxCaptureBytesPerStream);
  stderrReader.Start(maxCaptureBytesPerStream);

  const DWORD waited = ::WaitForSingleObject(processHandle.get(), timeoutMilliseconds);
  if (waited == WAIT_TIMEOUT) {
    result.timedOut = true;
    ::TerminateProcess(processHandle.get(), 0xFFFF);
    result.terminated = ::WaitForSingleObject(processHandle.get(), kKillGraceMs) == WAIT_OBJECT_0;
  } else {
    result.exited = waited == WAIT_OBJECT_0;
  }

  // 排空排在进程落账之后：进程已经不会再写，剩下的只是管道里已缓冲的字节与 EOF。
  stdoutReader.Finish(drainAfterExitMilliseconds);
  stderrReader.Finish(drainAfterExitMilliseconds);
  result.stdoutCapture = stdoutReader.Take();
  result.stderrCapture = stderrReader.Take();

  unsigned long exitCode = 0;
  if (::GetExitCodeProcess(processHandle.get(), &exitCode) != 0) {
    result.exitCode = exitCode;
  }
  return result;
}

}  // namespace gc::platform
