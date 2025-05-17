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

// 把参数数组拼成一行可复制、可自行执行的命令（路径含空格时给 -C 参数加引号）。
std::wstring FormatCopyableCommand(std::wstring_view repositoryRoot,
                                   const std::vector<std::wstring>& arguments) {
  std::wstring line = L"git -C \"";
  line += repositoryRoot;
  line += L"\"";
  for (const std::wstring& argument : arguments) {
    line.push_back(L' ');
    line += argument;
  }
  return line;
}

}  // namespace

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
  if (queries.workflowProbed && queries.workflow.HasSpecialFlowInProgress()) {
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
    // 引用明确不存在。无论恢复方向是删除还是重建，都没有一条「带预期旧值」的命令能安全表达。
    std::wstring why = L"记录里那条分支引用（" + clues.branchRef +
                       L"）现在已经不存在了。恢复它要么是把一个不存在的引用再删一次（无事可做），"
                       L"要么是在没有预期旧值核对的情况下重建它——后者不属于本程序会自动执行的原子引用"
                       L"回退。本程序不猜测它该落在哪，也不自动重建引用。";
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
    plan.targetRef = clues.branchRef;
    plan.expectedOldObjectId = clues.expectedCurrentOid;
    plan.newObjectId = clues.isRootDeletion ? std::wstring() : clues.moveToOid;
    std::vector<std::wstring> arguments;
    if (clues.isRootDeletion) {
      arguments = {L"update-ref", L"-d", L"-m", std::wstring(kRestoreReflogReason), clues.branchRef,
                   clues.expectedCurrentOid};
    } else {
      arguments = {L"update-ref", L"--create-reflog", L"-m", std::wstring(kRestoreReflogReason),
                   clues.branchRef, clues.moveToOid, clues.expectedCurrentOid};
    }
    plan.copyableCommand = FormatCopyableCommand(clues.repositoryRoot, arguments);
    std::wstring why = L"这条分支现在指着 " + ShortObjectId(currentOid) + L"，已经不是这次操作留下的位置（" +
                       ShortObjectId(clues.expectedCurrentOid) + L"）。说明那次操作之后分支又被别的动作"
                       L"推进或改动过——按记录直接恢复会落错位置，本程序不自动执行。";
    if (!clues.originalNote.empty()) {
      why += L"\n\n记录里的恢复线索：" + clues.originalNote;
    }
    why += L"\n\n下面这条命令带着「预期旧值 " + clues.expectedCurrentOid +
           L"」，此刻发出会被 Git 拒绝（引用已不在那个值上），因而不会误改任何东西；它只在你确认现场后"
           L"自行复制执行时有意义：\n  " + plan.copyableCommand;
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
  plan.targetRef = clues.branchRef;
  plan.expectedOldObjectId = clues.expectedCurrentOid;
  if (clues.isRootDeletion) {
    plan.arguments = {L"update-ref", L"-d", L"-m", std::wstring(kRestoreReflogReason), clues.branchRef,
                      clues.expectedCurrentOid};
    plan.commandLabel = L"git update-ref -d";
    plan.newObjectId.clear();
    plan.notice = L"只删除分支引用 " + clues.branchRef + L"（仅本地，带预期旧值 " +
                  ShortObjectId(clues.expectedCurrentOid) + L"）：索引与工作区保持原样。";
  } else {
    plan.arguments = {L"update-ref", L"--create-reflog", L"-m", std::wstring(kRestoreReflogReason),
                      clues.branchRef, clues.moveToOid, clues.expectedCurrentOid};
    plan.newObjectId = clues.moveToOid;
    plan.notice = L"只移动分支引用 " + clues.branchRef + L"（仅本地，带预期旧值 " +
                  ShortObjectId(clues.expectedCurrentOid) + L"）：索引与工作区保持原样。";
  }
  plan.copyableCommand = FormatCopyableCommand(clues.repositoryRoot, plan.arguments);

  std::wstring preview;
  preview += L"将在命令窗口里执行（工作目录：" + clues.repositoryRoot + L"）：\n";
  preview += L"  " + plan.copyableCommand + L"\n\n";
  preview += L"仓库：" + clues.repositoryRoot + L"\n";
  preview += L"目标分支：" + clues.branchRef + L"\n";
  preview += L"该分支现在指着：" + ShortObjectId(currentOid) + L"（完整 ID " + currentOid + L"）\n";
  if (clues.isRootDeletion) {
    preview += L"恢复动作：把这条分支引用删回「该分支本不存在」的状态（对应那次操作创建/移动了它的第一份内容）。\n";
  } else {
    preview += L"恢复动作：把这条分支引用挪回 " + ShortObjectId(clues.moveToOid) + L"（完整 ID " +
               clues.moveToOid + L"）。\n";
  }
  preview += L"预期旧值核对：Git 只在上面那条引用确实还指向 " + clues.expectedCurrentOid +
             L" 时才动它，值对不上就以非 0 原样拒绝。这一步是原子核对，不是先读后写。\n";
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

std::wstring DescribeRestoreRecheckMismatch(const GitQueryResult& recheckBranch,
                                            std::wstring_view expectedOldObjectId,
                                            std::wstring_view branchRef) {
  const UndoQueryRead read = ReadUndoQuery(recheckBranch);
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
  return {};
}

}  // namespace gc::git
