#include "support/tiny_test.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "platform/windows/utf_text.h"

namespace {

std::vector<unsigned char> Bytes(const std::string& text) {
  return std::vector<unsigned char>(text.begin(), text.end());
}

bool ContainsBytes(const std::string& text, std::vector<unsigned char> pattern) {
  const std::vector<unsigned char> haystack = Bytes(text);
  return std::search(haystack.begin(), haystack.end(), pattern.begin(), pattern.end()) != haystack.end();
}

}  // namespace

GC_TEST(utf_text_handles_empty_input) {
  GC_CHECK(gc::platform::Utf8ToUtf16(std::string{}).empty());
  GC_CHECK(gc::platform::Utf16ToUtf8(std::wstring{}).empty());
}

GC_TEST(utf_text_roundtrips_chinese_and_special_paths) {
  // 源文件为 UTF-8，因此普通窄字符串字面量就是 UTF-8 字节序列。
  const std::string utf8 = "Git 提交工具　仓库路径：C:\\项目 目录\\中文文件.txt";
  const std::wstring utf16 = gc::platform::Utf8ToUtf16(utf8);
  GC_CHECK(!utf16.empty());
  GC_CHECK(utf16.find(L"提交工具") != std::wstring::npos);
  GC_CHECK(utf16.find(L"C:\\项目 目录\\中文文件.txt") != std::wstring::npos);
  GC_CHECK(gc::platform::Utf16ToUtf8(utf16) == utf8);
}

GC_TEST(utf_text_preserves_embedded_null) {
  std::string utf8;
  utf8.push_back('a');
  utf8.push_back('\0');
  utf8 += "中文";

  const std::wstring utf16 = gc::platform::Utf8ToUtf16(std::string_view(utf8));
  GC_CHECK(utf16.size() == 4);
  GC_CHECK(utf16[1] == 0);
  GC_CHECK(gc::platform::Utf16ToUtf8(std::wstring_view(utf16)) == utf8);
}

GC_TEST(utf_text_replaces_invalid_bytes) {
  const std::string invalid = std::string("\xFF\xFE\x41", 3);
  const std::wstring utf16 = gc::platform::Utf8ToUtf16(invalid);
  GC_CHECK(!utf16.empty());
  GC_CHECK(utf16.find(L'A') != std::wstring::npos);
  GC_CHECK(utf16.find(L'\xFFFD') != std::wstring::npos);
}

GC_TEST(utf_text_replaces_unpaired_surrogate) {
  std::wstring unpaired;
  unpaired.push_back(L'x');
  unpaired.push_back(wchar_t{0xD800});
  unpaired.push_back(L'y');

  const std::string utf8 = gc::platform::Utf16ToUtf8(unpaired);
  GC_CHECK(utf8.find('x') != std::string::npos);
  GC_CHECK(utf8.find('y') != std::string::npos);
  GC_CHECK(ContainsBytes(utf8, std::vector<unsigned char>{0xEF, 0xBF, 0xBD}));
}

GC_TEST(utf_text_roundtrips_non_bmp) {
  const std::string utf8 = "\xF0\xA0\x80\x80";  // U+20000
  const std::wstring utf16 = gc::platform::Utf8ToUtf16(utf8);
  GC_CHECK(utf16.size() == 2);
  GC_CHECK(gc::platform::Utf16ToUtf8(utf16) == utf8);
}
