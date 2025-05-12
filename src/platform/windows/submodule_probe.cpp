#include "platform/windows/submodule_probe.h"

#include <vector>

#include "git/diff_view.h"     // IsWorktreeRelativePath / JoinWorktreeFilePath：路径约定只有一份
#include "git/repository.h"
#include "git/submodule_navigation.h"
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
    failed.launchDetail = arguments.empty() ? L"这条查询的参数没有通过构造期的形态检查"
                                            : L"没有可用的 Git 程序";
    return failed;
  }
  return runner(exePath, directory, arguments);
}

// 探测超时兜底：识别那一条最慢（要问四五句只读查询），沿用仓库识别的放宽值。
constexpr unsigned long kSubmoduleProbeTimeoutFallbackMs = 8000;

}  // namespace

git::SubmoduleEntryFacts CollectSubmoduleEntryFacts(const SubmoduleEntryRequest& request,
                                                    const SubmoduleProbeDeps& deps) {
  git::SubmoduleEntryFacts facts;
  facts.itemIsSubmodule = request.itemIsSubmodule;
  facts.relativePath = request.submoduleRelativePath;
  facts.flags = request.flags;
  facts.parentRoot = request.parentRoot;

  if (request.parentRoot.empty()) {
    facts.pathShapeProblem = L"当前没有可用的父仓库工作区根目录，没有可拼接的落点。";
    return facts;
  }
  std::wstring shapeProblem;
  facts.pathShapeOk = git::IsWorktreeRelativePath(request.submoduleRelativePath, &shapeProblem);
  facts.pathShapeProblem = shapeProblem;
  if (!facts.pathShapeOk) {
    return facts;
  }
  facts.resolvedDirectory =
      git::JoinWorktreeFilePath(request.parentRoot, request.submoduleRelativePath);

  // 目录形态只有三个答案，都必须来自档面事实：目录在、是文件、还是压根不存在。
  // 「探不了」只发生在依赖没给的时候（界面一定会给）。
  if (!deps.directoryExists || !deps.regularFileExists) {
    facts.presence = git::SubmodulePathPresence::unknown;
  } else if (deps.directoryExists(facts.resolvedDirectory)) {
    facts.presence = git::SubmodulePathPresence::existsDirectory;
  } else if (deps.regularFileExists(facts.resolvedDirectory)) {
    facts.presence = git::SubmodulePathPresence::existsNotDirectory;
  } else {
    facts.presence = git::SubmodulePathPresence::missing;
  }

  // 父索引那条 gitlink：未初始化、目录不见了、还是已经不是子模块，全靠这一条回答分开。
  const git::GitQueryResult indexListing =
      RunQuery(deps.runner, request.exePath, request.parentRoot,
               git::BuildSubmoduleIndexPointerArguments(request.parentRoot,
                                                       request.submoduleRelativePath));
  facts.index = git::ParseGitlinkIndexEntry(indexListing, request.submoduleRelativePath);
  if (StopRequested(request.stopFlag) && !facts.index.readOk) {
    facts.index.readFailure = L"程序正在退出，这条查询没有发出或没有问完";
  }

  // 只对「确实存在的目录」跑识别：目录不在时识别只会得到一句「这不是仓库」，
  // 那种场合该说的是「目录不见了」，不是仓库形态。
  if (facts.presence != git::SubmodulePathPresence::existsDirectory) {
    return facts;
  }
  if (StopRequested(request.stopFlag)) {
    return facts;  // childProbed 保持 false：判读层据此说「身份没能问出来」，不猜。
  }
  RepoDetectRequest detectRequest;
  detectRequest.exePath = request.exePath;
  detectRequest.directory = facts.resolvedDirectory;
  detectRequest.timeoutMilliseconds = request.timeoutMilliseconds;
  detectRequest.stopFlag = request.stopFlag;
  facts.child = DetectRepository(detectRequest, deps.detect);
  facts.childProbed = facts.child.kind != git::RepoKind::unknown &&
                      (facts.child.error == git::RepoError::none ||
                       facts.child.kind == git::RepoKind::notRepository ||
                       facts.child.kind == git::RepoKind::bare ||
                       facts.child.kind == git::RepoKind::insideGitDir);
  return facts;
}

SubmoduleEntryOutcome RunSubmoduleEntryLoad(const SubmoduleEntryRequest& request) {
  SubmoduleEntryOutcome outcome;
  outcome.repositoryDirectory = request.parentRoot;  // 原样回显：判别在界面线程。
  const unsigned long timeout = request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds
                                                                 : kSubmoduleProbeTimeoutFallbackMs;
  outcome.facts = CollectSubmoduleEntryFacts(request, MakeSubmoduleProbeDeps(timeout));
  return outcome;
}

git::SubmodulePointerFacts CollectSubmodulePointerFacts(const SubmodulePointerRequest& request,
                                                        const SubmoduleProbeDeps& deps) {
  git::SubmodulePointerQueries queries;
  queries.relativePath = request.submoduleRelativePath;
  if (request.parentRoot.empty()) {
    // 连父仓库根都没有了：三条查询一条也不发，判读层据此说「位置没问全」而不是「干净」。
    return git::InterpretSubmodulePointer(queries);
  }
  queries.indexListing = RunQuery(deps.runner, request.exePath, request.parentRoot,
                                  git::BuildSubmoduleIndexPointerArguments(
                                      request.parentRoot, request.submoduleRelativePath));
  if (StopRequested(request.stopFlag)) {
    return git::InterpretSubmodulePointer(queries);
  }
  queries.headPointer = RunQuery(deps.runner, request.exePath, request.parentRoot,
                                 git::BuildSubmoduleHeadPointerArguments(
                                     request.parentRoot, request.submoduleRelativePath));
  if (StopRequested(request.stopFlag)) {
    return git::InterpretSubmodulePointer(queries);
  }
  // 子模块那一份问的是它的目录（父根 + 相对路径当场拼出来，不是记忆里的旧路径）：
  // 目录被挪走时这条查询会明说问不出，而不是拿旧位置顶替。
  const std::wstring directory = request.submoduleDirectory.empty()
                                     ? git::JoinWorktreeFilePath(request.parentRoot,
                                                                 request.submoduleRelativePath)
                                     : request.submoduleDirectory;
  queries.submoduleHead =
      RunQuery(deps.runner, request.exePath, directory,
               git::BuildSubmoduleOwnHeadArguments(directory));
  return git::InterpretSubmodulePointer(queries);
}

SubmodulePointerOutcome RunSubmodulePointerLoad(const SubmodulePointerRequest& request) {
  SubmodulePointerOutcome outcome;
  outcome.repositoryDirectory = request.parentRoot;
  const unsigned long timeout = request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds
                                                                 : kSubmoduleProbeTimeoutFallbackMs;
  outcome.facts = CollectSubmodulePointerFacts(request, MakeSubmoduleProbeDeps(timeout));
  return outcome;
}

SubmoduleProbeDeps MakeSubmoduleProbeDeps(unsigned long timeoutMilliseconds) {
  SubmoduleProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  deps.detect = MakeRepoDetectDeps(timeoutMilliseconds);
  deps.directoryExists = [](const std::wstring& path) { return IsExistingDirectory(path); };
  deps.regularFileExists = [](const std::wstring& path) { return IsExistingRegularFile(path); };
  return deps;
}

}  // namespace gc::platform
