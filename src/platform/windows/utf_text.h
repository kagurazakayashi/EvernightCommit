#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// UTF-8（源码/文件/Git 字节流）↔ UTF-16（Win32 控件与路径）的唯一转换入口。
// 非法字节序列按 Windows 的宽松规则替换为 U+FFFD，不抛异常。
[[nodiscard]] std::wstring Utf8ToUtf16(std::string_view utf8);
[[nodiscard]] std::string Utf16ToUtf8(std::wstring_view utf16);

// 严格编码：UTF-16 → UTF-8。含孤立代理项等非法码元时返回 false 且不写 outBytes，
// 绝不用 U+FFFD、问号或“最佳匹配”顶替原值——调用方必须能区分“编码失败”与“编码成别的字”。
// 空串编码结果为空串且返回 true。
[[nodiscard]] bool TryUtf16ToUtf8Strict(std::wstring_view utf16, std::string& outBytes);

// 严格解码：UTF-8 → UTF-16。字节序列不合法（截断的序列、非法起始字节、代理项编码等）
// 时返回 false 且不写 outText，同样不产生 U+FFFD。空串解码结果为空串且返回 true。
[[nodiscard]] bool TryUtf8ToUtf16Strict(std::string_view utf8, std::wstring& outText);

}  // namespace gc::platform
