#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "git/command_window.h"
#include "platform/command_window_result.h"

namespace gc::platform {

// 统一界面需要的「交互式 Git 执行器」窄接口。
//
// Windows 与 Linux 的 Runner 都保留各自的原生实现与完成判定；这里只把界面真正会用到的
// 几件事合并成一份契约，使界面代码不出现平台条件编译：
//   * Start 同步返回「是否已交给执行器」，之后的终态由调用方轮询 TakeResult/DescribeOperation；
//   * 轮询而不是回调，是因为 wx 主窗口本来就有自己的定时器，且两个平台的 Runner
//     都已保证「结果一旦可取就只取一次」；
//   * Shutdown 只停止观察线程，绝不终止正在运行的 Git 或终端。
class CommandWindowService {
public:
  CommandWindowService() = default;
  CommandWindowService(const CommandWindowService&) = delete;
  CommandWindowService& operator=(const CommandWindowService&) = delete;
  virtual ~CommandWindowService() = default;

  virtual void SweepStaleOperationDirectories() = 0;
  virtual void Shutdown() = 0;

  [[nodiscard]] virtual bool Start(const git::CommandWindowOperation& operation,
                                   uint64_t* outOperationId,
                                   CommandWindowResult* failure) = 0;
  [[nodiscard]] virtual bool TakeResult(uint64_t operationId, CommandWindowResult* out) const = 0;
  [[nodiscard]] virtual bool DescribeOperation(uint64_t operationId, std::wstring* status,
                                               CommandWindowResult* result) const = 0;
  [[nodiscard]] virtual std::wstring OperationDirectory(uint64_t operationId) const = 0;
  [[nodiscard]] virtual size_t ActiveCount() const = 0;
};

// 当前平台的执行器。Linux 使用默认终端偏好（xterm → xfce4-terminal → …）；
// Windows 使用既有的控制台辅助入口链路（不投递窗口消息，由调用方轮询结果）。
[[nodiscard]] std::unique_ptr<CommandWindowService> MakeCommandWindowService();

}  // namespace gc::platform
