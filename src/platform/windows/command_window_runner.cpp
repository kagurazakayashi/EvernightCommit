#include "platform/windows/command_window_runner.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/windows/ansi_text.h"
#include "platform/windows/environment_block.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

constexpr unsigned long kPollIntervalMs = 200;constexpr unsigned long kNoTraceGraceMs = 15000;
constexpr unsigned long kProcessExitRetryWindowMs = 1500;
constexpr unsigned long kProcessExitRetryIntervalMs = 100;

// 命令窗口操作的默认环境保障（仍可被请求里的同名覆盖取代）：
//   GIT_TERMINAL_PROMPT=1                —— 凭据/口令必须在终端里问，不许被静默跳过；
//   删除 GIT_ASKPASS / SSH_ASKPASS       —— 二者会把提问搬成 GUI 弹窗，与“原生交互留在窗口里”冲突；
//   GIT_PAGER=cat                        —— 关键：新建控制台是交互式 TTY，Git 默认把
//     status/log/diff 之类输出交给分页器 less，脚本会卡在 call 那一行等用户按键，
//     于是退出码迟迟拿不到、临时仓库也被窗口占用。命令窗口本来就保留全部输出供滚动查看，
//     分页没有价值，因此这里直接关闭分页器（不影响凭据交互）。
const std::vector<git::EnvironmentOverride>& CommandWindowDefaultOverrides() {
  static const std::vector<git::EnvironmentOverride> kOverrides = {
      {L"GIT_TERMINAL_PROMPT", std::wstring(L"1")},
      {L"GIT_PAGER", std::wstring(L"cat")},
      {L"GIT_ASKPASS", std::nullopt},
      {L"SSH_ASKPASS", std::nullopt},
  };
  return kOverrides;
}

std::wstring FormatWindowsError(unsigned long errorCode) {
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

bool IsAsciiBytes(std::string_view bytes) {
  for (const char c : bytes) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u > 0x7Eu) {
      return false;
    }
  }
  return true;
}

std::wstring JoinPath(std::wstring_view base, std::wstring_view relative) {
  std::wstring result(base);
  if (!result.empty() && result.back() != L'\\') {
    result.push_back(L'\\');
  }
  result.append(relative);
  return result;
}

std::wstring TempRootPath() {
  std::wstring buffer(MAX_PATH + 4, L'\0');
  const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  if (length == 0 || length > buffer.size()) {
    return {};
  }
  buffer.resize(length);
  while (!buffer.empty() && (buffer.back() == L'\\' || buffer.back() == L'/')) {
    buffer.pop_back();
  }
  return buffer;
}

// cmd.exe 的所在目录：GetSystemDirectoryW 给出真正的系统目录（64 位进程下为 System32）。
// 注意不要用 GetWindowsDirectoryW/GetSystemWindowsDirectoryW —— 它们只到 C:\Windows，
// 那里没有 cmd.exe，CreateProcessW 会报“系统找不到指定的文件”。
std::wstring SystemDirectoryPath() {
  std::wstring buffer(MAX_PATH + 4, L'\0');
  const UINT length = ::GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length != 0 && length < buffer.size()) {
    buffer.resize(length);
    return buffer;
  }
  const DWORD queried = ::GetEnvironmentVariableW(L"windir", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (queried != 0 && queried < buffer.size()) {
    buffer.resize(queried);
    return JoinPath(buffer, L"System32");
  }
  return L"C:\\Windows\\System32";
}

bool WriteAllBytes(std::wstring_view path, std::string_view bytes) {
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

bool ReadAllBytes(std::wstring_view path, std::string& outBytes) {
  outBytes.clear();
  HANDLE raw = ::CreateFileW(path.data(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return false;
  }
  UniqueHandle handle(raw);
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
    if (outBytes.size() > 64u * 1024u) {
      outBytes.clear();
      return false;  // 结果文件只应是一行；异常大小视为不可信。
    }
  }
}

// 按标题里的唯一 ASCII 标记查找命令窗口。
// 不做整串精确匹配：脚本标题里的中文要先按系统 ANSI 码页编码、再由 cmd 按其控制台码页解码，
// 两个码页不一致时中文会变形，而纯 ASCII 标记（操作 ID）在任何码页往返下都保持稳定。
struct EnumWindowContext {
  std::wstring token;
  HWND found = nullptr;
};

BOOL CALLBACK CollectWindowByToken(HWND window, LPARAM lParam) {
  auto* context = reinterpret_cast<EnumWindowContext*>(lParam);
  const int length = ::GetWindowTextLengthW(window);
  if (length <= 0) {
    return TRUE;
  }
  std::wstring title(static_cast<size_t>(length) + 1, L'\0');
  const int written = ::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
  if (written <= 0) {
    return TRUE;
  }
  title.resize(static_cast<size_t>(written));
  if (title.find(context->token) != std::wstring::npos) {
    context->found = window;
    return FALSE;  // 找到即停止枚举
  }
  return TRUE;
}

HWND FindWindowOwningToken(std::wstring_view token) {
  if (token.empty()) {
    return nullptr;
  }
  EnumWindowContext context{std::wstring(token), nullptr};
  ::EnumWindows(&CollectWindowByToken, reinterpret_cast<LPARAM>(&context));
  return context.found;
}

// 脚本段的引用形态与 git::BuildGitCommandLine 一致：程序路径与每个参数都整体加引号。
// 边界校验已保证这些值不含双引号与控制字符，因此不需要 CRT 的反斜杠加倍规则，
// 只补一条：结尾反斜杠会把闭合引号转义成字面量，必须加倍。
std::wstring QuotePathSegment(std::wstring_view value) {
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

}  // namespace

// 观察记录：执行器记录表与观察线程各持一份强引用。
// Shutdown 丢弃记录表后线程仍持有自己的那份，绝不会访问已释放内存；
// 双方都放手时记录才真正释放。
struct CommandWindowWatchState {
  uint64_t id = 0;
  std::wstring requestOperationId;
  std::wstring displayName;
  std::wstring directory;
  std::wstring commandLine;
  std::wstring repositoryDirectory;
  std::wstring windowTitle;         // 脚本设置的控制台标题（展示用）
  std::wstring windowTitleToken;    // 标题里的唯一 ASCII 标记（操作 ID），用于可靠定位窗口
  std::wstring statusText;          // 界面可见的即时状态
  platform::UniqueHandle process;
  platform::UniqueHandle thread;
  HWND consoleWindow = nullptr;
  bool completionRecorded = false;
  bool threadFinished = false;  // 观察线程已跑完最后一行；之后记录才可被安全移除
  CommandWindowResult result;
};

// 记录表：完整类型只存在于本翻译单元，因此 CommandWindowRunner 的头文件
// 不需要（也不能）实例化它的析构 —— 构造与析构都在下面定义。
class CommandWindowRunner::Impl {
public:
  std::vector<std::shared_ptr<CommandWindowWatchState>> records;  // 进行中与已完成的记录
  std::vector<std::thread> watchers;                              // 观察线程本体

  std::shared_ptr<CommandWindowWatchState> Find(uint64_t id) const {
    for (const std::shared_ptr<CommandWindowWatchState>& state : records) {
      if (state->id == id) {
        return state;
      }
    }
    return nullptr;
  }

  // 只移除“结果已登记”的记录：未登记的记录至少被观察线程持有，
  // 但线程不再引用表项本身，所以这里可以安全放手。
  void DiscardRecordedLocked(uint64_t id) {
    std::erase_if(records, [id](const std::shared_ptr<CommandWindowWatchState>& state) {
      return state->id == id && state->completionRecorded;
    });
  }
};

CommandWindowRunner::CommandWindowRunner() : impl_(new Impl()) {}

CommandWindowRunner::~CommandWindowRunner() {
  Shutdown();
  delete impl_;
  impl_ = nullptr;
}

// 回收历史遗留的操作目录（定义在下方，启动时调用一次）。
static void SweepStaleOperationDirectories();

void CommandWindowRunner::Startup(HWND notifyWindow) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    notifyWindow_ = notifyWindow;
    // 注意：不重置 stopping_。已 Shutdown 的执行器不再接受新操作，
    // 需要复用时必须重建对象（UI 侧与本对象生命周期一致）。
  }
  // 回收上一次会话在“操作进行中退出”时留下的操作目录（那时不能删，见函数说明）。
  SweepStaleOperationDirectories();
}

void CommandWindowRunner::Shutdown() {
  std::vector<std::thread> watchers;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;  // 已停止：重复调用不做任何事。
    }
    stopping_ = true;
    notifyWindow_ = nullptr;  // 窗口即将销毁：不再向它投递通知。
    // 记录表放手，但先保留记录本身给正在收尾的线程（线程各持一份强引用）。
    watchers.swap(impl_->watchers);
  }
  stopCondition_.notify_all();
  // join 的是观察线程，不是 Git 进程 —— 用户关掉 GUI 不会打断正在执行的 Git。
  for (std::thread& watcher : watchers) {
    if (watcher.joinable()) {
      watcher.join();
    }
  }
  // 线程结束后仍没有结果的记录一律记为“结果未知”：界面绝不会停在“执行中”。
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const std::shared_ptr<CommandWindowWatchState>& state : impl_->records) {
      if (!state->completionRecorded) {
        state->completionRecorded = true;
        state->result.operationId = state->id;
        state->result.displayName = state->displayName;
        state->result.requestOperationId = state->requestOperationId;
        state->result.commandLine = state->commandLine;
        state->result.repositoryDirectory = state->repositoryDirectory;
        state->result.directory = state->directory;
        state->result.completion = git::CommandCompletion::stillUnknown;
        state->result.failureReason = L"应用退出时操作仍在进行，结果未知（命令窗口与 Git 未被终止）。";
        state->statusText = std::wstring(git::CommandCompletionLabel(git::CommandCompletion::stillUnknown));
      }
    }
  }
  TryReclaimPendingDirectories();
}

void CommandWindowRunner::TryReclaimPendingDirectories() {
  std::vector<std::wstring> pending;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(pendingCleanupDirectories_);
  }
  std::vector<std::wstring> stillBusy;
  for (const std::wstring& directory : pending) {
    if (::RemoveDirectoryW(directory.c_str()) == 0) {
      stillBusy.push_back(directory);
    }
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  for (std::wstring& directory : stillBusy) {
    pendingCleanupDirectories_.push_back(std::move(directory));
  }
}

// 清扫过期的操作目录：应用曾在操作进行中退出时，按设计不会删文件
// （仍在运行的命令窗口还要往里写结果，删了会在用户眼前报错），于是 %TEMP% 里会留下残骸。
// 这里在启动时回收“最后写入时间早于 kStaleOperationAge”的目录；正在进行的操作远新于此阈值，
// 因此不会被误删，也不依赖跨进程判断存活。
static void SweepStaleOperationDirectories() {
  constexpr ULONGLONG kStaleOperationAge = 60ULL * 60ULL * 10000ULL;  // 60 分钟（100ns 单位）
  const std::wstring tempRoot = TempRootPath();
  if (tempRoot.empty()) {
    return;
  }
  FILETIME nowFileTime{};
  ::GetSystemTimeAsFileTime(&nowFileTime);
  const ULONGLONG now =
      (static_cast<ULONGLONG>(nowFileTime.dwHighDateTime) << 32) | nowFileTime.dwLowDateTime;

  WIN32_FIND_DATAW find{};
  const std::wstring pattern = JoinPath(tempRoot, L"GcOp");
  HANDLE raw = ::FindFirstFileW((pattern + L"*").c_str(), &find);
  if (raw == INVALID_HANDLE_VALUE) {
    return;
  }
  struct FindGuard {
    HANDLE handle;
    ~FindGuard() { ::FindClose(handle); }
  } guard{raw};
  do {
    if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;
    }
    const ULONGLONG written =
        (static_cast<ULONGLONG>(find.ftLastWriteTime.dwHighDateTime) << 32) | find.ftLastWriteTime.dwLowDateTime;
    if (written >= now || now - written < kStaleOperationAge) {
      continue;  // 时间戳新于当前（时钟回拨）或仍在阈值内：可能是别的实例在用，留给下次清扫。
    }
    const std::wstring directory = JoinPath(tempRoot, find.cFileName);
    // 只回收已知的三个文件名，绝不递归删除未知内容；目录删得掉才说明真的空了。
    for (const char* name :
         {git::kStartMarkerFileName, git::kResultFileName, git::kScriptFileName}) {
      static_cast<void>(::DeleteFileW(JoinPath(directory, Utf8ToUtf16(std::string(name))).c_str()));
    }
    static_cast<void>(::RemoveDirectoryW(directory.c_str()));
  } while (::FindNextFileW(guard.handle, &find) != 0);
}

bool CommandWindowRunner::Start(const git::CommandWindowOperation& operation, uint64_t* outOperationId,
                                CommandWindowResult* failure) {
  if (outOperationId != nullptr) {
    *outOperationId = 0;
  }
  CommandWindowResult localFailure;
  localFailure.displayName = operation.displayName;
  localFailure.requestOperationId = operation.operationId;
  localFailure.commandLine = operation.gitExecutable;
  localFailure.repositoryDirectory = operation.repositoryDirectory;
  const auto reportFailure = [&](git::CommandCompletion completion, std::wstring reason) {
    localFailure.completion = completion;
    localFailure.failureReason = std::move(reason);
    if (failure != nullptr) {
      *failure = localFailure;
    }
    return false;
  };

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return reportFailure(git::CommandCompletion::launchFailed, L"执行器已停止，未启动命令窗口。");
    }
  }
  // 目录名与标题标记 = 进程 ID + 进程级全局序号。
  // 两处都必需：同一进程里可以并存多个执行器实例（测试每个用例各建一个，序号若从 1 重新开始
  // 标题就会撞车，按标题找窗口会关错目标——实测踩过）；跨进程则靠 PID 区分（实测踩过）。
  static std::atomic<uint64_t> sequence{0};
  const uint64_t id = sequence.fetch_add(1) + 1;
  const std::wstring idText =
      L"GcOp" + std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) + L"x" +
      std::to_wstring(id);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    localFailure.operationId = id;
  }

  // 1) 与平台无关的注入校验（引号区域、控制字符、长度）。
  std::wstring gitLine;
  git::CommandPlanReject reject = git::CommandPlanReject::none;
  std::wstring detail;
  if (!git::BuildGitCommandLine(operation, &gitLine, &reject, &detail)) {
    return reportFailure(git::CommandCompletion::launchFailed, detail);
  }
  localFailure.commandLine = gitLine;

  // 2) 操作独占目录：位于系统临时目录，名字即操作 ID，必须纯 ASCII
  //    （结果与标记文件靠 cmd 的重定向创建，非 ASCII 目录名会在码页往返中被破坏）。
  const std::wstring tempRoot = TempRootPath();
  if (tempRoot.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed, L"无法取得系统临时目录。");
  }
  const std::wstring directory = JoinPath(tempRoot, idText);
  std::string directoryAnsi;
  if (!EncodeToSystemAnsi(directory, directoryAnsi) || !IsAsciiBytes(directoryAnsi)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"临时目录不是 ASCII 路径，无法安全用于命令窗口脚本：" + directory);
  }
  if (::CreateDirectoryW(directory.c_str(), nullptr) == 0 && ::GetLastError() != ERROR_ALREADY_EXISTS) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"创建操作目录失败：" + FormatWindowsError(::GetLastError()));
  }
  localFailure.directory = directory;

  // 3) Git 程序必须是存在的普通文件：这一步在启动 cmd 之前完成，
  //    因此“Git 不存在”不会被误报成“执行完成、失败退出码”。
  if (!IsExistingRegularFile(operation.gitExecutable)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"Git 程序不存在或不是可执行文件：" + operation.gitExecutable);
  }
  if (!IsExistingDirectory(operation.repositoryDirectory)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"仓库目录不存在或不是目录：" + operation.repositoryDirectory);
  }

  // 4) 按系统 ANSI 码页编码进入脚本的各段；不可表示的字符直接拒绝，绝不降级成 '?'。
  std::string programAnsi;
  std::string argumentsAnsi;
  std::string titleAnsi;
  if (!EncodeToSystemAnsi(QuotePathSegment(operation.gitExecutable), programAnsi)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"Git 程序路径含系统 ANSI 码页无法表示的字符：" + operation.gitExecutable);
  }
  for (size_t index = 0; index < operation.arguments.size(); ++index) {
    std::string encoded;
    if (!EncodeToSystemAnsi(QuotePathSegment(operation.arguments[index]), encoded)) {
      return reportFailure(git::CommandCompletion::launchFailed,
                           L"参数含系统 ANSI 码页无法表示的字符：" + operation.arguments[index]);
    }
    if (index > 0) {
      argumentsAnsi.push_back(' ');
    }
    argumentsAnsi.append(encoded);
  }
  const std::wstring windowTitle =
      git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", operation.displayName, idText);
  if (!EncodeToSystemAnsi(windowTitle, titleAnsi)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"窗口标题含系统 ANSI 码页无法表示的字符：" + windowTitle);
  }

  git::CommandWindowPlan plan;
  if (!git::AssembleCommandWindowScript(idText, directoryAnsi, windowTitle, titleAnsi, programAnsi,
                                        argumentsAnsi, &plan)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"脚本内容不符合命令窗口的安全约束（引号区域或路径形态被拒绝）。");
  }
  const std::wstring scriptPath = JoinPath(directory, Utf8ToUtf16(std::string(git::kScriptFileName)));
  if (!WriteAllBytes(scriptPath, plan.scriptAnsi)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"写入一次性脚本失败：" + FormatWindowsError(::GetLastError()));
  }

  // 5) 环境块：继承当前进程环境 + 请求的受控覆盖 + 终端交互保障。
  std::vector<git::EnvironmentOverride> overrides = operation.environmentOverrides;
  for (const git::EnvironmentOverride& terminal : CommandWindowDefaultOverrides()) {
    bool present = false;
    for (const git::EnvironmentOverride& existing : overrides) {
      if (_wcsicmp(existing.name.c_str(), terminal.name.c_str()) == 0) {
        present = true;
        break;
      }
    }
    if (!present) {
      overrides.push_back(terminal);
    }
  }
  std::wstring environmentReason;
  std::wstring environmentBlock = BuildChildEnvironmentBlock(overrides, &environmentReason);
  if (environmentBlock.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed, environmentReason);
  }

  // 6) 启动 cmd.exe：显式 lpApplicationName、可写命令行缓冲、新控制台、仓库为工作目录。
  //    脚本内容不进命令行，因此用户输入不会被 shell 二次解析。
  const std::wstring cmdPath = JoinPath(SystemDirectoryPath(), L"cmd.exe");
  std::wstring applicationName = cmdPath;
  std::wstring commandLineText = QuotePathSegment(cmdPath) + L" /d /k " + QuotePathSegment(scriptPath);
  std::wstring mutableCommandLine = commandLineText;
  std::wstring workingDirectory = operation.repositoryDirectory;
  std::wstring titleBuffer = windowTitle;  // startup.lpTitle 需要可写且存活到 CreateProcessW 返回。

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_SHOWNORMAL;
  startup.lpTitle = titleBuffer.data();
  PROCESS_INFORMATION information{};
  const BOOL created =
      ::CreateProcessW(applicationName.data(), mutableCommandLine.data(), nullptr, nullptr,
                       /*bInheritHandles=*/FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                       environmentBlock.data(), workingDirectory.c_str(), &startup, &information);
  if (created == 0) {
    const unsigned long errorCode = ::GetLastError();
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"命令窗口启动失败：" + FormatWindowsError(errorCode) +
                             L"（程序：" + cmdPath + L"，工作目录：" + operation.repositoryDirectory + L"）");
  }

  auto state = std::make_shared<CommandWindowWatchState>();
  state->id = id;
  state->requestOperationId = operation.operationId;
  state->displayName = operation.displayName;
  state->directory = directory;
  state->commandLine = gitLine;
  state->repositoryDirectory = operation.repositoryDirectory;
  state->windowTitle = windowTitle;
  state->windowTitleToken = idText;
  state->statusText = L"已启动命令窗口，等待脚本执行";
  state->process.Reset(information.hProcess);
  state->thread.Reset(information.hThread);
  state->result.operationId = id;
  state->result.requestOperationId = operation.operationId;
  state->result.displayName = operation.displayName;
  state->result.commandLine = gitLine;
  state->result.repositoryDirectory = operation.repositoryDirectory;
  state->result.directory = directory;

  // 记录先进表、线程后启动：观察线程任何时候都能在表里找到自己的记录。
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    impl_->records.push_back(state);
  }
  std::thread watcher;
  try {
    watcher = std::thread([this, state] { Watch(*state); });
  } catch (const std::system_error&) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      impl_->records.erase(std::find(impl_->records.begin(), impl_->records.end(), state));
      state->process.Reset();
      state->thread.Reset();
    }
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"无法创建结果观察线程；命令窗口已打开，但本程序不会得知其退出码。");
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    impl_->watchers.push_back(std::move(watcher));
  }
  TryReclaimPendingDirectories();  // 顺带回收上一轮被命令窗口占住的目录
  if (outOperationId != nullptr) {
    *outOperationId = id;
  }
  return true;
}

std::vector<std::wstring> CommandWindowRunner::ActiveOperationNames() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::wstring> names;
  for (const std::shared_ptr<CommandWindowWatchState>& state : impl_->records) {
    if (!state->completionRecorded) {
      names.push_back(state->displayName.empty() ? state->requestOperationId : state->displayName);
    }
  }
  return names;
}

size_t CommandWindowRunner::ActiveCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const std::shared_ptr<CommandWindowWatchState>& state : impl_->records) {
    if (!state->completionRecorded) {
      ++count;
    }
  }
  return count;
}

bool CommandWindowRunner::TakeResult(uint64_t operationId, CommandWindowResult* out) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto state = impl_->Find(operationId);
  if (state == nullptr || !state->completionRecorded) {
    return false;
  }
  if (out != nullptr) {
    *out = state->result;
  }
  return true;
}

bool CommandWindowRunner::DescribeOperation(uint64_t operationId, std::wstring* status,
                                            CommandWindowResult* result) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto state = impl_->Find(operationId);
  if (state == nullptr) {
    return false;
  }
  if (status != nullptr) {
    *status = state->statusText;
  }
  if (result != nullptr && state->completionRecorded) {
    *result = state->result;
  }
  return true;
}

void CommandWindowRunner::CloseOperationWindow(uint64_t operationId) {
  HWND window = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto state = impl_->Find(operationId);
    if (state != nullptr) {
      window = state->consoleWindow;
      if (window != nullptr && ::IsWindow(window) == 0) {
        window = nullptr;  // 缓存的句柄已随窗口销毁失效：重新查找。
      }
      if (window == nullptr) {
        // 命令窗口是新建控制台，只能靠标题定位：用标题里的唯一 ASCII 标记做子串匹配，
        // 因为中文部分经过“系统 ANSI 码页编码 → cmd 按控制台码页解码”可能变形，
        // 整串精确匹配会失配（实测踩过）。标记跨码页稳定。
        window = FindWindowOwningToken(state->windowTitleToken);
        if (window == nullptr) {
          window = ::FindWindowW(nullptr, state->windowTitle.c_str());
        }
        state->consoleWindow = window;
      }
    }
  }
  if (window != nullptr && ::IsWindow(window) != 0) {
    ::PostMessageW(window, WM_CLOSE, 0, 0);
  }
}

std::wstring CommandWindowRunner::OperationDirectory(uint64_t operationId) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto state = impl_->Find(operationId);
  return state == nullptr ? std::wstring() : state->directory;
}

bool CommandWindowRunner::ConsoleExited(uint64_t operationId) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto state = impl_->Find(operationId);
  if (state == nullptr) {
    return true;  // 记录已回收：窗口自然也不再由本执行器跟踪。
  }
  return ::WaitForSingleObject(state->process.get(), 0) == WAIT_OBJECT_0;
}

void CommandWindowRunner::ClearAllResults() {
  const std::lock_guard<std::mutex> lock(mutex_);
  // 只移除“结果已登记且观察线程已跑完最后一行”的记录：
  // 线程函数仍在栈上引用记录里的路径字符串，提前释放会留下悬空引用。
  std::erase_if(impl_->records, [](const std::shared_ptr<CommandWindowWatchState>& state) {
    return state->completionRecorded && state->threadFinished;
  });
}

void CommandWindowRunner::Watch(CommandWindowWatchState& state) {
  CommandWindowRunner* runner = this;
  const std::wstring startPath = JoinPath(state.directory, Utf8ToUtf16(std::string(git::kStartMarkerFileName)));
  const std::wstring resultPath = JoinPath(state.directory, Utf8ToUtf16(std::string(git::kResultFileName)));
  const std::wstring scriptPath = JoinPath(state.directory, Utf8ToUtf16(std::string(git::kScriptFileName)));

  const auto readFile = [&startPath, &resultPath, &scriptPath](std::string_view name)
      -> std::optional<std::string> {
    const std::wstring path =
        name == git::kStartMarkerFileName ? startPath : (name == git::kResultFileName ? resultPath : scriptPath);
    std::string bytes;
    if (!ReadAllBytes(path, bytes)) {
      return std::nullopt;
    }
    return bytes;
  };

  CommandWindowResult result = state.result;
  git::CommandCompletion completion = git::CommandCompletion::launched;
  bool settled = false;
  const auto startedAt = std::chrono::steady_clock::now();

  while (!settled) {
    bool stopping = false;
    {
      std::unique_lock<std::mutex> lock(runner->mutex_);
      stopping = runner->stopping_;
      if (!stopping) {
        runner->stopCondition_.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs),
                                       [runner] { return runner->stopping_; });
        stopping = runner->stopping_;
      }
    }
    if (stopping) {
      completion = git::CommandCompletion::stillUnknown;
      result.failureReason = L"应用退出时操作仍在进行，结果未知（命令窗口与 Git 未被终止）。";
      break;
    }

    const bool processExited = ::WaitForSingleObject(state.process.get(), 0) == WAIT_OBJECT_0;
    git::CommandWindowObservation facts =
        git::ObserveCommandWindow(readFile, /*createProcessSucceeded=*/true, processExited);
    long exitCode = 0;
    completion = git::DecideCommandCompletion(facts, &exitCode);

    if (completion == git::CommandCompletion::finished) {
      result.exitCode = exitCode;
      if (exitCode == git::kCommandNotFoundExitCode) {
        completion = git::CommandCompletion::gitNotStarted;
        result.failureReason = L"Git 程序没有被创建（退出码 9009）：可能是执行期间 git.exe 被移动或删除。";
      }
      settled = true;
      break;
    }
    if (processExited && completion == git::CommandCompletion::terminated) {
      // cmd 已退出但结果文件缺失：给正在写入的结果文件一小段收尾时间，避免把“写完但未读到”
      // 误判成提前关窗。
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kProcessExitRetryWindowMs);
      while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kProcessExitRetryIntervalMs));
        long retryExitCode = 0;
        const git::CommandWindowObservation retryFacts =
            git::ObserveCommandWindow(readFile, true, true);
        if (git::DecideCommandCompletion(retryFacts, &retryExitCode) == git::CommandCompletion::finished) {
          completion = git::CommandCompletion::finished;
          result.exitCode = retryExitCode;
          break;
        }
      }
      if (completion == git::CommandCompletion::finished) {
        settled = true;
        break;
      }
    }
    if (processExited) {
      unsigned long consoleExitCode = 0;
      ::GetExitCodeProcess(state.process.get(), &consoleExitCode);
      result.consoleExitCode = consoleExitCode;
      if (completion == git::CommandCompletion::terminated) {
        result.failureReason = L"命令窗口在拿到 Git 退出码之前关闭，结果未知。";
      } else if (completion == git::CommandCompletion::scriptNeverRan) {
        result.failureReason = L"cmd.exe 未能执行一次性脚本（脚本可能被拒绝访问），结果未知。";
      }
      settled = true;
      break;
    }

    // 进程仍在：中间态只用于界面文案，不对外发通知。
    // 顺带缓存命令窗口句柄：脚本执行过 title 行之后才能按标题找到窗口，
    // 而应用退出（Shutdown）后不再有轮询线程 —— 若此刻仍未缓存，
    // 之后的 CloseOperationWindow 就只能干等。这里成功一次即停止查找。
    HWND cached = nullptr;
    bool needLookup = false;
    {
      const std::lock_guard<std::mutex> lock(runner->mutex_);
      needLookup = state.consoleWindow == nullptr;
      state.statusText = (completion == git::CommandCompletion::running) ? L"执行中"
                                                                        : L"已启动命令窗口，等待脚本执行";
    }
    if (needLookup) {
      cached = FindWindowOwningToken(state.windowTitleToken);
      if (cached != nullptr) {
        const std::lock_guard<std::mutex> lock(runner->mutex_);
        state.consoleWindow = cached;
      }
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
    if (completion == git::CommandCompletion::launched && elapsedMs > static_cast<long long>(kNoTraceGraceMs)) {
      completion = git::CommandCompletion::stillUnknown;
      result.failureReason = L"超过观察期限仍没有脚本执行痕迹，结果未知（命令窗口可能被外部关闭）。";
      settled = true;
      break;
    }
  }

  result.completion = completion;
  {
    const std::lock_guard<std::mutex> lock(runner->mutex_);
    // 标记与结果文件用完即删；目录要等命令窗口退出后才能回收，先挂起。
    ::DeleteFileW(startPath.c_str());
    ::DeleteFileW(resultPath.c_str());
    ::DeleteFileW(scriptPath.c_str());
    if (::RemoveDirectoryW(state.directory.c_str()) == 0) {
      runner->pendingCleanupDirectories_.push_back(state.directory);
    }
    state.result = result;
    state.completionRecorded = true;
    state.statusText = std::wstring(git::CommandCompletionLabel(completion));
    if (completion == git::CommandCompletion::finished || completion == git::CommandCompletion::gitNotStarted) {
      state.statusText += L"，Git 退出码 " + std::to_wstring(result.exitCode);
    }
  }
  HWND notifyWindow = nullptr;
  {
    const std::lock_guard<std::mutex> lock(runner->mutex_);
    notifyWindow = runner->stopping_ ? nullptr : runner->notifyWindow_;
  }
  if (notifyWindow != nullptr && ::IsWindow(notifyWindow) != 0) {
    ::PostMessageW(notifyWindow, CommandWindowRunner::kCompletionMessage,
                   static_cast<WPARAM>(static_cast<uint32_t>(state.id)),
                   static_cast<LPARAM>(static_cast<int64_t>(state.id) >> 32));
  }
  {
    const std::lock_guard<std::mutex> lock(runner->mutex_);
    state.threadFinished = true;  // 此后 ClearAllResults 才可以释放本记录。
  }
}

}  // namespace gc::platform
