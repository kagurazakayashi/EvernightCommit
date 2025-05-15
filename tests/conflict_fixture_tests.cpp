// 「Git 已停在冲突/暂停状态」的真实 Git 集成测试：全部在夹具自己创建并认领所有权的临时目录里，
// 用真实 Git 驱动生产逻辑（platform::ProbeConflictMarkers、platform::CollectConflictState
// 与 git::InterpretConflictState / git::BuildConflict*Plan），再把判读结论与仓库里的实际状态对着看。
//
// 分组口径（运行器把夹具文件里未列入只读名单的用例归入 git-mutating）：
//   * 前两条只跑 `git init`、只读查询与在临时根里造物（建目录、写痕迹档案、写工作区文件但不 add）：
//     不建提交、不推送、不动索引、不改任何配置 —— 逐行核实过，登记进 test_main.cpp 的只读名单；
//   * 后面每条都要提交图（制造真冲突、真变基、真 --continue/--abort 的后果），
//     一律由维护者运行，本会话绝不代跑。
//
// 覆盖的场景与为什么必须用真 Git：
//   * 三条只读查询的形态真的被 Git 接受（`diff --name-only -z --diff-filter=U`、
//     `symbolic-ref HEAD`、`rev-parse --verify --quiet HEAD`），不是 129 用法错误——
//     纯逻辑全绿证明不了这一点，而形态一错整条判读在真机上永远问不出东西；
//   * 痕迹档案的名称与内容形态由 Git 自己写下：MERGE_HEAD 里是那一份提交的完整对象 ID，
//     rebase-merge\\ 里的 head-name/onto/msgnum/end 各是什么，读回来的就是什么；
//   * 真变基冲突现场同时留着 rebase-merge\\ 与 CHERRY_PICK_HEAD：这检验「同伴痕迹不算并存」
//     那条判读——判错就会把一次正常变基说成状态不一致而一路拒绝到底；
//   * `git rebase --apply` 与 `git am` 共用 rebase-apply\\ 这一层：两条用例分别钉住
//     「读得回 head-name/onto 才算变基」与「读不回就承认分不清、两条命令都不给」；
//   * `merge --continue` 真的建立两个父的合并提交并清掉痕迹；钩子非 0 退出时现场原样留着、
//     提交数不变（这正是界面那句「不做任何自动恢复」的依据）；
//   * `merge --abort` / `rebase --abort` 之后痕迹消失：复核要说「已经没有痕迹」而不是
//     「没读回来」，方案层也不再生成任何命令。
//
// 关于编辑器：Git 文档写明变基的合并后端会在 --continue 时打开编辑器（apply 后端直接用原说明），
// merge --continue 则是「确认有中断中的合并后调用 git commit」。夹具用一个会留下运行痕迹的
// core.editor，把「这一步到底开没开编辑器」打印出来给维护者核对，不作为断言——
// 本程序的确认文字只说「由 Git 自己决定，本程序没传 --no-edit/-m」。
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "git/conflict_state.h"
#include "git/repository.h"
#include "platform/windows/conflict_probe.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ConflictFlowKind;
using gc::git::ConflictMarkerFacts;
using gc::git::ConflictOperationPlan;
using gc::git::ConflictStateFacts;
using gc::git::GitQueryResult;
using gc::platform::CollectConflictState;
using gc::platform::ConflictProbeDeps;
using gc::platform::ConflictProbeRequest;
using gc::platform::MakeConflictProbeDeps;
using gc::platform::ProbeConflictMarkers;
using gc::test::GitFixture;
using gc::test::GitRun;

void WriteText(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary);
  stream << content;
  stream.close();
}

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

std::wstring Trimmed(const std::wstring& text) { return gc::git::TrimWide(text); }

GitQueryResult AsQuery(const GitRun& run) {
  GitQueryResult result;
  result.started = run.started;
  result.timedOut = run.timedOut;
  result.exited = run.exited;
  result.exitCode = static_cast<int>(run.exitCode);
  result.utf16Output = run.out;
  result.utf16Error = run.err;
  return result;
}

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

// 界面传给探测的两个目录：工作区根与绝对 Git 目录都由 Git 自己回答（与仓库识别同源），
// 夹具因此不拿「工作区根 + \.git」去猜链接工作树那种落点。
ConflictProbeRequest FixtureRequest(GitFixture& fixture) {
  ConflictProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.absoluteGitDir =
      Trimmed(fixture.RunCheckedInRepo({L"rev-parse", L"--absolute-git-dir"}).out);
  request.timeoutMilliseconds = 20000;
  return request;
}

ConflictProbeDeps MakeFixtureProbeDeps(GitFixture& fixture) {
  ConflictProbeDeps deps = MakeConflictProbeDeps(20000);
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);  // 夹具固定使用自己验证过的 git.exe。
    return AsQuery(fixture.Run(arguments, directory));
  };
  return deps;
}

ConflictStateFacts Scene(GitFixture& fixture) {
  const ConflictProbeRequest request = FixtureRequest(fixture);
  return CollectConflictState(request, MakeFixtureProbeDeps(fixture));
}

void InitWithCommit(GitFixture& fixture, std::wstring_view directoryName = L"repo") {
  fixture.InitRepository(directoryName);
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");
}

// 两侧各改同一行的同一处：merge 必定停在冲突上（夹具的提交身份与时间是固定的）。
void EnterMergeConflict(GitFixture& fixture) {
  InitWithCommit(fixture);
  fixture.WriteFile(L"shared.txt", "main 这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"main 侧改动");
  fixture.RunCheckedInRepo({L"switch", L"-c", L"other"});
  fixture.WriteFile(L"shared.txt", "other 这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"other 侧改动");
  fixture.RunCheckedInRepo({L"switch", L"main"});
  GC_REQUIRE(!fixture.RunInRepo({L"merge", L"other"}).Success(),
             "夹具没能制造出合并冲突（merge 竟然成功了）");
}

// 在临时根里放一个「会留下运行痕迹」的编辑器：被调用时把消息文件复制一份，再原样返回 0。
std::wstring RecordingEditor(GitFixture& fixture) {
  const std::wstring path = fixture.PathInRoot(L"editor-probe.bat");
  WriteText(path,
            "@echo off\r\n"
            "copy /y \"%1\" \"%1.seen\" >nul\r\n"
            "exit /b 0\r\n");
  return path;
}

// ---------------------------------------------------------------------------
// 只读那两条：`git init` + 只读查询 + 在临时根里造物，不建提交、不推送、不动索引。
// ---------------------------------------------------------------------------

GC_TEST(conflict_probe_fixture_accepts_the_query_shapes_on_a_fresh_repository) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");  // 尚无提交：HEAD 问不成，符号引用答 refs/heads/main

  const ConflictStateFacts state = Scene(fixture);
  GC_REQUIRE(state.probed, "没探到 Git 目录，形态核对无从谈起");
  GC_REQUIRE(state.kind == ConflictFlowKind::none, "全新仓库不该判出任何停着的流程");
  // 三条查询都真的被 Git 接受：未合并清单答「干净的空」，symbolic-ref 答得出分支，
  // rev-parse --verify --quiet 在无提交时以「没有」作答而不是用法错误。
  GC_CHECK_MESSAGE(state.unmergedReadOk, "未合并清单那条查询没被 Git 正常回答");
  GC_CHECK(state.unmergedPaths.empty());
  GC_CHECK_MESSAGE(state.branchQueried, "symbolic-ref 那条查询没被 Git 正常回答");
  GC_CHECK(state.onBranch);
  GC_CHECK(state.headObjectId.empty());
  // 没有痕迹 ⇒ 两个入口都不可用，且一条命令都不生成。
  GC_CHECK(!state.continueAvailable && !state.abortAvailable);
  GC_CHECK(gc::git::BuildConflictAbortPlan(state).arguments.empty());
  GC_CHECK(gc::git::BuildConflictContinuePlan(state).arguments.empty());
}

GC_TEST(conflict_probe_fixture_separates_unreadable_git_dir_from_no_markers) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"notes.txt", "note\n");  // 只在工作区写一个文件，不 add、不提交
  const std::filesystem::path fake = fixture.PathInRoot(L"not-a-git-dir");
  std::filesystem::create_directories(fake);

  // 一个不存在的路径：存在性问不成的那一条要说「问不到」，绝不说「没有流程停着」。
  const std::wstring missingGitDir = fixture.PathInRoot(L"this-one-does-not-exist");
  const ConflictMarkerFacts unknown = ProbeConflictMarkers(missingGitDir);
  GC_CHECK_MESSAGE(!unknown.probed, "目录不存在时不该报「探到了」");
  GC_CHECK(!unknown.probeFailure.empty());
  const ConflictStateFacts unknownState =
      gc::git::InterpretConflictState(unknown, GitQueryResult{}, GitQueryResult{}, GitQueryResult{});
  GC_CHECK(unknownState.kind == ConflictFlowKind::unreadable);
  GC_CHECK(gc::git::BuildConflictAbortPlan(unknownState).arguments.empty());

  // 一个真实存在、里面放了痕迹档案的普通目录：这一条只钉「存在性 + 限长读取 + 严格 UTF-8」
  // 这一段生产代码怎么读 Git 目录里的档案，不改动任何仓库。
  std::filesystem::create_directories(fake / L"rebase-merge");
  WriteText(fake / "MERGE_HEAD", "1111111111111111111111111111111111111111\n");
  WriteText(fake / "rebase-merge" / "head-name", "refs/heads/main\n");
  WriteText(fake / "rebase-merge" / "onto", "2222222222222222222222222222222222222222\n");
  WriteText(fake / "rebase-merge" / "msgnum", "4\n");
  WriteText(fake / "rebase-merge" / "end", "9\n");
  const ConflictMarkerFacts read = ProbeConflictMarkers(fake.wstring());
  GC_REQUIRE(read.mergeHead && read.rebaseMergeDir, "痕迹存在性应读得到");
  GC_CHECK(read.mergeHeadOid == L"1111111111111111111111111111111111111111");
  GC_CHECK(read.rebaseHeadName == L"refs/heads/main");
  GC_CHECK(read.rebaseMsgnum == L"4" && read.rebaseEnd == L"9");
  GC_CHECK(!read.indexLock && !read.sequencerDir);

  // 变基目录优先：与 CHERRY_PICK_HEAD 并存不算「两种流程并存在跑」（真变基就是这么留的）。
  ConflictMarkerFacts companion = read;
  companion.cherryPickHead = true;
  const ConflictStateFacts companionState =
      gc::git::InterpretConflictState(companion, GitQueryResult{}, GitQueryResult{},
                                      GitQueryResult{});
  GC_CHECK(companionState.kind == ConflictFlowKind::rebaseMergeBackend);

  // 一个超过上限的档案：读不回来并记下名字——半截内容绝不当成 Git 留下的事实。
  // 另起一个只放这份超限 MERGE_HEAD 的目录：上面那个目录里还有 rebase-merge\，
  // 变基目录优先会把判读结果带走，混在一起就核不到「内容缺读」这一句。
  const std::filesystem::path oversizedDir = fixture.PathInRoot(L"oversized-merge-head");
  std::filesystem::create_directories(oversizedDir);
  WriteText(oversizedDir / "MERGE_HEAD", std::string(5000, 'x'));
  const ConflictMarkerFacts capped = ProbeConflictMarkers(oversizedDir.wstring());
  GC_CHECK(capped.mergeHead);
  GC_CHECK(capped.mergeHeadOid.empty());
  const bool recorded =
      std::any_of(capped.contentFailures.begin(), capped.contentFailures.end(),
                  [](const std::wstring& item) {
                    return Contains(item, L"MERGE_HEAD") && Contains(item, L"上限");
                  });
  GC_CHECK_MESSAGE(recorded, "超限的档案应记成「读不回来」而不是「没有」");
  // 判读层随之把「正在合并哪一份提交」说成问不到，而不是编一个对象 ID。
  // 三条查询在这一段里没发（用「没答上来」的空结果占位），只核内容缺读的那句话。
  const ConflictStateFacts cappedState = gc::git::InterpretConflictState(
      capped, GitQueryResult{}, GitQueryResult{}, GitQueryResult{});
  GC_CHECK(cappedState.kind == ConflictFlowKind::merge);
  GC_CHECK_MESSAGE(Contains(cappedState.flowTarget, L"没能原样读回"), "内容读不回要说成问不到");
}

// ---------------------------------------------------------------------------
// 以下每条都会产生提交（含 merge --continue 建立合并提交），由维护者运行。
// ---------------------------------------------------------------------------

GC_TEST(conflict_state_fixture_reads_a_real_merge_conflict) {
  GitFixture fixture;
  PrepareFixture(fixture);
  EnterMergeConflict(fixture);

  const ConflictStateFacts state = Scene(fixture);
  GC_REQUIRE(state.kind == ConflictFlowKind::merge, "真冲突现场应判成合并停在中间");
  GC_CHECK(!state.indexLock);
  // MERGE_HEAD 里就是对方那次提交的完整对象 ID：判读展示的是它，不是猜出来的分支名。
  const std::wstring otherSha = fixture.RevParseVerified(L"other");
  GC_CHECK_MESSAGE(Contains(state.flowTarget, gc::git::ShortObjectId(otherSha)),
                   "合并目标应按 MERGE_HEAD 展示");
  const bool listed = std::find(state.unmergedPaths.begin(), state.unmergedPaths.end(),
                                std::wstring(L"shared.txt")) != state.unmergedPaths.end();
  GC_CHECK_MESSAGE(listed, "Git 记下的未合并条目要原样读回来");
  // 还有未合并条目 ⇒ 不谎称可以继续；中止仍在本程序接手范围内。
  GC_CHECK(!state.continueAvailable);
  GC_CHECK(state.abortAvailable);
  const std::vector<std::wstring> mergeAbort{L"-c", L"submodule.recurse=false", L"merge", L"--abort"};
  GC_CHECK(gc::git::BuildConflictAbortPlan(state).arguments == mergeAbort);

  // 外部把这一步收尾（这里用 --abort 代表「别处已经处理过」）之后，痕迹必须整体消失，
  // 复核说的是「已经没有痕迹」而不是「没读回来」，方案层也不再生成任何命令。
  const ConflictStateFacts before = state;
  fixture.RunCheckedInRepo({L"merge", L"--abort"});
  const ConflictStateFacts after = Scene(fixture);
  GC_CHECK(after.kind == ConflictFlowKind::none);
  GC_CHECK(after.unmergedReadOk);
  GC_CHECK(after.unmergedPaths.empty());
  const std::wstring change = gc::git::DescribeConflictStateChange(before, after);
  GC_CHECK_MESSAGE(Contains(change, L"已经没有痕迹"), "痕迹消失要说成消失");
  GC_CHECK(!Contains(change, L"没能问出"));
  GC_CHECK(gc::git::BuildConflictAbortPlan(after).arguments.empty());
  // 中止之后回到合并前：main 那条分支只到自己那次提交，另一侧的改动没有进来。
  GC_CHECK(fixture.CommitCount() == 2);
  GC_CHECK(fixture.HeadSha() != otherSha);
}

GC_TEST(conflict_continue_fixture_creates_a_merge_commit_and_clears_markers) {
  GitFixture fixture;
  PrepareFixture(fixture);
  EnterMergeConflict(fixture);
  const long beforeCount = fixture.CommitCount();
  const std::wstring headBefore = fixture.HeadSha();
  const std::wstring otherSha = fixture.RevParseVerified(L"other");

  // 由用户自己写解决内容（本程序从不代写），再暂存 —— 这一步之后才允许继续。
  fixture.WriteFile(L"shared.txt", "两边都要的那一行\n");
  fixture.StageAll();
  const ConflictStateFacts resolved = Scene(fixture);
  GC_REQUIRE(resolved.kind == ConflictFlowKind::merge, "解决后痕迹仍在，仍是合并");
  GC_CHECK_MESSAGE(resolved.unmergedPaths.empty(), "解决并暂存之后未合并清单应为空");
  GC_CHECK(resolved.continueAvailable);
  const ConflictOperationPlan plan = gc::git::BuildConflictContinuePlan(resolved);
  GC_CHECK(!plan.blocked);

  // 真把那条 --continue 交出去（界面上跑的是同一个形态）。core.editor 换成会留下 .seen 副本的
  // 批处理：顺便看清这一步到底开没开编辑器——这一点本程序不替 Git 决定，因此只记录不断言。
  const std::wstring editor = RecordingEditor(fixture);
  const std::wstring editorSeen = editor + L".seen";
  const GitRun cont =
      fixture.RunInRepo({L"-c", L"core.editor=" + editor, L"merge", L"--continue"});
  std::printf("[观察] merge --continue 退出码=%lu，编辑器被调用过=%s\n",
              static_cast<unsigned long>(cont.exitCode),
              std::filesystem::exists(editorSeen) ? "是" : "否");
  GC_REQUIRE(cont.exited && cont.exitCode == 0,
             "解决并暂存之后 Git 应当接受这次 --continue：" + gc::platform::Utf16ToUtf8(cont.err));

  const ConflictStateFacts after = Scene(fixture);
  GC_CHECK(after.kind == ConflictFlowKind::none);
  GC_CHECK(after.unmergedPaths.empty());
  GC_CHECK_MESSAGE(fixture.CommitCount() == beforeCount + 1, "--continue 应当建立一次提交");
  // 那是一次有两个父的合并提交，其中一份父提交就是刚才 MERGE_HEAD 里记着的那份。
  const std::wstring object = fixture.HeadCommitObject();
  GC_CHECK_MESSAGE(Contains(object, L"parent " + otherSha),
                   "合并提交对象里应有 MERGE_HEAD 那份提交的 parent 行");
  GC_CHECK_MESSAGE(Contains(object, L"parent " + headBefore),
                   "合并提交对象里应有合并前 HEAD 那份提交的 parent 行");
  GC_CHECK(fixture.HeadSha() != headBefore);
}

GC_TEST(conflict_continue_fixture_keeps_the_scene_when_a_hook_refuses) {
  GitFixture fixture;
  PrepareFixture(fixture);
  EnterMergeConflict(fixture);
  fixture.WriteFile(L"shared.txt", "两边都要的那一行\n");
  fixture.StageAll();
  GC_REQUIRE(Scene(fixture).continueAvailable, "解决并暂存后应允许继续");
  const long countBefore = fixture.CommitCount();

  // 在夹具自己的仓库里装一个必定失败的 pre-commit 钩子（不动任何用户配置）。
  const std::wstring gitDir =
      Trimmed(fixture.RunCheckedInRepo({L"rev-parse", L"--absolute-git-dir"}).out);
  const std::filesystem::path hook = std::filesystem::path(gitDir) / L"hooks" / L"pre-commit";
  std::filesystem::create_directories(hook.parent_path());
  WriteText(hook, "#!/bin/sh\necho 夹具钩子拒绝\nexit 1\n");
  std::error_code ec;
  std::filesystem::permissions(hook,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ec);

  // 先确认这个钩子真的会被 Git 执行——否则无法用它证明「失败时现场保留」。
  const GitRun probe =
      fixture.RunInRepo({L"commit", L"--allow-empty", L"-m", L"钩子应当拦住这条"});
  GC_REQUIRE(!probe.Success(),
             "夹具里的 pre-commit 钩子没被执行（Windows 上的可执行形态与本例假设不符）。"
             "这条用例要改成 .bat 形态的钩子或换 core.hooksPath 指向的目录再验，"
             "不能把「钩子没跑」当成「钩子失败时现场保留」的证据");

  const GitRun cont = fixture.RunInRepo({L"merge", L"--continue"});
  GC_REQUIRE(cont.exited && cont.exitCode != 0, "钩子非 0 时 --continue 应当失败");

  // 现场原样留着：痕迹还在、未合并条目仍是 0（已解决已暂存）、提交数没变。
  const ConflictStateFacts after = Scene(fixture);
  GC_CHECK(after.kind == ConflictFlowKind::merge);
  GC_CHECK(after.unmergedReadOk);
  GC_CHECK(after.unmergedPaths.empty());
  GC_CHECK_MESSAGE(after.continueAvailable, "钩子失败后仍应允许再试一次（现场没被破坏）");
  GC_CHECK(fixture.CommitCount() == countBefore);
  // 同一份现场与自己比对不该报出任何变化。
  GC_CHECK(gc::git::DescribeConflictStateChange(after, after).empty());
}

GC_TEST(conflict_state_fixture_reads_a_real_rebase_with_companion_markers) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"shared.txt", "本地这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"本地改动");
  fixture.RunCheckedInRepo({L"switch", L"-c", L"topic"});
  fixture.WriteFile(L"shared.txt", "上游这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"上游改动");
  fixture.RunCheckedInRepo({L"switch", L"main"});
  GC_REQUIRE(!fixture.RunInRepo({L"rebase", L"topic"}).Success(), "夹具没能制造出变基冲突");

  const ConflictStateFacts state = Scene(fixture);
  GC_REQUIRE(state.kind == ConflictFlowKind::rebaseMergeBackend,
             "变基目录优先：与它并存的 CHERRY_PICK_HEAD 不该被判成两种流程并存");
  GC_CHECK_MESSAGE(Contains(state.flowTarget, L"refs/heads/main"), "head-name 应读回那条分支");
  GC_CHECK_MESSAGE(Contains(state.flowTarget, L"第 "), "步数应读回来");
  GC_CHECK(state.abortAvailable);
  // 命令形态只在「已全部解决并暂存」的现场才拿得出来（还有未合并条目时方案是空的）。
  fixture.WriteFile(L"shared.txt", "两边都要的那一行\n");
  fixture.StageAll();
  const ConflictStateFacts resolved = Scene(fixture);
  GC_CHECK(resolved.continueAvailable);
  const std::vector<std::wstring> rebaseContinue{
      L"-c", L"submodule.recurse=false", L"rebase", L"--continue"};
  GC_CHECK(gc::git::BuildConflictContinuePlan(resolved).arguments == rebaseContinue);

  fixture.RunCheckedInRepo({L"rebase", L"--abort"});
  const ConflictStateFacts after = Scene(fixture);
  GC_CHECK(after.kind == ConflictFlowKind::none);
  GC_CHECK_MESSAGE(fixture.CommitCount() == 2, "中止变基应回到变基前的那次提交");
}

GC_TEST(conflict_state_fixture_separates_rebase_apply_from_git_am) {
  GitFixture applyFixture;
  PrepareFixture(applyFixture);
  InitWithCommit(applyFixture);
  applyFixture.WriteFile(L"shared.txt", "本地这一侧\n");
  applyFixture.StageAll();
  applyFixture.Commit(L"本地改动");
  applyFixture.RunCheckedInRepo({L"switch", L"-c", L"topic"});
  applyFixture.WriteFile(L"shared.txt", "上游这一侧\n");
  applyFixture.StageAll();
  applyFixture.Commit(L"上游改动");
  applyFixture.RunCheckedInRepo({L"switch", L"main"});
  GC_REQUIRE(!applyFixture.RunInRepo({L"rebase", L"--apply", L"topic"}).Success(),
             "夹具没能用 --apply 后端制造出冲突");

  const ConflictStateFacts applyScene = Scene(applyFixture);
  // 本用例钉住一个尚未实测的假设：apply 后端的变基在 rebase-apply\\ 里也写 head-name/onto。
  // 它失败说明本程序对 apply 后端的形态判据要按真实 Git 修正（修好之前那种现场会被判成
  // 「分不清是变基还是 git am」而拒绝，不会发出错误命令）。
  GC_REQUIRE(applyScene.kind == ConflictFlowKind::rebaseApplyBackend,
             "rebase-apply 里没读到 head-name/onto 的形态：判据需要按真实 Git 修正");
  GC_CHECK(applyScene.abortAvailable);
  const std::vector<std::wstring> rebaseAbort{
      L"-c", L"submodule.recurse=false", L"rebase", L"--abort"};
  GC_CHECK(gc::git::BuildConflictAbortPlan(applyScene).arguments == rebaseAbort);
  static_cast<void>(applyFixture.RunInRepo({L"rebase", L"--abort"}));

  // 另一侧：git am 撞车时同样落在 rebase-apply\\，但没有 head-name —— 必须承认分不清，
  // 两条命令都不给。
  GitFixture amFixture;
  PrepareFixture(amFixture);
  InitWithCommit(amFixture, L"src");
  amFixture.WriteFile(L"shared.txt", "补丁要改的这一行\n");
  amFixture.StageAll();
  amFixture.Commit(L"补丁来源改动");
  const GitRun patchRun = amFixture.RunInRepo({L"format-patch", L"-1", L"--stdout"});
  GC_REQUIRE(patchRun.Success(), "夹具没能导出补丁");
  amFixture.InitRepository(L"target");
  amFixture.WriteFile(L"shared.txt", "目标里这一行不一样\n");
  amFixture.StageAll();
  amFixture.Commit(L"目标改动");
  const std::filesystem::path patchFile = std::filesystem::path(amFixture.Root()) / L"patch.mbox";
  WriteText(patchFile, gc::platform::Utf16ToUtf8(patchRun.out));

  GC_REQUIRE(!amFixture.RunInRepo({L"am", L"-3", L"--", patchFile.wstring()}).Success(),
             "夹具没能用 git am 制造出停住的状态");
  const ConflictStateFacts amScene = Scene(amFixture);
  const bool oneOfTwo = amScene.kind == ConflictFlowKind::ambiguousRebase ||
                        amScene.kind == ConflictFlowKind::rebaseApplyBackend;
  GC_CHECK_MESSAGE(oneOfTwo, "am 停住时应落在「分不清」或「确认是 apply 后端」两者之一");
  if (amScene.kind == ConflictFlowKind::ambiguousRebase) {
    GC_CHECK(!amScene.continueAvailable && !amScene.abortAvailable);
    GC_CHECK(Contains(amScene.abortBlockedReason, L"git am"));
    GC_CHECK(gc::git::BuildConflictContinuePlan(amScene).arguments.empty());
  }
  static_cast<void>(amFixture.RunInRepo({L"am", L"--abort"}));
}

}  // namespace
