#pragma once

#include <cstdint>

#include "git/fetch_plan.h"
#include "platform/windows/fetch_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「fetch」操作控制器：点击 → 后台只读目标预检 → 判读成方案。目标唯一给确认框；
// 定不下来列既有远端供单选；没有远端/查询失败如实说明——不猜 origin、不创建远端、不改配置。
// 抓取本身在命令窗口里跑（单槽由主窗口的执行器裁决），终态结论见 app/operation_conclusions。
class FetchFlow {
public:
  // 入口。调用前主窗口已通过准入判定（含「已有一次预检在跑」的拒绝）。
  void Start(OperationHost& host, const OperationContext& ctx);

  // 目标预检通知：换仓库后迟到的旧结果作废；选择框/确认框都是模态的，弹出时不算额外阶段。
  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 有没有一次 fetch 目标预检在跑（其它写操作的准入判定据此拒绝）。
  [[nodiscard]] bool Active() const noexcept { return probing_; }

  // 窗口关闭：两阶段收尾。
  void BeginStop();
  void JoinWorkers();

private:
  // 在命令窗口里启动一条确定的 fetch 方案（确认框已由调用方点头）。
  void Launch(OperationHost& host, const OperationContext& ctx, const git::FetchPlan& plan);

  platform::FetchProbeWorker worker_;
  bool probing_ = false;
};

}  // namespace gc::ui
