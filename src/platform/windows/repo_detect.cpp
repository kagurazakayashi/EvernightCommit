#include "platform/windows/repo_detect.h"

#include "platform/windows/git_query_result.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

// 识别用到的 Git 查询全部是只读的 rev-parse / symbolic-ref / for-each-ref：
// 不写对象库、不访问远端。形态分两批发问，因为裸仓库与 .git 内部会让路径类标志中止，
// 而布尔标志在两种形态下都恒定逐行作答（真实 Git 2.55 实测）。
const std::vector<std::wstring> kShapeFlagArguments{
    L"-C", L"{dir}", L"--no-optional-locks", L"--no-replace-objects", L"rev-parse",
    L"--is-bare-repository", L"--is-inside-work-tree", L"--is-inside-git-dir"};

const std::vector<std::wstring> kShapePathArguments{
    L"-C", L"{dir}", L"--no-optional-locks", L"--no-replace-objects", L"rev-parse",
    L"--absolute-git-dir", L"--git-common-dir", L"--show-toplevel"};

const std::vector<std::wstring> kSuperprojectArguments{
    L"-C", L"{dir}", L"--no-optional-locks", L"rev-parse", L"--show-superproject-working-tree"};

const std::vector<std::wstring> kSymbolicRefArguments{
    L"-C", L"{dir}", L"--no-optional-locks", L"symbolic-ref", L"--quiet", L"HEAD"};

const std::vector<std::wstring> kShortShaArguments{
    L"-C", L"{dir}", L"--no-optional-locks", L"rev-parse", L"--short", L"--verify", L"--quiet", L"HEAD"};

// {ref} 由已确认的分支名填入；for-each-ref 只接受 ref 过滤，用 HEAD 会一无所获。
const std::vector<std::wstring> kUpstreamArguments{
    L"-C", L"{dir}", L"--no-optional-locks", L"for-each-ref",
    L"--format=%(upstream:remotename)\t%(upstream:remoteref)", L"{ref}"};

// 把占位符替换成实际值，保持“参数数组”而不是拼接命令行字符串。
[[nodiscard]] std::vector<std::wstring> BindPlaceholders(const std::vector<std::wstring>& arguments,
                                                          const std::wstring& directory,
                                                          const std::wstring& ref) {
  std::vector<std::wstring> bound;
  bound.reserve(arguments.size());
  for (const std::wstring& argument : arguments) {
    if (argument == L"{dir}") {
      bound.push_back(directory);
    } else if (argument == L"{ref}") {
      bound.push_back(ref);
    } else {
      bound.push_back(argument);
    }
  }
  return bound;
}

[[nodiscard]] git::GitQueryResult RunQuery(const git::GitQueryRunner& runner, const std::wstring& exePath,
                                           const std::wstring& directory,
                                           const std::vector<std::wstring>& arguments,
                                           const std::wstring& ref = {}) {
  if (!runner) {
    git::GitQueryResult failed;
    failed.started = false;
    return failed;
  }
  return runner(exePath, directory, BindPlaceholders(arguments, directory, ref));
}

// Git 在 Windows 上输出正斜杠路径；换成本地写法并折叠为绝对路径，界面与后续调用统一使用。
[[nodiscard]] std::wstring LocalizePath(const RepoDetectDeps& deps, const std::wstring& directory,
                                        std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  std::wstring localized = deps.slashify ? deps.slashify(path) : std::wstring(path);
  if (deps.absolutize) {
    const std::wstring absolute = deps.absolutize(directory, localized);
    if (!absolute.empty()) {
      localized = absolute;
    }
  }
  return localized;
}

[[nodiscard]] git::RepoDetection FailWith(const std::wstring& directory, git::RepoError error,
                                           const std::wstring& detail) {
  git::RepoDetection detection;
  detection.kind = git::RepoKind::failed;
  detection.error = error;
  detection.root = directory;
  detection.message = git::BuildRepoErrorDetail(error, detail);
  return detection;
}

}  // namespace

git::RepoDetection DetectRepository(const RepoDetectRequest& request, const RepoDetectDeps& deps) {
  git::RepoDetection detection;
  detection.root = request.directory;
  if (request.directory.empty()) {
    detection.message = git::BuildRepoErrorDetail(git::RepoError::inputEmpty, {});
    detection.error = git::RepoError::inputEmpty;
    return detection;
  }
  if (request.exePath.empty() || (deps.gitExeIsFile && !deps.gitExeIsFile(request.exePath))) {
    detection.error = git::RepoError::gitUnavailable;
    detection.message = git::BuildRepoErrorDetail(detection.error, request.exePath);
    return detection;
  }
  if (deps.directoryExists && !deps.directoryExists(request.directory)) {
    detection.error = git::RepoError::inputNotDirectory;
    detection.message = git::BuildRepoErrorDetail(detection.error, request.directory);
    return detection;
  }

  // 集中环境策略的告知（移除过的重定向变量名，只含名字）：环境里真有这样的变量时，
  // 用户必须看见“它们被移除了、识别与操作仍绑定这个目录”，而不是被静默改了语义。
  std::wstring envNotice;
  const auto captureNotice = [&envNotice](const git::GitQueryResult& query) {
    if (envNotice.empty()) {
      envNotice = query.environmentNotice;
    }
  };
  const auto withNotice = [&envNotice](git::RepoDetection filled) {
    if (!envNotice.empty()) {
      filled.message += L"｜" + envNotice;
    }
    return filled;
  };

  // 1. 形态标志：一次查询拿回三个布尔值，任何仓库形态都会逐行作答。
  const git::GitQueryResult flags =
      RunQuery(deps.runner, request.exePath, request.directory, kShapeFlagArguments);
  captureNotice(flags);
  std::wstring detail;
  const git::RepoError flagFailure = git::ClassifyGitFailure(flags, detail);
  if (flagFailure != git::RepoError::none) {
    if (flagFailure == git::RepoError::notRepository) {
      detection.kind = git::RepoKind::notRepository;
      detection.error = git::RepoError::notRepository;
      detection.message = git::BuildRepoErrorDetail(detection.error, request.directory);
      return withNotice(std::move(detection));
    }
    return withNotice(FailWith(request.directory, flagFailure, detail));
  }
  git::RepoShape shape;
  if (!git::ParseShapeFlags(git::SplitLines(flags.utf16Output), &shape)) {
    detection.error = git::RepoError::badOutput;
    detection.message = git::BuildRepoErrorDetail(detection.error, L"rev-parse 布尔标志没有按行作答");
    return withNotice(std::move(detection));
  }

  // 2. 形态路径：裸仓库与 .git 内部会在给出两项目录后中止，因此非 0 退出是预期结果。
  const git::GitQueryResult paths =
      RunQuery(deps.runner, request.exePath, request.directory, kShapePathArguments);
  captureNotice(paths);
  git::ParseShapePaths(git::SplitLines(paths.utf16Output), &shape);
  detection.absoluteGitDir = LocalizePath(deps, request.directory, shape.absoluteGitDir);
  detection.kind = git::RepoKind::plainWorktree;

  if (shape.bare) {
    // 裸仓库没有工作区：识别成功但不进入提交流程，界面必须把原因说清楚。
    // root 留空，界面改报 Git 目录，免得展示一个不能用的“工作区根目录”。
    detection.kind = git::RepoKind::bare;
    detection.root.clear();
    detection.message = git::BuildRepoSummary(detection, request.directory);
    return withNotice(std::move(detection));
  }
  if (!shape.insideWorkTree || shape.insideGitDir) {
    // .git 目录内部没有可操作的工作区，root 留空：后续 Git 调用没有安全落点。
    detection.kind = git::RepoKind::insideGitDir;
    detection.root.clear();
    detection.message = git::BuildRepoSummary(detection, request.directory);
    return withNotice(std::move(detection));
  }
  if (shape.topLevel.empty()) {
    detection.error = git::RepoError::badOutput;
    detection.message = git::BuildRepoErrorDetail(detection.error, L"工作区根目录未能取得");
    return withNotice(std::move(detection));
  }

  detection.root = LocalizePath(deps, request.directory, shape.topLevel);
  const std::wstring commonDir = LocalizePath(deps, request.directory, shape.commonDir);
  // 链接工作树/子模块是“仓库来历”，游离 HEAD/尚无提交是“分支状态”；
  // 两套信息同时保留，界面才能既说明这块目录的来历又说明它现在的分支状况。
  detection.linked = !commonDir.empty() && !git::PathsEqualFolded(commonDir, detection.absoluteGitDir);
  if (detection.linked) {
    detection.kind = git::RepoKind::linkedWorktree;
  }

  // 2. 子模块：Git 自己知道这一块工作区是否由父仓库登记（.git 是文件的情形）。
  const git::GitQueryResult superproject =
      RunQuery(deps.runner, request.exePath, request.directory, kSuperprojectArguments);
  if (superproject.started && superproject.exited && superproject.exitCode == 0) {
    const std::wstring tree = LocalizePath(deps, request.directory, git::TrimWide(superproject.utf16Output));
    // 父仓库工作区必须区别于自身根目录，否则只是普通仓库。
    if (!tree.empty() && !git::PathsEqualFolded(tree, detection.root)) {
      detection.kind = git::RepoKind::submodule;
      detection.submodule = true;
      detection.superprojectTree = tree;
    }
  }

  // 3. HEAD：分支名 / 游离短提交 ID / 尚无提交。
  const git::GitQueryResult symbolicRef =
      RunQuery(deps.runner, request.exePath, request.directory, kSymbolicRefArguments);
  const git::GitQueryResult shortSha =
      RunQuery(deps.runner, request.exePath, request.directory, kShortShaArguments);
  const git::RepoHead head =
      git::ParseHeadOutput(git::TrimWide(symbolicRef.utf16Output), git::TrimWide(shortSha.utf16Output));
  detection.branch = head.branchName;
  detection.shortSha = head.shortSha;
  detection.headResolved = head.hasCommits;
  if (!head.hasCommits) {
    // symbolic-ref 成功说明只是分支尚无提交；两条查询都失败时不能猜，按失败原因报告。
    const bool unbornBranch =
        head.onBranch || (symbolicRef.started && symbolicRef.exited && !symbolicRef.timedOut &&
                          symbolicRef.exitCode == 0);
    std::wstring headFailure;
    const git::RepoError error = git::ClassifyGitFailure(shortSha, headFailure);
    if (unbornBranch || error == git::RepoError::none || error == git::RepoError::gitFailed) {
      detection.kind = git::RepoKind::noCommits;
    } else {
      return withNotice(FailWith(detection.root, error, headFailure));
    }
  } else if (!head.onBranch) {
    detection.kind = git::RepoKind::detached;
  }

  // 4. 上游：只有当前在分支上才谈得上上游；无上游只是展示状态，不影响仓库加载成功与否。
  if (head.onBranch && !head.branchName.empty()) {
    const git::GitQueryResult upstream =
        RunQuery(deps.runner, request.exePath, request.directory, kUpstreamArguments,
                 L"refs/heads/" + head.branchName);
    if (upstream.started && upstream.exited && upstream.exitCode == 0) {
      const std::vector<std::wstring> lines = git::SplitLines(upstream.utf16Output);
      detection.upstream = git::ParseUpstreamOutput(lines.empty() ? std::wstring() : lines.front()).name;
      detection.upstreamQueried = true;
    }
  }

  detection.message = git::BuildRepoSummary(detection, request.directory);
  detection.error = git::RepoError::none;
  return withNotice(std::move(detection));
}

RepoDetectDeps MakeRepoDetectDeps(unsigned long timeoutMilliseconds) {
  RepoDetectDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                     const std::vector<std::wstring>& arguments) {
    // 子进程工作目录显式绑定为该仓库；本进程的全局当前目录始终不变。
    // 环境走集中策略：继承的 GIT_DIR/GIT_WORK_TREE/GIT_INDEX_FILE 之类重定向
    // 不会再让“识别出来的仓库”和“用户选中的目录”是两回事。
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  deps.absolutize = [](const std::wstring& directory, std::wstring_view relative) {
    return ToAbsolutePathInDirectory(directory, relative);
  };
  deps.slashify = [](std::wstring_view path) { return NormalizePathSeparators(path); };
  deps.directoryExists = [](const std::wstring& path) { return IsExistingDirectory(path); };
  deps.gitExeIsFile = [](const std::wstring& path) { return IsExistingRegularFile(path); };
  return deps;
}

git::RepoDetection RunRepositoryDetection(const RepoDetectRequest& request) {
  return DetectRepository(request, MakeRepoDetectDeps(request.timeoutMilliseconds));
}

}  // namespace gc::platform
