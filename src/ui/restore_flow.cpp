#include "ui/restore_flow.h"

#include <string>
#include <utility>

#include "git/repository.h"
#include "ui/commands.h"

namespace gc::ui {
namespace {

// 恢复链路上三条后台查询共用的超时都来自 commands.h 的既有常量；这里只是把「问哪一组事实」
// 的构造收在一处，预检与复核不会各问一套。
platform::RestoreProbeRequest MakeRequest(const OperationContext& ctx,
                                          const git::RestoreClues& clues,
                                          unsigned long timeoutMilliseconds) {
  platform::RestoreProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.branchRef = clues.branchRef;
  request.moveToOid = clues.moveToOid;
  request.isRootDeletion = clues.isRootDeletion;
  request.timeoutMilliseconds = timeoutMilliseconds;
  return request;
}

}  // namespace

bool RestoreFlow::BindingHolds(const OperationContext& ctx) const {
  // 判定本身在 app::RepositoryBinding（纯逻辑，有测试钉住每一档失效原因）。
  return binding_.MatchesCurrent(ctx.repoUsable, ctx.repositoryIdentity,
                                 ctx.detection.absoluteGitDir, ctx.gitExecutable,
                                 ctx.detection.root);
}
void RestoreFlow::Start(OperationHost& host, const OperationContext& ctx, git::RestoreClues clues) {
  ResetState();
  // 先把身份钉下来：此后每一步（预检回来的结果、确认框、复核、命令窗口）都只认这一组。
  binding_.valid = true;
  binding_.workTreeRoot = ctx.detection.root;
  binding_.absoluteGitDir = ctx.detection.absoluteGitDir;
  binding_.gitExePath = ctx.gitExecutable;
  binding_.generation = ctx.repositoryIdentity.generation;
  clues_ = std::move(clues);
  stage_ = Stage::preConfirmProbe;

  const platform::RestoreProbeRequest request = MakeRequest(ctx, clues_, kRestoreProbeTimeoutMs);
  probeWorker_.Request(ctx.notifyWindow, kRestoreProbeCompleted, request,
                       [](const platform::RestoreProbeRequest& pending) {
                         return platform::RunRestoreProbeLoad(pending);
                       });
  host.SetStatus(L"按记录恢复之前，先在后台重读：这条分支现在指着什么、这个引用名本身是不是符号引用、"
                 L"这条分支有没有被别的工作树检出、要挪回去的对象还在不在、有没有流程停着"
                 L"（只读查询，不弹命令窗口、不改动仓库），读回来后给出确认框…");
}

void RestoreFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                   uint64_t completionSerial) {
  platform::RestoreProbeOutcome outcome;
  if (!probeWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 期间又发起了更晚的预检：这份结果不再有意义。
  }
  if (stage_ != Stage::preConfirmProbe) {
    return;  // 不是等中的那一次（已按取消、作废或结案处理过）。
  }
  if (!BindingHolds(ctx) || !binding_.MatchesProbedDirectory(outcome.repositoryDirectory)) {
    Abandon(host, L"预检完成时仓库身份已经变了（换过仓库、重新识别或换了 Git 程序），"
                  L"本次没有执行任何恢复。请对现在的仓库重新选一次记录。");
    return;
  }
  ConfirmAndPlan(host, ctx, outcome.queries);
}

void RestoreFlow::ConfirmAndPlan(OperationHost& host, const OperationContext& ctx,
                                 const git::RestorePreflightQueries& queries) {
  plan_ = git::BuildRestorePlan(clues_, queries);
  if (plan_.blocked) {
    // 不可行（分支已移动/删除、对象不可达、引用形态或占用不能放行、有流程停着、读不回来）：
    // 只把话说清楚，不生成任何命令。
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

  // 点头之后重问的是与预检同一组事实（不只是分支 OID）：确认框停留的几分钟里，外部完全可以
  // 开一个不移动分支的合并、把这个名字改成符号引用、或把这条分支检出到另一个工作树。
  // 那种场合旧的那份确认不属于新的现场，必须重新问一遍才能发命令。走后台，GUI 线程不等子进程。
  if (!BindingHolds(ctx)) {
    Abandon(host, L"确认框期间仓库身份已经变了，本次没有执行任何恢复。"
                  L"请对现在的仓库重新选一次记录。");
    return;
  }
  stage_ = Stage::recheckAfterConfirm;
  // 复核问的还是绑定的那个仓库（用绑定值构造，不用 ctx 的当前值）：这一步的意义就是
  // 「确认过的那份现状还在不在」，问错了仓库就连这个问题都变了。
  platform::RestoreProbeRequest request;
  request.exePath = binding_.gitExePath;
  request.repositoryDirectory = binding_.workTreeRoot;
  request.absoluteGitDir = binding_.absoluteGitDir;
  request.branchRef = plan_.targetRef;
  request.timeoutMilliseconds = kRestoreRecheckTimeoutMs;
  recheckWorker_.Request(ctx.notifyWindow, kRestoreRecheckCompleted, std::move(request),
                         [](const platform::RestoreProbeRequest& pending) {
                           return platform::CaptureRestoreRecheck(pending);
                         });
  host.SetStatus(L"点头之后、发出命令之前，正在后台重问同一组事实（分支现值、引用形态与占用、"
                 L"流程痕迹），确认确认框上写的那一份现状仍然成立…（只读查询，不弹命令窗口）…" +
                 std::wstring(L"要挪回去的那个对象是固定的完整 ID，不随外部进程改变，"
                              L"这一轮不重问；命令里带的预期旧值由 Git 原子核对。"));
}

void RestoreFlow::OnRecheckCompleted(OperationHost& host, const OperationContext& ctx,
                                     uint64_t completionSerial) {
  git::RestoreRecheckScene scene;
  if (!recheckWorker_.FetchLatest(completionSerial, &scene)) {
    return;  // 迟到的旧复核：丢弃。
  }
  if (stage_ != Stage::recheckAfterConfirm) {
    return;
  }
  if (!BindingHolds(ctx)) {
    Abandon(host, L"复核完成时仓库身份已经变了，本次没有执行任何命令。"
                  L"看清现状后如仍要恢复，请重新选一次这条记录。");
    return;
  }
  const std::wstring refusal = git::DescribeRestoreRecheckMismatch(scene, plan_.expectedOldObjectId,
                                                                   plan_.targetRef);
  if (!refusal.empty()) {
    // 放弃的是「这一份现状」，不是用户的恢复意图：仓库重读回来，看清现状后是否再来由用户决定。
    stage_ = Stage::none;
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
  operation.gitExecutable = binding_.gitExePath;
  operation.repositoryDirectory = binding_.workTreeRoot;
  operation.arguments = plan_.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan_.commandLabel + L"（" + binding_.workTreeRoot +
                        L"），恢复分支 " + plan_.targetRef + L"，等待 Git 退出码…";
  options.scopeNotice = plan_.notice;
  options.restoreOperation = true;
  // 这次恢复本身也是一次引用移动，落一条历史；仓库身份取绑定值，不取界面此刻的根。
  // 反悔线索指向「恢复之前的位置」：恢复后分支应指 newObjectId，若再要退回去就挪回
  // expectedOldObjectId（都带完整对象 ID）。
  options.history.record = true;
  options.history.flow = app::HistoryFlow::restore;
  options.history.workTreeRoot = binding_.workTreeRoot;
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
    // 删除形态：恢复把引用删掉了；重建不在本程序自动执行的范围内。
    options.history.restoreKind = app::HistoryRestoreKind::none;
    options.history.restoreNote =
        L"按记录删除了分支引用 " + plan_.targetRef + L"（原指向 " + plan_.expectedOldObjectId +
        L"）。要重建需你按完整对象 ID 自行执行，本程序不代做无预期旧值的引用写入。";
  }

  stage_ = Stage::commandRunning;  // 先占住流程状态：启动失败会被下面清掉。
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次恢复没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                   L"仓库没有被改动。");
    ResetState();
    return;
  }
  static_cast<void>(ctx);
}

void RestoreFlow::OnCommandSettled(OperationHost& host, bool succeeded,
                                   std::wstring_view conclusion) {
  if (stage_ != Stage::commandRunning) {
    return;  // 不属于这一次的恢复（已作废或本来就没发命令）。
  }
  std::wstring text = succeeded ? L"按记录恢复已完成（只移动了那一个分支引用，索引与工作区没动）："
                                : L"按记录恢复没有成功（这条命令自己报告的结论如下；"
                                  L"引用是否被移动以仓库现状为准，本程序不自动重试、不自动回退）：";
  text += std::wstring(conclusion);
  ResetState();
  host.SetStatus(text);
  host.ScheduleRefresh();
}

void RestoreFlow::Forget() noexcept { ResetState(); }

void RestoreFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  ResetState();
  host.SetStatus(std::wstring(reason));
}

void RestoreFlow::ResetState() noexcept {
  stage_ = Stage::none;
  binding_ = app::RepositoryBinding{};
  clues_ = git::RestoreClues{};
  plan_ = git::RestorePlan{};
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
