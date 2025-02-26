#include "platform/windows/utf_text.h"

#include <windows.h>

#include <string_view>

namespace gc::platform {
namespace {

constexpr std::string_view kUtf8Replacement{"\xEF\xBF\xBD"};  // U+FFFD

[[nodiscard]] bool IsHighSurrogate(wchar_t unit) noexcept {
  return unit >= 0xD800 && unit <= 0xDBFF;
}

[[nodiscard]] bool IsLowSurrogate(wchar_t unit) noexcept {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

}  // namespace

std::wstring Utf8ToUtf16(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int length = static_cast<int>(utf8.size());

  const int strict = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
  if (strict > 0) {
    std::wstring result(static_cast<size_t>(strict), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, result.data(), strict) == strict) {
      return result;
    }
  }

  // 宽松转换由 Windows 负责把非法字节序列替换为 U+FFFD。
  const int lenient = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
  if (lenient <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(lenient), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, result.data(), lenient) != lenient) {
    return {};
  }
  return result;
}

std::string Utf16ToUtf8(std::wstring_view utf16) {
  if (utf16.empty()) {
    return {};
  }
  const int length = static_cast<int>(utf16.size());

  const int strict =
      ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), length, nullptr, 0, nullptr, nullptr);
  if (strict > 0) {
    std::string result(static_cast<size_t>(strict), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), length, result.data(), strict, nullptr,
                              nullptr) == strict) {
      return result;
    }
  }

  // 含孤立代理项等非法码元时逐码元转换，无法编码的位置写入 U+FFFD。
  std::string result;
  for (size_t index = 0; index < utf16.size();) {
    size_t units = 1;
    if (IsHighSurrogate(utf16[index]) && index + 1 < utf16.size() && IsLowSurrogate(utf16[index + 1])) {
      units = 2;
    }
    const wchar_t* unit = utf16.data() + index;
    const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, unit, static_cast<int>(units), nullptr, 0,
                                            nullptr, nullptr);
    if (bytes > 0) {
      const size_t offset = result.size();
      result.resize(offset + static_cast<size_t>(bytes));
      ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, unit, static_cast<int>(units), result.data() + offset,
                            bytes, nullptr, nullptr);
    } else {
      result.append(kUtf8Replacement);
    }
    index += units;
  }
  return result;
}

}  // namespace gc::platform
