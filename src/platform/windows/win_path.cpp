#include "platform/windows/win_path.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "platform/windows/raii.h"

namespace gc::platform {
namespace {

std::wstring CollapseWithFullPathName(std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  const std::wstring value(path);
  const DWORD needed = ::GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
  if (needed == 0 || needed > 32768) {
    return {};
  }
  std::wstring buffer(needed, L'\0');
  const DWORD written = ::GetFullPathNameW(value.c_str(), needed, buffer.data(), nullptr);
  if (written == 0 || written >= needed) {
    return {};
  }
  buffer.resize(written);
  return buffer;
}

}  // namespace

bool IsExistingRegularFile(std::wstring_view path) {
  if (path.empty()) {
    return false;
  }
  const std::wstring value(path);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  // 目录与不存在都视为不可用；能否执行由后续 `--version` 探测给出确切原因。
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsExistingDirectory(std::wstring_view path) {
  if (path.empty()) {
    return false;
  }
  const std::wstring value(path);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ToAbsolutePath(std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  const std::wstring normalized = NormalizePathSeparators(path);
  return CollapseWithFullPathName(normalized);
}

std::wstring ToAbsolutePathInDirectory(std::wstring_view baseDirectory, std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  const std::wstring normalized = NormalizePathSeparators(path);
  if (baseDirectory.empty() || IsAbsolutePath(normalized)) {
    return CollapseWithFullPathName(normalized);
  }
  const std::wstring base = CollapseWithFullPathName(NormalizePathSeparators(baseDirectory));
  if (base.empty()) {
    return CollapseWithFullPathName(normalized);
  }
  std::wstring combined(base);
  if (combined.back() != L'\\') {
    combined.push_back(L'\\');
  }
  // 组合后再折叠，"D:\repo" + "..\repo2" 这类相对段才能正确上溯。
  combined.append(normalized);
  const std::wstring collapsed = CollapseWithFullPathName(combined);
  return collapsed.empty() ? normalized : collapsed;
}

std::wstring NormalizePathSeparators(std::wstring_view path) {
  std::wstring normalized(path);
  for (wchar_t& c : normalized) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  return normalized;
}

bool IsAbsolutePath(std::wstring_view path) {
  if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
    return true;  // UNC：\\server\share
  }
  if (path.size() < 3) {
    return false;
  }
  const bool driveLetter = (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
  return driveLetter && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring GetPathVariable() {
  const DWORD needed = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring buffer(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(L"PATH", buffer.data(), needed);
  if (written == 0 || written >= needed) {
    return {};
  }
  buffer.resize(written);
  return buffer;
}

git::WorktreeFileFacts ProbeWorktreeFileForPreview(std::wstring_view absolutePath) {
  git::WorktreeFileFacts facts;
  if (absolutePath.empty()) {
    facts.failureReason = L"路径为空。";
    return facts;  // probed 保持 false：连要探测哪个路径都不知道，一律按不可用处理。
  }
  const std::wstring value(absolutePath);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = ::GetLastError();
    facts.probed = true;
    facts.exists = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    if (!facts.exists) {
      facts.failureReason = L"读取文件属性失败（Windows 错误码 " + std::to_wstring(error) + L"）。";
    }
    return facts;
  }
  facts.probed = true;
  facts.exists = true;
  facts.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (facts.isDirectory) {
    return facts;
  }

  // 目录資訊之外還要確認「打得開、讀得動」：僅有存在性不足以判斷 Git 会不会报
  // “Could not access”，而那個錯誤的退出碼與「有差異」完全相同，只能在这里提前问清楚。
  const HANDLE file =
      ::CreateFileW(value.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    facts.readable = false;
    facts.failureReason = L"无法打开该文件用于读取（Windows 错误码 " +
                          std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return facts;
  }
  const UniqueHandle guard(file);
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(guard.get(), &size) == 0 || size.QuadPart < 0) {
    facts.readable = false;
    facts.failureReason = L"无法取得该文件的大小（Windows 错误码 " +
                          std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return facts;
  }
  facts.readable = true;
  facts.sizeBytes = static_cast<unsigned long long>(size.QuadPart);
  if (facts.sizeBytes == 0) {
    return facts;  // 空文件沒有採樣可讀，按文字（零內容）對待。
  }

  std::vector<char> bytes(git::kBinaryProbeBytes);
  DWORD got = 0;
  const DWORD want =
      static_cast<DWORD>(std::min<size_t>(bytes.size(), static_cast<size_t>(facts.sizeBytes)));
  if (::ReadFile(guard.get(), bytes.data(), want, &got, nullptr) == 0) {
    // 大小拿到了但讀不出一個字節：視為不可讀，宁可不開窗口也不讓窗口里出現一句看不懂的話。
    facts.readable = false;
    facts.failureReason = L"文件存在但读不出内容（Windows 错误码 " +
                          std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return facts;
  }
  facts.containsNullByte = std::find(bytes.begin(), bytes.begin() + got, '\0') != (bytes.begin() + got);
  return facts;
}

}  // namespace gc::platform
