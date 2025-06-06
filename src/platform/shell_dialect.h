#pragma once

#include "git/shell_text.h"

namespace gc::platform {

// 本平台「给人复制的那一行命令」默认按哪种 shell 编排。它只影响复制文本与确认框里
// 的可粘贴形态，不改变实际执行的参数数组。Windows 是交互式 cmd，Linux 是 POSIX shell；
// 界面不写平台条件编译，需要问的时候走这一句。
[[nodiscard]] git::ShellDialect DefaultShellDialect();

}  // namespace gc::platform
