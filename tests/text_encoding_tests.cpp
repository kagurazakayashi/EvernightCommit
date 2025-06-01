// 显式文本表示的纯逻辑测试：UTF-8 ↔ 本机宽字符、严格失败、UTF-16 单元计数。
// 同一份断言在 Windows（wchar_t=16 位）与 Linux（wchar_t=32 位）上含义相同：
// 依赖单元数的地方一律走 Utf16UnitsOf，不比较 wstring::size()。
#include "support/tiny_test.h"

#include <string>
#include <string_view>

#include "platform/text_encoding.h"

namespace {

using gc::platform::TruncateToUtf16Units;
using gc::platform::TryUtf8ToWideStrict;
using gc::platform::TryWideToUtf8Strict;
using gc::platform::Utf16UnitsOf;
using gc::platform::Utf8ToWide;
using gc::platform::WideToUtf8;

}  // namespace

GC_TEST(text_encoding_roundtrips_utf8_and_wide) {
  const std::string utf8 = "中文 emoji😀 space 引号\"' 前导- dash $&;|<>()[]{} 制表\t结束";
  const std::wstring wide = Utf8ToWide(utf8);
  GC_CHECK(!wide.empty());
  GC_CHECK(WideToUtf8(wide) == utf8);
  GC_CHECK(gc::platform::IsValidUtf8(utf8));

  std::wstring strictWide;
  GC_CHECK(TryUtf8ToWideStrict(utf8, strictWide));
  GC_CHECK(strictWide == wide);
  std::string strictBytes;
  GC_CHECK(TryWideToUtf8Strict(wide, strictBytes));
  GC_CHECK(strictBytes == utf8);
}

GC_TEST(text_encoding_strict_rejects_invalid_sequences_without_replacement) {
  const std::string cases[] = {
      std::string("\xC0\x80", 2),              // 过长编码的 NUL
      std::string("\xE4\xB8", 2),              // 截断的三字节序列
      std::string("\xED\xA0\x80", 3),          // CESU 形态的代理项
      std::string("\xF4\x90\x80\x80", 4),      // > U+10FFFF
      std::string("\x80", 1),                  // 孤立续字节
  };
  for (const std::string& bytes : cases) {
    std::wstring wide = L"sentinel";
    GC_CHECK_MESSAGE(!TryUtf8ToWideStrict(bytes, wide), "非法 UTF-8 必须严格失败");
    GC_CHECK_MESSAGE(wide.empty(), "严格失败时不得留下半份结果");
    GC_CHECK(!gc::platform::IsValidUtf8(bytes));
  }

  // 宽松转换保留可读部分，但替换符只用于显示；它绝不能拿去做路径操作。
  const std::wstring lenient = Utf8ToWide(std::string("\xFF\xFE\x41", 3));
  GC_CHECK(lenient.find(L'A') != std::wstring::npos);
  GC_CHECK(lenient.find(L'\xFFFD') != std::wstring::npos);
}

GC_TEST(text_encoding_strict_rejects_lone_surrogate_in_wide_text) {
  std::wstring lone;
  lone.push_back(L'x');
  lone.push_back(static_cast<wchar_t>(0xD800));
  lone.push_back(L'y');
  std::string bytes = "sentinel";
  GC_CHECK_MESSAGE(!TryWideToUtf8Strict(lone, bytes), "孤立代理项必须严格失败");
  GC_CHECK_MESSAGE(bytes.empty(), "严格失败时不得写出替换字符");

  const std::string lenient = WideToUtf8(lone);
  GC_CHECK(lenient.find('x') != std::string::npos);
  GC_CHECK(lenient.find('y') != std::string::npos);
  GC_CHECK(lenient.find("\xEF\xBF\xBD") != std::string::npos);
}

GC_TEST(text_encoding_counts_utf16_units_not_native_wchar) {
  const std::wstring text = Utf8ToWide("A😀中");
  GC_CHECK_MESSAGE(Utf16UnitsOf(text) == 4, "A=1、emoji=2、中=1");
  GC_CHECK(Utf16UnitsOf(Utf8ToWide("中")) == 1);
  GC_CHECK(Utf16UnitsOf(Utf8ToWide("😀")) == 2);
  GC_CHECK(Utf16UnitsOf(std::wstring{}) == 0);
}

GC_TEST(text_encoding_truncates_at_code_point_boundaries) {
  const std::wstring text = Utf8ToWide("A😀中");
  bool truncated = false;
  GC_CHECK(TruncateToUtf16Units(text, 1, &truncated) == Utf8ToWide("A"));
  GC_CHECK(truncated);
  GC_CHECK(TruncateToUtf16Units(text, 2, &truncated) == Utf8ToWide("A"));
  GC_CHECK(truncated);
  GC_CHECK(TruncateToUtf16Units(text, 3, &truncated) == Utf8ToWide("A😀"));
  GC_CHECK(truncated);
  GC_CHECK(TruncateToUtf16Units(text, 4, &truncated) == text);
  GC_CHECK(!truncated);
  // 上限 2 若按“宽字符个数”实现会从 emoji 中间截断；上面已经证明不会。
  const std::wstring cut = TruncateToUtf16Units(Utf8ToWide("😀😀"), 2);
  GC_CHECK(WideToUtf8(cut) == "😀");
}

GC_TEST(text_encoding_path_bytes_are_strict) {
  const std::wstring path = Utf8ToWide("目录/-name 引号'\"😀;$.txt");
  std::string bytes;
  GC_CHECK(gc::platform::TryPathBytesFromWide(path, bytes));
  GC_CHECK(bytes == "目录/-name 引号'\"😀;$.txt");

  std::wstring back;
  GC_CHECK(gc::platform::TryWidePathFromBytes(bytes, back));
  GC_CHECK(back == path);

  const std::string invalid("\xFF\xFE", 2);
  std::wstring rejected;
  GC_CHECK_MESSAGE(!gc::platform::TryWidePathFromBytes(invalid, rejected),
                   "非 UTF-8 文件名必须明确失败，不能替换成另一条路径");
  GC_CHECK(rejected.empty());
}
