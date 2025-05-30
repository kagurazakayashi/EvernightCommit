#include "ui/fetch_flow.h"

#include <string>
#include <utility>

#include "git/repository.h"
#include "ui/commands.h"

namespace gc::ui {

void FetchFlow::Start(OperationHost& host, const OperationContext& ctx) {
  probing_ = true;
  platform::FetchProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.timeoutMilliseconds = kFetchProbeTimeoutMs;
  worker_.Request(ctx.notify, kFetchProbeCompleted, std::move(request),
                  [](const platform::FetchProbeRequest& pending) {
                    return platform::RunFetchProbeLoad(pending);
                  });
  host.SetStatus(L"fetch 前先在后台只读询问：当前分支、这个分支配置的远端、仓库既有远端清单，"
                 L"外加会影响抓取范围的配置（prune／pruneTags／标签跟随与这个远端的 fetch 映射）"
                 L"——都是只读查询，不弹命令窗口、不接触任何远端；问回来后核对范围并给出抓取目标…");
}

void FetchFlow::OnProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                 uint64_t completionSerial) {
  platform::FetchProbeOutcome outcome;
  if (!worker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  if (!probing_) {
    return;  // 不是等中的那一次（例如已按「仓库切换」作废）。
  }
  probing_ = false;
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    // 预检是在旧仓库上跑的：那份远端清单对当前界面显示的仓库毫无意义。
    host.SetStatus(L"预检完成时仓库已经换掉，本次没有执行 fetch。请对现在的仓库重新点一次“fetch”。");
    return;
  }
  const std::wstring repositoryRoot = ctx.detection.root;
  git::FetchPlan plan = git::BuildFetchPlan(outcome.facts, repositoryRoot);

  if (plan.state == git::FetchPlanState::chooseRemote) {
    // 分支配置定不下目标：把既有远端一个个摆出来，目标（名字与 URL）必须看得见才谈得上「不猜」。
    platform::SingleChoiceRequest request;
    request.title = L"选择 fetch 的远端";
    request.label = plan.explanation;
    for (const git::FetchRemoteEntry& entry : plan.candidates) {
      request.items.push_back({entry.name, entry.fetchUrl});
    }
    request.okText = L"抓取选中的远端";
    request.hints.labelRows = 3;
    request.hints.contentWidth = 420;
    request.hints.listHeight = 140;
    const std::optional<size_t> picked = host.PromptSingleChoice(std::move(request));
    if (!picked.has_value() || *picked >= plan.candidates.size()) {
      host.SetStatus(L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。");
      return;
    }
    plan = git::ChooseFetchRemote(outcome.facts, plan.candidates[*picked].name, repositoryRoot);
  }

  if (plan.state != git::FetchPlanState::ready) {
    host.ShowInfo(L"现在不能 fetch",
                  L"没有打开命令窗口，也没有接触任何远端或改动仓库。\n\n" + plan.explanation);
    host.SetStatus(L"未执行 fetch。");
    return;
  }

  if (!host.Confirm(L"fetch 前请确认", plan.confirmationText, /*warningIcon=*/false)) {
    host.SetStatus(L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。");
    return;
  }
  Launch(host, ctx, plan);
}

void FetchFlow::Launch(OperationHost& host, const OperationContext& ctx, const git::FetchPlan& plan) {
  // 五个必需字段一律走统一装配点：参数漏传在这里首先是编译错误，不是运行期才发现的裸 git。
  // 参数按数组提交，不进任何 shell 字符串；目标只认名字，URL 由 Git 自己按配置解析。
  git::CommandWindowOperation operation = git::MakeCommandWindowOperation(
      plan.operationId, plan.displayName, ctx.gitExecutable, ctx.detection.root, plan.arguments);

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（目标：" + plan.remoteName +
                        L" @ " +
                        (plan.remoteUrl.empty() ? std::wstring(L"URL 未记录") : plan.remoteUrl) +
                        L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.fetchOperation = true;
  // 操作历史：fetch 的边界就是它不改的那些东西，因此没有「挪回」的对象，恢复类别为 none。
  options.history = app::MakeWriteHistoryCapture(app::HistoryFlow::fetch, ctx.detection.root,
                                                 plan.displayName);
  options.history.remoteName = plan.remoteName;
  options.history.restoreKind = app::HistoryRestoreKind::none;
  options.history.restoreNote =
      L"fetch 只更新远端跟踪引用（refs/remotes/ 下）、FETCH_HEAD 与对象库：不移动 HEAD、"
      L"不改本地分支/索引/工作区，没有需要「挪回」的本地引用。";
  if (!host.LaunchCommandWindow(operation, options)) {
    host.SetStatus(L"这次 fetch 没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
  }
}

void FetchFlow::BeginStop() {
  worker_.BeginStop();
}

void FetchFlow::JoinWorkers() {
  worker_.Join();
}

}  // namespace gc::ui
