#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// 路径是否为存在的普通文件（目录与不存在都返回 false）。能否执行由 --version 探测判定。
[[nodiscard]] bool IsExistingRegularFile(std::wstring_view path);

// 路径是否为存在的目录（仓库输入的基本可用性检查；文件与不存在都返回 false）。
[[nodiscard]] bool IsExistingDirectory(std::wstring_view path);

// GetFullPathNameW 规范化：折叠 "."、".."、多余斜杠，相对路径按进程当前目录展开。失败返回空串。
[[nodiscard]] std::wstring ToAbsolutePath(std::wstring_view path);

// 以 baseDirectory 为基准展开相对路径（Git 的 --git-common-dir 在旧版本可能返回相对路径）。
// path 已是绝对路径时只做折叠；baseDirectory 为空时按进程当前目录展开。失败返回空串。
[[nodiscard]] std::wstring ToAbsolutePathInDirectory(std::wstring_view baseDirectory, std::wstring_view path);

// 把正斜杠统一成反斜杠（Git 在 Windows 上输出正斜杠路径；两者在本系统等价）。
[[nodiscard]] std::wstring NormalizePathSeparators(std::wstring_view path);

// 是否已是绝对路径（盘符或 UNC 前缀）；用于判断是否需要按仓库目录展开。
[[nodiscard]] bool IsAbsolutePath(std::wstring_view path);

// 当前进程的 PATH 环境变量（UTF-16）。未定义时返回空串。
[[nodiscard]] std::wstring GetPathVariable();

}  // namespace gc::platform
