#pragma once

#include <string>
#include <vector>

#include "git/git_probe.h"

namespace gc::platform {

// 一次 `<候选> --version` 探测的界面可用结果。
struct GitExeVerification {
  git::GitProbeOutcome outcome = git::GitProbeOutcome::fileMissing;
  std::wstring path;     // 被验证的规范化绝对路径
  std::wstring version;  // 仅 verified 时非空，如 "2.45.0.windows.1"
  std::wstring message;  // 供界面展示的具体说明（含路径、退出码或输出片段）
};

// 按当前进程 PATH 发现 git.exe（效果对应 `where git` 的搜索意图；不扫盘、不读注册表）。
// 返回去重后的绝对路径候选，顺序与 PATH 一致。
[[nodiscard]] std::vector<std::wstring> DiscoverGitCandidates();

// 把“Git 程序”输入规范化为绝对路径；无法规范化时返回整理后的原输入，由验证步骤报告原因。
[[nodiscard]] std::wstring NormalizeGitExeInput(std::wstring_view input);

// 同步执行探测：先检查路径是文件，再隐藏窗口直接运行 `<exePath> --version`（参数数组，不经 cmd）。
// 会阻塞直到退出或超时，只允许在后台工作线程调用。
[[nodiscard]] GitExeVerification VerifyGitExe(std::wstring_view exePath, unsigned long timeoutMilliseconds);

}  // namespace gc::platform
