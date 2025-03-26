#include "platform/windows/win_path.h"

#include <windows.h>

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

}  // namespace gc::platform
