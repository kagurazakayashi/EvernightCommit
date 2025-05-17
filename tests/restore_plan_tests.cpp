#include "git/restore_plan.h"

#include <string>
#include <vector>

#include "git/commit_plan.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

namespace g = gc::git;

const std::wstring kB = L"refs/heads/main";
const std::wstring kOidC(40, L'c');  // 操作后该分支应指的对象（恢复前提 / 预期旧值）
const std::wstring kOidP(40, L'b');  // 要挪回去的对象（新值）
const std::wstring kOidD(40, L'd');  // 分支被别的动作挪去的位置

g::GitQueryResult Answered(const std::wstring& firstLine) {
  g::GitQueryResult r;
  r.started = true;
  r.exited = true;
  r.outputComplete = true;
  r.exitCode = 0;
  r.utf16Output = firstLine + L"\n";
  return r;
}

g::GitQueryResult NoResult() {
  g::GitQueryResult r;
  r.started = true;
  r.exited = true;
  r.outputComplete = true;
  r.exitCode = 1;  // --quiet 系：退出码 1 + 空输出 = 明确的「没有」
  return r;
}

g::GitQueryResult Failed() {
  g::GitQueryResult r;
  r.started = true;
  r.exited = true;
  r.outputComplete = true;
  r.exitCode = 128;
  r.utf16Error = L"fatal: not a valid object name";
  return r;
}

g::RestoreClues MakeClues() {
  g::RestoreClues clues;
  clues.valid = true;
  clues.repositoryRoot = L"P:\\repo\\proj";
  clues.branchRef = kB;
  clues.expectedCurrentOid = kOidC;
  clues.moveToOid = kOidP;
  clues.isRootDeletion = false;
  clues.originalNote = L"当初撤回把分支从 c… 挪到了 p…";
  return clues;
}

g::RestorePreflightQueries MakeQueries(const g::GitQueryResult& branch, const g::GitQueryResult& moveTo,
                                       bool moveToRan = true) {
  g::RestorePreflightQueries queries;
  queries.branchRan = true;
  queries.branchValue = branch;
  queries.moveToRan = moveToRan;
  queries.moveToValue = moveTo;
  return queries;
}

}  // namespace

GC_TEST(restore_plan_arguments_shape) {
  const std::vector<std::wstring> branchArgs = g::BuildRestoreBranchValueArguments(L"P:\\repo", kB);
  GC_CHECK(!branchArgs.empty());
  GC_CHECK_MESSAGE(branchArgs.back() == kB + L"^{commit}", "分支取值用 <ref>^{commit} 剥离问法");
  // 不合格的引用名根本不送进 Git。
  GC_CHECK(g::BuildRestoreBranchValueArguments(L"P:\\repo", L"main").empty());
  GC_CHECK(g::BuildRestoreBranchValueArguments(L"P:\\repo", L"bad name").empty());
  GC_CHECK(g::BuildRestoreBranchValueArguments(L"", kB).empty());
  // 对象可达性复用撤回同族查询：合格 ID 有参数，非 ID 返回空。
  GC_CHECK(!g::BuildRestoreTargetObjectArguments(L"P:\\repo", kOidP).empty());
  GC_CHECK(g::BuildRestoreTargetObjectArguments(L"P:\\repo", L"not-hex!").empty());
}

GC_TEST(restore_plan_feasible_ref_move) {
  const g::RestoreClues clues = MakeClues();
  const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK_MESSAGE(plan.feasibility == g::RestoreFeasibility::feasible, "分支仍在预期位置且目标可达 → 可行");
  GC_CHECK(!plan.blocked);
  GC_CHECK(plan.targetRef == kB);
  GC_CHECK(plan.expectedOldObjectId == kOidC);
  GC_CHECK(plan.newObjectId == kOidP);
  GC_CHECK_MESSAGE(plan.arguments.size() == 7, "update-ref --create-reflog -m <reason> <ref> <new> <old>");
  GC_CHECK(plan.arguments.front() == L"update-ref");
  GC_CHECK(plan.copyableCommand.find(L"git -C") == 0);
  GC_CHECK(plan.operationId == L"restore-ref");
  GC_CHECK(!plan.previewText.empty());
}

GC_TEST(restore_plan_feasible_root_deletion) {
  g::RestoreClues clues = MakeClues();
  clues.isRootDeletion = true;
  clues.moveToOid.clear();
  // 删除形态根本不跑「要挪去的对象」那条查询。
  const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), g::GitQueryResult{}, false);
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::feasible);
  GC_CHECK(!plan.blocked);
  GC_CHECK_MESSAGE(plan.arguments.size() == 6, "update-ref -d -m <reason> <ref> <old>");
  GC_CHECK(plan.arguments[1] == L"-d");
  GC_CHECK(plan.newObjectId.empty());
}

GC_TEST(restore_plan_root_deletion_with_move_target_is_undetermined) {
  // 线索自相矛盾：既标了删除又带要挪去的对象 → 拒绝，不猜。
  g::RestoreClues clues = MakeClues();
  clues.isRootDeletion = true;  // moveToOid 仍非空
  const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::undetermined);
  GC_CHECK(plan.blocked);
}

GC_TEST(restore_plan_branch_moved) {
  const g::RestoreClues clues = MakeClues();
  const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidD), Answered(kOidP));
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::branchMoved);
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.observedCurrentOid == kOidD);
  // 分支移动时仍给一条带预期旧值的命令供复制，但明确 blocked（不自动）。
  GC_CHECK(!plan.copyableCommand.empty());
  GC_CHECK(plan.explanation.find(L"已经不是这次操作留下的位置") != std::wstring::npos);
}

GC_TEST(restore_plan_branch_missing) {
  const g::RestoreClues clues = MakeClues();
  const g::RestorePreflightQueries queries = MakeQueries(NoResult(), Answered(kOidP));
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::branchMissing);
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.observedCurrentOid.empty());
}

GC_TEST(restore_plan_move_target_missing) {
  const g::RestoreClues clues = MakeClues();
  const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), NoResult());
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::moveToMissing);
  GC_CHECK(plan.blocked);
}

GC_TEST(restore_plan_undetermined_on_query_failure) {
  const g::RestoreClues clues = MakeClues();
  {
    const g::RestorePreflightQueries queries = MakeQueries(Failed(), Answered(kOidP));
    GC_CHECK(g::BuildRestorePlan(clues, queries).feasibility == g::RestoreFeasibility::undetermined);
  }
  {
    // 非删除形态但没跑「要挪去的对象」那条查询：不知道，不发。
    const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), g::GitQueryResult{}, false);
    GC_CHECK(g::BuildRestorePlan(clues, queries).feasibility == g::RestoreFeasibility::undetermined);
  }
}

GC_TEST(restore_plan_undetermined_on_bad_clues) {
  {
    g::RestoreClues clues = MakeClues();
    clues.expectedCurrentOid = L"short";  // 不是完整对象 ID
    const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
    GC_CHECK(g::BuildRestorePlan(clues, queries).feasibility == g::RestoreFeasibility::undetermined);
  }
  {
    g::RestoreClues clues = MakeClues();
    clues.branchRef = L"main";  // 不是 refs/ 完整引用
    const g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
    GC_CHECK(g::BuildRestorePlan(clues, queries).feasibility == g::RestoreFeasibility::undetermined);
  }
}

GC_TEST(restore_plan_refuses_when_flow_in_progress) {
  const g::RestoreClues clues = MakeClues();
  g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
  queries.workflowProbed = true;
  queries.workflow.mergeInProgress = true;
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::inProgressFlow);
  GC_CHECK(plan.blocked);
}

GC_TEST(restore_plan_non_hex_current_is_undetermined) {
  // 分支读回的当前指向不是合格的完整对象 ID 时判「未能判定」，绝不当成「已移动」或「可行」。
  // Git 的对象 ID 恒小写十六进制；这里用一串非法字符把这道形态门禁钉住。
  const g::RestoreClues clues = MakeClues();
  const std::wstring bogus(40, L'Z');
  const g::RestorePreflightQueries queries = MakeQueries(Answered(bogus), Answered(kOidP));
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::undetermined);
  GC_CHECK(plan.blocked);
}

GC_TEST(restore_plan_recheck_mismatch) {
  GC_CHECK_MESSAGE(g::DescribeRestoreRecheckMismatch(Answered(kOidC), kOidC, kB).empty(),
                   "复核仍一致 → 空串放行");
  GC_CHECK(g::DescribeRestoreRecheckMismatch(Answered(kOidD), kOidC, kB).find(L"又被挪走") !=
           std::wstring::npos);
  GC_CHECK(g::DescribeRestoreRecheckMismatch(NoResult(), kOidC, kB).find(L"已经不存在") !=
           std::wstring::npos);
  GC_CHECK(g::DescribeRestoreRecheckMismatch(Failed(), kOidC, kB).find(L"没能完成") != std::wstring::npos);
}
