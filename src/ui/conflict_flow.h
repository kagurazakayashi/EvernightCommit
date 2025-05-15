#pragma once

#include <cstdint>
#include <string>

#include "git/command_window.h"
#include "git/conflict_state.h"
#include "platform/windows/conflict_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「合并/变基等流程已经停在冲突或暂停状态」的操作控制器：三个入口共用一条链路。
//
//   查看   → 后台只读现场读取 → 把状态原样写进说明框（不弹确认、不发命令）
//   继续   → 后台只读现场读取 → 判读成方案 → 确认框（这一步由 Git 建立提交）→ 后台复核 → 命令窗口
//   中止   → 后台只读现场读取 → 判读成方案 → 风险确认框（会改动工作区）→ 后台复核 → 命令窗口
//
// 与「撤回最近提交」同一套规矩：中间状态与读回的事实全部由本类保管，触碰界面的唯一途径是
// OperationHost 与每次通知分派时随行的只读快照（OperationContext），绝不回头读窗口的可变状态。
// 因此「预检回来时仓库早已换掉」一定能被 ctx.detection.root 比对出来并作废。
//
// 三条硬性边界（本任务的要求）：
//   * 没有流程痕迹时不生成任何 --abort；索引里还有未合并条目时不生成任何 --continue——
//     两句都由 git/conflict_state 判读后给出原因，界面不另判一套；
//   * 绝不自动恢复：命令失败、结果未知、复核不一致时只把现场读回来如实说，
//     不 reset --hard、不 clean、不 stash、不删 index.lock、不选 ours/theirs，
//     也不动用户写在提交表单里的任何字；
//   * 复核走后台可作废 worker（GUI 线程绝不同步等子进程），取消这一份后台读取
//     绝不会取消用户已经启动的 Git 写操作。
class ConflictFlow {
public:
  enum class Entry {
    view = 0,       // 只看现场
    continueFlow,  // 把 --continue 交出去
    abortFlow,     // 把 --abort 交出去
  };

  // 一次走到哪一步。确认框是模态的，弹出时仍停在原阶段，不需要额外阶段。
  enum class Stage {
    none,
    viewProbe,        // 查看的现场读取在跑
    continueProbe,    // 继续的现场读取在跑
    abortProbe,       // 中止的现场读取在跑
    recheck,          // 用户点头之后、发命令之前的复核在跑
    aftermathReport,  // 命令没做成之后，把现场读回来补完那句结论
  };

  // 入口。调用前主窗口已通过准入判定；ctx 是点击瞬间的快照。
  void Start(OperationHost& host, const OperationContext& ctx, Entry entry);

  // 预检（查看/继续/中止三条路共用这一个通知，按 stage_ 分流）。
  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);
  // 执行前复核。
  void OnRecheckCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);
  // 命令没做成之后的现场读取（不带快照：随行数据都在控制器自己保管的那几份里）。
  void OnAftermathCompleted(OperationHost& host, uint64_t completionSerial);

  // 命令窗口那一步的终态交回这里：成功只补一句结论；失败则发起现场读取。
  // 返回值只告诉主窗口「这一条结论已经由本控制器接管」，主窗口据此不再自行落账。
  void BeginFailedReport(OperationHost& host, const OperationContext& ctx, Entry entry,
                         std::wstring baseConclusion, git::CommandCompletion completion, long exitCode,
                         std::wstring environmentNotice);

  // 还有没有一次冲突处理在走它自己的只读流程（其它写操作与导航的准入判定据此拒绝）。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  // 窗口关闭：两阶段收尾（先全体喊停，再逐个 Join）。
  void BeginStop();
  void JoinWorkers();

private:
  void Abandon(OperationHost& host, std::wstring_view reason);
  void RequestProbe(OperationHost& host, const OperationContext& ctx, Stage stage, Entry entry);
  // 预检回来：查看直接展示；继续/中止判读成方案并弹确认框，点头后转入后台复核。
  void HandleProbe(OperationHost& host, const OperationContext& ctx, const git::ConflictStateFacts& facts);
  void RequestRecheck(OperationHost& host, const OperationContext& ctx);
  void Launch(OperationHost& host, const OperationContext& ctx);
  [[nodiscard]] std::wstring DescribeAftermath(const git::ConflictStateFacts& latest) const;

  platform::ConflictProbeWorker probeWorker_;
  platform::ConflictProbeWorker recheckWorker_;
  platform::ConflictProbeWorker aftermathWorker_;

  Stage stage_ = Stage::none;
  Entry entry_ = Entry::view;
  // 预检读回来的现场与判读成的方案：复核比对与启动只认这两份，不再回读界面。
  git::ConflictStateFacts preflightFacts_;
  git::ConflictOperationPlan plan_;
  // 复核跑在哪个仓库上：回来时界面已换掉的，作废。
  std::wstring recheckTargetRoot_;
  // 现场读取补完结论时的随行数据（与 pull 整合失败的同一套做法）。
  std::wstring reportBaseNote_;
  std::wstring reportCommandLabel_;
  Entry reportEntry_ = Entry::view;
  git::CommandCompletion reportCompletion_ = git::CommandCompletion::launchFailed;
  long reportExitCode_ = 0;
  std::wstring reportEnvironmentNotice_;
  std::wstring reportRepositoryRoot_;
};

}  // namespace gc::ui
