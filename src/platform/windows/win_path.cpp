#include "platform/windows/win_path.h"

#include <windows.h>

namespace gc::platform {

bool IsExistingRegularFile(std::wstring_view path) {
  if (path.empty()) {
    return false;
  }
  const std::wstring value(path);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  // 目录与不存在都视为不可用；能否执行由后续 `--version` 探测给出确切原因。
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ToAbsolutePath(std::wstring_view path) {
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
