#pragma once

namespace gc::platform {

// Linux 终端辅助入口：由终端模拟器启动，读操作目录里的说明书，用明确的参数数组
// 直接 posix_spawn Git（不经 shell），把 Git 的 stdin/stdout/stderr 留在终端里。
// 命令行形态：<程序> --gc-terminal-helper <操作目录> <随机口令>
inline constexpr const char* kTerminalHelperSwitch = "--gc-terminal-helper";

// 命令行第一个参数是辅助入口开关时执行并向 exitCode 写结果；否则返回 false，
// 调用方继续走自己的主流程（测试程序与未来的 Linux 主程序共用同一份入口）。
[[nodiscard]] bool RunTerminalHelperIfRequested(int argc, char** argv, int* exitCode);

}  // namespace gc::platform
