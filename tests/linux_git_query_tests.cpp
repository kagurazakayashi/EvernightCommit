// Linux 真实 Git 查询与后台编排集成测试。
// 只在用例自有的 PosixTempDirectory 里 init/add/commit（隔离环境），不联网、不推送；
// 另用测试自写的 /bin/sh 脚本验证大输出、超时、取消与环境策略，脚本只存在于临时根内。
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/task_coordinator.h"
#include "git/commit_history.h"
#include "git/repository.h"
#include "platform/git_query.h"
#include "platform/text_encoding.h"
#include "support/linux_git_fixture.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::git::RepoError;
using gc::git::RepoKind;
using gc::git::WorkspaceLoadStatus;
using gc::platform::StopFlag;
using gc::test::LinuxGitFixture;
using gc::test::PosixTempDirectory;
using gc::test::RequireMakeDirectory;
using gc::test::RequireWriteFile;
using gc::test::ScopedEnvironmentVariable;

void RequirePrepared(LinuxGitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), "Linux Git 夹具前置失败：" + reason);
}

bool ContainsPath(const std::vector<gc::git::ChangeItem>& items, std::wstring_view path) {
  for (const gc::git::ChangeItem& item : items) {
    if (item.path == path) {
      return true;
    }
  }
  return false;
}

long long ElapsedMilliseconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

std::wstring MakeScript(const PosixTempDirectory& temp, std::string_view name, std::string_view body) {
  const std::string path = temp.PathIn(name);
  RequireWriteFile(path, std::string("#!/bin/sh\n") + std::string(body), 0755);
  return gc::platform::Utf8ToWide(path);
}

}  // namespace

GC_TEST(linux_git_query_detects_repository_shapes) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"a.txt", "a\n", true);
  fixture.Commit(L"初始提交");
  RequireMakeDirectory(gc::platform::WideToUtf8(fixture.PathInRoot(L"plain")));

  gc::platform::RepoDetectRequest request;
  request.exePath = fixture.GitExe();
  request.directory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const gc::git::RepoDetection plain =
      gc::platform::DetectRepository(request, fixture.MakeDetectDeps());
  GC_CHECK(plain.kind == RepoKind::plainWorktree);
  GC_CHECK(plain.error == RepoError::none);
  GC_CHECK(plain.branch == L"main");
  GC_CHECK(plain.headResolved);
  GC_CHECK_MESSAGE(gc::git::PathsEqualFolded(plain.root, fixture.RepoDir()),
                   "识别出的工作区根应与夹具仓库一致");

  request.directory = fixture.PathInRoot(L"plain");
  const gc::git::RepoDetection notRepo =
      gc::platform::DetectRepository(request, fixture.MakeDetectDeps());
  GC_CHECK(notRepo.kind == RepoKind::notRepository);

  fixture.InitRepository(L"bare.git", true);
  request.directory = fixture.RepoDir();
  const gc::git::RepoDetection bare =
      gc::platform::DetectRepository(request, fixture.MakeDetectDeps());
  GC_CHECK(bare.kind == RepoKind::bare);
  GC_CHECK_MESSAGE(bare.root.empty(), "裸仓库没有可用的工作区根目录");

  fixture.InitRepository(L"unborn");
  fixture.WriteFile(L"only-untracked.txt", "x\n", false);
  request.directory = fixture.RepoDir();
  const gc::git::RepoDetection unborn =
      gc::platform::DetectRepository(request, fixture.MakeDetectDeps());
  GC_CHECK(unborn.kind == RepoKind::noCommits);
  GC_CHECK(!unborn.headResolved);
}

GC_TEST(linux_git_query_reads_real_status_history_author_detail_and_changes) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"普通.txt", "第一版\n", true);
  fixture.WriteFile(L"带 空格 的'名字'.txt", "x\n", true);
  fixture.WriteFile(L"emoji-\xF0\x9F\x98\x80.txt", "emoji\n", true);
  fixture.Commit(L"包含中文与特殊文件名");

  fixture.WriteFile(L"普通.txt", "第二版\n", false);
  fixture.WriteFile(L"未跟踪 文件.txt", "u\n", false);

  gc::platform::WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  request.repositoryHasCommits = true;
  const gc::git::WorkspaceSnapshot snapshot =
      gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
  GC_REQUIRE_MESSAGE(snapshot.status == WorkspaceLoadStatus::loaded,
                     gc::platform::WideToUtf8(snapshot.message));
  GC_CHECK(snapshot.error == RepoError::none);
  GC_CHECK_MESSAGE(ContainsPath(snapshot.model.unstaged, L"普通.txt"),
                   "修改过的文件应出现在未暂存侧");
  GC_CHECK_MESSAGE(ContainsPath(snapshot.model.unstaged, L"未跟踪 文件.txt"),
                   "未跟踪文件应原样出现在未暂存侧");
  GC_CHECK_MESSAGE(!snapshot.model.recentCommits.empty(), "有提交时历史列表不能为空");
  GC_CHECK(snapshot.historyError == RepoError::none);
  GC_CHECK(snapshot.model.recentCommits.front().summary == L"包含中文与特殊文件名");

  fixture.RunCheckedInRepo({L"config", L"user.name", L"配置作者"});
  fixture.RunCheckedInRepo({L"config", L"user.email", L"config@example.invalid"});
  gc::platform::AuthorConfigRequest authorRequest;
  authorRequest.exePath = fixture.GitExe();
  authorRequest.repositoryDirectory = fixture.RepoDir();
  authorRequest.timeoutMilliseconds = 20000;
  const gc::git::AuthorIdentityConfig identity =
      gc::platform::LoadAuthorIdentity(authorRequest, fixture.MakeAuthorConfigDepsForTest());
  GC_CHECK(identity.name.state == gc::git::ConfigValueState::present);
  GC_CHECK(identity.email.state == gc::git::ConfigValueState::present);
  GC_CHECK(identity.name.value == L"配置作者");
  GC_CHECK(identity.email.value == L"config@example.invalid");

  const std::wstring head = fixture.HeadSha();
  GC_REQUIRE_MESSAGE(gc::git::LooksLikeFullObjectId(head), "HEAD 应是完整的对象 ID");

  gc::platform::CommitDetailRequest detailRequest;
  detailRequest.exePath = fixture.GitExe();
  detailRequest.repositoryDirectory = fixture.RepoDir();
  detailRequest.objectId = head;
  detailRequest.readSerial = 7;
  detailRequest.bindingGeneration = 3;
  detailRequest.timeoutMilliseconds = 20000;
  const gc::platform::CommitDetailOutcome detail =
      gc::platform::RunCommitDetailLoad(detailRequest);
  GC_REQUIRE_MESSAGE(detail.error == RepoError::none,
                     gc::platform::WideToUtf8(detail.message));
  GC_CHECK(detail.detail.objectId == head);
  GC_CHECK(detail.detail.summary == L"包含中文与特殊文件名");
  GC_CHECK(detail.readSerial == 7);
  GC_CHECK(detail.bindingGeneration == 3);
  GC_CHECK(!detail.text.empty());

  gc::platform::CommitChangesRequest changesRequest;
  changesRequest.exePath = fixture.GitExe();
  changesRequest.repositoryDirectory = fixture.RepoDir();
  changesRequest.objectId = head;
  changesRequest.readSerial = 8;
  changesRequest.bindingGeneration = 3;
  changesRequest.timeoutMilliseconds = 20000;
  const gc::platform::CommitChangesOutcome changes =
      gc::platform::RunCommitChangesLoad(changesRequest);
  GC_REQUIRE_MESSAGE(changes.error == RepoError::none,
                     gc::platform::WideToUtf8(changes.message));
  GC_CHECK(ContainsPath(changes.changes, L"普通.txt"));
  GC_CHECK(ContainsPath(changes.changes, L"带 空格 的'名字'.txt"));
  GC_CHECK(changes.objectId == head);
}

GC_TEST(linux_git_query_commit_probe_collects_real_facts) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"staged.txt", "x\n", true);
  fixture.RunCheckedInRepo({L"config", L"user.name", L"提交者"});
  fixture.RunCheckedInRepo({L"config", L"user.email", L"committer@example.invalid"});

  gc::platform::CommitProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const gc::git::CommitIdentityFacts facts =
      gc::platform::CollectCommitPreflight(request, fixture.MakeCommitProbeDepsForTest());
  GC_REQUIRE_MESSAGE(facts.queryOk, gc::platform::WideToUtf8(facts.queryFailure));
  GC_CHECK(facts.onBranch);
  GC_CHECK(facts.branchName == L"main");
  GC_CHECK(!facts.headResolved);
  GC_CHECK_MESSAGE(facts.indexTreeResolved, "暂存内容应能被 write-tree 标识");
  GC_CHECK(gc::git::LooksLikeFullObjectId(facts.indexTreeOid));
  GC_CHECK(facts.committerConfigRead);
  GC_CHECK(facts.committer == gc::git::CommitterIdentityState::available);
}

GC_TEST(linux_git_query_incomplete_output_is_a_failure_not_empty_status) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  PosixTempDirectory scripts;
  GC_REQUIRE_MESSAGE(scripts.Valid(), scripts.FailureReason());
  const std::wstring script = MakeScript(scripts, "invalid-utf8", "printf '\\377\\376'; exit 0\n");

  gc::platform::WorkspaceStatusDeps deps;
  deps.runner = [&fixture, script](const std::wstring& exePath, const std::wstring& directory,
                                   const std::vector<std::wstring>& arguments) {
    return fixture.Query(script, arguments, directory, 5000);
  };
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = script;
  request.repositoryDirectory = fixture.RepoDir();
  request.repositoryHasCommits = false;
  const gc::git::WorkspaceSnapshot snapshot =
      gc::platform::LoadWorkspaceStatus(request, deps);
  GC_CHECK(snapshot.status == WorkspaceLoadStatus::failed);
  GC_CHECK(snapshot.error == RepoError::outputIncomplete);
  GC_CHECK_MESSAGE(!snapshot.message.empty(), "不完整输出必须给出原因，不能显示成空仓库");
}

GC_TEST(linux_git_query_incomplete_commit_probe_is_not_approved) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"staged.txt", "x\n", true);
  PosixTempDirectory scripts;
  GC_REQUIRE_MESSAGE(scripts.Valid(), scripts.FailureReason());
  const std::wstring script = MakeScript(scripts, "invalid-probe", "printf '\\377\\376'; exit 0\n");

  gc::platform::CommitProbeDeps deps;
  deps.runner = [&fixture, script](const std::wstring& exePath, const std::wstring& directory,
                                   const std::vector<std::wstring>& arguments) {
    return fixture.Query(script, arguments, directory, 5000);
  };
  gc::platform::CommitProbeRequest request;
  request.exePath = script;
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 5000;
  const gc::git::CommitIdentityFacts facts =
      gc::platform::CollectCommitPreflight(request, deps);
  GC_CHECK_MESSAGE(!facts.queryOk, "不完整输出不能当成预检通过");
  GC_CHECK_MESSAGE(!facts.queryFailure.empty(), "预检失败必须给出原因");
}

GC_TEST(linux_git_query_timeout_is_classified_as_timeout) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  PosixTempDirectory scripts;
  GC_REQUIRE_MESSAGE(scripts.Valid(), scripts.FailureReason());
  const std::wstring script = MakeScript(scripts, "slow-git", "sleep 5\n");

  const auto start = std::chrono::steady_clock::now();
  const gc::git::GitQueryResult result = fixture.Query(script, {L"status"}, fixture.RepoDir(), 200);
  GC_CHECK_MESSAGE(ElapsedMilliseconds(start) < 3000, "超时查询必须有界返回");
  GC_CHECK(result.started);
  GC_CHECK(result.timedOut);
  GC_CHECK(!result.exited || result.timedOut);
  std::wstring detail;
  GC_CHECK(gc::git::ClassifyGitFailure(result, detail) == RepoError::gitTimeout);
  GC_CHECK(!detail.empty());
}

GC_TEST(linux_git_query_launch_failure_is_classified) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  const gc::git::GitQueryResult result =
      fixture.Query(L"/definitely/not/here/git", {L"status"}, fixture.RepoDir(), 2000);
  GC_CHECK(!result.started);
  std::wstring detail;
  GC_CHECK(gc::git::ClassifyGitFailure(result, detail) == RepoError::gitLaunchFailed);
  GC_CHECK(!detail.empty());
}

GC_TEST(linux_git_query_environment_policy_disables_interaction_and_paging) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::wstring script = MakeScript(
      temp, "print-env",
      "printf 'prompt=%s\\npager=%s\\ndir=%s\\nwork_tree=%s\\n' \"$GIT_TERMINAL_PROMPT\" "
      "\"$GIT_PAGER\" \"$GIT_DIR\" \"$GIT_WORK_TREE\"\n");

  ScopedEnvironmentVariable gitDir("GIT_DIR", "/tmp/evernight-hijack.git");
  ScopedEnvironmentVariable gitWorkTree("GIT_WORK_TREE", "/tmp/evernight-hijack");
  ScopedEnvironmentVariable pager("GIT_PAGER", "less");
  const gc::git::GitQueryResult result =
      gc::platform::RunGitBackgroundQuery(script, {}, temp.Path(), 2000);
  GC_REQUIRE_MESSAGE(result.started && result.outputComplete,
                     gc::platform::WideToUtf8(result.incompleteReason + result.launchDetail));
  const std::string out = gc::platform::WideToUtf8(result.utf16Output);
  GC_CHECK_MESSAGE(out.find("prompt=0\n") != std::string::npos,
                   "后台通道必须关闭终端提问，实际输出：" + out);
  GC_CHECK_MESSAGE(out.find("pager=cat\n") != std::string::npos,
                   "后台通道必须禁用分页器，实际输出：" + out);
  GC_CHECK_MESSAGE(out.find("dir=\n") != std::string::npos,
                   "继承的 GIT_DIR 必须被移除，实际输出：" + out);
  GC_CHECK_MESSAGE(out.find("work_tree=\n") != std::string::npos,
                   "继承的 GIT_WORK_TREE 必须被移除，实际输出：" + out);
}

GC_TEST(linux_git_query_worker_cancel_stops_inflight_child) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo");
  PosixTempDirectory scripts;
  GC_REQUIRE_MESSAGE(scripts.Valid(), scripts.FailureReason());
  const std::wstring script = MakeScript(scripts, "slow-worker-git", "sleep 5\n");

  gc::platform::WorkspaceStatusWorker worker;
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = script;
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 10000;
  request.repositoryHasCommits = false;
  request.readSerial = 1;
  request.bindingGeneration = 1;

  const auto start = std::chrono::steady_clock::now();
  worker.Request(nullptr, 0, request, [](const gc::platform::WorkspaceStatusRequest& value) {
    return gc::platform::RunWorkspaceStatusLoad(value);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  worker.BeginStop();
  worker.Join();
  const long long elapsed = ElapsedMilliseconds(start);
  GC_CHECK_MESSAGE(elapsed < 3000,
                   "停止信号必须中止在途后台查询，实际耗时 " + std::to_string(elapsed) + "ms");
  gc::platform::WorkspaceLoadOutcome outcome;
  GC_CHECK_MESSAGE(!worker.FetchLatest(1, &outcome),
                   "收尾期间完成的结果不能被界面采用");
}

GC_TEST(linux_git_query_switch_repository_discards_older_background_result) {
  LinuxGitFixture fixture;
  RequirePrepared(fixture);
  fixture.InitRepository(L"repo-a");
  const std::wstring directoryA = fixture.RepoDir();
  fixture.InitRepository(L"repo-b");
  const std::wstring directoryB = fixture.RepoDir();

  PosixTempDirectory scripts;
  GC_REQUIRE_MESSAGE(scripts.Valid(), scripts.FailureReason());
  const std::wstring slowScript = MakeScript(scripts, "slow-a", "sleep 0.4; exit 0\n");
  const std::wstring fastScript = MakeScript(scripts, "fast-b", "exit 0\n");

  gc::app::TaskCoordinator coordinator;
  GC_REQUIRE(coordinator.BindRepository(fixture.GitExe(), directoryA), "应绑定仓库 A");
  const gc::app::ReadTicket ticketA = coordinator.BeginRead();

  const auto dispatcher = gc::platform::MakeQueuedMainThreadDispatcher();
  gc::platform::WorkspaceStatusWorker worker;
  worker.SetCompletionDispatcher(dispatcher, 1);

  gc::platform::WorkspaceStatusRequest requestA;
  requestA.exePath = slowScript;
  requestA.repositoryDirectory = directoryA;
  requestA.timeoutMilliseconds = 10000;
  requestA.repositoryHasCommits = false;
  requestA.readSerial = ticketA.serial;
  requestA.bindingGeneration = ticketA.generation;
  worker.Request(nullptr, 0, requestA, [](const gc::platform::WorkspaceStatusRequest& value) {
    return gc::platform::RunWorkspaceStatusLoad(value);
  });
  // 等 A 真正进入工作线程再提交 B：worker 是单槽最新优先，若 B 在 A 开工前到达，
  // A 会被合并掉（那不是“切换仓库后旧结果迟到”的场景）。
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 用户切到仓库 B：身份版本递增，A 的读取凭据不再属于当前现场。
  GC_REQUIRE(coordinator.BindRepository(fixture.GitExe(), directoryB), "应切换到仓库 B");
  const gc::app::ReadTicket ticketB = coordinator.BeginRead();
  gc::platform::WorkspaceStatusRequest requestB;
  requestB.exePath = fastScript;
  requestB.repositoryDirectory = directoryB;
  requestB.timeoutMilliseconds = 20000;
  requestB.repositoryHasCommits = false;
  requestB.readSerial = ticketB.serial;
  requestB.bindingGeneration = ticketB.generation;
  worker.Request(nullptr, 0, requestB, [](const gc::platform::WorkspaceStatusRequest& value) {
    return gc::platform::RunWorkspaceStatusLoad(value);
  });

  std::vector<gc::platform::MainThreadDispatcher::Notice> notices;
  for (int attempt = 0; attempt < 1000 && notices.size() < 2; ++attempt) {
    std::vector<gc::platform::MainThreadDispatcher::Notice> drained = dispatcher->Drain();
    notices.insert(notices.end(), drained.begin(), drained.end());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  GC_REQUIRE_MESSAGE(notices.size() == 2,
                     "两次读取都应完成并投递通知，实际 " + std::to_string(notices.size()) + " 条");

  gc::platform::WorkspaceLoadOutcome outcome;
  GC_CHECK_MESSAGE(!worker.FetchLatest(ticketA.serial, &outcome),
                   "旧仓库的读取结果不能覆盖当前仓库");
  GC_REQUIRE_MESSAGE(worker.FetchLatest(ticketB.serial, &outcome),
                     "当前仓库的读取结果应可落地");
  GC_CHECK(outcome.snapshot.status == WorkspaceLoadStatus::loaded);
  GC_CHECK(outcome.readSerial == ticketB.serial);
  GC_CHECK(outcome.bindingGeneration == ticketB.generation);
  GC_CHECK_MESSAGE(coordinator.CompleteRead(ticketB.serial) == gc::app::ReadDisposition::accepted,
                   "当前读取应被协调器接受");
  GC_CHECK_MESSAGE(coordinator.CompleteRead(ticketA.serial) != gc::app::ReadDisposition::accepted,
                   "旧仓库的读取凭据必须被判为过期或不在途");

  worker.BeginStop();
  worker.Join();
  dispatcher->Close();
}
