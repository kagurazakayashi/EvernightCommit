// 「撤回最近提交」判讀與方案的純邏輯測試：全部用樁化的 GitQueryResult 驅動生產邏輯，
// 不起真實 Git、不碰文件系統。覆蓋：預檢查詢參數形態（父關係查詢綁定完整 ID 而非 HEAD）、
// HEAD/父提交/父對象/淺倉庫/遠端包含/工作區六組輸出的判讀三態與壞輸出拒絕、
// 「真正根提交 / 淺邊界 / 缺失父對象 / 非提交對象 / 兩份證據不一致」的分別、
// 方案層的明確拒絕條件（遊離、特殊流程、衝突、讀不回現狀、引用名不合格、父關係不可信）、
// 普通撤回與根提交的帶預期舊值 update-ref 命令構造（絕不用可變的 HEAD、絕不 reset --soft）、
// 合併提交與發佈狀態與淺倉庫的風險分級、確認文字裡「改動一起保留」「不保證從未 push」
// 「強制只越風險提示不換命令」「不承諾已被刪除的 reflog」這些必須說清的點。
// 真實 Git 的形態與命令效果在 undo_probe_fixture_tests.cpp 用臨時倉庫驗證。
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

// 完整對象 ID 形狀的樁值（40 個十六進制字符）。
constexpr std::wstring_view kShaA = L"1111111111111111111111111111111111111111";  // HEAD
constexpr std::wstring_view kShaB = L"2222222222222222222222222222222222222222";  // 第一父
constexpr std::wstring_view kShaC = L"3333333333333333333333333333333333333333";  // 第二父（合併）
constexpr std::wstring_view kTree = L"4444444444444444444444444444444444444444";  // 對象裡的 tree

// 進 reflog 的固定說明：命令裡就是一個 token，測試按它核對參數位次。
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

// git cat-file commit 的原始對象文本樁：頭部按 parent 行給定，空行之後是正文。
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

// 一份「可以正常撤回」的樁預檢：HEAD 可解析、兩份證據都說是單父、父對象在本地、
// 倉庫明確不是淺倉庫、遠端引用本地未發現已發佈、工作區乾淨。
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
  // 剝皮問法的「合格」回答就是那個 ID 自己（實測：合格的提交經 --verify --quiet 原樣答出）。
  queries.parentObject = Answer(0, std::wstring(kShaB) + L"\n");
  queries.headSummary = Answer(0, L"fix: 修一个很要紧的 bug\n");
  queries.remoteRefs = Answer(0, L"refs/remotes/origin/main\n");
  queries.remoteContains = Answer(0, L"");
  queries.status = Answer(0, L"");
  return queries;
}

// 改成「兩處都記錄沒有父」：真正根提交的形態（淺倉庫狀態由調用方決定要不要再改）。
void MakeRootShaped(UndoPreflightQueries& queries) {
  queries.parents = Answer(0, std::wstring(kShaA) + L"\n");
  queries.commitObject = Answer(0, CommitObjectText({}));
  queries.parentObjectRan = false;
  queries.parentObjectQueryOid.clear();
  queries.parentObject = {};
}

// 改成「合併提交」：歷史視圖與對象頭都記錄兩個父。
void MakeMergeShaped(UndoPreflightQueries& queries) {
  queries.parents =
      Answer(0, std::wstring(kShaA) + L" " + std::wstring(kShaB) + L" " + std::wstring(kShaC) + L"\n");
  queries.commitObject = Answer(0, CommitObjectText({kShaB, kShaC}));
  queries.parentObjectQueryOid = std::wstring(kShaB);
  queries.parentObject = Answer(0, std::wstring(kShaB) + L"\n");
}

UndoCommitPlanInput InputFromQueries(const UndoPreflightQueries& queries) {
  UndoCommitPlanInput input;
  input.facts = gc::git::InterpretUndoPreflight(queries);
  input.repositoryRoot = L"D:\\repo";
  return input;
}

}  // namespace

// ---- 查詢參數形態 ----

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
    // 預檢一律只讀：不出現任何會改動倉庫、聯網、或更激烈的命令詞。
    const std::wstring_view forbiddenWords[] = {L"--hard", L"revert", L"push", L"fetch", L"unshallow",
                                               L"reset", L"checkout", L"clean", L"stash"};
    for (const std::wstring_view forbidden : forbiddenWords) {
      GC_CHECK(!Contains(arguments, forbidden));
    }
  }
  // 對象相關的查詢必須帶 --no-replace-objects：被判讀採信的始終是倉庫裡那個真實對象。
  GC_CHECK(Contains(gc::git::BuildUndoHeadCommitArguments(dir), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoParentsArguments(dir, kShaA), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoCommitObjectArguments(dir, kShaA), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoParentObjectArguments(dir, kShaB), L"--no-replace-objects"));
  GC_CHECK(Contains(gc::git::BuildUndoRemoteContainsArguments(dir, kShaA), L"--no-replace-objects"));
  // 綁對象 ID 的查詢絕不用可變的 HEAD：問的必須是剛核實過的那個完整 ID。
  const std::vector<std::wstring> parents = gc::git::BuildUndoParentsArguments(dir, kShaA);
  GC_CHECK(Contains(parents, kShaA) && !Contains(parents, L"HEAD"));
  GC_CHECK(Contains(gc::git::BuildUndoCommitObjectArguments(dir, kShaA), kShaA));
  // 父對象的可讀性問法：`rev-parse --verify --quiet <ID>^{commit}`。
  // 回歸釘：絕不能退回 `cat-file -t --quiet`——本機 Git 2.53 對它一律退出碼 129，預檢一問就報錯。
  const std::vector<std::wstring> parentObject = gc::git::BuildUndoParentObjectArguments(dir, kShaB);
  GC_CHECK(Contains(parentObject, L"rev-parse") && Contains(parentObject, L"--verify") &&
           Contains(parentObject, std::wstring(kShaB) + L"^{commit}"));
  GC_CHECK(!Contains(parentObject, L"cat-file"));
  // 形態不合格的 ID 根本不配送進 Git（返回空數組＝跳過這條查詢）。
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
  // 沒有父、查詢沒答上來、輸出不合約定：都返回空串，調用方據此跳過那條追問。
  GC_CHECK(gc::git::UndoFirstReportedParent(Answer(0, std::wstring(kShaA) + L"\n")).empty());
  GC_CHECK(gc::git::UndoFirstReportedParent(Answer(128, L"", L"fatal: bad object\n")).empty());
  GC_CHECK(gc::git::UndoFirstReportedParent(LaunchFailed()).empty());
  GC_CHECK(gc::git::UndoFirstReportedParent(Answer(0, std::wstring(kShaA) + L" 0m0r3-b4d\n")).empty());
}

// ---- 判讀 ----

GC_TEST(undo_query_read_separates_absent_from_failed) {
  GC_CHECK(gc::git::ReadUndoQuery(Answer(0, L"refs/heads/main\n")).outcome ==
           UndoQueryOutcome::answered);
  // --quiet 系查詢的「退出碼 1 + 無輸出」是明確答案，不是錯誤。
  GC_CHECK(gc::git::ReadUndoQuery(Answer(1)).outcome == UndoQueryOutcome::noResult);
  // 退出碼 1 卻帶着標準輸出（不該出現在 --quiet 系查詢）→ 按失敗歸類，不能吞掉。
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
  // 父關係分類：兩份證據一致、目標父對象本地可讀、倉庫明確不是淺倉庫。
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
  // HEAD 不可解析時依賴它的查詢根本沒發：遠端結論必須是 notRun，不能謊稱「查過沒有」。
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

  // rev-list 報的自身 ID 與 rev-parse 的 HEAD 不一致：說明查詢之間 HEAD 動了，不能用。
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

// ---- 判讀：真正根提交 / 淺邊界 / 缺失父對象 / 非提交對象 ----

GC_TEST(interpret_verified_root_needs_both_evidences_and_not_shallow) {
  UndoPreflightQueries root = HealthyQueries();
  MakeRootShaped(root);
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(root);
  GC_CHECK(facts.head.parentsResolved);
  GC_CHECK(facts.head.parentObjectIds.empty());
  GC_CHECK(facts.head.target.kind == UndoTargetKind::verifiedRoot);

  // 淺倉庫裡「兩處都沒有父」只是歷史被截斷的形態，絕不能認作真正的第一個提交。
  UndoPreflightQueries shallowRoot = HealthyQueries();
  MakeRootShaped(shallowRoot);
  shallowRoot.shallowState = Answer(0, L"true\n");
  facts = gc::git::InterpretUndoPreflight(shallowRoot);
  GC_CHECK(facts.head.target.shallowQueried);
  GC_CHECK(facts.head.target.repositoryIsShallow);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::hiddenByShallow);

  // 問不出淺不淺（老版本 Git 不認這個選項）：連「是不是根提交」都判斷不了。
  UndoPreflightQueries unknownShallow = HealthyQueries();
  MakeRootShaped(unknownShallow);
  unknownShallow.shallowState = Answer(129, L"", L"error: unknown option `is-shallow-repository'\n");
  facts = gc::git::InterpretUndoPreflight(unknownShallow);
  GC_CHECK(!facts.head.target.shallowQueried);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
  GC_CHECK_MESSAGE(TextContains(facts.head.target.failure, L"浅"), Narrow(facts.head.target.failure));

  // 答案不是 true/false：按問不出來處理，不猜。
  UndoPreflightQueries junkShallow = HealthyQueries();
  MakeRootShaped(junkShallow);
  junkShallow.shallowState = Answer(0, L"maybe\n");
  facts = gc::git::InterpretUndoPreflight(junkShallow);
  GC_CHECK(!facts.head.target.shallowQueried);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
}

GC_TEST(interpret_shallow_boundary_is_hidden_parent_relation) {
  // 淺邊界的典型形態：提交對象自己記錄了父提交，歷史視圖卻不給（父對象沒抓下來）。
  UndoPreflightQueries hidden = HealthyQueries();
  hidden.parents = Answer(0, std::wstring(kShaA) + L"\n");
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(hidden);
  GC_CHECK(facts.head.parentsResolved);  // 「沒有父」是歷史視圖的明確答案
  GC_CHECK(facts.head.target.recordedParentIds.size() == 1 &&
           facts.head.target.recordedParentIds[0] == kShaB);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::hiddenByShallow);
}

GC_TEST(interpret_rejects_unreadable_or_non_commit_parent) {
  // 目標父對象在本地讀不到：這是「歷史不完整」的明確答案（--quiet 系契約：退出碼 1、無輸出）。
  UndoPreflightQueries missing = HealthyQueries();
  missing.parentObject = Answer(1);
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(missing);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::unreadableParent);
  GC_CHECK(facts.head.target.unreadableParentId == kShaB);

  // 那個 ID 存在卻不是提交對象（或要靠剝皮才成提交）：對象庫形態超出可安全判讀的範圍。
  UndoPreflightQueries notCommit = HealthyQueries();
  notCommit.parentObject = Answer(0, L"blob\n");
  facts = gc::git::InterpretUndoPreflight(notCommit);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::inconsistentParents);
  GC_CHECK(facts.head.target.mismatchedParentId == kShaB);

  // 平臺層沒問過那條查詢：不能默認「問過了、能讀」。
  UndoPreflightQueries notAsked = HealthyQueries();
  notAsked.parentObjectRan = false;
  facts = gc::git::InterpretUndoPreflight(notAsked);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  // 問的不是歷史視圖給出的那個第一父：證據接不上，同樣不可採信。
  UndoPreflightQueries askedOther = HealthyQueries();
  askedOther.parentObjectQueryOid = std::wstring(kShaC);
  facts = gc::git::InterpretUndoPreflight(askedOther);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  // 那條查詢本身失敗（超時/啓動失敗）：歸類為問不出來，不是「父對象不存在」。
  UndoPreflightQueries dead = HealthyQueries();
  dead.parentObject = LaunchFailed();
  facts = gc::git::InterpretUndoPreflight(dead);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
  GC_CHECK(!facts.head.target.failure.empty());
}

GC_TEST(interpret_rejects_inconsistent_or_unreadable_commit_object) {
  // 對象記錄了一個不同的父：與歷史視圖對不上。
  UndoPreflightQueries swapped = HealthyQueries();
  swapped.commitObject = Answer(0, CommitObjectText({kShaC}));
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(swapped);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::inconsistentParents);

  // 提交對象讀不回來／頭部不合約定：判不成任何可撤回的分類。
  UndoPreflightQueries noObject = HealthyQueries();
  noObject.commitObject = LaunchFailed();
  facts = gc::git::InterpretUndoPreflight(noObject);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);
  GC_CHECK(!facts.head.target.failure.empty());

  // 沒有 tree 行／沒有頭部結束的空白行：那不是可採信的提交對象。
  UndoPreflightQueries junk = HealthyQueries();
  junk.commitObject = Answer(0, L"parent 2222222222222222222222222222222222222222\n\n正文\n");
  facts = gc::git::InterpretUndoPreflight(junk);
  GC_CHECK(facts.head.target.kind == UndoTargetKind::undetermined);

  // 正文裡出現的 parent 行不能被當成父提交（第一個空行之前的頭部纔算）。
  UndoPreflightQueries bodyOnly = HealthyQueries();
  bodyOnly.commitObject = Answer(0, std::wstring(L"tree ") + std::wstring(kTree) +
                                  L"\nauthor x <y> 1 +0000\n\nparent " + std::wstring(kShaB) + L"\n");
  facts = gc::git::InterpretUndoPreflight(bodyOnly);
  // 視圖說有父、對象自己說沒有：兩份證據對不上（不是淺邊界那種「視圖少報」形態）。
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
  // 命中：已知已發佈，並保留命中的引用列表。
  UndoPreflightQueries contained = HealthyQueries();
  contained.remoteContains = Answer(0, L"refs/remotes/origin/main\nrefs/remotes/origin/dev\n");
  UndoPreflightFacts facts = gc::git::InterpretUndoPreflight(contained);
  GC_CHECK(facts.publish == UndoPublishEvidence::contained);
  GC_CHECK(facts.containingRemoteRefs.size() == 2);

  // 本地沒有任何遠端跟蹤引用：無從判斷。
  UndoPreflightQueries noRefs = HealthyQueries();
  noRefs.remoteRefs = Answer(0, L"");
  facts = gc::git::InterpretUndoPreflight(noRefs);
  GC_CHECK(facts.publish == UndoPublishEvidence::noRemoteRefs);

  // 查詢本身失敗：連「本地沒有」都說不了。
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

  // 輸出不合約定同樣整體作廢，絕不拿半套列表去談撤回。
  UndoPreflightQueries junk = HealthyQueries();
  junk.status = Answer(0, JoinNul({L"1 .M N... 100644"}));
  facts = gc::git::InterpretUndoPreflight(junk);
  GC_CHECK(!facts.statusOk);
}

// ---- 方案：明確拒絕的條件 ----

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
  // 特殊流程是明確拒絕：blocked 的方案不攜帶任何命令，也不給「強制」留通道。
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
  // 讀不回工作區/索引現狀就不撤回：看不到要「一起保留」的東西，確認框就是空的承諾。
  UndoPreflightQueries noStatus = HealthyQueries();
  noStatus.status = LaunchFailed();
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(noStatus));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"工作区/索引现状"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_when_publish_query_missing) {
  // 判讀層正常、但預檢根本沒問過遠端：拒絕打開確認框（不能拿「沒查」冒充「沒查到」）。
  UndoCommitPlanInput input = InputFromQueries(HealthyQueries());
  input.facts.publishQueried = false;
  input.facts.publish = UndoPublishEvidence::notRun;
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK(plan.blocked);
}

GC_TEST(plan_blocks_unshapely_branch_ref) {
  // 合格的完整引用名才談撤回：HEAD、相對名、帶空白或引號的一律擋在邊界上。
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

  // 判讀層原樣帶回的引用名不合格時：方案明確拒絕，不產生任何命令。
  UndoPreflightQueries oddRef = HealthyQueries();
  oddRef.symbolicRef = Answer(0, L"refs/heads/有 空格\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(oddRef));
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"引用名不合格"), Narrow(plan.blockedReason));
  GC_CHECK(plan.arguments.empty());
}

GC_TEST(plan_blocks_shallow_boundary_and_never_deletes_the_ref) {
  // 淺邊界：這是本任務要修的核心缺陷——過去「歷史視圖裡沒有父」被當成根提交，直接刪掉分支引用。
  UndoPreflightQueries hidden = HealthyQueries();
  hidden.parents = Answer(0, std::wstring(kShaA) + L"\n");  // 對象仍記錄 kShaB 為父
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(hidden));
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(!plan.requiresForce);  // 拒絕就是拒絕：不給「強制」留通道
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"浅"), Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"git fetch --unshallow"),
                   Narrow(plan.blockedReason));
  // 點名對象自己記錄的那個父，讓用戶知道被隱藏的是哪一條關係。
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, gc::git::ShortObjectId(kShaB)),
                   Narrow(plan.blockedReason));
  // 不自動聯網、不自動 unshallow：文字只能說「由你自己補全」。
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"不会替你联网"), Narrow(plan.blockedReason));
}

GC_TEST(plan_blocks_unreadable_parent_without_touching_the_network) {
  UndoPreflightQueries missing = HealthyQueries();
  missing.parentObject = Answer(1);  // --quiet 系契約的明確回答：本地沒有這個對象
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
  // 查詢已帶 --no-replace-objects：拒絕文字裡不許把 git replace 當藉口。
  GC_CHECK_MESSAGE(TextContains(plan.blockedReason, L"--no-replace-objects"),
                   Narrow(plan.blockedReason));

  // 提交對象讀不回來（父關係完全問不出來）同樣是明確拒絕。
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
  // git update-ref --create-reflog -m <說明> <完整分支引用> <父完整ID> <原完整ID>
  GC_CHECK_MESSAGE(
      plan.arguments.size() == 7 && plan.arguments[0] == L"update-ref" &&
          plan.arguments[1] == L"--create-reflog" && plan.arguments[2] == L"-m" &&
          plan.arguments[3] == kReason && plan.arguments[4] == L"refs/heads/main" &&
          plan.arguments[5] == kShaB && plan.arguments[6] == kShaA,
      "普通撤回必须是带预期旧值的 git update-ref <分支引用> <父ID> <原ID>");
  GC_CHECK(plan.operationId == L"undo-commit");
  GC_CHECK(plan.commandLabel == L"git update-ref");
  // 方案綁定的三件事直接可讀，不用從參數裡反推。
  GC_CHECK(plan.targetRef == L"refs/heads/main");
  GC_CHECK(plan.expectedOldObjectId == kShaA);
  GC_CHECK(plan.newObjectId == kShaB);
  GC_CHECK(plan.targetKind == UndoTargetKind::singleParent);
  // 絕不再用可變的 HEAD，也不再走沒有舊值前提的 reset --soft。
  GC_CHECK(!Contains(plan.arguments, L"HEAD"));
  GC_CHECK(!Contains(plan.arguments, L"reset"));
  GC_CHECK(!Contains(plan.arguments, L"--soft"));
  GC_CHECK(!Contains(plan.arguments, L"--hard"));
  GC_CHECK(!Contains(plan.arguments, L"^"));
  GC_CHECK(TextContains(plan.previewText, kShaA));            // 原提交完整 ID 必須擺出來
  GC_CHECK(TextContains(plan.previewText, L"git update-ref"));
  GC_CHECK(TextContains(plan.previewText, L"预期旧值核对"));
  GC_CHECK(TextContains(plan.previewText, L"要移动的分支引用：refs/heads/main"));
  GC_CHECK(TextContains(plan.previewText, L"两份证据"));
  GC_CHECK(TextContains(plan.restoreHint, kShaA));  // 恢復線索
  GC_CHECK(!TextContains(plan.previewText, L"--hard"));
  GC_CHECK(TextContains(plan.notice, L"仅本地"));
  GC_CHECK(TextContains(plan.notice, L"只移动分支引用"));
}

GC_TEST(plan_names_the_confirmed_branch_even_when_another_branch_shares_the_commit) {
  // 兩個分支指向同一個提交時，「HEAD」根本不能代表用戶確認的那一個：命令必須點名分支引用。
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
  // 已有暫存與未暫存改動：確認文字必須明說「一起保留、無法分成兩堆」，
  // 但不得把這種只動引用的撤回描述成會發生文本合併衝突。
  UndoPreflightQueries dirty = HealthyQueries();
  dirty.status = Answer(0, JoinNul({Ordinary(L"A.", L"staged-earlier.txt"), Ordinary(L".M", L"edited.txt")}));
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(dirty));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);  // 「改動一起保留」是說明，不是要強制確認的風險
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
  // 單條 update-ref 只原子核對引用的值；符號引用那一環靠執行前的同步複核，
  // 確認文字必須承認這個窗口，不能自稱鎖住了任意外部寫入者。
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(HealthyQueries()));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(TextContains(plan.previewText, L"关于竞争"), Narrow(plan.previewText));
  GC_CHECK(TextContains(plan.previewText, L"git update-ref --stdin"));
  GC_CHECK(!TextContains(plan.previewText, L"绝不可能被别人"));
}

// ---- 方案：根提交、合併提交、淺倉庫 ----

GC_TEST(plan_root_commit_undo_deletes_the_named_ref_with_old_value) {
  UndoPreflightQueries root = HealthyQueries();
  MakeRootShaped(root);
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(root));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(!plan.requiresForce);
  // git update-ref -d -m <說明> <完整分支引用> <原完整ID>：刪的是分支引用本身，不是 HEAD。
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
  GC_CHECK(!TextContains(plan.previewText, L"空提交"));  // 不創建空提交冒充撤回
  GC_CHECK(TextContains(plan.notice, L"只删除分支引用"));
  GC_CHECK(TextContains(plan.restoreHint, kShaA));
  // 刪除分支引用後它的 reflog 是否存在由 Git 決定：不能承諾一條可能已經消失的記錄。
  GC_CHECK_MESSAGE(TextContains(plan.restoreHint, L"不作为找回依据"), Narrow(plan.restoreHint));
  GC_CHECK_MESSAGE(TextContains(plan.previewText, L"不把它当作找回依据"), Narrow(plan.previewText));
}

GC_TEST(plan_shallow_repository_with_readable_parent_needs_force) {
  // 淺倉庫但兩份證據一致、目標父對象確實在本地：可以撤回，但必須走強制確認並說清歷史不完整。
  UndoPreflightQueries shallow = HealthyQueries();
  shallow.shallowState = Answer(0, L"true\n");
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(InputFromQueries(shallow));
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.requiresForce, "浅仓库撤回必须走「强制撤回（仅本地）」确认");
  GC_CHECK(TextContains(plan.previewText, L"浅仓库"));
  GC_CHECK(TextContains(plan.previewText, L"--is-shallow-repository"));
  // 「強制」不換命令：仍然是同一條帶預期舊值的引用移動。
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
  // 「強制」只是確認風險：命令仍是那條帶預期舊值的引用移動，絕不帶 push 相關任何東西。
  GC_CHECK(plan.arguments[0] == L"update-ref" && Contains(plan.arguments, L"--create-reflog"));
  GC_CHECK(!Contains(plan.arguments, L"push"));
  GC_CHECK(!TextContains(plan.previewText, L"force push") ||
           TextContains(plan.previewText, L"更不会自动 force push"));
}

GC_TEST(plan_undeterminable_publish_forces_confirmation) {
  // 信息不足（查詢失敗 / 沒有任何遠端跟蹤引用）都歸「無法判斷」→ 要強制確認。
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
  input.captured.shortSha = gc::git::ShortObjectId(kShaB);  // 界面當時顯示的是另一條提交
  input.captured.stagedItems = 7;                           // 與剛讀回的 0 項不一致
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK_MESSAGE(!plan.stateChangeNote.empty(), "点击瞬间的界面摘要与刚读回的现状不同时必须说明");
  GC_CHECK(TextContains(plan.stateChangeNote, L"以刚刚读回的为准"));
  GC_CHECK(TextContains(plan.previewText, L"以刚刚读回的为准"));

  // 短 ID 長度可能與 --short 不同：前綴一致就不該虛報「HEAD 變了」。
  UndoCommitPlanInput same = InputFromQueries(HealthyQueries());
  same.captured.valid = true;
  same.captured.hasHead = true;
  same.captured.shortSha = std::wstring(kShaA).substr(0, 7);
  const UndoCommitPlan calm = gc::git::BuildUndoCommitPlan(same);
  GC_CHECK_MESSAGE(calm.stateChangeNote.empty(), "同一个提交的不同长度短 ID 不该被判成变化");
}

GC_TEST(plan_index_lock_warns_without_blocking) {
  // 鎖歸 Git 自己管：這裡只提醒，不刪鎖、不終止別的進程，也不因此拒絕撤回。
  UndoCommitPlanInput input = InputFromQueries(HealthyQueries());
  input.workflow.indexLocked = true;
  const UndoCommitPlan plan = gc::git::BuildUndoCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Narrow(plan.blockedReason));
  GC_CHECK(TextContains(plan.notice, L"index.lock"));
}

// ---- 複核快照的判讀 ----

GC_TEST(interpret_head_snapshot_only_reads_branch_and_head) {
  const UndoHeadFacts ok = gc::git::InterpretUndoHeadSnapshot(Answer(0, L"refs/heads/main\n"),
                                                              Answer(0, std::wstring(kShaA) + L"\n"));
  GC_CHECK(ok.queryOk);
  GC_CHECK(ok.branchRef == L"refs/heads/main");
  GC_CHECK(ok.headObjectId == kShaA);
  GC_CHECK(!ok.parentsQueried);  // 複核根本不問父提交
  GC_CHECK(!ok.target.queried);

  const UndoHeadFacts dead = gc::git::InterpretUndoHeadSnapshot(LaunchFailed(), Answer(0, L"x"));
  GC_CHECK(!dead.queryOk);
  GC_CHECK(!dead.queryFailure.empty());
}

// ---- 確認後、執行前複核的裁決（界面據此決定發不發那條 update-ref）----

GC_TEST(undo_recheck_mismatch_refusal_is_verbatim) {
  UndoPreflightFacts preflight;
  preflight.head.queryOk = true;
  preflight.head.branchRef = L"refs/heads/main";
  preflight.head.headObjectId = std::wstring(kShaA);

  // 一致：空串 = 可以發命令。
  UndoHeadFacts same;
  same.queryOk = true;
  same.branchRef = L"refs/heads/main";
  same.headObjectId = std::wstring(kShaA);
  GC_CHECK_MESSAGE(gc::git::DescribeUndoRecheckMismatch(same, preflight).empty(),
                   "復核與預檢一致時不得攔下命令");

  // HEAD 換了一份提交：拒绝说明必须把两边的位置都报出来（短 ID 形态）。
  UndoHeadFacts moved;
  moved.queryOk = true;
  moved.branchRef = L"refs/heads/main";
  moved.headObjectId = std::wstring(kShaB);
  const std::wstring refusal = gc::git::DescribeUndoRecheckMismatch(moved, preflight);
  GC_CHECK(TextContains(refusal, L"确认之后、执行之前，HEAD/分支又变了"));
  GC_CHECK(TextContains(refusal, gc::git::ShortObjectId(kShaB)));
  GC_CHECK(TextContains(refusal, L"本次没有执行任何命令"));

  // 换到别的分支：分支名要出现在说明里。
  UndoHeadFacts switched;
  switched.queryOk = true;
  switched.branchRef = L"refs/heads/dev";
  switched.headObjectId = std::wstring(kShaA);
  GC_CHECK(TextContains(gc::git::DescribeUndoRecheckMismatch(switched, preflight),
                        L"refs/heads/dev"));

  // 不在分支上：按「不在分支上」如实说，不留空。
  UndoHeadFacts detached;
  detached.queryOk = true;
  detached.headObjectId = std::wstring(kShaA);
  GC_CHECK(TextContains(gc::git::DescribeUndoRecheckMismatch(detached, preflight), L"不在分支上"));

  // 复核没完成：另一套措辞，且不合并成一句「请重试」。
  UndoHeadFacts failed;
  failed.queryOk = false;
  failed.queryFailure = L"git 启动失败（示例原因）";
  const std::wstring failedRefusal = gc::git::DescribeUndoRecheckMismatch(failed, preflight);
  GC_CHECK(TextContains(failedRefusal, L"确认后复核 HEAD 没能完成（git 启动失败（示例原因））"));
  GC_CHECK(TextContains(failedRefusal, L"仓库状态正在重读"));

  // 失败但没有具体原因时给「原因未知」，不留空括号。
  UndoHeadFacts failedQuiet;
  failedQuiet.queryOk = false;
  GC_CHECK(TextContains(gc::git::DescribeUndoRecheckMismatch(failedQuiet, preflight), L"原因未知"));
}
