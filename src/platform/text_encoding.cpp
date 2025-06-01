#include "platform/text_encoding.h"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace gc::platform {
namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;

bool IsContinuation(unsigned char byte) noexcept { return (byte & 0xC0u) == 0x80u; }

// 严格解一个 UTF-8 码位。成功时写入 codePoint/width；失败返回 false 且不消费。
// 判据与 RFC 3629 一致：拒绝过长编码、代理项（含 CESU 三字节形态）、>U+10FFFF。
bool DecodeUtf8Strict(std::string_view bytes, size_t index, char32_t& codePoint, size_t& width) noexcept {
  const unsigned char first = static_cast<unsigned char>(bytes[index]);
  if (first < 0x80u) {
    codePoint = first;
    width = 1;
    return true;
  }
  auto continuation = [&bytes, index](size_t offset) -> int {
    if (index + offset >= bytes.size()) {
      return -1;
    }
    const unsigned char value = static_cast<unsigned char>(bytes[index + offset]);
    return IsContinuation(value) ? static_cast<int>(value) : -1;
  };

  if (first >= 0xC2u && first <= 0xDFu) {
    const int second = continuation(1);
    if (second < 0) {
      return false;
    }
    codePoint = (static_cast<char32_t>(first & 0x1Fu) << 6) | static_cast<char32_t>(second & 0x3Fu);
    width = 2;
    return true;
  }
  if (first >= 0xE0u && first <= 0xEFu) {
    const int second = continuation(1);
    const int third = continuation(2);
    if (second < 0 || third < 0) {
      return false;
    }
    if (first == 0xE0u && second < 0xA0) {
      return false;  // 过长编码
    }
    if (first == 0xEDu && second >= 0xA0) {
      return false;  // UTF-8 中的代理项（D800-DFFF / CESU）
    }
    codePoint = (static_cast<char32_t>(first & 0x0Fu) << 12) |
                (static_cast<char32_t>(second & 0x3Fu) << 6) | static_cast<char32_t>(third & 0x3Fu);
    width = 3;
    return true;
  }
  if (first >= 0xF0u && first <= 0xF4u) {
    const int second = continuation(1);
    const int third = continuation(2);
    const int fourth = continuation(3);
    if (second < 0 || third < 0 || fourth < 0) {
      return false;
    }
    if (first == 0xF0u && second < 0x90) {
      return false;  // 过长编码
    }
    if (first == 0xF4u && second >= 0x90) {
      return false;  // > U+10FFFF
    }
    codePoint = (static_cast<char32_t>(first & 0x07u) << 18) |
                (static_cast<char32_t>(second & 0x3Fu) << 12) |
                (static_cast<char32_t>(third & 0x3Fu) << 6) | static_cast<char32_t>(fourth & 0x3Fu);
    width = 4;
    return true;
  }
  return false;
}

void AppendUtf8(std::string& out, char32_t codePoint) {
  if (codePoint <= 0x7Fu) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
  } else if (codePoint <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
  }
}

wchar_t AsUnsignedWide(wchar_t unit) noexcept {
  return static_cast<wchar_t>(static_cast<std::make_unsigned_t<wchar_t>>(unit));
}

bool IsHighSurrogate(wchar_t unit) noexcept {
  const unsigned int value = static_cast<unsigned int>(AsUnsignedWide(unit));
  return value >= 0xD800u && value <= 0xDBFFu;
}

bool IsLowSurrogate(wchar_t unit) noexcept {
  const unsigned int value = static_cast<unsigned int>(AsUnsignedWide(unit));
  return value >= 0xDC00u && value <= 0xDFFFu;
}

// 把一个宽字符位置解成 Unicode 码位。成功时 width 是消耗的宽字符数；
// 非法码元返回 false 并把 width 设为 1（调用方据此跳过，不越过字符串尾）。
bool DecodeWide(std::wstring_view wide, size_t index, char32_t& codePoint, size_t& width) noexcept {
  const wchar_t unit = wide[index];
  if constexpr (sizeof(wchar_t) == 2) {
    const unsigned int value = static_cast<unsigned int>(AsUnsignedWide(unit));
    if (value >= 0xD800u && value <= 0xDBFFu) {
      if (index + 1 < wide.size() && IsLowSurrogate(wide[index + 1])) {
        const unsigned int low = static_cast<unsigned int>(AsUnsignedWide(wide[index + 1]));
        codePoint = 0x10000u + ((value - 0xD800u) << 10) + (low - 0xDC00u);
        width = 2;
        return true;
      }
      width = 1;
      return false;  // 孤立高代理项
    }
    if (value >= 0xDC00u && value <= 0xDFFFu) {
      width = 1;
      return false;  // 孤立低代理项
    }
    codePoint = static_cast<char32_t>(value);
    width = 1;
    return true;
  } else {
    codePoint = static_cast<char32_t>(static_cast<std::make_unsigned_t<wchar_t>>(unit));
    width = 1;
    if (codePoint > 0x10FFFFu) {
      return false;
    }
    if (codePoint >= 0xD800u && codePoint <= 0xDFFFu) {
      return false;  // UTF-32 平台上的代理项码位不是合法标量值
    }
    return true;
  }
}

void AppendWide(std::wstring& out, char32_t codePoint) {
  if constexpr (sizeof(wchar_t) == 2) {
    if (codePoint <= 0xFFFFu) {
      out.push_back(static_cast<wchar_t>(codePoint));
    } else {
      const char32_t value = codePoint - 0x10000u;
      out.push_back(static_cast<wchar_t>(0xD800u + (value >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00u + (value & 0x3FFu)));
    }
  } else {
    out.push_back(static_cast<wchar_t>(codePoint));
  }
}

}  // namespace

std::wstring Utf8ToWide(std::string_view utf8) {
  std::wstring out;
  out.reserve(utf8.size());
  size_t index = 0;
  while (index < utf8.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    if (DecodeUtf8Strict(utf8, index, codePoint, width)) {
      AppendWide(out, codePoint);
      index += width;
    } else {
      AppendWide(out, kReplacementCharacter);
      ++index;  // 宽松转换：每个非法字节记一个替换符，绝不猜原值。
    }
  }
  return out;
}

std::string WideToUtf8(std::wstring_view wide) {
  std::string out;
  out.reserve(wide.size());
  size_t index = 0;
  while (index < wide.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    if (DecodeWide(wide, index, codePoint, width)) {
      AppendUtf8(out, codePoint);
    } else {
      AppendUtf8(out, kReplacementCharacter);
    }
    index += width;
  }
  return out;
}

bool TryUtf8ToWideStrict(std::string_view utf8, std::wstring& outText) {
  outText.clear();
  std::wstring result;
  result.reserve(utf8.size());
  size_t index = 0;
  while (index < utf8.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    if (!DecodeUtf8Strict(utf8, index, codePoint, width)) {
      return false;
    }
    AppendWide(result, codePoint);
    index += width;
  }
  outText = std::move(result);
  return true;
}

bool TryWideToUtf8Strict(std::wstring_view wide, std::string& outBytes) {
  outBytes.clear();
  std::string result;
  result.reserve(wide.size());
  size_t index = 0;
  while (index < wide.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    if (!DecodeWide(wide, index, codePoint, width)) {
      return false;
    }
    AppendUtf8(result, codePoint);
    index += width;
  }
  outBytes = std::move(result);
  return true;
}

bool IsValidUtf8(std::string_view utf8) noexcept {
  size_t index = 0;
  while (index < utf8.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    if (!DecodeUtf8Strict(utf8, index, codePoint, width)) {
      return false;
    }
    index += width;
  }
  return true;
}

size_t Utf16UnitsOf(std::wstring_view wide) noexcept {
  size_t units = 0;
  size_t index = 0;
  while (index < wide.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    if (!DecodeWide(wide, index, codePoint, width)) {
      ++units;  // 非法孤立码元按 1 个单元计；调用方本应先拒绝这种文字。
    } else {
      units += codePoint > 0xFFFFu ? 2u : 1u;
    }
    index += width;
  }
  return units;
}

std::wstring TruncateToUtf16Units(std::wstring_view wide, size_t limit, bool* truncated) {
  size_t units = 0;
  size_t index = 0;
  while (index < wide.size()) {
    char32_t codePoint = 0;
    size_t width = 0;
    const bool valid = DecodeWide(wide, index, codePoint, width);
    const size_t codePointUnits = (!valid || codePoint <= 0xFFFFu) ? 1u : 2u;
    if (units + codePointUnits > limit) {
      break;
    }
    units += codePointUnits;
    index += width;
  }
  if (truncated != nullptr) {
    *truncated = index < wide.size();
  }
  return std::wstring(wide.substr(0, index));
}

bool TryPathBytesFromWide(std::wstring_view widePath, std::string& outBytes) {
  return TryWideToUtf8Strict(widePath, outBytes);
}

bool TryWidePathFromBytes(std::string_view bytes, std::wstring& outPath) {
  return TryUtf8ToWideStrict(bytes, outPath);
}

}  // namespace gc::platform
