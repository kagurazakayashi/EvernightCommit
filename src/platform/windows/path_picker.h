#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace gc::platform {

// 对话框默认打开的目录：本进程当前工作目录（不改变进程工作目录）。
[[nodiscard]] std::wstring CurrentWorkingDirectory();

// “浏览…”按钮使用的路径选择对话框。返回 std::nullopt 表示用户取消或结果无法取到文件系统路径。
[[nodiscard]] std::optional<std::wstring> BrowseForFolder(HWND owner, std::wstring_view title,
                                                          std::wstring_view initialPath);

[[nodiscard]] std::optional<std::wstring> BrowseForExecutable(HWND owner, std::wstring_view title,
                                                              std::wstring_view initialPath);

}  // namespace gc::platform
