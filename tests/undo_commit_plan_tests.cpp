// 「撤回最近提交」判读与方案的纯逻辑测试：全部用桩化的 GitQueryResult 驱动生产逻辑，
// 不起真实 Git、不碰文件系统。覆盖：预检查询参数形态（父关系查询绑定完整 ID 而非 HEAD）、
// HEAD/父提交/父对象/浅仓库/远端包含/工作区六组输出的判读三态与坏输出拒绝、
// 「真正根提交 / 浅边界 / 缺失父对象 / 非提交对象 / 两份证据不一致」的分别、
// 方案层的明确拒绝条件（游离、特殊流程、冲突、读不回现状、引用名不合格、父关系不可信）、
// 普通撤回与根提交的带预期旧值 update-ref 命令构造（绝不用可变的 HEAD、绝不 reset --soft）、
// 合并提交与发布状态与浅仓库的风险分级、确认文字里「改动一起保留」「不保证从未 push」
// 「强制只越风险提示不换命令」「不承诺已被删除的 reflog」这些必须说清的点。
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
using gc::git::UndoTargetKind;

// 完整对象 ID 形状的桩值（40 个十六进制字符）。
constexpr std::wstring_view kShaA = L"1111111111111111111111111111111111111111";  // HEAD
constexpr std::wstring_view kShaB = L"2222222222222222222222222222222222222222";  // 第一父
constexpr std::wstring_view kShaC = L"3333333333333333333333333333333333333333";  // 第二父（合并）
constexpr std::wstring_view kTree = L"4444444444444444444444444444444444444444";  // 对象里的 tree

// 进 reflog 的固定说明：命令里就是一个 token，测试按它核对参数位次。
constexpr std::wstring_view kReason = L"EvernightCommit:undo-last-commit";

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

// git cat-file commit 的原始对象文本桩：头部按 parent 行给定，空行之后是正文。
std::wstring CommitObjectText(std::initializer_list<std::wstring_view> parents) {
  std::wstring text = std::wstring(L"tree ") + std::wstring(kTree) + L"\n";
  for (const std::wstring_view parent : parents) {
    text += L"parent " + std::wstring(parent) + L"\n";
  }
  text += L"author 测试者 <tester@example.invalid> 1700000000 +0800\n";
  text += L"committer 测试者 <tester@example.invalid> 1700000000 +0800\n";
  text += L"\n标题正文里出现 parent 1111111111111111111111111111111111111111 也不算父提交\n";
  return text;
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

// 一份「可以正常撤回」的桩预检：HEAD 可解析、两份证据都说是单父、父对象在本地、
// 仓库明确不是浅仓库、远端引用本地未发现已发布、工作区干净。
UndoPreflightQueries HealthyQueries() {
  UndoPreflightQueries queries;
  queries.symbolicRef = Answer(0, L"refs/heads/main\n");
  queries.headCommit = Answer(0, std::wstring(kShaA) + L"\n");
  queries.commitDependentRan = true;
  queries.parents = Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L"\n");
  queries.commitObject = Answer(0, CommitObjectText({kShaB}));
  queries.shallowState = Answer(0, L"false\n");
  queries.parentObjectRan = true;
  queries.parentObjectQueryOid = std::wstring(kShaB);
  queries.parentObject = Answer(0, L"commit\n");
  queries.headSummary = Answer(0, L"fix: 修一个很要紧的 bug\n");
  queries.remoteRefs = Answer(0, L"refs/remotes/origin/main\n");
  queries.remoteContains = Answer(0, L"");
  queries.status = Answer(0, L"");
  return queries;
}

// 改成「两处都记录没有父」：真正根提交的形态（浅仓库状态由调用方决定要不要再改）。
void MakeRootShaped(UndoPreflightQueries& queries) {
  queries.parents = Answer(0, std::wstring(kShaA) + L"\n");
  queries.commitObject = Answer(0, CommitObjectText({}));
  queries.parentObjectRan = false;
  queries.parentObjectQueryOid.clear();
  queries.parentObject = {};
}

// 改成「合并提交」：历史视图与对象头都记录两个父。
void MakeMergeShaped(UndoPreflightQueries& queries) {
  queries.parents =
      Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L" " + std::wstring(kShaC) + L"\n");
  queries.commitObject = Answer(0, CommitObjectText({kShaB, kShaC}));
  queries.parentObjectQueryOid = std::wstring(kShaB);
  queries.parentObject = Answer(0, L"commit\n");
}

UndoCommitPlanInput InputFromQueries(const UndoPreflightQueries& queries) {
  UndoCommitPlanInput input;
  input.facts = gc::git::InterpretUndoPreflight(queries);
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
      gc::git::BuildUndoParentsArguments(dir, kShaA),
      gc::git::BuildUndoHeadSummaryArguments(dir, kShaA),
      gc::git::BuildUndoCommitObjectArguments(dir, kShaA),
      gc::git::BuildUndoShallowStateArguments(dir),
      gc::git::BuildUndoParentObjectArguments(dir, kShaB),
      gc::git::BuildUndoRemoteRefsArguments(dir),
      gc::git::BuildUndoRemoteContainsArguments(dir, kShaA),
  };
  for (const std::vector<std::wstring>& arguments : probeGroups) {
    GC_CHECK_MESSAGE(!arguments.empty() && arguments[0] == L"-C", "每条查询必须显式 -C 绑定仓库根");
    GC_CHECK(Contains(arguments, dir));
    GC_CHECK(Contains(arguments, L"--no-optional-locks"));
    // 预检一律只读：不出现任何会改动仓库、联网、或更激烈的命令词。
    const std::wstring_view forbiddenWords[] = {L"--hard", L"revert", L"push", L"fetch", L"unshallow",
                                               L"reset", L"checkout", L"clean", L"stash"};
    for (const std::wstring_view forbidden : forbiddenWords) {
      GC_CHECK(!Contains(arguments, forbidden));
    }
  }
  // 对象相关的查询必须带 --no-replace-objects：被判读采信的始终是仓库里那个真实对象。
  GC_CHECK(Contains(gc::git::BuildUndoHeadCommitArguments(dir), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoParentsArguments(dir, kShaA), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoCommitObjectArguments(dir, kShaA), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoParentObjectArguments(dir, kShaB), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoRemoteContainsArguments(dir, kShaA), L"--no-replace-objects"));
  // 绑对象 ID 的查询绝不用可变的 HEAD：问的必须是刚核实过的那个完整 ID。
  const std::vector<std::wstring> parents = gc::git::BuildUndoParentsArguments(dir, kShaA);
  GC_CHECK(Contains(parents, kShaA) && !Contains(parents, L"HEAD"));
  GC_CHECK(Contains(gc::git::BuildUndoCommitObjectArguments(dir, kShaA), kShaA));
  GC_CHECK(Contains(gc::git::BuildUndoParentObjectArguments(dir, kShaB), kShaB));
  // 形态不合格的 ID 根本不配送进 Git（返回空数组＝跳过这条查询）。
  GC_CHECK(gc::git::BuildUndoParentsArguments(dir, L"deadbeef").empty());
  GC_CHECK(gc::git::BuildUndoHeadSummaryArguments(dir, L"").empty());
  GC_CHECK(gc::git::BuildUndoCommitObjectArguments(dir, L"0m0r3-b4d").empty());
  GC_CHECK(gc::git::BuildUndoParentObjectArguments(dir, L"HEAD").empty());
  GC_CHECK(gc::git::BuildUndoRemoteContainsArguments(dir, L"deadbeef").empty());
  GC_CHECK(gc::git::BuildUndoRemoteContainsArguments(dir, L"").empty());
}

GC_TEST(undo_first_reported_parent_reads_the_history_view_only) {
  GC_CHECK(gc::git::UndoFirstReportedParent(
               Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L" " +
                        std::wstring(kShaC) + L"\n")) == kShaB);
  // 没有父、查询没答上来、输出不合约定：都返回空串，调用方据此跳过那条追问。
  GC_CHECK(gc::git::UndoFirstReportedParent(Answer(0, std::wstring(kShaA) + L"\n")).empty());
  GC_CHECK(gc::git::UndoFirstReportedParent(Answer(128, L"", L"fatal: bad object\n")).empty());
  GC_CHECK(gc::git::UndoFirstReportedParent(LaunchFailed()).empty());
  GC_CHECK(gc::git::UndoFirstReportedParent(Answer(0, std::wstring(kShaA) + L" 0m0r3-b4d\n")).empty());
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
  // 父关系分类：两份证据一致、目标父对象本地可读、仓库明确不是浅仓库。
  GC_CHECK(facts.head.target.queried);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::singleParent);
  GC_CHECK_MESSAGE(facts.head.target.failure.empty(), Narrow(facts.head.target.failure));
  GC_CHECK(facts.head.target.recordedParentIds.size() == 1 &&
           facts.head.target.recordedParentIds[0] == kShaB);
  GC_CHECK(facts.head.target.shallowQueried);
  GC_CHECK(!facts.head.target.repositoryIsShallow);
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
  unborn.commitObject = {};
  unborn.shallowState = {};
  unborn.parentObjectRan = false;
  unborn.parentObjectQueryOid.clear();
  unborn.parentObject = {};
  unborn.headSummary = {};
  unborn.remoteContains = {};
  facts = gc::git::InterpretUndoPreflight(unborn);
  GC_CHECK(facts.head.queryOk);
  GC_CHECK(facts.head.onBranch);
  GC_CHECK(!facts.head.headResolved);
  // HEAD 不可解析时依赖它的查询根本没发：远端结论必须是 notRun，不能谎称「查过没有」。
  GC_CHECK(!facts.publishQueried);
  GC_CHECK(facts.publish == UndoPublishEvidence::notRun);
  GC_CHECK(!facts.head.target.queried);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
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
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  UndoPreflightQueries badParent = HealthyQueries();
  badParent.parents = Answer(0, std::wstring(kShaA) + L" 0m0r3-b4d\n");
  facts = gc::git::InterpretUndoPreflight(badParent);
  GC_CHECK(!facts.head.parentsResolved);
}

// ---- 判读：真正根提交 / 浅边界 / 缺失父对象 / 非提交对象 ----

GC_TEST(interpret_verified_root_needs_both_evidences_and_not_shallow) {
  UndoPreflightQueries root = HealthyQueries();
  MakeRootShaped(root);
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(root);
  GC_CHECK(facts.head.parentsResolved);
  GC_CHECK(facts.head.parentObjectIds.empty());
  GC_CHECK(facts.head.target.kind == UndoTargetKind::verifiedRoot);

  // 浅仓库里「两处都没有父」只是历史被截断的形态，绝不能认作真正的第一个提交。
  UndoPreflightQueries shallowRoot = HealthyQueries();
  MakeRootShaped(shallowRoot);
  shallowRoot.shallowState = Answer(0, L"true\n");
  facts = gc::git::InterpretUndoPreflight(shallowRoot);
  GC_CHECK(facts.head.target.shallowQueried);
  GC_CHECK(facts.head.target.repositoryIsShallow);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::hiddenByShallow);

  // 问不出浅不浅（老版本 Git 不认这个选项）：连「是不是根提交」都判断不了。
  UndoPreflightQueries unknownShallow = HealthyQueries();
  MakeRootShaped(unknownShallow);
  unknownShallow.shallowState = Answer(129, L"", L"error: unknown option `is-shallow-repository'\n");
  facts = gc::git::InterpretUndoPreflight(unknownShallow);
  GC_CHECK(!facts.head.target.shallowQueried);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
  GC_CHECK_MESSAGE(TextContains(facts.head.target.failure, L"浅"), Narrow(facts.head.target.failure));

  // 答案不是 true/false：按问不出来处理，不猜。
  UndoPreflightQueries junkShallow = HealthyQueries();
  MakeRootShaped(junkShallow);
  junkShallow.shallowState = Answer(0, L"maybe\n");
  facts = gc::git::InterpretUndoPreflight(junkShallow);
  GC_CHECK(!facts.head.target.shallowQueried);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
}

GC_TEST(interpret_shallow_boundary_is_hidden_parent_relation) {
  // 浅边界的典型形态：提交对象自己记录了父提交，历史视图却不给（父对象没抓下来）。
  UndoPreflightQueries hidden = HealthyQueries();
  hidden.parents = Answer(0, std::wstring(kShaA) + L"\n");
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(hidden);
  GC_CHECK(facts.head.parentsResolved);  // 「没有父」是历史视图的明确答案
  GC_CHECK(facts.head.target.recordedParentIds.size() == 1 &&
           facts.head.target.recordedParentIds[0] == kShaB);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::hiddenByShallow);
}

GC_TEST(interpret_rejects_unreadable_or_non_commit_parent) {
  // 目标父对象在本地读不到：这是「历史不完整」的明确答案（cat-file -t --quiet 退出码 1、无输出）。
  UndoPreflightQueries missing = HealthyQueries();
  missing.parentObject = Answer(1);
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(missing);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::unreadableParent);
  GC_CHECK(facts.head.target.unreadableParentId == kShaB);

  // 那个 ID 存在却不是提交对象：对象库形态超出可安全判读的范围。
  UndoPreflightQueries notCommit = HealthyQueries();
  notCommit.parentObject = Answer(0, L"blob\n");
  facts = gc::git::InterpretUndoPreflight(notCommit);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::inconsistentParents);
  GC_CHECK(facts.head.target.mismatchedParentId == kShaB);

  // 平台层没问过那条查询：不能默认「问过了、能读」。
  UndoPreflightQueries notAsked = HealthyQueries();
  notAsked.parentObjectRan = false;
  facts = gc::git::InterpretUndoPreflight(notAsked);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  // 问的不是历史视图给出的那个第一父：证据接不上，同样不可采信。
  UndoPreflightQueries askedOther = HealthyQueries();
  askedOther.parentObjectQueryOid = std::wstring(kShaC);
  facts = gc::git::InterpretUndoPreflight(askedOther);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  // 那条查询本身失败（超时/启动失败）：归类为问不出来，不是「父对象不存在」。
  UndoPreflightQueries dead = HealthyQueries();
  dead.parentObject = LaunchFailed();
  facts = gc::git::InterpretUndoPreflight(dead);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
  GC_CHECK(!facts.head.target.failure.empty());
}

GC_TEST(interpret_rejects_inconsistent_or_unreadable_commit_object) {
  // 对象记录了一个不同的父：与历史视图对不上。
  UndoPreflightQueries swapped = HealthyQueries();
  swapped.commitObject = Answer(0, CommitObjectText({kShaC}));
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(swapped);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::inconsistentParents);

  // 提交对象读不回来／头部不合约定：判不成任何可撤回的分类。
  UndoPreflightQueries noObject = HealthyQueries();
  noObject.commitObject = LaunchFailed();
  facts = gc::git::InterpretUndoPreflight(noObject);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
  GC_CHECK(!facts.head.target.failure.empty());

  // 没有 tree 行／没有头部结束的空白行：那不是可采信的提交对象。
  UndoPreflightQueries junk = HealthyQueries();
  junk.commitObject = Answer(0, L"parent 2222222222222222222222222222222222222222\n\n正文\n");
  facts = gc::git::InterpretUndoPreflight(junk);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  // 正文里出现的 parent 行不能被当成父提交（第一个空行之前的头部才算）。
  UndoPreflightQueries bodyOnly = HealthyQueries();
  bodyOnly.commitObject = Answer(0, std::wstring(L"tree ") + std::wstring(kTree) +
                                  L"\nauthor x <y> 1 +0000\n\nparent " + std::wstring(kShaB) + L"\n");
  facts = gc::git::InterpretUndoPreflight(bodyOnly);
  // 视图说有父、对象自己说没有：两份证据对不上（不是浅边界那种「视图少报」形态）。
  GC_CHECK(facts.head.target.kind == UndoTargetKind::inconsistentParents);
  GC_CHECK(facts.head.target.recordedParentIds.empty());
}

GC_TEST(interpret_merge_commit_keeps_all_parents) {
  UndoPreflightQueries merge = HealthyQueries();
  MakeMergeShaped(merge);
  const UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(merge);
  GC_CHECK(facts.head.parentsResolved);
  GC_CHECK(facts.head.parentObjectIds.size() == 2);
  GC_CHECK(facts.head.parentObjectIds[0] == kShaB);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::mergeParents);
  GC_CHECK(facts.head.target.recordedParentIds.size() == 2);
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
  GC_CHECK(plan.arguments.empty());
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

GC_TEST(plan_blocks_unshapely_branch_ref) {
  // 合格的完整引用名才谈撤回：HEAD、相对名、带空白或引号的一律挡在边界上。
  GC_CHECK(gc::git::IsSafeUndoTargetRef(L"refs/heads/main"));
  GC_CHECK(gc::git::IsSafeUndoTargetRef(L"refs/heads/中文/分支"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"HEAD"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"main"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/a..b"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/@{1}"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/有 空格"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/带\"引号"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/带\\反斜杠"));
  GC_CHECK(!gc::git::IsSafeUndoTargetRef(L"refs/heads/带\t制表"));

  // 判读层原样带回的引用名不合格时：方案明确拒绝，不产生任何命令。
  UndoPreflightQueries oddRef = HealthyQueries();
  oddRef.symbolicRef = Answer(0, L"refs/heads/有 空格\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(oddRef));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"引用名不合格"), Narrow(plan.blockedReason));
  GC_CHECK(plan.arguments.empty());
}

GC_TEST(plan_blocks_shallow_boundary_and_never_deletes_the_ref) {
  // 浅边界：这是本任务要修的核心缺陷——过去「历史视图里没有父」被当成根提交，直接删掉分支引用。
  UndoPreflightQueries hidden = HealthyQueries();
  hidden.parents = Answer(0, std::wstring(kShaA) + L"\n");  // 对象仍记录 kShaB 为父
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(hidden));
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(!plan.requiresForce);  // 拒绝就是拒绝：不给「强制」留通道
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"浅"), Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"git fetch --unshallow"),
                   Narrow(plan.blockedReason));
  // 点名对象自己记录的那个父，让用户知道被隐藏的是哪一条关系。
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, gc::git::ShortObjectId(kShaB)),
                   Narrow(plan.blockedReason));
  // 不自动联网、不自动 unshallow：文字只能说「由你自己补全」。
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"不会替你联网"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_unreadable_parent_without_touching_the_network) {
  UndoPreflightQueries missing = HealthyQueries();
  missing.parentObject = Answer(1);  // cat-file -t --quiet 明确回答：本地没有这个对象
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(missing));
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"读不到"), Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"不会自动联网"), Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"git fetch"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_inconsistent_parents_and_unreadable_target) {
  UndoPreflightQueries swapped = HealthyQueries();
  swapped.commitObject = Answer(0, CommitObjectText({kShaC}));
  UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(swapped));
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"对不上"), Narrow(plan.blockedReason));
  // 查询已带 --no-replace-objects：拒绝文字里不许把 git replace 当借口。
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"--no-replace-objects"),
                   Narrow(plan.blockedReason));

  // 提交对象读不回来（父关系完全问不出来）同样是明确拒绝。
  UndoPreflightQueries noObject = HealthyQueries();
  noObject.commitObject = LaunchFailed();
  plan = gc::git::BuildUndoCommitPlan(InputFromQueries(noObject));
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.arguments.empty());
}

// ---- 方案：普通撤回 ----

GC_TEST(plan_normal_commit_uses_update_ref_with_expected_old_value) {
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(HealthyQueries()));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);
  // git update-ref --create-reflog -m <说明> <完整分支引用> <父完整ID> <原完整ID>
  GC_CHECK_MESSAGE(
      plan.arguments.size() == 7 && plan.arguments[0] == L"update-ref" &&
          plan.arguments[1] == L"--create-reflog" && plan.arguments[2] == L"-m" &&
          plan.arguments[3] == kReason && plan.arguments[4] == L"refs/heads/main" &&
          plan.arguments[5] == kShaB && plan.arguments[6] == kShaA,
      "普通撤回必须是带预期旧值的 git update-ref <分支引用> <父ID> <原ID>");
  GC_CHECK(plan.operationId == L"undo-commit");
  GC_CHECK(plan.commandLabel == L"git update-ref");
  // 方案绑定的三件事直接可读，不用从参数里反推。
  GC_CHECK(plan.targetRef == L"refs/heads/main");
  GC_CHECK(plan.expectedOldObjectId == kShaA);
  GC_CHECK(plan.newObjectId == kShaB);
  GC_CHECK(plan.targetKind == UndoTargetKind::singleParent);
  // 绝不再用可变的 HEAD，也不再走没有旧值前提的 reset --soft。
  GC_CHECK(!Contains(plan.arguments, L"HEAD"));
  GC_CHECK(!Contains(plan.arguments, L"reset"));
  GC_CHECK(!Contains(plan.arguments, L"--soft"));
  GC_CHECK(!Contains(plan.arguments, L"--hard"));
  GC_CHECK(!Contains(plan.arguments, L"^"));
  GC_CHECK(TextContains(plan.previewText, kShaA));            // 原提交完整 ID 必须摆出来
  GC_CHECK(TextContains(plan.previewText, L"git update-ref"));
  GC_CHECK(TextContains(plan.previewText, L"预期旧值核对"));
  GC_CHECK(TextContains(plan.previewText, L"要移动的分支引用：refs/heads/main"));
  GC_CHECK(TextContains(plan.previewText, L"两份证据"));
  GC_CHECK(TextContains(plan.restoreHint, kShaA));  // 恢复线索
  GC_CHECK(!TextContains(plan.previewText, L"--hard"));
  GC_CHECK(TextContains(plan.notice, L"仅本地"));
  GC_CHECK(TextContains(plan.notice, L"只移动分支引用"));
}

GC_TEST(plan_names_the_confirmed_branch_even_when_another_branch_shares_the_commit) {
  // 两个分支指向同一个提交时，「HEAD」根本不能代表用户确认的那一个：命令必须点名分支引用。
  UndoPreflightQueries feat = HealthyQueries();
  feat.symbolicRef = Answer(0, L"refs/heads/feat\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(feat));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(plan.targetRef == L"refs/heads/feat");
  GC_CHECK(Contains(plan.arguments, L"refs/heads/feat"));
  GC_CHECK(!Contains(plan.arguments, L"HEAD"));
  GC_CHECK(TextContains(plan.previewText, L"refs/heads/feat"));
}

GC_TEST(plan_lists_existing_changes_as_retained_together) {
  // 已有暂存与未暂存改动：确认文字必须明说「一起保留、无法分成两堆」，
  // 但不得把这种只动引用的撤回描述成会发生文本合并冲突。
  UndoPreflightQueries dirty = HealthyQueries();
  dirty.status = Answer(0, JoinNul({Ordinary(L"A.", L"staged-earlier.txt"), Ordinary(L".M", L"edited.txt")}));
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(dirty));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);  // 「改动一起保留」是说明，不是要强制确认的风险
  GC_CHECK(TextContains(plan.previewText, L"一起保留"));
  GC_CHECK(TextContains(plan.previewText, L"无法把两堆分开"));
  GC_CHECK(TextContains(plan.previewText, L"不会产生文本合并冲突"));
}

GC_TEST(plan_not_found_publish_notes_staleness_honestly) {
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(HealthyQueries()));
  GC_CHECK(!plan.requiresForce);
  GC_CHECK(TextContains(plan.previewText, L"本地信息未发现已发布"));
  GC_CHECK_MESSAGE(TextContains(plan.previewText, L"不能证明它从未被"),
                   "没有 fetch 过就要承认不能保证从未 push：" + Narrow(plan.previewText));
}

GC_TEST(plan_notes_the_race_window_honestly) {
  // 单条 update-ref 只原子核对引用的值；符号引用那一环靠执行前的同步复核，
  // 确认文字必须承认这个窗口，不能自称锁住了任意外部写入者。
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(HealthyQueries()));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(TextContains(plan.previewText, L"关于竞争"), Narrow(plan.previewText));
  GC_CHECK(TextContains(plan.previewText, L"git update-ref --stdin"));
  GC_CHECK(!TextContains(plan.previewText, L"绝不可能被别人"));
}

// ---- 方案：根提交、合并提交、浅仓库 ----

GC_TEST(plan_root_commit_undo_deletes_the_named_ref_with_old_value) {
  UndoPreflightQueries root = HealthyQueries();
  MakeRootShaped(root);
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(root));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);
  // git update-ref -d -m <说明> <完整分支引用> <原完整ID>：删的是分支引用本身，不是 HEAD。
  GC_CHECK_MESSAGE(
      plan.arguments.size() == 6 && plan.arguments[0] == L"update-ref" && plan.arguments[1] == L"-d" &&
          plan.arguments[2] == L"-m" && plan.arguments[3] == kReason &&
          plan.arguments[4] == L"refs/heads/main" && plan.arguments[5] == kShaA,
      "根提交必须走带预期旧值核对的 git update-ref -d <分支引用> <完整ID>");
  GC_CHECK(!Contains(plan.arguments, L"HEAD"));
  GC_CHECK(plan.commandLabel == L"git update-ref -d");
  GC_CHECK(plan.targetKind == UndoTargetKind::verifiedRoot);
  GC_CHECK(plan.newObjectId.empty());
  GC_CHECK(plan.expectedOldObjectId == kShaA);
  GC_CHECK(TextContains(plan.previewText, L"尚无提交"));
  GC_CHECK(TextContains(plan.previewText, L"真正的第一个提交"));
  GC_CHECK(TextContains(plan.previewText, L"预期旧值"));
  GC_CHECK(!TextContains(plan.previewText, L"空提交"));  // 不创建空提交冒充撤回
  GC_CHECK(TextContains(plan.notice, L"只删除分支引用"));
  GC_CHECK(TextContains(plan.restoreHint, kShaA));
  // 删除分支引用后它的 reflog 是否存在由 Git 决定：不能承诺一条可能已经消失的记录。
  GC_CHECK_MESSAGE(TextContains(plan.restoreHint, L"不作为找回依据"), Narrow(plan.restoreHint));
  GC_CHECK_MESSAGE(TextContains(plan.previewText, L"不把它当作找回依据"), Narrow(plan.previewText));
}

GC_TEST(plan_shallow_repository_with_readable_parent_needs_force) {
  // 浅仓库但两份证据一致、目标父对象确实在本地：可以撤回，但必须走强制确认并说清历史不完整。
  UndoPreflightQueries shallow = HealthyQueries();
  shallow.shallowState = Answer(0, L"true\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(shallow));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.requiresForce, "浅仓库撤回必须走「强制撤回（仅本地）」确认");
  GC_CHECK(TextContains(plan.previewText, L"浅仓库"));
  GC_CHECK(TextContains(plan.previewText, L"--is-shallow-repository"));
  // 「强制」不换命令：仍然是同一条带预期旧值的引用移动。
  GC_CHECK(plan.arguments[0] == L"update-ref" && Contains(plan.arguments, L"--create-reflog"));
  GC_CHECK(plan.arguments[5] == kShaB && plan.arguments[6] == kShaA);
}

GC_TEST(plan_merge_commit_undo_targets_first_parent_and_forces_confirmation) {
  UndoPreflightQueries merge = HealthyQueries();
  MakeMergeShaped(merge);
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(merge));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.requiresForce, "合并提交撤回属于要「强制撤回（仅本地）」确认的风险");
  GC_CHECK_MESSAGE(plan.arguments[5] == kShaB, "目标必须是第一父提交");
  GC_CHECK(plan.targetKind == UndoTargetKind::mergeParents);
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
  // 「强制」只是确认风险：命令仍是那条带预期旧值的引用移动，绝不带 push 相关任何东西。
  GC_CHECK(plan.arguments[0] == L"update-ref" && Contains(plan.arguments, L"--create-reflog"));
  GC_CHECK(!Contains(plan.arguments, L"push"));
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
  GC_CHECK(!ok.target.queried);

  const UndoHeadFacts dead = gc::git::InterpretUndoHeadSnapshot(LaunchFailed(), Answer(0, L"x"));
  GC_CHECK(!dead.queryOk);
  GC_CHECK(!dead.queryFailure.empty());
}
