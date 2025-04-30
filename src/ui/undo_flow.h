#pragma once

#include <cstdint>
#include <string>

#include "git/commit_plan.h"
#include "git/undo_commit_plan.h"
#include "platform/windows/undo_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「撤回最近提交」操作控制器：点击 → 后台只读预检 → 判读成方案 → 确认框（有风险时走
// 强制确认）→ 点头之后把 HEAD/分支的复核也交给后台 → 对得上才启动命令窗口。
// 中间状态与读回的事实全部由本类保管；它触碰界面的唯一途径是 OperationHost 与
// 每次通知分派时随行的只读快照（OperationContext），不会回头读窗口的可变状态。
//
// 与创建提交/拉取/推送不同，这里的「执行前复核」原本是 GUI 线程上的同步子进程调用：
// 确认框点头之后窗口可能长时间无响应。现在它走 UndoHeadRecheckWorker：
// 没回来就不发命令；换仓库后迟到的复核按「不是当前阶段等待的那一次」作废。
// 取消这一份后台读取不会取消用户已经启动的 Git 写操作——命令窗口那一步在复核通过之后才开始。
class UndoFlow {
public:
  // 一次「撤回」走到哪一步。确认框/风险框是模态的，弹出时仍停在原阶段，不需要额外阶段。
  enum class Stage {
    none,                 // 没有在流程中
    preConfirmProbe,      // 点击之后的只读预检在跑
    recheckAfterConfirm,  // 用户点头之后、发命令之前，HEAD/分支的后台复核在跑
  };

  // 入口。调用前主窗口已通过准入判定；ctx 是点击瞬间的快照，摘要留档也取自它。
  void Start(OperationHost& host, const OperationContext& ctx);

  // 预检通知（序号由后台控制器作废迟到结果；仓库换掉的判定在这里）。
  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 执行前复核通知：一致才发命令，不一致或没读回来都放弃这一次并请求重读。
  void OnRecheckCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 还有没有一次撤回在走它自己的只读流程（其它写操作的准入判定据此拒绝并发）。
  // 注意：换仓库时这一位不会立刻落下——预检结果回来后按「不是当前仓库的预检」作废并说明，
  // 这与既有界面一致（宁可让旧查询有明确的收场，也不悄悄丢掉一个还在等的阶段）。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  // 窗口关闭：两阶段收尾（先全体喊停，再逐个 Join）。
  void BeginStop();
  void JoinWorkers();

private:
  // 放弃这次撤回（预检失败、用户取消、复核不过）：原因写进状态栏，不打开命令窗口。
  void Abandon(OperationHost& host, std::wstring_view reason);
  // 预检回来：判读成方案并弹确认框；点头后转入后台复核。
  void ConfirmAndPlan(OperationHost& host, const OperationContext& ctx,
                      const git::UndoPreflightFacts& facts);
  // 复核通过后：在命令窗口里启动这条已核对的撤回方案。
  void Launch(OperationHost& host, const OperationContext& ctx, const git::UndoCommitPlan& plan,
              const git::UndoPreflightFacts& facts);

  platform::UndoProbeWorker probeWorker_;
  platform::UndoHeadRecheckWorker recheckWorker_;
  Stage stage_ = Stage::none;
  // 点击瞬间界面显示的摘要：预检回来后与它对比，不一致时确认框必须说明以刚读回的为准。
  git::CapturedSnapshot captured_;
  // 预检回来后的事实与判读成的方案：复核比对与启动都只认这两份，不再回读界面。
  git::UndoPreflightFacts preflightFacts_;
  git::UndoCommitPlan plan_;
  // 复核跑在哪个仓库上：回来时界面已换掉的，作废（复核任务本身按这份目录执行）。
  std::wstring recheckTargetRoot_;
};

}  // namespace gc::ui
