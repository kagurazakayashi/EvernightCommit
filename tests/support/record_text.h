#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gc::test {

// 把「以 prefix 开头那一行」的第 fieldIndex 列（0 是记录名）换成 newValue，其余原样。
// 两份记录文件（prefs / history）落盘时都把 '|' 转义成 \v，字段里不存在裸 '|'，
// 因此按 '|' 切列对合法文本与损坏文本都可靠。找不到匹配行时原样返回（用例要自己
// 保证前缀存在，否则会看到「本该 loaded 的文本读不回」而立刻暴露）。
inline std::wstring ReplaceField(std::wstring_view text,
                                 std::wstring_view prefix,
                                 size_t fieldIndex,
                                 std::wstring_view newValue) {
  std::wstring out;
  size_t start = 0;
  bool rewritten = false;
  for (;;) {
    const size_t nl = text.find(L'\n', start);
    std::wstring line(text.substr(start, nl == std::wstring_view::npos
                                            ? std::wstring_view::npos
                                            : nl - start));
    if (!rewritten && line.rfind(prefix, 0) == 0) {
      std::vector<std::wstring> cols;
      size_t cursor = 0;
      for (;;) {
        const size_t bar = line.find(L'|', cursor);
        cols.push_back(line.substr(cursor, bar == std::wstring::npos
                                            ? std::wstring::npos
                                            : bar - cursor));
        if (bar == std::wstring::npos) {
          break;
        }
        cursor = bar + 1;
      }
      if (fieldIndex < cols.size()) {
        cols[fieldIndex] = std::wstring(newValue);
        rewritten = true;
      }
      std::wstring joined;
      for (size_t i = 0; i < cols.size(); ++i) {
        if (i != 0) {
          joined.push_back(L'|');
        }
        joined += cols[i];
      }
      line = std::move(joined);
    }
    out += line;
    if (nl == std::wstring_view::npos) {
      break;
    }
    out.push_back(L'\n');
    start = nl + 1;
  }
  return rewritten ? out : std::wstring(text);
}

// 一条非法输入：值以宽字符给出（要拼进文件文本），标签只用于失败信息。
struct BadField {
  const char* label;
  std::wstring_view value;
};

}  // namespace gc::test
