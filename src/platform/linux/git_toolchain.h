#pragma once

#include <string>
#include <vector>

namespace gc::platform {

// Linux 侧只做“发现 + 可用性（文件/执行位）”检查。版本验证需要子进程执行通道，
// 属于后续步骤；本模块绝不通过运行 `git --version` 伪造“已验证可用”。

}  // namespace gc::platform
