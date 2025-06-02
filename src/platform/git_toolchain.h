#pragma once

#include <string>

#include "git/git_probe.h"
#include "platform/git_discovery.h"
#include "platform/process.h"

namespace gc::platform {

// 一次 `<候选> --version` 探测的界面可用结果。
struct GitExeVerification {
  git::GitProbeOutcome outcome = git::GitProbeOutcome::fileMissing;
  std::wstring path;     // 被验证的规范化绝对路径
  std::wstring version;  // 仅 verified 时非空，如 "2.45.0.windows.1"
  std::wstring message;  // 供界面展示的具体说明（含路径、退出码或输出片段）
};

// 同步执行探测：先检查路径是文件/可执行，再隐藏窗口直接运行 `<exePath> --version`
// （参数数组，不经 shell）。会阻塞直到退出、超时或被停止信号中止，
// 只允许在后台工作线程调用。cancel 只作用于这条后台探测，不涉及命令窗口。
[[nodiscard]] GitExeVerification VerifyGitExe(std::wstring_view exePath,
                                              unsigned long timeoutMilliseconds,
                                              StopFlag cancel = {});

}  // namespace gc::platform
