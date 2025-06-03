#pragma once

#include <cstdint>
#include <string>

#include "git/command_window.h"

namespace gc::platform {

// 一次外部终端操作的结果。Windows 的 CommandWindowRunner 与 Linux 的
// LinuxCommandWindowRunner 共用这一份形态，界面与操作协调器因此不必知道
// 操作是在 cmd 控制台还是 X11/Wayland 终端里跑的。
//
// 关键约束：
//   * exitCode 只在 completion == finished 时是 Git 的退出码；
//   * consoleExitCode / launcherProcessId 只用于诊断，绝不参与成败判定；
//   * 终端客户端提前退出、复用已有终端服务都不改变这条判定——
//     完成凭据始终是独占操作目录里的标记与结果文件。
struct CommandWindowResult {
  uint64_t operationId = 0;             // 执行器分配的 ID：完成通知与结果取回都按它绑定
  std::wstring requestOperationId;      // 请求携带的文本 ID（诊断展示）
  std::wstring displayName;             // 操作名称，如 "status"
  git::CommandCompletion completion = git::CommandCompletion::launchFailed;
  long exitCode = 0;                    // finished 时为 Git 退出码
  unsigned long consoleExitCode = 0;    // 辅助进程自身退出码（仅诊断，绝不当作 Git 的结果）
  std::wstring commandLine;             // 实际执行的 Git 命令（展示用）
  std::wstring repositoryDirectory;     // 命令窗口中 Git 的工作目录
  std::wstring failureReason;           // 失败/未知时的具体原因（含平台错误文本）
  std::wstring directory;               // 本次操作的临时目录（诊断与清理）
  // 集中环境策略从继承环境移除过的重定向变量告知（只含变量名，绝不含值）；没有移除则为空。
  std::wstring environmentNotice;
  [[nodiscard]] bool Success() const noexcept {
    return completion == git::CommandCompletion::finished && exitCode == 0;
  }
};

}  // namespace gc::platform
