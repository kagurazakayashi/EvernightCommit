#include "git/undo_commit_plan.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/commit_history.h"

namespace gc::git {
namespace {

// 远端引用列表在确认框里最多列几条；其余折成「等 N 处」。
constexpr size_t kMaxPublishRefList = 5;

std::wstring LowerAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
  }
  return result;
}

// 短 ID 与完整 ID 的对照：Git 的 --short 长度随仓库增长，界面摘要里的短 ID 未必正好
// 是 kShortObjectIdLength 个字符，因此「相等或一方是另一方的前缀」都算同一个提交；
// 任何一边为空则视为不可比（不参与「现状已变」的判定）。
bool ShaDisplaysMatch(std::wstring_view left, std::wstring_view right) {
  if (left.empty() || right.empty()) {
    return true;
  }
  const std::wstring a = LowerAscii(left);
  const std::wstring b = LowerAscii(right);
  if (a.size() <= b.size()) {
    return b.compare(0, a.size(), a) == 0;
  }
  return a.compare(0, b.size(), b) == 0;
}

std::wstring RefusalWith(const UndoQueryRead& read, std::wstring_view fallback) {
  if (!read.detail.empty()) {
    return std::wstring(read.detail);
  }
  return std::wstring(fallback);
}

// rev-list --parents 的一行：「自身ID 父1 父2 …」。按空白拆 token，逐个校验完整对象 ID。
bool ParseParentsLine(std::wstring_view line, std::wstring* selfId, std::vector<std::wstring>* parents,
                      std::wstring* failure) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (const wchar_t c : line) {
    if (c == L' ' || c == L'\t') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  if (tokens.empty()) {
    if (failure != nullptr) {
      *failure = L"rev-list --parents 没有给出任何内容，无法确定父提交。";
    }
    return false;
  }
  if (!LooksLikeFullObjectId(tokens.front())) {
    if (failure != nullptr) {
      *failure = L"rev-list --parents 的自身 ID 不符合完整对象 ID 的约定。";
    }
    return false;
  }
  for (size_t index = 1; index < tokens.size(); ++index) {
    if (!LooksLikeFullObjectId(tokens[index])) {
      if (failure != nullptr) {
        *failure = L"rev-list --parents 输出里有一个父提交 ID 形态不合格，无法安全撤回。";
      }
      return false;
    }
  }
  if (selfId != nullptr) {
    *selfId = tokens.front();
  }
  if (parents != nullptr) {
    parents->assign(tokens.begin() + 1, tokens.end());
  }
  return true;
}

std::wstring PublishEvidenceSentence(const UndoPreflightFacts& facts) {
  switch (facts.publish) {
    case UndoPublishEvidence::contained: {
      std::wstring listed;
      for (size_t index = 0; index < std::min(facts.containingRemoteRefs.size(), kMaxPublishRefList);
           ++index) {
        if (!listed.empty()) {
          listed += L"、";
        }
        listed += facts.containingRemoteRefs[index];
      }
      if (facts.containingRemoteRefs.size() > kMaxPublishRefList) {
        listed += L" 等 " + std::to_wstring(facts.containingRemoteRefs.size()) + L" 处";
      }
      return L"发布状态：已知已发布。本地远端跟踪引用显示该提交已被 " + listed +
             L" 包含——它很可能已经推送过。其他协作者可能已经基于它工作，撤回会在你们之间造成"
             L"历史分叉。本程序只会移动本机的分支引用，不会（也无法自动）改动远端。";
    }
    case UndoPublishEvidence::notFound:
      return L"发布状态：本地信息未发现已发布。本地的远端跟踪引用（截至最近一次 fetch；本程序没有"
             L"联网核对，这些引用可能已过期）中没有任何一个包含该提交。注意：这不能证明它从未被"
             L"推送过——也可能只是还没有 fetch 下来。";
    case UndoPublishEvidence::noRemoteRefs:
      return L"发布状态：无法判断。这个仓库本地没有任何远端跟踪引用（refs/remotes 为空），"
             L"可能从未推送，也可能只是从未 fetch；本程序不联网核对，不能保证它从未 push。";
    case UndoPublishEvidence::queryFailed:
      return L"发布状态：无法判断。询问远端跟踪引用时 Git 没能给出可采信的回答（" +
             (facts.publishFailureDetail.empty() ? std::wstring(L"原因未知")
                                                 : facts.publishFailureDetail) +
             L"）。判断不了就当作有已发布的风险处理。";
    case UndoPublishEvidence::notRun:
    default:
      return L"发布状态：未能查询。";
  }
}

}  // namespace

std::vector<std::wstring> BuildUndoSymbolicRefArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"symbolic-ref", L"--quiet", L"HEAD"};
}

std::vector<std::wstring> BuildUndoHeadCommitArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   L"HEAD"};
}

std::vector<std::wstring> BuildUndoParentsArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-list", L"--parents", L"-n", L"1",
                                   L"HEAD"};
}

std::vector<std::wstring> BuildUndoHeadSummaryArguments(std::wstring_view repositoryDirectory) {
  // 与 git/commit_history 同样的防护：--no-decorate 关掉装饰、log.showSignature=false
  // 覆盖用户配置，否则 %s 前后的杂项会污染这一行展示文本。
  return std::vector<std::wstring>{L"-C",    std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"-c", L"log.showSignature=false", L"log",
                                   L"-1",    L"--no-decorate", L"--format=%s",                 L"HEAD"};
}

std::vector<std::wstring> BuildUndoRemoteRefsArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"for-each-ref", L"--format=%(refname)", L"refs/remotes"};
}

std::vector<std::wstring> BuildUndoRemoteContainsArguments(std::wstring_view repositoryDirectory,
                                                           std::wstring_view headSha) {
  if (!LooksLikeFullObjectId(headSha)) {
    return {};  // 不合格的 ID 根本不配送进 Git；调用方据此跳过这条查询。
  }
  return std::vector<std::wstring>{L"-C",       std::wstring(repositoryDirectory),
                                   L"--no-optional-locks", L"for-each-ref",
                                   L"--contains", std::wstring(headSha), L"--format=%(refname)",
                                   L"refs/remotes"};
}

UndoQueryRead ReadUndoQuery(const GitQueryResult& result) {
  UndoQueryRead read;
  if (!result.started) {
    read.error = RepoError::gitLaunchFailed;
    read.detail = L"Git 查询进程未能启动";
    return read;
  }
  if (result.timedOut) {
    read.error = RepoError::gitTimeout;
    read.detail = L"Git 查询超时";
    return read;
  }
  if (!result.exited) {
    read.error = RepoError::gitFailed;
    read.detail = L"Git 查询未能正常结束";
    return read;
  }
  if (result.exitCode == 0) {
    read.outcome = UndoQueryOutcome::answered;
    for (std::wstring_view line : SplitLines(result.utf16Output)) {
      const std::wstring trimmed = TrimWide(line);
      if (!trimmed.empty()) {
        read.lines.push_back(trimmed);
      }
    }
    read.firstLine = read.lines.empty() ? std::wstring() : read.lines.front();
    return read;
  }
  if (result.exitCode == 1 && TrimWide(result.utf16Output).empty()) {
    // --quiet 系查询的「正常没有」：symbolic-ref（不在分支上）与 rev-parse --verify --quiet
    // （HEAD 不可解析）都以退出码 1、无输出作答。其余命令不该走到这里，判读方会按语义处理。
    read.outcome = UndoQueryOutcome::noResult;
    return read;
  }
  std::wstring detail;
  read.error = ClassifyGitFailure(result, detail);
  if (read.error == RepoError::none) {
    read.error = RepoError::gitFailed;
  }
  read.detail = detail;
  return read;
}

std::wstring_view UndoPublishEvidenceLabel(UndoPublishEvidence evidence) noexcept {
  switch (evidence) {
    case UndoPublishEvidence::notRun:
      return L"未查询";
    case UndoPublishEvidence::queryFailed:
      return L"无法判断（查询失败）";
    case UndoPublishEvidence::noRemoteRefs:
      return L"无法判断（没有远端跟踪引用）";
    case UndoPublishEvidence::notFound:
      return L"本地信息未发现已发布";
    case UndoPublishEvidence::contained:
      return L"已知已发布";
  }
  return L"未查询";
}

UndoHeadFacts InterpretUndoHeadSnapshot(const GitQueryResult& symbolicRef,
                                        const GitQueryResult& headCommit) {
  UndoHeadFacts facts;
  const UndoQueryRead sym = ReadUndoQuery(symbolicRef);
  const UndoQueryRead head = ReadUndoQuery(headCommit);
  if (sym.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出当前分支：" + RefusalWith(sym, L"symbolic-ref 未成功");
    return facts;
  }
  if (head.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出 HEAD 指向哪个提交：" + RefusalWith(head, L"rev-parse 未成功");
    return facts;
  }
  facts.queryOk = true;

  if (sym.outcome == UndoQueryOutcome::answered) {
    facts.onBranch = true;
    facts.branchRef = sym.firstLine;
    constexpr std::wstring_view kHeadsPrefix = L"refs/heads/";
    if (facts.branchRef.size() > kHeadsPrefix.size() &&
        facts.branchRef.compare(0, kHeadsPrefix.size(), kHeadsPrefix) == 0) {
      facts.branchName = facts.branchRef.substr(kHeadsPrefix.size());
    }
  }
  if (head.outcome == UndoQueryOutcome::answered) {
    if (LooksLikeFullObjectId(head.firstLine)) {
      facts.headResolved = true;
      facts.headObjectId = head.firstLine;
    } else {
      facts.queryOk = false;
      facts.queryFailure = L"rev-parse 给出的 HEAD 不符合完整对象 ID 的约定，无法安全撤回。";
    }
  }
  return facts;
}

UndoPreflightFacts InterpretUndoPreflight(const UndoPreflightQueries& queries) {
  UndoPreflightFacts facts;
  facts.head = InterpretUndoHeadSnapshot(queries.symbolicRef, queries.headCommit);

  if (facts.head.headResolved) {
    facts.publishQueried = queries.commitDependentRan;
    if (queries.commitDependentRan) {
      facts.head.parentsQueried = true;
      const UndoQueryRead parents = ReadUndoQuery(queries.parents);
      if (parents.outcome != UndoQueryOutcome::answered) {
        facts.head.parentsFailure = RefusalWith(parents, L"rev-list 未成功");
      } else {
        std::wstring selfId;
        std::wstring failure;
        if (ParseParentsLine(parents.firstLine, &selfId, &facts.head.parentObjectIds, &failure)) {
          facts.head.selfMatchesHead = (selfId == facts.head.headObjectId);
          facts.head.parentsResolved = facts.head.selfMatchesHead;
          if (!facts.head.selfMatchesHead) {
            facts.head.parentsFailure =
                L"rev-list 报告的提交与 rev-parse 给出的 HEAD 不是同一个对象（HEAD 正在被别的"
                L"进程改动？），本次不撤回。";
          }
        } else {
          facts.head.parentsFailure = failure;
        }
      }
      // 标题只用于确认文字，查不到不算致命。
      const UndoQueryRead summary = ReadUndoQuery(queries.headSummary);
      if (summary.outcome == UndoQueryOutcome::answered) {
        facts.head.headSummary = summary.firstLine;
      }

      const UndoQueryRead refs = ReadUndoQuery(queries.remoteRefs);
      const UndoQueryRead contains = ReadUndoQuery(queries.remoteContains);
      if (refs.outcome == UndoQueryOutcome::failed) {
        facts.publish = UndoPublishEvidence::queryFailed;
        facts.publishFailureDetail = RefusalWith(refs, L"远端跟踪引用列表查询未成功");
      } else if (contains.outcome == UndoQueryOutcome::failed) {
        facts.publish = UndoPublishEvidence::queryFailed;
        facts.publishFailureDetail = RefusalWith(contains, L"远端包含查询未成功");
      } else if (contains.outcome != UndoQueryOutcome::answered ||
                 refs.outcome != UndoQueryOutcome::answered) {
        // for-each-ref 就算一无所获也是退出码 0；走到这里说明回答不合约定，按查不到处理。
        facts.publish = UndoPublishEvidence::queryFailed;
        facts.publishFailureDetail = L"for-each-ref 的回答不符合约定（意外的退出码或输出）。";
      } else if (!contains.lines.empty()) {
        facts.publish = UndoPublishEvidence::contained;
        facts.containingRemoteRefs = contains.lines;
      } else if (refs.lines.empty()) {
        facts.publish = UndoPublishEvidence::noRemoteRefs;
      } else {
        facts.publish = UndoPublishEvidence::notFound;
      }
    }
  }

  facts.statusQueried = true;
  const UndoQueryRead status = ReadUndoQuery(queries.status);
  if (status.outcome != UndoQueryOutcome::answered) {
    facts.statusError = status.error;
    facts.statusDetail = RefusalWith(status, L"git status 未成功");
  } else {
    WorkspaceStatusParseResult parsed = ParseWorkspacePorcelainV2(queries.status.utf16Output);
    if (!parsed.error.empty()) {
      facts.statusError = RepoError::badOutput;
      facts.statusDetail = parsed.error;
    } else {
      facts.statusOk = true;
      facts.model = std::move(parsed.model);
    }
  }
  return facts;
}

UndoCommitPlan BuildUndoCommitPlan(const UndoCommitPlanInput& input) {
  UndoCommitPlan plan;
  plan.operationId = L"undo-commit";
  plan.displayName = L"撤回最近提交";

  const auto block = [&](std::wstring reason) {
    UndoCommitPlan blocked;
    blocked.blocked = true;
    blocked.blockedReason = std::move(reason);
    blocked.operationId = plan.operationId;
    blocked.displayName = plan.displayName;
    return blocked;
  };

  // ---- 前提核對：任何一項不成立都不產生命令 ----
  if (input.repositoryRoot.empty()) {
    return block(L"没有可用的仓库工作区根目录，无法确定这次撤回属于哪个仓库。请先点“刷新”。");
  }
  const UndoHeadFacts& head = input.facts.head;
  if (!head.queryOk) {
    return block(head.queryFailure.empty() ? std::wstring(L"没能问出 HEAD 与分支，无法撤回。")
                                           : head.queryFailure);
  }
  if (!head.headResolved) {
    return block(L"还没有任何提交，没有可撤回的提交。“撤回最近提交”只处理当前分支的最新一次提交；"
                 L"仓库（或这个分支）尚为空时，索引与工作区本来就没有“相对某次提交的改动”可言。");
  }
  if (!head.onBranch) {
    return block(L"当前处于游离 HEAD（不在任何分支上）。撤回针对的是「当前分支的最新一次提交」，"
                 L"游离状态下没有分支引用可挪，本程序不替游离 HEAD 移动引用。请先 checkout 回一个"
                 L"分支再操作。");
  }
  if (input.workflow.HasSpecialFlowInProgress()) {
    return block(L"这个仓库里有 Git 流程还没走完：" + input.workflow.SpecialFlowText() +
                 L"。那种状态下分支与 HEAD 引用正被流程当作进度记录使用，此时移动引用会把流程的现场"
                 L"搅乱；“强制继续”也不能替你绕过——Git 自己就先不接受。请在原来的 Git 命令里把流程"
                 L"走完（git merge --continue / --abort、git rebase --continue / --abort 等），"
                 L"再点“刷新”。");
  }
  if (!head.parentsQueried) {
    return block(L"没能读到这条提交的父提交信息，无法确定撤回的目标。请点“刷新”后重试。");
  }
  if (!head.parentsResolved) {
    return block(L"父提交查询没能得出可采信的结论：" +
                 (head.parentsFailure.empty() ? std::wstring(L"原因未知") : head.parentsFailure) +
                 L"。目标定不准，本程序不会执行撤回。");
  }
  if (!input.facts.statusQueried || !input.facts.statusOk) {
    return block(L"没能读到工作区/索引现状（" +
                 (input.facts.statusDetail.empty() ? std::wstring(L"原因未知")
                                                   : input.facts.statusDetail) +
                 L"），无法确认撤回会把哪些内容留在索引里。本程序不在看不懂的现状上移动引用。");
  }
  if (input.facts.model.HasConflicts()) {
    return block(L"还有没解决的冲突条目。冲突状态下的索引与「一次普通提交的产物」不是一个东西，"
                 L"本程序不在带冲突标记的索引上移动引用，也不会替你解决冲突。请先处理这些冲突"
                 L"（解决后“加入暂存区”，或用“← 移出暂存区”退回），再点“刷新”。");
  }
  if (!input.facts.publishQueried) {
    return block(L"没能完成“这条提交是否已被远端包含”的查询。发布状态判断不了的时候，"
                 L"本程序不打开撤回的确认框。请点“刷新”后重试。");
  }

  const bool rootUndo = head.parentObjectIds.empty();
  const bool mergeCommit = head.parentObjectIds.size() >= 2;
  const std::wstring target = rootUndo ? std::wstring() : head.parentObjectIds.front();
  if (!rootUndo && !LooksLikeFullObjectId(target)) {
    return block(L"父提交 ID 的形态不合格，无法把它作为撤回目标。");
  }
  if (!LooksLikeFullObjectId(head.headObjectId)) {
    return block(L"HEAD 的完整对象 ID 形态不合格，无法核对与恢复。");
  }

  // ---- 风险分类：哪些要走「强制撤回（仅本地）」 ----
  std::vector<std::wstring> forceReasons;
  if (input.facts.publish == UndoPublishEvidence::contained) {
    forceReasons.push_back(L"该提交已被本地远端跟踪引用包含（很可能已推送过）");
  }
  if (input.facts.publish == UndoPublishEvidence::queryFailed ||
      input.facts.publish == UndoPublishEvidence::noRemoteRefs) {
    forceReasons.push_back(L"无法判断该提交是否已被远端包含（信息不足）");
  }
  if (mergeCommit) {
    forceReasons.push_back(L"这是一条合并提交（撤回会丢掉它与第一父之外的父关联）");
  }
  plan.requiresForce = !forceReasons.empty();

  // ---- 命令 ----
  if (rootUndo) {
    plan.arguments = {L"update-ref", L"-d", L"HEAD", head.headObjectId};
    plan.commandLabel = L"git update-ref -d HEAD";
    plan.targetDisplay = L"回到「尚无提交」（分支引用被删除，索引与工作区不变）";
  } else {
    plan.arguments = {L"reset", L"--soft", target};
    plan.commandLabel = L"git reset --soft";
    plan.targetDisplay = L"父提交 " + ShortObjectId(target);
  }

  // ---- 確認文字 ----
  const WorkspaceModel& model = input.facts.model;
  std::wstring preview;
  std::wstring commandLine = L"  git";
  for (const std::wstring& argument : plan.arguments) {
    commandLine += L' ' + argument;
  }
  preview += L"将在命令窗口里执行（工作目录：" + input.repositoryRoot + L"）：\n";
  preview += commandLine + L"\n\n";

  preview += L"仓库：" + input.repositoryRoot + L"\n";
  preview += L"分支：" + head.branchRef + L"\n";
  preview += L"将撤回的提交：" + ShortObjectId(head.headObjectId) + L"（完整 ID " + head.headObjectId +
             L"）「" + (head.headSummary.empty() ? std::wstring(L"（无标题）") : head.headSummary) +
             L"」\n";
  if (rootUndo) {
    preview +=
        L"目标是回到「尚无提交」：根提交没有父提交，git reset --soft HEAD^ 表达不了那种状态。\n";
    preview += L"命令改用受控路径：Git 只在分支引用确实还指向上面这个完整 ID 时才删除它，"
               L"对不上就原样拒绝；索引与工作区一个字节都不动。\n";
  } else {
    preview += L"撤回目标：" + plan.targetDisplay + L"\n";
    if (mergeCommit) {
      preview += L"这是一条合并提交：以第一父提交为目标。其余父提交（及其独有的历史）不会出现在"
                 L"当前分支上，但对象仍在仓库里，reflog 与相关引用还能找回。\n";
    }
  }
  preview += L"\n";

  if (!model.staged.empty() || !model.unstaged.empty()) {
    preview += L"工作区/索引现状：已暂存 " + std::to_wstring(model.staged.size()) + L" 项、未暂存 " +
               std::to_wstring(model.unstaged.size()) + L" 项。\n";
    preview += L"撤回后，原提交的改动将与这些现有改动一起保留；Git 不记录“哪一行属于哪一次提交”，"
               L"本程序无法把两堆分开。软撤回不动索引，因此不会产生文本合并冲突。\n";
  } else {
    preview += L"工作区与索引目前是干净的：撤回后，原提交的全部改动会原样出现在“已暂存的更改”里。\n";
  }
  preview += L"\n";

  preview += PublishEvidenceSentence(input.facts) + L"\n\n";

  preview += L"恢复线索：原提交完整 ID " + head.headObjectId + L"。\n";
  preview += L"找回方式：git reset --soft " + head.headObjectId +
             L"（reflog 也保留着这条记录）。本程序不会自动恢复，也不会删除 reflog。\n";
  preview += L"这只移动本机的分支引用：不产生反向提交，不丢弃任何改动，不触碰远端，"
             L"更不会自动 force push。\n";
  if (plan.requiresForce) {
    std::wstring joined;
    for (size_t index = 0; index < forceReasons.size(); ++index) {
      if (index > 0) {
        joined += L"；";
      }
      joined += forceReasons[index];
    }
    preview += L"\n需要明确确认的风险：" + joined + L"。\n";
    preview += L"要继续请点击“强制撤回（仅本地）”。那个按钮只是确认你接受上述风险，"
               L"命令本身与上面展示的一条不差、不会改用更激烈的手段。";
  } else {
    preview += L"确定要执行吗？取消不会打开命令窗口，也不会改动仓库。";
  }

  plan.stateChangeNote = [&]() -> std::wstring {
    const CapturedSnapshot& captured = input.captured;
    if (!captured.valid) {
      return {};
    }
    const bool headChanged = captured.hasHead != head.headResolved ||
                             !ShaDisplaysMatch(captured.shortSha, ShortObjectId(head.headObjectId));
    const bool stagedChanged = captured.stagedItems != model.staged.size();
    if (!headChanged && !stagedChanged) {
      return {};
    }
    std::wstring note = L"界面原先显示的仓库现状与刚刚读回的已经不同：";
    if (headChanged) {
      note += L"HEAD 从「" +
              (captured.shortSha.empty() ? std::wstring(L"尚无提交") : captured.shortSha) + L"」变成「" +
              ShortObjectId(head.headObjectId) + L"」；";
    }
    if (stagedChanged) {
      note += L"已暂存的条目从 " + std::to_wstring(captured.stagedItems) + L" 项变成 " +
              std::to_wstring(model.staged.size()) + L" 项；";
    }
    note += L"下面的确认内容以刚刚读回的为准。请复核后再决定。";
    return note;
  }();
  if (!plan.stateChangeNote.empty()) {
    preview = plan.stateChangeNote + L"\n\n" + preview;
  }
  plan.previewText = std::move(preview);

  std::wstring notice = L"只移动分支引用（仅本地）：索引与工作区保持原样，原提交 " +
                        ShortObjectId(head.headObjectId) + L" 可用 reflog 找回。";
  if (input.workflow.indexLocked) {
    notice += L"（注意：仓库里已有 index.lock，另一个 Git 进程可能正在写索引；撤回本身只动引用，"
              L"但结果以 Git 的回答为准）";
  }
  plan.notice = std::move(notice);

  plan.restoreHint = L"原提交 " + ShortObjectId(head.headObjectId) + L"（完整 ID " + head.headObjectId +
                     L"）已撤回；如需找回，可执行 git reset --soft " + head.headObjectId +
                     L"，或用 git reflog 查看更早的位置。";
  return plan;
}

}  // namespace gc::git
