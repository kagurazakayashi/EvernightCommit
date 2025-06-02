#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/repository.h"
#include "platform/author_config.h"
#include "platform/commit_changes.h"
#include "platform/commit_detail.h"
#include "platform/commit_probe.h"
#include "platform/process.h"
#include "platform/repo_detect.h"
#include "platform/workspace_status.h"
#include "support/posix_test_support.h"

namespace gc::test {

// 一次 Linux 夹具执行的结果：保留原始进程事实，另附宽松解码的两路文本供诊断。
struct LinuxGitRun {
  gc::platform::ProcessRunResult raw;
  std::wstring out;
  std::wstring err;

  [[nodiscard]] bool Success() const noexcept {
    return raw.started && raw.termination == gc::platform::ProcessTermination::exited &&
           raw.exitCode == 0 && raw.AllStreamsComplete();
  }
};

// Linux 隔离 Git 夹具：
//   * 每个用例独占一个 PosixTempDirectory；
//   * 子进程环境从零构造，只含夹具显式给出的项：HOME/XDG/GIT_CONFIG_GLOBAL/
//     GIT_CONFIG_NOSYSTEM/GIT_TEMPLATE_DIR 全部落在临时根里，用户配置、hooks、凭据助手、
//     别名与系统配置都不可见；GIT_ALLOW_PROTOCOL=file 让任何网络 URL 在连接前就被拒绝；
//   * Git 程序由 PATH 发现，并用生产的 VerifyGitExe（真实 `--version`）验证；
//   * 所有命令都显式绑定工作目录且必须位于临时根内，并经过 push 闸门：
//     夹具只能用来看、提交与本地造对象，绝不推送。
class LinuxGitFixture {
public:
  LinuxGitFixture() = default;
  LinuxGitFixture(const LinuxGitFixture&) = delete;
  LinuxGitFixture& operator=(const LinuxGitFixture&) = delete;

  bool Prepare(std::string& failureReason);

  [[nodiscard]] const std::wstring& GitExe() const noexcept { return gitExe_; }
  [[nodiscard]] std::wstring Root() const { return temp_.Path(); }
  [[nodiscard]] const std::wstring& RepoDir() const noexcept { return repoDir_; }

  [[nodiscard]] std::wstring PathInRoot(std::wstring_view relative) const;
  [[nodiscard]] LinuxGitRun Run(const std::vector<std::wstring>& arguments,
                                std::wstring_view workingDirectory);
  [[nodiscard]] LinuxGitRun RunInRepo(const std::vector<std::wstring>& arguments);
  void RunChecked(const std::vector<std::wstring>& arguments, std::wstring_view workingDirectory);
  void RunCheckedInRepo(const std::vector<std::wstring>& arguments);

  void InitRepository(std::wstring_view directoryName = L"repo", bool bare = false);
  void SetActiveRepository(std::wstring_view directory);
  void WriteFile(std::wstring_view repositoryRelativePath, const std::string& utf8Content,
                 bool stage = false);
  void StageAll();
  void Commit(std::wstring_view subject);

  [[nodiscard]] std::wstring HeadSha();
  [[nodiscard]] std::wstring HeadSubject();

  // 用夹具的隔离环境构造生产编排依赖：问哪几条、怎么解析仍由生产代码决定。
  [[nodiscard]] gc::platform::RepoDetectDeps MakeDetectDeps();
  [[nodiscard]] gc::platform::WorkspaceStatusDeps MakeStatusDeps();
  [[nodiscard]] gc::platform::AuthorConfigDeps MakeAuthorConfigDepsForTest();
  [[nodiscard]] gc::platform::CommitProbeDeps MakeCommitProbeDepsForTest();
  [[nodiscard]] gc::platform::CommitDetailDeps MakeCommitDetailDepsForTest();
  [[nodiscard]] gc::platform::CommitChangesDeps MakeCommitChangesDepsForTest();

  // 直接以夹具隔离环境执行一次生产完整度判定（用于验证大输出/超时/启动失败）。
  [[nodiscard]] gc::git::GitQueryResult Query(const std::wstring& exePath,
                                              const std::vector<std::wstring>& arguments,
                                              const std::wstring& directory,
                                              unsigned long timeoutMilliseconds,
                                              gc::platform::StopFlag cancel = {}) const;

private:
  [[nodiscard]] gc::platform::ProcessRunResult RunProcess(
      const std::vector<std::wstring>& arguments, const std::wstring& workingDirectory,
      const std::vector<std::string>& extraEnvironment = {}) const;
  [[nodiscard]] std::wstring ResolveOwnedDirectory(std::wstring_view path) const;
  [[nodiscard]] gc::git::GitQueryRunner MakeRunner() const;

  PosixTempDirectory temp_;
  std::wstring gitExe_;
  std::wstring repoDir_;
  std::vector<std::string> baseEnvironment_;
  long commitSequence_ = 0;
};

}  // namespace gc::test
