#include "ui/commit_flow.h"

#include <string>
#include <utility>

#include "git/commit_history.h"
#include "git/commit_identity.h"
#include "git/repository.h"
#include "platform/windows/commit_message_file.h"
#include "platform/windows/local_time.h"
#include "ui/commands.h"

namespace gc::ui {
namespace {

// 把一段墙上时间换算成「交给 Git 的值 + 给人看的说明」；失败时写原因并返回 false。
// 时区与偏移一律按这一刻在本机实际生效的值问（platform/windows/local_time），
// 交给 Git 的日期串带着偏移，不会按「读它的那个进程的时区」被挪动。
bool BuildCommitTimeChoice(const git::CivilTime& wall, git::CommitTimeChoice* out,
                           std::wstring* refusal) {
  *out = git::CommitTimeChoice{};
  const platform::LocalInstant instant = platform::ResolveLocalWallTime(wall);
  if (!instant.valid) {
    *refusal = instant.failureReason;
    return false;
  }
  std::string gitDate;
  std::wstring dateRefusal;
  if (!git::FormatGitInternalDate(instant.utcEpochSeconds, instant.offsetMinutes, &gitDate, &dateRefusal)) {
    *refusal = dateRefusal;
    return false;
  }
  out->gitDate = std::move(gitDate);
  out->wall = wall;
  out->offsetMinutes = instant.offsetMinutes;
  out->displayText = git::FormatCommitTimeText(wall, instant.offsetMinutes);
  return true;
}

}  // namespace

void CommitFlow::Start(CommitOperationHost& host, const OperationContext& ctx,
                       const git::WorkspaceModel& model) {
  const auto refuse = [&](std::wstring_view message) { host.SetStatus(std::wstring(message)); };

  if (ctx.workspaceStatus != git::WorkspaceLoadStatus::loaded) {
    refuse(L"还没读到这个仓库的工作区状态（" + ctx.workspaceMessage +
           L"），无法确定要提交什么。请先点“刷新”。");
    return;
  }

  // 正文与身份的校验：规则在 git/commit_message 里，这里只把它的答案当执行前提。
  const CommitFormSnapshot form = host.CaptureCommitForm();
  const git::CommitFormValidity validity = git::ValidateCommitForm(form.data, ctx.committerState);
  if (!validity.Ok()) {
    host.RunFormValidation({});
    refuse(L"表单还没通过校验，因此没有提交任何内容：" + validity.StatusText());
    return;
  }

  // 「默认用提交时的当前时间」在这里落地：没人动过时间控件时，先把控件改成此刻，
  // 让用户亲眼看到要用的到底是哪个时间；应用启动时那个时刻绝不代替它。
  if (!host.CommitTimesUserEdited()) {
    host.ApplyDefaultTimesToNow();
  }

  // 点击瞬间先按屏幕上的时间试算一次：换算不通的话不用等预检回来就该看见原因
  // （预检回来后仍会按当时的屏幕内容重新算，那一份才是真正进确认框的）。
  const CommitFormSnapshot now = host.CaptureCommitForm();
  git::CommitTimeChoice authorTime;
  git::CommitTimeChoice committerTime;
  std::wstring refusal;
  if (!BuildCommitTimeChoice(now.authorWall, &authorTime, &refusal)) {
    refuse(L"作者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }
  if (now.timesSynced) {
    committerTime = authorTime;  // 同步勾选：提交者时间以作者时间为准（界面也已一致显示）。
  } else if (!BuildCommitTimeChoice(now.committerWall, &committerTime, &refusal)) {
    refuse(L"提交者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }

  // 记下点击瞬间界面显示的那份摘要，然后发起一次只读重读：
  // 确认框必须摆出「刚刚读回的仓库现状」，不能拿几分钟前的列表当真。
  captured_ = git::CapturedSnapshot{};
  captured_.valid = true;
  captured_.shortSha = ctx.detection.shortSha;
  captured_.hasHead = ctx.detection.headResolved;
  captured_.stagedItems = model.staged.size();
  stage_ = Stage::preConfirmRead;
  host.ScheduleRefresh();
  host.SetStatus(L"创建提交前先在后台重读仓库现状（只读查询，不弹命令窗口、不改动仓库），"
                 L"读回来后还要问齐这次提交要绑定的事实，然后给出确认框…");
}

bool CommitFlow::OnWorkspaceLoaded(CommitOperationHost& host, const OperationContext& ctx,
                                   bool loadSucceeded) {
  if (stage_ != Stage::preConfirmRead) {
    return false;  // 这一次读取不是「创建提交」点出来的，界面按普通刷新继续收尾。
  }
  if (!loadSucceeded) {
    // 列表都读不回来，确认框就没有「刚刚读回的现状」可摆：作废这次尝试，绝不拿旧列表当真。
    Abandon(host, L"提交前核对仓库现状时没能读到 git status：" + ctx.workspaceMessage +
                      L" 因此没有打开命令窗口，也没有对仓库做任何改动。请改正后再点一次“创建提交”。");
    return true;
  }
  RequestPreflightProbe(host, ctx);
  return true;
}

void CommitFlow::RequestPreflightProbe(CommitOperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::preConfirmProbe;
  platform::CommitProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.timeoutMilliseconds = kCommitProbeTimeoutMs;
  worker_.Request(ctx.notifyWindow, kCommitProbeCompleted, std::move(request),
                  [](const platform::CommitProbeRequest& pending) {
                    return platform::RunCommitProbeLoad(pending);
                  });
  host.SetStatus(L"创建提交前先在后台问齐这次提交要绑定的事实：工作区根与 Git 目录、完整分支引用、"
                 L"HEAD 完整对象 ID、索引内容标识、Git 目录里的流程痕迹、有效配置里的提交者身份。"
                 L"其中算索引内容用的 git write-tree 会把那棵树写进对象库、顺带刷新索引里过期的"
                 L"文件状态（不产生提交、不移动分支、不碰工作区文件）；问回来后给出确认框…");
}

void CommitFlow::OnProbeCompleted(CommitOperationHost& host, const OperationContext& ctx,
                                  const git::WorkspaceModel& model, uint64_t completionSerial) {
  platform::CommitProbeOutcome outcome;
  if (!worker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间已发起更晚的一趟查询，这份结果不再有意义。
  }
  const Stage stage = stage_;
  if (stage != Stage::preConfirmProbe && stage != Stage::executionRecheck) {
    return;  // 这一次尝试已经按「取消 / 换仓库 / 作废」结束，迟到的结果原样丢掉。
  }
  if (!ctx.repoUsable || !git::PathsEqualFolded(outcome.repositoryDirectory, ctx.detection.root)) {
    // 查询是在旧仓库上跑的：那份 HEAD/索引内容对当前界面显示的仓库毫无意义，
    // 表单原样留着，用户对新仓库重新点一次即可。
    Abandon(host, L"预检完成时仓库已经换掉，这次提交没有发出任何命令，"
                 L"刚写的提交信息文件已删除。表单里的内容一个字都没动，"
                 L"请对现在的仓库重新点一次“创建提交”。");
    return;
  }
  if (stage == Stage::preConfirmProbe) {
    HandlePreflightProbe(host, ctx, model, outcome);
  } else {
    HandleRecheckProbe(host, ctx, outcome);
  }
}

void CommitFlow::HandlePreflightProbe(CommitOperationHost& host, const OperationContext& ctx,
                                      const git::WorkspaceModel& model,
                                      const platform::CommitProbeOutcome& outcome) {
  const auto abandon = [&](std::wstring_view reason) { Abandon(host, reason); };

  const CommitFormSnapshot form = host.CaptureCommitForm();
  const git::CommitFormValidity validity =
      git::ValidateCommitForm(form.data, outcome.facts.committer);
  if (!validity.Ok()) {
    host.RunFormValidation({});
    abandon(L"预检回来之后表单校验没通过，因此没有提交任何内容：" + validity.StatusText());
    return;
  }
  git::GitIdentity author;
  std::wstring identityError;
  if (!git::ParseGitIdentity(form.data.author, &author, &identityError, L"作者")) {
    abandon(L"作者身份没能拆解成「姓名 <邮箱>」，因此没有提交任何内容：" + identityError);
    return;
  }

  git::CommitTimeChoice authorTime;
  git::CommitTimeChoice committerTime;
  std::wstring refusal;
  if (!BuildCommitTimeChoice(form.authorWall, &authorTime, &refusal)) {
    abandon(L"作者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }
  if (form.timesSynced) {
    committerTime = authorTime;
  } else if (!BuildCommitTimeChoice(form.committerWall, &committerTime, &refusal)) {
    abandon(L"提交者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }

  const git::ComposedCommitMessage composed = git::ComposeCommitMessage(form.data);
  if (!composed.rejectedCoauthors.empty() || composed.message.empty()) {
    // 校验已经过了才会走到这里，出现这种组合说明表单内容在校验之后又被改动了：
    // 宁可不提交，也不按一份没核对过的内容写提交信息。
    abandon(L"提交信息合成时出现了没能解析的合作者条目，因此没有提交任何内容。"
            L"请检查“合作者”列表后重新点击“创建提交”。");
    return;
  }

  const platform::CommitMessageFileWrite written =
      platform::WriteCommitMessageFile(composed.message);
  if (!written.written) {
    abandon(L"没有打开命令窗口，也没有对仓库做任何改动。写提交信息文件失败：" +
            written.failureReason);
    return;
  }
  // 文件已经存在了，先登记保管人：下面任何一条拒绝路径都要顺手回收它。
  messageFile_ = written.path;

  git::CommitPlanInput input;
  input.model = model;
  input.detection = ctx.detection;
  // 提交者可得性、那串身份文字与流程痕迹一律取自这一趟预检，不再取界面早前存下的那一份：
  // 确认框上写的、复核时比对的必须是同一次查询问回来的同一个东西。
  input.identity = outcome.facts;
  input.gitExecutable = ctx.gitExecutable;
  input.captured = captured_;
  captured_ = git::CapturedSnapshot{};
  input.author = author;
  input.message = composed.message;
  input.messageUtf8Bytes = written.payloadBytes;
  input.messageFilePath = written.path;
  input.authorTime = authorTime;
  input.committerTime = committerTime;
  input.timesSynced = form.timesSynced;

  const git::CommitPlan plan = git::BuildCommitPlan(input);
  if (plan.blocked) {
    // 方案层拒绝时这条命令根本不存在，信息文件也就没人会去读：随这次尝试一起回收。
    Abandon(host, L"没有打开命令窗口，也没有对仓库做任何改动。" + plan.blockedReason);
    return;
  }

  preflight_ = outcome;
  plan_ = plan;
  confirmedForm_ = form.data;

  if (!host.Confirm(L"创建提交前请确认", plan.previewText, /*warningIcon=*/true)) {
    Abandon(host, L"已取消：没有打开命令窗口，也没有对仓库做任何改动。刚写的提交信息文件已删除，"
                 L"表单里的内容一个字都没动。");
    return;
  }
  RequestExecutionRecheck(host, ctx);
}

void CommitFlow::RequestExecutionRecheck(CommitOperationHost& host, const OperationContext& ctx) {
  stage_ = Stage::executionRecheck;
  platform::CommitProbeRequest request;
  request.exePath = ctx.gitExecutable;
  request.repositoryDirectory = ctx.detection.root;
  request.timeoutMilliseconds = kCommitProbeTimeoutMs;
  worker_.Request(ctx.notifyWindow, kCommitProbeCompleted, std::move(request),
                  [](const platform::CommitProbeRequest& pending) {
                    return platform::RunCommitProbeLoad(pending);
                  });
  host.SetStatus(L"点头之后、发出命令之前，把预检那组只读查询原样重发一遍：工作区根与 Git 目录 / "
                 L"完整分支引用 / HEAD 完整对象 ID / 索引内容标识 / 流程痕迹 / 提交者身份都对得上，"
                 L"才启动那条 git commit…");
}

void CommitFlow::HandleRecheckProbe(CommitOperationHost& host, const OperationContext& ctx,
                                    const platform::CommitProbeOutcome& outcome) {
  static_cast<void>(ctx);  // 启动参数一律取自方案绑定的身份，这里不再读界面快照。
  const std::wstring change = git::DescribeCommitIdentityChange(plan_.identity, outcome.facts);
  if (!change.empty()) {
    // 复核不过：作废的是「这一份现状」，不是用户写下的内容。表单原样留着，信息文件回收，
    // 重读回来后由用户自己决定要不要对新现状再确认一次。
    host.ShowWarning(L"执行前复核：仓库又变了", change);
    Abandon(host, L"执行前复核发现现状与确认框上写的不一致，因此没有发出那条提交命令。"
                 L"表单里的内容一个字都没动，仓库状态正在重读，"
                 L"看清现状后如仍要提交请再点一次“创建提交”。");
    host.ScheduleRefresh();
    return;
  }
  // 方案取一份副本：启动流程要把它的内容搬进命令与选项，而这次尝试的记录在启动前就被清掉。
  const git::CommitPlan plan = plan_;
  const git::CommitFormData committedForm = confirmedForm_;
  Launch(host, plan, committedForm);
}

void CommitFlow::Launch(CommitOperationHost& host, const git::CommitPlan& plan,
                        const git::CommitFormData& committedForm) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  // 启动参数一律取自这份「确认框上写的、并且刚刚复核过的」身份，不再回读界面此刻的状态：
  // 命令属于那个仓库，就不能因为界面后来换了显示而跑到别的目录去。
  operation.gitExecutable = plan.identity.gitExecutable;
  operation.repositoryDirectory = plan.identity.repositoryDirectory;
  operation.arguments = plan.arguments;
  operation.environmentOverrides = plan.environmentOverrides;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（" +
                        plan.identity.repositoryDirectory + L"），本次提交 " +
                        std::to_wstring(plan.stagedItems) + L" 项已暂存内容（索引内容标识 " +
                        git::ShortObjectId(plan.identity.indexTreeOid) + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.messageFile = plan.identity.messageFilePath;
  options.commitOperation = true;
  options.committedForm = committedForm;
  // 操作历史：记下这次提交「确认时到底是哪一份」——分支与提交前 HEAD 都取自复核过的那份身份。
  // 新提交的对象 ID 此刻还没生成（由 Git 造），因此不做引用级自动回退；反悔交给「撤回最近提交」。
  options.history.record = true;
  options.history.flow = app::HistoryFlow::commit;
  options.history.workTreeRoot = plan.identity.repositoryDirectory;
  options.history.operationLabel = plan.displayName;
  options.history.sourceRef = plan.identity.branchRef;
  options.history.sourceObjectId = plan.identity.headObjectId;
  options.history.restoreKind = app::HistoryRestoreKind::none;
  options.history.restoreNote =
      L"如需反悔：若这条提交仍是该分支的最新提交，可用「撤回最近提交」把分支挪回提交前的 HEAD（" +
      plan.identity.headObjectId + L"）。本程序不自动撤销任何东西。";
  // 信息文件的所有权在这里转交给 ActiveOperation：只有拿到终态（或启动失败由执行路径回收）才删，
  // 命令窗口里的 Git 可能还在读它。
  ReleaseAttempt(false);
  if (!host.LaunchCommandWindow(operation, options)) {
    // 启动失败时信息文件已由执行路径回收，这里只补一句表单没动的说明。
    host.SetStatus(L"这次提交没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                   L"表单里的内容一个字都没动。");
    return;
  }
  if (!plan.stateChangeNote.empty()) {
    // 用户已经看过那句「以刚读回的为准」，这一句留在状态栏里，操作结束后还能对上号。
    host.SetStatus(L"已在命令窗口启动创建提交。" + plan.stateChangeNote);
  }
}

void CommitFlow::OnCommandSettled(CommitOperationHost& host, bool succeeded,
                                  const git::CommitFormData& committedForm) {
  // 「创建提交」的收尾只认 Git 的退出码：成功才清已提交的正文，失败时表单一个字都不动，
  // 用户可以直接改好再点一次（那种场合最不该丢的就是他刚写下来的东西）。
  if (!succeeded) {
    host.SetFormNote(L"提交没有成功：标题、描述、作者与合作者一个字都没动，"
                     L"改好之后再点一次“创建提交”。命令窗口里留着 Git 的完整输出。");
    return;
  }
  const git::CommitFormData current = host.CaptureCommitForm().data;
  const app::CommittedFormCleanup cleanup =
      app::PlanCommittedFormCleanup(committedForm, current, host.CommitTimesUserEdited());
  host.ApplyCommittedFormCleanup(cleanup);
  host.SetFormNote(cleanup.note);
}

void CommitFlow::Abandon(CommitOperationHost& host, std::wstring_view reason) {
  ReleaseAttempt(true);
  host.SetStatus(std::wstring(reason));
}

void CommitFlow::ReleaseAttempt(bool reclaimMessageFile) {
  if (reclaimMessageFile && !messageFile_.empty()) {
    // 这份信息文件从没进过命令窗口，Git 不会来读它：随这次尝试一起回收。
    platform::RemoveCommitMessageFile(messageFile_);
  }
  stage_ = Stage::none;
  captured_ = git::CapturedSnapshot{};
  preflight_ = platform::CommitProbeOutcome{};
  plan_ = git::CommitPlan{};
  confirmedForm_ = git::CommitFormData{};
  messageFile_.clear();
}

void CommitFlow::BeginStop() {
  worker_.BeginStop();
}

void CommitFlow::JoinWorkers() {
  worker_.Join();
}

}  // namespace gc::ui
