#pragma once

#include <cstdint>
#include <string>

#include "git/push_plan.h"
#include "platform/windows/push_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「推送」操作控制器：点击 → 后台只读预检 → 判读成方案（有风险走强制确认）→ 点头后
// 执行前复核（同一组查询原样重发）→ 命令窗口里的 push → 终态 → 向发布目标逐条只读核实。
// 「确认了哪一份提交、实际去了哪、事后在哪里核实过」必须是同一件事：方案与预检事实都
// 由本类保管，核实请求绑定预检时那个仓库的根，而不是界面此刻显示的根。
class PushFlow {
public:
  // 一次推送走到哪一步。预检与执行前复核共用 pushWorker_，靠这个标记分辨
  // 「回来的那份事实给谁用」；核实走 verifyWorker_，与它们互不干扰。
  enum class Stage {
    none,
    probe,        // 点击之后的只读预检在跑
    recheckProbe, // 点头之后的执行前复核在跑
    pushing,      // 命令窗口里的那条 push 在跑
    verifying,    // 向发布目标核对在跑（这一步不影响仓库，只影响结论措辞）
  };

  // 入口。调用前主窗口已通过准入判定。
  void Start(OperationHost& host, const OperationContext& ctx);

  // 预检/复核通知（按阶段分派；迟到或换仓库的结果一律作废）。
  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 命令窗口里那条 push 的终态：无论成败都发起对发布目标的核实——
  // 命令报成功时要拿它当证据，报失败时它正是「远端到底动没动」的唯一凭据。
  // conclusion 是界面已经拼到「命令窗口那头说了什么」的那半句结论。
  void BeginVerification(OperationHost& host, const OperationContext& ctx, bool commandSucceeded,
                         std::wstring_view conclusion);

  // 核实结果回来：结论写进状态栏；与命令窗口的结论不符时另开一个说明框，把逐目标证据摆出来。
  void OnVerifyCompleted(OperationHost& host, uint64_t completionSerial);

  // 有没有一次推送在走流程（含命令窗口步与核实步）。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  // 命令完成通知丢失时的结案：否则界面永远停在「等那一次推送」上，把下一次点击锁死。
  // 核实本来就只在终态之后才发起，走到这里说明它没机会跑——结论只能以命令窗口那头的输出为准。
  void AbandonFlow();

  // 窗口关闭：两阶段收尾（喊停后多目标核实不再逐个发起 ls-remote）。
  void BeginStop();
  void JoinWorkers();

private:
  // 预检回来：判读成方案。blocked 只说明原因；ready 给确认框（有风险时要求明确点「仍要推送」）。
  void HandleProbe(OperationHost& host, const OperationContext& ctx,
                   const platform::PushProbeOutcome& outcome);
  // 确认框点头之后：把预检那套只读查询原样重发一遍，比对两回事实——一致才启动那条命令。
  void RequestExecutionRecheck(OperationHost& host, const OperationContext& ctx);
  // 复核回来：一致才发命令；不一致就作废并请求重读。
  void HandleRecheck(OperationHost& host, const OperationContext& ctx,
                     const platform::PushProbeOutcome& outcome);
  // 在命令窗口里启动一条确定的推送方案（复核已通过）。
  void Launch(OperationHost& host, const OperationContext& ctx, const git::PushPlan& plan);
  // 放弃这次推送（预检失败、用户取消、复核不过）：原因写进状态栏，不打开命令窗口。
  void Abandon(OperationHost& host, std::wstring_view reason);

  platform::PushProbeWorker worker_;
  platform::PushVerifyWorker verifyWorker_;
  Stage stage_ = Stage::none;
  platform::PushProbeOutcome preflight_;  // 预检那一份：复核要和它逐条比对。
  git::PushPlan plan_;
};

}  // namespace gc::ui
