// 子模块导航的真实 Git 集成测试：全部在夹具自己创建并认领所有权的临时目录里，用真实 Git
// 驱动生产逻辑（git::BuildSubmodule*Arguments 的形态、platform::CollectSubmoduleEntryFacts、
// platform::CollectSubmodulePointerFacts、git::ParseGitlinkIndexEntry 与判读函数），
// 再把结论与仓库里的实际状态对着看。子模块来源只能是临时根内的本地仓库
// （夹具环境写死 GIT_ALLOW_PROTOCOL=file，任何网络形态的 URL 在发起连接前就被 Git 拒绝）。
//
// 分组口径（运行器把夹具文件里未列入只读名单的用例归入 git-mutating）：
//   * 前两条只跑 `git init`、只读查询与在临时目录里造物（写文件、建目录），不建提交、不推送；
//   * 后四条需要提交图或改动索引（子模块登记提交、子模块内部提交、update-index 造 gitlink），
//     一律由维护者运行。
//
// 覆盖的场景：
//   * 三条查询的参数形态真的被 Git 接受：`ls-files -s -z -- :(literal)<路径>` 用法正确，
//     `rev-parse --verify --quiet HEAD:<路径>` 在没有提交时以退出码 1 + 空输出答「没有」，
//     都不是 129 用法错误（这类形态一旦不被接受，整条判读在真机上永远问不出东西）；
//   * 路径不是子模块记录时如实拒绝，并给出「索引里没有这条记录」这句依据；
//   * 真子模块：进入裁决放行、内部状态三态由生产的 status 读回来再进同一份判读；
//   * gitlink 在索引里而目录不见踪影 / 目录只是个普通目录（Git 往上找到父仓库本身）这两种
//     「未初始化 / 被移走」的场合各自落到哪句措辞；clone 出来没初始化的仓库为什么不能进；
//   * 三份位置在「刚登记 / 已登记并提交 / 子模块内部又提交」三种场合各自落到哪个结论与措辞；
//   * 导航本身不动任何东西：父索引逐字节不变、两个仓库的提交数不变、都没配远端、
//     夹具那份「用户层」配置档案依旧为空。
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "git/pull_plan.h"
#include "git/repository.h"
#include "git/submodule_navigation.h"
#include "git/workspace_model.h"
#include "platform/windows/submodule_probe.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"
#include "platform/windows/workspace_status.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::GitQueryResult;
using gc::git::RepoKind;
using gc::git::SubmoduleEntryDecision;
using gc::git::SubmoduleEntryFacts;
using gc::git::SubmoduleEntryPlan;
using gc::git::SubmodulePathPresence;
using gc::git::SubmodulePointerFacts;
using gc::git::SubmodulePointerState;
using gc::git::WorkspaceModel;
using gc::platform::MakeSubmoduleProbeDeps;
using gc::platform::SubmoduleEntryRequest;
using gc::platform::SubmoduleProbeDeps;
using gc::platform::SubmodulePointerRequest;
using gc::test::GitFixture;
using gc::test::GitRun;

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

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

// 夹具的隔离执行器 + 生产的路径存在性检查：与界面同一份编排，只是子进程换到夹具环境里跑。
SubmoduleProbeDeps MakeFixtureProbeDeps(GitFixture& fixture) {
  SubmoduleProbeDeps deps = MakeSubmoduleProbeDeps(20000);
  deps.runner = [&fixture](const std::wstring& exePath, const std::wstring& directory,
                           const std::vector<std::wstring>& arguments) {
    static_cast<void>(exePath);  // 夹具固定使用自己验证过的 git.exe。
    return AsQuery(fixture.Run(arguments, directory));
  };
  deps.detect = fixture.MakeDetectDeps();
  return deps;
}

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

void InitWithCommit(GitFixture& fixture, std::wstring_view directoryName) {
  fixture.InitRepository(directoryName);
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");
}

// 真子模块：child-origin 作来源，parent 里 `submodule add` 登记为 child（是否再提交由用例决定）。
std::wstring InitParentWithSubmodule(GitFixture& fixture, bool commitTheRegistration) {
  InitWithCommit(fixture, L"child-origin");
  fixture.WriteFile(L"inside.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"子模块内容");
  const std::wstring childSource = fixture.RepoDir();

  InitWithCommit(fixture, L"parent");
  fixture.RunCheckedInRepo(
      {L"-c", L"protocol.file.allow=always", L"submodule", L"add", L"--", childSource, L"child"});
  if (commitTheRegistration) {
    fixture.RunCheckedInRepo({L"commit", L"-qm", L"登记子模块"});
  }
  fixture.SetActiveRepository(L"parent");
  return fixture.RepoDir();
}

SubmodulePointerFacts PointerFacts(GitFixture& fixture, const std::wstring& parentRoot,
                                   std::wstring_view path = L"child") {
  SubmodulePointerRequest request;
  request.exePath = fixture.GitExe();
  request.parentRoot = parentRoot;
  request.submoduleRelativePath = std::wstring(path);
  request.submoduleDirectory = gc::git::JoinWorktreeFilePath(parentRoot, path);
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectSubmodulePointerFacts(request, MakeFixtureProbeDeps(fixture));
}

SubmoduleEntryFacts EntryFacts(GitFixture& fixture, const std::wstring& parentRoot,
                               std::wstring_view path) {
  SubmoduleEntryRequest request;
  request.exePath = fixture.GitExe();
  request.parentRoot = parentRoot;
  request.submoduleRelativePath = std::wstring(path);
  request.itemIsSubmodule = true;
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectSubmoduleEntryFacts(request, MakeFixtureProbeDeps(fixture));
}

std::wstring IndexListing(GitFixture& fixture) {
  return fixture.RunCheckedInRepo({L"ls-files", L"-s"}).out;
}

void ExpectNoGlobalConfig(GitFixture& fixture) {
  GC_CHECK_MESSAGE(Trimmed(fixture.RunCheckedInRepo({L"config", L"--global", L"--list"}).out).empty(),
                   "子模块导航把使用者的全局配置文件改了");
}

std::wstring PointerStateText(const SubmodulePointerFacts& facts) {
  return std::wstring(gc::git::SubmodulePointerStateLabel(facts.state)) + L" / " + facts.detail;
}

}  // namespace

// ---- 只读：查询形态与「不是子模块记录」的路径 ----

GC_TEST(submodule_navigation_queries_fixture_are_accepted_by_real_git) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");  // 故意不建任何提交：HEAD 那一侧的答案也要钉住
  const std::wstring root = fixture.RepoDir();
  const SubmoduleProbeDeps deps = MakeFixtureProbeDeps(fixture);

  const GitQueryResult listing =
      deps.runner(fixture.GitExe(), root,
                  gc::git::BuildSubmoduleIndexPointerArguments(root, L"child"));
  // 形态被 Git 接受：退出码 0 + 空输出（这个路径在索引里不存在），不是 129 用法错误。
  GC_CHECK_MESSAGE(listing.started && listing.exited && listing.exitCode == 0,
                   ToUtf8(L"exit=" + std::to_wstring(listing.exitCode) + L" " +
                          listing.utf16Error));
  const gc::git::GitlinkIndexEntry absent =
      gc::git::ParseGitlinkIndexEntry(listing, L"child");
  GC_CHECK_MESSAGE(absent.readOk, ToUtf8(absent.readFailure));
  GC_CHECK(!absent.recordFound && !absent.isGitlink);

  const GitQueryResult headPointer =
      deps.runner(fixture.GitExe(), root,
                  gc::git::BuildSubmoduleHeadPointerArguments(root, L"child"));
  // 还没有任何提交：`HEAD:child` 以退出码 1 + 空输出答「没有」，那是明确答案而不是错误。
  GC_CHECK_MESSAGE(headPointer.started && headPointer.exited && headPointer.exitCode == 1,
                   ToUtf8(L"exit=" + std::to_wstring(headPointer.exitCode) + L" " +
                          headPointer.utf16Error));
  GC_CHECK(Trimmed(headPointer.utf16Output).empty());

  // 子模块自己 HEAD 那条与 pull 预检完全同一个形态（一份实现），这里确认它也被真实 Git 接受。
  GC_CHECK(gc::git::BuildSubmoduleOwnHeadArguments(root) ==
           gc::git::BuildPullHeadObjectArguments(root));
  const GitQueryResult ownHead =
      deps.runner(fixture.GitExe(), root, gc::git::BuildSubmoduleOwnHeadArguments(root));
  GC_CHECK(ownHead.started && ownHead.exited && ownHead.exitCode == 1);
}

GC_TEST(submodule_entry_probe_fixture_refuses_paths_that_are_not_gitlinks) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  const std::wstring root = fixture.RepoDir();

  // 一：路径根本不存在。
  const SubmoduleEntryFacts missing = EntryFacts(fixture, root, L"never-created");
  GC_CHECK(missing.presence == SubmodulePathPresence::missing);
  GC_CHECK(missing.index.readOk && !missing.index.recordFound);
  const SubmoduleEntryPlan missingPlan = gc::git::BuildSubmoduleEntryPlan(missing);
  GC_CHECK(missingPlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(Contains(missingPlan.reason, L"索引里已经没有"));

  // 二：同一个名字上是个文件。父索引里没有那条 gitlink，判读先按「记录不存在」拒绝；
  // 「不是目录」那一句留给真是 gitlink 的场合（下一条用例与纯逻辑各自钉着）。
  fixture.WriteFile(L"afile.txt", "x\n");
  const SubmoduleEntryFacts file = EntryFacts(fixture, root, L"afile.txt");
  GC_CHECK(file.presence == SubmodulePathPresence::existsNotDirectory);
  GC_CHECK(gc::git::BuildSubmoduleEntryPlan(file).decision == SubmoduleEntryDecision::refuse);

  // 三：只是一个普通目录（里面没有 .git）。Git 沿它往上找到的工作区根就是这个父仓库本身，
  // 而且它不认这块目录是子模块——这正是「子模块还没初始化」的形态。此刻的先决拒绝
  // 仍然是「索引里没有那条 gitlink」（这个夹具仓库压根没有子模块记录）。
  fixture.WriteFile(L"plain/inside.txt", "x\n");
  const SubmoduleEntryFacts plain = EntryFacts(fixture, root, L"plain");
  GC_CHECK(plain.presence == SubmodulePathPresence::existsDirectory);
  GC_CHECK(plain.childProbed);
  GC_CHECK(!plain.child.submodule && plain.child.kind != RepoKind::submodule);
  GC_CHECK(gc::git::PathsEqualFolded(plain.child.root, root));
  GC_CHECK(gc::git::BuildSubmoduleEntryPlan(plain).decision == SubmoduleEntryDecision::refuse);
  ExpectNoGlobalConfig(fixture);
}

// ---- 以下用例需要提交图或改索引，由维护者运行 ----

GC_TEST(submodule_entry_probe_fixture_allows_real_submodule_and_flags_its_state) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring root = InitParentWithSubmodule(fixture, true);

  const SubmoduleEntryFacts facts = EntryFacts(fixture, root, L"child");
  GC_CHECK(facts.presence == SubmodulePathPresence::existsDirectory);
  GC_CHECK(facts.index.isGitlink);
  GC_CHECK(facts.childProbed);
  GC_CHECK(facts.child.kind == RepoKind::submodule && facts.child.submodule);
  GC_CHECK(gc::git::PathsEqualFolded(facts.child.superprojectTree, root));
  const SubmoduleEntryPlan plan = gc::git::BuildSubmoduleEntryPlan(facts);
  GC_CHECK_MESSAGE(plan.decision == SubmoduleEntryDecision::enter, ToUtf8(plan.reason));

  // 内部状态由生产的 status 读回来，再作为事实送进同一份判读：三种情况必须分开说。
  fixture.WriteFile(L"child/inside.txt", "dirty\n");
  fixture.WriteFile(L"child/scratch.tmp", "new\n");
  gc::platform::WorkspaceStatusRequest statusRequest;
  statusRequest.exePath = fixture.GitExe();
  statusRequest.repositoryDirectory = root;
  statusRequest.timeoutMilliseconds = 20000;
  const gc::git::WorkspaceSnapshot snapshot =
      gc::platform::LoadWorkspaceStatus(statusRequest, fixture.MakeStatusDeps());
  GC_REQUIRE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, "生产 status 读取没成功");
  const ChangeItem* item = nullptr;
  for (const ChangeItem& candidate : snapshot.model.unstaged) {
    if (candidate.path == L"child") {
      item = &candidate;
    }
  }
  GC_REQUIRE(item != nullptr, "父仓库的未暂存侧没把 child 列出来");
  GC_CHECK(item->kind == ChangeKind::submodule);
  GC_CHECK(item->submodule.trackedChanges && item->submodule.untrackedChanges);
  GC_CHECK(!item->submodule.commitChanged);  // 只改了内部文件，指针没动

  SubmoduleEntryFacts dirty = facts;
  dirty.flags = item->submodule;
  const SubmoduleEntryPlan dirtyPlan = gc::git::BuildSubmoduleEntryPlan(dirty);
  GC_CHECK(dirtyPlan.decision == SubmoduleEntryDecision::enter);
  GC_CHECK(Contains(dirtyPlan.disclosure, L"已跟踪文件的改动"));
  GC_CHECK(Contains(dirtyPlan.disclosure, L"未跟踪文件"));
  GC_CHECK(Contains(dirtyPlan.disclosure, L"此刻与子模块 HEAD 一致"));
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(submodule_entry_probe_fixture_separates_moved_from_uninitialized) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring root = InitParentWithSubmodule(fixture, true);
  const std::wstring childDirectory = fixture.PathInRoot(L"parent/child");

  // 一：只把**工作区里**的那个目录删掉（文件系统操作，不碰索引）：gitlink 还在父索引里，
  // 目录却不见了。这种场合说的是「不见了」，不是「这不是仓库」。
  std::error_code removal;
  std::filesystem::remove_all(std::filesystem::path(childDirectory), removal);
  GC_REQUIRE(!removal, "夹具删除 child 目录失败：" + removal.message());
  GC_REQUIRE(!gc::platform::IsExistingDirectory(childDirectory), "child 目录应当已被删掉");
  const SubmoduleEntryFacts gone = EntryFacts(fixture, root, L"child");
  GC_CHECK(gone.index.isGitlink);
  GC_CHECK(gone.presence == SubmodulePathPresence::missing);
  const SubmoduleEntryPlan gonePlan = gc::git::BuildSubmoduleEntryPlan(gone);
  GC_CHECK(gonePlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(Contains(gonePlan.reason, L"那个目录已经不在了"));
  GC_CHECK(Contains(gonePlan.reason, L"不会替你 clone"));
  // 父索引确实没被这一串探测动过（那条 gitlink 原样在，也没被顺手暂存什么）。
  GC_CHECK(Contains(IndexListing(fixture), L"160000"));

  // 二：目录在，但里面没有 .git（Git 沿它往上找到的是父仓库本身）→ 未初始化。
  fixture.WriteFile(L"child/inside.txt", "不是仓库内容\n");
  const SubmoduleEntryFacts empty = EntryFacts(fixture, root, L"child");
  GC_CHECK(empty.presence == SubmodulePathPresence::existsDirectory);
  GC_CHECK(empty.childProbed);
  GC_CHECK(empty.child.kind != RepoKind::submodule);
  const SubmoduleEntryPlan emptyPlan = gc::git::BuildSubmoduleEntryPlan(empty);
  GC_CHECK(emptyPlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(Contains(emptyPlan.reason, L"还没有初始化"));
  GC_CHECK(Contains(emptyPlan.reason, L"git submodule update --init"));
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(submodule_entry_probe_fixture_refuses_uninitialized_clone) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring parentUrl = InitParentWithSubmodule(fixture, true);

  // clone 默认不初始化子模块：gitlink 在索引里，可那个目录要么是空的、要么根本不存在。
  // 不管它是哪一种，都不该被当成「可以进入的子模块工作区」——这一条比措辞更硬。
  fixture.CloneRepository(parentUrl, L"copy");
  const std::wstring copyRoot = fixture.RepoDir();
  const SubmoduleEntryFacts facts = EntryFacts(fixture, copyRoot, L"child");
  GC_CHECK(facts.index.isGitlink);
  const SubmoduleEntryPlan plan = gc::git::BuildSubmoduleEntryPlan(facts);
  GC_CHECK_MESSAGE(plan.decision == SubmoduleEntryDecision::refuse, ToUtf8(plan.reason));
  if (facts.presence == SubmodulePathPresence::existsDirectory) {
    GC_CHECK(facts.child.kind != RepoKind::submodule);  // 没有仓库身份，或往上找到了 clone 本身
    GC_CHECK(Contains(plan.reason, L"还没有初始化"));
  } else {
    GC_CHECK(Contains(plan.reason, L"目录已经不在了"));
  }
  GC_CHECK(Contains(plan.reason, L"git submodule update --init"));
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(submodule_pointer_states_fixture_from_a_real_submodule) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring root = InitParentWithSubmodule(fixture, false);

  fixture.SetActiveRepository(L"parent/child");
  const std::wstring submoduleHead = fixture.HeadSha();
  fixture.SetActiveRepository(L"parent");

  // 1) 刚 `submodule add`：索引里是 gitlink、父提交里还没有这条记录，位置与子模块一致
  //    ⇒ 「指针已暂存，等父仓库创建提交」。
  const SubmodulePointerFacts staged = PointerFacts(fixture, root);
  GC_CHECK_MESSAGE(staged.index.isGitlink, ToUtf8(staged.index.readFailure + staged.index.mode));
  GC_CHECK_MESSAGE(staged.state == SubmodulePointerState::pointerStaged,
                   ToUtf8(PointerStateText(staged)));
  GC_CHECK(staged.submoduleHeadId == submoduleHead);
  GC_CHECK(staged.headObjectId.empty());
  GC_CHECK(Contains(gc::git::ComposeSubmodulePointerReport(staged, L"child"),
                   L"下一步是在父仓库"));

  // 2) 提交登记之后：三份位置一致 ⇒ 「显示干净」，并连带那句「干净不等于已发布」。
  fixture.RunCheckedInRepo({L"commit", L"-qm", L"登记子模块"});
  const SubmodulePointerFacts clean = PointerFacts(fixture, root);
  GC_CHECK_MESSAGE(clean.state == SubmodulePointerState::consistentClean,
                   ToUtf8(PointerStateText(clean)));
  GC_CHECK(Contains(gc::git::ComposeSubmodulePointerReport(clean, L"child"),
                   L"子模块那次提交已经推到任何远端"));

  // 3) 子模块内部再提交一次：指针与父索引不再一致 ⇒ 「该由用户去暂存这条指针」。
  fixture.SetActiveRepository(L"parent/child");
  fixture.WriteFile(L"inside.txt", "second\n");
  fixture.StageAll();
  fixture.Commit(L"子模块内部又提交一次");
  const std::wstring movedHead = fixture.HeadSha();
  fixture.SetActiveRepository(L"parent");
  const SubmodulePointerFacts unstaged = PointerFacts(fixture, root);
  GC_CHECK_MESSAGE(unstaged.state == SubmodulePointerState::pointerUnstaged,
                   ToUtf8(PointerStateText(unstaged)));
  GC_CHECK(unstaged.submoduleHeadId == movedHead);
  GC_CHECK(unstaged.index.objectId != movedHead);
  const std::wstring report = gc::git::ComposeSubmodulePointerReport(unstaged, L"child");
  GC_CHECK(Contains(report, L"加入暂存区"));
  GC_CHECK(Contains(report, L"本程序不会替你在导航里 add／commit／push"));
  ExpectNoGlobalConfig(fixture);
}

GC_TEST(submodule_navigation_probes_do_not_touch_the_repositories) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring root = InitParentWithSubmodule(fixture, true);
  const std::wstring indexBefore = IndexListing(fixture);
  const long commitsBefore = fixture.CommitCount();
  // child 那份 origin 是夹具自己 `submodule add`（本地克隆）留下的，不是导航探测写上去的：
  // 所以这里核的是「探测前后逐字不变」，而不是「仓库里没有任何远端」。
  const std::wstring parentRemotes = Trimmed(fixture.RunInRepo({L"remote"}).out);
  fixture.SetActiveRepository(L"parent/child");
  const std::wstring childIndexBefore = IndexListing(fixture);
  const long childCommitsBefore = fixture.CommitCount();
  const std::wstring childHeadBefore = fixture.HeadSha();
  const std::wstring childRemotes = Trimmed(fixture.RunInRepo({L"remote"}).out);
  fixture.SetActiveRepository(L"parent");

  // 把导航会做的两组只读探测各跑一遍（进入之前的核对 + 返回之后的三份位置）。
  const SubmoduleEntryFacts entry = EntryFacts(fixture, root, L"child");
  static_cast<void>(gc::git::BuildSubmoduleEntryPlan(entry));
  static_cast<void>(PointerFacts(fixture, root));

  GC_CHECK(IndexListing(fixture) == indexBefore);
  GC_CHECK(fixture.CommitCount() == commitsBefore);
  fixture.SetActiveRepository(L"parent/child");
  GC_CHECK(IndexListing(fixture) == childIndexBefore);
  GC_CHECK(fixture.CommitCount() == childCommitsBefore);
  GC_CHECK(fixture.HeadSha() == childHeadBefore);
  fixture.SetActiveRepository(L"parent");
  // 导航这条链路不访问任何远端，也不写远端配置：两侧的 `git remote` 列表原样不动。
  GC_CHECK(Trimmed(fixture.RunInRepo({L"remote"}).out) == parentRemotes);
  fixture.SetActiveRepository(L"parent/child");
  GC_CHECK(Trimmed(fixture.RunInRepo({L"remote"}).out) == childRemotes);
  fixture.SetActiveRepository(L"parent");
  ExpectNoGlobalConfig(fixture);
}
