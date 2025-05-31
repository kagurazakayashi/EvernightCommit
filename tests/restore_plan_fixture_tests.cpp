// 「按记录恢复引用」的真实 Git 夹具。
//
// 只读那几条（git-readonly，会话内可自跑）钉住的是「形态」与「越界」两类事实：
//   * 恢复预检用的 `rev-parse --no-replace-objects --verify --quiet <完整分支引用>^{commit}`
//     确实被真实 Git 接受（不是退出码 129 的用法错误——那种话恢复预检一问就废），并且在
//     那条分支不存在时按「--quiet 系」的契约以退出码 1 + 空输出明确作答；
//   * 引用形态与占用的三条只读查询（symbolic-ref / check-ref-format / worktree list）在
//     真实 Git 上的回答形态与判读层的契约一致，包括 linked worktree 那种占用现场；
//   * 本程序的形状闸门与 Git 的权威裁定（check-ref-format）口径一致：闸门放行的名字
//     Git 必须也认，Git 拒的名字闸门必须提前挡；Git 认但不属于 refs/heads/ 的名字仍要被挡；
//   * 恢复真正发出的那两串参数（普通移动 / 根删除）在真实仓库里的效果：只有那一个引用名变了，
//     别的引用、索引、工作区一字不差。提交对象由夹具的 plumbing 辅助造（write-tree +
//     hash-object），全程不执行 git commit / git push。
//
// 最后一条会先建两次真实提交，属于 git-commit（只提交、不推送）：按 2026-10-17 起的授权边界
// 可以由会话自跑，前提是提交只发生在用例自有的隔离临时仓库里。它验证恢复用的原子
// update-ref（带预期旧值）在真实仓库里确实把分支挪回，
// 以及「预期旧值对不上时 Git 原样拒绝、引用不动」这条原子保护。
#include "git/restore_plan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "platform/windows/restore_probe.h"  // 生产的平台层预检：问哪三条、怎么带回答回来
#include "platform/windows/utf_text.h"       // Utf16ToUtf8：仅用于失败诊断文本
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

// 「程序实际发出的那一串参数」原样交给真实 Git：数组前面只补上仓库落点，其余一段都不改——
// 夹具执行的就是界面会执行的那一份数据，而不是测试自己另拼的一条相似命令。
GitRun RunProgramArguments(GitFixture& fixture, const std::vector<std::wstring>& arguments) {
  std::vector<std::wstring> withDirectory{L"-C", fixture.RepoDir()};
  for (const std::wstring& argument : arguments) {
    withDirectory.push_back(argument);
  }
  return fixture.Run(withDirectory, fixture.RepoDir());
}

// 按原始字节读临时根内的文件（「工作区一字未动」就按字节比，不按界面摘要比）。
std::string ReadRootRelativeFile(GitFixture& fixture, std::wstring_view rootRelativePath) {
  const std::wstring absolute = fixture.PathInRoot(rootRelativePath);
  std::ifstream file(std::filesystem::path(absolute), std::ios::binary);
  if (!file.is_open()) {
    throw gc::test::PrerequisiteFailure("读取测试文件失败：" +
                                        gc::platform::Utf16ToUtf8(absolute));
  }
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 一条完整分支引用的现值（不存在时为空串）。
std::wstring RefOid(GitFixture& fixture, std::wstring_view ref) {
  return fixture.RevParseVerified(ref);
}

// 恢复线索：本文件的用例都直接喂给 DescribeRestoreCopy / BuildRestorePlan，参数一律由生产构造给出。
gc::git::RestoreClues MakeMoveClues(GitFixture& fixture, std::wstring_view branchRef,
                                    std::wstring_view expectedCurrentOid,
                                    std::wstring_view moveToOid) {
  gc::git::RestoreClues clues;
  clues.valid = true;
  clues.repositoryRoot = fixture.RepoDir();
  clues.branchRef = std::wstring(branchRef);
  clues.expectedCurrentOid = std::wstring(expectedCurrentOid);
  clues.moveToOid = std::wstring(moveToOid);
  clues.isRootDeletion = false;
  return clues;
}

// 用生产的三条只读查询问出「这个名字本身的形态与占用」，再交给生产判读层。
gc::git::RefIntegrityFacts AskIntegrity(GitFixture& fixture, std::wstring_view branchRef,
                                        std::wstring_view worktreeRoot) {
  const std::wstring dir = fixture.RepoDir();
  gc::git::RefIntegrityQueries requests;
  requests.branchRef = std::wstring(branchRef);
  requests.currentWorktreeRoot = std::wstring(worktreeRoot);
  requests.branchSymref = AsQueryResult(
      fixture.Run(gc::git::BuildRefSymbolicProbeArguments(dir, branchRef), dir));
  requests.refFormat =
      AsQueryResult(fixture.Run(gc::git::BuildRefFormatProbeArguments(dir, branchRef), dir));
  requests.worktrees = AsQueryResult(fixture.Run(gc::git::BuildWorktreeListArguments(dir), dir));
  return gc::git::InterpretRefIntegrity(requests);
}

// 把一段宽字符诊断文本收窄成可读的 UTF-8（只用于失败消息，不参与任何判定）。
std::string Narrow(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

// 临时根内的条目清单（相对路径、排序后）。跑程序自己构造的那一串前后各取一次：一模一样，
// 就是「没有额外的 shell 副作用」的直接证据——`&` 若真被 cmd 当命令分隔，多出来的文件或目录
// 一定在这份清单里现形。
std::vector<std::wstring> RootEntries(GitFixture& fixture) {
  std::vector<std::wstring> names;
  std::error_code error;
  const std::filesystem::path root(fixture.Root());
  for (std::filesystem::recursive_directory_iterator it(root,
                                                       std::filesystem::directory_options::skip_permission_denied,
                                                       error),
       end;
       !error && it != end; it.increment(error)) {
    const std::filesystem::path& entry = it->path();
    if (entry.filename().empty()) {
      continue;
    }
    const std::wstring relative = entry.lexically_relative(root).wstring();
    names.push_back(relative.empty() ? entry.filename().wstring() : relative);
  }
  std::sort(names.begin(), names.end());
  return names;
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
                   "真实 worktree 输出必须按 porcelain 形态解析成功: " + Narrow(parseFailure));
  GC_REQUIRE(records.size() == 1, "刚 init 的仓库应恰好一条工作树记录");
  // 实测（本机 Git 2.56）：porcelain 里的路径用正斜杠（D:/…），与传进去的反斜杠是不同形的同一路径。
  // 判「是不是本工作树」必须按折叠后的路径比，不能按字面比。
  GC_CHECK_MESSAGE(gc::git::PathsEqualFolded(records[0].path, dir),
                   "记录的 worktree 路径应是夹具自己的工作区（实测形态是正斜杠）");

  // 判读层拿这几条真实回答必须得出「Git 认可这个名字、不是符号引用、没有被别的工作树占着」。
  const auto format =
      fixture.Run(gc::git::BuildRefFormatProbeArguments(dir, L"refs/heads/main"), dir);
  GC_REQUIRE(format.exited && !format.timedOut, "check-ref-format 查询没有正常结束");
  GC_CHECK_MESSAGE(format.exitCode != 129, "命令形态必须被真实 Git 接受（129=用法错误）");
  gc::git::RefIntegrityQueries integrityRequests;
  integrityRequests.branchRef = L"refs/heads/main";
  integrityRequests.currentWorktreeRoot = dir;
  integrityRequests.branchSymref = AsQueryResult(symref);
  integrityRequests.refFormat = AsQueryResult(format);
  integrityRequests.worktrees = listResult;
  const gc::git::RefIntegrityFacts integrity =
      gc::git::InterpretRefIntegrity(integrityRequests);
  GC_CHECK(integrity.branchRefUsable);
  GC_CHECK(integrity.formatRan && integrity.formatAccepted);
  GC_CHECK(integrity.symrefRan && !integrity.symrefIsSymbolic);
  GC_CHECK(integrity.worktreesRan && integrity.worktreesReadable);
  GC_CHECK(integrity.worktreeHolder.empty());
  GC_CHECK(gc::git::DescribeRefIntegrityRefusal(integrity, L"refs/heads/main").empty());
}

// git-commit（只建提交、不推送）：复现审查里那条"删符号引用时误伤被指向的分支"的证据，并证明本程序发出的
// 命令形态不会跟随。它需要真实建提交与真删引用，属 git-commit（不推送）：
//   gc_tests.exe restore_symref_update_ref_does_not_follow
// 期望的三件事：
//   1) 不带 --no-deref 的删除形态（本程序修复前的形态）会把 victim 删掉，alias 反而留着；
//   2) 本程序现在发出的那条（带 --no-deref + 预期旧值）动的只可能是 alias 那个名字本身
//      （要么被 Git 拒掉，要么把这个符号引用删了），victim 永远一动不动——实测本机 Git 2.53：
//      删除形态的旧值仍按**解引用后**的值比对，所以这里不能承诺「一定非 0 拒绝」；
//   3) 恢复预检对同一个现场必须判出"这是符号引用"并拒绝，不靠事后补一句说明。
//      同一件事的自跑版本（提交对象走 plumbing、不跑 git commit）见
//      restore_refuses_symbolic_refs_and_never_moves_the_branch_behind_them。
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
  const auto aliasFormat = fixture.Run(
      gc::git::BuildRefFormatProbeArguments(fixture.RepoDir(), L"refs/heads/alias"),
      fixture.RepoDir());
  gc::git::RefIntegrityQueries integrityRequests;
  integrityRequests.branchRef = L"refs/heads/alias";
  integrityRequests.currentWorktreeRoot = fixture.RepoDir();
  integrityRequests.branchSymref = AsQueryResult(symref);
  integrityRequests.refFormat = AsQueryResult(aliasFormat);
  integrityRequests.worktrees = AsQueryResult(worktrees);
  const gc::git::RefIntegrityFacts integrity =
      gc::git::InterpretRefIntegrity(integrityRequests);
  GC_CHECK(integrity.formatRan && integrity.formatAccepted);
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
  // 本程序实际发出的形态（由恢复链路自己构造，避免夹具与生产各写一份而慢慢漂移）。
  const gc::git::RestoreClues removalClues =
      MakeMoveClues(fixture, L"refs/heads/alias", commitOid, L"");
  gc::git::RestoreClues guardedClues = removalClues;
  guardedClues.isRootDeletion = true;
  const gc::git::RestoreCopyText guardedCopy =
      gc::git::DescribeRestoreCopy(guardedClues, gc::git::ShellDialect::cmdInteractive);
  GC_REQUIRE_MESSAGE(guardedCopy.cluesUsable, "线索应合格：" + Narrow(guardedCopy.refusal));
  const auto guarded = RunProgramArguments(fixture, guardedCopy.arguments);
  // 实测语义（本机 Git 2.53，与只读用例 restore_refuses_symbolic_refs_… 同一结论）：
  // `--no-deref` 让 Git 按**这个名字本身**处理，绝不追到 alias 背后的 victim 去改它；
  // 而删除形态的旧值仍按解引用后的值比对，所以这里可能是「非 0 拒绝」，也可能是
  // 「把 alias 这个符号引用名删掉」——两种都在确认过的引用范围之内，victim 都必须一动不动。
  // 本程序自己在预检与确认后复核里就把符号引用拒绝了，这条命令根本发不出去。
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"refs/heads/victim") == commitOid,
                   "被指向的那条分支一个字节都没被动过（这才是不追随的真正含义）");
  const auto aliasAfter = fixture.RunInRepo({L"symbolic-ref", L"--quiet", L"refs/heads/alias"});
  if (aliasAfter.exitCode == 0) {
    GC_CHECK_MESSAGE(guarded.exitCode != 0,
                     "alias 还在，说明这条命令被 Git 拒掉了（旧值按解引用后的值比对，现场对不上）");
  } else {
    GC_CHECK_MESSAGE(guarded.exitCode == 0,
                     "alias 不在了，说明 Git 删的就是这个名字本身；无论哪种，victim 都必须原样保留");
  }
}

// git-commit（建两次真实提交、不推送）：验证恢复用的原子 update-ref 真的把分支挪回、
// 且预期旧值不符时拒绝。按 2026-10-17 起的授权边界可自动运行：
//   gc_tests --group git-commit   或   gc_tests restore_update_ref
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

  // 恢复形态与撤回同族，而这一串必须由恢复链路自己构造（不是测试另写一份相似的命令）：
  // update-ref --no-deref --create-reflog -m <reason> <完整引用> <新值=first> <预期旧值=second>。
  const gc::git::RestoreCopyText copy = gc::git::DescribeRestoreCopy(
      MakeMoveClues(fixture, L"refs/heads/main", secondSha, firstSha),
      gc::git::ShellDialect::cmdInteractive);
  GC_REQUIRE_MESSAGE(copy.cluesUsable, "线索应合格：" + Narrow(copy.refusal));
  GC_CHECK(std::find(copy.arguments.begin(), copy.arguments.end(), L"--no-deref") !=
           copy.arguments.end());
  const std::vector<std::wstring>& restore = copy.arguments;

  const auto applied = RunProgramArguments(fixture, restore);
  GC_REQUIRE(applied.exited && !applied.timedOut, "update-ref 没有正常结束");
  GC_CHECK_MESSAGE(applied.exitCode == 0, "带预期旧值的原子 update-ref 应把分支从 second 挪回 first");
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == firstSha);

  // 现在分支已在 first，再拿「预期旧值=second」发一次：对不上，Git 必须原样拒绝、引用不动。
  const auto stale = RunProgramArguments(fixture, restore);
  GC_CHECK_MESSAGE(stale.exitCode != 0, "预期旧值与实际不符时 Git 必须拒绝（这正是原子保护的落点）");
  GC_CHECK(fixture.RevParseVerified(L"refs/heads/main") == firstSha);
}

// git-readonly：形状闸门与 Git 的权威裁定必须同口径。
// 本程序放行的名字，真实 Git 必须也认（否则方案会在 Git 那一侧被半路拒）；Git 拒的名字，
// 闸门必须提前挡下（连参数都不产出，绝不把形态不明的名字送进 Git）；而 Git 认但不属于
// refs/heads/ 的名字仍要被挡——命名空间是产品契约，Git 本身不管，所以这道关不能被「Git 已裁过」顶替。
GC_TEST(undo_restore_refname_gate_matches_gits_authoritative_verdict) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  const std::wstring dir = fixture.RepoDir();

  const wchar_t* const gateAccepts[] = {
      L"refs/heads/main",        L"refs/heads/feature/nested", L"refs/heads/中文/分支",
      L"refs/heads/demo&calc&rem", L"refs/heads/100%25done!",  L"refs/heads/v1.0",
      L"refs/heads/-leading",
  };
  for (const wchar_t* name : gateAccepts) {
    GC_CHECK_MESSAGE(gc::git::IsSafeUndoTargetRef(name), "闸门应放行完整本地分支名");
    const std::vector<std::wstring> arguments = gc::git::BuildRefFormatProbeArguments(dir, name);
    GC_REQUIRE_MESSAGE(!arguments.empty(), "闸门放行的名字必须给出裁定查询的参数");
    const auto run = fixture.Run(arguments, dir);
    GC_REQUIRE(run.exited && !run.timedOut, "check-ref-format 没有正常结束");
    GC_CHECK(run.exitCode != 129);  // 129 = Git 不认这串参数形态，这道关就形同虚设
    GC_CHECK_MESSAGE(run.exitCode == 0, "闸门放行的名字，Git 必须也认（不一致就说明有一边错了）");
  }

  // Git 拒的这批，闸门也必须拒（这些是两边共同的「不合法」，不是本程序臆造的规则）。
  const wchar_t* const gitRejects[] = {
      L"refs/heads/a..b",  L"refs/heads/x.lock", L"refs/heads/.hidden", L"refs/heads/a b",
      L"refs/heads/a~b",   L"refs/heads/a^b",    L"refs/heads/a:b",     L"refs/heads/a?b",
      L"refs/heads/a*b",   L"refs/heads/a[b",    L"refs/heads//x",      L"refs/heads/a/",
      L"refs/heads/@{1}",  L"refs/heads/a/",
  };
  for (const wchar_t* name : gitRejects) {
    // 直接问 Git（绕过闸门），先钉住「Git 确实拒它」这个既定事实：退出码 1、无输出。
    const auto raw = fixture.Run({L"-C", dir, L"--no-optional-locks", L"check-ref-format",
                                  std::wstring(name)},
                                 dir);
    GC_REQUIRE_MESSAGE(raw.exited && raw.exitCode == 1, "这一组的既定事实是 Git 以退出码 1 拒绝");
    GC_REQUIRE_MESSAGE(raw.out.empty(), "check-ref-format 不合格时不写任何东西");
    GC_CHECK_MESSAGE(!gc::git::IsSafeUndoTargetRef(name), "Git 拒的名字闸门必须提前挡下");
    GC_CHECK(gc::git::BuildRefFormatProbeArguments(dir, name).empty());
  }

  // Git 认、而本程序按更严的规则拒掉的一批（**只允许比 Git 严，不允许比 Git 松**）：
  // 标签、远端跟踪引用、refs/stash 是命名空间契约（Git 的引用名规则不管「算不算分支」），
  // 纯数字段是本程序额外的形状规则（Git 认 refs/heads/123，本程序不接受，免得与
  // `@{1}` 那类可被二次解析的写法混在一起）。正因为 Git 不管这些，这道闸门不能被顶替。
  const wchar_t* const gateIsStricter[] = {L"refs/tags/v1", L"refs/remotes/origin/main",
                                           L"refs/stash", L"refs/heads/123"};
  for (const wchar_t* name : gateIsStricter) {
    const auto raw = fixture.Run({L"-C", dir, L"--no-optional-locks", L"check-ref-format",
                                  std::wstring(name)},
                                 dir);
    GC_REQUIRE_MESSAGE(raw.exited && raw.exitCode == 0,
                       "Git 认这些名字（引用名规则不划分命名空间）");
    GC_CHECK_MESSAGE(!gc::git::IsSafeUndoTargetRef(name), "本程序必须比 Git 更严：这批都要挡下");
    GC_CHECK(gc::git::BuildRefFormatProbeArguments(dir, name).empty());
  }
  // HEAD 与相对名（伪引用/歧义输入的形态）：闸门挡下，根本不送进 Git。
  for (const wchar_t* name : {L"HEAD", L"main"}) {
    GC_CHECK(!gc::git::IsSafeUndoTargetRef(name));
    GC_CHECK(gc::git::BuildRefFormatProbeArguments(dir, name).empty());
  }
}

// git-readonly：恢复真正发出的那两串参数（普通移动 / 根删除）在真实仓库里的效果，并证伪「越界」：
// 只有那一个引用名变了，别的引用、索引、工作区一字不差；旧值对不上时 Git 自己拒绝。
// 两枚提交对象由夹具的 plumbing 辅助造（write-tree + hash-object），全程不跑 git commit。
GC_TEST(restore_update_ref_forms_come_from_the_program_and_touch_nothing_else) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");

  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  const std::wstring commitOld = fixture.SeedCommitObject(L"既有提交 old");
  fixture.WriteFile(L"b.txt", "2\n");
  fixture.StageAll();
  const std::wstring commitNew = fixture.SeedCommitObject(L"既有提交 new", commitOld);
  GC_REQUIRE(!commitOld.empty() && !commitNew.empty() && commitOld != commitNew,
             "需要两枚互不相同的既有提交对象");

  // 现场：当前工作树检出 main 且它停在 new（= 那次撤回之前的位置）；三条邻居引用不该被碰到。
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/main", commitNew});
  fixture.RunCheckedInRepo({L"symbolic-ref", L"HEAD", L"refs/heads/main"});
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/neighbor", commitNew});
  fixture.RunCheckedInRepo({L"update-ref", L"refs/tags/v1", commitOld});
  fixture.RunCheckedInRepo({L"update-ref", L"refs/remotes/origin/main", commitOld});

  const std::wstring indexBefore = fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;
  const std::string bytesA = ReadRootRelativeFile(fixture, L"repo/a.txt");
  const std::string bytesB = ReadRootRelativeFile(fixture, L"repo/b.txt");
  GC_CHECK(!indexBefore.empty());

  const gc::git::RestoreClues clues =
      MakeMoveClues(fixture, L"refs/heads/main", commitNew, commitOld);
  const gc::git::RestoreCopyText move =
      gc::git::DescribeRestoreCopy(clues, gc::git::ShellDialect::cmdInteractive);
  GC_REQUIRE(move.cluesUsable, "线索合格才有得测");
  GC_CHECK(std::find(move.arguments.begin(), move.arguments.end(), L"--no-deref") !=
           move.arguments.end());

  const auto moved = RunProgramArguments(fixture, move.arguments);
  GC_REQUIRE(moved.exited && !moved.timedOut, "update-ref 没有正常结束");
  GC_CHECK_MESSAGE(moved.exitCode == 0, "程序自己构造的那一串必须被真实 Git 接受");
  GC_CHECK(RefOid(fixture, L"refs/heads/main") == commitOld);
  // 只动了那一个名字：三条邻居引用一字不差。
  GC_CHECK(RefOid(fixture, L"refs/heads/neighbor") == commitNew);
  GC_CHECK(RefOid(fixture, L"refs/tags/v1") == commitOld);
  GC_CHECK(RefOid(fixture, L"refs/remotes/origin/main") == commitOld);
  // HEAD 仍是「指向这个名字」的符号引用，没被解引用改掉。
  const auto headSym = fixture.RunInRepo({L"symbolic-ref", L"--quiet", L"HEAD"});
  GC_CHECK(headSym.exitCode == 0 && headSym.out == L"refs/heads/main\n");
  // 索引与工作区的字节都没动；变的是它们相对 HEAD 的关系——b.txt 于是重新表现为已暂存的新增，
  // 正是恢复确认文字承诺的那件事。
  GC_CHECK(fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out == indexBefore);
  GC_CHECK(ReadRootRelativeFile(fixture, L"repo/a.txt") == bytesA);
  GC_CHECK(ReadRootRelativeFile(fixture, L"repo/b.txt") == bytesB);
  const std::vector<std::wstring> statusLines = fixture.StatusPorcelain();
  bool bShowsAsStaged = false;
  for (const std::wstring& line : statusLines) {
    if (line == L"A  b.txt") {
      bShowsAsStaged = true;
    }
  }
  GC_CHECK_MESSAGE(bShowsAsStaged, "移动引用后 HEAD 的树少了 b.txt：索引里那份必须显示为已暂存的新增");

  // 旧值变化：同一条参数再发一次，此刻对不上，Git 必须原样拒绝。
  const auto stale = RunProgramArguments(fixture, move.arguments);
  GC_CHECK_MESSAGE(stale.exitCode != 0, "预期旧值与实际不符时 Git 必须拒绝（原子保护的落点）");
  GC_CHECK(RefOid(fixture, L"refs/heads/main") == commitOld);
  GC_CHECK(RefOid(fixture, L"refs/heads/neighbor") == commitNew);

  // 根删除形态：删的是没被任何工作树检出的那条分支，其余照旧。
  gc::git::RestoreClues deleteClues = MakeMoveClues(fixture, L"refs/heads/neighbor", commitNew, L"");
  deleteClues.isRootDeletion = true;
  const gc::git::RestoreCopyText removal =
      gc::git::DescribeRestoreCopy(deleteClues, gc::git::ShellDialect::cmdInteractive);
  GC_REQUIRE(removal.cluesUsable, "删除形态的线索合格");
  GC_CHECK(std::find(removal.arguments.begin(), removal.arguments.end(), L"-d") !=
           removal.arguments.end());
  const auto removed = RunProgramArguments(fixture, removal.arguments);
  GC_CHECK_MESSAGE(removed.exitCode == 0, "带预期旧值的删除形态应删掉那一个名字");
  GC_CHECK(RefOid(fixture, L"refs/heads/neighbor").empty());
  GC_CHECK(RefOid(fixture, L"refs/heads/main") == commitOld);
  GC_CHECK(RefOid(fixture, L"refs/tags/v1") == commitOld);
  GC_CHECK(fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out == indexBefore);
}

// git-readonly：符号引用（含符号链）必须被查出来并拒绝，且命令在这种现场上never 改到它背后的分支。
// 与 restore_symref_update_ref_does_not_follow 同一件事，只是提交对象走 plumbing，不跑 git commit。
GC_TEST(restore_refuses_symbolic_refs_and_never_moves_the_branch_behind_them) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "1\n");
  fixture.StageAll();
  const std::wstring commitC = fixture.SeedCommitObject(L"既有提交 C");
  const std::wstring commitD = fixture.SeedCommitObject(L"既有提交 D", commitC);
  GC_REQUIRE(commitC != commitD, "两枚提交对象必须不同");

  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/victim", commitC});
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/other", commitD});
  fixture.RunCheckedInRepo({L"symbolic-ref", L"refs/heads/alias", L"refs/heads/victim"});
  // 符号链：chain → alias → victim。实测 symbolic-ref 报出的是**紧邻**的那个目标（不做全链解析），
  // 因此「链」也一样被查成符号引用；而解引用的问法两条都答出 victim 的那份提交，光看 OID 分不出来。
  fixture.RunCheckedInRepo({L"symbolic-ref", L"refs/heads/chain", L"refs/heads/alias"});

  const std::wstring indexBefore = fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;

  for (const wchar_t* symbolicName : {L"refs/heads/alias", L"refs/heads/chain"}) {
    const auto peeled = fixture.Run(
        gc::git::BuildRestoreBranchValueArguments(fixture.RepoDir(), symbolicName),
        fixture.RepoDir());
    GC_REQUIRE(peeled.exitCode == 0, "解引用的问法应照样答出背后那条分支的提交");
    GC_CHECK(peeled.out == commitC + L"\n");

    const gc::git::RefIntegrityFacts integrity =
        AskIntegrity(fixture, symbolicName, fixture.RepoDir());
    GC_CHECK(integrity.formatRan && integrity.formatAccepted);  // 名字合格，但形态不合格
    GC_CHECK(integrity.symrefRan && integrity.symrefIsSymbolic);
    const std::wstring refusal =
        gc::git::DescribeRefIntegrityRefusal(integrity, symbolicName);
    GC_CHECK(!refusal.empty());
    GC_CHECK(refusal.find(L"符号引用") != std::wstring::npos);
  }

  // 生产的完整判读：这种现场不给任何命令（连复制用的那一行都不给）。
  const gc::git::RestoreClues clues =
      MakeMoveClues(fixture, L"refs/heads/alias", commitC, commitD);
  gc::git::RestorePreflightQueries queries;
  queries.branchRan = true;
  queries.branchValue = AsQueryResult(
      fixture.Run(gc::git::BuildRestoreBranchValueArguments(fixture.RepoDir(), clues.branchRef),
                  fixture.RepoDir()));
  queries.refKindRan = true;
  queries.currentWorktreeRoot = fixture.RepoDir();
  queries.branchSymref = AsQueryResult(fixture.Run(
      gc::git::BuildRefSymbolicProbeArguments(fixture.RepoDir(), clues.branchRef),
      fixture.RepoDir()));
  queries.refFormat = AsQueryResult(fixture.Run(
      gc::git::BuildRefFormatProbeArguments(fixture.RepoDir(), clues.branchRef), fixture.RepoDir()));
  queries.worktrees = AsQueryResult(
      fixture.Run(gc::git::BuildWorktreeListArguments(fixture.RepoDir()), fixture.RepoDir()));
  queries.workflowProbed = true;
  queries.workflowReadable = true;
  const gc::git::RestorePlan plan = gc::git::BuildRestorePlan(clues, queries);
  GC_CHECK(plan.blocked);
  GC_CHECK(plan.feasibility == gc::git::RestoreFeasibility::refNotMovable);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.copyableCommand.empty());

  // 移动形态在这种现场上的实际落点（本机 Git 2.53 实测）：预期旧值按**解引用后**的值比对，
  // 对得上就接受，而写入落在**那个名字本身**——alias 被改写成普通引用、停在 commitD，
  // victim 一动不动。这正是 --no-deref 的边界：它不承诺「这一笔一定被拒」，承诺的是
  // 「只可能动确认过的那个名字」。本程序在预检与确认后复核里就把符号引用拒绝了，
  // 这种写法只在「复核之后到 Git 自己读引用之前」那段锁不住的窗口里才可能出现。
  const auto guardedMove = RunProgramArguments(
      fixture, gc::git::DescribeRestoreCopy(clues, gc::git::ShellDialect::cmdInteractive).arguments);
  GC_REQUIRE_MESSAGE(guardedMove.exitCode == 0,
                     "旧值按解引用后的值比对，对得上时这一笔应被接受：" + Narrow(guardedMove.err));
  GC_CHECK(RefOid(fixture, L"refs/heads/victim") == commitC);
  GC_CHECK(RefOid(fixture, L"refs/heads/other") == commitD);
  GC_CHECK(RefOid(fixture, L"refs/heads/alias") == commitD);
  // alias 不再是符号引用：改的就是那个名字本身，没有去追随它。
  const auto aliasNowSym =
      fixture.RunInRepo({L"symbolic-ref", L"--quiet", L"refs/heads/alias"});
  GC_CHECK_MESSAGE(aliasNowSym.exitCode != 0, "被改写的只有 alias 这个名字（victim 仍停在 commitC）");

  // 删除形态：换一个干净的符号引用现场，分别测「旧值对不上」与「旧值对得上」两种答案。
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/victim2", commitC});
  fixture.RunCheckedInRepo({L"symbolic-ref", L"refs/heads/alias2", L"refs/heads/victim2"});
  gc::git::RestoreClues wrongOld = MakeMoveClues(fixture, L"refs/heads/alias2", commitD, L"");
  wrongOld.isRootDeletion = true;
  const auto refusedDelete = RunProgramArguments(
      fixture, gc::git::DescribeRestoreCopy(wrongOld, gc::git::ShellDialect::cmdInteractive).arguments);
  GC_CHECK_MESSAGE(refusedDelete.exitCode != 0,
                   "预期旧值与解引用后的值不符时，Git 必须原样拒绝");
  GC_CHECK(RefOid(fixture, L"refs/heads/victim2") == commitC);
  gc::git::RestoreClues rightOld = MakeMoveClues(fixture, L"refs/heads/alias2", commitC, L"");
  rightOld.isRootDeletion = true;
  const auto removedName = RunProgramArguments(
      fixture, gc::git::DescribeRestoreCopy(rightOld, gc::git::ShellDialect::cmdInteractive).arguments);
  GC_CHECK_MESSAGE(removedName.exitCode == 0,
                   "旧值对得上时这一笔按那个名字本身处理（删掉 alias2 这个符号引用）");
  GC_CHECK(RefOid(fixture, L"refs/heads/victim2") == commitC);
  GC_CHECK(RefOid(fixture, L"refs/heads/alias2").empty());

  // 指向不存在引用的符号链接：Git 问不出旧值，只能拒（实测 unable to resolve reference）。
  fixture.RunCheckedInRepo({L"symbolic-ref", L"refs/heads/dangling", L"refs/heads/nowhere"});
  const gc::git::RestoreClues dangling =
      MakeMoveClues(fixture, L"refs/heads/dangling", commitC, commitD);
  const auto danglingRun = RunProgramArguments(
      fixture, gc::git::DescribeRestoreCopy(dangling, gc::git::ShellDialect::cmdInteractive).arguments);
  GC_CHECK_MESSAGE(danglingRun.exitCode != 0, "符号引用背后没有东西时这一笔必须被拒");
  GC_CHECK(RefOid(fixture, L"refs/heads/victim") == commitC);

  // 到这儿为止，索引与工作区的字节没动过，别的引用也没被波及。
  GC_CHECK(fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out == indexBefore);
  GC_CHECK(RefOid(fixture, L"refs/heads/other") == commitD);
}

// git-readonly：linked worktree 的占用必须由真实的 `git worktree list --porcelain` 判出来。
// 走生产的平台层预检（CollectRestoreFacts）+ 生产判读，钉住「问哪三条、怎么判」整条链路。
GC_TEST(restore_refuses_branches_checked_out_in_a_linked_worktree_on_real_git) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  const std::wstring commitC = fixture.SeedCommitObject(L"既有提交 C");
  const std::wstring commitD = fixture.SeedCommitObject(L"既有提交 D", commitC);
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/main", commitC});
  fixture.RunCheckedInRepo({L"symbolic-ref", L"HEAD", L"refs/heads/main"});

  // 建一个 linked worktree 并让它检出 feature（`git worktree add -b` 不建提交、不推送）。
  const std::wstring linked = fixture.PathInRoot(L"linked-wt");
  const auto add = fixture.Run({L"worktree", L"add", L"-b", L"feature", linked}, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(add.Success(), "夹具建 linked worktree 失败");
  GC_CHECK(RefOid(fixture, L"refs/heads/main") == commitC);
  GC_CHECK(RefOid(fixture, L"refs/heads/feature") == commitC);

  // 真实 porcelain：两条记录，且解析得开（路径实测为正斜杠）。
  const auto list =
      fixture.Run(gc::git::BuildWorktreeListArguments(fixture.RepoDir()), fixture.RepoDir());
  std::vector<gc::git::WorktreeRecord> records;
  std::wstring parseFailure;
  GC_CHECK(gc::git::ParseWorktreeListPorcelain(AsQueryResult(list), &records, &parseFailure));
  GC_REQUIRE(records.size() == 2, "两个工作树就该读到两条记录");

  // 判读层：feature 被「别的」工作树检出 → 拒绝；从那个 worktree 自己问，就不算占用。
  const gc::git::RefIntegrityFacts fromMain = AskIntegrity(fixture, L"refs/heads/feature", fixture.RepoDir());
  GC_CHECK(fromMain.formatRan && fromMain.formatAccepted);
  GC_CHECK(fromMain.worktreesRan && fromMain.worktreesReadable);
  GC_CHECK(!fromMain.worktreeHolder.empty());
  GC_CHECK(gc::git::PathsEqualFolded(fromMain.worktreeHolder, linked));
  GC_CHECK(fromMain.currentWorktreeBranch == L"refs/heads/main");
  GC_CHECK(gc::git::DescribeRefIntegrityRefusal(fromMain, L"refs/heads/feature").find(L"另一个工作树") !=
           std::wstring::npos);

  gc::git::RefIntegrityQueries asLinked;
  asLinked.branchRef = L"refs/heads/feature";
  asLinked.currentWorktreeRoot = linked;  // 换到那个工作树自己的视角
  asLinked.branchSymref = AsQueryResult(fixture.Run(
      gc::git::BuildRefSymbolicProbeArguments(fixture.RepoDir(), L"refs/heads/feature"),
      fixture.RepoDir()));
  asLinked.refFormat = AsQueryResult(fixture.Run(
      gc::git::BuildRefFormatProbeArguments(fixture.RepoDir(), L"refs/heads/feature"),
      fixture.RepoDir()));
  asLinked.worktrees = AsQueryResult(list);
  const gc::git::RefIntegrityFacts fromLinked = gc::git::InterpretRefIntegrity(asLinked);
  GC_CHECK(fromLinked.worktreeHolder.empty());
  GC_CHECK(fromLinked.currentWorktreeBranch == L"refs/heads/feature");

  // 生产平台层预检 + 生产判读：feature 的恢复被拒，main 的恢复可行且把「相对 HEAD 的差异会变」写进确认。
  gc::platform::RestoreProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.absoluteGitDir = fixture.RepoDir() + L"\\.git";
  request.timeoutMilliseconds = 20000;

  request.branchRef = L"refs/heads/feature";
  request.moveToOid = commitD;
  request.isRootDeletion = false;
  const gc::git::RestorePreflightQueries featureQueries = gc::platform::CollectRestoreFacts(request);
  GC_CHECK(featureQueries.refKindRan);
  GC_CHECK(featureQueries.refFormat.exitCode == 0);  // 那条裁定确实问了 Git，也答上了
  const gc::git::RestorePlan featurePlan = gc::git::BuildRestorePlan(
      MakeMoveClues(fixture, L"refs/heads/feature", commitC, commitD), featureQueries);
  GC_CHECK_MESSAGE(featurePlan.blocked, "被别的工作树检出的分支必须拒绝恢复");
  GC_CHECK(featurePlan.feasibility == gc::git::RestoreFeasibility::refNotMovable);
  GC_CHECK(featurePlan.arguments.empty());

  request.branchRef = L"refs/heads/main";
  const gc::git::RestorePreflightQueries mainQueries = gc::platform::CollectRestoreFacts(request);
  const gc::git::RestorePlan mainPlan =
      gc::git::BuildRestorePlan(MakeMoveClues(fixture, L"refs/heads/main", commitC, commitD), mainQueries);
  GC_CHECK_MESSAGE(!mainPlan.blocked, "本工作树自己检出的分支可以恢复（不是跨工作树动分支）");
  GC_CHECK(mainPlan.previewText.find(L"这条分支正是当前工作树检出的那一条") != std::wstring::npos);
  GC_CHECK(mainPlan.notice.find(L"相对 HEAD 的差异会变") != std::wstring::npos);

  // 到这儿为止一条引用都没被移动过：建工作树与判读都不改现场。
  GC_CHECK(RefOid(fixture, L"refs/heads/main") == commitC);
  GC_CHECK(RefOid(fixture, L"refs/heads/feature") == commitC);
}

// git-readonly：分支名里带对 cmd 有语法意义的字符时（本机 check-ref-format 实测
// refs/heads/demo&calc&rem、100%25done! 合法），「能粘出去的一行」和「真正发出去的那一串」
// 是两份东西：前者按目标 shell 的规则裁定（& 能被双引号包住，% 与 ! 一律不给一行命令），
// 后者是原值的 argv 数组，由 CreateProcessW 直接交给 Git，全程不经过任何 shell。
// 全程只用 init / add / update-ref / rev-parse / plumbing 造的既有对象：没有 commit、没有 push。
GC_TEST(restore_program_arguments_for_shell_significant_names_reach_git_intact) {
  GitFixture fixture;
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), "夹具准备失败: " + reason);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"sentinel.txt", "哨兵：跑完之后它还得在原地\n");
  fixture.StageAll();
  const std::wstring commitOld = fixture.SeedCommitObject(L"既有提交 old");
  fixture.WriteFile(L"next.txt", "2\n");
  fixture.StageAll();
  const std::wstring commitNew = fixture.SeedCommitObject(L"既有提交 new", commitOld);
  GC_REQUIRE(!commitOld.empty() && !commitNew.empty() && commitOld != commitNew,
             "需要两枚互不相同的既有提交对象");
  // 一条邻居引用：用来证明「动的只有确认过的那一个名字」。
  fixture.RunCheckedInRepo({L"update-ref", L"refs/heads/neighbor", commitNew});

  for (const wchar_t* shortName : {L"demo&calc&rem", L"100%DONE", L"a!b"}) {
    const std::wstring branchRef = std::wstring(L"refs/heads/") + shortName;
    fixture.RunCheckedInRepo({L"update-ref", branchRef, commitNew});
    const std::vector<std::wstring> entriesBefore = RootEntries(fixture);

    const gc::git::RestoreClues clues = MakeMoveClues(fixture, branchRef, commitNew, commitOld);
    const gc::git::RestoreCopyText copy =
        gc::git::DescribeRestoreCopy(clues, gc::git::ShellDialect::cmdInteractive);
    GC_REQUIRE_MESSAGE(copy.cluesUsable, "闸门应放行这类 Git 合法名：" + Narrow(copy.refusal));

    // ① 交给 Git 的数组：原值，一个字符都没被替换、没被删。
    GC_CHECK(std::find(copy.arguments.begin(), copy.arguments.end(), branchRef) !=
             copy.arguments.end());
    // ② 给人粘的那一行按 cmd 的规则单独裁定，和数组不是同一份东西。
    const bool cmdCanQuoteIt = std::wstring(shortName).find_first_of(L"%!") == std::wstring::npos;
    if (cmdCanQuoteIt) {
      GC_REQUIRE(!copy.copyableCommand.empty(), "双引号能包住的段应给一行命令");
      GC_CHECK(copy.copyableCommand.find(L'"' + branchRef + L'"') != std::wstring::npos);
    } else {
      GC_CHECK(copy.copyableCommand.empty());
      GC_CHECK(!copy.copyRefusal.empty());
      GC_CHECK(!copy.structuredFacts.empty());
    }

    // ③ 真正发出去的是那份数组：Git 必须原样收下并把这一个名字挪回去。
    const auto moved = RunProgramArguments(fixture, copy.arguments);
    GC_REQUIRE(moved.exited && !moved.timedOut, "update-ref 没有正常结束");
    GC_CHECK_MESSAGE(moved.exitCode == 0,
                     "程序自己构造的那一串必须被真实 Git 接受：" + Narrow(moved.err));
    GC_CHECK(RefOid(fixture, branchRef) == commitOld);
    GC_CHECK(RefOid(fixture, L"refs/heads/neighbor") == commitNew);

    // ④ 没有额外的 shell 副作用：临时根里的条目一字不差，哨兵文件也没被改动。
    GC_CHECK(RootEntries(fixture) == entriesBefore);
    GC_CHECK(ReadRootRelativeFile(fixture, L"repo/sentinel.txt") == "哨兵：跑完之后它还得在原地\n");
  }
}
