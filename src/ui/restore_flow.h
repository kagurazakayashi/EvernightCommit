#pragma once

#include <cstdint>
#include <string>

#include "git/restore_plan.h"
#include "platform/windows/restore_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「按操作历史里的线索恢复一次本地引用移动」的操作控制器。它是历史功能里唯一会真的改动仓库的
// 入口，而且只服务于「引用级可回退」的记录（HistoryRestoreKind::refMove）：
//   选中一条记录 → 后台只读重新预检（分支现在指着什么、要挪回去的对象还在不在、有没有流程停着）
//   → 判读成恢复方案 → 只有 feasible 才弹强制确认 → 点头之后把「分支现值」的复核也交给后台
//   → 对得上才在命令窗口里发一条带预期旧值的 git update-ref。
//
// 它绝不因为「记录当年写着可恢复」就直接执行旧命令：每次都要重新预检现状并让用户确认。
// 不可行（分支已移动/删除、对象不可达、有流程停着、读不回来）时只把话说清楚，不生成任何命令。
// 涉及远端历史的恢复（已推送）不走这里——那条路只有「解释 + 复制命令」，没有自动执行。
//
// 与撤回一样，执行前复核走后台（GUI 线程绝不同步等子进程）：没回来就不发命令。
class RestoreFlow {
public:
  enum class Stage {
    none,
    preConfirmProbe,      // 选中记录之后的只读预检在跑
    recheckAfterConfirm,  // 用户点头之后、发命令之前，分支现值的后台复核在跑
  };

  // 入口。主窗口从一条历史记录的恢复线索拷出 clues 传进来（调用前已确认这是一条 refMove 记录）。
  void Start(OperationHost& host, const OperationContext& ctx, git::RestoreClues clues);

  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);
  void OnRecheckCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  void BeginStop();
  void JoinWorkers();

private:
  void Abandon(OperationHost& host, std::wstring_view reason);
  // 预检回来：判读成恢复方案；不可行就解释清楚，feasible 才弹强制确认并转入后台复核。
  void ConfirmAndPlan(OperationHost& host, const OperationContext& ctx,
                      const git::RestorePreflightQueries& queries);
  // 复核通过后：在命令窗口里启动这条已核对的恢复方案（update-ref 带预期旧值）。
  void Launch(OperationHost& host, const OperationContext& ctx);

  platform::RestoreProbeWorker probeWorker_;
  platform::RestoreRecheckWorker recheckWorker_;
  Stage stage_ = Stage::none;
  git::RestoreClues clues_;
  git::RestorePlan plan_;
  std::wstring recheckTargetRoot_;
};

}  // namespace gc::ui
