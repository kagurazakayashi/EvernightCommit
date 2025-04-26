// 「撤回最近提交」預檢與執行效果的集成測試：在夾具自建的臨時倉庫裡，用真實 Git 驅動
// 生產編排（platform::CollectUndoPreflight）與生產方案層（git::BuildUndoCommitPlan），
// 再把方案合成的命令原樣交給真實 Git 執行，逐項核對：
//   * 分支 / HEAD 完整 ID / 父提交 / 提交對象自己的 parent 行 / 是否淺倉庫 / 標題（含中文）/
//     遠端跟蹤包含 / 工作區狀態的判讀；
//   * 撤回只移動分支引用：索引逐條不差、工作區文件字節不變，原提交改動與既有
//     暫存改動一起留在索引（本步驟要求的「不承諾自動分成兩堆」）；
//   * 兩條路徑都帶預期舊值：普通提交走 git update-ref --create-reflog <分支引用> <父ID> <原ID>，
//     真正根提交走 git update-ref -d <分支引用> <原ID>；舊值對不上時 Git 自己拒絕、分支一步不動，
//     分支被別人推進或換成別的分支之後，舊方案一律落空（含確認後的同步複核）；
//   * 刪除根提交那條分支引用之後，找回只靠原提交完整 ID（本程序不承諾一條可能被刪掉的 reflog）；
//   * 淺克隆 depth=1（歷史邊界）明確拒絕且不刪引用；depth=2（目標父在本地）可以撤回但要強制確認；
//   * 合併提交以第一父為目標並要求「強制撤回」；真實衝突現場與遊離 HEAD 明確拒絕；
//   * 「已知已發佈」用本地 bare 遠端 + push 製造（該 bare fixture 供後續 fetch/push 步驟複用），
//     未推送的新提交則回落為普通確認。
// 只在夾具自己創建並認領所有權的臨時目錄裡跑 Git，遠端只能是同根下的本地 bare 倉庫，絕不接觸網絡。
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "git/undo_commit_plan.h"
#include "git/workspace_model.h"
#include "platform/windows/undo_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::GitQueryResult;
using gc::git::UndoCommitPlan;
using gc::git::UndoCommitPlanInput;
using gc::git::UndoHeadFacts;
using gc::git::UndoPreflightFacts;
using gc::git::UndoPublishEvidence;
using gc::git::UndoTargetKind;
using gc::test::GitFixture;
using gc::test::GitRun;
using gc::test::PrerequisiteFailure;

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool LinesContain(const std::vector<std::wstring>& lines, std::wstring_view needle) {
  for (const std::wstring& line : lines) {
    if (line == needle) {
      return true;
    }
  }
  return false;
}

// 用夾具的隔離執行器裝配生產預檢依賴：查詢走與界面完全相同的代碼路徑。
gc::platform::UndoProbeDeps MakeProbeDeps(GitFixture& fixture) {
  gc::platform::UndoProbeDeps deps;
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) -> GitQueryResult {
    static_cast<void>(exePath);  // 夾具固定使用自己驗證過的 git.exe，工作目錄仍由參數顯式綁定。
    const GitRun run = fixture.Run(arguments, directory);
    GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = run.out;
    result.utf16Error = run.err;
    return result;
  };
  return deps;
}

UndoPreflightFacts Probe(GitFixture& fixture) {
  gc::platform::UndoProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectUndoPreflight(request, MakeProbeDeps(fixture));
}

// 從真實預檢事實直接生產方案（工作區狀態已在預檢裡重讀，這裡不再另讀）。
UndoCommitPlan PlanFromProbe(GitFixture& fixture, const UndoPreflightFacts& facts) {
  UndoCommitPlanInput input;
  input.facts = facts;
  input.repositoryRoot = fixture.RepoDir();
  return gc::git::BuildUndoCommitPlan(input);
}

// 索引的逐條清單（模式 + 對象 ID + 階段 + 路徑）：軟撤回前後必須一字不差。
std::wstring IndexListing(GitFixture& fixture) {
  return fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;
}

// 按原始字節讀工作區裡的文件（路徑相對臨時根）。
std::string ReadFileBytes(GitFixture& fixture, std::wstring_view rootRelativePath) {
  const std::filesystem::path path(fixture.PathInRoot(rootRelativePath));
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    throw PrerequisiteFailure("读取测试文件失败：" + gc::platform::Utf16ToUtf8(path.wstring()));
  }
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 建「本地 bare 遠端 + 兩條已推送的提交」的倉庫，返回 bare 倉庫的絕對路徑。
// 淺克隆用例從它按 --depth 抓取，所以來源必須先過夾具的遠端守衛（只允許臨時根內的本地路徑）。
std::wstring MakePushedOrigin(GitFixture& fixture) {
  fixture.InitBareRepository(L"origin.git");
  const std::wstring bareDir = fixture.PathInRoot(L"origin.git");
  std::string guard;
  if (!fixture.IsAllowedTestRemote(bareDir, guard)) {
    throw PrerequisiteFailure("浅克隆的本地来源不合格：" + guard);
  }
  fixture.InitRepository(L"repo");
  fixture.RunCheckedInRepo({L"remote", L"add", L"origin", bareDir});
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"第一条提交");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"第二条提交");
  fixture.RunCheckedInRepo({L"push", L"-u", L"origin", L"main"});
  return bareDir;
}

// 從本地來源按 depth 淺克隆出新工作區並設為活動工作區（夾具的 CloneRepository 不帶 --depth，
// 這裡走同一條隔離執行路徑，來源路徑已由上面的守衛核過）。
void ShallowCloneTo(GitFixture& fixture, const std::wstring& source, int depth,
                    std::wstring_view directoryName) {
  const GitRun clone =
      fixture.Run({L"clone", L"--depth", std::to_wstring(depth), L"--no-hardlinks", source,
                   std::wstring(directoryName)},
                  fixture.Root());
  if (!clone.Success()) {
    throw PrerequisiteFailure("浅克隆失败：" + ToUtf8(clone.err));
  }
  fixture.SetActiveRepository(directoryName);
}

// 分支自己的 reflog 是否還在（git reflog show <分支> 能不能答上來）。
bool BranchReflogReadable(GitFixture& fixture, std::wstring_view branch) {
  return fixture.Run({L"reflog", L"show", L"--format=%gD", std::wstring(branch)}, fixture.RepoDir())
      .Success();
}

}  // namespace

// ---- 普通分支：兩個提交、乾淨工作區 ----

GC_TEST(undo_probe_reports_real_branch_head_parents_and_summary) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"第一.txt", "内容一\n");
  fixture.StageAll();
  fixture.Commit(L"第一个提交");
  fixture.WriteFile(L"第二.txt", "内容二\n");
  fixture.StageAll();
  fixture.Commit(L"第二个提交：带中文与 & 特殊字符");

  const UndoPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.head.queryOk, "真实预检应判读成功：" + ToUtf8(facts.head.queryFailure));
  GC_CHECK(facts.head.onBranch);
  GC_CHECK(facts.head.branchRef == L"refs/heads/main");
  GC_CHECK(facts.head.branchName == L"main");
  GC_CHECK_MESSAGE(facts.head.headObjectId == fixture.HeadSha(), "HEAD 完整 ID 应与仓库一致");
  GC_CHECK_MESSAGE(facts.head.parentObjectIds.size() == 1 &&
                       facts.head.parentObjectIds[0] == fixture.ParentShaOfHead(),
                   "单父提交的父 ID 应与仓库一致");
  GC_CHECK_MESSAGE(facts.head.selfMatchesHead, "rev-list 的自身 ID 必须与 rev-parse 的 HEAD 对上");
  // 兩份父關係證據在真實倉庫裡必須互相吻合，淺倉庫狀態也要問得出來。
  GC_CHECK(facts.head.target.queried);
  GC_CHECK_MESSAGE(facts.head.target.kind == UndoTargetKind::singleParent,
                   "真实仓库里的普通提交应判成单父：" + ToUtf8(facts.head.target.failure));
  GC_CHECK_MESSAGE(facts.head.target.recordedParentIds == facts.head.parentObjectIds,
                   "提交对象自己记录的 parent 行应与历史视图一致");
  GC_CHECK(facts.head.target.shallowQueried);
  GC_CHECK_MESSAGE(!facts.head.target.repositoryIsShallow, "自建仓库不是浅仓库");
  GC_CHECK_MESSAGE(Contains(fixture.HeadCommitObject(), L"parent "),
                   "Git 的提交对象里确实有 parent 行（判读用的第二份证据）");
  GC_CHECK_MESSAGE(Contains(facts.head.headSummary, L"第二个提交"),
                   "中文标题应原样判读回来：" + ToUtf8(facts.head.headSummary));
  GC_CHECK(facts.statusOk);
  GC_CHECK_MESSAGE(facts.model.staged.empty() && facts.model.unstaged.empty(),
                   "刚提交完应是干净工作区");
  // 沒配任何遠端：無從判斷，絕不能謊稱「本地未發現」。
  GC_CHECK(facts.publish == UndoPublishEvidence::noRemoteRefs);

  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_CHECK_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK(plan.requiresForce);  // 無法判斷髮布狀態也要走強制確認
  GC_CHECK_MESSAGE(
      plan.arguments.size() == 7 && plan.arguments[0] == L"update-ref" &&
          plan.arguments[1] == L"--create-reflog" && plan.arguments[2] == L"-m" &&
          plan.arguments[4] == L"refs/heads/main" &&
          plan.arguments[5] == facts.head.parentObjectIds[0] && plan.arguments[6] == facts.head.headObjectId,
      "普通撤回必须是带预期旧值的 git update-ref --create-reflog <分支引用> <父ID> <原ID>");
  GC_CHECK(!LinesContain(plan.arguments, L"HEAD"));  // 絕不用可變的 HEAD 當目標名
  GC_CHECK(plan.targetRef == L"refs/heads/main");
  GC_CHECK(plan.expectedOldObjectId == facts.head.headObjectId);
  GC_CHECK(plan.newObjectId == facts.head.parentObjectIds[0]);
  // Git 自己也是這麼答的：那條引用當前確實指着確認過的舊值。
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/main") == plan.expectedOldObjectId,
                   "方案绑定的旧值必须就是分支引用的实际值");
}

GC_TEST(undo_probe_on_repository_without_commits_blocks_everything) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"未跟踪.txt", "还没提交\n");

  const UndoPreflightFacts facts = Probe(fixture);
  GC_CHECK_MESSAGE(facts.head.queryOk,
                   "「尚无提交」是明确答案，不是查询失败：" + ToUtf8(facts.head.queryFailure));
  GC_CHECK(facts.head.onBranch);
  GC_CHECK(!facts.head.headResolved);
  GC_CHECK(!facts.publishQueried);  // 沒有 HEAD 就沒有可查詢的對象：沒問過必須如實記着
  GC_CHECK(facts.publish == UndoPublishEvidence::notRun);
  GC_CHECK(!facts.head.target.queried);  // 父關係那三條查詢同樣一條都沒發
  GC_CHECK(facts.statusOk);

  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK_MESSAGE(Contains(plan.blockedReason, L"还没有任何提交"), ToUtf8(plan.blockedReason));
}

// ---- 執行效果：索引與工作區分毫不動 ----

GC_TEST(executing_normal_undo_keeps_index_bytes_and_staged_changes_together) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "A-1\n");
  fixture.StageAll();
  fixture.Commit(L"root");
  // c2 的內容：一個新文件 + 中文路徑文件 + 對 a.txt 的修改。
  fixture.WriteFile(L"b.txt", "B-来自c2\n");
  fixture.WriteFile(L"中文目录/丙文件.txt", "丙-内容\n");
  fixture.WriteFile(L"a.txt", "A-2\n");
  fixture.StageAll();
  fixture.Commit(L"c2");
  // 提交之後用戶又暫存了一個文件、還改了 a.txt 沒暫存：撤回後這些必須與原提交改動一起留着。
  fixture.WriteFile(L"d.txt", "D-我早就暂存了\n");
  fixture.Stage({L"d.txt"});
  fixture.WriteFile(L"a.txt", "A-3-没暂存\n");

  const UndoPreflightFacts facts = Probe(fixture);
  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK(Contains(plan.previewText, L"一起保留"));
  GC_CHECK(Contains(plan.previewText, L"无法把两堆分开"));
  GC_CHECK(Contains(plan.previewText, L"不会产生文本合并冲突"));

  const std::wstring indexBefore = IndexListing(fixture);
  const std::wstring headBefore = fixture.HeadSha();
  const std::wstring parentBefore = fixture.ParentShaOfHead();
  const std::string aBytesBefore = ReadFileBytes(fixture, L"repo\\a.txt");
  const std::string bingBytesBefore = ReadFileBytes(fixture, L"repo\\中文目录\\丙文件.txt");

  fixture.RunCheckedInRepo(plan.arguments);

  GC_CHECK_MESSAGE(fixture.HeadSha() == parentBefore, "分支应挪回第一父");
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "索引必须一字不差：只动了分支引用");
  GC_CHECK(ReadFileBytes(fixture, L"repo\\a.txt") == aBytesBefore);
  GC_CHECK(ReadFileBytes(fixture, L"repo\\中文目录\\丙文件.txt") == bingBytesBefore);
  // 原 c2 的改動 + 用戶已有的暫存，一起表現為「已暫存的更改」；未暫存那一份仍在未暫存側。
  // 同一文件兩側都有改動時 porcelain 合成一條 MM 記錄，這與兩側列表各顯示一條不矛盾。
  const std::vector<std::wstring> porcelain = fixture.StatusPorcelain();
  GC_CHECK(LinesContain(porcelain, L"A  b.txt"));
  GC_CHECK(LinesContain(porcelain, L"A  中文目录/丙文件.txt"));
  GC_CHECK(LinesContain(porcelain, L"A  d.txt"));
  GC_CHECK(LinesContain(porcelain, L"MM a.txt"));
  // 恢復線索必須帶着原提交完整 ID，而且按它寫着的那條命令真的能原樣找回（用例手工執行它，
  // 本程序自己絕不動手）。
  GC_CHECK(Contains(plan.restoreHint, headBefore));
  GC_CHECK(Contains(plan.restoreHint, L"git update-ref"));
  GC_CHECK(Contains(plan.restoreHint, L"refs/heads/main"));
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/main", headBefore});
  GC_CHECK(fixture.HeadSha() == headBefore);
}

GC_TEST(executing_normal_undo_creates_branch_reflog_even_when_logging_is_off) {
  // --create-reflog 的實際效果只能在真實倉庫裡看：把 reflog 記錄關掉之後，
  // 普通提交不會留下分支 reflog，而撤回那條命令必須照樣留下可以核對的找回線索。
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.WriteUserConfig("[core]\n\tlogAllRefUpdates = false\n");
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"第一条");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"第二条");

  const UndoPreflightFacts facts = Probe(fixture);
  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK(LinesContain(plan.arguments, L"--create-reflog"));
  GC_CHECK(Contains(plan.previewText, L"--create-reflog"));
  GC_CHECK(Contains(plan.restoreHint, L"git reflog show"));

  const GitRun before = fixture.Run({L"reflog", L"show", L"main"}, fixture.RepoDir());
  GC_CHECK_MESSAGE(!before.Success(), "关掉记录后普通提交不该留下分支 reflog：" + ToUtf8(before.out));

  fixture.RunCheckedInRepo(plan.arguments);
  const GitRun after =
      fixture.Run({L"reflog", L"show", L"--format=%gs", L"main"}, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(after.Success(),
                     "带 --create-reflog 的撤回必须把这次移动记进分支 reflog：" + ToUtf8(after.err));
  GC_CHECK_MESSAGE(Contains(after.out, L"EvernightCommit:undo-last-commit"),
                   "reflog 里要留着这条撤回的说明：" + ToUtf8(after.out));
}

GC_TEST(executing_root_commit_undo_leaves_unborn_branch_with_contents_staged) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"only.txt", "唯一文件\n");
  fixture.StageAll();
  fixture.Commit(L"根提交");

  const UndoPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.head.parentsResolved && facts.head.parentObjectIds.empty(),
                     "根提交的父列表应为空且判读成功");
  GC_REQUIRE_MESSAGE(facts.head.target.kind == UndoTargetKind::verifiedRoot,
                     "非浅仓库里两份证据都说没有父，才允许走删除引用的路径：" +
                         ToUtf8(facts.head.target.failure));
  GC_CHECK(facts.head.target.shallowQueried);
  GC_CHECK(!facts.head.target.repositoryIsShallow);
  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(
      plan.arguments.size() == 6 && plan.arguments[0] == L"update-ref" && plan.arguments[1] == L"-d" &&
          plan.arguments[2] == L"-m" && plan.arguments[3] == L"EvernightCommit:undo-last-commit" &&
          plan.arguments[4] == L"refs/heads/main" && plan.arguments[5] == facts.head.headObjectId,
      "根提交必须走带预期旧值的 git update-ref -d <分支引用> <完整ID>，不造空提交冒充");
  GC_CHECK(!LinesContain(plan.arguments, L"HEAD"));  // 刪的是確認過的分支引用，不是 HEAD
  GC_CHECK(plan.newObjectId.empty());
  GC_CHECK(Contains(plan.previewText, L"真正的第一个提交"));
  GC_CHECK(Contains(plan.previewText, L"预期旧值"));
  GC_CHECK(!Contains(plan.previewText, L"空提交"));

  const std::wstring originalSha = facts.head.headObjectId;
  const std::wstring indexBefore = IndexListing(fixture);
  const std::string bytesBefore = ReadFileBytes(fixture, L"repo\\only.txt");

  // 先驗證 Git 自己的舊值核對：值不匹配時拒絕執行，分支原樣不動。
  const GitRun wrongValue =
      fixture.Run({L"update-ref", L"-d", L"refs/heads/main",
                   L"9999999999999999999999999999999999999999"},
                  fixture.RepoDir());
  GC_CHECK_MESSAGE(!wrongValue.Success(), "旧值不匹配时 Git 必须拒绝");
  GC_CHECK(fixture.HeadSha() == originalSha);

  fixture.RunCheckedInRepo(plan.arguments);
  GC_CHECK_MESSAGE(fixture.HeadSha().empty(), "分支应回到尚无提交");
  GC_CHECK(fixture.CommitCount() == -1);
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "索引必须一字不差");
  GC_CHECK(ReadFileBytes(fixture, L"repo\\only.txt") == bytesBefore);
  GC_CHECK(LinesContain(fixture.StatusPorcelain(), L"A  only.txt"));
  // 分支引用已經不在了：本程序不把「reflog 還在」當成找回依據，只在文字裡說明可靠的是完整 ID。
  static_cast<void>(BranchReflogReadable(fixture, L"main"));
  GC_CHECK(Contains(plan.restoreHint, originalSha));
  GC_CHECK(Contains(plan.restoreHint, L"不作为找回依据"));
  GC_CHECK(Contains(plan.previewText, L"不把它当作找回依据"));

  // 按線索給的那條命令原樣找回（用例手工執行；本程序不會自動恢復）。
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/main", originalSha});
  GC_CHECK(fixture.HeadSha() == originalSha);
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "找回后索引仍一字不差");
}

// ---- 風險分級：合併提交 / 已發佈 / 真實衝突 / 遊離 HEAD ----

GC_TEST(merge_commit_undo_targets_first_parent_and_forces_confirmation) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"base.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"共同祖先");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"-b", L"feat"});
  fixture.WriteFile(L"feat.txt", "f\n");
  fixture.StageAll();
  fixture.Commit(L"feat 分支的工作");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"main"});
  fixture.WriteFile(L"main.txt", "m\n");
  fixture.StageAll();
  fixture.Commit(L"main 上的最后一个普通提交");
  const std::wstring firstParent = fixture.HeadSha();
  fixture.RunCheckedInRepo({L"merge", L"--no-ff", L"-m", L"合并 feat", L"feat"});

  const UndoPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.head.parentsResolved && facts.head.parentObjectIds.size() == 2,
                     "合并提交应判读出两个父：" + ToUtf8(facts.head.parentsFailure));
  GC_CHECK(facts.head.parentObjectIds[0] == firstParent);

  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_CHECK_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.requiresForce, "合并提交撤回必须走强制确认");
  GC_CHECK_MESSAGE(plan.arguments[5] == firstParent, "目标必须是第一父提交");
  GC_CHECK_MESSAGE(plan.arguments[6] == facts.head.headObjectId, "还要带上确认过的旧值");
  GC_CHECK(plan.targetKind == UndoTargetKind::mergeParents);
  GC_CHECK(Contains(plan.previewText, L"合并提交"));
  GC_CHECK(Contains(plan.previewText, L"第一父提交"));

  const std::wstring indexBefore = IndexListing(fixture);
  fixture.RunCheckedInRepo(plan.arguments);
  GC_CHECK(fixture.HeadSha() == firstParent);
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "合并提交撤回同样不动索引");
}

GC_TEST(remote_bare_origin_marks_commit_as_known_published) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 本地 bare fixture：本步驟用它製造「已知已發佈」，後續 fetch/pull/push 步驟複用同一形態。
  fixture.InitBareRepository(L"origin.git");
  const std::wstring bareDir = fixture.PathInRoot(L"origin.git");
  fixture.InitRepository(L"repo");
  fixture.RunCheckedInRepo({L"remote", L"add", L"origin", bareDir});
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"先立一个根");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"会被推上去的提交");
  fixture.RunCheckedInRepo({L"push", L"-u", L"origin", L"main"});
  const std::wstring pushedSha = fixture.HeadSha();
  const std::wstring parentSha = fixture.ParentShaOfHead();

  {
    const UndoPreflightFacts facts = Probe(fixture);
    GC_CHECK_MESSAGE(facts.publish == UndoPublishEvidence::contained,
                     "刚 push 过的提交应被本地远端跟踪引用判为已发布");
    GC_CHECK(LinesContain(facts.containingRemoteRefs, L"refs/remotes/origin/main"));
    GC_CHECK(facts.head.headObjectId == pushedSha);
    const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
    GC_CHECK_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
    GC_CHECK_MESSAGE(plan.requiresForce, "已知已发布必须走「强制撤回（仅本地）」确认");
    GC_CHECK(Contains(plan.previewText, L"已知已发布"));
    GC_CHECK(Contains(plan.previewText, L"refs/remotes/origin/main"));
    GC_CHECK(Contains(plan.previewText, L"历史分叉"));
    // 「強制」不換命令：仍然是那條帶預期舊值的溫和引用移動，絕不帶 push 相關任何東西。
    GC_CHECK(plan.arguments[0] == L"update-ref" && LinesContain(plan.arguments, L"--create-reflog"));
    GC_CHECK(!LinesContain(plan.arguments, L"push"));
    GC_CHECK(plan.arguments[5] == parentSha);
    GC_CHECK(plan.arguments[6] == pushedSha);
  }

  // 之後再提交一條沒 push 的：本地引用查不到它，回落為普通確認（但話要說不能保證從未 push）。
  fixture.WriteFile(L"c.txt", "3\n");
  fixture.StageAll();
  fixture.Commit(L"还没推的提交");
  const UndoPreflightFacts later = Probe(fixture);
  GC_REQUIRE_MESSAGE(later.head.headObjectId != pushedSha, "预检读的必须是新 HEAD");
  GC_CHECK(later.publish == UndoPublishEvidence::notFound);
  const UndoCommitPlan laterPlan = PlanFromProbe(fixture, later);
  GC_CHECK_MESSAGE(!laterPlan.blocked, ToUtf8(laterPlan.blockedReason));
  GC_CHECK(!laterPlan.requiresForce);
  GC_CHECK(Contains(laterPlan.previewText, L"不能证明它从未被"));
}

GC_TEST(real_conflict_and_detached_head_are_refused) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"both.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"共同祖先");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"-b", L"feat"});
  fixture.WriteFile(L"both.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"feat 改");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"main"});
  fixture.WriteFile(L"both.txt", "3\n");
  fixture.StageAll();
  fixture.Commit(L"main 改");
  const GitRun merge = fixture.Run({L"merge", L"--no-ff", L"feat"}, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(!merge.Success() && !merge.timedOut, "merge 应因真实冲突而失败中止");

  UndoPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.statusOk, "现状判读应成功：" + ToUtf8(facts.statusDetail));
  GC_CHECK(facts.model.HasConflicts());
  UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(Contains(plan.blockedReason, L"冲突"), ToUtf8(plan.blockedReason));
  GC_CHECK(plan.arguments.empty());

  // 遊離 HEAD：沒有分支引用可挪，同樣明確拒絕（即便工作區恢復乾淨）。
  static_cast<void>(fixture.Run({L"merge", L"--abort"}, fixture.RepoDir()));
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"--detach", L"HEAD"});
  facts = Probe(fixture);
  GC_CHECK_MESSAGE(facts.head.headResolved && !facts.head.onBranch,
                   "游离状态下 HEAD 可解析而 symbolic-ref 应答「没有」");
  plan = PlanFromProbe(fixture, facts);
  GC_CHECK(plan.blocked);
  GC_CHECK_MESSAGE(Contains(plan.blockedReason, L"游离 HEAD"), ToUtf8(plan.blockedReason));
}

// ---- 原子舊值前提：確認之後倉庫被別人改動時，舊方案一律落空 ----

GC_TEST(executed_plan_is_rejected_when_the_branch_moved_after_confirmation) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"第一条");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"第二条");

  const UndoPreflightFacts facts = Probe(fixture);
  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  const std::wstring confirmedSha = facts.head.headObjectId;
  GC_CHECK(plan.expectedOldObjectId == confirmedSha);

  // 確認框還開着的時候，另一個終端在這條分支上又提交了一次：舊值已經不再成立。
  fixture.WriteFile(L"c.txt", "3\n");
  fixture.StageAll();
  fixture.Commit(L"确认之后别人推进的提交");
  const std::wstring movedSha = fixture.HeadSha();
  GC_REQUIRE_MESSAGE(movedSha != confirmedSha, "夹具应当真的把分支推进了一条");

  const std::wstring indexBefore = IndexListing(fixture);
  const GitRun stale = fixture.Run(plan.arguments, fixture.RepoDir());
  GC_CHECK_MESSAGE(!stale.Success(), "旧值不匹配时 Git 必须拒绝这份旧方案");
  GC_CHECK_MESSAGE(fixture.HeadSha() == movedSha, "分支一步都不能动：绝不能从新位置再退一步");
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "被拒绝的执行不留任何索引痕迹");

  // 重新預檢讀到的是推進後的 HEAD：新方案綁的是新的舊值，與舊方案不是同一條命令。
  const UndoCommitPlan fresh = PlanFromProbe(fixture, Probe(fixture));
  GC_REQUIRE_MESSAGE(!fresh.blocked, ToUtf8(fresh.blockedReason));
  GC_CHECK_MESSAGE(fresh.expectedOldObjectId == movedSha, "重新预检必须读回推进后的 HEAD");
  GC_CHECK(fresh.expectedOldObjectId != plan.expectedOldObjectId);
  GC_CHECK(fresh.newObjectId == confirmedSha);
}

GC_TEST(undo_binds_the_confirmed_branch_ref_not_the_mutable_head) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"第一条");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"第二条");
  const std::wstring sharedSha = fixture.HeadSha();
  const std::wstring parentSha = fixture.ParentShaOfHead();
  fixture.RunCheckedInRepo({L"branch", L"other", L"main"});  // 另一個分支指着同一個提交

  const UndoPreflightFacts facts = Probe(fixture);
  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK(plan.targetRef == L"refs/heads/main");
  GC_CHECK(!LinesContain(plan.arguments, L"HEAD"));

  // 確認之後、執行之前，用戶把 HEAD 換到了同一條提交上的另一個分支：
  // 提交沒變，但「用戶確認的那個分支」已經變了——界面那一步同步複核必須看出這件事。
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"other"});
  const UndoHeadFacts recheck =
      gc::platform::CaptureUndoHeadSnapshot(fixture.GitExe(), fixture.RepoDir(), 20000);
  GC_REQUIRE_MESSAGE(recheck.queryOk, ToUtf8(recheck.queryFailure));
  GC_CHECK_MESSAGE(recheck.branchRef != facts.head.branchRef, "同步复核应看出分支被换掉");
  GC_CHECK_MESSAGE(recheck.headObjectId == sharedSha, "两个分支指向的是同一个提交");

  // 萬一那條命令還是被執行了，它按引用名綁定的目標也只可能是 refs/heads/main：
  // other 與當前 HEAD 都不受影響，索引一字不動。
  const std::wstring indexBefore = IndexListing(fixture);
  fixture.RunCheckedInRepo(plan.arguments);
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/main") == parentSha,
                   "被移动的只有确认过的那一个分支引用");
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/other") == sharedSha,
                   "另一个分支绝不能被牵连");
  GC_CHECK_MESSAGE(fixture.HeadSha() == sharedSha, "HEAD 还跟着 other，没有换位");
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "索引必须一字不差");
}

// ---- 淺倉庫：歷史邊界必須被認出來，絕不能當成根提交刪掉分支 ----

GC_TEST(shallow_clone_depth_one_is_refused_and_keeps_the_branch) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring bareDir = MakePushedOrigin(fixture);
  const std::wstring tipSha = fixture.HeadSha();
  const std::wstring parentSha = fixture.ParentShaOfHead();
  ShallowCloneTo(fixture, bareDir, 1, L"shallow");

  // 先固定住真實倉庫的形態：只有一條提交，父對象確實不在本地（這正是歷史視圖少報的原因）。
  GC_CHECK_MESSAGE(fixture.HeadSha() == tipSha, "浅克隆的 HEAD 应是远端那条 tip");
  GC_CHECK(fixture.CommitCount() == 1);
  const GitRun parentObject =
      fixture.Run({L"cat-file", L"-t", parentSha}, fixture.RepoDir());
  GC_CHECK_MESSAGE(!parentObject.Success(), "depth=1 的克隆里父对象应当读不到");
  GC_CHECK(Contains(fixture.HeadCommitObject(), L"parent "));  // 對象自己仍記錄着父

  const UndoPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.head.queryOk, ToUtf8(facts.head.queryFailure));
  GC_CHECK_MESSAGE(facts.head.target.repositoryIsShallow, "Git 应回答这是一个浅仓库");
  GC_CHECK_MESSAGE(facts.head.parentObjectIds.empty(), "历史视图里父关系被边界切断了");
  GC_CHECK_MESSAGE(facts.head.target.recordedParentIds.size() == 1 &&
                       facts.head.target.recordedParentIds[0] == parentSha,
                   "提交对象自己记录的父提交必须被读出来，才看得出两边对不上");
  GC_CHECK_MESSAGE(facts.head.target.kind == UndoTargetKind::hiddenByShallow,
                   "这正是旧实现会误判成根提交的形态：" + ToUtf8(facts.head.target.failure));

  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_CHECK_MESSAGE(plan.blocked, "浅边界绝不能进入撤回：" + ToUtf8(plan.blockedReason));
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(!plan.requiresForce);  // 拒絕就是拒絕，不給「強制」留通道
  GC_CHECK_MESSAGE(Contains(plan.blockedReason, L"浅"), ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(Contains(plan.blockedReason, L"git fetch --unshallow"),
                   "要说明补全历史是用户自己的事：" + ToUtf8(plan.blockedReason));

  // 什麼都沒被執行：分支、索引、工作區都還是淺克隆剛完成的樣子。
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/main") == tipSha, "分支引用绝不能被动过");
  GC_CHECK(fixture.HeadSha() == tipSha);
  GC_CHECK(ReadFileBytes(fixture, L"shallow\\b.txt") == "2\n");
}

GC_TEST(shallow_clone_depth_two_undoes_with_force_and_keeps_the_index) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring bareDir = MakePushedOrigin(fixture);
  const std::wstring tipSha = fixture.HeadSha();
  const std::wstring parentSha = fixture.ParentShaOfHead();
  ShallowCloneTo(fixture, bareDir, 2, L"shallow2");

  const UndoPreflightFacts facts = Probe(fixture);
  GC_REQUIRE_MESSAGE(facts.head.queryOk, ToUtf8(facts.head.queryFailure));
  GC_CHECK_MESSAGE(facts.head.target.repositoryIsShallow, "仍然是浅仓库");
  GC_CHECK_MESSAGE(facts.head.target.kind == UndoTargetKind::singleParent,
                   "目标父对象在本地时两份证据一致，可以撤回：" + ToUtf8(facts.head.target.failure));

  const UndoCommitPlan plan = PlanFromProbe(fixture, facts);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.requiresForce, "浅仓库撤回要走强制确认");
  GC_CHECK_MESSAGE(Contains(plan.previewText, L"浅仓库"), ToUtf8(plan.previewText));
  GC_CHECK(plan.arguments[0] == L"update-ref" && plan.arguments[5] == parentSha &&
           plan.arguments[6] == tipSha);

  const std::wstring indexBefore = IndexListing(fixture);
  fixture.RunCheckedInRepo(plan.arguments);
  GC_CHECK_MESSAGE(fixture.HeadSha() == parentSha, "分支应挪回那条本地可读的父提交");
  GC_CHECK_MESSAGE(IndexListing(fixture) == indexBefore, "浅仓库里同样只动引用：索引一字不差");
  GC_CHECK(LinesContain(fixture.StatusPorcelain(), L"A  b.txt"));  // 原 tip 的改動留在暫存區
  GC_CHECK(ReadFileBytes(fixture, L"shallow2\\b.txt") == "2\n");
}
