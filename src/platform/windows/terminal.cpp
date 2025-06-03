#include "platform/terminal.h"

#include <string>
#include <utility>
#include <vector>

#include "platform/terminal_adapters.h"

namespace gc::platform {

std::vector<TerminalCandidate> DiscoverTerminalCandidates() {
  // Windows 没有“终端模拟器参数”这一层：命令窗口辅助进程自己 AllocConsole。
  // 这里仍把这条形态登记成候选，让统一的终端选择接口在 Windows 上也有明确答案。
  TerminalCandidate candidate;
  candidate.kind = TerminalAdapterKind::windowsConsole;
  candidate.programPath.clear();
  candidate.discoveredAs = L"windows-console";
  return {candidate};
}

TerminalSelection DiscoverAndSelectTerminal(const TerminalPreferences& preferences) {
  TerminalSelection selection;
  selection.candidates = DiscoverTerminalCandidates();
  if (!preferences.preferredProgramPath.empty() ||
      (preferences.preferredKind != TerminalAdapterKind::unknown &&
       preferences.preferredKind != TerminalAdapterKind::windowsConsole)) {
    selection.failureReason =
        L"Windows 当前只支持由命令窗口辅助进程自建控制台的终端形态。";
    selection.guidance =
        L"请在设置中使用“Windows 控制台”这一项；本程序不会把交互式 Git 改成隐藏执行。";
    return selection;
  }
  selection.usable = true;
  selection.selected = selection.candidates.front();
  return selection;
}

TerminalLaunchResult LaunchTerminal(const TerminalLaunchPlan& plan) {
  TerminalLaunchResult result;
  result.kind = plan.kind;
  // Windows 的实际启动入口是 CommandWindowRunner：它需要 Unicode 环境块、工作目录与
  // CreateProcessW 的显式 lpApplicationName。这里不另造一条缺少这些约束的启动路径。
  result.failureReason =
      L"Windows 终端由 CommandWindowRunner 直接启动辅助进程；统一终端接口不单独启动控制台。";
  return result;
}

}  // namespace gc::platform
