#include "ui/restore_flow.h"

#include <string>
#include <utility>

#include "git/repository.h"
#include "ui/commands.h"

namespace gc::ui {

void RestoreFlow::Start(OperationHost& host, const OperationContext& ctx, git::RestoreClues clues) {
  clues_ = std::move(clues);
  stage_ = Stage::preConfirmProbe;

  platform::RestoreProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.branchRef = clues_.branchRef;
  request.moveToOid = clues_.moveToOid;
  request.isRootDeletion = clues_.isRootDeletion;
  request.timeoutMilliseconds = kRestoreProbeTimeoutMs;
  probeWorker_.Request(ctx.notifyWindow, kRestoreProbeCompleted, std::move(request),
                       [](const platform::RestoreProbeRequest& pending) {
                         return platform::RunRestoreProbeLoad(pending);
                       });
  host.SetStatus(L"按记录恢复之前，先在后台重读这条分支现在指着什么、要挪回去的对象还在不在、"
                 L"有没有流程停着（只读查询，不弹命令窗口、不改动仓库），读回来后给出确认框…");
}

void RestoreFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                   uint64_t completionSerial) {
  platform::RestoreProbeOutcome outcome;
  if (!probeWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 期间又发起了更晚的预检：这份结果不再有意义。
  }
  if (stage_ != Stage::preConfirmProbe) {
    return;  // 不是等中的那一次（例如已按仓库切换作废）。
  }
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    Abandon(host, L"预检完成时仓库已经换掉，本次没有执行任何恢复。请对现在的仓库重新选一次记录。");
    return;
  }
  ConfirmAndPlan(host, ctx, outcome.queries);
}

void RestoreFlow::ConfirmAndPlan(OperationHost& host, const OperationContext& ctx,
                                 const git::RestorePreflightQueries& queries) {
  plan_ = git::BuildRestorePlan(clues_, queries);
  if (plan_.blocked) {
    // 不可行（分支已移动/删除、对象不可达、有流程停着、读不回来）：只把话说清楚，不生成任何命令。
    host.ShowInfo(L"无法按记录恢复",
                  L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan_.explanation);
    Abandon(host, L"未执行恢复：" + plan_.explanation);
    return;
  }

  // feasible：走强制确认（这条命令会让 Git 移动一个引用，虽然只动引用、不碰索引与工作区）。
  const bool proceed = host.RiskConfirm(
      L"按记录恢复引用：风险确认", L"这条恢复只移动一个本地分支引用（带预期旧值，仅本地）",
      L"确认恢复（仅本地）", plan_.previewText);
  if (!proceed) {
    Abandon(host, L"已取消：没有打开命令窗口，也没有对仓库做任何改动。");
    return;
  }

  // 点头之后再核对一次分支现值：确认框是模态的，但期间外部终端照样可能动过仓库。走后台，不发命令之前必须回来。
  recheckTargetRoot_ = ctx.detection.root;
  stage_ = Stage::recheckAfterConfirm;
  platform::RestoreProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.branchRef = plan_.targetRef;
  request.timeoutMilliseconds = kRestoreRecheckTimeoutMs;
  recheckWorker_.Request(ctx.notifyWindow, kRestoreRecheckCompleted, std::move(request),
                         [](const platform::RestoreProbeRequest& pending) {
                           return platform::CaptureRestoreBranchRecheck(
                               pending.exePath, pending.repositoryDirectory, pending.branchRef,
                               pending.timeoutMilliseconds);
                         });
  host.SetStatus(L"点头之后、发出命令之前，正在后台核对这条分支还停在确认框上写的那一份…"
                 L"（只读查询，不弹命令窗口）…");
}

void RestoreFlow::OnRecheckCompleted(OperationHost& host, const OperationContext& ctx,
                                     uint64_t completionSerial) {
  git::GitQueryResult recheckBranch;
  if (!recheckWorker_.FetchLatest(completionSerial, &recheckBranch)) {
    return;  // 迟到的旧复核：丢弃。
  }
  if (stage_ != Stage::recheckAfterConfirm) {
    return;
  }
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(recheckTargetRoot_, ctx.detection.root)) {
    Abandon(host, L"复核完成时仓库已经换掉，本次没有执行任何恢复。请对现在的仓库重新选一次记录。");
    return;
  }
  const std::wstring refusal =
      git::DescribeRestoreRecheckMismatch(recheckBranch, plan_.expectedOldObjectId, plan_.targetRef);
  if (!refusal.empty()) {
    // 放弃的是「这一份现状」，不是用户的恢复意图：仓库重读回来，看清现状后是否再来由用户决定。
    host.SetStatus(refusal);
    host.ScheduleRefresh();
    return;
  }
  Launch(host, ctx);
}

void RestoreFlow::Launch(OperationHost& host, const OperationContext& ctx) {
  git::CommandWindowOperation operation;
  operation.operationId = plan_.operationId;
  operation.displayName = plan_.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  operation.arguments = plan_.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan_.commandLabel + L"（" + ctx.detection.root +
                        L"），恢复分支 " + plan_.targetRef + L"，等待 Git 退出码…";
  options.scopeNotice = plan_.notice;

  // 这次恢复本身也是一次引用移动，落一条历史。反悔线索指向「恢复之前的位置」：
  // 恢复后分支应指 newObjectId，若再要退回去就挪回 expectedOldObjectId（都带完整对象 ID）。
  options.history.record = true;
  options.history.flow = app::HistoryFlow::undo;
  options.history.operationLabel = plan_.displayName;
  options.history.sourceRef = plan_.targetRef;
  options.history.sourceObjectId = plan_.expectedOldObjectId;
  options.history.targetRef = plan_.targetRef;
  options.history.targetObjectId = plan_.newObjectId;
  if (!plan_.newObjectId.empty()) {
    options.history.restoreKind = app::HistoryRestoreKind::refMove;
    options.history.restoreBranchRef = plan_.targetRef;
    options.history.restoreExpectedCurrentId = plan_.newObjectId;
    options.history.restoreUndoToObjectId = plan_.expectedOldObjectId;
    options.history.restoreNote =
        L"按记录做的引用恢复：把 " + plan_.targetRef + L" 从 " + plan_.expectedOldObjectId +
        L" 挪到 " + plan_.newObjectId + L"。如需再反悔，可把它挪回 " + plan_.expectedOldObjectId +
        L"（本程序不会自动执行）。";
  } else {
    // 删除形态：恢复把引用删掉了；反手「重建」没有预期旧值可核，不属于自动回退范围。
    options.history.restoreKind = app::HistoryRestoreKind::none;
    options.history.restoreNote =
        L"按记录删除了分支引用 " + plan_.targetRef + L"（原指向 " + plan_.expectedOldObjectId +
        L"）。要重建需你按完整对象 ID 自行执行，本程序不代做无预期旧值的引用写入。";
  }

  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次恢复没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    return;
  }
}

void RestoreFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  clues_ = git::RestoreClues{};
  plan_ = git::RestorePlan{};
  recheckTargetRoot_.clear();
  host.SetStatus(std::wstring(reason));
}

void RestoreFlow::BeginStop() {
  probeWorker_.BeginStop();
  recheckWorker_.BeginStop();
}

void RestoreFlow::JoinWorkers() {
  probeWorker_.Join();
  recheckWorker_.Join();
}

}  // namespace gc::ui
