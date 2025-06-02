#include "support/linux_git_fixture.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/git_query.h"
#include "platform/git_toolchain.h"
#include "platform/linux/subprocess.h"
#include "platform/path.h"
#include "platform/text_encoding.h"
#include "support/push_guard.h"
#include "support/tiny_test.h"

namespace gc::test {
namespace {

constexpr unsigned long kGitTimeoutMs = 20000;
constexpr long long kCommitEpochBase = 1700000000LL;

std::string ToUtf8(std::wstring_view text) { return gc::platform::WideToUtf8(text); }

std::wstring FromUtf8(std::string_view text) { return gc::platform::Utf8ToWide(text); }

void RequireMakeDirs(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  GC_REQUIRE_MESSAGE(!error, "创建目录失败：" + path + "（" + error.message() + "）");
}

}  // namespace

bool LinuxGitFixture::Prepare(std::string& failureReason) {
  if (!temp_.Valid()) {
    failureReason = temp_.FailureReason();
    return false;
  }
  const std::string root = temp_.PathBytes();
  RequireMakeDirs(root + "/home");
  RequireMakeDirs(root + "/git-template");
  RequireWriteFile(root + "/home/empty.gitconfig", "", 0600);

  const std::vector<std::wstring> candidates = gc::platform::DiscoverGitCandidates();
  if (candidates.empty()) {
    failureReason = "PATH 中没有找到可执行的 git";
    return false;
  }
  const gc::platform::GitExeVerification verification =
      gc::platform::VerifyGitExe(candidates.front(), 5000);
  if (verification.outcome != gc::git::GitProbeOutcome::verified) {
    failureReason = ToUtf8(verification.message);
    return false;
  }
  gitExe_ = verification.path;

  std::wstring pathVariable = gc::platform::GetPathVariable();
  std::string pathBytes;
  if (pathVariable.empty() || !gc::platform::TryWideToUtf8Strict(pathVariable, pathBytes)) {
    pathBytes = "/usr/local/bin:/usr/bin:/bin";
  }
  baseEnvironment_ = {
      "PATH=" + pathBytes,
      "HOME=" + root + "/home",
      "XDG_CONFIG_HOME=" + root + "/home/.config",
      "GIT_CONFIG_GLOBAL=" + root + "/home/empty.gitconfig",
      "GIT_CONFIG_NOSYSTEM=1",
      "GIT_TEMPLATE_DIR=" + root + "/git-template",
      "GIT_ALLOW_PROTOCOL=file",
      "TZ=UTC0",
      "LC_ALL=C.UTF-8",
      "GIT_AUTHOR_NAME=测试作者",
      "GIT_AUTHOR_EMAIL=author@example.invalid",
      "GIT_COMMITTER_NAME=测试提交者",
      "GIT_COMMITTER_EMAIL=committer@example.invalid",
  };
  return true;
}

std::wstring LinuxGitFixture::ResolveOwnedDirectory(std::wstring_view path) const {
  std::wstring joined;
  if (gc::platform::IsAbsolutePath(path)) {
    joined = std::wstring(path);
  } else {
    joined = temp_.Path() + L"/" + std::wstring(path);
  }
  const std::wstring folded = gc::platform::ToAbsolutePath(joined);
  GC_REQUIRE_MESSAGE(!folded.empty(), "无法规范化测试路径");
  GC_REQUIRE_MESSAGE(gc::git::PathIsWithin(folded, temp_.Path()),
                     "夹具路径越出临时根：" + ToUtf8(folded));
  return folded;
}

std::wstring LinuxGitFixture::PathInRoot(std::wstring_view relative) const {
  return ResolveOwnedDirectory(relative);
}

gc::platform::ProcessRunResult LinuxGitFixture::RunProcess(
    const std::vector<std::wstring>& arguments, const std::wstring& workingDirectory,
    const std::vector<std::string>& extraEnvironment) const {
  GuardGitPushForTest(arguments, "Linux 夹具");
  gc::platform::ProcessRequest request;
  request.program = gitExe_;
  request.arguments = arguments;
  request.workingDirectory = workingDirectory;
  request.timeoutMilliseconds = kGitTimeoutMs;
  std::vector<std::string> environment = baseEnvironment_;
  environment.insert(environment.end(), extraEnvironment.begin(), extraEnvironment.end());
  return gc::platform::RunCapturedProcess(request, environment);
}

LinuxGitRun LinuxGitFixture::Run(const std::vector<std::wstring>& arguments,
                                 std::wstring_view workingDirectory) {
  const std::wstring directory =
      workingDirectory.empty() ? temp_.Path() : ResolveOwnedDirectory(workingDirectory);
  LinuxGitRun run;
  run.raw = RunProcess(arguments, directory);
  run.out = FromUtf8(run.raw.stdoutCapture.bytes);
  run.err = FromUtf8(run.raw.stderrCapture.bytes);
  return run;
}

LinuxGitRun LinuxGitFixture::RunInRepo(const std::vector<std::wstring>& arguments) {
  GC_REQUIRE_MESSAGE(!repoDir_.empty(), "夹具尚未初始化仓库");
  return Run(arguments, repoDir_);
}

void LinuxGitFixture::RunChecked(const std::vector<std::wstring>& arguments,
                                 std::wstring_view workingDirectory) {
  const LinuxGitRun run = Run(arguments, workingDirectory);
  GC_REQUIRE_MESSAGE(run.raw.started, "Git 未能启动：" + ToUtf8(run.raw.launchErrorText));
  GC_REQUIRE_MESSAGE(run.raw.AllStreamsComplete(),
                     "Git 输出未完整读回：" + ToUtf8(run.raw.stdoutCapture.note) + " / " +
                         ToUtf8(run.raw.stderrCapture.note));
  GC_REQUIRE_MESSAGE(run.raw.termination == gc::platform::ProcessTermination::exited &&
                         run.raw.exitCode == 0,
                     "Git 命令失败（退出码 " + std::to_string(run.raw.exitCode) + "）：" +
                         ToUtf8(run.err));
}

void LinuxGitFixture::RunCheckedInRepo(const std::vector<std::wstring>& arguments) {
  GC_REQUIRE_MESSAGE(!repoDir_.empty(), "夹具尚未初始化仓库");
  RunChecked(arguments, repoDir_);
}

void LinuxGitFixture::InitRepository(std::wstring_view directoryName, bool bare) {
  const std::wstring directory = PathInRoot(directoryName);
  const std::string bytes = ToUtf8(directory);
  RequireMakeDirs(bytes);
  std::vector<std::wstring> arguments = {L"init", L"--quiet", L"-b", L"main", L"."};
  if (bare) {
    arguments = {L"init", L"--quiet", L"--bare", L"-b", L"main", L"."};
  }
  RunChecked(arguments, directory);
  repoDir_ = directory;
  if (!bare) {
    RunChecked({L"config", L"core.quotepath", L"off"}, repoDir_);
    RunChecked({L"config", L"core.autocrlf", L"false"}, repoDir_);
    RunChecked({L"config", L"core.fileMode", L"true"}, repoDir_);
  }
}

void LinuxGitFixture::SetActiveRepository(std::wstring_view directory) {
  repoDir_ = ResolveOwnedDirectory(directory);
}

void LinuxGitFixture::WriteFile(std::wstring_view repositoryRelativePath,
                                const std::string& utf8Content, bool stage) {
  GC_REQUIRE_MESSAGE(!repoDir_.empty(), "夹具尚未初始化仓库");
  const std::wstring path = repoDir_ + L"/" + std::wstring(repositoryRelativePath);
  const std::string bytes = ToUtf8(path);
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(bytes).parent_path(), error);
  RequireWriteFile(bytes, utf8Content, 0644);
  if (stage) {
    RunCheckedInRepo({L"add", L"--", std::wstring(repositoryRelativePath)});
  }
}

void LinuxGitFixture::StageAll() { RunCheckedInRepo({L"add", L"-A"}); }

void LinuxGitFixture::Commit(std::wstring_view subject) {
  ++commitSequence_;
  const long long epoch = kCommitEpochBase + commitSequence_ * 60;
  const std::string date = std::to_string(epoch) + " +0000";
  const LinuxGitRun run = [&] {
    GuardGitPushForTest({L"commit"}, "Linux 夹具");
    gc::platform::ProcessRequest request;
    request.program = gitExe_;
    request.arguments = {L"commit", L"--quiet", L"-m", std::wstring(subject)};
    request.workingDirectory = repoDir_;
    request.timeoutMilliseconds = kGitTimeoutMs;
    std::vector<std::string> environment = baseEnvironment_;
    environment.push_back("GIT_AUTHOR_DATE=" + date);
    environment.push_back("GIT_COMMITTER_DATE=" + date);
    LinuxGitRun result;
    result.raw = gc::platform::RunCapturedProcess(request, environment);
    result.out = FromUtf8(result.raw.stdoutCapture.bytes);
    result.err = FromUtf8(result.raw.stderrCapture.bytes);
    return result;
  }();
  GC_REQUIRE_MESSAGE(run.raw.started && run.raw.termination == gc::platform::ProcessTermination::exited &&
                         run.raw.exitCode == 0,
                     "git commit 失败：" + ToUtf8(run.err));
}

std::wstring LinuxGitFixture::HeadSha() {
  const LinuxGitRun run = RunInRepo({L"rev-parse", L"--verify", L"--quiet", L"HEAD"});
  std::wstring text = run.out;
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
    text.pop_back();
  }
  return text;
}

std::wstring LinuxGitFixture::HeadSubject() {
  const LinuxGitRun run = RunInRepo({L"log", L"-1", L"--format=%s"});
  std::wstring text = run.out;
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
    text.pop_back();
  }
  return text;
}

gc::git::GitQueryRunner LinuxGitFixture::MakeRunner() const {
  return [this](const std::wstring& exePath, const std::wstring& directory,
                const std::vector<std::wstring>& arguments) {
    return Query(exePath, arguments, directory, kGitTimeoutMs);
  };
}

gc::git::GitQueryResult LinuxGitFixture::Query(const std::wstring& exePath,
                                               const std::vector<std::wstring>& arguments,
                                               const std::wstring& directory,
                                               unsigned long timeoutMilliseconds,
                                               gc::platform::StopFlag cancel) const {
  GuardGitPushForTest(arguments, "Linux 夹具查询");
  gc::platform::ProcessRequest request;
  request.program = exePath;
  request.arguments = arguments;
  request.workingDirectory = directory;
  request.timeoutMilliseconds = timeoutMilliseconds;
  request.cancel = std::move(cancel);
  const gc::platform::ProcessRunResult run =
      gc::platform::RunCapturedProcess(request, baseEnvironment_);
  return gc::platform::MakeGitQueryResult(run);
}

gc::platform::RepoDetectDeps LinuxGitFixture::MakeDetectDeps() {
  gc::platform::RepoDetectDeps deps;
  deps.runner = MakeRunner();
  deps.absolutize = [](const std::wstring& directory, std::wstring_view relative) {
    return gc::platform::ToAbsolutePathInDirectory(directory, relative);
  };
  deps.slashify = [](std::wstring_view path) { return gc::platform::NormalizePathSeparators(path); };
  deps.directoryExists = [](const std::wstring& path) { return gc::platform::IsExistingDirectory(path); };
  deps.gitExeIsFile = [](const std::wstring& path) {
    return gc::platform::IsExistingRegularFile(path);
  };
  return deps;
}

gc::platform::WorkspaceStatusDeps LinuxGitFixture::MakeStatusDeps() {
  gc::platform::WorkspaceStatusDeps deps;
  deps.runner = MakeRunner();
  return deps;
}

gc::platform::AuthorConfigDeps LinuxGitFixture::MakeAuthorConfigDepsForTest() {
  gc::platform::AuthorConfigDeps deps;
  deps.runner = MakeRunner();
  return deps;
}

gc::platform::CommitProbeDeps LinuxGitFixture::MakeCommitProbeDepsForTest() {
  gc::platform::CommitProbeDeps deps;
  deps.runner = MakeRunner();
  return deps;
}

gc::platform::CommitDetailDeps LinuxGitFixture::MakeCommitDetailDepsForTest() {
  gc::platform::CommitDetailDeps deps;
  deps.runner = MakeRunner();
  return deps;
}

gc::platform::CommitChangesDeps LinuxGitFixture::MakeCommitChangesDepsForTest() {
  gc::platform::CommitChangesDeps deps;
  deps.runner = MakeRunner();
  return deps;
}

}  // namespace gc::test
