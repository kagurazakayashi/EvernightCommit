#include "ui/upstream_write_flow.h"

#include <string>
#include <utility>

#include "git/repository.h"

namespace gc::ui {
namespace {

// 一段说明文字里引用「刚才那一步的结论」时用的形态：不换行、不空。
std::wstring OrNone(std::wstring_view text) {
  return text.empty() ? std::wstring(L"（没有结论原文）") : std::wstring(text);
}

}  // namespace

bool UpstreamWriteFlow::Begin(OperationHost& host, const OperationContext& ctx, Plan plan) {
  Reset();
  if (plan.steps.empty()) {
    host.SetStatus(L"上游设置：没有需要执行的配置命令，因此一条都没有发出。");
    return false;
  }
  // 接线层的自检：参数表是「审查过的那两条 git config」的唯一载体。空参数表在这条链路上
  // 只能是被漏传，绝不能让它变成「界面写着 git config、Git 实际收到裸 git」。
  for (size_t index = 0; index < plan.steps.size(); ++index) {
    if (plan.steps[index].arguments.empty()) {
      host.SetStatus(L"上游设置这一步没有发出：第 " + std::to_wstring(index + 1) +
                     L" 条配置命令的参数表是空的（接线缺陷，不是 Git 拒绝）。"
                     L"没有写任何配置，也不自动重试。请把这条反馈给开发者。");
      return false;
    }
  }
  plan_ = std::move(plan);
  LaunchStep(host, ctx, 0);
  return Active();
}

void UpstreamWriteFlow::LaunchStep(OperationHost& host, const OperationContext& ctx, size_t index) {
  if (index >= plan_.steps.size()) {
    Reset();
    host.SetStatus(L"上游设置：没有需要执行的配置命令。");
    return;
  }
  const git::UpstreamWriteStep& step = plan_.steps[index];
  waitingStep_ = index + 1;

  git::CommandWindowOperation operation;
  operation.operationId = step.operationId;
  operation.displayName = step.displayName;
  operation.gitExecutable = plan_.gitExecutable.empty() ? ctx.gitExecutable : plan_.gitExecutable;
  // 仍然绑在这次推送那个仓库根上：中途界面换了仓库，剩下的配置命令就不该发给另一个仓库。
  operation.repositoryDirectory = plan_.repositoryDirectory;
  // 这一行是 R1 的全部内容：已审查的参数必须原样交给执行器。参数走 Unicode 数组，
  // 不进任何 shell 字符串，所以键名/值里的特殊字符都不需要（也不允许）再做转义。
  operation.arguments = step.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + step.commandLabel + L"（上游设置第 " +
                        std::to_wstring(index + 1) + L"/" + std::to_wstring(plan_.steps.size()) +
                        L" 条），等待 Git 退出码…";
  options.scopeNotice = L"这一步只写这一把配置键：" + step.key + L" = " + step.value +
                        L"。只影响本地分支 " + plan_.branchName +
                        L" 的上游记录，不动别的配置、不动分支与工作区，也不接触远端。";
  options.upstreamWriteStep = static_cast<int>(index + 1);
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"上游设置这一步没能启动：命令窗口未能打开或启动失败（原因见上一行）。"
                   L"远端与本地配置都没有被这次操作改动——那条推送本身的结果以它自己的核实为准。");
    Reset();
  }
}

bool UpstreamWriteFlow::OnStepSettled(OperationHost& host, const OperationContext& ctx, int step,
                                      bool succeeded, std::wstring_view conclusion) {
  if (!Active() || static_cast<size_t>(step) != waitingStep_) {
    return false;  // 迟到的、或不属于这一次操作的结果。
  }
  const size_t settled = waitingStep_;
  const size_t done = settled - 1;

  if (!ctx.repoUsable || !git::PathsEqualFolded(plan_.repositoryDirectory, ctx.detection.root)) {
    host.SetStatus(L"上游设置停在第 " + std::to_wstring(settled) + L" 条：界面上的仓库已经换掉，"
                   L"剩下的配置命令没有发出。已写入的那几条原样留着，本程序不自动回退、不自动重发。\n"
                   L" · 刚才那一步（第 " + std::to_wstring(settled) + L" 条）在命令窗口里的结论：「" +
                   OrNone(conclusion) + L"」\n"
                   L" · 推送那一步的结论：「" + OrNone(plan_.pushConclusion) + L"」");
    Reset();
    return false;
  }

  if (!succeeded) {
    const git::UpstreamWriteStep& failed = plan_.steps[done];
    std::wstring text = L"推送与上游设置要分开说：\n";
    text += L" · 推送那一步：命令窗口的结论是「" + OrNone(plan_.pushConclusion) + L"」，"
            L"对端实况见上一行状态与刚才的核实结论。\n";
    text += L" · 上游设置：第 " + std::to_wstring(settled) + L" 条没有写成（" + failed.commandLabel +
            L"，命令窗口那头的结论是「" + OrNone(conclusion) + L"」）。\n";
    for (size_t index = 0; index < done; ++index) {
      text += L" · 第 " + std::to_wstring(index + 1) + L" 条已经写成：" + plan_.steps[index].conclusion +
              L"\n";
    }
    text += L" · 剩下的 " + std::to_wstring(plan_.steps.size() - settled) +
            L" 条没有发出：那种场合配置只写了一半，本程序不把它当成「上游已经设好」，"
            L"也不自动重试。请对照命令窗口里那条命令的输出自己决定要不要再跑一次。";
    Reset();
    host.ShowWarning(L"上游设置没有完成", text);
    host.SetStatus(text);
    return false;
  }

  if (settled < plan_.steps.size()) {
    // 上一条写成，接着发下一条；这一步自己没启动成就在这里结案（结果未知的那一条绝不重发）。
    LaunchStep(host, ctx, settled);
    return Active();
  }
  std::wstring summary = L"首次推送完成：推送那一步的结论是「" + OrNone(plan_.pushConclusion) +
                         L"」，上游设置的 " + std::to_wstring(plan_.steps.size()) +
                         L" 条配置都已写成：\n";
  for (const git::UpstreamWriteStep& writeStep : plan_.steps) {
    summary += L" · " + writeStep.conclusion + L"\n";
  }
  summary += L"这条分支现在有了上游；下一次点「推送」走的是普通推送（目标由仓库配置定）。";
  Reset();
  host.SetStatus(summary);
  return false;
}

void UpstreamWriteFlow::Reset() noexcept {
  waitingStep_ = 0;
  plan_ = Plan{};
}

}  // namespace gc::ui
