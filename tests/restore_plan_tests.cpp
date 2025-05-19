#include "git/restore_plan.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/commit_plan.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

namespace g = gc::git;

const std::wstring kB = L"refs/heads/main";
const std::wstring kRoot = L"P:\\repo\\proj";
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
  clues.repositoryRoot = kRoot;
  clues.branchRef = kB;
  clues.expectedCurrentOid = kOidC;
  clues.moveToOid = kOidP;
  clues.isRootDeletion = false;
  clues.originalNote = L"当初撤回把分支从 c… 挪到了 p…";
  return clues;
}

// 一条「这条分支只由本工作树检出」的 worktree list 回答（R2 起属于恢复的必需证据）。
g::GitQueryResult WorktreesHeldBy(const std::wstring& root) {
  g::GitQueryResult r;
  r.started = true;
  r.exited = true;
  r.outputComplete = true;
  r.exitCode = 0;
  r.utf16Output =
      L"worktree " + root + L"\nHEAD " + kOidC + L"\nbranch " + kB + L"\n\n";
  return r;
}

g::RestorePreflightQueries MakeQueries(const g::GitQueryResult& branch, const g::GitQueryResult& moveTo,
                                       bool moveToRan = true) {
  g::RestorePreflightQueries queries;
  queries.branchRan = true;
  queries.branchValue = branch;
  queries.moveToRan = moveToRan;
  queries.moveToValue = moveTo;
  // 引用形态与占用：默认给「问过且没问题」的答复，专门测拒绝的用例自己改这几项。
  queries.refKindRan = true;
  queries.branchSymref = NoResult();  // 明确「不是符号引用」
  queries.worktrees = WorktreesHeldBy(kRoot);
  queries.currentWorktreeRoot = kRoot;
  // 流程痕迹同样属于「必需证据」：没探到确定答案就不许走可执行恢复路径，
  // 因此基线里显式给出「问过且每个痕迹都能定论」。
  queries.workflowProbed = true;
  queries.workflowReadable = true;
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
  GC_CHECK_MESSAGE(plan.arguments.size() == 8, "update-ref --no-deref --create-reflog -m <reason> <ref> <new> <old>");
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
  GC_CHECK_MESSAGE(plan.arguments.size() == 7, "update-ref --no-deref -d -m <reason> <ref> <old>");
  GC_CHECK(plan.arguments[2] == L"-d");
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

namespace {

// 复核场景的「一切正常」基线：分支现值就是那份预期旧值、引用形态与占用都问过且没问题、
// 流程痕迹读得到且没有流程。下面每个用例只改自己那一处，确保测的是那一项。
g::RestoreRecheckScene HealthyScene(const g::GitQueryResult& branchValue) {
  g::RestoreRecheckScene scene;
  scene.branchRan = true;
  scene.branchValue = branchValue;
  scene.integrityRan = true;
  scene.integrity.branchRefUsable = true;
  scene.integrity.symrefRan = true;
  scene.integrity.worktreesRan = true;
  scene.integrity.worktreesReadable = true;
  scene.workflowProbed = true;
  scene.workflowReadable = true;
  return scene;
}

}  // namespace

GC_TEST(restore_plan_recheck_mismatch) {
  GC_CHECK(g::DescribeRestoreRecheckMismatch(HealthyScene(Answered(kOidC)), kOidC, kB).empty(),
           "复核仍一致 → 空串放行");
  GC_CHECK(g::DescribeRestoreRecheckMismatch(HealthyScene(Answered(kOidD)), kOidC, kB).find(L"又被挪走") !=
           std::wstring::npos);
  GC_CHECK(g::DescribeRestoreRecheckMismatch(HealthyScene(NoResult()), kOidC, kB).find(L"已经不存在") !=
           std::wstring::npos);
  GC_CHECK(g::DescribeRestoreRecheckMismatch(HealthyScene(Failed()), kOidC, kB).find(L"没能完成") !=
           std::wstring::npos);
  // 压根没问：不是「没问题」。
  g::RestoreRecheckScene notRun;
  GC_CHECK(g::DescribeRestoreRecheckMismatch(notRun, kOidC, kB).find(L"没有发出") != std::wstring::npos);
}

GC_TEST(restore_recheck_catches_everything_a_branch_oid_cannot_show) {
  // R4 的关键点：确认框停留期间现场可以变成「分支 OID 没动，但已经不能动它」的样子。
  // 只重问 OID 的复核会放行，这一版必须逐档拦下。
  const g::GitQueryResult same = Answered(kOidC);

  g::RestoreRecheckScene merged = HealthyScene(same);
  merged.workflow.mergeInProgress = true;
  const std::wstring mergeRefusal = g::DescribeRestoreRecheckMismatch(merged, kOidC, kB);
  GC_CHECK_MESSAGE(mergeRefusal.find(L"还没走完的 Git 流程") != std::wstring::npos,
                   "确认期间出现 MERGE_HEAD 而分支 OID 不变：必须拒绝");
  GC_CHECK(mergeRefusal.find(L"没有执行任何命令") != std::wstring::npos);

  g::RestoreRecheckScene flowUnread = HealthyScene(same);
  flowUnread.workflowReadable = false;
  flowUnread.workflowFailure = L"读不到 Git 目录里的 MERGE_HEAD 这一项";
  const std::wstring unreadRefusal = g::DescribeRestoreRecheckMismatch(flowUnread, kOidC, kB);
  GC_CHECK(unreadRefusal.find(L"没能确定") != std::wstring::npos);
  GC_CHECK(unreadRefusal.find(L"看不见") != std::wstring::npos);

  g::RestoreRecheckScene notProbed = HealthyScene(same);
  notProbed.workflowProbed = false;
  notProbed.workflowReadable = false;
  GC_CHECK(!g::DescribeRestoreRecheckMismatch(notProbed, kOidC, kB).empty());

  g::RestoreRecheckScene becameSymref = HealthyScene(same);
  becameSymref.integrity.symrefIsSymbolic = true;
  becameSymref.integrity.symrefTarget = L"refs/heads/other";
  GC_CHECK(g::DescribeRestoreRecheckMismatch(becameSymref, kOidC, kB).find(L"符号引用") !=
           std::wstring::npos);

  g::RestoreRecheckScene becameHeld = HealthyScene(same);
  becameHeld.integrity.worktreeHolder = L"P:\\另一个工作树";
  GC_CHECK(g::DescribeRestoreRecheckMismatch(becameHeld, kOidC, kB).find(L"另一个工作树") !=
           std::wstring::npos);

  g::RestoreRecheckScene integrityNotRun = HealthyScene(same);
  integrityNotRun.integrityRan = false;
  GC_CHECK(g::DescribeRestoreRecheckMismatch(integrityNotRun, kOidC, kB).find(L"没能重新问出") !=
           std::wstring::npos);
}

GC_TEST(restore_plan_refuses_unreadable_workflow_markers) {
  // 预检那一侧同样：流程痕迹读不出确定答案时，不能走可执行恢复路径。
  const g::RestoreClues clues = MakeClues();
  g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
  queries.workflowProbed = true;
  queries.workflowReadable = false;
  queries.workflowFailure = L"读不到这个仓库的 Git 目录";
  const g::RestorePlan plan = g::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::undetermined);
  GC_CHECK(plan.explanation.find(L"看不见") != std::wstring::npos);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.previewText.empty());

  g::RestorePreflightQueries neverProbed = MakeQueries(Answered(kOidC), Answered(kOidP));
  neverProbed.workflowProbed = false;
  neverProbed.workflowReadable = false;
  const g::RestorePlan plan2 = g::BuildRestorePlan(clues, neverProbed);
  GC_CHECK(plan2.blocked);
  GC_CHECK(plan2.feasibility == g::RestoreFeasibility::undetermined);
}

// ---- R2：恢复目标只能是一条本地分支，且不追随符号引用、不跨工作树 ----

GC_TEST(restore_plan_refuses_refs_outside_local_branch_namespace) {
  // 历史文件里的引用名按外部输入对待：标签、远端跟踪引用都不属于「本地分支」这条契约。
  const wchar_t* const notABranch[] = {
      L"refs/tags/v1.0", L"refs/remotes/origin/main", L"refs/notes/commits", L"HEAD", L"main",
      L"refs/heads/123", L"refs/heads/.hidden", L"refs/heads/a.lock",
  };
  for (const wchar_t* ref : notABranch) {
    g::RestoreClues clues = MakeClues();
    clues.branchRef = ref;
    const g::RestorePlan plan = g::BuildRestorePlan(clues, MakeQueries(Answered(kOidC), Answered(kOidP)));
    GC_CHECK_MESSAGE(plan.blocked, "非本地分支命名空间必须整条拒绝");
    GC_CHECK(plan.feasibility == g::RestoreFeasibility::undetermined);
    GC_CHECK(plan.arguments.empty());
    GC_CHECK(plan.explanation.find(L"refs/heads/") != std::wstring::npos);
  }
  // Git 允许但对 shell 敏感的字符仍然通过这一层（界面走参数数组）：R3 管的是复制文本，不是这里。
  g::RestoreClues legal = MakeClues();
  legal.branchRef = L"refs/heads/demo&calc&rem";
  g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
  queries.worktrees = WorktreesHeldBy(kRoot);  // 检出名与 branchRef 一致才算「本工作树自己」
  const g::RestorePlan legalPlan = g::BuildRestorePlan(legal, queries);
  GC_CHECK_MESSAGE(legalPlan.explanation.find(L"引用名不合格") == std::wstring::npos,
                   "合法但含特殊字符的分支名不该被形态校验挡下");
}

GC_TEST(restore_plan_refuses_unmovable_reference) {
  const g::RestoreClues clues = MakeClues();

  // 没问过引用自身的形态：不知道 ≠ 没问题。
  g::RestorePreflightQueries unasked = MakeQueries(Answered(kOidC), Answered(kOidP));
  unasked.refKindRan = false;
  g::RestorePlan plan = g::BuildRestorePlan(clues, unasked);
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::refNotMovable);
  GC_CHECK(plan.explanation.find(L"没能问出") != std::wstring::npos);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.copyableCommand.empty());

  // 它自己是一条符号引用：不加 --no-deref 的话 Git 会去改它指向的那条分支。
  g::RestorePreflightQueries symref = MakeQueries(Answered(kOidC), Answered(kOidP));
  symref.branchSymref = Answered(L"refs/heads/victim");
  plan = g::BuildRestorePlan(clues, symref);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::refNotMovable);
  GC_CHECK(plan.explanation.find(L"符号引用") != std::wstring::npos);
  GC_CHECK(plan.explanation.find(L"refs/heads/victim") != std::wstring::npos);
  GC_CHECK(plan.arguments.empty());

  // 这条分支正被另一个工作树检着用。
  g::RestorePreflightQueries held = MakeQueries(Answered(kOidC), Answered(kOidP));
  held.worktrees = WorktreesHeldBy(L"P:\\另一个工作树");
  plan = g::BuildRestorePlan(clues, held);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::refNotMovable);
  GC_CHECK(plan.explanation.find(L"另一个工作树") != std::wstring::npos);
  GC_CHECK(plan.arguments.empty());

  // 答上了但形态读不出：判「不能确定」，不按「没有别的占用」放行。
  g::RestorePreflightQueries junk = MakeQueries(Answered(kOidC), Answered(kOidP));
  junk.worktrees = Answered(L"这一行不是 worktree 记录的形态");
  plan = g::BuildRestorePlan(clues, junk);
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::refNotMovable);
  GC_CHECK(plan.explanation.find(L"读不出可靠形态") != std::wstring::npos);
}

GC_TEST(restore_command_carries_no_deref_with_expected_old_value) {
  const g::RestorePlan plan =
      g::BuildRestorePlan(MakeClues(), MakeQueries(Answered(kOidC), Answered(kOidP)));
  GC_REQUIRE(!plan.blocked, "健康现场应给出可执行方案");
  GC_REQUIRE(plan.arguments.size() >= 6, "普通恢复的参数形态");
  GC_CHECK(plan.arguments[0] == L"update-ref");
  GC_CHECK_MESSAGE(plan.arguments[1] == L"--no-deref", "不追随符号引用要写进命令形态");
  GC_CHECK(plan.arguments.back() == kOidC);  // 预期旧值
  GC_CHECK(plan.targetRef == kB);
  GC_CHECK(plan.previewText.find(L"--no-deref") != std::wstring::npos);
  GC_CHECK(plan.notice.find(L"不追随符号引用") != std::wstring::npos);
  GC_CHECK(plan.copyableCommand.find(L"--no-deref") != std::wstring::npos);

  // 删除形态（回到「该分支本不存在」）同样不解引用。
  g::RestoreClues rootClues = MakeClues();
  rootClues.isRootDeletion = true;
  rootClues.moveToOid.clear();
  const g::RestorePlan rootPlan =
      g::BuildRestorePlan(rootClues, MakeQueries(Answered(kOidC), g::GitQueryResult{}, false));
  GC_REQUIRE(!rootPlan.blocked, "删除形态应给出可执行方案");
  GC_CHECK(rootPlan.arguments[0] == L"update-ref");
  GC_CHECK(rootPlan.arguments[1] == L"--no-deref");
  GC_CHECK(std::find(rootPlan.arguments.begin(), rootPlan.arguments.end(), L"-d") !=
           rootPlan.arguments.end());
  GC_CHECK(rootPlan.arguments.back() == kOidC);
  GC_CHECK(rootPlan.newObjectId.empty());
}

// ---- R3：三种表示分开；不能安全粘贴就不提供粘贴文本，但绝不因此挡住执行链路 ----

GC_TEST(restore_copy_separates_data_preview_and_pasteable_text) {
  // Git 认为合法、对 cmd 却敏感的分支名：`&` 能被引号包住，`%` 不行。
  g::RestoreClues ampersand = MakeClues();
  ampersand.branchRef = L"refs/heads/demo&calc&rem";
  const g::RestoreCopyText safe =
      g::DescribeRestoreCopy(ampersand, g::ShellDialect::cmdInteractive);
  GC_CHECK(safe.cluesUsable);
  GC_CHECK(!safe.copyableCommand.empty());
  GC_CHECK(safe.copyableCommand.find(L"\"refs/heads/demo&calc&rem\"") != std::wstring::npos);
  GC_CHECK(safe.copyRefusal.empty());
  GC_CHECK(safe.previewCommand.find(L"[refs/heads/demo&calc&rem]") != std::wstring::npos);

  // 含 % 的名字：cmd 里没有任何不改动数据的写法能可靠表达 → 不给粘贴文本，改给字段清单。
  g::RestoreClues percent = MakeClues();
  percent.branchRef = L"refs/heads/100%DONE";
  const g::RestoreCopyText risky = g::DescribeRestoreCopy(percent, g::ShellDialect::cmdInteractive);
  GC_CHECK(risky.cluesUsable);
  GC_CHECK(risky.copyableCommand.empty());
  GC_CHECK(!risky.copyRefusal.empty());
  GC_CHECK(risky.structuredFacts.find(L"refs/heads/100%DONE") != std::wstring::npos);
  GC_CHECK(!risky.previewCommand.empty());
  // 换成 PowerShell 就表达得了（单引号按字面）：这就是「按目标 shell 判定」而不是「一律不准粘」。
  const g::RestoreCopyText inPs =
      g::DescribeRestoreCopy(percent, g::ShellDialect::powershell);
  GC_CHECK(!inPs.copyableCommand.empty());

  // 最关键的一条：粘贴受限，执行链路不受限——同一个线索做成的方案照样带着原值参数。
  g::RestorePreflightQueries queries = MakeQueries(Answered(kOidC), Answered(kOidP));
  const g::RestorePlan plan = g::BuildRestorePlan(percent, queries);
  GC_CHECK_MESSAGE(!plan.blocked, "合法分支名不该被复制层的限制挡掉：" + 
                                     std::string(plan.explanation.begin(), plan.explanation.end()));
  GC_CHECK(plan.feasibility == g::RestoreFeasibility::feasible);
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"refs/heads/100%DONE") !=
           plan.arguments.end());
  GC_CHECK(plan.copyableCommand.empty());
  GC_CHECK(!plan.copyRefusal.empty());
  GC_CHECK(!plan.structuredCopy.empty());
  GC_CHECK(plan.previewText.find(L"--no-deref") != std::wstring::npos);
  // 方案里的参数数组与复制路径复现的那一串完全同源（一处修好不会漏另一处）。
  const g::RestoreCopyText fromClues =
      g::DescribeRestoreCopy(percent, g::ShellDialect::cmdInteractive);
  GC_CHECK(plan.arguments == fromClues.arguments);
}

GC_TEST(restore_copy_refuses_unusable_clues_without_producing_text) {
  g::RestoreClues broken = MakeClues();
  broken.branchRef = L"refs/tags/v1";  // 历史文件里的非分支命名空间：按外部输入对待
  const g::RestoreCopyText copy = g::DescribeRestoreCopy(broken, g::ShellDialect::cmdInteractive);
  GC_CHECK(!copy.cluesUsable);
  GC_CHECK(copy.arguments.empty());
  GC_CHECK(copy.copyableCommand.empty());
  GC_CHECK(copy.previewCommand.empty());
  GC_CHECK(!copy.refusal.empty());

  g::RestoreClues noOid = MakeClues();
  noOid.moveToOid = L"短ID";
  const g::RestoreCopyText second = g::DescribeRestoreCopy(noOid, g::ShellDialect::cmdInteractive);
  GC_CHECK(!second.cluesUsable);
  GC_CHECK(!second.refusal.empty());

  // 损坏历史文件里的控制字符与引号：既进不了引用名校验，也进不了任何命令文本。
  g::RestoreClues dirty = MakeClues();
  dirty.branchRef = L"refs/heads/a\\" + std::wstring(1, L'\0') + L"b";
  GC_CHECK(!g::DescribeRestoreCopy(dirty, g::ShellDialect::cmdInteractive).cluesUsable);
  dirty.branchRef = L"refs/heads/a\\" + std::wstring(L"\"");
  GC_CHECK(!g::DescribeRestoreCopy(dirty, g::ShellDialect::cmdInteractive).cluesUsable);
}
