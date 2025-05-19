#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// 一条命令有三种表示，必须分开，不能用同一种简单拼接代替三者：
//   ① 交给 CreateProcessW 的参数数组（数据，一项一个元素）——生产链路上真正执行的那一份；
//   ② 给人阅读的预览（说明「将要做什么」，不承诺能被粘回终端）；
//   ③ 能在某个指定 shell 里原样粘贴执行的文本。
// 本模块只负责 ③ 的构造与「这份数据在该 shell 里能不能可靠表达」的判定，并且绝不改动 ① 的数组。
//
// 为什么需要这一步判定：Git 允许分支名里出现对 shell 有语法意义的字符（实测合法名如
// `refs/heads/demo&calc&rem`）。把这串名字裸拼进 cmd 的一行命令里，`&` 就不再是分支名，
// 而是「前一条命令跑完再跑后一条」——用户粘贴的那一刻才是风险落点（不是 GUI 的参数数组）。
// 反过来，也不能因为某个合法分支名不好粘就把它在 GUI 链路上拒掉：那是拿展示问题去限制数据。
enum class ShellDialect {
  cmdInteractive,  // 交互式 cmd（默认未启用延迟扩展；用户是否启用无法由本程序确认）
  powershell,      // PowerShell（单引号字符串按字面处理）
};

[[nodiscard]] std::wstring_view ShellDialectLabel(ShellDialect dialect) noexcept;

struct ShellCommandText {
  bool expressible = false;  // false = 这份数据在该 shell 里无法可靠表达：不提供粘贴文本
  std::wstring text;         // expressible 时的整行命令；否则为空
  std::wstring refusal;      // 不能表达时：说清是哪一段、因为什么（绝不静默替换字符）
  std::wstring note;         // 能表达时也要交代的一句（该 shell 的默认状态与边界）
};

// 把「可执行文件 + 参数数组」编成指定 shell 的一行文本。
// executable 为空时用 `git`（PATH 里的那个），与本程序实际按绝对路径启动的形态并不相同——
// 这是给人粘贴的便利形态，界面文案必须如实说明。
// arguments 一律按数据原样对待：本函数不删字符、不转义成别的值、不替用户换 shell。
[[nodiscard]] ShellCommandText FormatShellCommand(std::wstring_view executable,
                                                 const std::vector<std::wstring>& arguments,
                                                 ShellDialect dialect);

// 给人阅读的预览行：把参数数组逐字列出（不含任何 shell 语义，也绝不能再被当成命令粘回去）。
[[nodiscard]] std::wstring FormatCommandPreview(std::wstring_view executable,
                                                 const std::vector<std::wstring>& arguments);

// 结构化字段清单：不能安全粘成一行时复制这一份——每个参数一行、标明是数据不是命令。
[[nodiscard]] std::wstring FormatStructuredCommandFacts(std::wstring_view executable,
                                                        const std::vector<std::wstring>& arguments);

}  // namespace gc::git
