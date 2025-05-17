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
using gc::test::GitFixture;
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
