#include "platform/windows/pathspec_file.h"

#include <windows.h>

#include <algorithm>
#include <atomic>

#include "platform/windows/raii.h"
#include "platform/windows/utf_text.h"

namespace gc::platform {
namespace {

constexpr int kCreateRetryLimit = 8;

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

std::wstring TempDirectory() {
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

// 进程内递增序号：同一进程里连续几次暂存都要有自己的清单，前一次的还没被 Git 读完时
// 不能被后一次覆盖。序号只保证本进程不重名，因此文件名同时带上进程号。
unsigned long NextSequence() {
  static std::atomic<unsigned long> counter{0};
  return counter.fetch_add(1) + 1;
}

}  // namespace

PathspecFileWrite WriteNulPathspecFile(const std::vector<std::wstring>& pathspecElements,
                                      std::wstring_view filePrefix) {
  PathspecFileWrite result;
  if (pathspecElements.empty()) {
    result.failureReason = L"路径清单为空：没有范围的可执行命令一律拒绝，不写清单文件。";
    return result;
  }
  const std::wstring directory = TempDirectory();
  if (directory.empty()) {
    result.failureReason =
        L"无法确定系统临时目录（GetTempPathW 失败：" + FormatWindowsError(::GetLastError()) + L"）。";
    return result;
  }

  // 字节形态一次拼好再写：每条 pathspec 元素按 UTF-8 原样编码、结尾一个 NUL。
  // 元素已由 git/staging_plan 带上 :(literal) 字面前缀，这里不补前缀、也不做任何转义。
  std::string payload;
  for (const std::wstring& element : pathspecElements) {
    if (element.empty()) {
      result.failureReason = L"路径清单里出现空条目，已拒绝写出。";
      return result;
    }
    payload += Utf16ToUtf8(element);
    payload.push_back('\0');
  }

  // 文件名前缀也要能安全进命令行（清单路径会作为参数出现在命令窗口执行的那一条里）：
  // 调用方给的是本模块内写死的 ASCII 前缀，这里仍然复核一次，不接受意外形态。
  std::wstring prefix(filePrefix);
  if (prefix.empty() || prefix.size() > 16 ||
      !std::all_of(prefix.begin(), prefix.end(),
                   [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-'; })) {
    result.failureReason = L"清单文件名前缀不符合纯字母与减号的约定，已拒绝写出。";
    return result;
  }

  const unsigned long pid = ::GetCurrentProcessId();
  for (int attempt = 0; attempt < kCreateRetryLimit; ++attempt) {
    const std::wstring path = directory + L"\\" + prefix + L"-" + std::to_wstring(pid) + L"-" +
                              std::to_wstring(NextSequence()) + L".nul";
    UniqueHandle handle(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!handle) {
      // 同名文件已存在（别人占用）就换下一个序号，绝不覆盖别人的档案。
      continue;
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(handle.get(), payload.data(), static_cast<DWORD>(payload.size()),
                                &written, nullptr);
    const unsigned long writeError = ::GetLastError();
    if (ok == 0 || written != static_cast<DWORD>(payload.size())) {
      result.failureReason = L"写入路径清单失败：" + FormatWindowsError(writeError) + L"（" + path + L"）";
      return result;
    }
    result.written = true;
    result.path = path;
    return result;
  }

  result.failureReason = L"临时目录里已连续 " + std::to_wstring(kCreateRetryLimit) +
                         L" 个候选文件名都无法独占创建，已放弃写出路径清单（不覆盖任何现有文件）。";
  return result;
}

void RemoveNulPathspecFile(std::wstring_view path) {
  if (path.empty()) {
    return;
  }
  // 善后清理：此时 Git 已经退出。删不掉（被占用、权限）不影响仓库状态，也不需要告诉用户 ——
  // 用户要知道的是 Git 的结果，不是一个临时清单文件的删除状态；临时目录里的残留可被系统回收。
  static_cast<void>(::DeleteFileW(std::wstring(path).c_str()));
}

}  // namespace gc::platform
