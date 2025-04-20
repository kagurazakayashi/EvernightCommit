#include "platform/windows/command_window_helper.h"

#include <windows.h>

#include <shellapi.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/raii.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

constexpr size_t kMaxSpecBytes = 256u * 1024u;

// Windows 系统消息文本（诊断用）。本模块自带一份，避免为一条错误文字跨模块取依赖。
std::wstring FormatError(unsigned long errorCode) {
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

std::vector<std::wstring> CommandLineArguments() {
  int count = 0;
  LPWSTR* parsed = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  if (parsed == nullptr) {
    return {};
  }
  std::vector<std::wstring> arguments;
  arguments.reserve(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    arguments.emplace_back(parsed[index]);
  }
  ::LocalFree(parsed);
  return arguments;
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view relative) {
  std::wstring result(base);
  if (!result.empty() && result.back() != L'\\') {
    result.push_back(L'\\');
  }
  result.append(relative);
  return result;
}

// 去掉结尾分隔符，便于“父目录 == 临时根”这种逐段比较。
std::wstring TrimTrailingSeparators(std::wstring_view path) {
  std::wstring result(path);
  while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) {
    result.pop_back();
  }
  return result;
}

// 最后一个路径段（目录名或文件名）。分隔符形态在此一并归一，只用于取名字。
std::wstring LastSegment(std::wstring_view path) {
  const std::wstring trimmed = TrimTrailingSeparators(path);
  const size_t separator = trimmed.find_last_of(L"\\/");
  return separator == std::wstring::npos ? trimmed : trimmed.substr(separator + 1);
}

// 父目录（去掉最后一个段）。没有分隔符时返回空串。
std::wstring ParentSegment(std::wstring_view path) {
  const std::wstring trimmed = TrimTrailingSeparators(path);
  const size_t separator = trimmed.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::wstring() : trimmed.substr(0, separator);
}

// 与执行器同源的临时目录根：GetTempPathW 再去掉结尾分隔符。
// 辅助进程由执行器直接创建，环境变量原样继承，因此两边算出的根必然一致；
// 这里重新取值是为了核对“说明书所在的目录确实属于当前用户的临时目录”，
// 而不是接受命令行上随便写的一个路径。
std::wstring TemporaryRootPath() {
  std::wstring buffer(MAX_PATH + 4, L'\0');
  const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  if (length == 0 || length > buffer.size()) {
    return {};
  }
  buffer.resize(length);
  return TrimTrailingSeparators(buffer);
}

bool WriteBytes(std::wstring_view path, std::string_view bytes) {
  UniqueHandle handle(::CreateFileW(path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    return false;
  }
  if (bytes.empty()) {
    return ::SetEndOfFile(handle.get()) != 0;
  }
  DWORD written = 0;
  return ::WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                     nullptr) != 0 &&
         written == bytes.size();
}

// 读全文，超过上限视为不可信（说明书只可能是几百字节）。
bool ReadBytes(std::wstring_view path, std::string& outBytes) {
  outBytes.clear();
  UniqueHandle handle(::CreateFileW(path.data(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    return false;
  }
  char buffer[4096];
  for (;;) {
    DWORD got = 0;
    if (::ReadFile(handle.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr) == 0) {
      return false;
    }
    if (got == 0) {
      return true;
    }
    outBytes.append(buffer, got);
    if (outBytes.size() > kMaxSpecBytes) {
      outBytes.clear();
      return false;
    }
  }
}

// 命令窗口里的输入/输出通道。控制台子进程要能原生交互（Git 自己提问口令），
// 就必须拿到可继承的 CONIN$/CONOUT$ 句柄：辅助进程是 GUI 子系统，
// 标准句柄是自己 AllocConsole 之后打开的，再显式塞进 STARTUPINFO 才会传给 Git。
struct ConsoleStreams {
  UniqueHandle input;
  UniqueHandle output;
  [[nodiscard]] bool Valid() const noexcept { return bool(input) && bool(output); }
};

ConsoleStreams OpenConsoleStreams() {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  ConsoleStreams streams;
  streams.input.Reset(::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
  streams.output.Reset(::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
  return streams;
}

void ConsoleWrite(const ConsoleStreams& streams, std::wstring_view text) {
  if (!streams.output || text.empty()) {
    return;
  }
  // WriteConsoleW 直接写控制台缓冲区：不经过任何码页往返，中文与 emoji 原样显示。
  DWORD written = 0;
  static_cast<void>(
      ::WriteConsoleW(streams.output.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
}

// 让辅助进程拥有自己独占的控制台。
// 先 FreeConsole 再 AllocConsole 是刻意的：本入口既可能由没有控制台的界面进程启动，
// 也可能在测试里被一个已经连着控制台的进程启动（gc_tests 是控制台子系统，
// 默认会继承测试运行器的控制台，那样“命令窗口”就不是一个新窗口了，测试也就没测到真实形态）。
// 主动断开再新建，两种宿主子系统下的行为完全一致：一个新窗口，输入输出都归它。
bool EnsureOwnConsole(std::wstring* failureReason) {
  static_cast<void>(::FreeConsole());
  if (::AllocConsole() == 0) {
    if (failureReason != nullptr) {
      *failureReason = L"无法创建命令窗口控制台：" + FormatError(::GetLastError());
    }
    return false;
  }
  HWND window = ::GetConsoleWindow();
  if (window == nullptr) {
    if (failureReason != nullptr) {
      *failureReason = L"命令窗口控制台已创建但取不到窗口句柄。";
    }
    return false;
  }
  static_cast<void>(::ShowWindow(window, SW_SHOWNORMAL));
  return true;
}

// 程序与每个参数都整体加引号（与 git 层的命令行构造同一形态）。
// 这里的值都已经过 git::BuildGitCommandLine 的引号与控制字符校验，
// 因此只需处理“结尾反斜杠会转义闭合引号”这一种情况。
std::wstring QuoteArgument(std::wstring_view value) {
  std::wstring result;
  result.push_back(L'"');
  size_t pendingBackslashes = 0;
  for (const wchar_t c : value) {
    if (c == L'\\') {
      ++pendingBackslashes;
      continue;
    }
    result.append(pendingBackslashes, L'\\');
    pendingBackslashes = 0;
    result.push_back(c);
  }
  result.append(pendingBackslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::wstring SystemDirectoryPath() {
  std::wstring buffer(MAX_PATH + 4, L'\0');
  const UINT length = ::GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length != 0 && length < buffer.size()) {
    buffer.resize(length);
    return buffer;
  }
  const DWORD queried =
      ::GetEnvironmentVariableW(L"windir", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (queried != 0 && queried < buffer.size()) {
    buffer.resize(queried);
    return JoinPath(buffer, L"System32");
  }
  return L"C:\\Windows\\System32";
}

// 用继承来的控制台跑一条命令并等它结束；返回 false 表示进程根本没被创建。
// 工作目录一律取说明书里的仓库目录（与执行器过去交给 cmd 的那一个等价）。
// 参数按 const std::wstring& 收：CreateProcessW 的 lpApplicationName 与 lpCurrentDirectory
// 都必须是 NUL 结尾的字符串，string_view 不保证这一点，即使实际调用方传的都是 std::wstring
// 也不该依赖那个巧合。
bool RunInheritedConsole(const ConsoleStreams& streams, const std::wstring& program,
                         std::wstring_view commandLine, const std::wstring& workingDirectory,
                         unsigned long* exitCode, std::wstring* failureReason) {
  std::wstring mutableCommandLine(commandLine);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup.wShowWindow = SW_SHOWNORMAL;
  startup.hStdInput = streams.input.get();
  startup.hStdOutput = streams.output.get();
  startup.hStdError = streams.output.get();
  PROCESS_INFORMATION information{};
  // bInheritHandles=TRUE 才让子进程真的用上上面三个句柄；dwCreationFlags 为 0，
  // 子进程因此留在辅助进程这个控制台里 —— Git 的凭据提问、进度输出都在同一窗口。
  const BOOL created = ::CreateProcessW(program.c_str(), mutableCommandLine.data(), nullptr, nullptr,
                                        /*bInheritHandles=*/TRUE, 0, nullptr,
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup, &information);
  if (created == 0) {
    if (failureReason != nullptr) {
      *failureReason = FormatError(::GetLastError());
    }
    return false;
  }
  UniqueHandle process(information.hProcess);
  UniqueHandle thread(information.hThread);
  // 无限等待是刻意的：Git 可能停在“输入口令”上等很久。执行器观察的是标记文件与结果文件，
  // 不会因此卡住；用户任何时候关掉命令窗口，控制台上的进程一起被系统收掉。
  static_cast<void>(::WaitForSingleObject(process.get(), INFINITE));
  if (exitCode != nullptr) {
    static_cast<void>(::GetExitCodeProcess(process.get(), exitCode));
  }
  return true;
}

struct HelperContext {
  std::wstring operationDirectory;
  std::wstring expectedNonce;
  std::wstring title;
  std::wstring gitExecutable;
  std::wstring workingDirectory;
  std::wstring commandLine;  // 展示与执行同一条（宽字符，未经任何码页转换）
  std::wstring failureReason;
  ConsoleStreams streams;
};

// 说明书与标记文件的名字与执行器共用同一份常量（git::k*FileName 是 ASCII 字面量）。
std::wstring InOperationDirectory(const HelperContext& context, std::string_view fileName) {
  return JoinPath(context.operationDirectory, Utf8ToUtf16(std::string(fileName)));
}

// 说明书读回并校验：任何不合格式都返回 false，绝不猜、绝不修。
bool LoadAndValidateSpec(HelperContext& context) {
  const std::wstring specPath = InOperationDirectory(context, git::kSpecFileName);
  std::string specBytes;
  if (!ReadBytes(specPath, specBytes)) {
    context.failureReason = L"读不到操作说明书，或它超过可信大小上限：" + specPath;
    return false;
  }
  std::wstring specText;
  // 严格解码：说明书若被截断、掺入非法字节序列，宁可不执行，也不带着 U+FFFD 去跑。
  if (!TryUtf8ToUtf16Strict(specBytes, specText)) {
    context.failureReason = L"操作说明书不是合法的 UTF-8 文本，拒绝执行。";
    return false;
  }
  git::CommandWindowSpec spec;
  std::wstring parseReason;
  if (!git::ParseCommandWindowSpecText(specText, &spec, &parseReason)) {
    context.failureReason = parseReason;
    return false;
  }
  if (spec.nonce != context.expectedNonce) {
    context.failureReason = L"操作说明书的口令与本次启动不符，拒绝执行。";
    return false;
  }
  const std::wstring directoryName = LastSegment(context.operationDirectory);
  if (spec.directoryToken != directoryName) {
    context.failureReason = L"操作说明书声明的目录与它实际所在目录不符，拒绝执行。";
    return false;
  }
  if (!git::IsSafeOperationDirectoryName(directoryName)) {
    context.failureReason = L"操作目录名不符合执行器生成的形态，拒绝执行。";
    return false;
  }

  // 说明书里的内容一律按不可信输入重新过一遍边界校验 —— 与 GUI 提交时用的是同一个函数，
  // 因此界面看到的那条命令行与实际执行的那条必然同源。
  git::CommandWindowOperation operation;
  operation.operationId = spec.operationId;
  operation.displayName = spec.operationId;
  operation.gitExecutable = spec.gitExecutable;
  operation.repositoryDirectory = spec.workingDirectory;
  operation.arguments = spec.arguments;
  git::CommandPlanReject reject = git::CommandPlanReject::none;
  std::wstring detail;
  std::wstring commandLine;
  if (!git::BuildGitCommandLine(operation, &commandLine, &reject, &detail)) {
    context.failureReason = L"操作说明书的内容没有通过执行边界校验：" + detail;
    return false;
  }
  context.title = std::move(spec.title);
  context.gitExecutable = std::move(spec.gitExecutable);
  context.workingDirectory = std::move(spec.workingDirectory);
  context.commandLine = std::move(commandLine);
  return true;
}

int Execute(const HelperContext& context) {
  const std::wstring startPath = InOperationDirectory(context, git::kStartMarkerFileName);
  const std::wstring resultPath = InOperationDirectory(context, git::kResultFileName);

  // 标题用 Unicode API 设置：中文标题在任何代码页的机器上都原样显示，
  // 也不再因为“某个码页装不下”而让一次合法操作失败。
  static_cast<void>(::SetConsoleTitleW(context.title.c_str()));
  // 控制台码页显式对齐 Git 自己的 UTF-8 输出；失败就维持系统默认，只是显示问题，不改判定。
  static_cast<void>(::SetConsoleOutputCP(CP_UTF8));
  static_cast<void>(::SetConsoleCP(CP_UTF8));

  if (!WriteBytes(startPath, "start\r\n")) {
    ConsoleWrite(context.streams, L"[EvernightCommit] 无法写出开始标记，操作未执行。\r\n");
    return 2;
  }
  // 窗口里显示的就是即将执行的那一条命令（原样宽字符，不含任何转义形态）。
  ConsoleWrite(context.streams, context.commandLine + L"\r\n");

  if (!IsExistingRegularFile(context.gitExecutable)) {
    // 与 cmd 时代的 9009 同形态：执行器据此报告“Git 未能启动”，而不是“执行完成 + 非 0 退出码”。
    static_cast<void>(WriteBytes(resultPath, std::to_string(git::kCommandNotFoundExitCode) + "\r\n"));
    ConsoleWrite(context.streams,
                 L"[EvernightCommit] Git 程序不存在或已被移除：" + context.gitExecutable + L"\r\n");
    return 3;
  }
  if (!IsExistingDirectory(context.workingDirectory)) {
    static_cast<void>(WriteBytes(resultPath, std::to_string(git::kCommandNotFoundExitCode) + "\r\n"));
    ConsoleWrite(context.streams,
                 L"[EvernightCommit] 仓库目录不存在或已被移除：" + context.workingDirectory + L"\r\n");
    return 3;
  }

  unsigned long gitExitCode = 0;
  std::wstring launchFailure;
  if (!RunInheritedConsole(context.streams, context.gitExecutable, context.commandLine,
                           context.workingDirectory, &gitExitCode, &launchFailure)) {
    static_cast<void>(WriteBytes(resultPath, std::to_string(git::kCommandNotFoundExitCode) + "\r\n"));
    ConsoleWrite(context.streams, L"[EvernightCommit] Git 进程未能创建：" + launchFailure + L"\r\n");
    return 3;
  }

  // 退出码按有符号 32 位写回，与过去 cmd 的 %ERRORLEVEL% 形态一致（例如 -2 而不是 4294967294）。
  const std::string resultText =
      std::to_string(static_cast<long>(static_cast<std::int32_t>(gitExitCode))) + "\r\n";
  if (!WriteBytes(resultPath, resultText)) {
    ConsoleWrite(context.streams, L"[EvernightCommit] 无法写出结果文件，本程序不会得知 Git 的退出码。\r\n");
  }
  ConsoleWrite(context.streams,
               L"[EvernightCommit] Git 已退出，窗口保持打开，可继续查看上方输出。\r\n");

  // 把窗口交给 cmd /k：命令跑完后仍是交互提示符，用户可以继续翻看与操作。
  // 这一步失败不影响成败判定 —— 结果文件已经落账，执行器要的是 Git 的退出码。
  const std::wstring cmdPath = JoinPath(SystemDirectoryPath(), L"cmd.exe");
  std::wstring shellFailure;
  if (!RunInheritedConsole(context.streams, cmdPath, QuoteArgument(cmdPath) + L" /d /k",
                           context.workingDirectory, nullptr, &shellFailure)) {
    ConsoleWrite(context.streams,
                 L"[EvernightCommit] 交互提示符未能启动（" + shellFailure + L"），窗口即将关闭。\r\n");
    return 0;
  }
  return 0;
}

// 命令行形态：--gc-console-helper <操作目录> <随机口令>。参数个数固定，不接受其他输入。
bool ParseHelperArguments(const std::vector<std::wstring>& arguments, HelperContext& context,
                          std::wstring* failureReason) {
  if (arguments.size() != 4) {
    *failureReason = L"命令窗口辅助入口的参数形态不正确：需要且只允许 <操作目录> 与 <口令> 两项。";
    return false;
  }
  const std::wstring& operationDirectory = arguments[2];
  const std::wstring& nonce = arguments[3];
  if (!git::IsSafeNonce(nonce)) {
    *failureReason = L"命令窗口辅助入口收到的口令形态不合法。";
    return false;
  }
  if (operationDirectory.empty() || !IsAbsolutePath(operationDirectory)) {
    *failureReason = L"操作目录必须是绝对路径。";
    return false;
  }
  const std::wstring directoryName = LastSegment(operationDirectory);
  if (!git::IsSafeOperationDirectoryName(directoryName)) {
    *failureReason = L"操作目录名不符合执行器生成的形态：" + directoryName;
    return false;
  }
  // 只接受“当前用户临时目录根下的那一个独占目录”：父目录必须正好是临时根
  // （盘符大小写不敏感，NTFS 的路径比较本来就不区分大小写）。
  const std::wstring temporaryRoot = TemporaryRootPath();
  if (temporaryRoot.empty()) {
    *failureReason = L"无法取得系统临时目录。";
    return false;
  }
  const std::wstring parent = TrimTrailingSeparators(ParentSegment(operationDirectory));
  if (parent.empty() || _wcsicmp(parent.c_str(), temporaryRoot.c_str()) != 0) {
    *failureReason = L"操作目录不在当前用户的临时目录根下：" + operationDirectory;
    return false;
  }
  context.operationDirectory = JoinPath(temporaryRoot, directoryName);
  context.expectedNonce = nonce;
  return true;
}

int HelperMain(const std::vector<std::wstring>& arguments) {
  HelperContext context;
  std::wstring failureReason;
  if (!ParseHelperArguments(arguments, context, &failureReason)) {
    return 1;
  }
  if (!EnsureOwnConsole(&failureReason)) {
    return 1;
  }
  context.streams = OpenConsoleStreams();
  if (!context.streams.Valid()) {
    return 1;
  }
  if (!LoadAndValidateSpec(context)) {
    ConsoleWrite(context.streams,
                 L"[EvernightCommit] 本次操作未被执行：" + context.failureReason + L"\r\n");
    return 1;
  }
  return Execute(context);
}

}  // namespace

bool RunCommandWindowHelperIfRequested(int* exitCode) {
  if (exitCode != nullptr) {
    *exitCode = 0;
  }
  const std::vector<std::wstring> arguments = CommandLineArguments();
  if (arguments.size() < 2 || arguments[1] != kCommandWindowHelperSwitch) {
    return false;  // 普通启动：继续走界面或测试主流程。
  }
  const int code = HelperMain(arguments);
  if (exitCode != nullptr) {
    *exitCode = code;
  }
  return true;
}

}  // namespace gc::platform
