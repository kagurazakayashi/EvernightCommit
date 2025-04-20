#include "platform/windows/utf_text.h"

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {
namespace {

constexpr std::string_view kUtf8Replacement{"\xEF\xBF\xBD"};  // U+FFFD

[[nodiscard]] bool IsHighSurrogate(wchar_t unit) noexcept {
  return unit >= 0xD800 && unit <= 0xDBFF;
}

[[nodiscard]] bool IsLowSurrogate(wchar_t unit) noexcept {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

// CP_UTF8 的合法参数组合（这是本程序唯一还需要的“按码页选择参数”的地方）：
//   * lpDefaultChar 与 lpUsedDefaultChar 必须为 nullptr —— 传非空会让调用失败；
//   * WC_NO_BEST_FIT_CHARS 对 CP_UTF8/CP_UTF7 是非法标志，不能用它来表达“不许最佳匹配”；
//   * 反向用 MB_ERR_INVALID_CHARS 表达“非法字节序列就失败”，不许换 U+FFFD。
// 系统 ANSI 码页（CP_ACP）与 OEM 码页则相反：那里 WC_NO_BEST_FIT_CHARS 与非空
// lpUsedDefaultChar 才是表达“不可表示就失败”的手段，而两者对 CP_UTF8 都违契。
// 本程序已经不需要把任何数据编码成 ANSI 码页字节，所以这里只保留 UTF-8 一条路径。
//
// 但 WC_ERR_INVALID_CHARS 挡不住“孤立代理项”：实测（Windows 11 + UCRT）它会把
// 非法码元悄悄换成 U+FFFD 并返回成功。因此“不改写原值”不能只靠标志位，
// 两个方向都必须再做一次往返比对 —— 这也是这两个函数存在的原因。
// 孤立代理项：Windows 的两个方向都不按标准 UTF-8 处理它。
//   * 编码：WC_ERR_INVALID_CHARS 实测会把非法码元换成 U+FFFD 后“成功返回”（也可能按
//     三字节 CESU 形态原样写出），两者都等于悄悄改写了用户的名字；
//   * 解码：ED 80..BF 这个“把代理项编成三字节”的形态（CESU）被 Windows 接受，
//     而它按 The Unicode Table / RFC 3629 都是非法 UTF-8，别的实现（Git、Go、Rust）会拒绝。
// 所以两个方向都先自己做码元级判定，绝不把“合法性”交给 Windows 的脾气。
[[nodiscard]] bool HasUnpairedSurrogate(std::wstring_view utf16) {
  for (size_t index = 0; index < utf16.size(); ++index) {
    const wchar_t unit = utf16[index];
    if (IsHighSurrogate(unit)) {
      if (index + 1 >= utf16.size() || !IsLowSurrogate(utf16[index + 1])) {
        return true;
      }
      ++index;  // 代理项对整体消费掉
    } else if (IsLowSurrogate(unit)) {
      return true;  // 前面没有配对的高代理项
    }
  }
  return false;
}

[[nodiscard]] bool ContainsCesuForm(std::string_view utf8) {
  const unsigned char surrogateLead = 0xEDu;
  for (size_t index = 0; index + 1 < utf8.size(); ++index) {
    if (static_cast<unsigned char>(utf8[index]) == surrogateLead) {
      const unsigned char follow = static_cast<unsigned char>(utf8[index + 1]);
      if (follow >= 0x80u && follow <= 0xBFu) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool EncodeUtf8(std::wstring_view utf16, std::vector<char>& outBuffer, int& outLength) {
  outLength = 0;
  if (utf16.empty()) {
    return true;
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                                          static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return false;
  }
  outBuffer.assign(static_cast<size_t>(needed), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                                           static_cast<int>(utf16.size()), outBuffer.data(), needed,
                                           nullptr, nullptr);
  if (written != needed) {
    return false;
  }
  outLength = written;
  return true;
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

bool TryUtf16ToUtf8Strict(std::wstring_view utf16, std::string& outBytes) {
  outBytes.clear();
  if (HasUnpairedSurrogate(utf16)) {
    return false;  // 无效码元：明确失败，不写 U+FFFD，也不写 CESU 三字节形态。
  }
  std::vector<char> buffer;
  int length = 0;
  if (!EncodeUtf8(utf16, buffer, length)) {
    return false;
  }
  if (utf16.empty()) {
    return true;
  }
  // 往返校验：解回来必须与原码元序列逐字相同，任何“悄悄改写”都算失败。
  std::wstring roundTrip;
  if (!TryUtf8ToUtf16Strict(std::string_view(buffer.data(), static_cast<size_t>(length)), roundTrip)) {
    return false;
  }
  if (roundTrip.size() != utf16.size() ||
        std::wstring_view(roundTrip.data(), roundTrip.size()) != utf16) {
    return false;
  }
  outBytes.assign(buffer.data(), static_cast<size_t>(length));
  return true;
}

bool TryUtf8ToUtf16Strict(std::string_view utf8, std::wstring& outText) {
  outText.clear();
  if (utf8.empty()) {
    return true;
  }
  if (ContainsCesuForm(utf8)) {
    return false;  // 三字节代理项：Windows 认它，但它不是合法 UTF-8。
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
  if (needed < 0) {
    return false;
  }
  if (needed == 0) {
    // 空输入才会返回 0；上面已经处理过 empty，走到这里就是字节序列不合法。
    return utf8.empty();
  }
  std::wstring result(static_cast<size_t>(needed), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), result.data(), needed);
  if (written != needed) {
    return false;
  }
  // 再编回去比对字节：挡住“合法但被 Windows 规范化过”的形态（例如非最短形式的编码在
  // 某些码页组合下可能往返不一致），说明书里的值必须是原样的数据。
  std::vector<char> roundTrip;
  int roundLength = 0;
  if (!EncodeUtf8(std::wstring_view(result.data(), static_cast<size_t>(written)), roundTrip, roundLength)) {
    return false;
  }
  if (roundLength != static_cast<int>(utf8.size()) ||
      std::string_view(roundTrip.data(), static_cast<size_t>(roundLength)) != utf8) {
    return false;
  }
  outText = std::move(result);
  return true;
}

}  // namespace gc::platform
