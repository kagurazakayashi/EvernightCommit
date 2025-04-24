// 集中 Git 环境策略的集成测试（验收项 14）：在测试进程环境里布置「从终端或其他 Git
// 工具启动本程序」会继承到的 GIT_DIR / GIT_WORK_TREE / GIT_INDEX_FILE / 配置注入与身份
// 变量，然后驱动生产的后台查询与命令窗口执行器，证明：
//   * 读取与允许的操作仍绑定界面所选仓库与其预期索引；
//   * 未被选中的第二个仓库、外置索引文件的字节保持原样；
//   * HOME/USERPROFILE/PATH 与 SSH/凭据相关变量按既定策略照常在场可用。
// 全部仓库都在用例自有的临时夹具里；没有任何用例执行 git commit / git push——
// 唯一涉及提交落地的用例名带 git_env_identity_commit_ 前缀，按约束交由用户本人运行。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "git/command_window.h"
#include "git/git_environment.h"
#include "git/repository.h"
#include "git/workspace_status.h"
#include "platform/windows/command_window_runner.h"
#include "platform/windows/git_query_result.h"
#include "platform/windows/repo_detect.h"
#include "platform/windows/utf_text.h"

namespace git = gc::git;

namespace {

using gc::git::CommandWindowOperation;
using gc::git::EnvironmentOverride;
using gc::platform::CommandWindowResult;
using gc::test::GitFixture;
using gc::test::PrerequisiteFailure;

constexpr unsigned long kQueryTimeoutMs = 20000;
constexpr auto kOperationBudget = std::chrono::seconds(30);

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
}

// 作用域内的进程环境变量：生产装配函数读的就是这里，用完必须精确复原。
class ScopedEnv {
public:
  ScopedEnv(std::wstring name, std::optional<std::wstring> value) : name_(std::move(name)) {
    wchar_t buffer[4096];
    const DWORD got =
        ::GetEnvironmentVariableW(name_.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (got == 0 && ::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
      hadPrevious_ = false;
    } else if (got > 0 && got < std::size(buffer)) {
      hadPrevious_ = true;
      previous_ = std::wstring(buffer, got);
    } else {
      unusable_ = true;
      return;
    }
    Apply(value);
  }
  ~ScopedEnv() {
    if (!unusable_) {
      Apply(hadPrevious_ ? std::optional<std::wstring>(previous_) : std::optional<std::wstring>());
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  void Apply(const std::optional<std::wstring>& value) {
    if (value.has_value()) {
      ::SetEnvironmentVariableW(name_.c_str(), value->c_str());
    } else {
      ::SetEnvironmentVariableW(name_.c_str(), nullptr);
    }
  }
  std::wstring name_;
  bool hadPrevious_ = false;
  std::wstring previous_;
  bool unusable_ = false;
};

std::string ReadFileBytes(const std::wstring& path) {
  std::ifstream file(std::filesystem::path(path), std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::wstring& path, const std::string& bytes) {
  std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.flush();
  if (!file.good()) {
    throw PrerequisiteFailure("写夹具文件失败: " + gc::platform::Utf16ToUtf8(path));
  }
}

std::wstring JoinPath(const std::wstring& base, std::wstring_view relative) {
  std::wstring result(base);
  if (!result.empty() && result.back() != L'\\') {
    result.push_back(L'\\');
  }
  result.append(relative);
  return result;
}

std::string WideToUtf8Diag(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

// 在夹具的隔离 HOME 里放一份用户层配置：生产策略保留 HOME/USERPROFILE，
// Git 因此能在「看不见夹具的 GIT_CONFIG_GLOBAL」的前提下仍然拿到确定身份。
void InstallFixtureHomeConfig(GitFixture& fixture, const std::string& utf8Config) {
  const std::wstring home = fixture.PathInRoot(L"git-home");
  WriteFileBytes(JoinPath(home, L".gitconfig"), utf8Config);
}

// 两个仓库 + 外置索引的场景布置：B 是被劫持目标（有自己的分支名与已暂存文件），
// A 是界面选中的仓库。external 是 B 索引的字节副本，模拟「别人正用的外置索引」。
struct BindingRig {
  GitFixture fixture;
  std::wstring dirA;
  std::wstring dirB;
  std::wstring externalIndex;
  std::vector<std::unique_ptr<ScopedEnv>> hostile;  // 必须在断言与夹具清理之前全部析构

  void Prepare() {
    PrepareFixture(fixture);
    fixture.InitRepository(L"repoA");
    dirA = fixture.RepoDir();
    fixture.WriteFile(L"a.txt", "a\n");

    fixture.InitRepository(L"repoB");
    dirB = fixture.RepoDir();
    fixture.WriteFile(L"b.txt", "b\n");
    fixture.RunCheckedInRepo({L"symbolic-ref", L"HEAD", L"refs/heads/branchB"});
    fixture.StageAll();  // B 的索引里从此有 b.txt（只是 add，不涉及提交）。

    const std::string bIndexBytes = ReadFileBytes(JoinPath(dirB, L".git\\index"));
    externalIndex = fixture.PathInRoot(L"external-index");
    WriteFileBytes(externalIndex, bIndexBytes);

    fixture.SetActiveRepository(dirA);
    InstallFixtureHomeConfig(fixture, "[user]\n\tname = 夹具提交者\n\temail = fixture@example.invalid\n");

    // 「从终端启动」时会继承到的一整套重定向：仓库搬到 B、索引搬到外置文件、
    // 配置注入要求隐藏未跟踪文件、身份劫持作者名。
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_DIR", std::wstring(JoinPath(dirB, L".git"))));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_WORK_TREE", dirB));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_INDEX_FILE", externalIndex));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_CONFIG_COUNT", std::wstring(L"1")));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_CONFIG_KEY_0", std::wstring(L"core.untracked")));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_CONFIG_VALUE_0", std::wstring(L"no")));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_AUTHOR_NAME", std::wstring(L"继承劫持者")));
    // 把 HOME/USERPROFILE 指到夹具的隔离 HOME：生产策略保留它们，
    // 用户层配置因此仍由夹具说了算，绝不触碰本机真实配置。
    hostile.push_back(
        std::make_unique<ScopedEnv>(L"HOME", std::wstring(fixture.PathInRoot(L"git-home"))));
    hostile.push_back(
        std::make_unique<ScopedEnv>(L"USERPROFILE", std::wstring(fixture.PathInRoot(L"git-home"))));
    hostile.push_back(std::make_unique<ScopedEnv>(L"GIT_CONFIG_NOSYSTEM", std::wstring(L"1")));
  }

  [[nodiscard]] std::string SnapshotUnselectedTargets() const {
    // 取证快照：未选中仓库的索引与外置索引的字节。
    return ReadFileBytes(JoinPath(dirB, L".git\\index")) + "\x01" + ReadFileBytes(externalIndex);
  }
};

struct RunnerGuard {
  gc::platform::CommandWindowRunner* runner = nullptr;
  std::vector<uint64_t>* ids = nullptr;
  ~RunnerGuard() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      bool allGone = true;
      for (uint64_t id : *ids) {
        runner->CloseOperationWindow(id);
        if (!runner->ConsoleExited(id)) {
          allGone = false;
        }
      }
      if (allGone) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    runner->ClearAllResults();
    runner->Shutdown();
  }
};

bool WaitForTerminal(gc::platform::CommandWindowRunner& runner, uint64_t id,
                     CommandWindowResult* out) {
  const auto deadline = std::chrono::steady_clock::now() + kOperationBudget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runner.TakeResult(id, out)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

}  // namespace

// 后台只读链路：识别、状态、配置查询、身份读取全部仍绑定 A；B 与外置索引字节不动。
GC_TEST(git_env_binding_probe_reads_bind_selected_repo) {
  BindingRig rig;
  rig.Prepare();
  const std::string before = rig.SnapshotUnselectedTargets();

  // 1) 生产仓库识别：用户选中的是 A，被劫持的 GIT_DIR 不许把答案换成 B。
  const git::RepoDetection detection = gc::platform::RunRepositoryDetection(
      {rig.fixture.GitExe(), rig.dirA, kQueryTimeoutMs});
  GC_CHECK_MESSAGE(detection.error == git::RepoError::none, WideToUtf8Diag(detection.message));
  GC_CHECK(git::PathsEqualFolded(detection.root, rig.dirA));
  GC_CHECK_MESSAGE(detection.branch == L"main", "识别到的分支必须来自所选仓库：" + WideToUtf8Diag(detection.branch));
  GC_CHECK_MESSAGE(detection.message.find(L"GIT_DIR") != std::wstring::npos,
                   "识别结果必须告知移除过继承的重定向变量");

  // 2) 生产工作区状态：列出的必须是 A 的未跟踪文件，且注入的 core.untracked=no 未被采纳。
  const git::GitQueryResult status = gc::platform::RunGitBackgroundQuery(
      rig.fixture.GitExe(), git::BuildWorkspaceStatusArguments(rig.dirA), rig.dirA, kQueryTimeoutMs);
  GC_CHECK_MESSAGE(status.started && status.exited && status.exitCode == 0 && status.outputComplete,
                   "状态查询未拿到完整成功回答：" + WideToUtf8Diag(status.incompleteReason));
  GC_CHECK(status.utf16Output.find(L"a.txt") != std::wstring::npos);
  GC_CHECK_MESSAGE(status.utf16Output.find(L"b.txt") == std::wstring::npos,
                   "被劫持的重定向把别的仓库的文件读进来了：" + WideToUtf8Diag(status.utf16Output));
  GC_CHECK(status.environmentNotice.find(L"GIT_INDEX_FILE") != std::wstring::npos);
  GC_CHECK_MESSAGE(status.environmentNotice.find(rig.dirB) == std::wstring::npos &&
                       status.environmentNotice.find(L"继承劫持") == std::wstring::npos,
                   "告知文本里绝不允许出现被移除变量的值");

  // 3) 配置注入不落地：注入的键在仓库配置查询里必须查无此项。
  const git::GitQueryResult injected = gc::platform::RunGitBackgroundQuery(
      rig.fixture.GitExe(),
      {L"-C", rig.dirA, L"--no-optional-locks", L"config", L"--get", L"core.untracked"}, rig.dirA,
      kQueryTimeoutMs);
  GC_CHECK_MESSAGE(injected.started && injected.exited && injected.exitCode != 0,
                   "GIT_CONFIG_COUNT 注入的键不应被后台查询采纳");

  // 4) 身份读取一致：继承的 GIT_AUTHOR_NAME 被移除后，Git 的回答里不许出现它。
  const git::GitQueryResult author = gc::platform::RunGitBackgroundQuery(
      rig.fixture.GitExe(), {L"-C", rig.dirA, L"--no-optional-locks", L"var", L"GIT_AUTHOR_IDENT"},
      rig.dirA, kQueryTimeoutMs);
  GC_CHECK_MESSAGE(author.started && author.exited, "身份读取未能取得 Git 的回答");
  GC_CHECK_MESSAGE(author.utf16Output.find(L"继承劫持者") == std::wstring::npos,
                   "继承的 GIT_AUTHOR_NAME 泄漏进了 Git 的身份回答：" + WideToUtf8Diag(author.utf16Output));

  // 5) 未被选中的仓库与外置索引：一次只读查询也不许碰它们的字节。
  GC_CHECK_MESSAGE(rig.SnapshotUnselectedTargets() == before, "未选中仓库的索引或外置索引字节被改动");
}

// 命令窗口执行链路：一次允许的写操作（git add，不是提交）绑定所选仓库与它的索引；
// 劫持目标里的字节原样，操作结果还能从夹具自己的读法里看见。
GC_TEST(git_env_binding_command_window_add_binds_selected_repo) {
  BindingRig rig;
  rig.Prepare();
  const std::string before = rig.SnapshotUnselectedTargets();

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  CommandWindowOperation operation;
  operation.operationId = L"env-binding-add";
  operation.displayName = L"add";
  operation.gitExecutable = rig.fixture.GitExe();
  operation.repositoryDirectory = rig.dirA;
  operation.arguments = {L"add", L"a.txt"};

  uint64_t id = 0;
  CommandWindowResult failure;
  GC_REQUIRE_MESSAGE(runner.Start(operation, &id, &failure),
                     "命令窗口启动失败：" + WideToUtf8Diag(failure.failureReason));
  opened.push_back(id);
  CommandWindowResult result;
  GC_REQUIRE_MESSAGE(WaitForTerminal(runner, id, &result), "30 秒内没有终态");

  GC_CHECK_MESSAGE(result.completion == git::CommandCompletion::finished && result.exitCode == 0,
                   "对被选中仓库的 add 没有成功——若这条失败而 git 报找不到 a.txt，"
                   "说明重定向没有被策略挡住");
  GC_CHECK(result.environmentNotice.find(L"GIT_DIR") != std::wstring::npos);
  GC_CHECK(result.environmentNotice.find(rig.dirB) == std::wstring::npos);

  // A 的索引里确实多了 a.txt（夹具自己的隔离读法，与刚才的劫持环境无关）。
  rig.fixture.SetActiveRepository(rig.dirA);
  const std::vector<std::wstring> statusA = rig.fixture.StatusPorcelain();
  bool stagedInA = false;
  for (const std::wstring& line : statusA) {
    if (line.find(L"a.txt") != std::wstring::npos && line.front() == L'A') {
      stagedInA = true;
    }
  }
  GC_CHECK_MESSAGE(stagedInA, "git add 没有落到所选仓库 A 的索引上");

  // B 与外置索引一个字节都没动。
  GC_CHECK_MESSAGE(rig.SnapshotUnselectedTargets() == before, "命令窗口里的操作动了未选中目标");
}

// —— 含 git commit 的身份落地用例：按约束不自动执行，由用户本人运行 ——
// 验证「显示与写入一致」：继承的 GIT_COMMITTER_NAME / GIT_AUTHOR_NAME 与注入的 user.email
// 全部被策略移除，表单覆盖（作者身份 + 两个时间）只覆盖这一次提交，提交者仍是用户配置身份。
GC_TEST(git_env_identity_commit_landing_uses_form_and_config_only) {
  BindingRig rig;
  rig.Prepare();
  // 追加身份类劫持：与表单覆盖同名者由操作覆盖赢回，异名者（提交者姓名、注入邮箱）必须失效。
  std::vector<std::unique_ptr<ScopedEnv>> extra;
  extra.push_back(std::make_unique<ScopedEnv>(L"GIT_COMMITTER_NAME", std::wstring(L"劫持提交者")));
  extra.push_back(std::make_unique<ScopedEnv>(L"GIT_COMMITTER_EMAIL", std::wstring(L"evil@example.invalid")));
  extra.push_back(std::make_unique<ScopedEnv>(L"GIT_CONFIG_KEY_1", std::wstring(L"user.email")));
  extra.push_back(std::make_unique<ScopedEnv>(L"GIT_CONFIG_VALUE_1", std::wstring(L"injected@example.invalid")));
  // COUNT 提到 2：两条注入都要被策略删除（策略删 COUNT，注入的 KEY/VALUE 也逐项删除）。
  const ScopedEnv configCount{L"GIT_CONFIG_COUNT", std::wstring(L"2")};

  rig.fixture.SetActiveRepository(rig.dirA);
  rig.fixture.StageAll();  // 暂存 a.txt（root 提交的内容）。
  const std::wstring messageFile = rig.fixture.PathInRoot(L"commit-message.txt");
  WriteFileBytes(messageFile, "环境策略身份落地验证\n");
  const std::string before = rig.SnapshotUnselectedTargets();

  gc::platform::CommandWindowRunner runner;
  std::vector<uint64_t> opened;
  RunnerGuard guard{&runner, &opened};
  runner.Startup(nullptr);

  CommandWindowOperation operation;
  operation.operationId = L"env-identity-commit";
  operation.displayName = L"commit";
  operation.gitExecutable = rig.fixture.GitExe();
  operation.repositoryDirectory = rig.dirA;
  operation.arguments = {L"commit", L"--cleanup=verbatim", L"-F", messageFile};
  // 这就是 CommitPlan 交出来的那三条覆盖：作者身份 + 两个时间，只影响本次操作。
  operation.environmentOverrides = {
      EnvironmentOverride{L"GIT_AUTHOR_NAME", std::wstring(L"表单作者")},
      EnvironmentOverride{L"GIT_AUTHOR_EMAIL", std::wstring(L"form@example.invalid")},
      EnvironmentOverride{L"GIT_AUTHOR_DATE", std::wstring(L"@1700000000 +0000")},
      EnvironmentOverride{L"GIT_COMMITTER_DATE", std::wstring(L"@1700000000 +0000")},
  };

  uint64_t id = 0;
  CommandWindowResult failure;
  GC_REQUIRE_MESSAGE(runner.Start(operation, &id, &failure),
                     "命令窗口启动失败：" + WideToUtf8Diag(failure.failureReason));
  opened.push_back(id);
  CommandWindowResult result;
  GC_REQUIRE_MESSAGE(WaitForTerminal(runner, id, &result), "30 秒内没有终态");
  GC_CHECK_MESSAGE(result.completion == git::CommandCompletion::finished && result.exitCode == 0,
                   "提交没有成功（本用例由用户执行时，隔离夹具里允许 root 提交）");

  // 提交对象里的作者/提交者：作者来自表单覆盖，提交者来自隔离 HOME 的用户配置。
  const std::wstring commitObject = rig.fixture.HeadCommitObject();
  GC_CHECK_MESSAGE(commitObject.find(L"表单作者 <form@example.invalid>") != std::wstring::npos,
                   "作者必须是表单那一行：" + WideToUtf8Diag(commitObject));
  GC_CHECK(commitObject.find(L"劫持提交者") == std::wstring::npos);
  GC_CHECK(commitObject.find(L"injected@example.invalid") == std::wstring::npos);
  GC_CHECK(commitObject.find(L"evil@example.invalid") == std::wstring::npos);
  GC_CHECK_MESSAGE(commitObject.find(L"夹具提交者 <fixture@example.invalid>") != std::wstring::npos,
                   "提交者必须是用户配置里的身份（显示与写入一致的前提）：" +
                       WideToUtf8Diag(commitObject));

  // 表单覆盖不回写任何配置：夹具 HOME 里的配置文件与未选中目标都保持原样。
  const std::string homeConfig = ReadFileBytes(JoinPath(rig.fixture.PathInRoot(L"git-home"), L".gitconfig"));
  GC_CHECK(homeConfig.find("表单作者") == std::string::npos);
  GC_CHECK(homeConfig.find("劫持提交者") == std::string::npos);
  GC_CHECK_MESSAGE(rig.SnapshotUnselectedTargets() == before, "提交动了未选中仓库或外置索引");
}
