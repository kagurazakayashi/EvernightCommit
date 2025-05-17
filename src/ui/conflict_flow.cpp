#include "ui/conflict_flow.h"

#include <string>
#include <utility>

#include "git/repository.h"
#include "ui/commands.h"

namespace gc::ui {
namespace {

constexpr std::wstring_view kProbeNote =
    L"正在后台读回这个仓库的流程现场（Git 目录里的痕迹 + 未合并条目 + 当前分支/HEAD，"
    L"全部只读查询，不弹命令窗口、不改动仓库）…";

}  // namespace

void ConflictFlow::Start(OperationHost& host, const OperationContext& ctx, Entry entry) {
  entry_ = entry;
  preflightFacts_ = git::ConflictStateFacts{};
  plan_ = git::ConflictOperationPlan{};
  recheckTargetRoot_.clear();
  const Stage stage = entry == Entry::view
                          ? Stage::viewProbe
                          : (entry == Entry::continueFlow ? Stage::continueProbe : Stage::abortProbe);
  RequestProbe(host, ctx, stage, entry);
}

void ConflictFlow::RequestProbe(OperationHost& host, const OperationContext& ctx, Stage stage,
                                Entry entry) {
  stage_ = stage;

  platform::ConflictProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kConflictProbeTimeoutMs;
  probeWorker_.Request(ctx.notifyWindow, kConflictProbeCompleted, std::move(request),
                       [](const platform::ConflictProbeRequest& pending) {
                         return platform::RunConflictProbeLoad(pending);
                       });
  host.SetStatus(std::wstring(entry == Entry::view ? L"查看冲突与暂停流程："
                                 : (entry == Entry::continueFlow ? L"继续该流程之前："
                                                                 : L"中止该流程之前：")) +
                 std::wstring(kProbeNote));
}

void ConflictFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                    uint64_t completionSerial) {
  platform::ConflictProbeOutcome outcome;
  if (!probeWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 期间又发起了更晚的读取，这份结果不再有意义。
  }
  if (stage_ != Stage::viewProbe && stage_ != Stage::continueProbe && stage_ != Stage::abortProbe) {
    return;  // 不是等中的那一次。
  }
  const bool wasView = stage_ == Stage::viewProbe;
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    Abandon(host, L"现场读取完成时仓库已经换掉，本次没有发出任何命令。请对现在的仓库重新点一次。");
    return;
  }
  preflightFacts_ = outcome.state;
  if (wasView) {
    // 查看这条路不弹确认、也不发命令：把读回来的现场原样交代清楚就够了。
    host.ShowInfo(L"冲突与暂停流程的现场", git::DescribeConflictState(outcome.state));
    host.SetStatus(L"已按刚才读回的现场展示这个仓库停着的是什么流程（只读，没有打开命令窗口，"
                   L"也没有改动仓库）。");
    return;
  }
  HandleProbe(host, ctx, outcome.state);
}

void ConflictFlow::HandleProbe(OperationHost& host, const OperationContext& ctx,
                               const git::ConflictStateFacts& facts) {
  const bool continuing = entry_ == Entry::continueFlow;
  const git::ConflictOperationPlan plan =
      continuing ? git::BuildConflictContinuePlan(facts) : git::BuildConflictAbortPlan(facts);
  if (plan.blocked) {
    host.ShowInfo(continuing ? L"不能继续这个流程" : L"不能中止这个流程",
                  L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason);
    Abandon(host, (continuing ? L"没有发出继续的命令：" : L"没有发出中止的命令：") +
                      plan.blockedReason);
    return;
  }

  const bool proceed =
      continuing ? host.Confirm(L"继续该流程前请确认", plan.confirmationText, /*warningIcon=*/true)
                 : host.RiskConfirm(L"中止该流程：风险确认", L"这条命令会改动你的工作区与索引",
                                    L"仍然中止（改动工作区）", plan.confirmationText);
  if (!proceed) {
    Abandon(host, L"已取消：没有打开命令窗口，也没有对仓库做任何改动。"
                  L"你写在冲突文件里的内容与提交表单里的字都原样留着。");
    return;
  }
  plan_ = plan;
  RequestRecheck(host, ctx);
}

void ConflictFlow::RequestRecheck(OperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::recheck;
  recheckTargetRoot_ = ctx.detection.root;

  platform::ConflictProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kConflictProbeTimeoutMs;
  recheckWorker_.Request(ctx.notifyWindow, kConflictRecheckCompleted, std::move(request),
                         [](const platform::ConflictProbeRequest& pending) {
                           return platform::RunConflictProbeLoad(pending);
                         });
  host.SetStatus(L"点头之后、发出命令之前，再把刚才那组只读查询原样重发一遍核对现场"
                 L"（流程痕迹 / 未合并条目 / 当前分支 / HEAD）：对得上才发那条命令…");
}

void ConflictFlow::OnRecheckCompleted(OperationHost& host, const OperationContext& ctx,
                                      uint64_t completionSerial) {
  platform::ConflictProbeOutcome outcome;
  if (!recheckWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 迟到的旧复核。
  }
  if (stage_ != Stage::recheck) {
    return;  // 这一次已经按其它路径结案。
  }
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(recheckTargetRoot_, ctx.detection.root)) {
    Abandon(host, L"复核完成时仓库已经换掉，本次没有发出任何命令。请对现在的仓库重新点一次。");
    return;
  }
  const std::wstring change = git::DescribeConflictStateChange(preflightFacts_, outcome.state);
  if (!change.empty()) {
    // 放弃的是「这一份现状」，不是用户的意图：重读回来，看清现状后要不要再做由用户决定。
    host.ShowWarning(L"执行前复核：现场又变了", change);
    host.SetStatus(L"执行前复核发现现场与确认框上写的不一致，因此那条命令没有发出。"
                   L"仓库状态正在重读，看清现状后如仍要做同一件事请重新点一次。");
    host.ScheduleRefresh();
    return;
  }
  Launch(host, ctx);
}

void ConflictFlow::Launch(OperationHost& host, const OperationContext& ctx) {
  git::CommandWindowOperation operation;
  operation.operationId = plan_.operationId;
  operation.displayName = plan_.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  operation.arguments = plan_.arguments;

  const bool continuing = entry_ == Entry::continueFlow;
  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan_.commandLabel + L"（" +
                        (continuing ? std::wstring(L"继续这个流程") : std::wstring(L"中止这个流程")) +
                        L"；工作目录 " + ctx.detection.root + L"），等待 Git 退出码…";
  options.scopeNotice = plan_.notice;
  options.conflictContinueOperation = continuing;
  options.conflictAbortOperation = !continuing;
  // 操作历史：继续/中止都是多提交重放或工作区重写，不是单条能安全「挪回」的本地引用，
  // 恢复类别为 none；中止本身已是回退动作，本程序不在其上再叠加任何自动撤销。
  options.history.record = true;
  options.history.flow = continuing ? app::HistoryFlow::conflictContinue : app::HistoryFlow::conflictAbort;
  options.history.workTreeRoot = ctx.detection.root;
  options.history.operationLabel = plan_.displayName;
  options.history.restoreKind = app::HistoryRestoreKind::none;
  options.history.restoreNote =
      continuing ? L"「继续该流程」会让 Git 建立提交并改动索引/工作区：那是合并/重放的结果，"
                    L"不是一条能安全「挪回」的本地引用。本程序不自动撤销，处理以命令窗口输出与现场为准。"
                 : L"「中止该流程」由 Git 重写工作区与索引、回到流程开始前的状态；它本身已是回退动作，"
                    L"且不止涉及一个引用。本程序不在此之上再自动撤销。";
  const std::wstring commandLabel = plan_.commandLabel;
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这条命令没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                   L"仓库没有被改动过，现场与你的表单都原样留着。");
    preflightFacts_ = git::ConflictStateFacts{};
    plan_ = git::ConflictOperationPlan{};
    return;
  }
  // 命令已经在窗口里跑了：本控制器的这一轮到此结束（终态由主窗口的命令窗口回调交回来，
  // 槽位的释放与刷新策略都走既有那一条路）。
  preflightFacts_ = git::ConflictStateFacts{};
  plan_ = git::ConflictOperationPlan{};
  recheckTargetRoot_.clear();
  host.SetStatus(L"已在命令窗口启动 " + commandLabel + L"。窗口里的输出与退出码由你自己核对；"
                 L"窗口可以一直留着，本程序不等它关闭就按 Git 的终态结案。");
}

void ConflictFlow::BeginFailedReport(OperationHost& host, const OperationContext& ctx, Entry entry,
                                     std::wstring baseConclusion, git::CommandCompletion completion,
                                     long exitCode, std::wstring environmentNotice) {
  entry_ = entry;
  stage_ = Stage::aftermathReport;
  reportBaseNote_ = std::move(baseConclusion);
  reportCompletion_ = completion;
  reportExitCode_ = exitCode;
  reportEnvironmentNotice_ = std::move(environmentNotice);

  platform::ConflictProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kConflictProbeTimeoutMs;
  aftermathWorker_.Request(ctx.notifyWindow, kConflictAftermathCompleted, std::move(request),
                           [](const platform::ConflictProbeRequest& pending) {
                             return platform::RunConflictProbeLoad(pending);
                           });
  host.SetStatus(L"这一步没有按预期完成，正在后台把现场读回来如实说明"
                 L"（只读；本程序不会做任何自动恢复动作）…");
}

void ConflictFlow::OnAftermathCompleted(OperationHost& host, uint64_t completionSerial) {
  platform::ConflictProbeOutcome outcome;
  if (!aftermathWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 迟到的旧读取。
  }
  if (stage_ != Stage::aftermathReport) {
    return;
  }
  stage_ = Stage::none;

  const std::wstring text = DescribeAftermath(outcome.state);
  host.ShowWarning(entry_ == Entry::continueFlow ? L"继续这个流程的那一步没有完成"
                                                 : L"中止这个流程的那一步没有完成",
                   text);

  std::wstring note = entry_ == Entry::continueFlow
                          ? std::wstring(L"继续这一步没有完成：")
                          : std::wstring(L"中止这一步没有完成：");
  if (!outcome.state.probed) {
    note += L"现场没能读回来（" +
            (outcome.state.probeFailure.empty() ? std::wstring(L"原因没能带回来")
                                                 : outcome.state.probeFailure) +
            L"）。以命令窗口里 Git 的真实输出为准。";
  } else if (outcome.state.kind == git::ConflictFlowKind::none) {
    note += L"现在这个仓库里已经没有流程痕迹了——这一步很可能其实已经走完，"
            L"或者那个流程已在别处被中止。成败仍只按刚才命令窗口的退出码算，本程序不据此改判。";
  } else {
    note += L"仓库仍停在「" + git::ConflictFlowKindLabel(outcome.state.kind) + L"」";
    if (outcome.state.unmergedReadOk) {
      note += L"，未合并 " + std::to_wstring(outcome.state.unmergedPaths.size()) + L" 个文件";
    }
    note += L"。本程序没有 abort/reset/clean/stash，也没删任何锁，更没有替你选内容；"
            L"处理现场由你决定（详见刚才的说明框）。";
  }
  std::wstring conclusion = reportBaseNote_ + note;
  if (!reportEnvironmentNotice_.empty()) {
    conclusion += L"｜" + reportEnvironmentNotice_;
  }
  host.RememberOperationConclusion(conclusion);
  reportBaseNote_.clear();
  reportEnvironmentNotice_.clear();
  host.ScheduleRefresh();
}

std::wstring ConflictFlow::DescribeAftermath(const git::ConflictStateFacts& latest) const {
  const bool continuing = entry_ == Entry::continueFlow;
  std::wstring text = continuing
                          ? std::wstring(L"刚才那条「继续这个流程」的命令在命令窗口里以非成功收场。")
                          : std::wstring(L"刚才那条「中止这个流程」的命令在命令窗口里以非成功收场。");
  text += L"\n那一步的终态：" + std::wstring(git::CommandCompletionLabel(reportCompletion_)) +
          L"，Git 的退出码：" + std::to_wstring(reportExitCode_) + L"\n";
  text += L"（「进程没起来」「被提前关掉窗口」「超过观察期限仍没有结果」与「Git 自己返回非 0」"
          L"是不同的事，上面那行写的就是本次拿到的事实；退出码只在「正常结束」时才是 Git 的回答。）\n";
  text += L"\n下面是刚才重新读回来的现场（只读查询）：\n" + git::DescribeConflictState(latest) + L"\n";
  text += L"\n本程序没有做的事：\n";
  text += L"  · 没有 reset --hard、没有 clean、没有 stash、没有删 index.lock，"
          L"也没有替你选 ours/theirs 或改动任何文件内容。\n";
  text += L"  · 没有自动重试，也没有替你补一条收尾命令；你写在提交表单里的字一个字没动。\n";
  text += L"\n接下来由你决定：按上面那份现场继续解决冲突后再点「继续该流程」，"
          L"或改点「中止该流程」，或到外部终端里自己处理。"
          L"如果你怀疑这一步其实已经做成，先点「刷新」看仓库现状——"
          L"成败仍以刚才那个命令窗口里 Git 的退出码为准。\n";
  return text;
}

void ConflictFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  preflightFacts_ = git::ConflictStateFacts{};
  plan_ = git::ConflictOperationPlan{};
  recheckTargetRoot_.clear();
  host.SetStatus(std::wstring(reason));
}

void ConflictFlow::BeginStop() {
  probeWorker_.BeginStop();
  recheckWorker_.BeginStop();
  aftermathWorker_.BeginStop();
}

void ConflictFlow::JoinWorkers() {
  probeWorker_.Join();
  recheckWorker_.Join();
  aftermathWorker_.Join();
}

}  // namespace gc::ui
