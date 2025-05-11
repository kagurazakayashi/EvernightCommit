#include "platform/windows/push_probe.h"

#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId：发问之前先验形态，不把半截 ID 交给 Git
#include "git/first_push_plan.h"
#include "git/pull_plan.h"
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

git::PushPreflightFacts FailedPreflight(std::wstring reason) {
  git::PushPreflightFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

git::FirstPushFacts FailedFirstPushFacts(std::wstring reason) {
  git::FirstPushFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

// 预检里最慢的一条是 `git config --list`（要按 include 叠出全部生效配置），仍然是本地只读查询；
// 沿用工作区读取的放宽值，容纳慢盘与巨型配置。
constexpr unsigned long kPushProbeTimeoutFallbackMs = 20000;

// 首次推送向导的候选清单：最慢的一条同样是 `git config --list`，逐远端的 get-url 都是本地查询。
constexpr unsigned long kFirstPushTargetsTimeoutFallbackMs = 20000;

// 首次推送向导选定目标之后的预检里含只读 ls-remote（要按发布地址访问远端），
// 与推送后的核实同一档放宽值：问不到只落成「问不到」，不因此断言任何一边改了。
constexpr unsigned long kFirstPushProbeTimeoutFallbackMs = 30000;

// 普通推送预检与首次推送预检共用的前四条只读查询：分支 → 才有分支级上游可问、
// HEAD 那份提交、生效配置清单。两个入口问的是同一批事实，判读用的键名与形态也同源，
// 于是「点推送时界面说的那份现状」不会因为走了另一条链路而变成两套说法。
// 各结构的字段按名字搬过去即可，两个判读层各自只采认自己那份。
struct PreflightPrefix {
  git::GitQueryResult symbolicRef;
  git::GitQueryResult headObject;
  git::GitQueryResult upstream;
  bool upstreamRan = false;
  git::GitQueryResult trackingObject;
  bool trackingRan = false;
  git::GitQueryResult configListing;
  std::wstring branchName;
  std::wstring upstreamRemote;
};

PreflightPrefix RunPreflightPrefix(const PushProbeDeps& deps, const std::wstring& exePath,
                                   const std::wstring& dir) {
  PreflightPrefix prefix;
  prefix.symbolicRef = RunQuery(deps.runner, exePath, dir, git::BuildPullSymbolicRefArguments(dir));
  prefix.headObject = RunQuery(deps.runner, exePath, dir, git::BuildPullHeadObjectArguments(dir));

  if (const git::UndoQueryRead symbolic = git::ReadUndoQuery(prefix.symbolicRef);
      symbolic.outcome == git::UndoQueryOutcome::answered &&
      git::PullBranchNameFromRef(symbolic.firstLine, &prefix.branchName)) {
    prefix.upstreamRan = true;
    prefix.upstream =
        RunQuery(deps.runner, exePath, dir, git::BuildPullUpstreamArguments(dir, prefix.branchName));
    if (const git::UndoQueryRead upstream = git::ReadUndoQuery(prefix.upstream);
        upstream.outcome == git::UndoQueryOutcome::answered && !upstream.lines.empty()) {
      const git::PullUpstreamInfo info = git::ParsePullUpstreamLine(upstream.lines.front());
      prefix.upstreamRemote = info.remote;
      if (info.configured) {
        prefix.trackingRan = true;
        prefix.trackingObject =
            RunQuery(deps.runner, exePath, dir,
                     git::BuildPullTrackingObjectArguments(dir, info.trackingRef));
      }
    }
  }
  prefix.configListing =
      RunQuery(deps.runner, exePath, dir, git::BuildPushConfigListingArguments(dir));
  return prefix;
}

// 一条查询压根没能发出去时的占位结果：started=false，判读层因此记成「没问过」而不是「答案为空」。
git::GitQueryResult NotAskedQuery() {
  git::GitQueryResult result;
  result.started = false;
  return result;
}

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
  // 分支与 HEAD 这两条与 pull 的预检同源：同一个约定只留一份实现（参数构造与判读都在 git/pull_plan）。
  // 首次推送向导用同一段前缀，两个入口对「现在的仓库是什么样」的回答因此是同一份。
  PreflightPrefix prefix = RunPreflightPrefix(deps, request.exePath, dir);
  if (StopRequested(request.stopFlag)) {
    return FailedPreflight(L"程序正在退出，预检中止");
  }
  git::PushPreflightQueries queries;
  queries.symbolicRef = std::move(prefix.symbolicRef);
  queries.headObject = std::move(prefix.headObject);
  queries.upstream = std::move(prefix.upstream);
  queries.upstreamRan = prefix.upstreamRan;
  queries.trackingObject = std::move(prefix.trackingObject);
  queries.trackingRan = prefix.trackingRan;
  queries.configListing = std::move(prefix.configListing);
  const std::wstring branchName = prefix.branchName;
  const std::wstring upstreamRemote = prefix.upstreamRemote;

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
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
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
    // 多目标核实的整体退出策略：程序正在退出时，剩下的发布目标按「没问过」如实记录，
    // 不再逐个发起 ls-remote——WM_DESTROY 因此只等当前这一条在途查询，而不是全部目标排完。
    if (StopRequested(request.stopFlag)) {
      git::PushTargetCheck skipped;
      skipped.url = url;
      skipped.queried = false;
      skipped.failure = L"程序正在退出，核对没有对它发出";
      checks.push_back(std::move(skipped));
      continue;
    }
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

git::FirstPushCandidateFacts CollectFirstPushTargets(const FirstPushTargetsRequest& request,
                                                    const PushProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    git::FirstPushCandidateFacts failed;
    failed.configFailure = L"没有可用的仓库工作区根目录，无法确定这个仓库有哪些远端。";
    return failed;
  }
  const std::wstring& dir = request.repositoryDirectory;

  const git::GitQueryResult configListing =
      RunQuery(deps.runner, request.exePath, dir, git::BuildPushConfigListingArguments(dir));
  // 要逐个远端发问，得先知道 Git 列出了哪些名字。这里用的判读函数与
  // InterpretFirstPushCandidates 是同一个、输入也一样，因此不会出现「平台层问了一套顺序、
  // 判读层按另一套顺序配对」的错位；名字形态不合格而发不出查询时补一条占位，保住下标。
  const git::PushConfigListing listing = git::ParsePushConfigListing(configListing);
  std::vector<git::GitQueryResult> urlQueries;
  if (listing.readOk) {
    for (const std::wstring& name : listing.RemoteNames()) {
      // 退出收尾：剩下的远端不再逐个发问，判读层把它们记成「这条查询没发出去」，
      // 而不是「这个远端没有发布地址」。
      if (StopRequested(request.stopFlag)) {
        break;
      }
      const std::vector<std::wstring> arguments = git::BuildPushRemoteUrlArguments(dir, name);
      urlQueries.push_back(arguments.empty()
                               ? NotAskedQuery()
                               : RunQuery(deps.runner, request.exePath, dir, arguments));
    }
  }
  return git::InterpretFirstPushCandidates(configListing, urlQueries);
}

FirstPushTargetsOutcome RunFirstPushTargetsLoad(const FirstPushTargetsRequest& request) {
  FirstPushTargetsOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  const unsigned long timeout = request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds
                                                                 : kFirstPushTargetsTimeoutFallbackMs;
  outcome.candidates = CollectFirstPushTargets(request, MakePushProbeDeps(timeout));
  return outcome;
}

git::FirstPushFacts CollectFirstPushProbe(const FirstPushProbeRequest& request,
                                          const PushProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedFirstPushFacts(L"没有可用的仓库工作区根目录，无法确认这次首次推送属于哪个仓库。");
  }
  const std::wstring& dir = request.repositoryDirectory;

  git::FirstPushProbeQueries queries;
  queries.chosenRemoteName = request.remoteName;
  queries.targetBranchRef = request.targetBranchRef;

  // 现状那四条与「点推送」时普通预检同源：分支 / HEAD 那份提交 / 上游（用来确认「还是没有」）/ 生效配置。
  PreflightPrefix prefix = RunPreflightPrefix(deps, request.exePath, dir);
  if (StopRequested(request.stopFlag)) {
    return FailedFirstPushFacts(L"程序正在退出，预检中止");
  }
  queries.symbolicRef = std::move(prefix.symbolicRef);
  queries.headObject = std::move(prefix.headObject);
  queries.upstream = std::move(prefix.upstream);
  queries.upstreamRan = prefix.upstreamRan;
  queries.configListing = std::move(prefix.configListing);

  const git::PushConfigListing listing = git::ParsePushConfigListing(queries.configListing);
  const std::vector<std::wstring> urlArguments =
      git::BuildPushRemoteUrlArguments(dir, request.remoteName);
  if (!urlArguments.empty()) {
    queries.chosenRemoteUrlRan = true;
    queries.chosenRemoteUrl = RunQuery(deps.runner, request.exePath, dir, urlArguments);
  }

  // 引用名的权威裁定：Git 说不能用就连一条向外问的查询都不发。
  const std::vector<std::wstring> formatArguments =
      git::BuildFirstPushRefFormatArguments(dir, request.targetBranchRef);
  if (!formatArguments.empty()) {
    queries.refFormatRan = true;
    queries.refFormat = RunQuery(deps.runner, request.exePath, dir, formatArguments);
  }
  const bool referenceAccepted =
      queries.refFormatRan &&
      git::ReadUndoQuery(queries.refFormat).outcome == git::UndoQueryOutcome::answered;

  if (referenceAccepted && !StopRequested(request.stopFlag)) {
    // 逐发布地址问「那条引用在不在、停在哪」。这一步只读，但**确实访问远端**：
    // 认证由 Git 自己的方式处理，后台查询带 GIT_TERMINAL_PROMPT=0，问不到就以问不到收场。
    const git::PushUrlResolution resolution =
        git::ResolvePushUrls(queries.chosenRemoteUrl, queries.chosenRemoteUrlRan, listing,
                             request.remoteName);
    for (const std::wstring& url : resolution.urls) {
      if (StopRequested(request.stopFlag)) {
        break;  // 剩下的地址由判读层按「条数对不上」如实记成没问过。
      }
      queries.presenceQueries.push_back(
          RunQuery(deps.runner, request.exePath, dir,
                   git::BuildPushRemoteProbeArguments(dir, url, request.targetBranchRef)));
    }
    std::vector<git::PushTargetCheck> checks;
    checks.reserve(queries.presenceQueries.size());
    for (size_t index = 0; index < queries.presenceQueries.size(); ++index) {
      checks.push_back(git::InterpretPushTargetCheck(resolution.urls[index],
                                                      queries.presenceQueries[index],
                                                      request.targetBranchRef));
    }
    // 领先/落后只在「对端那份位置问得出、各目标答得一致、而且确实与这次要推的不是同一份」时才问：
    // 那个对象本地读不到时 rev-list 会死在 Git 自己的解析上，那种场合结论就是「判断不了」。
    const git::FirstPushPresenceSummary presence = git::SummarizeFirstPushPresence(checks);
    std::wstring headObjectId;
    if (const git::UndoQueryRead head = git::ReadUndoQuery(queries.headObject);
        head.outcome == git::UndoQueryOutcome::answered && git::LooksLikeFullObjectId(head.firstLine)) {
      headObjectId = head.firstLine;
    }
    if (presence.known && presence.exists && !presence.disagrees && !headObjectId.empty() &&
        presence.remoteObjectId != headObjectId) {
      const std::vector<std::wstring> arguments = git::BuildFirstPushRelationshipArguments(
          dir, presence.remoteObjectId, headObjectId);
      if (!arguments.empty()) {
        queries.relationshipRan = true;
        queries.relationship = RunQuery(deps.runner, request.exePath, dir, arguments);
      }
    }
  }

  // 配置文件落点：只是给确认框那句「会写进哪个文件」当依据，问不到不构成拒绝理由。
  if (!StopRequested(request.stopFlag)) {
    queries.configPathRan = true;
    queries.configPath =
        RunQuery(deps.runner, request.exePath, dir, git::BuildFirstPushConfigPathArguments(dir));
  }

  return git::InterpretFirstPushProbe(queries);
}

FirstPushProbeOutcome RunFirstPushProbeLoad(const FirstPushProbeRequest& request) {
  FirstPushProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  const unsigned long timeout =
      request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds : kFirstPushProbeTimeoutFallbackMs;
  outcome.facts = CollectFirstPushProbe(request, MakePushProbeDeps(timeout));
  return outcome;
}

}  // namespace gc::platform
