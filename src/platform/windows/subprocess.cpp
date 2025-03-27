#include "platform/windows/subprocess.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

#include "platform/windows/raii.h"

namespace gc::platform {
namespace {

constexpr size_t kMaxCapturedOutputBytes = 1u << 20;  // 1 MiB；超出后继续排空管道但丢弃内容。
constexpr unsigned long kKillGraceMs = 5000;

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

std::wstring FormatLaunchErrorText(unsigned long errorCode) {
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
  return text;
}

// 输出管道抽取线程：持续 ReadFile，直到写端全部关闭（进程退出）或被取消。
// 独立线程保证父进程在等待退出的同时管道不会写满，也就不会死锁。
struct PipeReader {
  UniqueHandle readHandle;
  std::string output;

  void Run() {
    char buffer[65536];
    for (;;) {
      DWORD got = 0;
      if (::ReadFile(readHandle.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr) == 0) {
        break;  // EOF（ERROR_BROKEN_PIPE）、被 CancelSynchronousIo 取消或句柄失效。
      }
      if (got == 0) {
        continue;
      }
      if (output.size() < kMaxCapturedOutputBytes) {
        const size_t room = kMaxCapturedOutputBytes - output.size();
        output.append(buffer, room < got ? room : got);
      }
    }
  }
};

// 一路输出（stdout 或 stderr）的管道两端。
struct CapturedPipe {
  UniqueHandle read;
  UniqueHandle write;
};

// 建立一条不继承读端的管道；写端供子进程继承，父进程启动后立即关闭。
bool OpenCapturedPipe(CapturedPipe& pipe, SECURITY_ATTRIBUTES& inheritable, std::wstring& errorText,
                      unsigned long& errorCode) {
  HANDLE rawRead = nullptr;
  HANDLE rawWrite = nullptr;
  if (::CreatePipe(&rawRead, &rawWrite, &inheritable, 256 * 1024) == 0) {
    errorCode = ::GetLastError();
    errorText = L"无法创建输出管道：" + FormatLaunchErrorText(errorCode);
    return false;
  }
  pipe.read.Reset(rawRead);
  pipe.write.Reset(rawWrite);
  if (::SetHandleInformation(pipe.read.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
    errorCode = ::GetLastError();
    errorText = L"管道句柄设置失败：" + FormatLaunchErrorText(errorCode);
    return false;
  }
  return true;
}

// 启动抽取线程。线程创建失败时放弃这一路输出，但进程等待照常进行。
bool StartReader(PipeReader& reader, std::thread& thread) {
  try {
    thread = std::thread([&reader]() { reader.Run(); });
  } catch (const std::system_error&) {
    reader.readHandle.Reset();
    return false;
  }
  return true;
}

void JoinReader(std::thread& thread) {
  // 万一孙进程继承了写端导致 ReadFile 仍阻塞，显式取消它，保证线程必然能结束。
  ::CancelSynchronousIo(thread.native_handle());
  thread.join();
}

}  // namespace

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

SubprocessRunResult RunHiddenCaptured(std::wstring_view program, const std::vector<std::wstring>& arguments,
                                      std::wstring_view workingDirectory, unsigned long timeoutMilliseconds,
                                      const wchar_t* environmentBlock) {
  SubprocessRunResult result;
  result.commandLine = BuildCommandLine(program, arguments);
  if (program.empty()) {
    result.launchError = ERROR_INVALID_NAME;
    result.launchErrorText = L"程序路径为空";
    return result;
  }

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  CapturedPipe stdoutPipe;
  CapturedPipe stderrPipe;
  std::wstring pipeError;
  unsigned long pipeErrorCode = 0;
  if (!OpenCapturedPipe(stdoutPipe, inheritable, pipeError, pipeErrorCode) ||
      !OpenCapturedPipe(stderrPipe, inheritable, pipeError, pipeErrorCode)) {
    result.launchError = pipeErrorCode;
    result.launchErrorText = pipeError;
    return result;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nullptr;  // 内部查询命令不需要交互输入。
  startup.hStdOutput = stdoutPipe.write.get();
  startup.hStdError = stderrPipe.write.get();

  PROCESS_INFORMATION processInformation{};
  std::wstring mutableCommand = result.commandLine;  // CreateProcessW 要求可写缓冲。
  const std::wstring programString(program);
  const std::wstring workDir(workingDirectory);

  // CreateProcessW 的 lpEnvironment 形参是 LPVOID/TCHAR*，语义上只读；去 const 仅限本调用。
  void* environment = const_cast<void*>(static_cast<const void*>(environmentBlock));
  const BOOL created =
      ::CreateProcessW(programString.c_str(), mutableCommand.data(), nullptr, nullptr,
                       /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                       environment, workDir.empty() ? nullptr : workDir.c_str(), &startup,
                       &processInformation);
  // 父进程一侧的写端立即关闭：否则管道永远不会 EOF，读取线程无法结束。
  stdoutPipe.write.Reset();
  stderrPipe.write.Reset();
  if (created == 0) {
    result.launchError = ::GetLastError();
    result.launchErrorText = FormatLaunchErrorText(result.launchError);
    return result;
  }
  result.started = true;
  UniqueHandle processHandle(processInformation.hProcess);
  UniqueHandle threadHandle(processInformation.hThread);

  // 两条流各用独立线程抽取：单线程轮读会在其中一条写满时死锁。
  std::unique_ptr<PipeReader> stdoutReader = std::make_unique<PipeReader>();
  std::unique_ptr<PipeReader> stderrReader = std::make_unique<PipeReader>();
  stdoutReader->readHandle.Reset(stdoutPipe.read.Release());
  stderrReader->readHandle.Reset(stderrPipe.read.Release());
  std::thread stdoutThread;
  std::thread stderrThread;
  bool stdoutStarted = StartReader(*stdoutReader, stdoutThread);
  bool stderrStarted = StartReader(*stderrReader, stderrThread);

  const DWORD waited = ::WaitForSingleObject(processHandle.get(), timeoutMilliseconds);
  if (waited == WAIT_TIMEOUT) {
    result.timedOut = true;
    ::TerminateProcess(processHandle.get(), 0xFFFF);
    result.exited = ::WaitForSingleObject(processHandle.get(), kKillGraceMs) == WAIT_OBJECT_0;
  } else {
    result.exited = waited == WAIT_OBJECT_0;
  }

  if (stdoutStarted) {
    JoinReader(stdoutThread);
    result.utf8Stdout = std::move(stdoutReader->output);
  }
  if (stderrStarted) {
    JoinReader(stderrThread);
    result.utf8Stderr = std::move(stderrReader->output);
  }
  result.utf8Output = result.utf8Stdout + result.utf8Stderr;
  unsigned long exitCode = 0;
  if (::GetExitCodeProcess(processHandle.get(), &exitCode) != 0) {
    result.exitCode = exitCode;
  }
  return result;
}

}  // namespace gc::platform
