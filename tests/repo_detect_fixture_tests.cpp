// 用真实临时仓库驱动生产识别逻辑（platform::DetectRepository），
// 覆盖步骤 3 的形态边界：普通工作区、子目录上溯、尚无提交、非仓库、裸仓库、
// .git 内部、游离 HEAD、链接工作树、子模块。全部在夹具临时根内建立真实仓库。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <filesystem>
#include <string>
#include <vector>

#include "git/repository.h"
#include "platform/windows/repo_detect.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::PathsEqualFolded;
using gc::platform::DetectRepository;
using gc::platform::RepoDetectRequest;
using gc::test::GitFixture;

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
}

gc::git::RepoDetection DetectAt(GitFixture& fixture, const std::wstring& directory) {
  RepoDetectRequest request;
  request.exePath = fixture.GitExe();
  request.directory = directory;
  request.timeoutMilliseconds = 20000;
  const gc::platform::RepoDetectDeps deps = fixture.MakeDetectDeps();
  return DetectRepository(request, deps);
}

// 建一个带单次提交的最小仓库，供“有历史”类形态使用。
void InitWithCommit(GitFixture& fixture, std::wstring_view directoryName) {
  fixture.InitRepository(directoryName);
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");
}

}  // namespace

GC_TEST(detect_plain_repository_reports_branch_and_no_upstream) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture, L"repo");

  const auto detection = DetectAt(fixture, fixture.RepoDir());
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::plainWorktree,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK(detection.error == gc::git::RepoError::none);
  GC_CHECK(detection.branch == L"main");
  GC_CHECK_MESSAGE(detection.headResolved, "已有一次提交时 HEAD 应可解析");
  GC_CHECK_MESSAGE(PathsEqualFolded(detection.root, fixture.RepoDir()), "工作区根应等于夹具仓库目录");
  GC_CHECK_MESSAGE(detection.upstreamQueried && detection.upstream.empty(),
                   "没有远端时应查询成功且报告未设置上游");
  GC_CHECK(!detection.message.empty());
}

GC_TEST(detect_walks_up_from_subdirectory) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture, L"repo");
  fixture.WriteFile(L"src/deep/placeholder.txt", "x\n");

  const std::wstring deep = fixture.PathInRoot(L"repo/src/deep");
  const auto detection = DetectAt(fixture, deep);
  GC_CHECK(detection.kind == gc::git::RepoKind::plainWorktree);
  GC_CHECK_MESSAGE(PathsEqualFolded(detection.root, fixture.RepoDir()), "应从子目录上溯到工作区根");
  GC_CHECK_MESSAGE(detection.message.find(L"上溯") != std::wstring::npos,
                   "说明文本应告知用户已自动上溯");
}

GC_TEST(detect_empty_repository_as_without_commits) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");

  const auto detection = DetectAt(fixture, fixture.RepoDir());
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::noCommits,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK(detection.branch == L"main");
  GC_CHECK_MESSAGE(!detection.headResolved, "尚无提交时 HEAD 不可解析");
}

GC_TEST(detect_plain_directory_as_not_repository) {
  GitFixture fixture;
  PrepareFixture(fixture);
  const std::wstring plain = fixture.PathInRoot(L"plain-dir");
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(plain), ec);
  GC_REQUIRE(!ec, "创建非仓库目录失败：" + ec.message());

  const auto detection = DetectAt(fixture, plain);
  GC_CHECK(detection.kind == gc::git::RepoKind::notRepository);
  GC_CHECK(detection.error == gc::git::RepoError::notRepository);
  GC_CHECK(!detection.message.empty());
}

GC_TEST(detect_bare_repository_has_no_workspace) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitBareRepository(L"bare.git");

  const auto detection = DetectAt(fixture, fixture.RepoDir());
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::bare,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK_MESSAGE(detection.root.empty(), "裸仓库没有工作区根，root 必须留空");
  GC_CHECK(detection.error == gc::git::RepoError::none);
}

GC_TEST(detect_inside_git_directory) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture, L"repo");

  const auto detection = DetectAt(fixture, fixture.PathInRoot(L"repo/.git"));
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::insideGitDir,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK_MESSAGE(detection.root.empty(), ".git 内部没有可用工作区，root 必须留空");
}

GC_TEST(detect_detached_head) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture, L"repo");
  fixture.WriteFile(L"tracked.txt", "second\n");
  fixture.StageAll();
  fixture.Commit(L"第二次提交");
  fixture.RunCheckedInRepo({L"checkout", L"--detach", L"HEAD"});

  const auto detection = DetectAt(fixture, fixture.RepoDir());
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::detached,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK(detection.branch.empty());
  GC_CHECK_MESSAGE(!detection.shortSha.empty(), "游离 HEAD 应给出短提交 ID");
  GC_CHECK(detection.headResolved);
}

GC_TEST(detect_linked_worktree) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture, L"repo");
  const std::wstring worktree = fixture.PathInRoot(L"linked-wt");
  fixture.RunCheckedInRepo(
      {L"worktree", L"add", L"--quiet", L"-b", L"wt-branch", worktree, L"HEAD"});

  const auto detection = DetectAt(fixture, worktree);
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::linkedWorktree,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK(detection.linked);
  GC_CHECK(detection.branch == L"wt-branch");
  GC_CHECK_MESSAGE(PathsEqualFolded(detection.root, worktree), "工作区根应是链接工作树目录");
}

GC_TEST(detect_submodule_working_tree) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 先做一个“可被子模块引用”的真实仓库，再做父仓库并登记子模块。
  InitWithCommit(fixture, L"child-origin");
  const std::wstring childSource = fixture.RepoDir();
  InitWithCommit(fixture, L"parent");
  fixture.RunCheckedInRepo({L"-c", L"protocol.file.allow=always", L"submodule", L"add", L"--",
                            childSource, L"child"});

  const std::wstring childDir = fixture.PathInRoot(L"parent/child");
  const auto detection = DetectAt(fixture, childDir);
  GC_CHECK_MESSAGE(detection.kind == gc::git::RepoKind::submodule,
                   gc::platform::Utf16ToUtf8(detection.message));
  GC_CHECK(detection.submodule);
  GC_CHECK_MESSAGE(!detection.superprojectTree.empty() &&
                       PathsEqualFolded(detection.superprojectTree,
                                        fixture.PathInRoot(L"parent")),
                   "子模块应报出父仓库工作区");
}
