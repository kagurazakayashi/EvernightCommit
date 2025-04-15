// 「撤回最近提交」判读与方案的纯逻辑测试：全部用桩化的 GitQueryResult 驱动生产逻辑，
// 不起真实 Git、不碰文件系统。覆盖：预检查询参数形态、HEAD/父提交/远端包含/工作区四组
// 输出的判读三态与坏输出拒绝、方案层的明确拒绝条件（游离、特殊流程、冲突、读不回现状）、
// 普通撤回与根提交（update-ref 受控路径）的命令构造、合并提交与发布状态的风险分级、
// 确认文字里「改动一起保留」「不保证从未 push」「强制只越风险提示不换命令」这些必须说清的点。
// 真实 Git 的形态与命令效果在 undo_probe_fixture_tests.cpp 用临时仓库验证。
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_history.h"
#include "git/repository.h"
#include "git/undo_commit_plan.h"
#include "git/workspace_status.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeKind;
using gc::git::GitQueryResult;
using gc::git::RepoError;
using gc::git::RepositoryWorkflowState;
using gc::git::UndoCommitPlan;
using gc::git::UndoCommitPlanInput;
using gc::git::UndoHeadFacts;
using gc::git::UndoPreflightFacts;
using gc::git::UndoPreflightQueries;
using gc::git::UndoPublishEvidence;
using gc::git::UndoQueryOutcome;
using gc::git::UndoQueryRead;

// 完整对象 ID 形状的桩值（40 个十六进制字符）。
constexpr std::wstring_view kShaA = L"1111111111111111111111111111111111111111";  // HEAD
constexpr std::wstring_view kShaB = L"2222222222222222222222222222222222222222";  // 第一父
constexpr std::wstring_view kShaC = L"3333333333333333333333333333333333333333";  // 第二父（合并）

GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
  return result;
}

GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
  return result;
}

std::wstring JoinNul(std::initializer_list<std::wstring_view> records) {
  std::wstring text;
  for (std::wstring_view record : records) {
    text += record;
    text.push_back(L'\0');
  }
  return text;
}

std::wstring Ordinary(std::wstring_view xy, std::wstring_view path) {
  return std::wstring(L"1 ") + std::wstring(xy) + L" N... 100644 100644 100644 " +
         std::wstring(kShaB) + L" " + std::wstring(kShaA) + L" " + std::wstring(path);
}

bool Contains(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

bool TextContains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

std::string Narrow(std::wstring_view text) {
  std::string out;
  for (const wchar_t value : text) {
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
    } else {
      char buffer[16]{};
      std::snprintf(buffer, sizeof(buffer), "<U+%04X>", static_cast<unsigned>(value));
      out += buffer;
    }
  }
  return out;
}

// 一份「可以正常撤回」的桩预检：HEAD 可解析、有一个父、引用本地未发现已发布、工作区干净。
UndoPreflightQueries HealthyQueries() {
  UndoPreflightQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.headCommit = Answer(0, std::wstring(kShaA) + L"\n");
  queries.commitDependentRan = true;
  queries.parents = Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L"\n");
  queries.headSummary = Answer(0, L"fix: 修一个很要紧的 bug\n");
  queries.remoteRefs = Answer(0, L"refs/remotes/origin/main\n");
  queries.remoteContains = Answer(0, L"");
  queries.status = Answer(0, L"");
  return queries;
}

UndoCommitPlanInput InputFromQueries(const UndoPreflightQueries& queries) {
  UndoCommitPlanInput input;
  input.facts = gc::git::InterpretUndoPreflight(queries);
  input.repositoryRoot = L"D:\\repo";
  return input;
}

// 直接按字段造事实（跳过判读）用：方案层测试只需要事实本身。
UndoCommitPlanInput MakeInput(UndoPreflightFacts facts) {
  UndoCommitPlanInput input;
  input.facts = std::move(facts);
  input.repositoryRoot = L"D:\\repo";
  return input;
}

}  // namespace

// ---- 查询参数形态 ----

GC_TEST(undo_probe_arguments_are_read_only_and_bound_to_repository) {
  const std::wstring dir = L"D:\\工作区 & 目录";
  const std::vector<std::vector<std::wstring>> probeGroups{
      gc::git::BuildUndoSymbolicRefArguments(dir),
      gc::git::BuildUndoHeadCommitArguments(dir),
      gc::git::BuildUndoParentsArguments(dir),
      gc::git::BuildUndoHeadSummaryArguments(dir),
      gc::git::BuildUndoRemoteRefsArguments(dir),
      gc::git::BuildUndoRemoteContainsArguments(dir, kShaA),
  };
  for (const std::vector<std::wstring>& arguments : probeGroups) {
    GC_CHECK_MESSAGE(!arguments.empty() && arguments[0] == L"-C", "每条查询必须显式 -C 绑定仓库根");
    GC_CHECK(Contains(arguments, dir));
    GC_CHECK(Contains(arguments, L"--no-optional-locks"));
  }
  // 远端包含必须带合格完整 ID；形态不合格的 ID 根本不配送进 Git（返回空数组=跳过）。
  GC_CHECK(gc::git::BuildUndoRemoteContainsArguments(dir, L"deadbeef").empty());
  GC_CHECK(gc::git::BuildUndoRemoteContainsArguments(dir, L"").empty());
  GC_CHECK(Contains(gc::git::BuildUndoRemoteContainsArguments(dir, kShaA), kShaA));
  // 撤回绝不构造带 --hard / revert / push 的任何东西。
  const std::vector<std::wstring> parents = gc::git::BuildUndoParentsArguments(dir);
  GC_CHECK(!Contains(parents, L"--hard") && !Contains(parents, L"revert"));
}

// ---- 判读 ----

GC_TEST(undo_query_read_separates_absent_from_failed) {
  GC_CHECK(gc::git::ReadUndoQuery(Answer(0, L"refs/heads/main\n")).outcome ==
           UndoQueryOutcome::answered);
  // --quiet 系查询的「退出码 1 + 无输出」是明确答案，不是错误。
  GC_CHECK(gc::git::ReadUndoQuery(Answer(1)).outcome == UndoQueryOutcome::noResult);
  // 退出码 1 却带着标准输出（不该出现在 --quiet 系查询）→ 按失败归类，不能吞掉。
  const UndoQueryRead noisy = gc::git::ReadUndoQuery(Answer(1, L"trailing junk"));
  GC_CHECK(noisy.outcome == UndoQueryOutcome::failed);
  const UndoQueryRead dead = gc::git::ReadUndoQuery(LaunchFailed());
  GC_CHECK(dead.outcome == UndoQueryOutcome::failed);
  GC_CHECK(dead.error == RepoError::gitLaunchFailed);
}

GC_TEST(interpret_head_on_branch_with_one_parent) {
  const UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(HealthyQueries());
  GC_CHECK_MESSAGE(facts.head.queryOk, "桩回答完备时判读应成功：" + Narrow(facts.head.queryFailure));
  GC_CHECK(facts.head.onBranch);
  GC_CHECK(facts.head.branchRef == L"refs/heads/main");
  GC_CHECK(facts.head.branchName == L"main");
  GC_CHECK(facts.head.headResolved);
  GC_CHECK(facts.head.headObjectId == kShaA);
  GC_CHECK(facts.head.parentsResolved);
  GC_CHECK(facts.head.selfMatchesHead);
  GC_CHECK(facts.head.parentObjectIds.size() == 1 && facts.head.parentObjectIds[0] == kShaB);
  GC_CHECK(facts.head.headSummary == L"fix: 修一个很要紧的 bug");
  GC_CHECK(facts.publish == UndoPublishEvidence::notFound);
  GC_CHECK(facts.statusOk);
}

GC_TEST(interpret_detached_and_unborn_head) {
  UndoPreflightQueries detached = HealthyQueries();
  detached.symbolicRef = Answer(1);  // 不在分支上
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(detached);
  GC_CHECK(facts.head.queryOk);
  GC_CHECK(!facts.head.onBranch);
  GC_CHECK(facts.head.headResolved);

  UndoPreflightQueries unborn = HealthyQueries();
  unborn.symbolicRef = Answer(0, L"refs/heads/main\n");
  unborn.headCommit = Answer(1);  // HEAD 不可解析
  unborn.commitDependentRan = false;
  unborn.parents = {};
  unborn.headSummary = {};
  unborn.remoteContains = {};
  facts = gc::git::InterpretUndoPreflight(unborn);
  GC_CHECK(facts.head.queryOk);
  GC_CHECK(facts.head.onBranch);
  GC_CHECK(!facts.head.headResolved);
  // HEAD 不可解析时依赖它的查询根本没发：远端结论必须是 notRun，不能谎称「查过没有」。
  GC_CHECK(!facts.publishQueried);
  GC_CHECK(facts.publish == UndoPublishEvidence::notRun);
}

GC_TEST(interpret_rejects_malformed_object_ids_and_self_mismatch) {
  UndoPreflightQueries badId = HealthyQueries();
  badId.headCommit = Answer(0, L"not-a-sha\n");
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(badId);
  GC_CHECK(!facts.head.queryOk);
  GC_CHECK_MESSAGE(!facts.head.headResolved, "半截 ID 绝不能被当成可用的 HEAD");

  // rev-list 报的自身 ID 与 rev-parse 的 HEAD 不一致：说明查询之间 HEAD 动了，不能用。
  UndoPreflightQueries mismatch = HealthyQueries();
  mismatch.parents = Answer(0, std::wstring(kShaB) + L" " + std::wstring(kShaC) + L"\n");
  facts = gc::git::InterpretUndoPreflight(mismatch);
  GC_CHECK(facts.head.parentsQueried);
  GC_CHECK(!facts.head.parentsResolved);
  GC_CHECK(!facts.head.selfMatchesHead);

  UndoPreflightQueries badParent = HealthyQueries();
  badParent.parents = Answer(0, std::wstring(kShaA) + L" 0m0r3-b4d\n");
  facts = gc::git::InterpretUndoPreflight(badParent);
  GC_CHECK(!facts.head.parentsResolved);
}

GC_TEST(interpret_merge_commit_keeps_all_parents) {
  UndoPreflightQueries merge = HealthyQueries();
  merge.parents =
      Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L" " + std::wstring(kShaC) + L"\n");
  const UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(merge);
  GC_CHECK(facts.head.parentsResolved);
  GC_CHECK(facts.head.parentObjectIds.size() == 2);
  GC_CHECK(facts.head.parentObjectIds[0] == kShaB);
}

GC_TEST(interpret_publish_evidence_three_states) {
  // 命中：已知已发布，并保留命中的引用列表。
  UndoPreflightQueries contained = HealthyQueries();
  contained.remoteContains = Answer(0, L"refs/remotes/origin/main\nrefs/remotes/origin/dev\n");
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(contained);
  GC_CHECK(facts.publish == UndoPublishEvidence::contained);
  GC_CHECK(facts.containingRemoteRefs.size() == 2);

  // 本地没有任何远端跟踪引用：无从判断。
  UndoPreflightQueries noRefs = HealthyQueries();
  noRefs.remoteRefs = Answer(0, L"");
  facts = gc::git::InterpretUndoPreflight(noRefs);
  GC_CHECK(facts.publish == UndoPublishEvidence::noRemoteRefs);

  // 查询本身失败：连「本地没有」都说不了。
  UndoPreflightQueries failed = HealthyQueries();
  failed.remoteContains = Answer(128, L"", L"fatal: bad object\n");
  facts = gc::git::InterpretUndoPreflight(failed);
  GC_CHECK(facts.publish == UndoPublishEvidence::queryFailed);
  GC_CHECK(!facts.publishFailureDetail.empty());
}

GC_TEST(interpret_status_failure_marks_facts_unusable) {
  UndoPreflightQueries broken = HealthyQueries();
  broken.status = Answer(128, L"", L"fatal: bad config line 3\n");
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(broken);
  GC_CHECK(facts.statusQueried);
  GC_CHECK(!facts.statusOk);
  GC_CHECK(facts.statusError != RepoError::none);

  // 输出不合约定同样整体作废，绝不拿半套列表去谈撤回。
  UndoPreflightQueries junk = HealthyQueries();
  junk.status = Answer(0, JoinNul({L"1 .M N... 100644"}));
  facts = gc::git::InterpretUndoPreflight(junk);
  GC_CHECK(!facts.statusOk);
}

// ---- 方案：明确拒绝的条件 ----

GC_TEST(plan_blocks_when_head_unusable_or_absent) {
  UndoPreflightQueries noCommits = HealthyQueries();
  noCommits.headCommit = Answer(1);
  noCommits.commitDependentRan = false;
  UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(noCommits));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"还没有任何提交"), Narrow(plan.blockedReason));

  UndoPreflightQueries detached = HealthyQueries();
  detached.symbolicRef = Answer(1);
  plan = gc::git::BuildUndoCommitPlan(InputFromQueries(detached));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"游离 HEAD"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_special_flows_and_conflicts) {
  UndoCommitPlanInput input = InputFromQueries(HealthyQueries());
  input.workflow.rebaseInProgress = true;
  UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"变基"), Narrow(plan.blockedReason));
  // 特殊流程是明确拒绝：blocked 的方案不携带任何命令，也不给「强制」留通道。
  GC_CHECK(!plan.requiresForce);
  GC_CHECK(plan.arguments.empty());

  UndoPreflightQueries conflicted = HealthyQueries();
  conflicted.status = Answer(0, JoinNul({L"u UU N... 100644 100644 100644 100644 "
                                        L"1111111111111111111111111111111111111111 "
                                        L"2222222222222222222222222222222222222222 "
                                        L"3333333333333333333333333333333333333333 both-sides.txt"}));
  plan = gc::git::BuildUndoCommitPlan(InputFromQueries(conflicted));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"冲突"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_when_workspace_state_unread) {
  // 读不回工作区/索引现状就不撤回：看不到要「一起保留」的东西，确认框就是空的承诺。
  UndoPreflightQueries noStatus = HealthyQueries();
  noStatus.status = LaunchFailed();
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(noStatus));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"工作区/索引现状"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_when_publish_query_missing) {
  // 判读层正常、但预检根本没问过远端：拒绝打开确认框（不能拿「没查」冒充「没查到」）。
  UndoCommitPlanInput input = InputFromQueries(HealthyQueries());
  input.facts.publishQueried = false;
  input.facts.publish = UndoPublishEvidence::notRun;
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK(plan.blocked);
}

// ---- 方案：普通撤回 ----

GC_TEST(plan_normal_commit_resets_soft_to_first_parent) {
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(HealthyQueries()));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);
  GC_CHECK_MESSAGE(plan.arguments.size() == 3 && plan.arguments[0] == L"reset" &&
                       plan.arguments[1] == L"--soft" && plan.arguments[2] == kShaB,
                   "命令必须是 git reset --soft <第一父完整ID>");
  GC_CHECK(plan.operationId == L"undo-commit");
  GC_CHECK(plan.commandLabel == L"git reset --soft");
  GC_CHECK(TextContains(plan.previewText, kShaA));       // 原提交完整 ID 必须摆出来
  GC_CHECK(TextContains(plan.previewText, L"git reset --soft"));
  GC_CHECK(TextContains(plan.restoreHint, kShaA));       // 恢复线索
  GC_CHECK(!TextContains(plan.previewText, L"--hard"));  // 确认文字不提更激烈的命令
  GC_CHECK(!TextContains(plan.arguments[1], L"^"));      // 不用会歧义的 HEAD^，用完整 ID
  GC_CHECK(TextContains(plan.notice, L"仅本地"));
}

GC_TEST(plan_lists_existing_changes_as_retained_together) {
  // 已有暂存与未暂存改动：确认文字必须明说「一起保留、无法分成两堆」，
  // 但不得把软撤回描述成会发生文本合并冲突。
  UndoPreflightQueries dirty = HealthyQueries();
  dirty.status = Answer(0, JoinNul({Ordinary(L"A.", L"staged-earlier.txt"), Ordinary(L".M", L"edited.txt")}));
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(dirty));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);  // 「改动一起保留」是说明，不是要强制确认的风险
  GC_CHECK(TextContains(plan.previewText, L"一起保留"));
  GC_CHECK(TextContains(plan.previewText, L"无法把两堆分开"));
  // 软撤回不动索引：可以明说「不会产生文本合并冲突」，但绝不能反过来吓唬人。
  GC_CHECK(TextContains(plan.previewText, L"不会产生文本合并冲突"));
}

GC_TEST(plan_not_found_publish_notes_staleness_honestly) {
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(HealthyQueries()));
  GC_CHECK(!plan.requiresForce);
  GC_CHECK(TextContains(plan.previewText, L"本地信息未发现已发布"));
  GC_CHECK_MESSAGE(TextContains(plan.previewText, L"不能证明它从未被"),
                   "没有 fetch 过就要承认不能保证从未 push：" + Narrow(plan.previewText));
}

// ---- 方案：根提交与合并提交 ----

GC_TEST(plan_root_commit_undo_uses_checked_update_ref) {
  UndoPreflightQueries root = HealthyQueries();
  root.parents = Answer(0, std::wstring(kShaA) + L"\n");  // 没有任何父
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(root));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);
  GC_CHECK_MESSAGE(
      plan.arguments.size() == 4 && plan.arguments[0] == L"update-ref" && plan.arguments[1] == L"-d" &&
          plan.arguments[2] == L"HEAD" && plan.arguments[3] == kShaA,
      "根提交必须走带预期旧值核对的 git update-ref -d HEAD <完整ID>");
  GC_CHECK(TextContains(plan.previewText, L"尚无提交"));
  GC_CHECK(TextContains(plan.previewText, L"只在分支引用确实还指向"));
  GC_CHECK(!TextContains(plan.previewText, L"空提交"));  // 不创建空提交冒充撤回
  GC_CHECK(TextContains(plan.restoreHint, kShaA));
}

GC_TEST(plan_merge_commit_undo_forces_confirmation_on_first_parent) {
  UndoPreflightQueries merge = HealthyQueries();
  merge.parents =
      Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L" " + std::wstring(kShaC) + L"\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(merge));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.requiresForce, "合并提交撤回属于要「强制撤回（仅本地）」确认的风险");
  GC_CHECK_MESSAGE(plan.arguments[2] == kShaB, "目标必须是第一父提交");
  GC_CHECK(TextContains(plan.previewText, L"合并提交"));
  GC_CHECK(TextContains(plan.previewText, L"第一父提交"));
}

GC_TEST(plan_published_commit_forces_confirmation_and_lists_refs) {
  UndoPreflightQueries published = HealthyQueries();
  published.remoteContains = Answer(0, L"refs/remotes/origin/main\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(published));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(plan.requiresForce);
  GC_CHECK(TextContains(plan.previewText, L"已知已发布"));
  GC_CHECK(TextContains(plan.previewText, L"refs/remotes/origin/main"));
  GC_CHECK(TextContains(plan.previewText, L"强制撤回（仅本地）"));
  // 「强制」只是确认风险：命令仍是普通软撤回，绝不带 push 相关任何东西。
  GC_CHECK(plan.arguments[0] == L"reset" && plan.arguments[1] == L"--soft");
  GC_CHECK(!TextContains(plan.previewText, L"force push") ||
           TextContains(plan.previewText, L"更不会自动 force push"));
}

GC_TEST(plan_undeterminable_publish_forces_confirmation) {
  // 信息不足（查询失败 / 没有任何远端跟踪引用）都归「无法判断」→ 要强制确认。
  UndoPreflightQueries failed = HealthyQueries();
  failed.remoteContains = Answer(128, L"", L"fatal: Oops\n");
  UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(failed));
  GC_CHECK(!plan.blocked);
  GC_CHECK(plan.requiresForce);
  GC_CHECK(TextContains(plan.previewText, L"无法判断"));

  UndoPreflightQueries noRefs = HealthyQueries();
  noRefs.remoteRefs = Answer(0, L"");
  plan = gc::git::BuildUndoCommitPlan(InputFromQueries(noRefs));
  GC_CHECK(!plan.blocked);
  GC_CHECK(plan.requiresForce);
  GC_CHECK(TextContains(plan.previewText, L"不能保证它从未 push"));
}

GC_TEST(plan_state_change_note_compares_captured_snapshot) {
  UndoCommitPlanInput input = InputFromQueries(HealthyQueries());
  input.captured.valid = true;
  input.captured.hasHead = true;
  input.captured.shortSha = gc::git::ShortObjectId(kShaB);  // 界面当时显示的是另一条提交
  input.captured.stagedItems = 7;                           // 与刚读回的 0 项不一致
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(!plan.stateChangeNote.empty(), "点击瞬间的界面摘要与刚读回的现状不同时必须说明");
  GC_CHECK(TextContains(plan.stateChangeNote, L"以刚刚读回的为准"));
  GC_CHECK(TextContains(plan.previewText, L"以刚刚读回的为准"));

  // 短 ID 长度可能与 --short 不同：前缀一致就不该虚报「HEAD 变了」。
  UndoCommitPlanInput same = InputFromQueries(HealthyQueries());
  same.captured.valid = true;
  same.captured.hasHead = true;
  same.captured.shortSha = std::wstring(kShaA).substr(0, 7);
  const UndoCommitPlan calm = gc::git::BuildUndoCommitPlan(same);
  GC_CHECK_MESSAGE(calm.stateChangeNote.empty(), "同一个提交的不同长度短 ID 不该被判成变化");
}

GC_TEST(plan_index_lock_warns_without_blocking) {
  // 锁归 Git 自己管：这里只提醒，不删锁、不终止别的进程，也不因此拒绝撤回。
  UndoCommitPlanInput input = InputFromQueries(HealthyQueries());
  input.workflow.indexLocked = true;
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(TextContains(plan.notice, L"index.lock"));
}

// ---- 复核快照的判读 ----

GC_TEST(interpret_head_snapshot_only_reads_branch_and_head) {
  const UndoHeadFacts ok = gc::git::InterpretUndoHeadSnapshot(Answer(0, L"refs/heads/main\n"),
                                                              Answer(0, std::wstring(kShaA) + L"\n"));
  GC_CHECK(ok.queryOk);
  GC_CHECK(ok.branchRef == L"refs/heads/main");
  GC_CHECK(ok.headObjectId == kShaA);
  GC_CHECK(!ok.parentsQueried);  // 复核根本不问父提交

  const UndoHeadFacts dead = gc::git::InterpretUndoHeadSnapshot(LaunchFailed(), Answer(0, L"x"));
  GC_CHECK(!dead.queryOk);
  GC_CHECK(!dead.queryFailure.empty());
}
