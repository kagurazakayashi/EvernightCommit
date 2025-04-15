#include "platform/windows/undo_probe.h"

#include <vector>

#include "git/workspace_status.h"
#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"

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

git::UndoPreflightFacts FailedFacts(std::wstring reason) {
  git::UndoPreflightFacts facts;
  facts.head.queryFailure = std::move(reason);
  facts.statusDetail = L"预检未能发起";
  return facts;
}

}  // namespace

git::UndoPreflightFacts CollectUndoPreflight(const UndoProbeRequest& request,
                                             const UndoProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedFacts(L"没有可用的仓库工作区根目录");
  }
  if (!deps.runner) {
    return FailedFacts(L"没有可用的 Git 程序");
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::UndoPreflightQueries queries;
  queries.symbolicRef = RunQuery(deps.runner, request.exePath, dir,
                                 git::BuildUndoSymbolicRefArguments(dir));
  queries.headCommit =
      RunQuery(deps.runner, request.exePath, dir, git::BuildUndoHeadCommitArguments(dir));

  // HEAD 可解析才追问父提交、标题与远端包含：那种查询在空仓库里必然非 0 退出，
  // 不是错误却要解释；而且没有 HEAD ID 也没有可查询的目标。
  const git::UndoQueryRead headRead = git::ReadUndoQuery(queries.headCommit);
  if (headRead.outcome == git::UndoQueryOutcome::answered &&
      !git::BuildUndoRemoteContainsArguments(dir, headRead.firstLine).empty()) {
    queries.commitDependentRan = true;
    queries.parents = RunQuery(deps.runner, request.exePath, dir, git::BuildUndoParentsArguments(dir));
    queries.headSummary =
        RunQuery(deps.runner, request.exePath, dir, git::BuildUndoHeadSummaryArguments(dir));
    queries.remoteRefs = RunQuery(deps.runner, request.exePath, dir, git::BuildUndoRemoteRefsArguments(dir));
    queries.remoteContains = RunQuery(deps.runner, request.exePath, dir,
                                      git::BuildUndoRemoteContainsArguments(dir, headRead.firstLine));
  }
  queries.status = RunQuery(deps.runner, request.exePath, dir, git::BuildWorkspaceStatusArguments(dir));
  return git::InterpretUndoPreflight(queries);
}

UndoProbeDeps MakeUndoProbeDeps(unsigned long timeoutMilliseconds) {
  UndoProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    const SubprocessRunResult run = RunHiddenCaptured(exePath, arguments, directory, timeoutMilliseconds);
    git::GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = Utf8ToUtf16(run.utf8Stdout);
    result.utf16Error = Utf8ToUtf16(run.utf8Stderr);
    return result;
  };
  return deps;
}

git::UndoHeadFacts CaptureUndoHeadSnapshot(const std::wstring& exePath,
                                           const std::wstring& repositoryDirectory,
                                           unsigned long timeoutMilliseconds) {
  UndoProbeDeps deps = MakeUndoProbeDeps(timeoutMilliseconds);
  const std::wstring dir = repositoryDirectory;
  const git::GitQueryResult symbolicRef =
      RunQuery(deps.runner, exePath, dir, git::BuildUndoSymbolicRefArguments(dir));
  const git::GitQueryResult headCommit =
      RunQuery(deps.runner, exePath, dir, git::BuildUndoHeadCommitArguments(dir));
  return git::InterpretUndoHeadSnapshot(symbolicRef, headCommit);
}

UndoProbeOutcome RunUndoProbeLoad(const UndoProbeRequest& request) {
  UndoProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  outcome.facts = CollectUndoPreflight(request, MakeUndoProbeDeps(request.timeoutMilliseconds));
  return outcome;
}

}  // namespace gc::platform
