#pragma once

#include <cstdint>
#include <string>

#include "git/pull_plan.h"
#include "platform/windows/pull_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「pull」操作控制器。pull 分两步，两步都在命令窗口里看得见，中间夹只读预检：
//   获取（fetch）→ 重读事实并判出关系/风险/策略 → 点头 → 执行前复核 → 整合（merge / rebase）。
// 整合失败不再是「当场同步读现场」：现场读取（工作流痕迹 + 未合并条目 + HEAD 位置）
// 走后台任务，读回来之后才把那句结论交还给界面——GUI 线程不在终态之后被子进程拖住。
class PullFlow {
public:
  // pull 进行到哪一步。三个阶段共用同一个后台预检器，靠这个标记分辨「回来的那份事实给谁用」：
  //   fetchProbe     —— 抓取前的阶段一只读预检在跑（分支/HEAD/上游/策略配置/现状）；
  //   fetching       —— 命令窗口里的那次获取在跑（网络操作，用户看得见）；
  //   integrateProbe —— 抓取成功后的阶段二预检在跑（关系/带入文件/冲突预演）；
  //   recheckProbe   —— 用户点头之后的执行前复核在跑（把阶段一那套查询原样重发一遍）；
  //   integrating    —— 命令窗口里的那次整合在跑；
  //   aftermathReport—— 整合未成功：正在后台把「卡在什么现场」读回来，读回来才结案的。
  // 确认框/选择框是模态的，不需要额外的阶段：弹出时界面仍停在对应的预检阶段上。
  enum class Stage {
    none,
    fetchProbe,
    fetching,
    integrateProbe,
    recheckProbe,
    integrating,
    aftermathReport,
  };

  // 入口。调用前主窗口已通过准入判定。
  void Start(OperationHost& host, const OperationContext& ctx);

  // 预检回来，按阶段分派到三条路之一（迟到或换仓库的结果一律作废）。
  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 抓取的操作终态回来：成功才继续做阶段二预检；失败就停在这里（不自动重试、不改配置）。
  void OnFetchSettled(OperationHost& host, const OperationContext& ctx, bool fetchSucceeded);

  // 整合命令的失败终态：界面先把「命令窗口那头说了什么」的结论底稿交进来，
  // 本控制器在后台把现场读回来后补完结论、弹说明框，再请求重读并结案。
  void BeginIntegrationFailedReport(OperationHost& host, const OperationContext& ctx,
                                    std::wstring baseConclusion, git::CommandCompletion completion,
                                    long exitCode, std::wstring environmentNotice);

  // 现场读取回来的通知：补完的结论交还界面（含说明框与随后的重读请求）。
  // 迟到或重复的通知按序号与阶段作废，不会把同一份现场消费两次。
  void OnAftermathCompleted(OperationHost& host, uint64_t completionSerial);

  // 有没有一次 pull 在走流程（含两步命令窗口与失败现场读取）。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  // 命令完成通知丢失/窗口收尾时的结案：界面不能永远停在「等待某一步」上。
  void AbandonFlow();

  // 窗口关闭：两阶段收尾。
  void BeginStop();
  void JoinWorkers();

private:
  // 阶段一：给出「本地分支 ← 远端分支」的确认框，点头才在命令窗口里 fetch。
  void HandleFetchProbe(OperationHost& host, const OperationContext& ctx,
                        const platform::PullProbeOutcome& outcome);
  // 阶段二：判关系与风险 → 需要时让用户选合并/变基 → 给带风险清单的确认框。
  void HandleIntegrateProbe(OperationHost& host, const OperationContext& ctx,
                            const platform::PullProbeOutcome& outcome);
  // 用给定的策略选择合成方案，并按方案的状态走「说明 / 选择框 / 确认框」三条路之一。
  void ComposeAndConfirmIntegrate(OperationHost& host, const OperationContext& ctx,
                                  const platform::PullProbeOutcome& outcome,
                                  git::PullStrategyChoice choice);
  // 确认框点头之后：在后台把预检那套只读查询重发一遍，比对两回事实——一致才启动整合命令。
  void RequestExecutionRecheck(OperationHost& host, const OperationContext& ctx);
  void HandleRecheck(OperationHost& host, const OperationContext& ctx,
                     const platform::PullProbeOutcome& outcome);
  // 在命令窗口里启动一条确定的整合方案（复核已通过）。
  void LaunchIntegrate(OperationHost& host, const OperationContext& ctx,
                       const git::PullIntegratePlan& plan);
  // 放弃这次 pull（预检失败、用户取消、复核不过）：原因写进状态栏，必要时说明 fetch 的遗留影响。
  void Abandon(OperationHost& host, std::wstring_view reason);

  platform::PullProbeWorker worker_;
  platform::PullAftermathWorker aftermathWorker_;
  Stage stage_ = Stage::none;
  platform::PullProbeOutcome integrateFacts_;  // 确认框与执行前复核共同的比对基准。
  git::PullIntegratePlan plan_;
  // 这一次 pull 是否已经在命令窗口里跑过「获取」：取消整合时要把这一点交代清楚
  // （远端跟踪引用已经被那次抓取更新过，本程序不会、也不该把它退回去）。
  bool fetchAlreadyRan_ = false;

  // 失败现场读取的随行数据：结论底稿、命令窗口的终态与环境告知（读回来后拼完整结论）。
  std::wstring reportBaseNote_;
  git::CommandCompletion reportCompletion_ = git::CommandCompletion::launchFailed;
  long reportExitCode_ = 0;
  std::wstring reportEnvironmentNotice_;
};

}  // namespace gc::ui
