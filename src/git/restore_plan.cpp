#include "git/restore_plan.h"

#include <string>
#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId

namespace gc::git {
namespace {

// 进 reflog 的说明：纯 ASCII、不含空格与引号，一个 token 就是一条命令行参数。
constexpr std::wstring_view kRestoreReflogReason = L"EvernightCommit:restore-ref";

std::wstring LowerAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
  }
  return result;
}

bool SameObjectId(std::wstring_view left, std::wstring_view right) {
  return !left.empty() && !right.empty() && LowerAscii(left) == LowerAscii(right);
}

}  // namespace

// 把一份恢复线索复现成「实际会发出的那串参数」与三种表示（R3）。
// 这里是唯一的构造点：可行方案、被挡方案、界面上从记录直接复制，都走这一份，
// 不会再出现「一处修好了、另一处仍把引用名裸拼进命令行」。
RestoreCopyText DescribeRestoreCopy(const RestoreClues& clues, ShellDialect dialect) {
  RestoreCopyText copy;
  copy.dialectLabel = std::wstring(ShellDialectLabel(dialect));
  if (!clues.valid || clues.repositoryRoot.empty()) {
    copy.refusal = L"这条记录没有可用的仓库工作区根，连要恢复哪个仓库都说不清，因此不产生任何命令文本。";
    return copy;
  }
  if (!IsSafeUndoTargetRef(clues.branchRef)) {
    copy.refusal = L"记录里的分支引用名不合格（读到的是「" + clues.branchRef +
                   L"」），不把它复现成任何命令。";
    return copy;
  }
  if (!LooksLikeFullObjectId(clues.expectedCurrentOid)) {
    copy.refusal = L"记录里缺少合格的「操作后该分支应指的对象」完整 ID，无法复现那条带预期旧值的命令。";
    return copy;
  }
  const bool deletion = clues.isRootDeletion;
  if (deletion && !clues.moveToOid.empty()) {
    copy.refusal = L"这条记录的恢复线索自相矛盾：既标了「回到无引用」又带了一个要挪去的对象。";
    return copy;
  }
  if (!deletion && !LooksLikeFullObjectId(clues.moveToOid)) {
    copy.refusal = L"记录里没有合格的「要挪回去的对象」完整 ID，无法复现那条命令。";
    return copy;
  }

  if (deletion) {
    copy.arguments = {L"update-ref", L"--no-deref", L"-d", L"-m",
                      std::wstring(kRestoreReflogReason), clues.branchRef, clues.expectedCurrentOid};
  } else {
    copy.arguments = {L"update-ref", L"--no-deref", L"--create-reflog", L"-m",
                      std::wstring(kRestoreReflogReason), clues.branchRef, clues.moveToOid,
                      clues.expectedCurrentOid};
  }

  // 数据数组之外再加一份「带上仓库落点」的同序列，供预览/复制：
  // 复制出来的那一行必须绑定到记录那个仓库，而不是用户粘贴时恰好所在的目录。
  std::vector<std::wstring> withDirectory;
  withDirectory.push_back(L"-C");
  withDirectory.push_back(clues.repositoryRoot);
  for (const std::wstring& argument : copy.arguments) {
    withDirectory.push_back(argument);
  }
  copy.cluesUsable = true;
  copy.previewCommand = FormatCommandPreview(L"git", withDirectory);
  copy.structuredFacts = FormatStructuredCommandFacts(L"git", withDirectory);
  const ShellCommandText shell = FormatShellCommand(L"git", withDirectory, dialect);
  if (shell.expressible) {
    copy.copyableCommand = shell.text;
    copy.copyNote = shell.note;
  } else {
    copy.copyRefusal = shell.refusal;
  }
  return copy;
}


std::vector<std::wstring> BuildRestoreBranchValueArguments(std::wstring_view repositoryDirectory,
                                                           std::wstring_view branchRef) {
  if (!IsSafeUndoTargetRef(branchRef) || repositoryDirectory.empty()) {
    return {};  // 引用名不合格或没有仓库落点：根本不送进 Git。
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   std::wstring(branchRef) + L"^{commit}"};
}

std::vector<std::wstring> BuildRestoreTargetObjectArguments(std::wstring_view repositoryDirectory,
                                                             std::wstring_view targetObjectId) {
  // 复用撤回预检里同族的 <ID>^{commit} 剥离问法：不另发明命令形态，已被真实 Git 夹具钉过。
  return BuildUndoParentObjectArguments(repositoryDirectory, targetObjectId);
}

std::wstring_view RestoreFeasibilityLabel(RestoreFeasibility feasibility) noexcept {
  switch (feasibility) {
    case RestoreFeasibility::undetermined:
      return L"未能判定";
    case RestoreFeasibility::inProgressFlow:
      return L"有流程停着，不能移动引用";
    case RestoreFeasibility::refNotMovable:
      return L"目标引用本身不能按名字移动";
    case RestoreFeasibility::branchMissing:
      return L"目标分支已不存在";
    case RestoreFeasibility::branchMoved:
      return L"目标分支已被挪到别处";
    case RestoreFeasibility::moveToMissing:
      return L"要挪回去的对象在本地读不到";
    case RestoreFeasibility::feasible:
      return L"可以按原子引用回退恢复";
  }
  return L"未能判定";
}

RestorePlan BuildRestorePlan(const RestoreClues& clues, const RestorePreflightQueries& queries) {
  RestorePlan plan;
  plan.operationId = L"restore-ref";
  plan.displayName = L"按记录恢复引用";
  plan.commandLabel = L"git update-ref";

  const auto conclude = [&](RestoreFeasibility feasibility, std::wstring why) {
    plan.feasibility = feasibility;
    plan.explanation = std::move(why);
    plan.blocked = true;
    return plan;
  };

  // ---- 线索自身要成立，否则连「恢复什么」都说不清 ----
  if (!clues.valid || clues.repositoryRoot.empty()) {
    return conclude(RestoreFeasibility::undetermined,
                    L"这条记录的恢复线索缺少可用的仓库工作区根，无法判断恢复目标。");
  }
  if (!IsSafeUndoTargetRef(clues.branchRef)) {
    return conclude(RestoreFeasibility::undetermined,
                    L"记录里的分支引用名不合格（读到的是「" + clues.branchRef +
                        L"」）。恢复只按完整引用名（refs/heads/…）移动一个分支，不碰别的。");
  }
  // 历史文件里的引用名按外部输入对待，光有名字合格还不够：默认形态的 `update-ref` 会追随符号引用
  // 去改「它指向的那条分支」，而这条分支也可能正被另一个 linked worktree 用着。
  // 这两件事必须这一轮问过；没问过（refKindRan=false）不是「没问题」，而是「不知道」。
  {
    std::wstring refusal;
    if (!queries.refKindRan) {
      refusal = L"这一轮没能问出「" + clues.branchRef +
                L"」这个引用名本身的形态（是不是符号引用、有没有被别的工作树用着）。"
                L"不知道就不动它——恢复需要按名字移动且只移动这一条，因此本次不产生任何命令。";
    } else {
      refusal = DescribeRefIntegrityRefusal(
          InterpretRefIntegrity(queries.branchSymref, queries.worktrees, clues.branchRef,
                                queries.currentWorktreeRoot),
          clues.branchRef);
    }
    if (!refusal.empty()) {
      return conclude(RestoreFeasibility::refNotMovable, refusal);
    }
  }
  if (!LooksLikeFullObjectId(clues.expectedCurrentOid)) {
    return conclude(RestoreFeasibility::undetermined,
                    L"记录里缺少一个合格的「操作后该分支应指的对象」完整 ID，无法核对这次恢复的前提。");
  }
  if (clues.isRootDeletion) {
    if (!clues.moveToOid.empty()) {
      return conclude(RestoreFeasibility::undetermined,
                      L"这条记录的恢复线索自相矛盾：既标了「回到无引用」又带了一个要挪去的对象。");
    }
  } else if (!LooksLikeFullObjectId(clues.moveToOid)) {
    return conclude(RestoreFeasibility::undetermined,
                    L"记录里没有合格的「要挪回去的对象」完整 ID，无法确定恢复目标。");
  }

  // ---- 有流程停着：那种状态下引用被流程当进度记录用，移动引用会搅乱现场 ----
  // 先问「这一轮读没读到」：六个 false 只有在每个痕迹都问出确定答案时才等于「没有流程」。
  // 目录读不动、或者压根没探——那是「不能确定」，绝不能走可执行恢复路径。
  if (!queries.workflowProbed || !queries.workflowReadable) {
    return conclude(RestoreFeasibility::undetermined,
                    L"这一轮没能确定这个仓库里停没停着 Git 流程（" +
                        (queries.workflowFailure.empty()
                             ? std::wstring(queries.workflowProbed ? L"部分痕迹读不出确定答案"
                                                                   : L"没有发起这次探测")
                             : queries.workflowFailure) +
                        L"）。「看不见」不是「没有」，在不能确定的现场移动引用会把别人的流程搅乱，"
                        L"因此本次不产生任何命令。请点“刷新”后重新查看这条记录。");
  }
  if (queries.workflow.HasSpecialFlowInProgress()) {
    return conclude(RestoreFeasibility::inProgressFlow,
                    L"这个仓库里有 Git 流程还没走完：" + queries.workflow.SpecialFlowText() +
                        L"。那种状态下分支引用正被当作流程进度使用，此时按记录移动引用会把现场搅乱。"
                        L"请在原来的 Git 命令里把流程走完（git merge/rebase/cherry-pick 的 "
                        L"--continue 或 --abort），再重新查看这条记录。");
  }

  // ---- 读回这条分支现在到底指着什么 ----
  if (!queries.branchRan) {
    return conclude(RestoreFeasibility::undetermined, L"没有读取这条分支的当前指向，无法判断这次恢复是否仍成立。");
  }
  const UndoQueryRead branchRead = ReadUndoQuery(queries.branchValue);
  if (branchRead.outcome == UndoQueryOutcome::failed) {
    return conclude(
        RestoreFeasibility::undetermined,
        L"没能问出这条分支现在指着什么：" +
            (branchRead.detail.empty() ? std::wstring(L"查询未成功") : branchRead.detail) +
            L"。读不回来就当不知道，绝不据此判定「可以恢复」。");
  }
  if (branchRead.outcome == UndoQueryOutcome::noResult) {
    // 引用明确不存在。删除形态无事可做；重建形态虽然 Git 也支持（用零值旧 OID 核对「确实还不存在」
    // 的创建形态），但那不属于本程序在这里承诺的「把一条引用挪回它确认过的位置」，
    // 所以本程序不提供这个入口——这是支持范围的说法，不是 Git 做不到。
    std::wstring why = L"记录里那条分支引用（" + clues.branchRef +
                       L"）现在已经不存在了。恢复它要么是把一个不存在的引用再删一次（无事可做），"
                       L"要么是按「创建」形态重建它——后者不在本程序自动执行的范围内"
                       L"（Git 支持以零值旧 OID 核对「还不存在」的创建，本程序只是不提供这个入口），"
                       L"也不猜测它该落在哪。";
    if (!clues.originalNote.empty()) {
      why += L"\n\n记录里的恢复线索：" + clues.originalNote;
    }
    why += L"\n\n如果确需恢复，可以用命令行按 reflog 或对象完整 ID 自行判断（本程序只把线索说清楚，不代执行）。";
    return conclude(RestoreFeasibility::branchMissing, why);
  }
  // answered：拿到当前完整提交 ID。
  const std::wstring& currentOid = branchRead.firstLine;
  if (!LooksLikeFullObjectId(currentOid)) {
    return conclude(RestoreFeasibility::undetermined,
                    L"读回的分支当前指向不是合格的完整对象 ID，无法核对，不执行任何恢复。");
  }
  plan.observedCurrentOid = currentOid;
  if (!SameObjectId(currentOid, clues.expectedCurrentOid)) {
    // 分支还在，但已经不在这次操作留下的位置上——现场已变。给一条带预期旧值的命令供复制，
    // 但明确标注：照发也会被 Git 用预期旧值挡下，因此这里不自动执行。
    const RestoreCopyText copy = DescribeRestoreCopy(clues, ShellDialect::cmdInteractive);
    plan.targetRef = clues.branchRef;
    plan.expectedOldObjectId = clues.expectedCurrentOid;
    plan.newObjectId = clues.isRootDeletion ? std::wstring() : clues.moveToOid;
    plan.arguments = copy.arguments;  // 与「真要执行时」同一份数据形态，界面无需再拼一遍
    plan.previewCommand = copy.previewCommand;
    plan.structuredCopy = copy.structuredFacts;
    plan.copyableCommand = copy.copyableCommand;
    plan.copyRefusal = copy.copyRefusal;
    plan.copyNote = copy.copyNote;
    plan.dialectLabel = copy.dialectLabel;
    std::wstring why = L"这条分支现在指着 " + ShortObjectId(currentOid) + L"，已经不是这次操作留下的位置（" +
                       ShortObjectId(clues.expectedCurrentOid) + L"）。说明那次操作之后分支又被别的动作"
                       L"推进或改动过——按记录直接恢复会落错位置，本程序不自动执行。";
    if (!clues.originalNote.empty()) {
      why += L"\n\n记录里的恢复线索：" + clues.originalNote;
    }
    why += L"\n\n下面这串参数带着「预期旧值 " + clues.expectedCurrentOid +
           L"」，此刻发出会被 Git 拒绝（引用已不在那个值上），因而不会误改任何东西；它只在你确认现场后"
           L"自行复制执行时有意义。";
    if (!plan.copyableCommand.empty()) {
      why += L"\n\n可粘贴到" + plan.dialectLabel + L"的一行（复制不等于执行）：\n  " +
             plan.copyableCommand;
    } else {
      why += L"\n\n这一串参数不能安全地粘成一行（" + plan.copyRefusal +
             L"）。本程序不替换任何字符、也不替你换 shell；可以复制下面这份字段清单，"
             L"在终端里自己按段输入：";
    }
    why += L"\n" + plan.previewCommand;
    plan.feasibility = RestoreFeasibility::branchMoved;
    plan.explanation = std::move(why);
    plan.blocked = true;
    return plan;
  }

  // ---- 分支仍停在预期位置：非删除形态还要确认「要挪回去的对象」本地可达 ----
  if (!clues.isRootDeletion) {
    if (!queries.moveToRan) {
      return conclude(RestoreFeasibility::undetermined, L"没有核对「要挪回去的那个对象」在不在本地，无法安全恢复。");
    }
    const UndoQueryRead moveToRead = ReadUndoQuery(queries.moveToValue);
    if (moveToRead.outcome == UndoQueryOutcome::failed) {
      return conclude(
          RestoreFeasibility::undetermined,
          L"没能问出要挪回去的那个对象（" + ShortObjectId(clues.moveToOid) + L"）在不在本地：" +
              (moveToRead.detail.empty() ? std::wstring(L"查询未成功") : moveToRead.detail));
    }
    if (moveToRead.outcome == UndoQueryOutcome::noResult) {
      return conclude(
          RestoreFeasibility::moveToMissing,
          L"要挪回去的那个对象（" + ShortObjectId(clues.moveToOid) + L"，完整 ID " + clues.moveToOid +
              L"）在本地读不到，或并不是一个能解出的提交对象：它可能已被回收，或这份历史本就不完整。"
              L"本程序不把分支引用挪向一个读不到的对象，也不会自动联网补全。请先用你自己的方式补全历史"
              L"（git fetch 等），再重新查看这条记录。");
    }
    if (!SameObjectId(moveToRead.firstLine, clues.moveToOid)) {
      return conclude(
          RestoreFeasibility::undetermined,
          L"问「" + ShortObjectId(clues.moveToOid) + L"^{commit}」时 Git 答的是别的对象（" +
              ShortObjectId(moveToRead.firstLine) + L"），形态超出能安全判读的范围，不执行恢复。");
    }
  }

  // ---- feasible：产出一条与撤回同族的、带预期旧值的原子命令 ----
  // 参数、预览、可粘贴文本、字段清单一律取自 DescribeRestoreCopy：确认框里说的那一串、
  // 实际发出的那一条、以及界面上「复制恢复命令」复制的那一份，从此是同一份构造。
  const RestoreCopyText copy = DescribeRestoreCopy(clues, ShellDialect::cmdInteractive);
  if (!copy.cluesUsable) {
    return conclude(RestoreFeasibility::undetermined, copy.refusal);
  }
  plan.targetRef = clues.branchRef;
  plan.expectedOldObjectId = clues.expectedCurrentOid;
  plan.arguments = copy.arguments;
  plan.previewCommand = copy.previewCommand;
  plan.structuredCopy = copy.structuredFacts;
  plan.copyableCommand = copy.copyableCommand;
  plan.copyRefusal = copy.copyRefusal;
  plan.copyNote = copy.copyNote;
  plan.dialectLabel = copy.dialectLabel;
  if (clues.isRootDeletion) {
    plan.commandLabel = L"git update-ref -d";
    plan.newObjectId.clear();
    plan.notice = L"只删除分支引用 " + clues.branchRef + L"（仅本地，按这个名字本身删除、不追随符号引用，"
                  L"并带预期旧值 " + ShortObjectId(clues.expectedCurrentOid) +
                  L"）：索引与工作区保持原样。";
  } else {
    plan.commandLabel = L"git update-ref";
    plan.newObjectId = clues.moveToOid;
    plan.notice = L"只移动分支引用 " + clues.branchRef + L"（仅本地，按这个名字本身移动、不追随符号引用，"
                  L"并带预期旧值 " + ShortObjectId(clues.expectedCurrentOid) +
                  L"）：索引与工作区保持原样。";
  }

  std::wstring preview;
  preview += L"将在命令窗口里执行（工作目录：" + clues.repositoryRoot + L"）。下面逐段列出实际送进 Git "
             L"的参数，方括号里是数据、不是 shell 语法：\n";
  preview += L"  " + plan.previewCommand + L"\n";
  if (!plan.copyableCommand.empty()) {
    preview += L"同一串参数的" + plan.dialectLabel + L"可粘贴形态（本程序不会替你执行它）：\n  " +
               plan.copyableCommand + L"\n";
  } else {
    preview += L"这一串参数不能安全地粘成一行（" + plan.copyRefusal +
               L"），因此这里不提供粘贴文本，也不替换任何字符；参数仍按上面的数组原样送进 Git，"
               L"这条链路的执行不经过任何 shell 字符串。\n";
  }
  preview += L"\n仓库：" + clues.repositoryRoot + L"\n";
  preview += L"目标分支：" + clues.branchRef + L"\n";
  preview += L"该分支现在指着：" + ShortObjectId(currentOid) + L"（完整 ID " + currentOid + L"）\n";
  if (clues.isRootDeletion) {
    preview += L"恢复动作：把这条分支引用删回「该分支本不存在」的状态（对应那次操作创建/移动了它的第一份内容）。\n";
  } else {
    preview += L"恢复动作：把这条分支引用挪回 " + ShortObjectId(clues.moveToOid) + L"（完整 ID " +
               clues.moveToOid + L"）。\n";
  }
  preview += L"预期旧值核对：Git 只在上面那条引用**本身**（--no-deref：不解引用）确实还指向 " +
             clues.expectedCurrentOid + L" 时才动它，值对不上就以非 0 原样拒绝。"
             L"这一步是原子核对，不是先读后写；确认之间这个名字被换成符号引用时，"
             L"Git 读到的不是那个对象 ID，于是同样被拒。\n";
  preview += L"这条分支现在归哪个工作树用，也已经在本轮按 git worktree list 问过：被别的"
             L"工作树检用时直接拒绝，不跨工作树动分支。\n";
  preview += L"影响：这条命令只移动（或删除）这一个本地分支引用，索引与工作区一个字节都不动；"
             L"不产生反向提交，不丢弃任何改动，不触碰远端，更不会自动 force push。\n";
  if (!clues.originalNote.empty()) {
    preview += L"\n当初记录里留下的恢复线索：" + clues.originalNote + L"\n";
  }
  preview += L"\n确定要执行吗？取消不会打开命令窗口，也不会改动仓库。";
  plan.previewText = std::move(preview);

  plan.feasibility = RestoreFeasibility::feasible;
  plan.blocked = false;
  plan.explanation = plan.notice;
  return plan;
}

std::wstring DescribeRestoreRecheckMismatch(const RestoreRecheckScene& scene,
                                            std::wstring_view expectedOldObjectId,
                                            std::wstring_view branchRef) {
  // 复核问的是与预检同一组事实，任何一项读不回来或变了，都不允许拿旧确认去发命令。
  if (!scene.branchRan) {
    return L"确认后、执行前的复核没有发出这条分支现值的查询，本次没有执行任何命令。"
           L"看清现状后如仍要恢复，请重新查看这条记录再决定。";
  }
  const UndoQueryRead read = ReadUndoQuery(scene.branchValue);
  if (read.outcome == UndoQueryOutcome::failed) {
    return L"确认后、执行前的复核没能完成（" +
           (read.detail.empty() ? std::wstring(L"原因未知") : read.detail) +
           L"），本次没有执行任何命令。仓库状态正在重读，请看清现状后再来。";
  }
  if (read.outcome == UndoQueryOutcome::noResult) {
    return L"确认之后、执行之前，这条分支（" + std::wstring(branchRef) +
           L"）已经不存在了。本次没有执行任何命令；请按记录里写的对象完整 ID 自行判断后再操作。";
  }
  const std::wstring& currentOid = read.firstLine;
  if (!LooksLikeFullObjectId(currentOid)) {
    return L"复核读回的分支当前指向不合格，无法核对，本次没有执行任何命令。";
  }
  if (LowerAscii(currentOid) != LowerAscii(expectedOldObjectId)) {
    return L"确认之后、执行之前，分支又被挪走了（现在指 " + ShortObjectId(currentOid) + L"，而恢复前提是 " +
           ShortObjectId(expectedOldObjectId) + L"）。本次没有执行任何命令。看清现状后如仍要恢复，"
           L"请重新查看这条记录再决定。";
  }

  // 分支 OID 没动，不代表现场没变：外部可以开一个合并而完全不移动分支；也可以把这个名字
  // 改成符号引用，或把这条分支检出到另一个工作树。这三件事必须这一轮重新问。
  if (!scene.workflowProbed || !scene.workflowReadable) {
    return L"确认之后、执行之前，这一轮没能确定仓库里停没停着 Git 流程（" +
           (scene.workflowFailure.empty()
                ? std::wstring(scene.workflowProbed ? L"部分痕迹读不出确定答案" : L"没有发起这次探测")
                : scene.workflowFailure) +
           L"）。「看不见」不是「没有」，本次没有执行任何命令。";
  }
  if (scene.workflow.HasSpecialFlowInProgress()) {
    return L"确认之后、执行之前，这个仓库里出现了还没走完的 Git 流程：" +
           scene.workflow.SpecialFlowText() +
           L"。那种状态下引用正被流程当作进度使用，此时按记录移动引用会把现场搅乱，"
           L"本次没有执行任何命令。请在原来的 Git 命令里把流程走完，再重新查看这条记录。";
  }
  if (!scene.integrityRan) {
    return L"确认之后、执行之前，没能重新问出这条引用自身的形态与占用情况，"
           L"不能确定它还是不是当初那一条普通分支引用，本次没有执行任何命令。";
  }
  const std::wstring integrityRefusal = DescribeRefIntegrityRefusal(scene.integrity, branchRef);
  if (!integrityRefusal.empty()) {
    return L"确认之后、执行之前，这条分支引用的形态或占用情况已经不能放行，本次没有执行任何命令。\n" +
           integrityRefusal;
  }
  return {};
}

}  // namespace gc::git
