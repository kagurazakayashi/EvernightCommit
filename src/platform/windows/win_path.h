#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// 路径是否为存在的普通文件（目录与不存在都返回 false）。能否执行由 --version 探测判定。
[[nodiscard]] bool IsExistingRegularFile(std::wstring_view path);

// GetFullPathNameW 规范化：折叠 "."、".."、多余斜杠，相对路径按进程当前目录展开。失败返回空串。
[[nodiscard]] std::wstring ToAbsolutePath(std::wstring_view path);

// 当前进程的 PATH 环境变量（UTF-16）。未定义时返回空串。
[[nodiscard]] std::wstring GetPathVariable();

}  // namespace gc::platform
