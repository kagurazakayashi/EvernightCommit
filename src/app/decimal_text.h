#pragma once

#include <string_view>

namespace gc::app {

// long long 的可表示上界；写成字面量而不是引 <climits>，让本头文件保持零依赖。
inline constexpr long long kDecimalLongLongMax = 9223372036854775807LL;

// 严格十进制解析：记录文件里的数字属于「损坏/异常本地数据」，因此这里既不做
// 宽松转换，也绝不允许先溢出再回绕——上界在乘加之前判定，任意长度的输入只会
// 被拒绝，不会触发有符号溢出（此前最多 19 位的写法在 9999999999999999999 上
// 已由 UBSan 复现 signed integer overflow）。
//
// 只接受 '0'..'9' 序列（可带单个前导 '-'）：空串、空白、'+5'、"1e3"、" 1"、
// "1 "、全角数字一律拒绝。允许前导零（"007" 等价 7），因为拒绝它属于格式收紧、
// 与本次的溢出修复无关。

// 非负路径：结果必须落在 [0, upperInclusive]。upperInclusive < 0 时不可能成立。
[[nodiscard]] inline bool ParseNonNegativeDecimal(std::wstring_view text,
                                                 long long upperInclusive,
                                                 long long* out) {
  if (text.empty() || out == nullptr || upperInclusive < 0) {
    return false;
  }
  long long value = 0;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') {
      return false;
    }
    const long long digit = static_cast<long long>(c - L'0');
    // value*10+digit 不超过上界的充要条件是 value <= floor((上界-digit)/10)。
    // 上界-digit 为负时整数除法向零截断会得到 0，此时只放行 value 尚为 0 的一步，
    // 循环结束后再用同一条上界兜住，因此中间值始终不会超出 [0, 上界] 或溢出。
    if (value > (upperInclusive - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  if (value > upperInclusive) {
    return false;
  }
  *out = value;
  return true;
}

// 带符号路径：结果必须落在 [lowerInclusive, upperInclusive]。
// lowerInclusive 不允许取 long long 下界（对它取负本身就是溢出），本工程用不到。
[[nodiscard]] inline bool ParseSignedDecimal(std::wstring_view text,
                                            long long lowerInclusive,
                                            long long upperInclusive,
                                            long long* out) {
  if (text.empty() || out == nullptr || lowerInclusive > upperInclusive ||
      lowerInclusive == -kDecimalLongLongMax - 1) {
    return false;
  }
  size_t index = 0;
  const bool negative = text[0] == L'-';
  if (negative) {
    index = 1;
  }
  if (index >= text.size()) {
    return false;
  }
  // 幅度上界：负号路径受 -lowerInclusive 限制，正号路径受 upperInclusive 限制。
  const long long magnitudeCap = negative ? -lowerInclusive : upperInclusive;
  if (magnitudeCap < 0) {
    return false;  // 整个区间都在 0 之外，任何数字幅度都不成立。
  }
  long long magnitude = 0;
  for (; index < text.size(); ++index) {
    const wchar_t c = text[index];
    if (c < L'0' || c > L'9') {
      return false;
    }
    const long long digit = static_cast<long long>(c - L'0');
    if (magnitude > (magnitudeCap - digit) / 10) {
      return false;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (magnitude > magnitudeCap) {
    return false;
  }
  const long long value = negative ? -magnitude : magnitude;
  if (value < lowerInclusive || value > upperInclusive) {
    return false;
  }
  *out = value;
  return true;
}

// 缩窄到 int 之前先按 int 的表示范围裁定：调用方给出的业务上界若宽于 int，
// 这里仍然拒绝，而不是截断成一个看起来合理的数。
[[nodiscard]] inline bool ParseInt32Decimal(std::wstring_view text,
                                           long long lowerInclusive,
                                           long long upperInclusive,
                                           int* out) {
  if (out == nullptr) {
    return false;
  }
  constexpr long long kInt32Min = -2147483648LL;
  constexpr long long kInt32Max = 2147483647LL;
  if (lowerInclusive < kInt32Min || upperInclusive > kInt32Max) {
    return false;
  }
  long long value = 0;
  if (!ParseSignedDecimal(text, lowerInclusive, upperInclusive, &value)) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

}  // namespace gc::app
