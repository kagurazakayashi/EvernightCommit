// 共用层边界核对（不参与 Windows 构建，也不链接进 gc_tests）。
//
// 这份文件唯一的用途是让「拆分有没有被改回去」变成一条编译器能回答的问题：
// 只要有人往 src/git、src/app、src/platform 的契约头或九个操作控制器的头文件里
// 重新塞进 <windows.h>、HWND/HFONT 这类原生类型，或者从 ui/operation_host.h 里
// 直接暴露 Win32 对话框参数，在 Linux（或任何没有 Windows SDK 的环境）用下面的命令
// 一编译就会失败：
//
//   g++ -std=c++20 -fsyntax-only -Isrc tests/portable_boundary_check.cpp
//
// 注意它管不到「include 了 platform/windows 目录里的头」这件事：控制器目前仍然直接
// 使用 platform/windows/*_probe.h 的请求与结果类型，那些头本身已经不含任何 Windows 类型，
// 什么时候该把契约提上来见 docs/architecture.md「已知待办差异」第 3 条。
//
// 通过与否都不代表 Linux 版可用：这里的 std::wstring 在两侧宽度不同，
// 行为回归仍然只能在 Windows 上按 docs/testing.md 的分组执行。详见 docs/building.md 第 5 节。
#include "platform/dialogs.h"
#include "platform/git_task_worker.h"
#include "platform/local_time.h"
#include "platform/notification.h"

#include "ui/commit_flow.h"
#include "ui/conflict_flow.h"
#include "ui/fetch_flow.h"
#include "ui/operation_host.h"
#include "ui/pull_flow.h"
#include "ui/push_flow.h"
#include "ui/restore_flow.h"
#include "ui/submodule_flow.h"
#include "ui/undo_flow.h"
#include "ui/upstream_write_flow.h"

#include <type_traits>

namespace {

// 用一个「指向结构体的指针」模拟各平台的原生句柄（Win32 的 HWND__*、wxWidgets 的
// wxEvtHandler*）：它必须能隐式进入 NotificationTarget，而反向解释只允许写在平台实现里。
struct SimulatedNativeWindow;
using SimulatedHandle = SimulatedNativeWindow*;

static_assert(std::is_same_v<gc::platform::NotificationTarget, void*>);

// 后台 worker 的代次规则是共用逻辑：序号作废、两阶段停止、结果按最新一次取回。
struct ProbeRequest {
  std::wstring exePath;
  gc::platform::StopFlag stopFlag;
};
struct ProbeOutcome {
  std::wstring note;
};

void WorkerKeepsGenerationRules(SimulatedHandle window) {
  gc::platform::GitTaskWorker<ProbeRequest, ProbeOutcome> worker;
  worker.Request(window, 1u, ProbeRequest{}, [](const ProbeRequest&) { return ProbeOutcome{}; });
  ProbeOutcome outcome;
  (void)worker.FetchLatest(1, &outcome);
  worker.BeginStop();
  worker.Join();
  static_assert(std::is_same_v<decltype(gc::platform::StopRequested(std::declval<
                                           const gc::platform::StopFlag&>())),
                               bool>);
}

// 窄接口只认平台无关的数据：投递目标、文本内容、对话框请求，没有句柄与消息号。
void HostContractStaysNeutral() {
  gc::ui::OperationContext context;
  static_assert(std::is_same_v<decltype(context.notify), gc::platform::NotificationTarget>);
  static_assert(std::is_same_v<decltype(context.detection), gc::git::RepoDetection>);

  gc::platform::SingleChoiceRequest choice;
  choice.items.push_back({L"label", L"detail"});
  choice.hints.listHeight = 140;

  gc::platform::TextPromptRequest prompt;
  prompt.validate = [](const std::wstring& value) { return value; };
  static_assert(std::is_same_v<decltype(prompt.validate),
                               std::function<std::wstring(const std::wstring&)>>);
}

// 本机时间换算的契约不含原生时间结构，只有墙上时间、UTC 秒与偏移。
void LocalTimeContractIsNeutral() {
  const gc::platform::LocalInstant instant = gc::platform::CurrentLocalInstant();
  static_assert(std::is_same_v<decltype(instant.utcEpochSeconds), long long>);
  static_assert(std::is_same_v<decltype(instant.offsetMinutes), int>);
}

}  // namespace
