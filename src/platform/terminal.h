#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// 交互式终端的发现、选择与启动契约。
//
// 这里只描述“用哪一个终端、以什么参数把它叫起来”，不描述 Git 操作本身：
// Git 参数、工作目录和随机口令始终写在操作目录的说明书里，由终端里的辅助程序
// 按明确的参数数组直接启动，不经过 shell。
//
// 一个重要区分：终端启动器的退出码只代表“客户端进程结束了”，它既不代表终端窗口
// 已关闭，更不代表 Git 已结束。终端客户端可以很快退出（把窗口交给已有终端服务），
// 也可以一直挂着等窗口关闭；完成判定只认操作目录里的开始标记与结果文件。

enum class TerminalAdapterKind {
  unknown = 0,
  windowsConsole,  // Windows：辅助进程自己 AllocConsole，无终端模拟器参数
  xterm,           // xterm -T <title> -e <program> <args...>
  xfce4Terminal,   // xfce4-terminal --title <title> -x <program> <args...>
  gnomeTerminal,   // gnome-terminal --title <title> --wait -- <program> <args...>
  konsole,         // konsole --title <title> -e <program> <args...>
  alacritty,       // alacritty --title <title> -e <program> <args...>
  kitty,           // kitty --title <title> <program> <args...>
};

[[nodiscard]] bool IsKnownTerminalAdapter(TerminalAdapterKind kind) noexcept;

// 稳定的 ASCII 标识（用于设置持久化与诊断），如 "xterm" / "xfce4-terminal"。
[[nodiscard]] std::wstring_view TerminalAdapterKindId(TerminalAdapterKind kind) noexcept;
[[nodiscard]] std::wstring_view TerminalAdapterDisplayName(TerminalAdapterKind kind) noexcept;

// 严格解析适配器标识；未知值返回 false，绝不猜成某个默认终端。
[[nodiscard]] bool ParseTerminalAdapterKind(std::wstring_view id, TerminalAdapterKind* out) noexcept;

struct TerminalCandidate {
  TerminalAdapterKind kind = TerminalAdapterKind::unknown;
  std::wstring programPath;  // 绝对路径；windowsConsole 为空
  // 发现时用的名字。x-terminal-emulator 解析到已知实现时记成“xfce4-terminal（经 x-terminal-emulator）”。
  std::wstring discoveredAs;
  bool viaAlternatives = false;
};

struct TerminalPreferences {
  // 用户显式选择。preferredProgramPath 非空时优先按路径匹配；
  // preferredKind 非 unknown 时优先按适配器类型匹配。
  TerminalAdapterKind preferredKind = TerminalAdapterKind::unknown;
  std::wstring preferredProgramPath;
  // 显式选择不可用时是否允许回退到其它已验证适配器。
  // 界面应在设置里让用户看到这个选择；默认允许回退，避免“装了一个用不了就完全没有终端”。
  bool allowFallback = true;
};

struct TerminalSelection {
  bool usable = false;
  TerminalCandidate selected;
  std::vector<TerminalCandidate> candidates;
  std::wstring failureReason;
  // 用户可照做的说明（缺终端、没有桌面会话、显式选择不可用等）。
  std::wstring guidance;
  // 选择过程中的非致命说明（例如显式路径不可用、已回退到默认终端）。
  std::wstring note;
};

// 纯逻辑：从候选列表按偏好选出唯一可用终端。不会启动任何程序。
[[nodiscard]] TerminalSelection SelectTerminalFromCandidates(
    const std::vector<TerminalCandidate>& candidates, const TerminalPreferences& preferences);

struct TerminalLaunchPlan {
  TerminalAdapterKind kind = TerminalAdapterKind::unknown;
  std::wstring program;                 // 终端模拟器程序（绝对路径）
  std::vector<std::wstring> arguments;  // 终端参数数组，一项一个元素，绝不拼成 shell 字符串
  std::wstring workingDirectory;        // 可选；Linux 终端通常不需要
  std::wstring failureReason;
  std::wstring note;
};

struct TerminalLaunchResult {
  bool started = false;
  TerminalAdapterKind kind = TerminalAdapterKind::unknown;
  // 终端客户端进程号，仅用于诊断/测试清理；不是 Git 的进程号，也不参与完成判定。
  std::uint64_t launcherProcessId = 0;
  std::wstring failureReason;
  std::wstring guidance;
};

// 平台实现：按 PATH 与平台规则发现候选。Linux 识别 xterm / xfce4-terminal /
// gnome-terminal / konsole / alacritty / kitty，并解析 x-terminal-emulator 的实际目标；
// Windows 只提供“控制台辅助入口”这一种。
[[nodiscard]] std::vector<TerminalCandidate> DiscoverTerminalCandidates();

// 平台实现：发现 + 纯逻辑选择 + 桌面会话可用性检查。
[[nodiscard]] TerminalSelection DiscoverAndSelectTerminal(const TerminalPreferences& preferences);

// 平台实现：启动终端模拟器并立即返回；不等待窗口关闭，也不读终端输出。
// 返回 started=true 只表示终端客户端进程已被创建，不表示 Git 已开始或已完成。
[[nodiscard]] TerminalLaunchResult LaunchTerminal(const TerminalLaunchPlan& plan);

}  // namespace gc::platform
