#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "git/command_window.h"
#include "platform/command_window_result.h"
#include "platform/main_thread_dispatch.h"
#include "platform/terminal.h"

namespace gc::platform {

// 执行器的内部记录类型（定义只在 .cpp 中，头文件不需要它的完整形态）。
struct RunnerImpl;

// Linux 交互式 Git 执行器：不使用后台捕获通道，也不把写操作改成隐藏执行。
//
// 一次 Start 做四件事：
//   1) 与 GUI 同一套边界校验（操作 ID、程序/工作目录、参数数量与字符安全）；
//   2) 在临时根下原子认领 GcOp<pid>x<随机段> 目录，独占写说明书与租约；
//   3) 用选中的终端适配器启动“本程序自己的辅助入口”，终端命令行上只有操作目录与口令；
//   4) 由独立观察线程读开始标记/结果文件，按真实 Git 退出码结案并投递完成通知。
//
// 终端客户端退出、复用已有终端服务、GUI 提前关闭都不会被当成 Git 结束；反过来，
// Git 结束也不会等待终端窗口关闭。终端被用户关闭时，辅助进程的 flock 随进程释放，
// 观察端据此把“已开跑但没拿到结果”报为结果未知，绝不自动重试。
class LinuxCommandWindowRunner {
public:
  explicit LinuxCommandWindowRunner(
      std::shared_ptr<MainThreadDispatcher> dispatcher = nullptr, uint64_t sourceId = 0);
  LinuxCommandWindowRunner(const LinuxCommandWindowRunner&) = delete;
  LinuxCommandWindowRunner& operator=(const LinuxCommandWindowRunner&) = delete;
  ~LinuxCommandWindowRunner();

  // 回收上一次会话留下的陈旧操作目录。桌面主程序在启动时调用一次；测试可直接调用。
  void SweepStaleOperationDirectories();

  // 停止观察线程并 join；进行中的操作记为结果未知，终端与 Git 不受影响。
  void Shutdown();

  // 完成通知：只携带 {sourceId, 操作 ID}；未设置调度器时结果由调用方轮询取回。
  void SetCompletionDispatcher(std::shared_ptr<MainThreadDispatcher> dispatcher,
                               uint64_t sourceId);

  // 仅测试：固定一个已验证的终端候选（例如测试自己的假终端脚本），跳过桌面发现。
  // 生产代码不调用；它让没有图形会话的机器也能验证“终端启动 → 辅助程序 → Git 结果”链路。
  void SetTerminalSelectionForTest(TerminalSelection selection);

  // 提交一次操作。同步失败（校验/终端不可用/目录写不出/终端启动失败）返回 false，
  // 并填写 failure；成功返回 true 且之后一定有且仅有一次结果登记。
  [[nodiscard]] bool Start(const git::CommandWindowOperation& operation,
                           const TerminalPreferences& preferences, uint64_t* outOperationId,
                           CommandWindowResult* failure);

  [[nodiscard]] std::vector<std::wstring> ActiveOperationNames() const;
  [[nodiscard]] size_t ActiveCount() const;
  [[nodiscard]] bool TakeResult(uint64_t operationId, CommandWindowResult* out) const;
  [[nodiscard]] bool DescribeOperation(uint64_t operationId, std::wstring* status,
                                       CommandWindowResult* result) const;
  [[nodiscard]] std::wstring OperationDirectory(uint64_t operationId) const;
  [[nodiscard]] unsigned long long LauncherProcessId(uint64_t operationId) const;
  [[nodiscard]] std::vector<std::wstring> PreservedOperationDirectories() const;
  void ClearAllResults();

private:
  RunnerImpl* impl_ = nullptr;
};

}  // namespace gc::platform
