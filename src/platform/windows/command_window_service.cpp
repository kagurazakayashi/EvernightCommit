#include "platform/command_window_service.h"

#include <memory>

#include "platform/windows/command_window_runner.h"

namespace gc::platform {
namespace {

class WindowsCommandWindowService final : public CommandWindowService {
public:
  // 统一 wxWidgets 界面用轮询取回终态，因此不绑定 HWND 通知窗口。
  // 命令窗口本身、说明书的核对与结果文件判定都沿用既有实现。
  WindowsCommandWindowService() { runner_.Startup(nullptr); }

  void SweepStaleOperationDirectories() override { runner_.SweepStaleOperationDirectories(); }

  void Shutdown() override { runner_.Shutdown(); }

  [[nodiscard]] bool Start(const git::CommandWindowOperation& operation, uint64_t* outOperationId,
                           CommandWindowResult* failure) override {
    return runner_.Start(operation, outOperationId, failure);
  }

  [[nodiscard]] bool TakeResult(uint64_t operationId, CommandWindowResult* out) const override {
    return runner_.TakeResult(operationId, out);
  }

  [[nodiscard]] bool DescribeOperation(uint64_t operationId, std::wstring* status,
                                       CommandWindowResult* result) const override {
    return runner_.DescribeOperation(operationId, status, result);
  }

  [[nodiscard]] std::wstring OperationDirectory(uint64_t operationId) const override {
    return runner_.OperationDirectory(operationId);
  }

  [[nodiscard]] size_t ActiveCount() const override { return runner_.ActiveCount(); }

private:
  CommandWindowRunner runner_;
};

}  // namespace

std::unique_ptr<CommandWindowService> MakeCommandWindowService() {
  return std::make_unique<WindowsCommandWindowService>();
}

}  // namespace gc::platform
