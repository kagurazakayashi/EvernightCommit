#pragma once

#include <string>

namespace gc::platform {

// Linux 临时目录解析结果（供测试与诊断使用；生产写出函数内部走同一套规则）。
// 规则见 docs/architecture.md「临时文件」：绝对路径、不是符号链接、属主是当前用户；
// 组/其他人可写时必须带 sticky 位（/tmp 的 1777 形态）。
struct ScratchDirectory {
  bool ok = false;
  std::wstring path;
  std::wstring failureReason;
};

[[nodiscard]] ScratchDirectory ResolveScratchDirectory();

}  // namespace gc::platform
