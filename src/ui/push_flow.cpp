#include "ui/push_flow.h"

#include <string>
#include <utility>

#include "git/commit_history.h"
#include "git/repository.h"
#include "ui/commands.h"

namespace gc::ui {

void PushFlow::Start(OperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::probe;
  platform::PushProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kPushProbeTimeoutMs;
  worker_.Request(ctx.notifyWindow, kPushProbeCompleted, std::move(request),
                  [](const platform::PushProbeRequest& pending) {
                    return platform::RunPushProbeLoad(pending);
                  });
  host.SetStatus(L"推送前先在后台只读问清：在哪个分支、要推哪一份提交、上游是谁、"
                 L"这次实际会推给哪个远端的哪个地址、本地相对上一次抓取领先几个"
                 L"（只读查询，不弹命令窗口、不接触任何远端），问回来后把源分支 / 目标远端 / "
                 L"目标分支一起摆给你确认…");
}

void PushFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                uint64_t completionSerial) {
  platform::PushProbeOutcome outcome;
  if (!worker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  const Stage stage = stage_;
  if (stage != Stage::probe && stage != Stage::recheckProbe) {
    return;  // 这一次推送已经按「取消 / 结案」作废，迟到的结果原样丢掉。
  }
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    Abandon(host, L"预检完成时仓库已经换掉，这次推送没有执行任何命令。请对现在的仓库重新点一次「推送」。");
    return;
  }
  if (stage == Stage::probe) {
    HandleProbe(host, ctx, outcome);
  } else {
    HandleRecheck(host, ctx, outcome);
  }
}

void PushFlow::HandleProbe(OperationHost& host, const OperationContext& ctx,
                           const platform::PushProbeOutcome& outcome) {
  const git::PushPlan plan = git::BuildPushPlan(outcome.facts, ctx.detection.root);
  if (plan.state == git::PushPlanState::blocked) {
    host.ShowInfo(L"现在不能推送", plan.explanation);
    Abandon(host, L"未执行推送：前提不成立（原因见刚才的说明框）。没有打开命令窗口，"
                 L"也没有接触任何远端、没有改动仓库。");
    return;
  }
  const bool proceed =
      plan.requiresForce
          ? host.RiskConfirm(L"推送：风险确认", L"这份预检带有需要你自己核对的风险",
                             L"仍要按这份预检推送", plan.confirmationText)
          : host.Confirm(L"推送前请确认", plan.confirmationText, /*warningIcon=*/true);
  if (!proceed) {
    Abandon(host, L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。");
    return;
  }
  preflight_ = outcome;  // 复核要拿它当「预检时的那份现状」。
  plan_ = plan;
  RequestExecutionRecheck(host, ctx);
}

void PushFlow::RequestExecutionRecheck(OperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::recheckProbe;
  platform::PushProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.absoluteGitDir = ctx.detection.absoluteGitDir;
  request.timeoutMilliseconds = kPushProbeTimeoutMs;
  worker_.Request(ctx.notifyWindow, kPushProbeCompleted, std::move(request),
                  [](const platform::PushProbeRequest& pending) {
                    return platform::RunPushProbeLoad(pending);
                  });
  host.SetStatus(L"点头之后、执行之前，再把预检那套只读查询原样重发一遍：分支 / 要推的那一份提交 / "
                 L"上游 / 发布目标都对得上，才发出那条 push…");
}

void PushFlow::HandleRecheck(OperationHost& host, const OperationContext& ctx,
                             const platform::PushProbeOutcome& outcome) {
  const std::wstring change = git::DescribePushChange(preflight_.facts, outcome.facts);
  if (!change.empty()) {
    host.ShowWarning(L"执行前复核：仓库又变了", change);
    stage_ = Stage::none;
    preflight_ = platform::PushProbeOutcome{};
    plan_ = git::PushPlan{};
    host.SetStatus(L"执行前复核发现现状与预检时不一致，因此没有发出推送命令。"
                   L"仓库状态正在重读，看清现状后如仍要推送请再点一次。");
    host.ScheduleRefresh();
    return;
  }
  Launch(host, ctx, plan_);
}

void PushFlow::Launch(OperationHost& host, const OperationContext& ctx, const git::PushPlan& plan) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  // 参数按数组提交，不进任何 shell 字符串：命令源侧是复核确认过的完整提交 ID，目标远端只认名字，
  // URL 不进命令行（由 Git 自己按配置解析，界面展示与核实用的那份是 get-url --push --all 的展开
  // 回答），因此这条链路上都不会出现凭据。
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（推送 " +
                        git::ShortObjectId(plan.pushedObjectId) + L" → 远端「" + plan.remoteName +
                        L"」的 " + plan.remoteBranchRef + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pushOperation = true;
  stage_ = Stage::pushing;
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次推送没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                   L"远端没有被接触，本地也没有任何改动。");
    stage_ = Stage::none;
    preflight_ = platform::PushProbeOutcome{};
    plan_ = git::PushPlan{};
  }
}

void PushFlow::BeginVerification(OperationHost& host, const OperationContext& ctx,
                                 bool commandSucceeded, std::wstring_view conclusion) {
  stage_ = Stage::verifying;
  platform::PushVerifyRequest request;
  request.exePath = ctx.gitExecutable;
  // 工作目录取预检时那个仓库的根，而不是界面此刻显示的根：点头到终态之间用户可能已经切了仓库，
  // 但这次核实问的始终是刚才那一条推送的去向。
  request.repositoryDirectory = preflight_.repositoryDirectory;
  request.remoteBranchRef = plan_.remoteBranchRef;
  request.expectedObjectId = plan_.pushedObjectId;
  request.pushUrls = plan_.pushUrls;
  request.pushCommandSucceeded = commandSucceeded;
  request.commandConclusion = std::wstring(conclusion);
  request.timeoutMilliseconds = kPushVerifyTimeoutMs;
  verifyWorker_.Request(ctx.notifyWindow, kPushVerifyCompleted, std::move(request),
                        [](const platform::PushVerifyRequest& pending) {
                          return platform::RunPushVerifyLoad(pending);
                        });
  host.SetStatus(L"正在向确认框上列出的那些**发布目标**发只读 ls-remote，核对那条引用到底停在"
                 L"哪一份提交（这一步要按发布目标的地址问，不是按本地那个远端跟踪引用；"
                 L"需要认证时由 Git 自己的方式处理，可能要等一会儿）…");
}

void PushFlow::OnVerifyCompleted(OperationHost& host, uint64_t completionSerial) {
  platform::PushVerifyOutcome outcome;
  if (!verifyWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 更晚一次的核实已经取代了它（或这一次已经结案）。
  }
  if (stage_ != Stage::verifying) {
    return;
  }
  stage_ = Stage::none;
  preflight_ = platform::PushProbeOutcome{};
  plan_ = git::PushPlan{};

  const git::PushVerificationReport& report = outcome.report;
  std::wstring text = report.headline;
  for (const std::wstring& line : report.lines) {
    text += L"\n" + line;
  }
  host.SetStatus(text);

  // 「Git 说成功了却没核实上」与「核实到的位置和推出去的那一份不是一个东西」必须当面讲清楚，
  // 状态栏那行会被后续刷新挤掉。全都对得上的场合不再多弹一次窗。
  const bool needsDialog =
      report.verdict == git::PushVerificationVerdict::mismatched ||
      (outcome.pushCommandSucceeded && report.verdict != git::PushVerificationVerdict::confirmed);
  if (needsDialog) {
    if (outcome.pushCommandSucceeded) {
      host.ShowWarning(L"推送结果与发布目标的实况", text);
    } else {
      host.ShowInfo(L"推送结果与发布目标的实况", text);
    }
  }
}

void PushFlow::AbandonFlow() {
  stage_ = Stage::none;
  preflight_ = platform::PushProbeOutcome{};
  plan_ = git::PushPlan{};
}

void PushFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  AbandonFlow();
  host.SetStatus(std::wstring(reason));
}

void PushFlow::BeginStop() {
  worker_.BeginStop();
  verifyWorker_.BeginStop();
}

void PushFlow::JoinWorkers() {
  worker_.Join();
  verifyWorker_.Join();
}

}  // namespace gc::ui
