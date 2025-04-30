#include "platform/windows/commit_probe.h"

#include <vector>

#include "git/author_config.h"
#include "git/repository.h"
#include "platform/windows/git_query_result.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

git::GitQueryResult RunQuery(const git::GitQueryRunner& runner, const std::wstring& exePath,
                             const std::wstring& directory,
                             const std::vector<std::wstring>& arguments) {
  if (!runner || arguments.empty()) {
    git::GitQueryResult failed;
    failed.started = false;
    return failed;
  }
  return runner(exePath, directory, arguments);
}

git::CommitIdentityFacts FailedIdentity(std::wstring reason) {
  git::CommitIdentityFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

// 预检里最慢的一条是 write-tree（大仓库要现算若干棵树），量级与 git status 相当：
// 沿用工作区读取的 20 秒放宽值。超时一律按「这份事实没读回来」放弃这次提交，
// 绝不退回用旧事实确认，也绝不自动重试。
constexpr unsigned long kCommitProbeTimeoutFallbackMs = 20000;

std::wstring FirstAnsweredLine(const git::GitQueryResult& result) {
  for (const std::wstring_view line : git::SplitLines(result.utf16Output)) {
    const std::wstring trimmed = git::TrimWide(line);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return {};
}

}  // namespace

git::CommitIdentityFacts CollectCommitPreflight(const CommitProbeRequest& request,
                                                const CommitProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedIdentity(L"没有可用的仓库工作区根目录，预检没有可绑定的查询落点。");
  }
  if (!deps.runner) {
    return FailedIdentity(L"没有可用的 Git 程序，预检无法发起任何查询。");
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::CommitIdentityQueries queries;
  queries.requestedRepositoryDirectory = dir;
  queries.worktree = RunQuery(deps.runner, request.exePath, dir, git::BuildCommitWorktreeArguments(dir));
  queries.branchRef = RunQuery(deps.runner, request.exePath, dir, git::BuildCommitBranchRefArguments(dir));
  queries.headObject =
      RunQuery(deps.runner, request.exePath, dir, git::BuildCommitHeadObjectArguments(dir));
  // 退出收尾：停止信号已起就不再往后问（含会写对象库的 write-tree，这一趟结果随窗口一起作废）。
  if (StopRequested(request.stopFlag)) {
    return FailedIdentity(L"程序正在退出，预检中止。");
  }
  // 索引内容标识。这一条是整组查询里唯一会写对象库的（其余几条纯读），副作用见头文件说明。
  queries.indexTree =
      RunQuery(deps.runner, request.exePath, dir, git::BuildCommitIndexTreeArguments(dir));
  // 提交者身份的两项各问一条：合并成一条问法在 Git 2.56 不成立（见 git/author_config）。
  queries.configName = RunQuery(deps.runner, request.exePath, dir,
                                git::BuildIdentityConfigArguments(dir, git::kCommitCommitterConfigKeys[0]));
  queries.configEmail = RunQuery(deps.runner, request.exePath, dir,
                                 git::BuildIdentityConfigArguments(dir, git::kCommitCommitterConfigKeys[1]));

  // 流程痕迹的落点是这一趟刚问到的 Git 目录，不是界面早前存下的那一份：链接工作树的状态档案
  // 落在各自的工作树目录里，拿错目录探就等于用别人的痕迹给这次提交放行。
  // 目录本身没问出来时连「没有痕迹」都不说，只标成「没探过」——判读层据此把整份事实当不可信。
  const bool worktreeAnswered = queries.worktree.exitCode == 0 && queries.worktree.exited &&
                                !queries.worktree.timedOut && queries.worktree.outputComplete;
  if (worktreeAnswered) {
    const std::wstring gitDir = FirstAnsweredLine(queries.worktree);
    if (!gitDir.empty()) {
      queries.workflow = ProbeRepositoryWorkflowState(gitDir);
      queries.workflowProbed = true;
    }
  }
  return git::InterpretCommitIdentity(queries);
}

CommitProbeDeps MakeCommitProbeDeps(unsigned long timeoutMilliseconds) {
  CommitProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  return deps;
}

CommitProbeOutcome RunCommitProbeLoad(const CommitProbeRequest& request) {
  CommitProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：换仓库的判别在界面线程做。
  const unsigned long timeout =
      request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds : kCommitProbeTimeoutFallbackMs;
  outcome.facts = CollectCommitPreflight(request, MakeCommitProbeDeps(timeout));
  return outcome;
}

}  // namespace gc::platform
