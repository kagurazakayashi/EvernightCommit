#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "git/command_window.h"
#include "git/first_push_plan.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「首次推送之后的上游写入」这一段：两条 `git config` 各自一个命令窗口、各自一个退出码。
// 单独成类的理由不是整齐：这一段此前出过「参数已经生成却没传进执行器」的缺陷（界面写着
// 跑 git config，实际发给 Git 的是空参数），而那段接线埋在推送控制器的几百行里，没有任何
// 一层能断言「宿主真的收到了那两份参数」。这里把「发第几条、发给哪个仓库、带哪些参数、
// 前一条失败时绝不发后一条」收成一个可被假宿主观察的单元。
//
// 不变量（与 AGENTS 的首次推送条款一致）：
//   * 只有用户在向导里选了「设置上游」、且推送那一步报告成功，才会走到这里（由调用方保证）；
//   * 参数原样取自已审查的 UpstreamWriteStep.arguments，绝不从界面此刻的状态重新拼；
//   * 工作目录始终是这次推送绑定的那个仓库根；界面中途换仓库时剩下的步骤作废，
//     已写入的一条不自动回退、不自动重发；
//   * 「推送成功」「第一条写成」「第二条写成」是三件分开报告的事；
//   * 启动失败 / 非零退出 / 通知丢失（结果未知）各自成一句，不合并成「推送并设置上游成功」。
class UpstreamWriteFlow {
public:
  struct Plan {
    std::vector<git::UpstreamWriteStep> steps;
    std::wstring branchName;               // 只用于范围说明里的「本地分支」字样
    std::wstring gitExecutable;
    std::wstring repositoryDirectory;      // 这次推送绑定的仓库根（不是界面此刻的根）
    std::wstring pushConclusion;           // 推送那一步的结论文字，逐段引用
  };

  // 开始逐条写入。第一步当场发出；一条都没能启动时返回 false（原因已写进状态栏）。
  // 返回 true 表示「至少发出去了一条，后面还要等终态」。
  bool Begin(OperationHost& host, const OperationContext& ctx, Plan plan);

  // 某一步的终态。step 从 1 开始；succeeded 是那条命令自己的退出码判定；
  // conclusion 是界面已经拼好的那句结论。返回 true = 还有下一步在等终态，false = 这一段已结束。
  bool OnStepSettled(OperationHost& host, const OperationContext& ctx, int step, bool succeeded,
                     std::wstring_view conclusion);

  // 完成通知丢失时的静默结案：调用方（主窗口的轮询）已经把「结果未知」那句话写进状态栏，
  // 这里只负责清掉自己的等待状态，绝不自动重发那几条配置命令。
  void Forget() noexcept { Reset(); }

  [[nodiscard]] bool Active() const noexcept { return waitingStep_ != 0; }
  [[nodiscard]] const Plan& plan() const noexcept { return plan_; }

private:
  // 发出第 index 条（0 起）。参数缺失时在这里就拒绝，绝不让裸 git 通过接线层。
  void LaunchStep(OperationHost& host, const OperationContext& ctx, size_t index);
  // 结束这一段：清状态并回到「没有写在等」。
  void Reset() noexcept;

  Plan plan_;
  // 正在等哪一条的终态（1 起；0 = 没有在等）。
  size_t waitingStep_ = 0;
};

}  // namespace gc::ui
