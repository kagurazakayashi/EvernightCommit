#include "platform/windows/fetch_probe.h"

#include <vector>

#include "platform/windows/git_query_result.h"

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

git::FetchTargetFacts FailedFacts(std::wstring reason) {
  git::FetchTargetFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

}  // namespace

git::FetchTargetFacts CollectFetchTarget(const FetchProbeRequest& request, const FetchProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedFacts(L"没有可用的仓库工作区根目录");
  }
  if (!deps.runner) {
    return FailedFacts(L"没有可用的 Git 程序");
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::FetchTargetQueries queries;
  queries.symbolicRef = RunQuery(deps.runner, request.exePath, dir,
                                 git::BuildFetchSymbolicRefArguments(dir));

  // symbolic-ref  answered 且给出 refs/heads/ 才追问分支配置的远端：
  // 键名里的分支名必须由 Git 自己给出，游离 HEAD 根本没有那样一条配置可问。
  const git::UndoQueryRead symbolic = git::ReadUndoQuery(queries.symbolicRef);
  std::wstring branchName;
  if (symbolic.outcome == git::UndoQueryOutcome::answered) {
    constexpr std::wstring_view prefix = L"refs/heads/";
    const std::wstring_view ref = symbolic.firstLine;
    if (ref.size() > prefix.size() && ref.compare(0, prefix.size(), prefix) == 0) {
      branchName = std::wstring(ref.substr(prefix.size()));
    }
  }
  const std::vector<std::wstring> branchRemoteArguments =
      git::BuildFetchBranchRemoteArguments(dir, branchName);
  if (!branchRemoteArguments.empty()) {
    queries.branchRemoteRan = true;
    queries.branchRemote =
        RunQuery(deps.runner, request.exePath, dir, branchRemoteArguments);
  }

  queries.remotes =
      RunQuery(deps.runner, request.exePath, dir, git::BuildFetchRemotesArguments(dir));
  // 影响抓取范围的两条配置查询（远端级与全局级）：与 pull 第一步用的是 git/fetch_scope
  // 里同一份参数构造，两个入口的范围口径因此不可能分叉。
  queries.scope.remoteConfig =
      RunQuery(deps.runner, request.exePath, dir, git::BuildFetchScopeRemoteConfigArguments(dir));
  queries.scope.globalConfig =
      RunQuery(deps.runner, request.exePath, dir, git::BuildFetchScopeGlobalConfigArguments(dir));
  return git::InterpretFetchTarget(queries);
}

FetchProbeDeps MakeFetchProbeDeps(unsigned long timeoutMilliseconds) {
  FetchProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  return deps;
}

FetchProbeOutcome RunFetchProbeLoad(const FetchProbeRequest& request) {
  FetchProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  outcome.facts = CollectFetchTarget(request, MakeFetchProbeDeps(request.timeoutMilliseconds));
  return outcome;
}

}  // namespace gc::platform
