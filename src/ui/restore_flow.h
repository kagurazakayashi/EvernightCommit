#pragma once

#include <cstdint>
#include <string>

#include "app/task_coordinator.h"  // RepositoryBinding：这一步等的还是不是绑定时那个仓库
#include "git/restore_plan.h"
#include "platform/windows/restore_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「按操作历史里的线索恢复一次本地引用移动」的操作控制器。它是历史功能里唯一会真的改动仓库的
// 入口，而且只服务于「引用级可回退」的记录（HistoryRestoreKind::refMove）：
//   选中一条记录 → 准入判定 → 后台只读重新预检（分支现值、引用自身形态与占用、有没有流程停着）
//   → 判读成恢复方案 → 只有 feasible 才弹强制确认 → 点头之后把**同一组事实**的复核交给后台
//   → 全部对得上才在命令窗口里发那条带预期旧值的 git update-ref → 终态交回本控制器结案。
//
// 它绝不因为「记录当年写着可恢复」就直接执行旧命令：每次都要重新预检现状并让用户确认。
// 不可行（分支已移动/删除、对象不可达、引用形态或占用不能放行、有流程停着、读不回来）时
// 只把话说清楚，不生成任何命令。涉及远端历史的恢复（已推送）不走这里。
//
// 与撤回一样，执行前复核走后台（GUI 线程绝不同步等子进程）：没回来就不发命令。
//
// 生命周期：这条链上的每个非终态阶段都占着自己的流程状态（预检 / 复核 / 命令窗口里那条命令），
// 由主窗口的准入判定与其它六个流程、子模块导航互斥（见 app/operation_gate）。失效判定绑的
// 不是路径字符串，而是「工作区根 + 绝对 Git 目录 + git.exe + 协调器代次」这一组身份：
// 切走再切回同一个路径、重新识别仓库、换 Git 程序都会让它对不上，从而作废这一份确认。
class RestoreFlow {
public:
  enum class Stage {
    none,
    preConfirmProbe,      // 选中记录之后的只读预检在跑
    recheckAfterConfirm,  // 用户点头之后、发命令之前，同一组事实的后台复核在跑
    commandRunning,       // 命令窗口里那条 update-ref 在跑（还没拿到终态）
  };

  // 入口。调用前主窗口已通过准入判定。
  void Start(OperationHost& host, const OperationContext& ctx, git::RestoreClues clues);

  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);
  void OnRecheckCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 命令窗口那条恢复的终态（成功/失败都只报告，不自动重试、不自动回退）。
  // conclusion 是主窗口已经拼好的那半句结论。
  void OnCommandSettled(OperationHost& host, bool succeeded, std::wstring_view conclusion);

  // 完成通知丢失（主窗口的轮询发现执行器已经不记得这次操作）：清掉等待状态。
  // 「没拿到终态」既不是成功也不是失败，那句未知由主窗口按统一措辞写进状态栏。
  void Forget() noexcept;

  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  void BeginStop();
  void JoinWorkers();

private:
  // 这一轮绑定的仓库身份（判定在 app::RepositoryBinding，纯逻辑可测）：代次 + 工作区根 +
  // 绝对 Git 目录 + git.exe 一起比，只比路径看不出「切走又切回同一路径」与「重新识别过」。
  [[nodiscard]] bool BindingHolds(const OperationContext& ctx) const;

  void Abandon(OperationHost& host, std::wstring_view reason);
  // 预检回来：判读成恢复方案；不可行就解释清楚，feasible 才弹强制确认并转入后台复核。
  void ConfirmAndPlan(OperationHost& host, const OperationContext& ctx,
                      const git::RestorePreflightQueries& queries);
  // 复核通过后：在命令窗口里启动这条已核对的恢复方案（update-ref 带预期旧值）。
  void Launch(OperationHost& host, const OperationContext& ctx);
  void ResetState() noexcept;

  platform::RestoreProbeWorker probeWorker_;
  platform::RestoreRecheckWorker recheckWorker_;
  Stage stage_ = Stage::none;
  app::RepositoryBinding binding_;
  git::RestoreClues clues_;
  git::RestorePlan plan_;
};

}  // namespace gc::ui
