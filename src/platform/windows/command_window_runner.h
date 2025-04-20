#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/raii.h"

namespace gc::platform {

// 执行器的内部记录类型（定义只在 .cpp）；这里前置声明供私有成员函数签名使用。
struct CommandWindowWatchState;

// 一次外部命令窗口操作的最终结果（观察线程写出，UI 线程只读）。
struct CommandWindowResult {
  uint64_t operationId = 0;             // 执行器分配的 ID：完成通知与结果取回都按它绑定
  std::wstring requestOperationId;      // 请求携带的文本 ID（诊断展示）
  std::wstring displayName;             // 操作名称，如 "status"
  git::CommandCompletion completion = git::CommandCompletion::launchFailed;
  long exitCode = 0;                    // finished 时为 Git 退出码
  unsigned long consoleExitCode = 0;    // 命令窗口辅助进程自身退出码（仅诊断，绝不当作 Git 的结果）
  std::wstring commandLine;             // 实际执行的 Git 命令（展示用）
  std::wstring repositoryDirectory;     // 命令窗口中 Git 的工作目录
  std::wstring failureReason;           // 失败/未知时的具体原因（含 Windows 错误文本）
  std::wstring directory;               // 本次操作的临时目录（诊断与清理）
  [[nodiscard]] bool Success() const noexcept {
    return completion == git::CommandCompletion::finished && exitCode == 0;
  }
};

// 外部命令窗口执行器：用户主动执行的 Git 操作一律在“新控制台 + 本程序自带的命令窗口辅助
// 入口”里运行，Git 原生交互（凭据、GPG 口令）留在该终端；Git 结束后辅助进程把窗口交给
// cmd /k 保留下来，用户可继续翻看输出。
//
// 为什么不再用 cmd.exe 的一次性批处理：批处理是字节流，进入它的每一条路径与参数都得先按
// 某个码页编码，而“中文用户名下的临时目录”“西文代码页机器上的中文仓库路径”“中文窗口标题”
// 在这些码页里根本装不下 —— 只能要么拒绝一次合法操作，要么把用户的文件名改成别的样子。
// 现在整条链路只有 Unicode API：CreateProcessW 传路径与命令行、SetConsoleTitleW 传标题、
// CreateFileW 读写操作目录里的说明书与标记文件，系统 ANSI 码页不再参与任何一步。
//
// 为什么用“辅助入口 + 说明书文件”，而不是 system/_wsystem 或把输入加引号当安全脚本：
//   * CreateProcessW 直接启动本程序的辅助模式：显式 lpApplicationName、可写命令行缓冲、
//     仓库工作目录与 Unicode 环境块；命令行上只有操作目录与随机口令两项数据。
//   * 要执行什么只来自操作独占目录里的 spec.txt（严格 UTF-8），辅助进程读回后必须
//     核对目录名与口令，再用与 GUI 同一套校验（git::BuildGitCommandLine）复核，
//     不合格就不执行，也不会把这个入口当成任意命令执行器（详见 command_window_helper.h）。
//   * 含双引号、控制字符的请求、超过数量/长度上限的请求都在启动前被拒绝。
//   * 完成凭据不是“窗口开了”或“Git 输出里的某句话”，而是独占目录里的标记文件与
//     结果文件，因此能区分“启动成功/执行中/执行完成/启动失败/结果未知”。
//   * Git 退出码写在 result.txt：辅助进程与 cmd 自身的退出码只用于诊断。
//
// 线程模型：Start 在调用线程只做“规划 + 写说明书 + 创建进程”，不等待 Git；
// 每个进行中的操作有独立观察线程，结束后登记结果并向 notifyWindow PostMessage。
// 消息只携带操作 ID，负载由执行器保管，因此窗口销毁后残留消息被系统丢弃也不会悬空。
// Shutdown 停止观察线程，但绝不终止正在运行的 Git 或命令窗口。
class CommandWindowRunner {
public:
  // 完成通知消息：wParam = 操作 ID 低 32 位，lParam = 高 32 位（x64 下 LPARAM 为 64 位）。
  static constexpr UINT kCompletionMessage = WM_APP + 20;

  CommandWindowRunner();
  CommandWindowRunner(const CommandWindowRunner&) = delete;
  CommandWindowRunner& operator=(const CommandWindowRunner&) = delete;
  ~CommandWindowRunner();

  // notifyWindow 接收完成通知；nullptr 表示不投递通知（测试用轮询取结果）。
  void Startup(HWND notifyWindow);
  // 停止观察线程并 join；进行中的操作记为“结果未知”，Git 进程不受影响。
  void Shutdown();

  // 提交一次操作。规划被拒、目录/说明书写不出、辅助进程启动失败时同步返回 false，
  // 并填写 failure（含 completion 与原因）；成功返回 true 且 outOperationId 非 0，
  // 之后一定有且仅有一次结果登记（包括“结果未知”）。
  [[nodiscard]] bool Start(const git::CommandWindowOperation& operation, uint64_t* outOperationId,
                           CommandWindowResult* failure);

  [[nodiscard]] std::vector<std::wstring> ActiveOperationNames() const;
  [[nodiscard]] size_t ActiveCount() const;
  // 取回已完成操作的结果；结果由执行器保管到 ClearAllResults 或 Shutdown。
  [[nodiscard]] bool TakeResult(uint64_t operationId, CommandWindowResult* out) const;
  // 操作的即时状态与最终结果（结果只在完成后可取）。无此操作返回 false。
  [[nodiscard]] bool DescribeOperation(uint64_t operationId, std::wstring* status,
                                       CommandWindowResult* result) const;
  // 关闭某操作的命令窗口（Git 已结束时只是收起窗口；不影响已判定的结果）。
  void CloseOperationWindow(uint64_t operationId);
  // 操作的临时目录（说明书、标记与结果文件所在处），供界面诊断与测试取证。无此操作返回空串。
  [[nodiscard]] std::wstring OperationDirectory(uint64_t operationId) const;
  // 命令窗口（辅助进程）是否已退出（记录已回收时也视为退出）。
  // 供界面决定是否还显示“窗口仍在保留”，以及测试在清理临时仓库前等待目录被释放。
  [[nodiscard]] bool ConsoleExited(uint64_t operationId) const;
  void ClearAllResults();

private:
  class Impl;

  // 观察线程本体（记录类型在本文件顶部前置声明，定义在 .cpp）。
  void Watch(CommandWindowWatchState& state);
  void TryReclaimPendingDirectories();

  mutable std::mutex mutex_;
  std::condition_variable stopCondition_;
  HWND notifyWindow_ = nullptr;
  bool stopping_ = false;
  std::vector<std::wstring> pendingCleanupDirectories_;  // 命令窗口还占着的操作目录，择机回收
  Impl* impl_ = nullptr;  // 记录表：类型只在 .cpp 定义，构造/析构同样在 .cpp
};

}  // namespace gc::platform
