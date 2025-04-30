#pragma once

#include <cstdint>
#include <string>

#include "git/commit_plan.h"
#include "git/commit_message.h"
#include "platform/windows/commit_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「创建提交」操作控制器。一次「创建提交」要走三段只读动作，全程不碰命令窗口，
// 直到最后一步才发命令：
//   点击 → 后台重读工作区状态（列表）→ 后台问一组身份事实（预检）→ 确认框
//   → 点头 → 同一组查询原样重发一遍（执行前复核）→ 对得上才启动命令窗口。
// 确认范围与实际索引的一致就靠这两趟同构查询：确认框摆的是预检那一份，
// 发命令前逐条复核的也是那一份；启动时的工作目录与 git.exe 一律取自方案本身。
// 表单内容只通过 CommitOperationHost 的快照接口取得——控制器不持有任何控件。
class CommitFlow {
public:
  // 一次「创建提交」走到哪一步。重读、预检、复核共用同一个后台预检器与同一个阶段标记，
  // 靠它分辨「回来的那份事实给谁用」，也让其它写操作的入口知道这里正占着这份索引：
  //   preConfirmRead  —— 点击之后，后台重读工作区状态（确认框要摆刚刚读回的列表）；
  //   preConfirmProbe —— 列表回来了，后台问那一组身份事实（确认框之前）；
  //   executionRecheck—— 用户点头之后、发命令之前，同一组查询原样重发一遍。
  // 确认框是模态的，弹出时界面仍停在 preConfirmProbe 上，不需要额外的阶段。
  enum class Stage {
    none,
    preConfirmRead,
    preConfirmProbe,
    executionRecheck,
  };

  // 点击入口。调用前主窗口已通过准入判定；ctx 与 model 都是点击瞬间的只读快照。
  void Start(CommitOperationHost& host, const OperationContext& ctx,
             const git::WorkspaceModel& model);

  // 一次工作区读取的终态回来：若这一次读取属于「创建提交」的核对（preConfirmRead），
  // 认领并推进；返回值 = 是否认领（未认领时界面按普通刷新继续收尾）。
  bool OnWorkspaceLoaded(CommitOperationHost& host, const OperationContext& ctx, bool loadSucceeded);

  // 预检/复核回来：按 stage 分派，迟到的或换了仓库的结果一律作废。
  void OnProbeCompleted(CommitOperationHost& host, const OperationContext& ctx,
                        const git::WorkspaceModel& model, uint64_t completionSerial);

  // 「创建提交」的命令窗口终态：成功才走「逐栏比对后再清」的收尾，失败时表单一个字都不动。
  void OnCommandSettled(CommitOperationHost& host, bool succeeded,
                        const git::CommitFormData& committedForm);

  // 还有没有一次「创建提交」在走它的只读流程（重读/预检/复核）：其它写操作入口据此拒绝，
  // 免得两条流程同时改同一份索引。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  // 这次尝试到此为止：清掉阶段标记、点击瞬间的摘要、预检事实与方案。
  // 信息文件归本方案保管时（还没进命令窗口）一并回收；命令已经启动的那一路必须传 false，
  // 因为那份文件此刻是 ActiveOperation 的财产，Git 可能还在读。
  void ReleaseAttempt(bool reclaimMessageFile);

  // 窗口关闭：两阶段收尾。
  void BeginStop();
  void JoinWorkers();

private:
  // 列表读回来后发起身份预检（确认框之前的那一趟）。
  void RequestPreflightProbe(CommitOperationHost& host, const OperationContext& ctx);
  // 预检回来：合成方案（确认框摆的就是这份方案），点头后把方案与预检事实一起留下，
  // 再做执行前复核。
  void HandlePreflightProbe(CommitOperationHost& host, const OperationContext& ctx,
                            const git::WorkspaceModel& model,
                            const platform::CommitProbeOutcome& outcome);
  // 点头之后、发命令之前：把预检那组查询原样重发一遍。
  void RequestExecutionRecheck(CommitOperationHost& host, const OperationContext& ctx);
  // 复核回来：逐条比对「确认框上那一份」与「刚刚读回的这一份」，任何一条不符就不发命令。
  void HandleRecheckProbe(CommitOperationHost& host, const OperationContext& ctx,
                          const platform::CommitProbeOutcome& outcome);
  // 在命令窗口里启动一条已复核通过的提交方案（工作目录与 git.exe 都取自方案绑定的身份）。
  void Launch(CommitOperationHost& host, const git::CommitPlan& plan,
              const git::CommitFormData& committedForm);
  // 放弃这次提交（核对失败、用户取消、复核不过）：删掉刚写的信息文件（还没交给 Git 的那份），
  // 把原因写进状态栏，表单一个字都不动。
  void Abandon(CommitOperationHost& host, std::wstring_view reason);

  platform::CommitProbeWorker worker_;
  Stage stage_ = Stage::none;
  // 点击瞬间界面显示的摘要（确认框要交代它以刚读回的为准）。
  git::CapturedSnapshot captured_;
  // preflight/plan 只在走到确认框之后才有内容——复核比对的是「预检那一份」与「点头后重读
  // 的那一份」，两者缺一就不发命令。plan.identity 里带着这次的信息文件路径：谁持有它，
  // 谁负责回收。
  platform::CommitProbeOutcome preflight_;
  git::CommitPlan plan_;
  // 确认框点头时那一份表单内容（标题/描述/合作者）：命令成功后的收尾按它逐栏比对，
  // 期间用户另写的草稿不在清理范围内。
  git::CommitFormData confirmedForm_;
  // 这次尝试写好的提交信息文件。还没交给命令窗口时由这里保管，作废时回收；
  // 交给命令窗口之后所有权转给 ActiveOperation，那条路上绝不能再删（Git 可能还在读）。
  std::wstring messageFile_;
};

}  // namespace gc::ui
