#include "ui/push_flow.h"

#include <string>
#include <utility>
#include <vector>

#include "git/commit_history.h"
#include "git/repository.h"
#include "platform/windows/identity_prompt.h"
#include "platform/windows/remote_choice_dialog.h"
#include "ui/commands.h"

namespace gc::ui {
namespace {

// 首次推送向导里「要不要顺手把上游设好」那两个选项：单选列表框的既有形态（名字 + 一句说明），
// 与 pull 选整合方式、fetch 选远端用的是同一个框——不再造第二个选择界面。
constexpr std::wstring_view kUpstreamChoiceOnlyPush = L"只推送，不改本地配置";
constexpr std::wstring_view kUpstreamChoiceOnlyPushDetail =
    L"推送之后这条分支仍然没有上游：普通推送与 pull 会继续拒绝它，配置文件一个字不写。";
constexpr std::wstring_view kUpstreamChoiceSetUpstream = L"推送成功后设置这条分支的上游";
constexpr std::wstring_view kUpstreamChoiceSetUpstreamDetail =
    L"两条本地配置各自一条命令、各自报告结果（键与值在下一步的确认框里逐条列出）。";

}  // namespace

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
                 L"目标分支一起摆给你确认；这条分支还没有上游而仓库里有可用远端时，改走首次推送…");
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

void PushFlow::OnFirstPushTargetsCompleted(OperationHost& host, const OperationContext& ctx,
                                           uint64_t completionSerial) {
  platform::FirstPushTargetsOutcome outcome;
  if (!targetsWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 更晚一次的询问取代了它，或这一次已经作废。
  }
  if (stage_ != Stage::firstPushTargets) {
    return;
  }
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    Abandon(host, L"远端清单问回来时仓库已经换掉，这次没有执行任何命令。请对现在的仓库重新点「推送」。");
    return;
  }
  HandleFirstPushTargets(host, ctx, outcome);
}

void PushFlow::OnFirstPushProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                         uint64_t completionSerial) {
  platform::FirstPushProbeOutcome outcome;
  if (!firstWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 期间又发起了更晚的预检/复核，这份结果不再有意义。
  }
  const Stage stage = stage_;
  if (stage != Stage::firstPushProbe && stage != Stage::firstPushRecheck) {
    return;
  }
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    Abandon(host, L"目标预检回来时仓库已经换掉，这次没有执行任何命令、没有写任何配置。"
                  L"请对现在的仓库重新点一次「推送」。");
    return;
  }
  if (stage == Stage::firstPushProbe) {
    HandleFirstPushProbe(host, ctx, outcome);
  } else {
    HandleFirstPushRecheck(host, ctx, outcome);
  }
}

void PushFlow::HandleProbe(OperationHost& host, const OperationContext& ctx,
                           const platform::PushProbeOutcome& outcome) {
  const git::PushPlan plan = git::BuildPushPlan(outcome.facts, ctx.detection.root);
  if (plan.state == git::PushPlanState::blocked) {
    // 「这条分支还没有上游」是唯一一种可以由用户当场补齐前提的拒绝：仓库里有配好地址的
    // 远端时改走首次推送，让目标由用户选出来，而不是本程序猜一个。其余拒绝照原文展示。
    if (git::CanOfferFirstPush(outcome.facts)) {
      StartFirstPushTargets(host, ctx, outcome.facts);
      return;
    }
    host.ShowInfo(L"现在不能推送", plan.explanation);
    Abandon(host, L"未执行推送：前提不成立（原因见刚才的说明框）。没有打开命令窗口，"
                 L"也没有接触任何远端、没有改动仓库、没有写任何配置。");
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
  LaunchRequest request;
  request.operationId = plan_.operationId;
  request.displayName = plan_.displayName;
  request.commandLabel = plan_.commandLabel;
  request.argumentList = plan_.arguments;
  request.scopeNotice = plan_.notice;
  request.pushedObjectId = plan_.pushedObjectId;
  request.remoteName = plan_.remoteName;
  request.remoteBranchRef = plan_.remoteBranchRef;
  request.pushUrls = plan_.pushUrls;
  Launch(host, ctx, request);
}

// ---- 首次推送：候选远端 ----

void PushFlow::StartFirstPushTargets(OperationHost& host, const OperationContext& ctx,
                                     const git::PushPreflightFacts& baseFacts) {
  stage_ = Stage::firstPushTargets;
  firstPushBase_ = baseFacts;
  platform::FirstPushTargetsRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.timeoutMilliseconds = kFirstPushTargetsTimeoutMs;
  targetsWorker_.Request(ctx.notifyWindow, kFirstPushTargetsCompleted, std::move(request),
                         [](const platform::FirstPushTargetsRequest& pending) {
                           return platform::RunFirstPushTargetsLoad(pending);
                         });
  host.SetStatus(L"这条分支还没有上游，改走首次推送：先在后台逐个远端问出「实际会推到哪个地址」"
                 L"（git remote get-url --push --all，本地只读、不接触远端），"
                 L"然后把候选摆出来让你选目标远端与目标分支…");
}

void PushFlow::HandleFirstPushTargets(OperationHost& host, const OperationContext& ctx,
                                      const platform::FirstPushTargetsOutcome& outcome) {
  std::vector<git::FirstPushRemoteCandidate> selectable;
  for (const git::FirstPushRemoteCandidate& candidate : outcome.candidates.candidates) {
    if (candidate.selectable()) {
      selectable.push_back(candidate);
    }
  }
  if (selectable.empty()) {
    host.ShowInfo(L"现在不能首次推送", git::DescribeFirstPushCandidateRefusal(outcome.candidates));
    Abandon(host, L"未执行首次推送：一个可选的目标远端都没摆出来（原因见刚才的说明框）。"
                  L"没有打开命令窗口，没有接触任何远端，没有改动仓库，也没有写任何配置。");
    return;
  }

  // 第一步：选目标远端。分支与那份完整提交 ID 先摆在说明里——「推哪一份」必须先看得见，
  // 才谈得上「推到哪里」。
  platform::RemoteChoiceSpec spec;
  spec.title = L"首次推送：选择目标远端";
  spec.label =
      L"分支 " + firstPushBase_.branchName + L" 还没有上游。这次要把 " +
      git::ShortObjectId(firstPushBase_.headObjectId) + L"（完整 ID " + firstPushBase_.headObjectId +
      L"）首次推送到下面某个远端。列表里每个远端后面的地址都是 Git 自己展开的实际发布地址"
      L"（pushurl 优先、insteadOf 改写已叠好）。取消不会执行任何命令、不会接触远端、不会写配置。";
  spec.okText = L"推到这个远端";
  spec.cancelText = L"取消";
  spec.emptyItemDetail = L"（这个远端没能问出发布地址）";
  spec.needSelectionHint = L"先在列表里点选一个远端，再按确定。（取消不会执行任何命令）";
  for (const git::FirstPushRemoteCandidate& candidate : selectable) {
    spec.items.push_back({candidate.name, candidate.urlsDisplay()});
  }
  const RemoteChoiceLayoutHints hints{/*labelRows=*/4, /*contentWidth=*/460, /*listHeight=*/160};
  const std::optional<size_t> picked = host.PromptRemoteChoice(std::move(spec), hints);
  if (!picked.has_value() || *picked >= selectable.size()) {
    Abandon(host, L"已取消：没有选目标远端，因此没有打开命令窗口、没有接触任何远端、没有写任何配置。");
    return;
  }
  firstPushRemoteName_ = selectable[*picked].name;

  // 第二步：目标分支名。建议值是当前分支名，但**不替用户按下确定**——
  // 这个名字最终由 Git 的 check-ref-format 裁定，输入框里只做纯形态提示（不能在这里等子进程）。
  platform::IdentityPromptSpec namePrompt;
  namePrompt.title = L"首次推送：目标分支名";
  namePrompt.label = L"要把这条分支推到远端「" + firstPushRemoteName_ +
                     L"」的哪个分支名？默认用本地分支名，改成别的也可以。"
                     L"这个名字会交给 git check-ref-format 当场裁定，不合格就不发任何命令。";
  namePrompt.initialValue = firstPushBase_.branchName;
  namePrompt.okText = L"就用这个分支名";
  namePrompt.cancelText = L"取消";
  namePrompt.validate = [](const std::wstring& value) {
    return git::DescribeFirstPushBranchNameProblem(value);
  };
  const TextInputLayoutHints nameHints{/*labelRows=*/3, /*noteRows=*/2, /*contentWidth=*/460};
  const std::optional<std::wstring> entered = host.PromptForText(std::move(namePrompt), nameHints);
  if (!entered.has_value()) {
    Abandon(host, L"已取消：没有输入目标分支名，因此没有打开命令窗口、没有接触任何远端、"
                  L"没有写任何配置。");
    return;
  }
  firstPushBranchName_ = *entered;

  // 第三步：要不要设上游。「推送」与「写本地配置」是两件事，这里就问清楚，
  // 后面确认框里也会分别列出。
  // 选项数在本地记一份：spec 是按值交给对话框的（移动之后不能再用它的 items 做上界判定）。
  constexpr size_t kUpstreamChoiceCount = 2;
  platform::RemoteChoiceSpec upstreamSpec;
  upstreamSpec.title = L"首次推送：要不要设置上游";
  upstreamSpec.label = L"推送到远端「" + firstPushRemoteName_ + L"」的 " +
                       git::ShortObjectId(firstPushBase_.headObjectId) + L" → refs/heads/" +
                       firstPushBranchName_ +
                       L"。这条分支现在没有上游：要不要在推送成功之后，把上游写进这个仓库的本地配置？";
  upstreamSpec.okText = L"按选中的做";
  upstreamSpec.cancelText = L"取消";
  upstreamSpec.items.push_back({std::wstring(kUpstreamChoiceOnlyPush),
                                std::wstring(kUpstreamChoiceOnlyPushDetail)});
  upstreamSpec.items.push_back({std::wstring(kUpstreamChoiceSetUpstream),
                                std::wstring(kUpstreamChoiceSetUpstreamDetail)});
  const RemoteChoiceLayoutHints upstreamHints{/*labelRows=*/3, /*contentWidth=*/460, /*listHeight=*/90};
  const std::optional<size_t> upstreamChoice =
      host.PromptRemoteChoice(std::move(upstreamSpec), upstreamHints);
  if (!upstreamChoice.has_value() || *upstreamChoice >= kUpstreamChoiceCount) {
    Abandon(host, L"已取消：没有打开命令窗口、没有接触任何远端、没有写任何配置。");
    return;
  }
  firstPushSetUpstream_ = *upstreamChoice == 1;
  firstPushActive_ = true;
  StartFirstPushProbe(host, ctx, /*forRecheck=*/false);
}

// ---- 首次推送：目标预检、确认、复核 ----

void PushFlow::StartFirstPushProbe(OperationHost& host, const OperationContext& ctx, bool forRecheck) {
  stage_ = forRecheck ? Stage::firstPushRecheck : Stage::firstPushProbe;
  platform::FirstPushProbeRequest request;
  request.exePath = ctx.gitExecutable;
  // 复核阶段仍然绑在预检时那个仓库的根上：中途界面换了仓库，这份复核就不属于刚才那份方案了
  // （回来时会和当前界面比对，不一致直接作废）。
  request.repositoryDirectory = ctx.detection.root;
  request.remoteName = firstPushRemoteName_;
  request.targetBranchRef = git::BuildFirstPushTargetBranchRef(firstPushBranchName_);
  request.timeoutMilliseconds = kFirstPushProbeTimeoutMs;
  firstWorker_.Request(ctx.notifyWindow, kFirstPushProbeCompleted, std::move(request),
                       [](const platform::FirstPushProbeRequest& pending) {
                         return platform::RunFirstPushProbeLoad(pending);
                       });
  host.SetStatus(
      forRecheck
          ? std::wstring(L"点头之后、执行之前，把选定目标那次只读查询原样重发一遍：分支 / 那份提交 / "
                         L"上游还是没有 / 引用名的 Git 裁定 / 发布地址 / 对端那条引用现在在哪，"
                         L"全对得上才发命令…")
          : std::wstring(L"正在按选定的目标做只读预检：先问 Git 这个引用名能不能用，再向每一个发布地址"
                         L"问一次「那条引用在不在、停在哪」（这一步是只读 ls-remote，**会访问远端**，"
                         L"需要认证时按你自己的凭据方式处理，可能要等一会儿）…"));
}

void PushFlow::HandleFirstPushProbe(OperationHost& host, const OperationContext& ctx,
                                    const platform::FirstPushProbeOutcome& outcome) {
  const git::FirstPushPlan plan = git::BuildFirstPushPlan(
      {outcome.facts, ctx.detection.root, firstPushSetUpstream_});
  if (plan.state == git::FirstPushPlanState::blocked) {
    host.ShowInfo(L"现在不能首次推送", plan.explanation);
    Abandon(host, L"未执行首次推送：前提不成立（原因见刚才的说明框）。没有打开命令窗口，"
                  L"没有接触远端（除刚才那一步只读询问），没有改动仓库，也没有写任何配置。");
    return;
  }
  const bool proceed =
      plan.requiresForce
          ? host.RiskConfirm(L"首次推送：风险确认", L"这份预检带有需要你自己核对的风险",
                             L"仍要按这份预检推送", plan.confirmationText)
          : host.Confirm(L"首次推送前请确认", plan.confirmationText, /*warningIcon=*/true);
  if (!proceed) {
    Abandon(host, L"已取消：没有打开命令窗口，没有接触远端，也没有写任何配置——"
                  L"包括那两条上游设置，一个字都没写。");
    return;
  }
  firstPreflight_ = outcome;  // 复核要拿它当「预检时的那份现状」。
  firstPlan_ = plan;
  StartFirstPushProbe(host, ctx, /*forRecheck=*/true);
}

void PushFlow::HandleFirstPushRecheck(OperationHost& host, const OperationContext& ctx,
                                      const platform::FirstPushProbeOutcome& outcome) {
  const std::wstring change = git::DescribeFirstPushChange(firstPreflight_.facts, outcome.facts);
  if (!change.empty()) {
    host.ShowWarning(L"执行前复核：仓库或远端又变了", change);
    stage_ = Stage::none;
    firstPreflight_ = platform::FirstPushProbeOutcome{};
    firstPlan_ = git::FirstPushPlan{};
    firstPushActive_ = false;
    host.SetStatus(L"执行前复核与预检对不上，因此没有发出任何命令，也没有写任何配置。"
                   L"仓库状态正在重读，看清现状后如仍要推送请再点一次。");
    host.ScheduleRefresh();
    return;
  }
  LaunchRequest request;
  request.operationId = firstPlan_.operationId;
  request.displayName = firstPlan_.displayName;
  request.commandLabel = firstPlan_.commandLabel;
  request.argumentList = firstPlan_.arguments;
  request.scopeNotice = firstPlan_.notice;
  request.pushedObjectId = firstPlan_.pushedObjectId;
  request.remoteName = firstPlan_.remoteName;
  request.remoteBranchRef = firstPlan_.targetBranchRef;
  request.pushUrls = firstPlan_.pushUrls;
  Launch(host, ctx, request);
}

// ---- 命令窗口里的推送 ----

void PushFlow::Launch(OperationHost& host, const OperationContext& ctx,
                      const LaunchRequest& request) {
  git::CommandWindowOperation operation;
  operation.operationId = request.operationId;
  operation.displayName = request.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  operation.repositoryDirectory = ctx.detection.root;
  // 参数按数组提交，不进任何 shell 字符串：命令源侧是复核确认过的完整提交 ID，目标远端只认名字，
  // URL 不进命令行（由 Git 自己按配置解析，界面展示与核实用的那份是 get-url --push --all 的展开
  // 回答），因此这条链路上都不会出现凭据。
  operation.arguments = request.argumentList;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + request.commandLabel + L"（推送 " +
                        git::ShortObjectId(request.pushedObjectId) + L" → 远端「" +
                        request.remoteName + L"」的 " + request.remoteBranchRef +
                        L"），等待 Git 退出码…";
  options.scopeNotice = request.scopeNotice;
  options.pushOperation = true;
  stage_ = Stage::pushing;
  binding_ = request;  // 核实要问的那一份：与刚才确认的完全同一批地点与那一份提交。
  boundRepositoryDirectory_ = ctx.detection.root;  // 核实与上游写入都认这一个仓库根。
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次推送没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                   L"远端没有被接触，本地也没有任何改动。");
    stage_ = Stage::none;
    preflight_ = platform::PushProbeOutcome{};
    firstPreflight_ = platform::FirstPushProbeOutcome{};
    plan_ = git::PushPlan{};
    firstPlan_ = git::FirstPushPlan{};
    firstPushActive_ = false;
    boundRepositoryDirectory_.clear();
  }
}

void PushFlow::BeginVerification(OperationHost& host, const OperationContext& ctx,
                                 bool commandSucceeded, std::wstring_view conclusion) {
  stage_ = Stage::verifying;
  upstreamPushConclusion_ = std::wstring(conclusion);
  // 上游写入的前提有两个：用户当场选了「要设」，而且推送那一步报告成功。
  // 推送失败时一条配置都不写——那种场合远端根本没收到这条分支，写上游只会把分支指向一个
  // 并不存在的东西；也不自动重推，是否再来一次由用户决定。
  upstreamPending_ = firstPushActive_ && commandSucceeded && !firstPlan_.upstreamSteps.empty();
  platform::PushVerifyRequest request;
  request.exePath = ctx.gitExecutable;
  // 工作目录取命令窗口那条 push 当时绑定的仓库根，而不是界面此刻显示的根：点头到终态之间
  // 用户可能已经切了仓库，但这次核实问的始终是刚才那一条推送的去向。
  request.repositoryDirectory = boundRepositoryDirectory_;
  request.remoteBranchRef = binding_.remoteBranchRef;
  request.expectedObjectId = binding_.pushedObjectId;
  request.pushUrls = binding_.pushUrls;
  request.pushCommandSucceeded = commandSucceeded;
  request.commandConclusion = std::wstring(conclusion);
  request.timeoutMilliseconds = kPushVerifyTimeoutMs;
  verifyWorker_.Request(ctx.notifyWindow, kPushVerifyCompleted, std::move(request),
                        [](const platform::PushVerifyRequest& pending) {
                          return platform::RunPushVerifyLoad(pending);
                        });
  host.SetStatus(L"正在向确认框上列出的那些**发布目标**发只读 ls-remote，核对那条引用到底停在"
                 L"哪一份提交（这一步要按发布目标的地址问，不是按本地某个远端跟踪引用；"
                 L"需要认证时由 Git 自己的方式处理，可能要等一会儿）…" +
                 (upstreamPending_ ? std::wstring(L"核实结束之后才轮到上游那两条配置命令。")
                                   : std::wstring()));
}

void PushFlow::OnVerifyCompleted(OperationHost& host, const OperationContext& ctx,
                                 uint64_t completionSerial) {
  platform::PushVerifyOutcome outcome;
  if (!verifyWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 更晚一次的核实已经取代了它（或这一次已经结案）。
  }
  if (stage_ != Stage::verifying) {
    return;
  }

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

  // 核实与写上游互不影响：核实只说「对端现在在哪」，写不写上游由用户当场那个决定说了算，
  // 而且只看推送那一步的成败。所以核实的结论无论哪一档，该写的照写、该跳过的照跳过。
  if (upstreamPending_) {
    upstreamPending_ = false;
    stage_ = Stage::writingUpstream;
    LaunchUpstreamStep(host, ctx, 0);
    return;
  }
  stage_ = Stage::none;
  preflight_ = platform::PushProbeOutcome{};
  firstPreflight_ = platform::FirstPushProbeOutcome{};
  plan_ = git::PushPlan{};
  firstPlan_ = git::FirstPushPlan{};
  firstPushActive_ = false;
}

void PushFlow::LaunchUpstreamStep(OperationHost& host, const OperationContext& ctx, size_t index) {
  if (index >= firstPlan_.upstreamSteps.size()) {
    FinishUpstreamWrites(host, L"上游设置：没有需要执行的配置命令。");
    return;
  }
  const git::UpstreamWriteStep& step = firstPlan_.upstreamSteps[index];
  upstreamStep_ = index + 1;

  git::CommandWindowOperation operation;
  operation.operationId = step.operationId;
  operation.displayName = step.displayName;
  operation.gitExecutable = ctx.gitExecutable;
  // 仍然绑在这次推送那个仓库根上：中途界面换了仓库，剩下的配置命令就不该发给另一个仓库。
  operation.repositoryDirectory = boundRepositoryDirectory_;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + step.commandLabel + L"（上游设置第 " +
                        std::to_wstring(index + 1) + L"/" +
                        std::to_wstring(firstPlan_.upstreamSteps.size()) + L" 条），等待 Git 退出码…";
  options.scopeNotice = L"这一步只写这一把配置键：" + step.key + L" = " + step.value +
                        L"。只影响本地分支 " + firstPlan_.branchName +
                        L" 的上游记录，不动别的配置、不动分支与工作区，也不接触远端。";
  options.upstreamWriteStep = static_cast<int>(index + 1);
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"上游设置这一步没能启动：命令窗口未能打开或启动失败（原因见上一行）。"
                   L"远端与本地配置都没有被这次操作改动——那条推送本身的结果以它自己的核实为准。");
    stage_ = Stage::none;
    firstPreflight_ = platform::FirstPushProbeOutcome{};
    firstPlan_ = git::FirstPushPlan{};
    firstPushActive_ = false;
    upstreamStep_ = 0;
  }
}

void PushFlow::OnUpstreamStepSettled(OperationHost& host, const OperationContext& ctx, int step,
                                     bool succeeded, std::wstring_view conclusion) {
  if (stage_ != Stage::writingUpstream || upstreamStep_ == 0 ||
      static_cast<size_t>(step) != upstreamStep_) {
    return;  // 迟到的、或不属于这一次操作的结果。
  }
  if (!git::PathsEqualFolded(boundRepositoryDirectory_, ctx.detection.root)) {
    host.SetStatus(L"上游设置停在第 " + std::to_wstring(upstreamStep_) + L" 条：界面上的仓库已经换掉，"
                   L"剩下的配置命令没有发出。已写入的那几条原样留着，本程序不自动回退、不自动重发。\n"
                   L" · 刚才那一步（第 " + std::to_wstring(upstreamStep_) + L" 条）在命令窗口里的结论：「" +
                   std::wstring(conclusion) + L"」\n"
                   L" · 推送那一步的结论：「" + upstreamPushConclusion_ + L"」");
    stage_ = Stage::none;
    firstPreflight_ = platform::FirstPushProbeOutcome{};
    firstPlan_ = git::FirstPushPlan{};
    firstPushActive_ = false;
    upstreamStep_ = 0;
    return;
  }

  const size_t done = upstreamStep_ - 1;
  if (!succeeded) {
    const git::UpstreamWriteStep& failed = firstPlan_.upstreamSteps[done];
    std::wstring text = L"推送与上游设置要分开说：\n";
    text += L" · 推送那一步：命令窗口的结论是「" + upstreamPushConclusion_ + L"」，"
            L"对端实况见上一行状态与刚才的核实结论。\n";
    text += L" · 上游设置：第 " + std::to_wstring(upstreamStep_) + L" 条没有写成（" +
            failed.commandLabel + L"，命令窗口那头的结论是「" + std::wstring(conclusion) +
            L"」）。\n";
    for (size_t index = 0; index < done; ++index) {
      text += L" · 第 " + std::to_wstring(index + 1) + L" 条已经写成：" +
              firstPlan_.upstreamSteps[index].conclusion + L"\n";
    }
    text += L" · 剩下的 " + std::to_wstring(firstPlan_.upstreamSteps.size() - upstreamStep_) +
            L" 条没有发出：那种场合配置只写了一半，本程序不把它当成「上游已经设好」，"
            L"也不自动重试。请对照命令窗口里那条命令的输出自己决定要不要再跑一次。";
    stage_ = Stage::none;
    firstPreflight_ = platform::FirstPushProbeOutcome{};
    firstPlan_ = git::FirstPushPlan{};
    firstPushActive_ = false;
    upstreamStep_ = 0;
    host.ShowWarning(L"上游设置没有完成", text);
    host.SetStatus(text);
    return;
  }

  if (upstreamStep_ < firstPlan_.upstreamSteps.size()) {
    LaunchUpstreamStep(host, ctx, upstreamStep_);
    return;
  }
  std::wstring summary = L"首次推送完成：推送那一步的结论是「" + upstreamPushConclusion_ +
                         L"」，上游设置的两条配置都已写成：\n";
  for (const git::UpstreamWriteStep& writeStep : firstPlan_.upstreamSteps) {
    summary += L" · " + writeStep.conclusion + L"\n";
  }
  summary += L"这条分支现在有了上游；下一次点「推送」走的是普通推送（目标由仓库配置定）。";
  FinishUpstreamWrites(host, summary);
}

void PushFlow::FinishUpstreamWrites(OperationHost& host, std::wstring_view summary) {
  stage_ = Stage::none;
  firstPreflight_ = platform::FirstPushProbeOutcome{};
  firstPlan_ = git::FirstPushPlan{};
  firstPushActive_ = false;
  upstreamStep_ = 0;
  host.SetStatus(std::wstring(summary));
}

void PushFlow::AbandonFlow() {
  stage_ = Stage::none;
  preflight_ = platform::PushProbeOutcome{};
  firstPreflight_ = platform::FirstPushProbeOutcome{};
  plan_ = git::PushPlan{};
  firstPlan_ = git::FirstPushPlan{};
  firstPushActive_ = false;
  upstreamPending_ = false;
  upstreamStep_ = 0;
  upstreamPushConclusion_.clear();
  firstPushRemoteName_.clear();
  firstPushBranchName_.clear();
  binding_ = LaunchRequest{};
  boundRepositoryDirectory_.clear();
}

void PushFlow::Abandon(OperationHost& host, std::wstring_view reason) {
  AbandonFlow();
  host.SetStatus(std::wstring(reason));
}

void PushFlow::BeginStop() {
  worker_.BeginStop();
  verifyWorker_.BeginStop();
  targetsWorker_.BeginStop();
  firstWorker_.BeginStop();
}

void PushFlow::JoinWorkers() {
  worker_.Join();
  verifyWorker_.Join();
  targetsWorker_.Join();
  firstWorker_.Join();
}

}  // namespace gc::ui
