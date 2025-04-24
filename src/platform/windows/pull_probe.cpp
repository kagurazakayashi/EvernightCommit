#include "platform/windows/pull_probe.h"

#include <vector>

#include "git/workspace_status.h"
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

git::PullTargetFacts FailedTarget(std::wstring reason) {
  git::PullTargetFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

git::PullRelationshipFacts FailedRelationship(std::wstring reason) {
  git::PullRelationshipFacts facts;
  facts.queryOk = false;
  facts.queryFailure = std::move(reason);
  return facts;
}

// 一次 pull 预检里所有本地只读查询共用的超时：状态那一条最慢（要展开未跟踪目录），
// 沿用工作区读取的放宽值；其余几条都是毫秒级。
constexpr unsigned long kPullStatusTimeoutFallbackMs = 20000;

}  // namespace

git::PullTargetFacts CollectPullTarget(const PullProbeRequest& request, const PullProbeDeps& deps) {
  if (request.repositoryDirectory.empty()) {
    return FailedTarget(L"没有可用的仓库工作区根目录");
  }
  if (!deps.runner) {
    return FailedTarget(L"没有可用的 Git 程序");
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::PullTargetQueries queries;
  queries.symbolicRef = RunQuery(deps.runner, request.exePath, dir, git::BuildPullSymbolicRefArguments(dir));
  queries.headObject =
      RunQuery(deps.runner, request.exePath, dir, git::BuildPullHeadObjectArguments(dir));

  // 分支名由 Git 自己给出（symbolic-ref），游离 HEAD 根本没有分支记录可问上游，
  // 也就拼不出 branch.〈名〉.rebase 这样的键——那几条查询在问之前就确定不用发。
  std::wstring branchName;
  if (const git::UndoQueryRead symbolic = git::ReadUndoQuery(queries.symbolicRef);
      symbolic.outcome == git::UndoQueryOutcome::answered &&
      git::PullBranchNameFromRef(symbolic.firstLine, &branchName)) {
    queries.upstreamRan = true;
    queries.upstream =
        RunQuery(deps.runner, request.exePath, dir, git::BuildPullUpstreamArguments(dir, branchName));

    // 上游问得到才继续问「那个跟踪引用停在哪」「分支级 rebase」：前一步没成立时后面没有问题可问。
    if (const git::UndoQueryRead upstreamRead = git::ReadUndoQuery(queries.upstream);
        upstreamRead.outcome == git::UndoQueryOutcome::answered && !upstreamRead.lines.empty()) {
      const git::PullUpstreamInfo info = git::ParsePullUpstreamLine(upstreamRead.lines.front());
      if (info.configured) {
        queries.trackingObjectRan = true;
        queries.trackingObject =
            RunQuery(deps.runner, request.exePath, dir, git::BuildPullTrackingObjectArguments(dir,
                                                                                              info.trackingRef));
      }
      if (!branchName.empty()) {
        queries.configBranchRebaseRan = true;
        queries.configBranchRebase = RunQuery(deps.runner, request.exePath, dir,
                                              git::BuildPullConfigArguments(
                                                  dir, L"branch." + branchName + L".rebase"));
      }
    }
  }

  for (const std::wstring_view key : {L"pull.rebase", L"pull.ff", L"merge.ff"}) {
    // 「没设」与「设成了什么」是两回事，四条 config 都得问一句，由判读层按退出码区分。
    const std::vector<std::wstring> arguments = git::BuildPullConfigArguments(dir, key);
    const git::GitQueryResult result = RunQuery(deps.runner, request.exePath, dir, arguments);
    if (key == L"pull.rebase") {
      queries.configPullRebase = result;
    } else if (key == L"pull.ff") {
      queries.configPullFf = result;
    } else {
      queries.configMergeFf = result;
    }
  }

  // 工作区/索引现状与界面列表、撤回预检用的是同一条查询与同一套解析：
  // 「这次整合会不会盖到你没提交的东西上」判的就是这份现状，不能在这里另发明一种定义。
  queries.statusRan = true;
  queries.status = RunQuery(deps.runner, request.exePath, dir, git::BuildWorkspaceStatusArguments(dir));

  // 流程痕迹的依据是 Git 目录里的档案（MERGE_HEAD、rebase-merge\ …），与创建提交/撤回同一來源：
  // 正在走 merge/rebase 的仓库不能叠一次 pull，而这既不是查询也不是命令能问出来的。
  if (!request.absoluteGitDir.empty()) {
    queries.workflow = ProbeRepositoryWorkflowState(request.absoluteGitDir);
    queries.workflowProbed = true;
  }

  return git::InterpretPullTarget(queries);
}

git::PullRelationshipFacts CollectPullRelationship(const git::PullTargetFacts& target,
                                                   const PullProbeRequest& request,
                                                   const PullProbeDeps& deps) {
  if (!target.queryOk) {
    return FailedRelationship(L"阶段一的预检没拿到可采信的事实，关系查询无从发起。");
  }
  if (!target.headResolved || !target.trackingResolved) {
    return FailedRelationship(L"本地 HEAD 或远端跟踪引用的位置没问出来，无法核对本地与远端的关系。");
  }
  if (!deps.runner || request.repositoryDirectory.empty()) {
    return FailedRelationship(L"没有可用的 Git 程序或仓库目录");
  }

  const std::wstring& dir = request.repositoryDirectory;
  const std::wstring& head = target.headObjectId;
  const std::wstring& tracking = target.trackingObjectId;

  // 第一趟只问「各有几个独有提交」：关系判出来了，才知道后面要不要做合并式冲突预演
  // （可快进时内容不可能冲突，预演是白做；分叉时才需要）。
  git::PullRelationshipQueries firstPass;
  firstPass.aheadBehind = RunQuery(deps.runner, request.exePath, dir,
                                   git::BuildPullAheadBehindArguments(dir, head, tracking));
  const git::PullRelationshipFacts counted = git::InterpretPullRelationship(firstPass);
  if (!counted.queryOk) {
    return counted;
  }

  git::PullRelationshipQueries queries;
  queries.aheadBehind = firstPass.aheadBehind;
  queries.mergeBaseRan = true;
  queries.mergeBase =
      RunQuery(deps.runner, request.exePath, dir, git::BuildPullMergeBaseArguments(dir, head, tracking));
  if (const git::UndoQueryRead base = git::ReadUndoQuery(queries.mergeBase);
      base.outcome == git::UndoQueryOutcome::answered) {
    queries.incomingRan = true;
    queries.incoming = RunQuery(deps.runner, request.exePath, dir,
                                git::BuildPullIncomingArguments(dir, base.firstLine, tracking));
  }
  if (counted.relationship == git::PullRelationship::diverged) {
    // 冲突预演在此刻还不知道最终会选合并还是变基（选择框还没弹）。它只是把「合并路线上
    // 会冲突在哪」如实测出来备着：判读层给出结果，方案层在用户选了变基时明确不使用它。
    queries.mergeTreeRan = true;
    queries.mergeTree =
        RunQuery(deps.runner, request.exePath, dir, git::BuildPullMergeTreeArguments(dir, head, tracking));
  }
  return git::InterpretPullRelationship(queries);
}

PullProbeDeps MakePullProbeDeps(unsigned long timeoutMilliseconds) {
  PullProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    // 输出按字节数取（含 --name-only -z 里的 NUL），路径因此能原样用；完整度判定见 MakeGitQueryResult。
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  return deps;
}

PullProbeOutcome RunPullProbeLoad(const PullProbeRequest& request) {
  PullProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  const unsigned long timeout =
      request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds : kPullStatusTimeoutFallbackMs;
  const PullProbeDeps deps = MakePullProbeDeps(timeout);
  outcome.target = CollectPullTarget(request, deps);  // 流程痕迹也在这趟里填进 target.workflow
  if (request.includeRelationship) {
    outcome.relationship = CollectPullRelationship(outcome.target, request, deps);
  }
  return outcome;
}

PullAftermath CapturePullAftermath(const std::wstring& exePath, const std::wstring& repositoryDirectory,
                                   const std::wstring& absoluteGitDir,
                                   unsigned long timeoutMilliseconds) {
  PullAftermath aftermath;
  // 流程痕迹的依据是 Git 目录里的档案（MERGE_HEAD、rebase-merge\ 等），与创建提交/撤回同一来源；
  // 未合并条目与分支/HEAD 的位置才是问 Git 要的。两边都只是把现场读回来，不做任何收尾。
  aftermath.workflow = ProbeRepositoryWorkflowState(absoluteGitDir);

  const PullProbeDeps deps = MakePullProbeDeps(timeoutMilliseconds);
  const git::GitQueryResult listing =
      RunQuery(deps.runner, exePath, repositoryDirectory,
               git::BuildPullConflictListingArguments(repositoryDirectory));
  const git::GitQueryResult symbolicRef = RunQuery(deps.runner, exePath, repositoryDirectory,
                                                   git::BuildPullSymbolicRefArguments(repositoryDirectory));
  const git::GitQueryResult headObject = RunQuery(deps.runner, exePath, repositoryDirectory,
                                                  git::BuildPullHeadObjectArguments(repositoryDirectory));
  aftermath.conflict = git::InterpretPullConflictState(listing, symbolicRef, headObject);
  return aftermath;
}

}  // namespace gc::platform
