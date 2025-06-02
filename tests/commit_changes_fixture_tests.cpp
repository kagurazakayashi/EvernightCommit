// 「提交文件变化」的集成测试：在夹具自建的临时仓库里造出真实历史，把生产代码构造的
// Git 参数原样交给真实 Git 执行，核对「列表里会出现什么」与「双击后命令窗口会跑什么」。
// 覆盖：修改/新增/删除/重命名的清单、合并提交按第一个父提交给出变化、空提交的合法空清单、
// 不可信对象 ID 在发起查询之前就被拒、以及带通配符字符的文件名不被过度命中。
// 只在夹具的临时仓库里跑 Git，绝不接触开发仓库或远端。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_changes.h"
#include "git/repository.h"
#include "git/workspace_model.h"
#include "platform/commit_changes.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::platform::CommitChangesOutcome;
using gc::git::CommitItem;
using gc::test::GitFixture;
using gc::test::GitRun;

std::string Narrow(std::wstring_view text) { return gc::platform::Utf16ToUtf8(std::wstring(text)); }

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

bool Has(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

// 复用夹具的隔离执行器装配查询依赖：CommitChangesDeps 的 runner 与 git status 那一条
// 是同一形态（显式 -C + 受守卫的工作目录 + 隔离环境块），因此不必另建第二套执行器。
gc::platform::CommitChangesDeps MakeDeps(GitFixture& fixture) {
  gc::platform::CommitChangesDeps deps;
  deps.runner = fixture.MakeStatusDeps().runner;
  return deps;
}

gc::platform::CommitChangesRequest MakeRequest(GitFixture& fixture, std::wstring_view objectId) {
  gc::platform::CommitChangesRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.objectId = std::wstring(objectId);
  request.timeoutMilliseconds = 30000;
  return request;
}

// 找到某条路径的变化项；找不到返回 nullptr（调用方负责给出可读的「实际有哪些」）。
const ChangeItem* Find(const std::vector<ChangeItem>& changes, std::wstring_view path) {
  for (const ChangeItem& item : changes) {
    if (item.path == path) {
      return &item;
    }
  }
  return nullptr;
}

std::string DescribeChanges(const std::vector<ChangeItem>& changes) {
  std::string seen;
  for (const ChangeItem& item : changes) {
    seen += "(" + Narrow(item.statusCode) + "/" + Narrow(item.path) + ")";
  }
  return seen.empty() ? std::string("<空>") : seen;
}

const ChangeItem& RequireChange(const CommitChangesOutcome& outcome, std::wstring_view path,
                                const std::string& label) {
  const ChangeItem* found = Find(outcome.changes, path);
  if (found == nullptr) {
    GC_REQUIRE_MESSAGE(false, label + " 缺少变化项：" + Narrow(path) + " 实际有：" +
                                  DescribeChanges(outcome.changes));
  }
  return *found;
}

std::string Describe(const GitRun& run) {
  return "exit=" + std::to_string(run.exitCode) + " out=[" + Narrow(run.out) + "] err=[" +
         Narrow(run.err) + "]";
}

}  // namespace

GC_TEST(commit_changes_real_lists_modified_added_deleted_and_rename) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"keep.txt", "keep v1\n");
  fixture.WriteFile(L"old 名称.txt", "rename me\n");
  fixture.WriteFile(L"gone.txt", "vanish\n");
  fixture.StageAll();
  fixture.Commit(L"基线");

  // 一个提交里同时造出四种变化：修改、新增、删除、重命名。
  fixture.WriteFile(L"keep.txt", "keep v2\n");
  fixture.WriteFile(L"fresh.txt", "brand new\n");
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(fixture.PathInRoot(L"repo/gone.txt")), ec);
  GC_REQUIRE_MESSAGE(!ec, "删除工作区文件失败");
  fixture.RunCheckedInRepo({L"mv", L"old 名称.txt", L"new 名称.txt"});
  fixture.StageAll();
  fixture.Commit(L"第二");

  const std::wstring oid = fixture.HeadSha();
  GC_REQUIRE(!oid.empty(), "提交后 HEAD 必须可解析");

  const CommitChangesOutcome outcome = gc::platform::LoadCommitChanges(MakeRequest(fixture, oid),
                                                                      MakeDeps(fixture));
  GC_REQUIRE_MESSAGE(outcome.error == gc::git::RepoError::none, Narrow(outcome.message));
  GC_CHECK(outcome.changes.size() == 4);

  const ChangeItem& modified = RequireChange(outcome, L"keep.txt", "修改项");
  GC_CHECK(modified.kind == ChangeKind::modified);
  const ChangeItem& added = RequireChange(outcome, L"fresh.txt", "新增项");
  GC_CHECK(added.kind == ChangeKind::added);
  const ChangeItem& deleted = RequireChange(outcome, L"gone.txt", "删除项");
  GC_CHECK(deleted.kind == ChangeKind::deleted);
  // 重命名带着旧、新两条路径（本模块存在的理由之一：只看新路径会把它显示成「新增」）。
  const ChangeItem& renamed = RequireChange(outcome, L"new 名称.txt", "重命名项");
  GC_REQUIRE_MESSAGE(renamed.kind == ChangeKind::renamed,
                     "应识别为重命名，实际 " + Narrow(renamed.statusCode));
  GC_CHECK(renamed.oldPath == L"old 名称.txt");
  GC_CHECK(!outcome.message.empty());
}

GC_TEST(commit_changes_real_merge_commit_reports_first_parent_changes) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.RunCheckedInRepo({L"checkout", L"-b", L"side"});
  fixture.WriteFile(L"side.txt", "from side\n");
  fixture.StageAll();
  fixture.Commit(L"侧面提交");
  fixture.RunCheckedInRepo({L"checkout", L"main"});
  fixture.WriteFile(L"main.txt", "from main\n");
  fixture.StageAll();
  fixture.Commit(L"主线提交");
  // 两侧改了不同文件：合并必然无冲突、必然产生一枚两父提交。
  fixture.RunCheckedInRepo({L"merge", L"--no-ff", L"--no-edit", L"side"});

  const std::wstring oid = fixture.HeadSha();
  GC_REQUIRE(!oid.empty(), "合并后 HEAD 必须可解析");
  const std::wstring parent2 =
      fixture.RunChecked({L"rev-parse", L"HEAD^2"}, L"repo").out;
  GC_REQUIRE(!parent2.empty(), "本用例的前提是合并提交有两个父提交");

  const CommitChangesOutcome outcome = gc::platform::LoadCommitChanges(MakeRequest(fixture, oid),
                                                                      MakeDeps(fixture));
  GC_REQUIRE_MESSAGE(outcome.error == gc::git::RepoError::none, Narrow(outcome.message));
  // 实测依据：默认形态下 git show 对合并提交一个 name-status 都不输出，那样会被误读成
  // 「这条提交没改东西」；带 --first-parent 后相对第一个父提交给出变化。
  GC_REQUIRE_MESSAGE(!outcome.changes.empty(), "合并提交应相对第一个父提交列出变化");
  const ChangeItem& sideFile = RequireChange(outcome, L"side.txt", "合并提交");
  GC_CHECK(sideFile.kind == ChangeKind::added);
}

GC_TEST(commit_changes_real_empty_commit_lists_nothing_but_succeeds) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.RunCheckedInRepo({L"commit", L"--allow-empty", L"-m", L"空提交"});

  const CommitChangesOutcome outcome =
      gc::platform::LoadCommitChanges(MakeRequest(fixture, fixture.HeadSha()), MakeDeps(fixture));
  // 空清单是合法事实，不是读取失败：界面据 message 如实说明即可。
  GC_REQUIRE_MESSAGE(outcome.error == gc::git::RepoError::none, Narrow(outcome.message));
  GC_CHECK(outcome.changes.empty());
  GC_CHECK(Has(outcome.message, L"没有文件变化"));
}

GC_TEST(commit_changes_real_rejects_untrusted_object_id_before_running) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "a\n");
  fixture.StageAll();
  fixture.Commit(L"基线");

  // 大写十六进制：LooksLikeFullObjectId 只认 %H 的小写输出，必须在发起查询之前拒绝，
  // 绝不把不可信文本交给 git show（工作线程这一层是最后一道关口）。
  const CommitChangesOutcome upper = gc::platform::LoadCommitChanges(
      MakeRequest(fixture, L"0123456789ABCDEF0123456789ABCDEF01234567"), MakeDeps(fixture));
  GC_CHECK(upper.error != gc::git::RepoError::none);
  GC_CHECK(upper.changes.empty());
  GC_CHECK(!upper.message.empty());

  // 空工作区根：没有安全落点，同样不发查询。
  gc::platform::CommitChangesRequest noDir = MakeRequest(fixture, fixture.HeadSha());
  noDir.repositoryDirectory.clear();
  const CommitChangesOutcome noRoot = gc::platform::LoadCommitChanges(noDir, MakeDeps(fixture));
  GC_CHECK(noRoot.error != gc::git::RepoError::none);
  GC_CHECK(noRoot.changes.empty());
}

// 下面两条刻意不执行任何 commit：用 plumbing 造出一枚真实的、有父提交的对象，
// 令 AI 侧（只允许跑 pure/windows/git-readonly 三组）也能亲手核验。
GC_TEST(commit_changes_seeded_object_reports_real_changes_without_commit) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  const std::wstring parent = fixture.SeedCommitObject(L"被引用的父提交");
  fixture.WriteFile(L"base.txt", "base v2\n");
  fixture.WriteFile(L"added.txt", "added\n");
  fixture.StageAll();
  const std::wstring child = fixture.SeedCommitObject(L"本用例要读的提交", parent);

  const CommitChangesOutcome outcome =
      gc::platform::LoadCommitChanges(MakeRequest(fixture, child), MakeDeps(fixture));
  GC_REQUIRE_MESSAGE(outcome.error == gc::git::RepoError::none, Narrow(outcome.message));
  GC_REQUIRE_MESSAGE(outcome.changes.size() == 2,
                     "相对父提交应有两条变化，实际：" + DescribeChanges(outcome.changes));
  const ChangeItem& modified = RequireChange(outcome, L"base.txt", "修改项");
  GC_CHECK(modified.kind == ChangeKind::modified);
  const ChangeItem& added = RequireChange(outcome, L"added.txt", "新增项");
  GC_CHECK(added.kind == ChangeKind::added);

  // 双击那一步会执行的参数，同样交给真实 Git 跑一遍。
  CommitItem commit;
  commit.objectId = child;
  commit.summary = L"本用例要读的提交";
  const gc::git::CommitFileDiffPlan plan = gc::git::BuildCommitFileDiffPlan(commit, modified);
  GC_REQUIRE_MESSAGE(plan.allowed, Narrow(plan.refusalReason));
  const GitRun run = fixture.RunInRepo(plan.arguments);
  GC_CHECK_MESSAGE(run.exitCode == 0, Describe(run));
  GC_CHECK_MESSAGE(Has(run.out, L"base v2"), Narrow(run.out));
}

GC_TEST(commit_changes_real_missing_object_is_a_read_failure) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "a\n");
  fixture.StageAll();
  fixture.Commit(L"基线");

  // 形态合法、对象不存在：这是真实的读取失败（git show 非 0 退出），必须归类上报，
  // 不能展示成「这条提交没有文件变化」。
  const CommitChangesOutcome outcome = gc::platform::LoadCommitChanges(
      MakeRequest(fixture, L"0000000000000000000000000000000000000000"), MakeDeps(fixture));
  GC_CHECK(outcome.error != gc::git::RepoError::none);
  GC_CHECK(outcome.changes.empty());
  GC_CHECK(!outcome.message.empty());
}

GC_TEST(commit_changes_real_file_diff_plan_does_not_over_match) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  // Windows 合法、但会被 glob 当成字符类的档名，与相邻的 ab.txt 只差一对中括号。
  fixture.WriteFile(L"a[b].txt", "one\n");
  fixture.WriteFile(L"ab.txt", "one\n");
  fixture.StageAll();
  fixture.Commit(L"基线");
  fixture.WriteFile(L"a[b].txt", "two\n");
  fixture.WriteFile(L"ab.txt", "two\n");
  fixture.StageAll();
  fixture.Commit(L"第二");

  CommitItem commit;
  commit.objectId = fixture.HeadSha();
  commit.summary = L"第二";
  const CommitChangesOutcome outcome = gc::platform::LoadCommitChanges(MakeRequest(fixture, commit.objectId),
                                                                      MakeDeps(fixture));
  GC_REQUIRE_MESSAGE(outcome.error == gc::git::RepoError::none, Narrow(outcome.message));
  const ChangeItem& bracket = RequireChange(outcome, L"a[b].txt", "提交文件变化");

  const gc::git::CommitFileDiffPlan plan = gc::git::BuildCommitFileDiffPlan(commit, bracket);
  GC_REQUIRE_MESSAGE(plan.allowed, Narrow(plan.refusalReason));
  const GitRun run = fixture.RunInRepo(plan.arguments);
  GC_CHECK_MESSAGE(run.exitCode == 0, Describe(run));
  GC_CHECK_MESSAGE(Has(run.out, L"a[b].txt"), Narrow(run.out));
  // 字面 pathspec 关掉通配：一次查看不该把 ab.txt 的改动也倒进窗口。
  GC_CHECK_MESSAGE(!Has(run.out, L"diff --git a/ab.txt "),
                   "字面 pathspec 不该连带命中 ab.txt：" + Narrow(run.out));
}
