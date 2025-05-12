#include "ui/submodule_flow.h"

#include <string>
#include <utility>

#include "git/diff_view.h"
#include "git/repository.h"
#include "ui/commands.h"

namespace gc::ui {
namespace {

// 仓库键：与「表单属于哪个工作区」用的同一个判据（只按工作区根，换 Git 程序不算换仓库）。
std::wstring RepositoryKey(const OperationContext& ctx) {
  if (ctx.detection.root.empty()) {
    return std::wstring();
  }
  return git::CanonicalPathKey(ctx.detection.root);
}

std::wstring HeldKeyOf(const git::RepoDetection& detection) {
  return detection.root.empty() ? std::wstring() : git::CanonicalPathKey(detection.root);
}

}  // namespace

app::HeldForm SubmoduleFlow::CaptureHeld(CommitOperationHost& host,
                                         const OperationContext& ctx) const {
  app::HeldForm held;
  held.repositoryKey = HeldKeyOf(ctx.detection);
  held.repositoryRoot = ctx.detection.root;
  const CommitFormSnapshot snapshot = host.CaptureCommitForm();
  held.form = snapshot.data;
  held.authorWall = snapshot.authorWall;
  held.committerWall = snapshot.committerWall;
  held.timesSynced = snapshot.timesSynced;
  held.timesUserEdited = host.CommitTimesUserEdited();
  held.hasUserContent = host.CommitFormHasUserContent();
  return held;
}

void SubmoduleFlow::Enter(CommitOperationHost& host, const OperationContext& ctx,
                          const git::ChangeItem& item) {
  if (stage_ != Stage::none) {
    host.SetStatus(L"已经有一次子模块导航在走（探测或切换还没完成），再点不会排队。"
                   L"请等那一步结束。");
    return;
  }
  if (!ctx.repoUsable || ctx.detection.root.empty()) {
    host.SetStatus(L"现在没有可用的父仓库工作区，没有「从哪儿进去」这回事。请先点“刷新”。");
    return;
  }
  if (item.kind != git::ChangeKind::submodule) {
    host.SetStatus(L"选中的这一条不是子模块条目，因此没有进入任何仓库。"
                   L"“进入子模块”只对状态列写着「子模块」的那一条有效。");
    return;
  }
  pendingItem_ = item;  // 拷贝一份：探测期间列表可能又被刷新掉，问的是点中的那一条。
  stage_ = Stage::entering;

  platform::SubmoduleEntryRequest request;
  request.exePath = ctx.gitExecutable;
  request.parentRoot = ctx.detection.root;
  request.submoduleRelativePath = item.path;
  request.flags = item.submodule;
  request.itemIsSubmodule = true;
  request.timeoutMilliseconds = kSubmoduleProbeTimeoutMs;
  entryWorker_.Request(ctx.notifyWindow, kSubmoduleEntryProbeCompleted, std::move(request),
                       [](const platform::SubmoduleEntryRequest& pending) {
                         return platform::RunSubmoduleEntryLoad(pending);
                       });
  host.SetStatus(L"进入子模块之前先在后台只读问三件事：那个目录在不在、Git 认不认它是"
                 L"**当前这个父仓库**登记的子模块（--show-superproject-working-tree）、"
                 L"父索引里那条 gitlink 记的是哪一份提交。全程不弹命令窗口、不改任何东西、"
                 L"不访问远端，也不会替你初始化子模块…");
}

void SubmoduleFlow::OnEntryProbeCompleted(CommitOperationHost& host, const OperationContext& ctx,
                                          uint64_t completionSerial) {
  platform::SubmoduleEntryOutcome outcome;
  if (!entryWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 更晚一次的探测取代了它，或这一次已经作废。
  }
  if (stage_ != Stage::entering) {
    return;
  }
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    stage_ = Stage::none;
    host.SetStatus(L"探测回来时界面绑定的仓库已经换掉，因此没有进入任何子模块，"
                   L"表单与代管内容都没有动。");
    return;
  }
  const git::SubmoduleEntryPlan plan = git::BuildSubmoduleEntryPlan(outcome.facts);
  if (plan.decision == git::SubmoduleEntryDecision::refuse) {
    stage_ = Stage::none;
    host.ShowInfo(L"不能进入这个子模块", plan.reason);
    host.SetStatus(L"没有进入任何仓库：上面的原因说清了是哪一条不过。父仓库的索引、表单与"
                   L"代管内容一个字都没动，也没有访问远端。");
    return;
  }

  // 三条都过了：先把父仓库界面上那份草稿采集下来，再发起切换。
  // 顺序不能颠倒——切换会把旧列表与身份一起作废，采集晚了拿到的就不是刚才那一份。
  // 切换本身失败（没有发起识别）时什么都不做：表单没清、栈没压，用户看到的还是原样。
  const app::HeldForm parentDraft = CaptureHeld(host, ctx);
  const std::wstring targetDirectory = plan.targetDirectory;
  std::wstring statusNote =
      L"正在把界面绑定的仓库换成子模块：" + pendingItem_.path + L"（只读核对已过：目录存在、"
      L"Git 认它是当前父仓库登记的子模块）。" +
      (parentDraft.hasUserContent
           ? std::wstring(L"\n父仓库这份未提交的表单草稿已由导航收起来代管，回到父仓库时原样交回。")
           : std::wstring(L"\n离开时父仓库的表单里没有你写的内容，因此没有需要代管的草稿。")) +
      L"\n" + plan.disclosure;
  if (!host.NavigateRepository(targetDirectory, statusNote)) {
    stage_ = Stage::none;
    return;  // 原因已由实现写进状态栏。
  }

  app::JourneyHop hop;
  hop.parentRoot = ctx.detection.root;
  hop.parentKey = RepositoryKey(ctx);
  hop.submodulePath = pendingItem_.path;
  hop.childRoot = outcome.facts.child.root;
  hop.childKey = HeldKeyOf(outcome.facts.child);
  journey_.Descent(std::move(hop), parentDraft);
  host.ClearFormForNavigation(
      parentDraft.hasUserContent
          ? L"父仓库那份未提交的表单内容已由“子模块导航”收起来代管；回到那个仓库时原样交回。"
            L"这里是子模块自己的表单，从空的开始。"
          : L"这里是子模块自己的表单（父仓库那边离开时没有你写的内容，因此没有代管）。");
  expectedArrivalRoot_ = targetDirectory;
  stage_ = Stage::awaitingEntry;
}

void SubmoduleFlow::ReturnToParent(CommitOperationHost& host, const OperationContext& ctx) {
  if (stage_ != Stage::none) {
    host.SetStatus(L"已经有一次子模块导航在走（探测或切换还没完成），再点不会排队。"
                   L"请等那一步结束。");
    return;
  }
  if (!ctx.repoUsable || ctx.detection.root.empty()) {
    host.SetStatus(L"现在没有可用的工作区，连「现在在哪个仓库」都答不上来，因此没有返回。"
                   L"代管里的草稿仍在原处。请先点“刷新”。");
    return;
  }
  const app::SubmoduleJourney::ReturnCheck check = journey_.CheckReturn(ctx.detection.root);
  if (check == app::SubmoduleJourney::ReturnCheck::nothingHeld) {
    host.SetStatus(L"没有可退回去的一层：这台界面上还没有从父仓库进过子模块。"
                   L"（换绑定的仓库本来要靠“本地仓库”那一栏自己填路径，导航只管进过的那几层。）");
    return;
  }
  if (check == app::SubmoduleJourney::ReturnCheck::notOnThatChild) {
    const app::JourneyHop* top = journey_.top();
    host.SetStatus(L"没有返回：栈顶记下的是从「" +
                   (top != nullptr && !top->childRoot.empty() ? top->childRoot
                                                              : std::wstring(L"（没记下来）")) +
                   L"」进来的一层，而界面现在绑定的是「" + ctx.detection.root +
                   L"」。把父仓库那份草稿交到一个不相干的仓库上，比不返回更糟。"
                   L"代管内容一个字没动：想回到那个父仓库，请在“本地仓库”那一栏填它的路径。");
    return;
  }

  const app::JourneyHop hop = *journey_.top();
  const app::HeldForm childDraft = CaptureHeld(host, ctx);
  std::wstring statusNote =
      L"正在把界面绑定的仓库退回父仓库：" + hop.parentRoot + L"\n" +
      (childDraft.hasUserContent ? std::wstring(L"子模块这份未提交的表单内容已由导航收起来代管，"
                                                L"再进这个子模块时原样交回。")
                                 : std::wstring(L"离开子模块时它的表单里没有你写的内容，"
                                               L"因此没有需要代管的草稿。")) +
      L"\n回到父仓库后还会只读问三份位置（父索引记的 / 父提交记的 / 子模块现在在哪），"
      L"提示该不该暂存指针——本程序不会替你 add、commit、push，也不递归处理别的子模块。";
  if (!host.NavigateRepository(hop.parentRoot, statusNote)) {
    stage_ = Stage::none;
    return;
  }
  if (childDraft.hasUserContent) {
    journey_.Hold(childDraft);
  }
  host.ClearFormForNavigation(
      childDraft.hasUserContent
          ? L"子模块那份未提交的表单内容已由“子模块导航”收起来代管；再进这个子模块时原样交回。"
            L"这里是父仓库的表单。"
          : L"这里是父仓库的表单（离开子模块时它那边没有你写的内容，因此没有代管）。");
  pendingHop_ = hop;
  pendingHopValid_ = journey_.PopTop(&pendingHop_);
  expectedArrivalRoot_ = hop.parentRoot;
  stage_ = Stage::awaitingReturn;
}

void SubmoduleFlow::HandBackForm(CommitOperationHost& host, const OperationContext& ctx,
                                 const std::wstring& repositoryKey) {
  app::HeldForm held;
  if (repositoryKey.empty() || !journey_.Take(repositoryKey, &held)) {
    return;  // 这个仓库没有代管记录：表单一个字都不动。
  }
  const auto decision = app::PlanDraftSwap(held, host.CommitFormHasUserContent());
  if (decision == app::DraftSwapDecision::heldIsEmpty ||
      decision == app::DraftSwapDecision::nothingHeld) {
    return;  // 代管的那份本来就没有内容：交还没有意义，更不能去盖现在屏幕上的东西。
  }
  if (decision == app::DraftSwapDecision::askBeforeSwap) {
    // 两份都是用户写的字：谁盖谁必须由人当场选，程序不取舍。
    // 用风险确认框而不是普通确认框：按钮上的字要说清按下去是哪一份留下，
    // 而「取消」在这里不是「放弃导航」，是「留下界面上这一份」。
    const bool restoreHeld = host.RiskConfirm(
        L"回到这个仓库：有两份未提交的表单内容", L"代管里有一份，界面上现在也写着另一份",
        L"交回代管那份",
        L"点“交回代管那份”：代管那份倒回表单，界面上这一份由程序再收进代管（两份都不丢）。\n"
        L"点“取消”：界面上这一份原样留着，代管那份继续保管，下次回到这个仓库还能交还。\n"
        L"两份内容都不会被悄悄覆盖。");
    if (!restoreHeld) {
      journey_.Hold(held);
      host.SetFormNote(L"代管里那份" + held.repositoryRoot +
                       L"的草稿继续保管着，界面上你新写的东西一个字没动。");
      return;
    }
    const app::HeldForm screenDraft = CaptureHeld(host, ctx);
    if (screenDraft.hasUserContent) {
      journey_.Hold(screenDraft);
    }
  }
  host.ApplyHeldFormForNavigation(
      held, L"这份内容是这个仓库离开时由“子模块导航”代管的草稿，现在原样交回" +
            (decision == app::DraftSwapDecision::askBeforeSwap
                 ? std::wstring(L"（界面上那一份也已收进代管，没有丢）")
                 : std::wstring(L"（交回前界面是空的，没有任何内容被覆盖）")) +
            L"。");
}

void SubmoduleFlow::OnRepositoryArrived(CommitOperationHost& host, const OperationContext& ctx) {
  if (stage_ != Stage::awaitingEntry && stage_ != Stage::awaitingReturn) {
    return;  // 这次识别是用户自己换仓库或刷新引起的，不属于任何一次导航。
  }
  const bool arrived =
      !expectedArrivalRoot_.empty() && !ctx.detection.root.empty() &&
      git::PathsEqualFolded(expectedArrivalRoot_, ctx.detection.root);
  if (!arrived) {
    // 我们发起的那次切换被人抢了（用户直接在“本地仓库”里填了别的路径）：
    // 导航就此收场，代管内容一律留在原处——那里面是用户写的字，不能顺手清。
    stage_ = Stage::none;
    expectedArrivalRoot_.clear();
    pendingHopValid_ = false;
    host.SetStatus(L"这次导航没有完成：界面绑定的仓库已经不是导航要去的那一个"
                   L"（导航发起后又有人改了“本地仓库”那一栏）。代管里的草稿与表单都没有动。");
    return;
  }
  const std::wstring key = RepositoryKey(ctx);
  const bool wasReturn = stage_ == Stage::awaitingReturn;
  stage_ = Stage::none;
  expectedArrivalRoot_.clear();
  HandBackForm(host, ctx, key);
  if (wasReturn && pendingHopValid_) {
    const app::JourneyHop hop = pendingHop_;
    pendingHopValid_ = false;
    StartPointerProbe(host, ctx, hop);
  }
}

void SubmoduleFlow::OnRepositoryArrivalFailed(CommitOperationHost& host,
                                              const OperationContext& ctx) {
  if (stage_ != Stage::awaitingEntry && stage_ != Stage::awaitingReturn) {
    return;
  }
  stage_ = Stage::none;
  expectedArrivalRoot_.clear();
  pendingHopValid_ = false;
  host.SetStatus(L"这次导航没有落地：目标目录没有可用的工作区（原因见上一行）。"
                 L"代管里的草稿原样留着，父索引、提交与远端都没有被碰过。当前仓库：" +
                 (ctx.detection.root.empty() ? std::wstring(L"（Git 没有答出根目录）")
                                             : ctx.detection.root));
}

void SubmoduleFlow::StartPointerProbe(CommitOperationHost& host, const OperationContext& ctx,
                                      const app::JourneyHop& hop) {
  stage_ = Stage::pointing;
  platform::SubmodulePointerRequest request;
  request.exePath = ctx.gitExecutable;
  request.parentRoot = ctx.detection.root;
  request.submoduleRelativePath = hop.submodulePath;
  request.submoduleDirectory =
      git::JoinWorktreeFilePath(ctx.detection.root, hop.submodulePath);
  request.timeoutMilliseconds = kSubmoduleProbeTimeoutMs;
  pointerWorker_.Request(ctx.notifyWindow, kSubmodulePointerProbeCompleted, std::move(request),
                         [](const platform::SubmodulePointerRequest& pending) {
                           return platform::RunSubmodulePointerLoad(pending);
                         });
  host.SetStatus(L"已回到父仓库。正在只读核对三份位置：父索引里那条 gitlink、父提交里记的那一份、"
                 L"以及子模块自己现在的 HEAD（不改动任何一边，也不访问远端）…");
}

void SubmoduleFlow::OnPointerProbeCompleted(CommitOperationHost& host, const OperationContext& ctx,
                                            uint64_t completionSerial) {
  platform::SubmodulePointerOutcome outcome;
  if (!pointerWorker_.FetchLatest(completionSerial, &outcome)) {
    return;
  }
  if (stage_ != Stage::pointing) {
    return;
  }
  stage_ = Stage::none;
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    host.SetStatus(L"指针核对回来时仓库已经换掉，因此没有给出任何提示，也没有动任何东西。");
    return;
  }
  const std::wstring path = pendingHopValid_ ? pendingHop_.submodulePath : std::wstring();
  const std::wstring report =
      git::ComposeSubmodulePointerReport(outcome.facts, path.empty() ? L"（路径没记下来）" : path);
  host.SetStatus(report);
  host.RememberOperationConclusion(L"子模块指针核对：" +
                                   std::wstring(git::SubmodulePointerStateLabel(outcome.facts.state)));
  // 有得做与没法判断的场合都当面说一句：状态栏那行会被随后的刷新挤掉，
  // 而「下一步该由谁做」正是这次导航的落点。显示干净的场合不再多弹一次窗。
  if (outcome.facts.state != git::SubmodulePointerState::consistentClean) {
    host.ShowInfo(L"子模块指针与父仓库的关系", report);
  }
}

void SubmoduleFlow::BeginStop() {
  entryWorker_.BeginStop();
  pointerWorker_.BeginStop();
}

void SubmoduleFlow::JoinWorkers() {
  entryWorker_.Join();
  pointerWorker_.Join();
}

}  // namespace gc::ui
