#include "platform/windows/command_window_runner.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/windows/command_window_helper.h"
#include "platform/windows/environment_block.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

constexpr unsigned long kPollIntervalMs = 200;constexpr unsigned long kNoTraceGraceMs = 15000;
constexpr unsigned long kProcessExitRetryWindowMs = 1500;
constexpr unsigned long kProcessExitRetryIntervalMs = 100;
// 认领操作目录的最多重试次数：每次都用新的随机段，撞名只是极小概率的意外，
// 连撞几次就说明环境不对劲，宁可不启动这次操作，也不接受任何一个已经存在的目录。
constexpr int kOperationDirectoryClaimAttempts = 4;

// 命令窗口操作的默认环境保障（仍可被请求里的同名覆盖取代）：
//   GIT_TERMINAL_PROMPT=1                —— 凭据/口令必须在终端里问，不许被静默跳过；
//   删除 GIT_ASKPASS / SSH_ASKPASS       —— 二者会把提问搬成 GUI 弹窗，与“原生交互留在窗口里”冲突；
//   GIT_PAGER=cat                        —— 关键：新建控制台是交互式 TTY，Git 默认把
//     status/log/diff 之类输出交给分页器 less，辅助进程会停在等用户按键的那一页上，
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

// 本程序的完整路径：命令窗口辅助入口就住在同一个可执行文件里，
// 启动它时用 GetModuleFileNameW 取路径，因此装在中文目录、带空格的目录都无需任何码页转换。
std::wstring CurrentExecutablePath() {
  std::wstring buffer(MAX_PATH + 4, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size() - 1) {
      buffer.resize(length);
      return buffer;
    }
    buffer.resize(buffer.size() * 2, L'\0');  // 路径比预估长（含长路径前缀时可能很长）：扩容再试。
  }
  return {};
}

// 本次操作的随机口令：说明书里的 nonce 与命令行上传来的相符，辅助进程才肯执行。
// MSVC 的 std::random_device 取的是系统加密级随机源（内部即 rand_s），不是可预测的序号；
// 熵源异常时宁可让这次操作起不来，也不退回“进程 ID + 计数”这种能被猜到的形态。
std::wstring GenerateOperationNonce() {
  static constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
  try {
    std::random_device entropy;
    std::wstring text;
    text.reserve(32);
    for (int group = 0; group < 8; ++group) {
      const unsigned int value = entropy();
      for (int nibble = 7; nibble >= 0; --nibble) {
        text.push_back(kHexDigits[(value >> (nibble * 4)) & 0xFu]);
      }
    }
    return text;  // 32 个十六进制字符：纯 ASCII 字母数字，满足 git::IsSafeNonce
  } catch (...) {
    return {};
  }
}

// 操作目录名的随机段：与口令同一个随机源，小写十六进制 16 个字符（64 位熵），
// 长度落在 git::kOperationDirectoryRandom{Min,Max}Length 允许的区间里。
// 目录名本身不是所有权证明（所有权只来自“原子新建成功”），但它必须猜不到：
// 名字能被预测，就能被别人抢先占住，于是“只有新建成功才算认领”会退化成“这次操作起不来”。
std::string GenerateOperationRandomSegment() {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  try {
    std::random_device entropy;
    std::string text;
    text.reserve(16);
    for (int group = 0; group < 2; ++group) {
      const unsigned int value = entropy();
      for (int nibble = 7; nibble >= 0; --nibble) {
        text.push_back(kHexDigits[(value >> (nibble * 4)) & 0xFu]);
      }
    }
    return text;
  } catch (...) {
    return {};
  }
}

// FILETIME（以及 GetSystemTimeAsFileTime 的返回值）折成 100ns 刻度的无符号整数。
std::uint64_t ToFileTimeTicks(const FILETIME& value) noexcept {
  return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32u) |
         static_cast<std::uint64_t>(value.dwLowDateTime);
}

// 独占探测一个文件：以“只读 + 共享 0”开一次，不读任何字节就把它关掉。
// 判定来自两个方向：我们要读的权限得被现有句柄的共享模式允许，而现有句柄的写权限也得被我们
// 声明的共享模式（0）允许 —— 后一条必然不满足，所以只要还有任何人持有它，这次开启就被拒绝。
// 为什么不用“0 访问权”或纯属性查询（实测踩过）：Windows 对查询/设置信息这类访问不做共享判定，
// 那种探测谁都查不出来，租约机制会静默失效。只读开启不改内容、不改时间戳，对辅助进程与 Git 无副作用。
enum class FileHoldState {
  absent,   // 文件不存在（或父目录已经没了）
  free,     // 存在且无人持有
  held,     // 存在且正被某个句柄使用 —— 操作还活着的直接证据
  blocked,  // 读不出结论（权限、被标记删除、意外错误）：一律保守当作“可能还在用”
};

FileHoldState ProbeFileHold(std::wstring_view path) {
  HANDLE raw = ::CreateFileW(path.data(), GENERIC_READ, /*dwShareMode=*/0, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw != INVALID_HANDLE_VALUE) {
    static_cast<void>(::CloseHandle(raw));
    return FileHoldState::free;
  }
  switch (const unsigned long code = ::GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return FileHoldState::absent;
    case ERROR_SHARING_VIOLATION:
      return FileHoldState::held;
    default:
      static_cast<void>(code);
      return FileHoldState::blocked;
  }
}

// 目录本身是普通目录，还是重解析点（符号链接 / junction），或者读不出来。
// 用 FILE_FLAG_OPEN_REPARSE_POINT 打开才能拿到“链接自己”的属性：不带这个标志时
// 系统会顺着链接进去，链接就伪装成它指向的目标。
enum class DirectoryShape {
  plain,
  reparsePoint,
  unreadable,
};

DirectoryShape ClassifyDirectory(std::wstring_view directory) {
  UniqueHandle handle(::CreateFileW(directory.data(), /*dwDesiredAccess=*/0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
  if (!handle) {
    return DirectoryShape::unreadable;
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle.get(), &information) == 0) {
    return DirectoryShape::unreadable;
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return DirectoryShape::reparsePoint;  // 不是目录：不是本程序新建的那一个，按不可回收处理。
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return DirectoryShape::reparsePoint;
  }
  return DirectoryShape::plain;
}

// 回收一个操作目录的结果。
enum class ReclaimOutcome {
  removed,  // 已删除（或本来就不存在）
  inUse,    // 有名单内的文件还在被持有：之后再看一眼，别硬来
  blocked,  // 权限不足 / 目录里有来历不明的内容 / 目录形态不对：本轮不再重试，交给下次启动的陈旧清扫
};

struct ReclaimReport {
  ReclaimOutcome outcome = ReclaimOutcome::removed;
  std::wstring reason;
};

// 名单内文件名的宽字符形态（这些常量都是 ASCII 字面量），以及它在名单里的槽位。
std::wstring WideFileName(std::string_view name) { return Utf8ToUtf16(std::string(name)); }

size_t OperationFileIndex(std::string_view name) {
  for (size_t index = 0; index < git::kOperationFileNames.size(); ++index) {
    if (git::kOperationFileNames[index] == name) {
      return index;
    }
  }
  return git::kOperationFileNames.size();  // 找不到是代码错误：调用方据此判失败，不猜位置。
}

// 回收一个“本程序认定拥有”的操作目录：所有权由调用方保证（要么亲手原子新建，要么是超过阈值
// 且名字形态完全符合本程序生成规则的目录）。是否还活着则完全由独占探测决定，绝不看目录年龄猜。
//
// requireSpecConsumed 是留给“刚刚失败 / 刚刚结束”这条路径的额外保守条件：说明书还在而开始标记
// 还没出现时不动手，因为无法排除“辅助进程马上要读说明书”。启动清扫不用这条 —— 那里已经先过了
// 几十分钟的阈值，而租约、说明书、开始标记几个文件全部无人持有，没有任何证据表明还有人在用。
//
// 只删名单里的那几个名字：目录里还有别的文件（别人放进去的、或还没落盘完成的）时，
// RemoveDirectoryW 自然失败，内容原样保留，本例程绝不递归清空任何东西。
ReclaimReport ReclaimOperationDirectory(std::wstring_view directory, bool requireSpecConsumed) {
  ReclaimReport report;
  const std::wstring path(directory);

  switch (ClassifyDirectory(path)) {
    case DirectoryShape::plain:
      break;
    case DirectoryShape::reparsePoint:
      report.outcome = ReclaimOutcome::blocked;
      report.reason = L"目录不是普通目录（重解析点或形态不明），删除会把动作落到别处，保留：" + path;
      return report;
    case DirectoryShape::unreadable:
      report.outcome = ReclaimOutcome::blocked;
      report.reason = L"读不到目录信息（" + FormatWindowsError(::GetLastError()) +
                      L"），保留：" + path;
      return report;
  }

  std::array<FileHoldState, git::kOperationFileNames.size()> holds{};
  for (size_t index = 0; index < git::kOperationFileNames.size(); ++index) {
    const std::string_view name = git::kOperationFileNames[index];
    holds[index] = ProbeFileHold(JoinPath(path, WideFileName(name)));
    if (holds[index] == FileHoldState::held) {
      report.outcome = ReclaimOutcome::inUse;
      report.reason = L"名单内的文件仍在被使用（" + WideFileName(name) +
                      L"），操作可能还在进行，目录保留：" + path;
      return report;
    }
    if (holds[index] == FileHoldState::blocked) {
      report.outcome = ReclaimOutcome::blocked;
      report.reason = L"无法确认名单内的文件是否仍被使用（" + WideFileName(name) +
                      L"），状态未知的目录保留：" + path;
      return report;
    }
  }

  const size_t specIndex = OperationFileIndex(git::kSpecFileName);
  const size_t startIndex = OperationFileIndex(git::kStartMarkerFileName);
  if (specIndex >= holds.size() || startIndex >= holds.size()) {
    report.outcome = ReclaimOutcome::blocked;
    report.reason = L"内部错误：回收名单里缺少说明书或开始标记，目录保留：" + path;
    return report;
  }
  if (requireSpecConsumed && holds[specIndex] != FileHoldState::absent &&
      holds[startIndex] == FileHoldState::absent) {
    report.outcome = ReclaimOutcome::inUse;
    report.reason = L"说明书还在而开始标记还没出现，不能排除辅助进程尚未读它，保留：" + path;
    return report;
  }

  for (size_t index = 0; index < holds.size(); ++index) {
    if (holds[index] != FileHoldState::free) {
      continue;
    }
    const std::wstring fileName = JoinPath(path, WideFileName(git::kOperationFileNames[index]));
    if (::DeleteFileW(fileName.c_str()) == 0) {
      report.outcome = ReclaimOutcome::blocked;
      report.reason = L"删除名单内的文件失败：" + FormatWindowsError(::GetLastError()) + L"（" +
                      fileName + L"），目录保留：" + path;
      return report;
    }
  }

  if (::RemoveDirectoryW(path.c_str()) == 0) {
    const unsigned long code = ::GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
      report.outcome = ReclaimOutcome::blocked;
      report.reason = L"目录删不掉（里面还有本程序名单之外的内容，或仍被引用：" +
                      FormatWindowsError(code) + L"），保留：" + path;
      return report;
    }
  }
  report.outcome = ReclaimOutcome::removed;
  report.reason.clear();
  return report;
}

// 往自己刚原子认领的目录里落一个文件：CREATE_NEW —— 这个名字存在就说明事情不对（被人占过、
// 或本程序在同一路径上重复写过），绝不覆盖、也绝不复用已有内容。
bool WriteOwnedFileExclusive(std::wstring_view path, std::string_view bytes) {
  UniqueHandle handle(::CreateFileW(path.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    return false;
  }
  if (!bytes.empty()) {
    DWORD written = 0;
    if (::WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                    nullptr) == 0 ||
        written != static_cast<DWORD>(bytes.size())) {
      return false;
    }
  }
  // 说明书是辅助进程要读的唯一输入：写完先确认缓冲区已经落到文件系统再放手，
  // 免得“文件建出来了但内容还没可见”，辅助进程读到空文件而判本次操作不合格。
  return ::FlushFileBuffers(handle.get()) != 0;
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

// 按标题里的唯一标记（操作目录名，纯 ASCII）查找命令窗口。
// 标题现在由辅助进程用 SetConsoleTitleW 直接设置，中文部分在什么代码页的机器上都不会变形，
// 整串精确匹配也已可用；这里仍按子串标记查找，是因为窗口刚创建时标题可能还没写上，
// 而纯 ASCII 标记从始至终稳定，命中一次就可以缓存句柄。
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

// 辅助进程命令行的引用形态与 git::BuildGitCommandLine 一致：每个值整体加引号。
// Windows 不允许路径与目录名里出现双引号，口令又是纯十六进制，因此没有需要转义的引号，
// 只剩一条要处理：结尾反斜杠会把闭合引号转义成字面量，必须加倍。
// 这些值只作为参数被 CommandLineToArgvW 还原成数据，不会被任何 shell 再解析。
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

// 认领一次操作独占的目录：新建成功才算拥有，随后在同一个目录里独占创建租约并交出句柄。
// 失败时把刚新建的目录原地回收（此时还没有任何别人能碰它），绝不留下“认领失败却还占着目录”的残骸。
bool ClaimOperationDirectory(std::wstring_view tempRoot, std::wstring_view token,
                             UniqueHandle* outLease, std::wstring* outPath,
                             std::wstring* failureReason) {
  if (outLease != nullptr) {
    outLease->Reset();
  }
  if (outPath != nullptr) {
    outPath->clear();
  }
  const auto fail = [&](std::wstring reason) {
    if (failureReason != nullptr) {
      *failureReason = std::move(reason);
    }
    return false;
  };
  if (tempRoot.empty() || token.empty()) {
    return fail(L"内部错误：缺少临时目录根或操作目录名。");
  }
  if (!git::IsSafeOperationDirectoryName(token)) {
    return fail(L"操作目录名不符合本程序生成的形态：" + std::wstring(token));
  }
  const std::wstring directory = JoinPath(tempRoot, token);

  // 只有“真的新建出来”才等于拥有它：CreateDirectoryW 对已经存在的目录直接失败，
  // 因此这一次调用同时是认领与冲突检测，不需要另外去猜“这个名字是谁的、里面有什么”。
  if (::CreateDirectoryW(directory.c_str(), nullptr) == 0) {
    const unsigned long code = ::GetLastError();
    if (code == ERROR_ALREADY_EXISTS) {
      return fail(L"目录已存在，不属于本次操作（已存在的名字不是所有权证明）：" + directory);
    }
    return fail(L"创建操作目录失败：" + FormatWindowsError(code) + L"（" + directory + L"）");
  }

  // 刚新建的对象必须是个普通目录。中途被人换成指向别处的链接时，往“这个目录”里写说明书
  // 以及日后的清理都会落到目录之外，所以核对不过就先放手再拒绝。
  if (ClassifyDirectory(directory) != DirectoryShape::plain) {
    static_cast<void>(ReclaimOperationDirectory(directory, /*requireSpecConsumed=*/false));
    return fail(L"新建的操作目录不是普通目录（疑似重解析点），拒绝在其中执行：" + directory);
  }

  // 租约：独占创建（CREATE_NEW —— 目录是刚新建的，这个名字本来也不该被占用）且共享模式 0。
  // 别的实例（甚至同一进程里的另一个执行器对象）独占探测它时会撞上共享冲突，
  // 于是“目录还在进行”不需要跨进程猜；进程异常退出时由系统关闭句柄，
  // 于是“探测得到并能独占打开”同时是“前主人已经不在了”的证据。
  const std::wstring leasePath = JoinPath(directory, WideFileName(git::kLeaseFileName));
  UniqueHandle lease(::CreateFileW(leasePath.c_str(), GENERIC_WRITE, /*dwShareMode=*/0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!lease) {
    const unsigned long code = ::GetLastError();
    static_cast<void>(ReclaimOperationDirectory(directory, /*requireSpecConsumed=*/false));
    return fail(L"创建操作目录租约失败：" + FormatWindowsError(code) + L"（" + leasePath + L"）");
  }
  // 租约里只写目录名，方便出问题时人工核对；写不写得成都不是判据 —— 起作用的是这个句柄本身。
  const std::string leaseText = std::string("lease\t") + Utf16ToUtf8(std::wstring(token)) + "\r\n";
  DWORD written = 0;
  static_cast<void>(
      ::WriteFile(lease.get(), leaseText.data(), static_cast<DWORD>(leaseText.size()), &written, nullptr));
  static_cast<void>(::FlushFileBuffers(lease.get()));

  if (outLease != nullptr) {
    *outLease = std::move(lease);
  }
  if (outPath != nullptr) {
    *outPath = directory;
  }
  return true;
}

// 观察记录：执行器记录表与观察线程各持一份强引用。
// Shutdown 丢弃记录表后线程仍持有自己的那份，绝不会访问已释放内存；
// 双方都放手时记录才真正释放。
struct CommandWindowWatchState {
  uint64_t id = 0;
  std::wstring requestOperationId;
  std::wstring displayName;
  std::wstring directory;
  std::wstring directoryToken;      // 目录名（也是窗口标题里的唯一标记）
  std::string asciiNonce;           // 本次操作的随机口令：标记与结果文件都按它核对归属
  platform::UniqueHandle lease;     // 活动租约：持有期间这个目录不会被任何实例回收
  std::wstring commandLine;
  std::wstring repositoryDirectory;
  std::wstring windowTitle;         // 辅助进程用 SetConsoleTitleW 设置的控制台标题（展示用）
  std::wstring windowTitleToken;    // 标题里的唯一标记（操作目录名），用于可靠定位窗口
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
  std::vector<std::wstring> preservedDirectories;                 // 回收例程判定“不能动”的目录与原因

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

// 保留下来的目录最多记这么多条（每条含原因），只用于诊断与测试取证：
// 长时间运行或反复启动时不能让这份记录自己变成无界增长的东西。
static constexpr size_t kPreservedDirectoryLogLimit = 32;

void CommandWindowRunner::Startup(HWND notifyWindow) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    notifyWindow_ = notifyWindow;
    // 注意：不重置 stopping_。已 Shutdown 的执行器不再接受新操作，
    // 需要复用时必须重建对象（UI 侧与本对象生命周期一致）。
  }
  // 回收上一次会话在“操作进行中退出”时留下的操作目录（那时不能删，见下面函数说明）。
  SweepStaleOperationDirectories();
}

void CommandWindowRunner::RecordPreservedDirectory(std::wstring directory, std::wstring reason) {
  const std::wstring entry = directory + L" —— " + reason;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    impl_->preservedDirectories.push_back(entry);
    if (impl_->preservedDirectories.size() > kPreservedDirectoryLogLimit) {
      impl_->preservedDirectories.erase(impl_->preservedDirectories.begin());
    }
  }
  // 开发渠道留一份：调试器/DebugView 里能直接看到“哪个目录为什么没被回收”。
  ::OutputDebugStringW((L"[EvernightCommit] 保留操作目录：" + entry + L"\r\n").c_str());
}

std::vector<std::wstring> CommandWindowRunner::PreservedOperationDirectories() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return impl_->preservedDirectories;
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

// 回收那些“操作已经落账、但当时目录还被命令窗口占着”的残留。
// 这里不加时间门槛（本进程刚刚还在用，谈不上陈旧），因此按更保守的规则走：
// 说明书还没被开始标记证明“已经读走”时不动手；判定为“还有人用”的继续挂在待回收表里下次再看，
// 判定为“删不动”（权限、名单之外的内容）的不再原地空转，留下记录交给下一次启动的陈旧清扫。
void CommandWindowRunner::TryReclaimPendingDirectories() {
  std::vector<std::wstring> pending;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(pendingCleanupDirectories_);
  }
  for (const std::wstring& directory : pending) {
    const ReclaimReport report = ReclaimOperationDirectory(directory, /*requireSpecConsumed=*/true);
    if (report.outcome == ReclaimOutcome::inUse) {
      const std::lock_guard<std::mutex> lock(mutex_);
      pendingCleanupDirectories_.push_back(directory);
    } else if (report.outcome == ReclaimOutcome::blocked) {
      RecordPreservedDirectory(directory, report.reason);
    }
  }
}

// 清扫上一次会话留下的操作目录。应用曾在操作进行中退出时，按设计不会删文件
// （仍在运行的命令窗口还要往里写结果，删了会在用户眼前报错），于是 %TEMP% 里会留下残骸。
//
// “够老”只是被考虑的资格，绝不是“它已经死了”的结论：旧实现把 60 分钟写成
// 60 * 60 * 10000 个 100ns 刻度，其实只有 3.6 秒，于是每一次正常的长操作
// （凭据输入等待、大仓库 fetch/push、稍微慢一点的 status）都可能被下一次启动当成死目录清掉，
// 连带删走还没被读取的说明书。现在两道判定都要过：
//   1) 时间：超过具名阈值，且时间戳可信（缺失、等于或晚于当前时间一律不回收）；
//   2) 存活：逐个独占探测本程序自己写出的那几个文件。执行器的租约句柄、辅助进程握着的
//      开始标记，只要有一个还在被人使用，就整目录原样保留 —— 这一步不依赖进程 ID，
//      因此 PID 被系统复用也不会把别人的活动目录误判成自己的残留。
// 判定不下的（权限不足、目录形态可疑、名单之外的内容）保留并记录，绝不“猜它死了”。
void CommandWindowRunner::SweepStaleOperationDirectories() {
  const std::wstring tempRoot = TempRootPath();
  if (tempRoot.empty()) {
    return;
  }
  FILETIME nowFileTime{};
  ::GetSystemTimeAsFileTime(&nowFileTime);
  const std::uint64_t now = ToFileTimeTicks(nowFileTime);

  WIN32_FIND_DATAW find{};
  const std::wstring pattern = JoinPath(tempRoot, git::kOperationDirectoryPrefix);
  HANDLE raw = ::FindFirstFileW((pattern + std::wstring(L"*")).c_str(), &find);
  if (raw == INVALID_HANDLE_VALUE) {
    return;
  }
  struct FindGuard {
    HANDLE handle;
    ~FindGuard() { ::FindClose(handle); }
  } guard{raw};
  do {
    if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;  // 同名文件不是操作目录。
    }
    if ((find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      // 名字像操作目录，本身却是链接：删除它会顺着链接动到别处的东西，这是越界清理。
      RecordPreservedDirectory(JoinPath(tempRoot, find.cFileName),
                               L"名字符合但目录是重解析点，不回收（避免把清理带到别处）。");
      continue;
    }
    if (!git::IsSafeOperationDirectoryName(find.cFileName)) {
      continue;  // 不是本程序会生成的名字：跟本次回收无关，连看都不看里面有什么。
    }
    if (!IsOperationDirectoryOldEnoughToReclaim(ToFileTimeTicks(find.ftLastWriteTime), now,
                                                kStaleOperationDirectoryAgeTicks)) {
      continue;  // 时间戳新于当前（时钟回拨或别人刚写）或还没到阈值：可能是别的实例在用，留给下次。
    }
    const std::wstring directory = JoinPath(tempRoot, find.cFileName);
    const ReclaimReport report = ReclaimOperationDirectory(directory, /*requireSpecConsumed=*/false);
    if (report.outcome != ReclaimOutcome::removed) {
      RecordPreservedDirectory(directory, report.reason);
    }
  } while (::FindNextFileW(guard.handle, &find) != 0);
}

// 提前返回时收尾本次认领到的目录：先放手租约（句柄还握着的话目录根本删不掉），
// 再按回收例程处理；mayStillBeRunning 为真时说明命令窗口可能已经起来了，
// 保守规则会留下还没被读取的说明书，并把目录挂进待回收表。
void CommandWindowRunner::ReleaseClaimedDirectory(std::wstring_view directory,
                                                  bool mayStillBeRunning) {
  const ReclaimReport report =
      ReclaimOperationDirectory(directory, /*requireSpecConsumed=*/mayStillBeRunning);
  if (report.outcome == ReclaimOutcome::inUse) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pendingCleanupDirectories_.emplace_back(directory);
  } else if (report.outcome == ReclaimOutcome::blocked) {
    RecordPreservedDirectory(std::wstring(directory), report.reason);
  }
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
  // 数字操作 ID 只在本进程内有意义（完成通知与结果取回都按它绑定）。
  // 目录名与窗口标题标记则要跨进程、跨实例都撞不上，因此形态是 GcOp<进程ID>x<随机段>：
  // 同一进程里并存的多个执行器、被系统复用的进程 ID，都由“每次重新生成的随机段”区分，
  // 而不是由序号区分 —— 序号能被猜到，名字就能被别人抢先占住（实测踩过按标题找错窗口的坑）。
  static std::atomic<uint64_t> sequence{0};
  const uint64_t id = sequence.fetch_add(1) + 1;
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

  // 2) 操作独占目录：位于系统临时目录，目录名就是本次操作的唯一标记。
  //    临时根可以是中文用户名一类的非 ASCII 路径：说明书、标记与结果文件全部由本程序和
  //    辅助进程用 Unicode API（CreateFileW）读写，不再经过 cmd 批处理的字节流，
  //    因此“临时目录必须纯 ASCII，否则中文用户不能用”这个旧前提已经不成立。
  const std::wstring tempRoot = TempRootPath();
  if (tempRoot.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed, L"无法取得系统临时目录。");
  }
  std::wstring directoryToken;
  std::wstring directory;
  UniqueHandle lease;
  {
    std::wstring claimFailure;
    bool claimed = false;
    for (int attempt = 0; attempt < kOperationDirectoryClaimAttempts; ++attempt) {
      const std::string randomSegment = GenerateOperationRandomSegment();
      if (randomSegment.empty()) {
        claimFailure = L"无法生成操作目录名的随机段（系统随机源不可用），本次操作没有启动。";
        break;
      }
      directoryToken = std::wstring(git::kOperationDirectoryPrefix) +
                       std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) + L"x" +
                       Utf8ToUtf16(randomSegment);
      claimed = ClaimOperationDirectory(tempRoot, directoryToken, &lease, &directory, &claimFailure);
      if (claimed) {
        break;
      }
      // 撞名（别人提前占了这个名字，或上一次同 PID 同随机段的巧合）：换一个猜不到的名字再来一次，
      // 绝不在已存在的目录里落下本次操作的任何一个文件。
    }
    if (!claimed) {
      return reportFailure(git::CommandCompletion::launchFailed, claimFailure);
    }
  }
  localFailure.directory = directory;

  // 从这一行起，这个目录归本次调用所有：任何提前返回都要收尾，不能把它丢在 %TEMP% 里不管。
  // 交接给观察线程（下面 std::move(lease) 之后）时置 dismissed，由观察线程负责善后。
  struct OwnedDirectoryGuard {
    CommandWindowRunner* self;
    UniqueHandle* lease;
    std::wstring directory;
    bool dismissed = false;
    ~OwnedDirectoryGuard() {
      if (dismissed) {
        return;
      }
      lease->Reset();  // 先放手租约：句柄还握着的时候这个目录本来就删不掉。
      self->ReleaseClaimedDirectory(directory, /*mayStillBeRunning=*/false);
    }
  } ownedDirectory{this, &lease, directory};

  // 3) Git 程序必须是存在的普通文件：这一步在启动命令窗口之前完成，
  //    因此“Git 不存在”不会被误报成“执行完成、失败退出码”。
  if (!IsExistingRegularFile(operation.gitExecutable)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"Git 程序不存在或不是可执行文件：" + operation.gitExecutable);
  }
  if (!IsExistingDirectory(operation.repositoryDirectory)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"仓库目录不存在或不是目录：" + operation.repositoryDirectory);
  }

  // 4) 操作说明书：一次操作的全部语义（Git 程序、参数、工作目录、窗口标题）只以数据形态
  //    写进独占目录里的 spec.txt，由辅助进程读回、再用同一套边界校验复核之后才执行。
  //    落盘用严格 UTF-8 —— 它不是“本机码页”，是双方约定的格式，因此中文、emoji 与
  //    任何超出传统代码页的字符都原样可表达；只有孤立代理项这类非法码元会被明确拒绝，
  //    绝不写成问号、U+FFFD 或“最佳匹配”来冒充原值。
  const std::wstring windowTitle = git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口",
                                                              operation.displayName, directoryToken);
  const std::wstring nonce = GenerateOperationNonce();
  if (nonce.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"无法生成本次操作的随机口令，辅助进程无法与它绑定。");
  }
  reject = git::CommandPlanReject::none;
  detail.clear();
  std::wstring specText;
  if (!git::BuildCommandWindowSpecText(operation, directoryToken, windowTitle, nonce, &specText,
                                       &reject, &detail)) {
    return reportFailure(git::CommandCompletion::launchFailed, detail);
  }
  // 口令的 ASCII 形态是“这条痕迹属于本次操作”的凭据：开始标记与结果文件里都写它，
  // 观察端只认与这里逐字相同的那一个。说明书刚刚已经用同一个校验确认过口令的形态。
  std::string asciiNonce;
  if (!git::NonceToAscii(nonce, &asciiNonce)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"内部错误：随机口令不是 ASCII 字母数字，标记文件无法与它绑定。");
  }
  std::string specBytes;
  if (!TryUtf16ToUtf8Strict(specText, specBytes)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"操作内容含无法用 UTF-8 表达的码元（孤立代理项），已拒绝执行：" +
                             operation.repositoryDirectory);
  }
  const std::wstring specPath = JoinPath(directory, WideFileName(git::kSpecFileName));
  if (!WriteOwnedFileExclusive(specPath, specBytes)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"写入操作说明书失败：" + FormatWindowsError(::GetLastError()) + L"（" +
                             specPath + L"）");
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

  // 6) 启动命令窗口辅助进程（本程序自己的隐藏入口）：显式 lpApplicationName、可写命令行
  //    缓冲、仓库为工作目录、Unicode 环境块。新的控制台由辅助进程自己 AllocConsole，
  //    所以这里不开 CREATE_NEW_CONSOLE —— 由它自己建控制台，界面进程与它在窗口归属上才不含糊。
  //    命令行上只有“操作目录 + 随机口令”两项数据，说明书的内容与 Git 的参数都不在其上，
  //    因此没有任何东西会被 shell 二次解析，这个入口也当不了通用命令执行器。
  const std::wstring helperPath = CurrentExecutablePath();
  if (helperPath.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed, L"无法取得本程序自身的路径。");
  }
  std::wstring applicationName = helperPath;
  std::wstring commandLineText = QuotePathSegment(helperPath) + L" " +
                                 QuotePathSegment(std::wstring(kCommandWindowHelperSwitch)) + L" " +
                                 QuotePathSegment(directory) + L" " + QuotePathSegment(nonce);
  std::wstring mutableCommandLine = commandLineText;
  std::wstring workingDirectory = operation.repositoryDirectory;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_SHOWNORMAL;
  PROCESS_INFORMATION information{};
  const BOOL created =
      ::CreateProcessW(applicationName.data(), mutableCommandLine.data(), nullptr, nullptr,
                       /*bInheritHandles=*/FALSE, CREATE_UNICODE_ENVIRONMENT,
                       environmentBlock.data(), workingDirectory.c_str(), &startup, &information);
  if (created == 0) {
    const unsigned long errorCode = ::GetLastError();
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"命令窗口启动失败：" + FormatWindowsError(errorCode) +
                             L"（程序：" + helperPath + L"，工作目录：" + operation.repositoryDirectory + L"）");
  }

  auto state = std::make_shared<CommandWindowWatchState>();
  state->id = id;
  state->requestOperationId = operation.operationId;
  state->displayName = operation.displayName;
  state->directory = directory;
  state->directoryToken = directoryToken;
  state->asciiNonce = asciiNonce;
  state->lease = std::move(lease);  // 租约交给观察线程：它负责在操作落账时放手
  state->commandLine = gitLine;
  state->repositoryDirectory = operation.repositoryDirectory;
  state->windowTitle = windowTitle;
  state->windowTitleToken = directoryToken;
  state->statusText = L"已启动命令窗口，等待辅助进程就绪";
  state->process.Reset(information.hProcess);
  state->thread.Reset(information.hThread);
  state->result.operationId = id;
  state->result.requestOperationId = operation.operationId;
  state->result.displayName = operation.displayName;
  state->result.commandLine = gitLine;
  state->result.repositoryDirectory = operation.repositoryDirectory;
  state->result.directory = directory;
  ownedDirectory.dismissed = true;

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
      state->lease.Reset();  // 没有观察线程了：这里自己放手租约，目录才可能被删掉。
    }
    // 命令窗口已经打开，可能正在读说明书 —— 按“可能还在运行”的保守规则收尾，
    // 删不动的目录挂进待回收表，由下一次操作或下一次启动再看。
    ReleaseClaimedDirectory(directory, /*mayStillBeRunning=*/true);
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
        // 命令窗口是新建控制台，只能靠标题定位：优先用标题里的唯一标记做子串匹配
        // （标题由辅助进程用 SetConsoleTitleW 原样设置，中文在任何代码页的机器上都不变形；
        //  标记匹配只是给“标题还没写上”的启动瞬间留一条命中路径），
        // 命不中再退回整串精确匹配。
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
  const std::string& expectedNonce = state.asciiNonce;
  const auto readFile = [&state](std::string_view name) -> std::optional<std::string> {
    std::string bytes;
    if (!ReadAllBytes(JoinPath(state.directory, WideFileName(name)), bytes)) {
      return std::nullopt;
    }
    return bytes;
  };

  CommandWindowResult result = state.result;
  git::CommandCompletion completion = git::CommandCompletion::launched;
  long exitCode = 0;
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
    git::CommandWindowObservation facts = git::ObserveCommandWindow(
        readFile, /*createProcessSucceeded=*/true, processExited, expectedNonce);
    exitCode = 0;
    completion = git::DecideCommandCompletion(facts, &exitCode);

    // 结果行由辅助进程“写完并改名”才发布，改名可见与这里的轮询之间只差一瞬间：
    // 辅助进程已退出、正要判“提前关窗”时，给它一小段宽限，避免把“恰好读到前一瞬”误判成终态。
    // 宽限期只等 Git 的回答出现（finished / gitNotStarted 都来自合格的结果行）；
    // 期间辅助进程已退出，判定器只会再给出 terminated，不借此提前结案。
    if (completion == git::CommandCompletion::terminated) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kProcessExitRetryWindowMs);
      while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kProcessExitRetryIntervalMs));
        long retryExitCode = 0;
        const git::CommandWindowObservation retryFacts =
            git::ObserveCommandWindow(readFile, true, true, expectedNonce);
        const git::CommandCompletion retryCompletion =
            git::DecideCommandCompletion(retryFacts, &retryExitCode);
        if (retryCompletion == git::CommandCompletion::finished ||
            retryCompletion == git::CommandCompletion::gitNotStarted) {
          completion = retryCompletion;
          exitCode = retryExitCode;
          break;
        }
      }
    }

    // 结案条件只有一个，而且说死了：**判定器给出什么终态，就在这里落账**——
    // 不看命令窗口关没关。cmd /k 的保留窗口可以活到用户把它关掉为止，
    // 旧实现只对 finished 及时结案、其余终态要等进程退出，Git 报告 9009 之类的形态
    // 会把操作永远停在“执行中”，槽位、清单文件与界面文案全被拖住。这是本循环的回归点。
    if (git::IsCommandCompletionTerminal(completion)) {
      if (completion == git::CommandCompletion::finished) {
        result.exitCode = exitCode;  // 只有这一种状态携带 Git 自己的退出码。
      } else {
        result.exitCode = 0;         // 其余终态没有任何“Git 的退出码”可言，绝不留下一个像样的 0。
      }
      if (completion == git::CommandCompletion::gitNotStarted) {
        result.failureReason =
            L"Git 进程未能创建（辅助进程直接报告：git.exe 在执行期间被移动/删除，或系统拒绝创建进程）。"
            L"原因看命令窗口里的输出；本次操作 Git 没有给出任何退出码。";
      } else if (completion == git::CommandCompletion::terminated) {
        result.failureReason = L"命令窗口在拿到 Git 退出码之前关闭，结果未知。";
      } else if (completion == git::CommandCompletion::helperNeverStarted) {
        result.failureReason = L"命令窗口辅助进程没能开始执行操作（可能被安全软件拦截），结果未知。";
      }
      if (processExited) {
        unsigned long consoleExitCode = 0;
        ::GetExitCodeProcess(state.process.get(), &consoleExitCode);
        result.consoleExitCode = consoleExitCode;
      }
      settled = true;
      break;
    }

    // 中间态（launched / running）只用于界面文案，不对外发通知。
    // 顺带缓存命令窗口句柄：辅助进程设置标题之后才能按标题找到窗口，
    // 而应用退出（Shutdown）后不再有轮询线程 —— 若此刻仍未缓存，
    // 之后的 CloseOperationWindow 就只能干等。这里成功一次即停止查找。
    HWND cached = nullptr;
    bool needLookup = false;
    {
      const std::lock_guard<std::mutex> lock(runner->mutex_);
      needLookup = state.consoleWindow == nullptr;
      state.statusText = (completion == git::CommandCompletion::running) ? L"执行中"
                                                                        : L"已启动命令窗口，等待辅助进程就绪";
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
      // “已启动”超过宽限期仍没有任何可承认的痕迹：降级为终态“结果未知”，
      // 让槽位与界面照样结案，绝不停在“永远已启动”。running 不设时限是刻意的——
      // Git 可能合法地停在凭据提问上等很久，那时窗口里一切可见，用户自己决定关窗（→ terminated）。
      completion = git::CommandCompletion::stillUnknown;
      result.failureReason = L"超过观察期限仍没有执行痕迹，结果未知（命令窗口可能被外部关闭）。";
      settled = true;
      break;
    }
  }

  result.completion = completion;
  {
    const std::lock_guard<std::mutex> lock(runner->mutex_);
    // completionRecorded 只在这里、且每个观察线程恰好一次地登记：
    // 同一记录不会二次落账，槽位释放与通知投递都以这一次登记为准。
    state.result = result;
    state.completionRecorded = true;
    state.statusText = std::wstring(git::CommandCompletionLabel(completion));
    if (completion == git::CommandCompletion::finished) {
      state.statusText += L"，Git 退出码 " + std::to_wstring(result.exitCode);
    }
  }
  // 善后：先放手租约（自己的句柄还握着时目录本来就删不掉），再按“可能还有人用”的保守规则回收。
  // 命令窗口里的辅助进程可能仍在等凭据或跑 Git —— 它手里握着开始标记的句柄，回收例程独占一探
  // 就知道，于是这里只会把目录挂起来等下一次，绝不会删掉它还要用的东西，也不会报错给用户。
  // 判定为“删不动”（权限、名单之外的内容）的记一笔，交给下一次启动的陈旧清扫，不在这里空转。
  state.lease.Reset();
  const ReclaimReport reclaim =
      ReclaimOperationDirectory(state.directory, /*requireSpecConsumed=*/true);
  if (reclaim.outcome == ReclaimOutcome::inUse) {
    const std::lock_guard<std::mutex> lock(runner->mutex_);
    runner->pendingCleanupDirectories_.push_back(state.directory);
  } else if (reclaim.outcome == ReclaimOutcome::blocked) {
    runner->RecordPreservedDirectory(state.directory, reclaim.reason);
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
