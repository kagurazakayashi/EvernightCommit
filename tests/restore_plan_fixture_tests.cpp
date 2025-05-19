// 「按记录恢复引用」的真实 Git 夹具。
//
// 本文件里一条是只读（git-readonly，可在会话内自跑）：钉住恢复预检用的
//   `rev-parse --no-replace-objects --verify --quiet <完整分支引用>^{commit}`
// 这一形态确实被真实 Git 接受（不是退出码 129 的用法错误——那种话恢复预检一问就废），
// 并且在没有提交的仓库里那条分支不存在时按「--quiet 系」的契约以退出码 1 + 空输出明确作答。
//
// 另一条会先建两次提交，属于 git-mutating：按 AGENTS 的提交/推送限制只由维护者本人运行，
// 本会话不代跑。它验证恢复用的原子 update-ref（带预期旧值）在真实仓库里确实把分支挪回，
// 以及「预期旧值对不上时 Git 原样拒绝、引用不动」这条原子保护。
#include "git/restore_plan.h"

#include <string>
#include <vector>

#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {
using gc::git::GitQueryResult;
using gc::test::GitFixture;
using gc::test::GitRun;

// 夹具跑一条命令拿回的是 GitRun；判读函数要的是 GitQueryResult。这里只做字段搬运，
// 不改任何判定，与撤回夹具里的 runner 转换同一份口径。
GitQueryResult AsQueryResult(const GitRun& run) {
  GitQueryResult result;
  result.started = run.started;
  result.timedOut = run.timedOut;
  result.exited = run.exited;
  result.outputComplete = run.outputComplete;
  result.exitCode = static_cast<int>(run.exitCode);
  result.utf16Output = run.out;
  result.utf16Error = run.err;
  return result;
}
}  // namespace

GC_TEST(restore_branch_probe_accepts_the_query_shape_on_a_fresh_repository) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  const std::wstring dir = fixture.RepoDir();

  // 合格引用名给出查询参数；不合法的（相对名、含空格）根本不产出。
  const std::vector<std::wstring> args =
      gc::git::BuildRestoreBranchValueArguments(dir, L"refs/heads/main");
  GC_REQUIRE(!args.empty(), "合格分支引用名应给出查询参数");
  GC_CHECK(gc::git::BuildRestoreBranchValueArguments(dir, L"main").empty());

  const auto run = fixture.Run(args, dir);
  GC_REQUIRE(run.exited && !run.timedOut, "预检查询没有正常结束");
  GC_CHECK_MESSAGE(run.exitCode != 129, "命令形态必须被真实 Git 接受（129=用法错误）");
  GC_CHECK_MESSAGE(run.exitCode == 1,
                   "没有提交的仓库里 refs/heads/main 不存在，--quiet 应以退出码 1 明确作答");
  GC_CHECK(run.out.empty());

  // 委托的 <ID>^{commit} 剥离问法沿用撤回那条已实测形态：非 ID 不产出参数。
  GC_CHECK(gc::git::BuildRestoreTargetObjectArguments(dir, L"not-a-hex-oid").empty());
}

// git-readonly：钉住 R2 新增的两条只读查询在真实 Git 上的形态与契约。
// 一条是「这个名字本身是不是符号引用」（git symbolic-ref --quiet <完整引用>），
// 一条是「这条分支归哪个工作树」（git worktree list --porcelain）。两者都不改动仓库。
GC_TEST(restore_ref_integrity_queries_match_real_git_contract) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  const std::wstring dir = fixture.RepoDir();

  // 参数形态：只有完整的本地分支引用才产出查询；相对名与别的命名空间根本不送进 Git。
  GC_REQUIRE_MESSAGE(!gc::git::BuildRefSymbolicProbeArguments(dir, L"refs/heads/main").empty(), "完整分支引用应给出符号引用探测参数");
  GC_CHECK(gc::git::BuildRefSymbolicProbeArguments(dir, L"main").empty());
  GC_CHECK(gc::git::BuildRefSymbolicProbeArguments(dir, L"refs/tags/v1").empty());
  // Git 允许但对 shell 敏感的字符在这一层放行（界面走 Unicode 参数数组，不是 shell 字符串）。
  GC_CHECK(!gc::git::BuildRefSymbolicProbeArguments(dir, L"refs/heads/demo&calc&rem").empty());

  // 真问一次：还没有这条分支 → 「--quiet 系」的明确「不是符号引用」= 退出码 1 + 空输出。
  const auto symref =
      fixture.Run(gc::git::BuildRefSymbolicProbeArguments(dir, L"refs/heads/main"), dir);
  GC_REQUIRE(symref.exited && !symref.timedOut, "symbolic-ref 查询没有正常结束");
  GC_CHECK_MESSAGE(symref.exitCode != 129, "命令形态必须被真实 Git 接受（129=用法错误）");
  GC_CHECK_MESSAGE(symref.exitCode == 1, "普通/不存在的引用应按退出码 1 + 空输出答「不是符号引用」");
  GC_CHECK(symref.out.empty());

  // worktree list --porcelain：真实输出必须能被本项目的解析器读成一条工作树记录。
  const auto list = fixture.Run(gc::git::BuildWorktreeListArguments(dir), dir);
  GC_REQUIRE(list.exited && !list.timedOut && list.exitCode == 0, "git worktree list 应成功回答");
  const GitQueryResult listResult = AsQueryResult(list);
  std::vector<gc::git::WorktreeRecord> records;
  std::wstring parseFailure;
  GC_CHECK_MESSAGE(gc::git::ParseWorktreeListPorcelain(listResult, &records, &parseFailure),
                   "真实 worktree 输出必须按 porcelain 形态解析成功: " +
                       std::string(parseFailure.begin(), parseFailure.end()));
  GC_REQUIRE(records.size() == 1, "刚 init 的仓库应恰好一条工作树记录");
  // 实测（本机 Git 2.56）：porcelain 里的路径用正斜杠（D:/…），与传进去的反斜杠是不同形的同一路径。
  // 判「是不是本工作树」必须按折叠后的路径比，不能按字面比。
  GC_CHECK_MESSAGE(gc::git::PathsEqualFolded(records[0].path, dir),
                   "记录的 worktree 路径应是夹具自己的工作区（实测形态是正斜杠）");

  // 判读层拿这两条真实回答必须得出「不是符号引用、没有被别的工作树占着」。
  const gc::git::RefIntegrityFacts integrity = gc::git::InterpretRefIntegrity(
      AsQueryResult(symref), listResult, L"refs/heads/main", dir);
  GC_CHECK(integrity.branchRefUsable);
  GC_CHECK(integrity.symrefRan && !integrity.symrefIsSymbolic);
  GC_CHECK(integrity.worktreesRan && integrity.worktreesReadable);
  GC_CHECK(integrity.worktreeHolder.empty());
  GC_CHECK(gc::git::DescribeRefIntegrityRefusal(integrity, L"refs/heads/main").empty());
}

// git-mutating：复现审查里那条"删符号引用时误伤被指向的分支"的证据，并证明本程序发出的
// 命令形态不会跟随。它需要真实建提交与真删引用，按约束只由维护者本人运行：
//   gc_tests.exe restore_symref_update_ref_does_not_follow
// 期望的三件事：
//   1) 不带 --no-deref 的删除形态（本程序修复前的形态）会把 victim 删掉，alias 反而留着；
//   2) 本程序现在发出的那条（带 --no-deref + 预期旧值）被 Git 以非 0 拒绝，victim 与 alias 都不动；
//   3) 恢复预检对同一个现场必须判出"这是符号引用"并拒绝，不靠事后补一句说明。
GC_TEST(restore_symref_update_ref_does_not_follow) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"one");
  const std::wstring commitOid = fixture.RevParseVerified(L"refs/heads/main");
  GC_REQUIRE(!commitOid.empty(), "需要一个已建好的提交对象");

  // victim = 落在那份提交上的普通分支；alias = 指向 victim 的符号引用。
  GC_REQUIRE(fixture.RunInRepo({L"update-ref", L"refs/heads/victim", commitOid}).exitCode == 0,
             "夹具建 victim 失败");
  GC_REQUIRE(
      fixture.RunInRepo({L"symbolic-ref", L"refs/heads/alias", L"refs/heads/victim"}).exitCode == 0,
      "夹具建符号引用失败");

  // 解引用的问法照样答出那份提交：这正是"只看 OID 分辨不出符号引用"的原因。
  const auto peeled =
      fixture.Run(gc::git::BuildRestoreBranchValueArguments(fixture.RepoDir(), L"refs/heads/alias"),
                  fixture.RepoDir());
  GC_REQUIRE_MESSAGE(peeled.exitCode == 0 && peeled.out == commitOid + L"\n",
                     "alias 经 rev-parse <ref>^{commit} 应解出 victim 的那份提交");

  // 预检的两条真实查询：symbolic-ref 必须以退出码 0 答出它指向谁，判读层据此拒绝。
  const auto symref =
      fixture.Run(gc::git::BuildRefSymbolicProbeArguments(fixture.RepoDir(), L"refs/heads/alias"),
                  fixture.RepoDir());
  GC_REQUIRE_MESSAGE(symref.exitCode == 0, "符号引用名上的 symbolic-ref 应以退出码 0 回答");
  GC_CHECK(symref.out == L"refs/heads/victim\n");
  const auto worktrees =
      fixture.Run(gc::git::BuildWorktreeListArguments(fixture.RepoDir()), fixture.RepoDir());
  const gc::git::RefIntegrityFacts integrity = gc::git::InterpretRefIntegrity(
      AsQueryResult(symref), AsQueryResult(worktrees), L"refs/heads/alias", fixture.RepoDir());
  GC_CHECK(integrity.symrefRan);
  GC_CHECK_MESSAGE(integrity.symrefIsSymbolic, "真实符号引用必须被判出来");
  GC_CHECK(!gc::git::DescribeRefIntegrityRefusal(integrity, L"refs/heads/alias").empty());

  const std::wstring victimBefore = fixture.RevParseVerified(L"refs/heads/victim");
  GC_CHECK(victimBefore == commitOid);

  // 1) 修复前的形态（没有 --no-deref）：删 alias 实际删掉的是 victim。
  const auto beforeShape = fixture.RunInRepo(
      {L"update-ref", L"-d", L"-m", L"EvernightCommit:fixture", L"refs/heads/alias", commitOid});
  GC_CHECK_MESSAGE(beforeShape.exitCode == 0, "不带 --no-deref 的删除会成功\\n但成功的是误伤");
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/victim").empty(),
                   "审查复现点：victim 被这条命令删掉了（本程序不得发出这种形态）");
  const auto aliasStillThere = fixture.RunInRepo({L"symbolic-ref", L"--quiet", L"refs/heads/alias"});
  GC_CHECK_MESSAGE(aliasStillThere.exitCode == 0, "alias 本身没被删，仍然指向已被删掉的 victim");

  // 2) 重做同样的现场，用本程序实际发出的形态：--no-deref + 预期旧值。
  GC_REQUIRE(fixture.RunInRepo({L"update-ref", L"refs/heads/victim", commitOid}).exitCode == 0,
             "夹具重建 victim 失败");
  GC_REQUIRE(
      fixture.RunInRepo({L"symbolic-ref", L"refs/heads/alias", L"refs/heads/victim"}).exitCode == 0,
      "夹具重建 alias 失败");
  const auto guarded = fixture.RunInRepo(
      {L"update-ref", L"--no-deref", L"-d", L"-m", L"EvernightCommit:fixture", L"refs/heads/alias",
       commitOid});
  GC_CHECK_MESSAGE(guarded.exitCode != 0,
                   "带 --no-deref 时 Git 读的是这个名字本身（ref: …），与预期旧值不符必须以非 0 拒绝");
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/victim") == commitOid,
                   "被指向的那条分支一个字节都没被动过");
  const auto aliasAfter = fixture.RunInRepo({L"symbolic-ref", L"--quiet", L"refs/heads/alias"});
  GC_CHECK(aliasAfter.exitCode == 0);
}

// git-mutating：建两次提交，验证恢复用的原子 update-ref 真的把分支挪回、且预期旧值不符时拒绝。
// 按约束只由维护者运行：gc_tests.exe restore_update_ref（或 --group git-mutating）。
GC_TEST(restore_update_ref_fixture_moves_branch_back_and_rejects_stale_old) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  fixture.Commit(L"first");
  const std::wstring firstSha = fixture.RevParseVerified(L"refs/heads/main");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  fixture.Commit(L"second");
  const std::wstring secondSha = fixture.RevParseVerified(L"refs/heads/main");
  GC_REQUIRE(!firstSha.empty() && !secondSha.empty() && firstSha != secondSha,
             "需要两个不同的提交才能演示引用回退");

  // 恢复形态与撤回同族：update-ref --create-reflog -m <reason> <完整引用> <新值=first> <预期旧值=second>
  const std::vector<std::wstring> restore{
      L"update-ref", L"--create-reflog", L"-m", L"EvernightCommit:test-restore", L"refs/heads/main",
      firstSha, secondSha};

  const auto applied = fixture.RunInRepo(restore);
  GC_REQUIRE(applied.exited && !applied.timedOut, "update-ref 没有正常结束");
  GC_CHECK_MESSAGE(applied.exitCode == 0, "带预期旧值的原子 update-ref 应把分支从 second 挪回 first");
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == firstSha);

  // 现在分支已在 first，再拿「预期旧值=second」发一次：对不上，Git 必须原样拒绝、引用不动。
  const auto stale = fixture.RunInRepo(restore);
  GC_CHECK_MESSAGE(stale.exitCode != 0, "预期旧值与实际不符时 Git 必须拒绝（这正是原子保护的落点）");
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == firstSha);
}
