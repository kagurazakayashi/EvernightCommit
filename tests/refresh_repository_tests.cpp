// 用真实临时仓库驱动「刷新」这条链路：外部改动能否被下一次读取带回来、属于上个仓库的
// 迟到结果会不会被丢弃、读取失败后能否再刷新恢复，以及 Git 锁文件在读取失败时的表现。
// 所有仓库都由夹具在本机临时目录里新建，绝不触碰用户真实仓库或远端。
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.h"
#include "app/task_coordinator.h"
#include "git/repository.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/workspace_status.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::app::AppState;
using gc::app::ReadDisposition;
using gc::app::RefreshSchedule;
using gc::app::TaskCoordinator;
using gc::git::ChangeItem;
using gc::git::RepoError;
using gc::git::WorkspaceLoadStatus;
using gc::git::WorkspaceModel;
using gc::git::WorkspaceSnapshot;
using gc::platform::WorkspaceStatusDeps;
using gc::platform::WorkspaceStatusRequest;
using gc::test::GitFixture;
using gc::test::GitRun;

void Prepare(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE(fixture.Prepare(reason), reason);
}

// 走生产读取路径：夹具只提供隔离执行器，不参与解析或判定。
WorkspaceSnapshot Load(GitFixture& fixture, const std::wstring& directory) {
  WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = directory;
  request.timeoutMilliseconds = 20000;
  return gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
}

bool HasPath(const std::vector<ChangeItem>& items, std::wstring_view path) {
  for (const ChangeItem& item : items) {
    if (item.path == path) {
      return true;
    }
  }
  return false;
}

// 一次完整的刷新：绑定身份 → 取凭证 → 读取 → 判定后落地。返回该结果是否被采纳。
bool RefreshOnce(TaskCoordinator& tasks, AppState& state, GitFixture& fixture, const std::wstring& directory) {
  GC_REQUIRE_MESSAGE(tasks.RequestRefresh() == RefreshSchedule::start, "刷新应当被允许发起一次读取");
  const gc::app::ReadTicket ticket = tasks.BeginRead();
  const WorkspaceSnapshot snapshot = Load(fixture, directory);
  if (tasks.CompleteRead(ticket.serial) != ReadDisposition::accepted) {
    return false;
  }
  state.SetWorkspace(snapshot);
  return true;
}

// 可控的失败读取：桩执行器给出 Git 在锁文件存在时的真实形态（非 0 退出 + fatal 行）。
WorkspaceSnapshot LoadWithStub(std::wstring_view stderrText, long exitCode) {
  WorkspaceStatusRequest request;
  request.exePath = L"git.exe";
  request.repositoryDirectory = L"D:\\项目\\仓库甲";
  WorkspaceStatusDeps deps;
  deps.runner = [stderrText, exitCode](const std::wstring&, const std::wstring&,
                                       const std::vector<std::wstring>&) {
    gc::git::GitQueryResult result;
    result.started = true;
    result.exited = true;
    result.exitCode = static_cast<int>(exitCode);
    result.utf16Error = std::wstring(stderrText);
    return result;
  };
  return gc::platform::LoadWorkspaceStatus(request, deps);
}

}  // namespace

GC_TEST(refresh_picks_up_changes_made_outside_the_program) {
  GitFixture fixture;
  Prepare(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.Stage({L"tracked.txt"});
  fixture.Commit(L"base");

  TaskCoordinator tasks;
  AppState state;
  GC_REQUIRE_MESSAGE(tasks.BindRepository(fixture.GitExe(), fixture.RepoDir()), "首次绑定仓库身份应当成功");
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, fixture.RepoDir()), "刷新应完成一次被采纳的读取");
  GC_CHECK(state.Workspace().status == WorkspaceLoadStatus::loaded);
  GC_CHECK(!HasPath(state.WorkspaceModel().unstaged, L"tracked.txt"));

  // 外部程序（这里是夹具，等价于用户在另一个终端里）改了仓库：
  // 一次刷新就该把新状态读回来，不依赖任何文件监听。
  fixture.WriteFile(L"tracked.txt", "edited elsewhere\n");
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, fixture.RepoDir()), "刷新应完成一次被采纳的读取");
  GC_CHECK(HasPath(state.WorkspaceModel().unstaged, L"tracked.txt"));

  // 再暂存一次：条目从「未暂存」移到「已暂存」，两侧不会同时留同一份旧内容。
  fixture.Stage({L"tracked.txt"});
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, fixture.RepoDir()), "刷新应完成一次被采纳的读取");
  GC_CHECK(HasPath(state.WorkspaceModel().staged, L"tracked.txt"));
  GC_CHECK(!HasPath(state.WorkspaceModel().unstaged, L"tracked.txt"));
}

GC_TEST(late_result_from_previous_repository_is_discarded) {
  GitFixture fixture;
  Prepare(fixture);

  // 仓库甲：一处未暂存修改。
  fixture.InitRepository(L"repoA");
  fixture.WriteFile(L"only-in-a.txt", "base\n");
  fixture.Stage({L"only-in-a.txt"});
  fixture.Commit(L"a base");
  fixture.WriteFile(L"only-in-a.txt", "changed\n");
  const std::wstring repoA = fixture.RepoDir();

  // 仓库乙：一处已暂存修改，未暂存侧为空。
  fixture.InitRepository(L"repoB");
  fixture.WriteFile(L"only-in-b.txt", "base\n");
  fixture.Stage({L"only-in-b.txt"});
  fixture.Commit(L"b base");
  fixture.WriteFile(L"only-in-b.txt", "staged change\n");
  fixture.Stage({L"only-in-b.txt"});
  const std::wstring repoB = fixture.RepoDir();

  TaskCoordinator tasks;
  AppState state;
  GC_REQUIRE_MESSAGE(tasks.BindRepository(fixture.GitExe(), repoA), "绑定仓库甲的身份应成功");
  GC_REQUIRE_MESSAGE(tasks.RequestRefresh() == RefreshSchedule::start, "甲的读取应可发起");
  const gc::app::ReadTicket ticketOfA = tasks.BeginRead();
  const WorkspaceSnapshot snapshotOfA = Load(fixture, repoA);
  GC_REQUIRE_MESSAGE(HasPath(snapshotOfA.model.unstaged, L"only-in-a.txt"), "甲的读取结果应有内容可比对");

  // 用户切到仓库乙，乙的结果先回来并落地。
  GC_REQUIRE_MESSAGE(tasks.BindRepository(fixture.GitExe(), repoB), "绑定仓库乙的身份应成功");
  state.SetWorkspace(snapshotOfA);  // 若不做身份判定就会把甲的列表当成乙的现状留下
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, repoB), "刷新应完成一次被采纳的读取");
  const WorkspaceModel modelOfB = state.WorkspaceModel();
  GC_REQUIRE_MESSAGE(HasPath(modelOfB.staged, L"only-in-b.txt"), "乙的结果应已落地");

  // 甲的读取这时才完成：属于上个仓库的结果一律丢弃，列表保持乙的内容。
  GC_CHECK_MESSAGE(tasks.CompleteRead(ticketOfA.serial) != ReadDisposition::accepted,
                   "上个仓库的迟到结果必须被丢弃");
  GC_CHECK(state.WorkspaceModel().staged.size() == modelOfB.staged.size());
  GC_CHECK(HasPath(state.WorkspaceModel().staged, L"only-in-b.txt"));
  GC_CHECK_MESSAGE(state.WorkspaceModel().unstaged.empty(), "上个仓库的未暂存条目不能出现在当前列表");
}

GC_TEST(read_failure_lands_as_failure_and_recovers_on_next_refresh) {
  GitFixture fixture;
  Prepare(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.Stage({L"tracked.txt"});
  fixture.Commit(L"base");

  TaskCoordinator tasks;
  AppState state;
  GC_REQUIRE_MESSAGE(tasks.BindRepository(fixture.GitExe(), fixture.RepoDir()), "首次绑定仓库身份应当成功");

  // 读取失败（这里是 Git 无法解析配置的真实形态）：状态必须是 failed，
  // 并且模型为空——界面宁可显示“读取失败 + 原因”，也不能交出一份看不准的列表。
  const WorkspaceSnapshot failed =
      LoadWithStub(L"fatal: bad config line 1 in file D:/nowhere/config\n", 128);
  GC_REQUIRE_MESSAGE(failed.status == WorkspaceLoadStatus::failed, "读取失败必须归为 failed 而不是空列表");
  GC_CHECK(failed.error == RepoError::gitFailed);
  state.SetWorkspace(failed);
  const gc::git::EmptyStateTexts hints = state.WorkspaceHintTexts();
  GC_CHECK(hints.unstaged.find(L"读取失败") != std::wstring::npos);
  GC_CHECK(!hints.unstaged.empty());

  // 用户改正情况后点刷新：同一身份下的新读取被采纳，列表恢复真实内容。
  fixture.WriteFile(L"tracked.txt", "changed\n");
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, fixture.RepoDir()), "刷新应完成一次被采纳的读取");
  GC_CHECK(state.Workspace().status == WorkspaceLoadStatus::loaded);
  GC_CHECK(HasPath(state.WorkspaceModel().unstaged, L"tracked.txt"));
}

GC_TEST(index_lock_failure_is_named_and_the_lock_file_is_left_alone) {
  GitFixture fixture;
  Prepare(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.Stage({L"tracked.txt"});
  fixture.Commit(L"base");
  fixture.WriteFile(L"tracked.txt", "changed\n");

  // 由夹具自己造出锁文件（临时仓库内的测试现场，不是用户仓库）。
  const std::filesystem::path lock(std::wstring(fixture.RepoDir()) + L"/.git/index.lock");
  std::error_code ec;
  std::filesystem::create_directories(lock.parent_path(), ec);
  std::ofstream(lock) << std::string();
  GC_REQUIRE_MESSAGE(std::filesystem::exists(lock), "测试需要已存在的 .git/index.lock");

  // 只读路径本身不该把锁当成错误，更不该顺手删掉它。
  const WorkspaceSnapshot snapshot = Load(fixture, fixture.RepoDir());
  GC_CHECK(snapshot.status == WorkspaceLoadStatus::loaded);
  GC_REQUIRE_MESSAGE(std::filesystem::exists(lock), "读取工作区绝不删除 Git 的锁文件");

  // 写操作才会真的被锁挡住：用夹具拿到的 Git 真实输出去验证归类，而不是照抄一段假设文本。
  const GitRun blocked = fixture.RunInRepo({L"add", L"--", L"tracked.txt"});
  GC_REQUIRE_MESSAGE(!blocked.Success(), "索引被锁住时 git add 应当失败");
  gc::git::GitQueryResult query;
  query.started = true;
  query.exited = true;
  query.exitCode = static_cast<int>(blocked.exitCode);
  query.utf16Error = blocked.err;
  std::wstring detail;
  GC_CHECK_MESSAGE(gc::git::ClassifyGitFailure(query, detail) == RepoError::indexLocked,
                   "Git 关于 index.lock 的报错要归成「被其他 Git 操作占用」");
  const std::wstring message = gc::git::BuildWorkspaceFailureMessage(RepoError::indexLocked, detail);
  GC_CHECK(message.find(L"占用") != std::wstring::npos);
  GC_CHECK(message.find(L"index.lock") != std::wstring::npos);

  // 锁消失后，下一次刷新就能恢复真实状态。
  std::filesystem::remove(lock, ec);
  GC_REQUIRE_MESSAGE(!std::filesystem::exists(lock), "清理测试自己创建的锁文件");
  const GitRun retry = fixture.RunInRepo({L"add", L"--", L"tracked.txt"});
  GC_CHECK_MESSAGE(retry.Success(), "锁释放后 git add 应能成功");
}

GC_TEST(operation_refresh_after_nonzero_exit_still_reads_the_repository) {
  GitFixture fixture;
  Prepare(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"tracked.txt", "base\n");
  fixture.Stage({L"tracked.txt"});
  fixture.Commit(L"base");
  fixture.WriteFile(L"tracked.txt", "changed\n");

  TaskCoordinator tasks;
  AppState state;
  GC_REQUIRE_MESSAGE(tasks.BindRepository(fixture.GitExe(), fixture.RepoDir()), "首次绑定仓库身份应当成功");
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, fixture.RepoDir()), "刷新应完成一次被采纳的读取");

  // 一次在命令窗口里失败的操作：暂存做了一半，仓库确实被改动了。
  unsigned long long serial = 0;
  GC_REQUIRE_MESSAGE(tasks.BeginOperation(L"stage", &serial), "首次发起操作应成功占用槽位");
  const gc::app::OperationOutcome outcome =
      tasks.FinishOperation(serial, gc::git::CommandCompletion::finished, 128, std::wstring_view{});
  GC_CHECK(!outcome.succeeded);
  GC_CHECK(outcome.refreshRequested);
  tasks.RememberOperationConclusion(outcome.note);

  // 失败也要重读：这一句就是「失败的操作同样可能已改动仓库」的落地。
  fixture.Stage({L"tracked.txt"});
  GC_REQUIRE_MESSAGE(RefreshOnce(tasks, state, fixture, fixture.RepoDir()), "刷新应完成一次被采纳的读取");
  GC_CHECK(HasPath(state.WorkspaceModel().staged, L"tracked.txt"));
  const std::wstring line = tasks.ReadFinishedText(state.Workspace().message);
  GC_CHECK_MESSAGE(line.find(L"stage") != std::wstring::npos, "状态栏要保留刚才那次失败操作的结论");
  GC_CHECK(line.find(L"工作区已读取") != std::wstring::npos);
}
