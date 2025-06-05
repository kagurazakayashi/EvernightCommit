#include "platform/command_window_service.h"

#include <memory>

#include "platform/terminal.h"
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

  // Windows 的命令窗口固定走控制台辅助入口（AllocConsole），没有可替换的终端模拟器实现：
  // 这里如实拒绝，绝不静默收下偏好造成「设置成功」的假象。
  [[nodiscard]] bool SetTerminalPreferences(const TerminalPreferences& preferences,
                                            std::wstring* refusal) override {
    static_cast<void>(preferences);
    if (refusal != nullptr) {
      *refusal = L"Windows 的命令窗口固定使用控制台辅助入口，本机没有可替换的终端模拟器实现；"
                 L"终端选择只在 Linux 生效。";
    }
    return false;
  }

  [[nodiscard]] TerminalPreferences GetTerminalPreferences() const override {
    return TerminalPreferences{};
  }

  [[nodiscard]] TerminalSelection DescribeTerminalSelection() override {
    return DiscoverAndSelectTerminal(TerminalPreferences{});
  }

private:
  CommandWindowRunner runner_;
};

}  // namespace

std::unique_ptr<CommandWindowService> MakeCommandWindowService() {
  return std::make_unique<WindowsCommandWindowService>();
}

}  // namespace gc::platform
