#include "platform/windows/commit_message_file.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "platform/windows/raii.h"
#include "platform/windows/utf_text.h"

namespace gc::platform {
namespace {

constexpr int kCreateRetryLimit = 8;
// 文件名里的固定前缀：纯 ASCII，且不含会被 cmd 特殊对待的字符。
constexpr const wchar_t* kFilePrefix = L"gc-msg";

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

// 进程内递增序号：同一进程里连着两次「确认提交」各有各的信息文件，
// 前一份还没被命令窗口里的 Git 读完时绝不能被后一份覆盖。
unsigned long NextSequence() {
  static std::atomic<unsigned long> counter{0};
  return counter.fetch_add(1) + 1;
}

}  // namespace

CommitMessageFileWrite WriteCommitMessageFile(std::wstring_view utf16Message) {
  CommitMessageFileWrite result;
  if (utf16Message.empty()) {
    result.failureReason = L"提交信息是空的，没有写出信息文件，也没有执行任何命令。";
    return result;
  }
  const std::wstring directory = TempDirectory();
  if (directory.empty()) {
    result.failureReason =
        L"无法确定系统临时目录（GetTempPathW 失败：" + FormatWindowsError(::GetLastError()) + L"）。";
    return result;
  }

  // 正文按 UTF-8 原样落盘：行尾、空白、中文一概不动，只在末尾补一个换行。
  std::string payload = Utf16ToUtf8(utf16Message);
  if (payload.empty()) {
    result.failureReason = L"提交信息编码成 UTF-8 之后是空的（只有代理项被丢掉了），已拒绝写出。";
    return result;
  }
  if (payload.back() != '\n') {
    payload.push_back('\n');
  }

  const unsigned long pid = ::GetCurrentProcessId();
  for (int attempt = 0; attempt < kCreateRetryLimit; ++attempt) {
    const std::wstring path = directory + L"\\" + kFilePrefix + L"-" + std::to_wstring(pid) + L"-" +
                              std::to_wstring(NextSequence()) + L".txt";
    UniqueHandle handle(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!handle) {
      continue;  // 同名文件已存在（别人占用）：换下一个序号，绝不覆盖别人的档案。
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(handle.get(), payload.data(), static_cast<DWORD>(payload.size()),
                                &written, nullptr);
    const unsigned long writeError = ::GetLastError();
    if (ok == 0 || written != static_cast<DWORD>(payload.size())) {
      result.failureReason = L"写入提交信息文件失败：" + FormatWindowsError(writeError) + L"（" + path +
                             L"）";
      return result;
    }
    result.written = true;
    result.path = path;
    result.payloadBytes = payload.size();
    return result;
  }

  result.failureReason = L"临时目录里已连续 " + std::to_wstring(kCreateRetryLimit) +
                         L" 个候选文件名都无法独占创建，已放弃写出提交信息（不覆盖任何现有文件）。";
  return result;
}

void RemoveCommitMessageFile(std::wstring_view path) {
  if (path.empty()) {
    return;
  }
  static_cast<void>(::DeleteFileW(std::wstring(path).c_str()));
}

}  // namespace gc::platform
