#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// UTF-8（源码/文件/Git 字节流）↔ UTF-16（Win32 控件与路径）的唯一转换入口。
// 非法字节序列按 Windows 的宽松规则替换为 U+FFFD，不抛异常。
[[nodiscard]] std::wstring Utf8ToUtf16(std::string_view utf8);
[[nodiscard]] std::string Utf16ToUtf8(std::wstring_view utf16);

}  // namespace gc::platform
