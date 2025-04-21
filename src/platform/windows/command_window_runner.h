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

// ---------- 时间与陈旧判定（纯算术，可被单元测试直接核对参考值） ----------
//
// FILETIME 与 GetSystemTimeAsFileTime 都以“100 纳秒”为一个刻度：一秒是 10'000'000 个刻度。
// 旧实现把“60 分钟”写成 60 * 60 * 10000，在这个单位下其实只有 3.6 秒，于是任何一次
// 正常的长操作（凭据输入等待、大仓库 fetch/push）都可能被下一次启动的清扫当成死目录。
// 现在阈值只由下面这组具名换算表达，不再出现手工乘法。
inline constexpr std::uint64_t kFileTimeTicksPerSecond = 10'000'000ULL;

[[nodiscard]] constexpr std::uint64_t FileTimeTicksFromSeconds(std::uint64_t seconds) noexcept {
  return seconds * kFileTimeTicksPerSecond;
}

[[nodiscard]] constexpr std::uint64_t FileTimeTicksFromMinutes(std::uint64_t minutes) noexcept {
  return FileTimeTicksFromSeconds(minutes * 60ULL);  // 1 分钟 = 60 秒
}

// 回收阈值：60 分钟，也就是 3600 秒、36'000'000'000 个 100ns 刻度。
inline constexpr std::uint64_t kStaleOperationDirectoryAgeTicks = FileTimeTicksFromMinutes(60ULL);

// 时间上是否已经“够老”。一律采取保守回答：
// 时间戳缺失（0）、时间戳等于或晚于当前时间（时钟回拨、未来时间戳、别的实例刚写的），
// 或者还没跨过阈值，都返回 false —— 宁可留下残骸等下一次，也绝不少删一个正在用的目录。
[[nodiscard]] constexpr bool IsOperationDirectoryOldEnoughToReclaim(std::uint64_t lastWriteTicks,
                                                                   std::uint64_t nowTicks,
                                                                   std::uint64_t ageTicks) noexcept {
  if (lastWriteTicks == 0 || nowTicks == 0) {
    return false;
  }
  if (lastWriteTicks >= nowTicks) {
    return false;
  }
  return nowTicks - lastWriteTicks >= ageTicks;
}

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
// 操作目录的所有权与生命周期（回收、租约与归属核对）：
//   * 目录名是 GcOp<进程ID>x<随机段>，随机段每次新生成；只有 CreateDirectoryW 真的新建成功
//     才算认领，撞名就换一个猜不到的名字重来，绝不采纳已经存在的目录、也不复用里面任何文件。
//     认领后立刻在目录里独占创建 lease.txt 并持有句柄（见 ClaimOperationDirectory）。
//   * start.txt 与 result.txt 都带着本次操作的随机口令，观察端只认口令相符的那一份；
//     result.txt 由 result.tmp 改名发布，因此不会出现“读到半截”或“把上一次的旧成功当成本次回答”。
//   * 回收要同时满足两件事：时间上确实陈旧（具名阈值，见下面的换算），且没有任何活动持有者
//     —— 逐个以“只读 + 共享 0”探测名单内的文件：执行器的租约、辅助进程握着的开始标记，
//     有一个被查出来就整目录保留。判不下的一律保留并记录（见 PreservedOperationDirectories），
//     且只删名单内的文件名，绝不递归清空目录里来历不明的内容。
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

  // 回收上一次会话留下的操作目录（Startup 调用一次；也供隔离测试直接触发）。
  // 一个目录要同时满足两件事才被回收：
  //   1) 时间上确实陈旧（超过 kStaleOperationDirectoryAgeTicks，且时间戳可信）；
  //   2) 没有任何活动持有者 —— 逐个独占探测本程序自己写出的那几个文件，
  //      只要有一个正被别的句柄持有（执行器的租约、或辅助进程握着的开始标记），就整目录保留。
  // 除此之外还有一条：说明书存在而开始标记不存在时不回收，因为无法排除“辅助进程还没读它”。
  // 回收只逐个删除名单内的文件，目录里只要还有来历不明的内容就删不掉，于是原样保留。
  void SweepStaleOperationDirectories();

  // 最近被回收例程判定为“不能动”的目录及原因（诊断与测试取证；上限若干条，只保留最近的）。
  [[nodiscard]] std::vector<std::wstring> PreservedOperationDirectories() const;

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
  void RecordPreservedDirectory(std::wstring directory, std::wstring reason);
  // 收尾本进程亲手认领、但没有交出去的操作目录（启动前失败、线程创建失败等提前返回）。
  void ReleaseClaimedDirectory(std::wstring_view directory, bool mayStillBeRunning);

  mutable std::mutex mutex_;
  std::condition_variable stopCondition_;
  HWND notifyWindow_ = nullptr;
  bool stopping_ = false;
  std::vector<std::wstring> pendingCleanupDirectories_;  // 命令窗口还占着的操作目录，择机回收
  Impl* impl_ = nullptr;  // 记录表：类型只在 .cpp 定义，构造/析构同样在 .cpp
};

// 原子认领一个本程序独占的操作目录：只有 CreateDirectoryW 真的“新建成功”才算拿到所有权，
// 目录已存在（无论里面有什么）一律拒绝，也绝不复用其中任何文件。认领成功后立刻在同一个目录里
// 独占创建租约文件 lease.txt 并把句柄交给调用方 —— 句柄在，就是这个目录还在本进程进行中的凭据；
// 进程异常退出时由系统关闭它，于是“租约能独占打开”同时是“前主人已经不在了”的证据。
// 还会核对新建出来的确实是个目录而不是重解析点（符号链接/junction 会把写入与清理带到目录之外）。
// token 必须由调用方每次给出新的不可预测值（见 git::IsSafeOperationDirectoryName 的形态），
// 冲突时调用方换名重试；outPath 只在成功时填写。
[[nodiscard]] bool ClaimOperationDirectory(std::wstring_view tempRoot, std::wstring_view token,
                                           UniqueHandle* outLease, std::wstring* outPath,
                                           std::wstring* failureReason);

}  // namespace gc::platform
