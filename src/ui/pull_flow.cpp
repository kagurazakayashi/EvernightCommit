#include "ui/pull_flow.h"

#include <algorithm>
#include <string>
#include <utility>

#include "git/commit_history.h"
#include "git/repository.h"
#include "platform/windows/remote_choice_dialog.h"
#include "ui/commands.h"

namespace gc::ui {

void PullFlow::Start(OperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::fetchProbe;
  platform::PullProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullProbeTimeoutMs;
  request.includeRelationship = false;
  worker_.Request(ctx.notifyWindow, kPullProbeCompleted, std::move(request),
                  [](const platform::PullProbeRequest& pending) {
                    return platform::RunPullProbeLoad(pending);
                  });
  host.SetStatus(L"pull 第一步：先在后台只读问清「在哪个分支、这个分支的上游是谁、你的 "
                 L"pull/rebase 与 ff 配置怎么写的、会影响抓取范围的配置（prune／标签／fetch 映射）、"
                 L"工作区现状」（不弹命令窗口、不接触任何远端），问回来后先把要处理的分支对摆给你看…");
}

void PullFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                uint64_t completionSerial) {
  platform::PullProbeOutcome outcome;
  if (!worker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  const Stage stage = stage_;
  if (stage == Stage::none) {
    return;  // 这一次 pull 已经按「取消 / 换仓库 / 结案」作废，迟到的结果原样丢掉。
  }
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    Abandon(host, L"预检完成时仓库已经换掉，这次 pull 没有执行任何命令。请对现在的仓库重新点一次「pull」。");
    return;
  }
  switch (stage) {
    case Stage::fetchProbe:
      HandleFetchProbe(host, ctx, outcome);
      break;
    case Stage::integrateProbe:
      HandleIntegrateProbe(host, ctx, outcome);
      break;
    case Stage::recheckProbe:
      HandleRecheck(host, ctx, outcome);
      break;
    default:
      Abandon(host, L"预检回来时这次 pull 已经不在等它了，没有执行任何命令。");
      break;
  }
}

void PullFlow::HandleFetchProbe(OperationHost& host, const OperationContext& ctx,
                                const platform::PullProbeOutcome& outcome) {
  const git::PullFetchPlan plan = git::BuildPullFetchPlan(outcome.target, ctx.detection.root);
  if (plan.state == git::PullFetchPlanState::blocked) {
    host.ShowInfo(L"现在不能 pull", plan.explanation);
    Abandon(host, L"未执行 pull：前提不成立（原因见刚才的说明框）。没有打开命令窗口，也没有接触"
                 L"任何远端或改动仓库。");
    return;
  }
  if (!host.Confirm(L"pull 第一步：先获取，请确认", plan.confirmationText, /*warningIcon=*/false)) {
    Abandon(host, L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。整合这一步更没有被谈起。");
    return;
  }

  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  operation.arguments = plan.arguments;  // 目标只认远端名字，URL 由 Git 自己按配置解析。

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（pull 的第一步：获取；" +
                        plan.remoteName + L" → " + plan.trackingRef + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pullFetchOperation = true;
  // 操作历史：获取步与 fetch 同边界，不动本地引用，恢复类别为 none。
  options.history.record = true;
  options.history.flow = app::HistoryFlow::pull;
  options.history.workTreeRoot = ctx.detection.root;
  options.history.operationLabel = plan.displayName;
  options.history.remoteName = plan.remoteName;
  options.history.restoreKind = app::HistoryRestoreKind::none;
  options.history.restoreNote =
      L"pull 的获取步与 fetch 同边界：只更新远端跟踪引用，不移动 HEAD/分支/索引/工作区。";
  stage_ = Stage::fetching;
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次 pull 停在第一步：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                   L"远端没有被接触，仓库也没有改动。");
    AbandonFlow();
  }
}

void PullFlow::OnFetchSettled(OperationHost& host, const OperationContext& ctx, bool fetchSucceeded) {
  if (stage_ != Stage::fetching) {
    // 通知迟到（期间换了仓库、或这一步已按别的路径结案）：绝不能凭这份旧状态继续往下整合。
    AbandonFlow();
    return;
  }
  if (!fetchSucceeded) {
    AbandonFlow();
    host.SetStatus(L"pull 停在第一步：命令窗口里那次获取没有成功，因此没有做任何整合。"
                   L"远端跟踪引用有没有被这次抓取改动，以正在重读的现状为准；本程序不自动重试，"
                   L"也不会因此 prune、改用别的远端或改写任何远端配置。原因看命令窗口里 Git 的真实输出。");
    return;
  }

  stage_ = Stage::integrateProbe;
  fetchAlreadyRan_ = true;
  platform::PullProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullProbeTimeoutMs;
  request.includeRelationship = true;
  worker_.Request(ctx.notifyWindow, kPullProbeCompleted, std::move(request),
                  [](const platform::PullProbeRequest& pending) {
                    return platform::RunPullProbeLoad(pending);
                  });
  host.SetStatus(L"获取已完成。正在后台重读仓库现状，并只读核对本地与远端的关系（各有几个独有提交、"
                 L"共同基准、这次会带进哪些文件、内容冲突预演），判回来后将问你要不要整合…");
}

void PullFlow::HandleIntegrateProbe(OperationHost& host, const OperationContext& ctx,
                                    const platform::PullProbeOutcome& outcome) {
  // 这一份事实是确认框与「执行前复核」的比对基准，必须先存下来再弹框。
  integrateFacts_ = outcome;
  ComposeAndConfirmIntegrate(host, ctx, outcome, git::PullStrategyChoice::none);
}

void PullFlow::ComposeAndConfirmIntegrate(OperationHost& host, const OperationContext& ctx,
                                          const platform::PullProbeOutcome& outcome,
                                          git::PullStrategyChoice choice) {
  git::PullIntegratePlanInput input;
  input.target = outcome.target;
  input.relationship = outcome.relationship;
  input.repositoryRoot = ctx.detection.root;
  input.choice = choice;
  const git::PullIntegratePlan plan = git::BuildPullIntegratePlan(input);

  // 取消/拒绝时几乎都要带同一句：那次获取已经动了远端跟踪引用，而且不该被退回去。
  const std::wstring fetchLeftover =
      fetchAlreadyRan_
          ? std::wstring(L"要说明的是：刚才那次获取（在命令窗口里跑的那条 fetch）已经把远端跟踪引用更新到"
                         L"远端的位置，这一处改动保留着——本程序不会把它退回去，也不需要你做什么。"
                         L"你取消的是接下来的整合：本地分支、索引与工作区都没有被动过。")
          : std::wstring(L"没有执行整合：本地分支、索引与工作区都没有被动过，也没有接触任何远端。");

  switch (plan.state) {
    case git::PullPlanState::blocked: {
      host.ShowInfo(L"现在不能整合", plan.explanation);
      Abandon(host, L"未执行整合：" + fetchLeftover);
      return;
    }
    case git::PullPlanState::nothingToIntegrate: {
      host.ShowInfo(L"pull：远端没有要整合的内容", plan.explanation);
      Abandon(host, plan.explanation + L"\n" + fetchLeftover);
      return;
    }
    case git::PullPlanState::chooseStrategy: {
      // 分叉而配置没定策略：合并与变基两种做法摆出来，选完还会再有一次带风险清单的确认。
      platform::RemoteChoiceSpec spec;
      spec.title = L"选择这次 pull 的整合方式";
      spec.label = plan.explanation;
      for (const std::wstring& candidate : plan.strategyCandidates) {
        spec.items.push_back({candidate, L""});
      }
      spec.okText = L"按选中的方式整合";
      spec.emptyItemDetail.clear();  // 这里没有第二栏，别补一句与远端无关的占位。
      spec.needSelectionHint = L"先在列表里点选一种整合方式，再按确定。（取消不会执行任何命令）";
      const RemoteChoiceLayoutHints hints{/*labelRows=*/4, /*contentWidth=*/560, /*listHeight=*/110};
      const std::optional<size_t> picked = host.PromptRemoteChoice(std::move(spec), hints);
      if (!picked.has_value() || *picked >= plan.strategyCandidates.size()) {
        Abandon(host, L"已取消整合方式的选择：" + fetchLeftover);
        return;
      }
      ComposeAndConfirmIntegrate(
          host, ctx, outcome,
          *picked == 0 ? git::PullStrategyChoice::chooseMerge : git::PullStrategyChoice::chooseRebase);
      return;
    }
    case git::PullPlanState::ready:
      break;
  }

  const bool proceed =
      plan.requiresForce
          ? host.RiskConfirm(L"pull 整合：风险确认", L"这份预检带有需要你自己核对的风险",
                             L"按这份预检继续整合", plan.confirmationText)
          : host.Confirm(L"pull 第二步：整合前请确认", plan.confirmationText, /*warningIcon=*/true);
  if (!proceed) {
    Abandon(host, L"已取消整合：" + fetchLeftover);
    return;
  }
  plan_ = plan;
  RequestExecutionRecheck(host, ctx);
}

void PullFlow::RequestExecutionRecheck(OperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::recheckProbe;
  platform::PullProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullProbeTimeoutMs;
  request.includeRelationship = false;
  worker_.Request(ctx.notifyWindow, kPullProbeCompleted, std::move(request),
                  [](const platform::PullProbeRequest& pending) {
                    return platform::RunPullProbeLoad(pending);
                  });
  host.SetStatus(L"点头之后、执行之前，再把预检那套只读查询原样重发一遍核对现状"
                 L"（分支 / HEAD / 远端跟踪引用 / 工作区）：对得上才执行那条整合命令…");
}

void PullFlow::HandleRecheck(OperationHost& host, const OperationContext& ctx,
                             const platform::PullProbeOutcome& outcome) {
  const std::wstring change = git::DescribePullChange(integrateFacts_.target, outcome.target);
  if (!change.empty()) {
    host.ShowWarning(L"执行前复核：仓库又变了", change);
    AbandonFlow();
    host.SetStatus(L"执行前复核发现现状与预检时不一致，因此没有发出整合命令。"
                   L"仓库状态正在重读，看清现状后如仍要 pull 请再点一次。");
    host.ScheduleRefresh();
    return;
  }
  LaunchIntegrate(host, ctx, plan_);
}

void PullFlow::LaunchIntegrate(OperationHost& host, const OperationContext& ctx,
                               const git::PullIntegratePlan& plan) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（pull 的第二步：整合；工作目录 " +
                        ctx.detection.root + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pullIntegrateOperation = true;
  // 操作历史：整合会移动 HEAD/分支并重写索引/工作区，是多提交合并/重放，不是单条能安全
  // 「挪回」的本地引用；恢复类别为 none，本程序不自动撤销。
  options.history.record = true;
  options.history.flow = app::HistoryFlow::pull;
  options.history.workTreeRoot = ctx.detection.root;
  options.history.operationLabel = plan.displayName;
  options.history.targetObjectId = plan.targetObjectId;
  options.history.restoreKind = app::HistoryRestoreKind::none;
  options.history.restoreNote =
      L"pull 的整合步会移动 HEAD/分支并改索引与工作区：那是合并或多提交重放，不是一条能安全"
      L"「挪回」的本地引用。本程序不自动撤销；若需要退回，请按分支 reflog 自行判断。";
  stage_ = Stage::integrating;
  const bool hadFetch = fetchAlreadyRan_;
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次整合没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。" +
                   std::wstring(hadFetch ? L"之前那次获取只更新了远端跟踪引用，本地没有被改动。"
                                         : L"本地没有任何改动。"));
    AbandonFlow();
  }
}

void PullFlow::BeginIntegrationFailedReport(OperationHost& host, const OperationContext& ctx,
                                            std::wstring baseConclusion,
                                            git::CommandCompletion completion, long exitCode,
                                            std::wstring environmentNotice) {
  // 现场读取走后台：这一步只把命令窗口的真实终态带着走——
  // 「没读回来之前不结案、也不请求重读」，避免结论与现状互相打架。
  stage_ = Stage::aftermathReport;
  reportBaseNote_ = std::move(baseConclusion);
  reportCompletion_ = completion;
  reportExitCode_ = exitCode;
  reportEnvironmentNotice_ = std::move(environmentNotice);

  platform::PullAftermathRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullRecheckTimeoutMs;
  aftermathWorker_.Request(ctx.notifyWindow, kPullAftermathCompleted, std::move(request),
                           [](const platform::PullAftermathRequest& pending) {
                             return platform::RunPullAftermathLoad(pending);
                           });
  host.SetStatus(L"pull 整合没有完成：正在后台把现场读回来（有没有流程停在进行中、哪些文件未合并、"
                 L"HEAD 现在在哪——全部只读），读回来后如实说明…");
}

void PullFlow::OnAftermathCompleted(OperationHost& host, uint64_t completionSerial) {
  platform::PullAftermath aftermath;
  if (!aftermathWorker_.FetchLatest(completionSerial, &aftermath)) {
    return;  // 重复或迟到的通知：同一份现场绝不消费两次。
  }
  if (stage_ != Stage::aftermathReport) {
    return;  // 这一次 pull 已按其它路径结案。
  }
  // 到这里这一次整合命令已有终态，现场读取只是给结论补充实况；无论读成什么样都要把结论收完。
  std::wstring text;
  text += L"命令窗口里的那次整合没有完成（" + std::wstring(git::CommandCompletionLabel(reportCompletion_));
  if (reportCompletion_ == git::CommandCompletion::finished) {
    // 只有 finished 携带 Git 真实给过的退出码；gitNotStarted 与未知形态没有数值可列，
    // 由状态标签本身说明情况。
    text += L"，Git 退出码 " + std::to_wstring(static_cast<long long>(reportExitCode_));
  }
  text += L"）。\n\n";
  text += L"当前流程：" +
          (aftermath.workflow.HasSpecialFlowInProgress()
               ? aftermath.workflow.SpecialFlowText()
               : std::wstring(L"没有 Git 流程停在进行中（这次整合没有留下未完成的流程）")) +
          L"\n";
  if (!aftermath.conflict.readOk) {
    text += L"未合并的文件：没能读回来（" +
            (aftermath.conflict.readFailure.empty() ? std::wstring(L"原因未知")
                                                    : aftermath.conflict.readFailure) +
            L"）。以命令窗口里 Git 的真实输出为准。\n";
  } else if (aftermath.conflict.conflictPaths.empty()) {
    text += L"未合并的文件：无（索引里没有未合并条目，这次失败不是留下冲突的那种失败）。\n";
  } else {
    text += L"未合并的文件共 " + std::to_wstring(aftermath.conflict.conflictPaths.size()) + L" 个：";
    const size_t shown = std::min(aftermath.conflict.conflictPaths.size(), size_t{8});
    for (size_t index = 0; index < shown; ++index) {
      text += L"\n  · " + aftermath.conflict.conflictPaths[index];
    }
    if (aftermath.conflict.conflictPaths.size() > shown) {
      text += L"\n  · …（其余 " +
              std::to_wstring(aftermath.conflict.conflictPaths.size() - shown) + L" 个见“未暂存的更改”）";
    }
    text += L"\n这些文件也正列在“未暂存的更改”里（重读完成后带「冲突」状态）。\n";
  }
  if (!aftermath.conflict.branchRef.empty()) {
    text += L"当前分支：" + aftermath.conflict.branchRef + L"\n";
  }
  if (!aftermath.conflict.headObjectId.empty()) {
    text += L"HEAD 现在在：" + git::ShortObjectId(aftermath.conflict.headObjectId) + L"\n";
  }
  text += L"\n本程序不会替你收尾：不会 abort、不会 reset、不会 continue，也不会选任何一方的内容——"
          L"这些决定属于你，命令窗口里留着 Git 的完整输出。\n";
  text += L"你可以继续那个流程（git merge --continue / git rebase --continue），"
          L"或按你自己的判断中止（git merge --abort / git rebase --abort）。";

  host.ShowWarning(L"pull 整合没有完成", text);

  std::wstring note = L"pull 整合未完成";
  if (aftermath.workflow.HasSpecialFlowInProgress()) {
    note += L"：仓库停在「" + aftermath.workflow.SpecialFlowText() + L"」";
  }
  if (!aftermath.conflict.conflictPaths.empty()) {
    note += L"，未合并 " + std::to_wstring(aftermath.conflict.conflictPaths.size()) + L" 个文件";
  }
  note += L"。本程序没有 abort/reset/continue，处理现场由你决定（详见刚才的说明框）。";

  // 拼接方式与既有界面一致：结论底稿后面直接续上现场结论，不加「｜」分隔。
  std::wstring conclusion = reportBaseNote_ + note;
  if (!reportEnvironmentNotice_.empty()) {
    // 集中环境策略移除过继承的重定向变量时，结论必须把这句话说完。
    conclusion += L"｜" + reportEnvironmentNotice_;
  }
  host.RememberOperationConclusion(conclusion);
  AbandonFlow();
  // 无论成功还是失败都要重读一次：失败的操作同样可能已经改动仓库
  // （合并留下冲突），只有退出码决定要不要报成功。
  host.ScheduleRefresh();
}

void PullFlow::AbandonFlow() {
  stage_ = Stage::none;
  integrateFacts_ = platform::PullProbeOutcome{};
  plan_ = git::PullIntegratePlan{};
  fetchAlreadyRan_ = false;
  reportBaseNote_.clear();
  reportEnvironmentNotice_.clear();
  reportExitCode_ = 0;
}

void PullFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  AbandonFlow();
  host.SetStatus(std::wstring(reason));
}

void PullFlow::BeginStop() {
  worker_.BeginStop();
  aftermathWorker_.BeginStop();
}

void PullFlow::JoinWorkers() {
  worker_.Join();
  aftermathWorker_.Join();
}

}  // namespace gc::ui
