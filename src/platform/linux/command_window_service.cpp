#include "platform/command_window_service.h"

#include <memory>
#include <utility>

#include "platform/linux/command_window_runner.h"
#include "platform/terminal.h"

namespace gc::platform {
namespace {

class LinuxCommandWindowService final : public CommandWindowService {
public:
  LinuxCommandWindowService() : runner_(nullptr, 0) {}

  void SweepStaleOperationDirectories() override { runner_.SweepStaleOperationDirectories(); }

  void Shutdown() override { runner_.Shutdown(); }

  [[nodiscard]] bool Start(const git::CommandWindowOperation& operation, uint64_t* outOperationId,
                           CommandWindowResult* failure) override {
    return runner_.Start(operation, preferences_, outOperationId, failure);
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

  [[nodiscard]] bool SetTerminalPreferences(const TerminalPreferences& preferences,
                                            std::wstring* refusal) override {
    static_cast<void>(refusal);
    preferences_ = preferences;
    return true;
  }

  [[nodiscard]] TerminalPreferences GetTerminalPreferences() const override {
    return preferences_;
  }

  [[nodiscard]] TerminalSelection DescribeTerminalSelection() override {
    return DiscoverAndSelectTerminal(preferences_);
  }

private:
  TerminalPreferences preferences_;
  LinuxCommandWindowRunner runner_;
};

}  // namespace

std::unique_ptr<CommandWindowService> MakeCommandWindowService() {
  return std::make_unique<LinuxCommandWindowService>();
}

}  // namespace gc::platform
