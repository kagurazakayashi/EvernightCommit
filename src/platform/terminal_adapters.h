#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "platform/terminal.h"

namespace gc::platform {

// 纯逻辑的终端参数构造。每个适配器只使用各自明确支持的参数：
//   xterm:        -T <title> -e <program> <args...>
//   xfce4-terminal: --title <title> -x <program> <args...>
//   gnome-terminal: --title <title> --wait -- <program> <args...>
//   konsole:      --title <title> -e <program> <args...>
//   alacritty:    --title <title> -e <program> <args...>
//   kitty:        --title <title> <program> <args...>
// windowsConsole 由 Windows 执行器直接创建辅助进程，不走这里的终端参数。
//
// 关键安全边界：进入终端命令行只有“本程序自己的辅助入口、操作目录、随机口令”三项，
// 仓库路径、分支名与提交内容都不在终端参数里；它们只存在于说明书，由辅助程序按
// 参数数组直接交给 Git。xfce4-terminal 的 -x 会按其自身规则解析剩余命令行，
// 因此这里只接受不含空白与 shell 元字符的 ASCII 简单参数；不满足时明确拒绝该适配器，
// 由选择层回退到直接按 argv 启动的终端，而不是把这些字符拼成一条 shell 命令。
[[nodiscard]] TerminalLaunchPlan BuildTerminalLaunchPlan(const TerminalCandidate& candidate,
                                                          std::wstring_view title,
                                                          std::wstring_view program,
                                                          const std::vector<std::wstring>& arguments);

// 终端标题只用于展示与人工定位；控制字符会破坏终端转义序列，直接拒绝。
[[nodiscard]] bool IsSafeTerminalTitle(std::wstring_view title) noexcept;

}  // namespace gc::platform
