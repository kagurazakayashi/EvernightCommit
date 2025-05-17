#include "ui/undo_flow.h"

#include <string>
#include <utility>

#include "git/commit_history.h"
#include "git/repository.h"
#include "platform/windows/undo_probe.h"
#include "platform/windows/win_path.h"
#include "ui/commands.h"

namespace gc::ui {

void UndoFlow::Start(OperationHost& host, const OperationContext& ctx) {
  // 记下点击瞬间界面显示的那份摘要：预检回来后与它对比，不一致时确认框必须说明
  // 「以下以刚读回的为准」（与创建提交同一套原则，不假装旧状态还成立）。
  captured_ = git::CapturedSnapshot{};
  captured_.valid = true;
  captured_.shortSha = ctx.detection.shortSha;
  captured_.hasHead = ctx.detection.headResolved;
  captured_.stagedItems = ctx.stagedItemCount;  // 点击瞬间的「已暂存的更改」条数（快照由入口采集）。
  stage_ = Stage::preConfirmProbe;

  platform::UndoProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.timeoutMilliseconds = kUndoProbeTimeoutMs;
  probeWorker_.Request(ctx.notifyWindow, kUndoProbeCompleted, std::move(request),
                       [](const platform::UndoProbeRequest& pending) {
                         return platform::RunUndoProbeLoad(pending);
                       });
  host.SetStatus(L"撤回最近提交前先在后台重读分支、HEAD、父提交、远端跟踪引用与工作区状态"
                 L"（只读查询，不弹命令窗口、不改动仓库），读回来后给出确认框…");
}

void UndoFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                uint64_t completionSerial) {
  platform::UndoProbeOutcome outcome;
  if (!probeWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  if (stage_ != Stage::preConfirmProbe) {
    return;  // 不是等中的那一次（例如已按「仓库切换」作废）。
  }
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    // 预检是在旧仓库上跑的：那份 HEAD/父提交/远端事实对当前界面显示的仓库毫无意义。
    Abandon(host, L"预检完成时仓库已经换掉，本次没有执行任何撤回。请对现在的仓库重新点一次“撤回最近提交”。");
    return;
  }
  preflightFacts_ = outcome.facts;
  ConfirmAndPlan(host, ctx, outcome.facts);
}

void UndoFlow::ConfirmAndPlan(OperationHost& host, const OperationContext& ctx,
                              const git::UndoPreflightFacts& facts) {
  git::UndoCommitPlanInput input;
  input.facts = facts;
  // 流程痕迹的依据是 Git 目录里的档案存在性（毫秒级、非子进程），与创建提交同一来源。
  input.workflow = platform::ProbeRepositoryWorkflowState(ctx.detection.absoluteGitDir);
  input.repositoryRoot = ctx.detection.root;
  input.captured = captured_;
  captured_ = git::CapturedSnapshot{};

  const git::UndoCommitPlan plan = git::BuildUndoCommitPlan(input);
  if (plan.blocked) {
    host.ShowInfo(L"无法撤回最近提交",
                  L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason);
    Abandon(host, L"未执行撤回：" + plan.blockedReason);
    return;
  }

  const bool proceed =
      plan.requiresForce
          ? host.RiskConfirm(L"撤回最近提交：风险确认", L"这条撤回带有需要你自己核对的风险",
                             L"强制撤回（仅本地）", plan.previewText)
          : host.Confirm(L"撤回最近提交前请确认", plan.previewText, /*warningIcon=*/true);
  if (!proceed) {
    Abandon(host, L"已取消：没有打开命令窗口，也没有对仓库做任何改动。");
    return;
  }

  // 用户点头之后再核对一次 HEAD/分支：确认框是模态的，但期间外部终端照样可能动过仓库。
  // 这一核对现在走后台（原来是 GUI 线程上的同步子进程调用，点头之后窗口可能长时间冻结）：
  // 对不上或没读回来就取消并刷新——绝不对着已经变了的目标执行旧方案；没回来之前不发命令。
  plan_ = plan;
  recheckTargetRoot_ = ctx.detection.root;
  stage_ = Stage::recheckAfterConfirm;
  platform::UndoProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.timeoutMilliseconds = kUndoRecheckTimeoutMs;
  recheckWorker_.Request(ctx.notifyWindow, kUndoRecheckCompleted, std::move(request),
                         [](const platform::UndoProbeRequest& pending) {
                           return platform::CaptureUndoHeadSnapshot(
                               pending.exePath, pending.repositoryDirectory, pending.timeoutMilliseconds);
                         });
  host.SetStatus(L"点头之后、发出命令之前，正在后台核对 HEAD 与分支还停在确认框上写的那一份…"
                 L"（只读查询，不弹命令窗口）…");
}

void UndoFlow::OnRecheckCompleted(OperationHost& host, const OperationContext& ctx,
                                  uint64_t completionSerial) {
  git::UndoHeadFacts recheck;
  if (!recheckWorker_.FetchLatest(completionSerial, &recheck)) {
    return;  // 迟到的旧复核：丢弃。
  }
  if (stage_ != Stage::recheckAfterConfirm) {
    return;  // 这一次撤回已经按其它路径结案。
  }
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(recheckTargetRoot_, ctx.detection.root)) {
    Abandon(host, L"复核完成时仓库已经换掉，本次没有执行任何撤回。请对现在的仓库重新点一次“撤回最近提交”。");
    return;
  }
  // 方案层裁决：「复核没能完成」与「HEAD/分支又变了」分别说清，绝不合并成一句“请重试”。
  const std::wstring refusal = git::DescribeUndoRecheckMismatch(recheck, preflightFacts_);
  if (!refusal.empty()) {
    // 放弃的是「这一份现状」，不是用户的意图：仓库重读回来，看清现状后是否再来由用户决定。
    host.SetStatus(refusal);
    host.ScheduleRefresh();
    return;
  }
  Launch(host, ctx, plan_, preflightFacts_);
}

void UndoFlow::Launch(OperationHost& host, const OperationContext& ctx,
                      const git::UndoCommitPlan& plan, const git::UndoPreflightFacts& facts) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（" +
                        ctx.detection.root + L"），撤回提交 " +
                        git::ShortObjectId(facts.head.headObjectId) + L"，等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.undoOperation = true;
  options.restoreHint = plan.restoreHint;
  // 操作历史：撤回是「移动一个本地分支引用」，带完整对象 ID，因此是唯一可审查回退的一类。
  // 反手恢复就是把那条分支从「撤回后的父提交」挪回「被撤回的原提交」——同一条原子 update-ref 命令族。
  options.history.record = true;
  options.history.flow = app::HistoryFlow::undo;
  options.history.workTreeRoot = ctx.detection.root;
  options.history.operationLabel = plan.displayName;
  options.history.sourceRef = plan.targetRef;
  options.history.sourceObjectId = plan.expectedOldObjectId;  // 撤回前该分支指着的原提交
  if (!plan.newObjectId.empty()) {
    options.history.restoreKind = app::HistoryRestoreKind::refMove;
    options.history.restoreBranchRef = plan.targetRef;
    options.history.restoreExpectedCurrentId = plan.newObjectId;        // 撤回后分支应指（父提交）
    options.history.restoreUndoToObjectId = plan.expectedOldObjectId;   // 恢复要挪回的原提交
  } else {
    // 根提交撤回删掉了引用：恢复它等于「无预期旧值地重建引用」，不属于自动回退范围。
    options.history.restoreKind = app::HistoryRestoreKind::none;
  }
  options.history.restoreNote = plan.restoreHint;
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次撤回没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    return;
  }
  if (!plan.stateChangeNote.empty()) {
    // 用户已经看过那句「以刚读回的为准」，这一句留在状态栏里，操作结束后还能对上号。
    host.SetStatus(L"已在命令窗口启动撤回最近提交。" + plan.stateChangeNote);
  }
}

void UndoFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  captured_ = git::CapturedSnapshot{};
  preflightFacts_ = git::UndoPreflightFacts{};
  plan_ = git::UndoCommitPlan{};
  recheckTargetRoot_.clear();
  host.SetStatus(std::wstring(reason));
}

void UndoFlow::BeginStop() {
  probeWorker_.BeginStop();
  recheckWorker_.BeginStop();
}

void UndoFlow::JoinWorkers() {
  probeWorker_.Join();
  recheckWorker_.Join();
}

}  // namespace gc::ui
