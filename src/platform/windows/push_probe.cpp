#include "platform/windows/push_probe.h"

#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId：发问之前先验形态，不把半截 ID 交给 Git
#include "git/pull_plan.h"
#include "platform/windows/git_query_result.h"
#include "platform/windows/subprocess.h"
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

git::PushPreflightFacts FailedPreflight(std::wstring reason) {
  git::PushPreflightFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

// 预检里最慢的一条是 `git config --list`（要按 include 叠出全部生效配置），仍然是本地只读查询；
// 沿用工作区读取的放宽值，容纳慢盘与巨型配置。
constexpr unsigned long kPushProbeTimeoutFallbackMs = 20000;

}  // namespace

git::PushPreflightFacts CollectPushPreflight(const PushProbeRequest& request,
                                             const PushProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedPreflight(L"没有可用的仓库工作区根目录");
  }
  if (!deps.runner) {
    return FailedPreflight(L"没有可用的 Git 程序");
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::PushPreflightQueries queries;
  // 分支与 HEAD 这两条与 pull 的预检同源：同一个约定只留一份实现（参数构造与判读都在 git/pull_plan）。
  queries.symbolicRef = RunQuery(deps.runner, request.exePath, dir, git::BuildPullSymbolicRefArguments(dir));
  queries.headObject =
      RunQuery(deps.runner, request.exePath, dir, git::BuildPullHeadObjectArguments(dir));

  std::wstring branchName;
  std::wstring upstreamRemote;
  std::wstring trackingRef;
  if (const git::UndoQueryRead symbolic = git::ReadUndoQuery(queries.symbolicRef);
      symbolic.outcome == git::UndoQueryOutcome::answered &&
      git::PullBranchNameFromRef(symbolic.firstLine, &branchName)) {
    queries.upstreamRan = true;
    queries.upstream =
        RunQuery(deps.runner, request.exePath, dir, git::BuildPullUpstreamArguments(dir, branchName));
    if (const git::UndoQueryRead upstreamRead = git::ReadUndoQuery(queries.upstream);
        upstreamRead.outcome == git::UndoQueryOutcome::answered && !upstreamRead.lines.empty()) {
      const git::PullUpstreamInfo info = git::ParsePullUpstreamLine(upstreamRead.lines.front());
      upstreamRemote = info.remote;
      trackingRef = info.trackingRef;
      if (info.configured) {
        queries.trackingRan = true;
        queries.trackingObject =
            RunQuery(deps.runner, request.exePath, dir,
                     git::BuildPullTrackingObjectArguments(dir, info.trackingRef));
      }
    }
  }

  queries.configListing =
      RunQuery(deps.runner, request.exePath, dir, git::BuildPushConfigListingArguments(dir));

  // 「实际推给哪个远端」要由这份配置清单裁决定出来，才能接着问那个远端的发布 URL——
  // 这一步的裁决与判读层用的是同一个函数，界面与判读因此不会各算一套目标。
  const git::PushRemoteChoice choice =
      git::ResolvePushRemoteFromQueries(queries.configListing, branchName, upstreamRemote);
  if (!choice.remoteName.empty()) {
    const std::vector<std::wstring> arguments =
        git::BuildPushRemoteUrlArguments(dir, choice.remoteName);
    if (!arguments.empty()) {
      queries.remoteUrlRan = true;
      queries.remoteUrl = RunQuery(deps.runner, request.exePath, dir, arguments);
    }
  }

  // 领先/落后只在两侧的位置都问得出来时才谈：HEAD 或跟踪引用缺一个，这个问题就不成立，
  // 判读层会把它记成「没能问出」而不是「零个」，方案层据此不提那个数字。
  if (const git::UndoQueryRead head = git::ReadUndoQuery(queries.headObject);
      head.outcome == git::UndoQueryOutcome::answered && git::LooksLikeFullObjectId(head.firstLine)) {
    if (const git::UndoQueryRead tracking = git::ReadUndoQuery(queries.trackingObject);
        tracking.outcome == git::UndoQueryOutcome::answered &&
        git::LooksLikeFullObjectId(tracking.firstLine)) {
      queries.aheadBehindRan = true;
      queries.aheadBehind = RunQuery(deps.runner, request.exePath, dir,
                                     git::BuildPullAheadBehindArguments(dir, head.firstLine,
                                                                       tracking.firstLine));
    }
  }

  // 流程痕迹只是给「仓库里还有一次合并没走完」那句说明用的依据：推送不动索引与工作区，
  // 因此它不构成拒绝理由（判读层把它当成事实搬进 facts，由方案层如实交代）。
  if (!request.absoluteGitDir.empty()) {
    queries.workflow = ProbeRepositoryWorkflowState(request.absoluteGitDir);
    queries.workflowProbed = true;
  }

  return git::InterpretPushPreflight(queries);
}

PushProbeDeps MakePushProbeDeps(unsigned long timeoutMilliseconds) {
  PushProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return MakeGitQueryResult(RunHiddenCaptured(exePath, arguments, directory, timeoutMilliseconds));
  };
  return deps;
}

PushProbeOutcome RunPushProbeLoad(const PushProbeRequest& request) {
  PushProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  const unsigned long timeout =
      request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds : kPushProbeTimeoutFallbackMs;
  outcome.facts = CollectPushPreflight(request, MakePushProbeDeps(timeout));
  return outcome;
}

PushVerifyOutcome RunPushVerifyLoad(const PushVerifyRequest& request) {
  PushVerifyOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;
  const unsigned long timeout =
      request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds : kPushProbeTimeoutFallbackMs;
  const PushProbeDeps deps = MakePushProbeDeps(timeout);

  std::vector<git::PushTargetCheck> checks;
  checks.reserve(request.pushUrls.size());
  for (const std::wstring& url : request.pushUrls) {
    const std::vector<std::wstring> arguments =
        git::BuildPushRemoteProbeArguments(request.repositoryDirectory, url, request.remoteBranchRef);
    // 参数构造期就拒绝（例如引用名形态不合格）时不留「没问过」的空档：直接记一条问不到的结果，
    // 报告里说得出是没问而不是问失败。
    if (arguments.empty()) {
      git::PushTargetCheck skipped;
      skipped.url = url;
      skipped.queried = false;
      skipped.failure = L"这个目标地址或目标引用的形态不合格，核对没有对它发出";
      checks.push_back(std::move(skipped));
      continue;
    }
    const git::GitQueryResult result =
        RunQuery(deps.runner, request.exePath, request.repositoryDirectory, arguments);
    checks.push_back(git::InterpretPushTargetCheck(url, result, request.remoteBranchRef));
  }
  outcome.report = git::ComposePushVerification(checks, request.expectedObjectId,
                                                request.remoteBranchRef, request.pushCommandSucceeded,
                                                request.commandConclusion);
  outcome.pushCommandSucceeded = request.pushCommandSucceeded;
  return outcome;
}

}  // namespace gc::platform
