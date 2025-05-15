// 「Git 已停在冲突/暂停状态」的纯逻辑测试：全部用桩化的 ConflictMarkerFacts 与
// GitQueryResult 驱动生产逻辑，不起真实 Git、不碰文件系统、不接触网络。覆盖：
//   * 痕迹判读：没有痕迹 / MERGE_HEAD / rebase-merge\ / 只有 rebase-apply\（读得回与读不回 head-name）
//     / CHERRY_PICK_HEAD / REVERT_HEAD / BISECT_LOG / 只有 SQUASH_MSG / 多种并存的「不一致」
//     / 连存在性都问不成的「问不到」——六种拒绝各有各的原因，绝不把「看不见」当「没有」；
//   * 变基进行中的同伴痕迹（CHERRY_PICK_HEAD、MERGE_HEAD）不算「两种流程并存」，
//     否则一次正常变基会被说成状态不一致而一路拒绝到底；
//   * 「继续」的前提：索引里还有未合并条目时绝不谎称可以继续；清单没读回来时同样不作答；
//     index.lock 存在时两个入口都拒绝（本程序绝不删锁）；
//   * 「中止」在没有任何流程时不生成任何 --abort 命令；未合并清单读不全也不影响中止能否发出；
//   * 方案形态：-c submodule.recurse=false + 子命令 + --continue/--abort；刻意不带
//     --no-verify / --no-edit / -m / --ours / --theirs / --force / --quit，也不带任何路径；
//   * 确认文字的口径：继续那一步由 Git 建立提交（钩子、签名、编辑器都按 Git 自己的规则），
//     中止那一步会改动工作区并丢弃已写好的解决内容，且 --quit 不由本程序代执行；
//   * 执行前复核：痕迹消失（外部终端已走完/中止）、种类换了、清单变了、HEAD/分支变了、
//     新出现 index.lock、「这一轮没读完」——各说各话，不合并成一句「请重试」；
//   * 展示文本：流程类型、当前分支与目标、未合并文件（超过 8 个只报个数）、继续前提、
//     两个入口的可用性与原因，以及「双击那一行看 Git 原生差异」的既有路径。
// 真实 Git 的落地（真冲突、真变基目录、真 --continue/--abort 后果）在
// conflict_fixture_tests.cpp 验证；含提交的那部分由维护者运行。
#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "git/conflict_state.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ConflictFlowKind;
using gc::git::ConflictMarkerFacts;
using gc::git::ConflictOperationPlan;
using gc::git::ConflictStateFacts;
using gc::git::GitQueryResult;

constexpr std::wstring_view kOidA = L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::wstring_view kOidB = L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::wstring_view kHead = L"refs/heads/main";
constexpr std::wstring_view kHeadOid = L"cccccccccccccccccccccccccccccccccccccccc";

GitQueryResult Answer(std::wstring_view output, int exitCode = 0) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  return result;
}

// --quiet 系查询的「明确没有」：退出码 1 + 空输出。
GitQueryResult NoResult() { return Answer(std::wstring_view{}, 1); }

GitQueryResult NotAnswered() {
  GitQueryResult result;
  result.started = false;
  result.launchDetail = L"进程未能启动";
  return result;
}

GitQueryResult IncompleteOutput() {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = 0;
  result.outputComplete = false;
  result.incompleteReason = L"标准输出超过字节上限被截断";
  result.utf16Output = L"partial\0data";
  return result;
}

std::wstring NulJoined(std::initializer_list<std::wstring_view> records) {
  std::wstring joined;
  for (const std::wstring_view record : records) {
    joined += record;
    joined.push_back(L'\0');
  }
  return joined;
}

bool TextContains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool HasArgument(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

// 一份「什么都没停着」的痕迹：probed 为真、所有存在性为假。
ConflictMarkerFacts CleanMarkers() {
  ConflictMarkerFacts markers;
  markers.probed = true;
  return markers;
}

// 三条查询的「干净答案」：在 main 分支上、HEAD 可解析、没有未合并条目。
struct QuerySet {
  GitQueryResult listing = Answer(std::wstring_view{});
  GitQueryResult symbolicRef = Answer(kHead);
  GitQueryResult headObject = Answer(kHeadOid);
};

ConflictStateFacts Interpret(const ConflictMarkerFacts& markers, const QuerySet& queries) {
  return gc::git::InterpretConflictState(markers, queries.listing, queries.symbolicRef,
                                         queries.headObject);
}

ConflictMarkerFacts MergeMarkers() {
  ConflictMarkerFacts markers = CleanMarkers();
  markers.mergeHead = true;
  markers.mergeHeadOid = kOidA;
  return markers;
}

ConflictMarkerFacts RebaseMergeMarkers() {
  ConflictMarkerFacts markers = CleanMarkers();
  markers.rebaseMergeDir = true;
  markers.rebaseHeadName = kHead;
  markers.rebaseOnto = kOidB;
  markers.rebaseMsgnum = L"3";
  markers.rebaseEnd = L"7";
  return markers;
}

ConflictStateFacts MergeAtConflict() {
  QuerySet queries;
  ConflictMarkerFacts markers = MergeMarkers();
  auto state = Interpret(markers, queries);
  return state;
}

ConflictStateFacts MergeResolvedAndStaged() {
  return Interpret(MergeMarkers(), QuerySet{});
}

GC_TEST(conflict_clean_repository_offers_neither_entry_and_emits_no_abort) {
  const ConflictStateFacts state = Interpret(CleanMarkers(), QuerySet{});
  GC_CHECK(state.probed);
  GC_REQUIRE_MESSAGE(state.kind == ConflictFlowKind::none, "没有痕迹应判成 none");
  GC_CHECK(!state.continueAvailable);
  GC_CHECK(!state.abortAvailable);
  // 「没有流程时不得生成 abort 命令」：拒绝理由必须点明这一条，两份方案一条参数也不产生。
  GC_CHECK(TextContains(state.abortBlockedReason, L"不会生成任何 --abort"));
  const ConflictOperationPlan abort = gc::git::BuildConflictAbortPlan(state);
  GC_CHECK(abort.blocked);
  GC_CHECK(abort.arguments.empty());
  GC_CHECK(abort.operationId.empty());
  const ConflictOperationPlan cont = gc::git::BuildConflictContinuePlan(state);
  GC_CHECK(cont.blocked);
  GC_CHECK(cont.arguments.empty());
  // 展示文本也要说清「没有流程停着」，不能只留一句空的清单。
  GC_CHECK(TextContains(gc::git::DescribeConflictState(state), L"没有 Git 流程停在进行中"));
}

GC_TEST(conflict_unprobed_git_dir_is_unknown_not_absent) {
  ConflictMarkerFacts markers;  // probed 默认 false
  markers.probeFailure = L"Git 目录在这一轮问不出形态";
  const ConflictStateFacts state = Interpret(markers, QuerySet{});
  GC_REQUIRE(state.kind == ConflictFlowKind::unreadable, "问不到应判成 unreadable");
  GC_CHECK(!state.continueAvailable && !state.abortAvailable);
  const std::wstring view = gc::git::DescribeConflictState(state);
  GC_CHECK(TextContains(view, L"没能探到"));
  // 「问不到」绝不被写成「没有流程停着」，也不生成任何命令。
  GC_CHECK(!TextContains(view, L"没有 Git 流程停在进行中"));
  GC_CHECK(gc::git::BuildConflictAbortPlan(state).arguments.empty());
  GC_CHECK(gc::git::BuildConflictContinuePlan(state).arguments.empty());
}

GC_TEST(conflict_merge_with_unmerged_files_refuses_continue_but_offers_abort) {
  QuerySet queries;
  queries.listing = Answer(NulJoined({L"src/a.cpp", L"docs/说明 与 & 符号.md", L"src/b.cpp"}));
  const ConflictStateFacts state = Interpret(MergeMarkers(), queries);
  GC_REQUIRE(state.kind == ConflictFlowKind::merge, "MERGE_HEAD 应判成合并");
  GC_CHECK(state.unmergedReadOk);
  GC_REQUIRE(state.unmergedPaths.size() == 3, "未合并条目按 NUL 原样拆出");
  GC_CHECK(!state.continueAvailable);
  GC_CHECK(TextContains(state.continueBlockedReason, L"还有 3 个未合并的文件"));
  GC_CHECK(TextContains(state.continueBlockedReason, L"本程序不会替你选 ours/theirs"));
  GC_CHECK(state.abortAvailable);
  // 路径是数据：含空格与 & 的路径原样保留，不被拆坏。
  GC_CHECK(std::find(state.unmergedPaths.begin(), state.unmergedPaths.end(),
                     std::wstring(L"docs/说明 与 & 符号.md")) != state.unmergedPaths.end());
  const ConflictOperationPlan cont = gc::git::BuildConflictContinuePlan(state);
  GC_CHECK(cont.blocked && cont.arguments.empty());
}

GC_TEST(conflict_merge_resolved_and_staged_plans_continue_creating_a_commit) {
  const ConflictStateFacts state = MergeResolvedAndStaged();
  GC_REQUIRE(state.continueAvailable, "无未合并条目时应允许继续");
  const ConflictOperationPlan plan = gc::git::BuildConflictContinuePlan(state);
  GC_CHECK(!plan.blocked);
  const std::vector<std::wstring> expected{L"-c", L"submodule.recurse=false", L"merge", L"--continue"};
  GC_CHECK(plan.arguments == expected);
  GC_CHECK(plan.operationId == L"conflict-continue");
  GC_CHECK(plan.commandLabel == L"merge --continue");
  const std::vector<std::wstring> mergeAbort{L"-c", L"submodule.recurse=false", L"merge", L"--abort"};
  GC_CHECK(gc::git::BuildConflictAbortPlan(state).arguments == mergeAbort);
  // 确认框必须说清这一步由 Git 建立提交，且钩子/签名/编辑器都不被本程序压掉。
  GC_CHECK(TextContains(plan.confirmationText, L"由 Git 建立提交"));
  GC_CHECK(TextContains(plan.confirmationText, L"pre-commit"));
  GC_CHECK(TextContains(plan.confirmationText, L"commit-msg"));
  GC_CHECK(TextContains(plan.confirmationText, L"prepare-commit-msg"));
  GC_CHECK(TextContains(plan.confirmationText, L"commit.gpgsign"));
  GC_CHECK(TextContains(plan.confirmationText, L"编辑器"));
  GC_CHECK(TextContains(plan.confirmationText, L"暂存区里的那一份树"));
  // 命令本身绝不夹带任何「替用户决定」的选项。
  for (std::wstring_view forbidden : {L"--no-verify", L"--no-edit", L"-m", L"--ours", L"--theirs",
                                      L"--force", L"--quit", L"-X"}) {
    GC_CHECK_MESSAGE(!HasArgument(plan.arguments, forbidden), "继续那条命令里不该出现这个选项");
  }
  GC_CHECK(TextContains(plan.notice, L"不递归子模块"));
}

GC_TEST(conflict_abort_confirmation_names_the_worktree_it_discards) {
  const ConflictStateFacts state = MergeAtConflict();
  const ConflictOperationPlan plan = gc::git::BuildConflictAbortPlan(state);
  GC_CHECK(!plan.blocked);
  GC_CHECK(TextContains(plan.confirmationText, L"会改动你的工作区"));
  GC_CHECK(TextContains(plan.confirmationText, L"试图重建合并开始前的状态"));
  GC_CHECK(TextContains(plan.confirmationText, L"git reset --merge"));
  GC_CHECK(TextContains(plan.confirmationText, L"无法重建"));
  GC_CHECK(TextContains(plan.confirmationText, L"已经写好的解决内容随之丢弃"));
  GC_CHECK(TextContains(plan.confirmationText, L"--quit"));
  GC_CHECK(TextContains(plan.confirmationText, L"不会 reset --hard"));
  // 中止不依赖未合并清单：清单读不回来也照样能中止（只是说明的完整度降一档）。
  QuerySet queries;
  queries.listing = NotAnswered();
  const ConflictStateFacts unreadable = Interpret(MergeMarkers(), queries);
  GC_CHECK(!unreadable.continueAvailable);
  GC_CHECK(unreadable.abortAvailable);
  GC_CHECK(TextContains(gc::git::BuildConflictAbortPlan(unreadable).confirmationText,
                        L"没能读回来"));
}

GC_TEST(conflict_rebase_companion_markers_are_not_a_mixed_state) {
  // 真实的变基冲突现场：rebase-merge\ 与 CHERRY_PICK_HEAD（甚至 MERGE_HEAD）同时存在。
  ConflictMarkerFacts markers = RebaseMergeMarkers();
  markers.cherryPickHead = true;
  markers.pickHeadOid = kOidB;
  QuerySet queries;
  queries.listing = Answer(NulJoined({L"file.txt"}));
  const ConflictStateFacts state = Interpret(markers, queries);
  GC_REQUIRE(state.kind == ConflictFlowKind::rebaseMergeBackend, "变基目录优先，同伴痕迹不算并存");
  GC_CHECK(!state.continueAvailable);
  GC_CHECK(state.abortAvailable);
  GC_CHECK(TextContains(state.flowTarget, L"refs/heads/main"));
  GC_CHECK(TextContains(state.flowTarget, L"第 3 步 / 共 7 步"));
  const std::vector<std::wstring> rebaseContinue{
      L"-c", L"submodule.recurse=false", L"rebase", L"--continue"};
  // 命令形态只在「已经全部解决并暂存」的现场才拿得出来（还有未合并条目时方案是空的）。
  const ConflictStateFacts rebaseResolved = Interpret(markers, QuerySet{});
  GC_CHECK(rebaseResolved.continueAvailable);
  GC_CHECK(gc::git::BuildConflictContinuePlan(rebaseResolved).arguments == rebaseContinue);
  // 交互式那一条只是补充说明：确认框要交代这一步会打开编辑器（合并后端按 Git 文档如此）。
  markers.rebaseInteractiveMark = true;
  const ConflictStateFacts interactive = Interpret(markers, QuerySet{});
  GC_CHECK(TextContains(interactive.flowTarget, L"交互式"));
  GC_CHECK(TextContains(gc::git::BuildConflictContinuePlan(interactive).confirmationText, L"编辑器"));
}

GC_TEST(conflict_rebase_apply_backend_needs_head_name_and_onto) {
  ConflictMarkerFacts markers = CleanMarkers();
  markers.rebaseApplyDir = true;
  markers.rebaseHeadName = kHead;
  markers.rebaseOnto = kOidB;
  const ConflictStateFacts known = Interpret(markers, QuerySet{});
  GC_REQUIRE(known.kind == ConflictFlowKind::rebaseApplyBackend, "读得回形态才算变基(apply 后端)");
  GC_CHECK(known.abortAvailable);

  // 读不回 head-name/onto：分不清 rebase --apply 与 git am，两条命令都不给。
  ConflictMarkerFacts unknown = CleanMarkers();
  unknown.rebaseApplyDir = true;
  unknown.contentFailures.push_back(L"rebase-apply\\head-name：打不开");
  const ConflictStateFacts ambiguous = Interpret(unknown, QuerySet{});
  GC_REQUIRE(ambiguous.kind == ConflictFlowKind::ambiguousRebase, "读不回形态应承认分不清");
  GC_CHECK(!ambiguous.continueAvailable && !ambiguous.abortAvailable);
  GC_CHECK(TextContains(ambiguous.abortBlockedReason, L"git am"));
  GC_CHECK(gc::git::BuildConflictAbortPlan(ambiguous).arguments.empty());
  GC_CHECK(TextContains(gc::git::DescribeConflictState(ambiguous), L"git am"));
}

GC_TEST(conflict_sequence_flows_pick_their_own_subcommand) {
  ConflictMarkerFacts pick = CleanMarkers();
  pick.cherryPickHead = true;
  pick.pickHeadOid = kOidA;
  const ConflictStateFacts pickState = Interpret(pick, QuerySet{});
  GC_REQUIRE(pickState.kind == ConflictFlowKind::cherryPick, "CHERRY_PICK_HEAD 独立存在应判成拣选");
  const std::vector<std::wstring> pickContinue{
      L"-c", L"submodule.recurse=false", L"cherry-pick", L"--continue"};
  GC_CHECK(gc::git::BuildConflictContinuePlan(pickState).arguments == pickContinue);

  ConflictMarkerFacts revert = CleanMarkers();
  revert.revertHead = true;
  revert.pickHeadOid = kOidA;
  const ConflictStateFacts revertState = Interpret(revert, QuerySet{});
  GC_REQUIRE(revertState.kind == ConflictFlowKind::revert, "REVERT_HEAD 独立存在应判成撤销");
  const std::vector<std::wstring> revertAbort{
      L"-c", L"submodule.recurse=false", L"revert", L"--abort"};
  GC_CHECK(gc::git::BuildConflictAbortPlan(revertState).arguments == revertAbort);
  GC_CHECK(TextContains(gc::git::BuildConflictAbortPlan(revertState).confirmationText,
                        L"回到序列开始前"));
  // 序列待办在那里：说明必须点出「管的是一整条序列，不止眼前这一步」。
  ConflictMarkerFacts sequence = pick;
  sequence.sequencerDir = true;
  const ConflictStateFacts sequenceState = Interpret(sequence, QuerySet{});
  GC_CHECK(TextContains(gc::git::DescribeConflictState(sequenceState), L"整条序列"));
}

GC_TEST(conflict_bisect_and_squash_only_are_reported_not_taken_over) {
  ConflictMarkerFacts bisect = CleanMarkers();
  bisect.bisectLog = true;
  const ConflictStateFacts bisectState = Interpret(bisect, QuerySet{});
  GC_REQUIRE(bisectState.kind == ConflictFlowKind::bisect, "BISECT_LOG 应认得但不接手");
  GC_CHECK(!bisectState.continueAvailable && !bisectState.abortAvailable);
  GC_CHECK(TextContains(bisectState.abortBlockedReason, L"不接手"));
  GC_CHECK(gc::git::BuildConflictAbortPlan(bisectState).arguments.empty());

  // 只有 SQUASH_MSG（或 MERGE_MODE）而没有 MERGE_HEAD：--continue/--abort 都以 MERGE_HEAD 为前提。
  ConflictMarkerFacts squash = CleanMarkers();
  squash.squashMsg = true;
  const ConflictStateFacts squashState = Interpret(squash, QuerySet{});
  GC_REQUIRE(squashState.kind == ConflictFlowKind::squashOnly, "只有 SQUASH_MSG 应单独成态");
  GC_CHECK(TextContains(squashState.continueBlockedReason, L"MERGE_HEAD 存在为前提"));
  GC_CHECK(gc::git::BuildConflictContinuePlan(squashState).arguments.empty());
  GC_CHECK(gc::git::BuildConflictAbortPlan(squashState).arguments.empty());
}

GC_TEST(conflict_incompatible_markers_are_refused_as_inconsistent) {
  ConflictMarkerFacts markers = CleanMarkers();
  markers.mergeHead = true;
  markers.revertHead = true;
  const ConflictStateFacts state = Interpret(markers, QuerySet{});
  GC_REQUIRE(state.kind == ConflictFlowKind::mixed, "没有变基目录时两种头并存算不一致");
  GC_CHECK(!state.continueAvailable && !state.abortAvailable);
  GC_CHECK(TextContains(state.abortBlockedReason, L"互不相容"));
  // 不一致时必须把看到了什么全列出来，不能只报第一个。
  GC_CHECK(TextContains(gc::git::DescribeConflictState(state), L"MERGE_HEAD"));
  GC_CHECK(TextContains(gc::git::DescribeConflictState(state), L"REVERT_HEAD"));
  GC_CHECK(gc::git::BuildConflictAbortPlan(state).arguments.empty());

  ConflictMarkerFacts withBisect = MergeMarkers();
  withBisect.bisectLog = true;
  GC_REQUIRE_MESSAGE(Interpret(withBisect, QuerySet{}).kind == ConflictFlowKind::mixed,
             "二分定位与合并并排也是不一致");
}

GC_TEST(conflict_index_lock_blocks_both_entries_without_deleting_anything) {
  ConflictMarkerFacts markers = MergeMarkers();
  markers.indexLock = true;
  const ConflictStateFacts state = Interpret(markers, QuerySet{});
  GC_CHECK(!state.continueAvailable && !state.abortAvailable);
  GC_CHECK(TextContains(state.continueBlockedReason, L"index.lock"));
  GC_CHECK(TextContains(state.continueBlockedReason, L"不会替你删那个锁"));
  GC_CHECK(gc::git::BuildConflictContinuePlan(state).arguments.empty());
  GC_CHECK(gc::git::BuildConflictAbortPlan(state).arguments.empty());
}

GC_TEST(conflict_incomplete_unmerged_listing_is_not_answered_as_clean) {
  QuerySet queries;
  queries.listing = NotAnswered();  // 读管道失败/进程没起来：整份不是可采信的答复
  const ConflictStateFacts state = Interpret(MergeMarkers(), queries);
  GC_CHECK(!state.unmergedReadOk);
  GC_CHECK(!state.continueAvailable);
  GC_CHECK(TextContains(state.continueBlockedReason, L"清单没读回来"));
  // 输出里有半条记录（缺结尾 NUL）同样整份拒绝。
  QuerySet truncated;
  truncated.listing = Answer(L"file-a\0file-b");
  const ConflictStateFacts half = Interpret(MergeMarkers(), truncated);
  GC_CHECK(!half.unmergedReadOk);
  GC_CHECK(half.unmergedPaths.empty());
  GC_CHECK(TextContains(half.unmergedReadFailure, L"记录约定"));
}

GC_TEST(conflict_branch_and_head_unknown_are_not_answered_as_absent) {
  QuerySet queries;
  queries.symbolicRef = NotAnswered();
  queries.headObject = NotAnswered();
  const ConflictStateFacts state = Interpret(MergeMarkers(), queries);
  GC_CHECK(!state.branchQueried && !state.headQueried);
  const std::wstring view = gc::git::DescribeConflictState(state);
  GC_CHECK(TextContains(view, L"没能问出来"));
  GC_CHECK(!TextContains(view, L"HEAD 没有指向任何分支引用"));

  // Git 明确回答「没有这条符号引用」才是真的不在分支上（游离 HEAD）。
  QuerySet detached;
  detached.symbolicRef = NoResult();
  const ConflictStateFacts detachedState = Interpret(MergeMarkers(), detached);
  GC_CHECK(detachedState.branchQueried && !detachedState.onBranch);
  GC_CHECK(TextContains(gc::git::DescribeConflictState(detachedState), L"HEAD 没有指向任何分支引用"));
  // 半截对象 ID 不当成 HEAD 位置。
  QuerySet bogus;
  bogus.headObject = Answer(L"abc123");
  GC_CHECK(Interpret(MergeMarkers(), bogus).headObjectId.empty());
}

GC_TEST(conflict_unreadable_marker_content_is_reported_by_name) {
  ConflictMarkerFacts markers = CleanMarkers();
  markers.mergeHead = true;  // 存在，但内容没读回来
  markers.contentFailures.push_back(L"MERGE_HEAD：读不出内容");
  const ConflictStateFacts state = Interpret(markers, QuerySet{});
  GC_REQUIRE(state.kind == ConflictFlowKind::merge, "存在性成立就还是合并，不因内容缺读而改判");
  GC_CHECK(TextContains(state.flowTarget, L"没能原样读回"));
  GC_CHECK(TextContains(gc::git::DescribeConflictState(state), L"读不回来不等于没有"));
}

GC_TEST(conflict_view_caps_the_list_but_reports_the_true_count) {
  QuerySet queries;
  queries.listing = Answer(NulJoined({L"a1.txt", L"a2.txt", L"a3.txt", L"a4.txt", L"a5.txt",
                                      L"a6.txt", L"a7.txt", L"a8.txt", L"a9.txt", L"b10.txt",
                                      L"b11.txt"}));
  const ConflictStateFacts state = Interpret(MergeMarkers(), queries);
  GC_REQUIRE(state.unmergedPaths.size() == 11, "个数按 Git 给的全部条目计");
  const std::wstring view = gc::git::DescribeConflictState(state);
  GC_CHECK(TextContains(view, L"共 11 个"));
  GC_CHECK(TextContains(view, L"其余 3 个没在这里列出"));
}

GC_TEST(conflict_recheck_silent_when_the_scene_is_unchanged) {
  const ConflictStateFacts confirmed = MergeAtConflict();
  const ConflictStateFacts latest = MergeAtConflict();
  GC_CHECK_MESSAGE(gc::git::DescribeConflictStateChange(confirmed, latest).empty(),
                   "同一份现场不该报变化");
}

GC_TEST(conflict_recheck_names_a_flow_finished_elsewhere) {
  const ConflictStateFacts confirmed = MergeAtConflict();
  const ConflictStateFacts latest = Interpret(CleanMarkers(), QuerySet{});
  const std::wstring change = gc::git::DescribeConflictStateChange(confirmed, latest);
  GC_CHECK(TextContains(change, L"已经没有痕迹"));
  GC_CHECK(TextContains(change, L"外部终端"));
  GC_CHECK(TextContains(change, L"这条命令没有发出"));
}

GC_TEST(conflict_recheck_separates_kind_change_from_read_failure) {
  const ConflictStateFacts confirmed = MergeAtConflict();

  ConflictMarkerFacts turnedIntoRebase = RebaseMergeMarkers();
  const ConflictStateFacts kindChanged = Interpret(turnedIntoRebase, QuerySet{});
  const std::wstring kindText = gc::git::DescribeConflictStateChange(confirmed, kindChanged);
  GC_CHECK(TextContains(kindText, L"停着的流程变成了"));

  ConflictMarkerFacts unreadable;
  unreadable.probed = false;
  unreadable.probeFailure = L"Git 目录被移走了";
  const ConflictStateFacts notRead = Interpret(unreadable, QuerySet{});
  const std::wstring notReadText = gc::git::DescribeConflictStateChange(confirmed, notRead);
  // 「这一轮没读完」与「流程已经不在」必须是两句话。
  GC_CHECK(TextContains(notReadText, L"没能完成"));
  GC_CHECK(!TextContains(notReadText, L"已经没有痕迹"));
}

GC_TEST(conflict_recheck_reports_moved_head_branch_and_lock) {
  const ConflictStateFacts confirmed = MergeResolvedAndStaged();

  QuerySet moved;
  moved.headObject = Answer(kOidB);
  moved.symbolicRef = Answer(L"refs/heads/other");
  const ConflictStateFacts movedState = Interpret(MergeMarkers(), moved);
  const std::wstring movedText = gc::git::DescribeConflictStateChange(confirmed, movedState);
  GC_CHECK(TextContains(movedText, L"HEAD 从"));
  GC_CHECK(TextContains(movedText, L"当前分支从"));

  // MERGE_HEAD 换了一份提交：目标不同，确认框上写的那件事已经不成立。
  ConflictMarkerFacts otherTarget = MergeMarkers();
  otherTarget.mergeHeadOid = kOidB;
  const std::wstring targetText =
      gc::git::DescribeConflictStateChange(confirmed, Interpret(otherTarget, QuerySet{}));
  GC_CHECK(TextContains(targetText, L"流程自己的目标也换了"));

  ConflictMarkerFacts locked = MergeMarkers();
  locked.indexLock = true;
  const std::wstring lockText = gc::git::DescribeConflictStateChange(confirmed, Interpret(locked, QuerySet{}));
  GC_CHECK(TextContains(lockText, L"index.lock"));
}

GC_TEST(conflict_autostash_and_merge_head_are_disclosed_not_acted_on) {
  ConflictMarkerFacts markers = MergeMarkers();
  markers.mergeAutostash = true;
  const ConflictStateFacts state = Interpret(markers, QuerySet{});
  const std::wstring view = gc::git::DescribeConflictState(state);
  GC_CHECK(TextContains(view, L"自动 stash"));
  // 说明里只交代「Git 按它自己的规则处理那一份」，本程序既不应用也不丢弃它。
  GC_CHECK(TextContains(view, L"本程序不碰它"));
  GC_CHECK(!HasArgument(gc::git::BuildConflictAbortPlan(state).arguments, L"--quit"));
}

}  // namespace
