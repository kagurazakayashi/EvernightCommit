#include "support/tiny_test.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "git/repository.h"
#include "platform/windows/repo_detect.h"

namespace {

using gc::git::GitQueryResult;
using gc::git::RepoDetection;
using gc::git::RepoError;
using gc::git::RepoKind;

// 关键字互不冲突：布尔组含 --is-bare-repository，路径组含 --absolute-git-dir，短提交 ID 查询含 --short。
constexpr const wchar_t* kFlagsKeyword = L"--is-bare-repository";
constexpr const wchar_t* kPathsKeyword = L"--absolute-git-dir";
constexpr const wchar_t* kSuperKeyword = L"--show-superproject-working-tree";
constexpr const wchar_t* kSymbolicKeyword = L"symbolic-ref";
constexpr const wchar_t* kShortKeyword = L"--short";
constexpr const wchar_t* kUpstreamKeyword = L"for-each-ref";

// 只读查询的桩：按命令里的关键字返回预先准备的输出与退出码，测试不启动任何真实进程。
struct FakeGit {
  std::map<std::wstring, GitQueryResult> byKeyword;
  std::vector<std::wstring> order;
  std::vector<std::wstring> lastArguments;  // 最后一次查询的参数数组，用于断言绑定目录与 ref
  std::vector<std::wstring> matched;        // 依次记录命中过的查询关键字
  GitQueryResult fallback;

  GitQueryResult Run(const std::wstring&, const std::wstring& directory,
                     const std::vector<std::wstring>& arguments) {
    lastArguments = arguments;
    std::wstring joined;
    for (const std::wstring& argument : arguments) {
      joined += argument + L' ';
    }
    for (const std::wstring& keyword : order) {
      if (joined.find(keyword) != std::wstring::npos) {
        matched.push_back(keyword);
        return byKeyword[keyword];
      }
    }
    return fallback;
  }

  bool Queried(const wchar_t* keyword) const {
    return std::find(matched.begin(), matched.end(), std::wstring(keyword)) != matched.end();
  }

  // -C 之后的那一项就是本次查询绑定的目录。
  std::wstring boundDirectory() const {
    for (size_t index = 0; index + 1 < lastArguments.size(); ++index) {
      if (lastArguments[index] == L"-C") {
        return lastArguments[index + 1];
      }
    }
    return {};
  }
};

GitQueryResult Ok(std::wstring output, std::wstring error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = 0;
  result.utf16Output = std::move(output);
  result.utf16Error = std::move(error);
  return result;
}

GitQueryResult Fail(int exitCode, std::wstring error, std::wstring output = {}) {
  GitQueryResult result = Ok(std::move(output), std::move(error));
  result.exitCode = exitCode;
  return result;
}

// 普通工作区的标准答案，取值形式与 Git for Windows 2.55 本机实测一致：
// 布尔组三行恒定；路径组依次是绝对 git 目录、公共 git 目录（可能是 ".git" 这类相对片段）、工作区根。
FakeGit MakeRepoFake(std::wstring symbolicRef = L"refs/heads/main", std::wstring shortSha = L"1a2b3c4",
                     std::wstring upstream = L"origin\trefs/heads/main") {
  FakeGit fake;
  fake.order = {kSuperKeyword, kUpstreamKeyword, kSymbolicKeyword, kShortKeyword, kFlagsKeyword, kPathsKeyword};
  fake.byKeyword[kFlagsKeyword] = Ok(L"false\ntrue\nfalse\n");
  fake.byKeyword[kPathsKeyword] = Ok(L"D:/repo/.git\n.git\nD:/repo\n");
  fake.byKeyword[kSuperKeyword] = Ok(L"");
  fake.byKeyword[kSymbolicKeyword] = symbolicRef.empty() ? Fail(1, L"") : Ok(symbolicRef + L"\n");
  fake.byKeyword[kShortKeyword] = shortSha.empty() ? Fail(1, L"") : Ok(shortSha + L"\n");
  fake.byKeyword[kUpstreamKeyword] = upstream.empty() ? Ok(L"") : Ok(upstream + L"\n");
  fake.fallback = Fail(1, L"");
  return fake;
}

void SetAnswer(FakeGit& fake, const wchar_t* keyword, GitQueryResult answer) {
  fake.byKeyword[keyword] = std::move(answer);
}

gc::platform::RepoDetectDeps MakeDeps(FakeGit& fake) {
  gc::platform::RepoDetectDeps deps;
  deps.runner = [&fake](const std::wstring& exe, const std::wstring& directory,
                        const std::vector<std::wstring>& arguments) {
    return fake.Run(exe, directory, arguments);
  };
  const auto backslashed = [](std::wstring_view path) {
    std::wstring result(path);
    for (wchar_t& c : result) {
      if (c == L'/') {
        c = L'\\';
      }
    }
    return result;
  };
  // 与真实实现同构：统一分隔符，相对片段（含 ".."）按查询目录展开，绝对路径原样保留。
  // 真实的 ToAbsolutePathInDirectory 用 GetFullPathNameW 折叠，桩里按同样的语义手工折叠。
  deps.slashify = backslashed;
  deps.absolutize = [backslashed](const std::wstring& directory, std::wstring_view path) {
    const std::wstring result = backslashed(path);
    const bool absolute = result.size() >= 3 && result[1] == L':' && (result[2] == L'\\' || result[2] == L'/');
    std::vector<std::wstring> segments;
    const std::wstring source = absolute ? result : backslashed(directory) + L"\\" + result;
    size_t cursor = 0;
    while (cursor <= source.size()) {
      const size_t separator = source.find(L'\\', cursor);
      const std::wstring segment =
          source.substr(cursor, separator == std::wstring::npos ? std::wstring_view::npos : separator - cursor);
      cursor = (separator == std::wstring::npos) ? source.size() + 1 : separator + 1;
      if (segment.empty() || segment == L".") {
        continue;
      }
      if (segment == L"..") {
        if (segments.size() > 1) {
          segments.pop_back();
        }
        continue;
      }
      segments.push_back(segment);
    }
    std::wstring collapsed;
    for (const std::wstring& segment : segments) {
      if (!collapsed.empty()) {
        collapsed += L'\\';
      }
      collapsed += segment;
    }
    // 盘符段 "D:" 之后必须补回根分隔符，与 GetFullPathNameW 的输出一致。
    if (collapsed.size() == 2 && collapsed[1] == L':') {
      collapsed += L'\\';
    }
    return collapsed;
  };
  deps.directoryExists = [](const std::wstring&) { return true; };
  deps.gitExeIsFile = [](const std::wstring&) { return true; };
  return deps;
}

gc::platform::RepoDetectRequest MakeRequest(std::wstring directory) {
  gc::platform::RepoDetectRequest request;
  request.exePath = L"C:\\Program Files\\Git\\bin\\git.exe";
  request.directory = std::move(directory);
  request.timeoutMilliseconds = 1000;
  return request;
}

RepoDetection Detect(FakeGit& fake, const std::wstring& directory) {
  return gc::platform::DetectRepository(MakeRequest(directory), MakeDeps(fake));
}

}  // namespace

GC_TEST(shape_flags_and_paths_parse_by_position) {
  gc::git::RepoShape shape;
  GC_CHECK(gc::git::ParseShapeFlags({L"false", L"true", L"false"}, &shape));
  GC_CHECK(!shape.bare && shape.insideWorkTree && !shape.insideGitDir);
  GC_CHECK(gc::git::ParseShapeFlags({L"true", L"false", L"true"}, &shape));
  GC_CHECK(shape.bare && !shape.insideWorkTree && shape.insideGitDir);
  // 行数不足、布尔位上混入路径都不算成功。
  GC_CHECK(!gc::git::ParseShapeFlags({L"false", L"true"}, &shape));
  GC_CHECK(!gc::git::ParseShapeFlags({L"true", L"D:/x", L"true"}, &shape));

  // 路径组：裸仓库与 .git 内部只给出前两项，工作区根留空但不算解析失败。
  gc::git::RepoShape paths;
  GC_CHECK(gc::git::ParseShapePaths({L"D:/repo/.git", L".git", L"D:/repo"}, &paths));
  GC_CHECK(paths.absoluteGitDir == L"D:/repo/.git" && paths.commonDir == L".git" && paths.topLevel == L"D:/repo");
  GC_CHECK(gc::git::ParseShapePaths({L"D:/bare.git", L"."}, &paths));
  GC_CHECK(paths.topLevel.empty());
  GC_CHECK(!gc::git::ParseShapePaths({L"D:/bare.git"}, &paths));
  GC_CHECK(!gc::git::ParseShapePaths({L"", L"."}, &paths));
}

GC_TEST(head_output_separates_branch_detached_and_unborn) {
  const gc::git::RepoHead branch = gc::git::ParseHeadOutput(L"refs/heads/\u4e3b\u5206\u652f", L"1a2b3c4");
  GC_CHECK(branch.onBranch && branch.branchName == L"\u4e3b\u5206\u652f" && branch.hasCommits);

  const gc::git::RepoHead detached = gc::git::ParseHeadOutput(L"", L"9f8e7d6");
  GC_CHECK(!detached.onBranch && detached.shortSha == L"9f8e7d6" && detached.hasCommits);

  const gc::git::RepoHead unborn = gc::git::ParseHeadOutput(L"refs/heads/main", L"");
  GC_CHECK(unborn.onBranch && unborn.branchName == L"main" && !unborn.hasCommits && unborn.unborn);

  // refs/tags 不是分支，按“不在分支上”处理。
  const gc::git::RepoHead tagLike = gc::git::ParseHeadOutput(L"refs/tags/v1", L"1a2b3c4");
  GC_CHECK(!tagLike.onBranch && tagLike.shortSha == L"1a2b3c4");
}

GC_TEST(upstream_output_handles_missing_upstream) {
  GC_CHECK(gc::git::ParseUpstreamOutput(L"origin\trefs/heads/main").name == L"origin/main");
  GC_CHECK(gc::git::ParseUpstreamOutput(L"origin\trefs/heads/\u4e3b\u5206\u652f").name ==
           L"origin/\u4e3b\u5206\u652f");
  GC_CHECK(!gc::git::ParseUpstreamOutput(L"\t").present);
  GC_CHECK(!gc::git::ParseUpstreamOutput(L"").present);
  GC_CHECK(!gc::git::ParseUpstreamOutput(L"origin\t").present);
}

GC_TEST(path_helpers_fold_separators_case_and_boundaries) {
  GC_CHECK(gc::git::PathsEqualFolded(L"D:/Repo/Sub\\", L"d:\\repo\\sub"));
  GC_CHECK(gc::git::PathIsWithin(L"D:\\repo\\sub", L"D:/repo"));
  GC_CHECK(!gc::git::PathIsWithin(L"D:\\repo-backup", L"D:\\repo"));
  GC_CHECK(gc::git::PathIsWithin(L"d:\\REPO\\.git\\worktrees\\fix", L"D:\\repo\\.git"));
  GC_CHECK(!gc::git::PathsEqualFolded(L"D:\\repo\\.git", L"D:\\repo\\.git\\worktrees\\fix"));
}

GC_TEST(git_failure_classification_is_ordered_and_specific) {
  std::wstring detail;

  GitQueryResult notRepo =
      Fail(128, L"fatal: not a git repository (or any of the parent directories): .git");
  GC_CHECK(gc::git::ClassifyGitFailure(notRepo, detail) == RepoError::notRepository);

  GitQueryResult dubious =
      Fail(128, L"fatal: detected dubious ownership in repository at 'P:/yashi/\u4ed3\u5e93'");
  GC_CHECK(gc::git::ClassifyGitFailure(dubious, detail) == RepoError::dubiousOwnership);
  GC_CHECK(detail.find(L"safe.directory") != std::wstring::npos);

  GitQueryResult denied = Fail(128, L"fatal: unable to access 'P:/secret': Permission denied");
  GC_CHECK(gc::git::ClassifyGitFailure(denied, detail) == RepoError::accessDenied);

  GitQueryResult missing = Fail(128, L"fatal: cannot change to 'D:/\u4e0d\u5b58\u5728': No such file");
  GC_CHECK(gc::git::ClassifyGitFailure(missing, detail) == RepoError::inputNotDirectory);

  GitQueryResult launch;  // started=false 优先于一切输出内容
  launch.utf16Error = L"fatal: not a git repository";
  GC_CHECK(gc::git::ClassifyGitFailure(launch, detail) == RepoError::gitLaunchFailed);

  GitQueryResult timeout = Ok(L"partial");
  timeout.timedOut = true;
  GC_CHECK(gc::git::ClassifyGitFailure(timeout, detail) == RepoError::gitTimeout);

  GitQueryResult other = Fail(129, L"error: unknown option `--brand-new-flag'");
  GC_CHECK(gc::git::ClassifyGitFailure(other, detail) == RepoError::gitFailed);
  GC_CHECK(gc::git::ClassifyGitFailure(Ok(L"true"), detail) == RepoError::none);

  // 致命信息在 stderr、机器输出在 stdout：归类必须同时看到两条流。
  GC_CHECK(gc::git::ClassifyGitFailure(Ok(L"true\nfalse\ntrue\n", L"fatal: not a git repository"), detail) ==
           RepoError::none);
  GitQueryResult splitFailure = Ok(L"", L"fatal: not a git repository");
  splitFailure.exitCode = 128;
  GC_CHECK(gc::git::ClassifyGitFailure(splitFailure, detail) == RepoError::notRepository);
}

GC_TEST(detect_plain_worktree_reports_root_branch_and_upstream) {
  FakeGit fake = MakeRepoFake();
  const RepoDetection detection = Detect(fake, L"D:\\repo");

  GC_CHECK(detection.kind == RepoKind::plainWorktree);
  GC_CHECK(detection.error == RepoError::none);
  GC_CHECK(detection.root == L"D:\\repo");
  GC_CHECK(detection.absoluteGitDir == L"D:\\repo\\.git");
  GC_CHECK(detection.branch == L"main");
  GC_CHECK(detection.upstream == L"origin/main");
  GC_CHECK(detection.headResolved);
  GC_CHECK(detection.message.find(L"\u666e\u901a\u5de5\u4f5c\u533a") != std::wstring::npos);
  // 实测证明 for-each-ref 用 HEAD 过滤查不到上游，必须按已确认的分支 ref 过滤。
  GC_CHECK(fake.lastArguments.back() == L"refs/heads/main");
}

GC_TEST(detect_binds_every_query_to_the_selected_directory) {
  FakeGit fake = MakeRepoFake();
  const RepoDetection detection = Detect(fake, L"E:\\\u53e6\u4e00\u4e2a\u4ed3\u5e93");

  GC_CHECK(detection.error == RepoError::none);
  // 每条查询都显式 -C 用户选择的目录，不依赖也不改变进程全局工作目录。
  GC_CHECK(fake.Queried(kFlagsKeyword) && fake.Queried(kUpstreamKeyword));
  GC_CHECK(fake.boundDirectory() == L"E:\\\u53e6\u4e00\u4e2a\u4ed3\u5e93");
}

GC_TEST(detect_walks_up_from_subdirectory_and_keeps_chinese_path) {
  FakeGit fake = MakeRepoFake();
  // 从子目录进入时 Git 的实测形态：工作区根指向上一层，公共 git 目录变成相对片段。
  SetAnswer(fake, kPathsKeyword,
            Ok(L"D:/\u4e2d\u6587\u4ed3\u5e93 \u76ee\u5f55/.git\n../.git\nD:/\u4e2d\u6587\u4ed3\u5e93 \u76ee\u5f55\n"));

  const RepoDetection detection = Detect(fake, L"D:\\\u4e2d\u6587\u4ed3\u5e93 \u76ee\u5f55\\src");

  GC_CHECK(detection.kind == RepoKind::plainWorktree);
  GC_CHECK(detection.root == L"D:\\\u4e2d\u6587\u4ed3\u5e93 \u76ee\u5f55");
  GC_CHECK(detection.message.find(L"\u5df2\u4ece\u6240\u9009\u5b50\u76ee\u5f55\u4e0a\u6eaf") != std::wstring::npos);
}

GC_TEST(detect_no_upstream_is_still_a_successful_load) {
  FakeGit fake = MakeRepoFake(L"refs/heads/main", L"1a2b3c4", L"");
  const RepoDetection detection = Detect(fake, L"D:\\repo");

  GC_CHECK(detection.kind == RepoKind::plainWorktree);
  GC_CHECK(detection.error == RepoError::none);
  GC_CHECK(detection.upstream.empty());
  GC_CHECK(detection.upstreamQueried);
  GC_CHECK(gc::git::FormatUpstreamDisplay(detection).find(L"\u672a\u8bbe\u7f6e\u4e0a\u6e38") != std::wstring::npos);
}

GC_TEST(detect_without_commits_reports_branch_and_no_upstream) {
  FakeGit fake = MakeRepoFake(L"refs/heads/\u521d\u59cb\u5206\u652f", L"", L"");
  const RepoDetection detection = Detect(fake, L"D:\\repo");

  GC_CHECK(detection.kind == RepoKind::noCommits);
  GC_CHECK(gc::git::KindHasWorkspace(detection.kind));
  GC_CHECK(detection.branch == L"\u521d\u59cb\u5206\u652f");
  GC_CHECK(!detection.headResolved);
  GC_CHECK(gc::git::FormatBranchDisplay(detection).find(L"\u5c1a\u65e0\u63d0\u4ea4") != std::wstring::npos);
}

GC_TEST(detect_detached_head_reports_short_sha) {
  FakeGit fake = MakeRepoFake(L"", L"9f8e7d6");
  const RepoDetection detection = Detect(fake, L"D:\\repo");

  GC_CHECK(detection.kind == RepoKind::detached);
  GC_CHECK(detection.shortSha == L"9f8e7d6");
  GC_CHECK(gc::git::FormatBranchDisplay(detection).find(L"\u6e38\u79bb HEAD\uff089f8e7d6\uff09") !=
           std::wstring::npos);
  GC_CHECK(gc::git::FormatUpstreamDisplay(detection).find(L"\u6e38\u79bb") != std::wstring::npos);
  // 游离 HEAD 没有分支，不该发起上游查询。
  GC_CHECK(!fake.Queried(kUpstreamKeyword));
}

GC_TEST(detect_bare_repository_has_clear_explanation_and_no_workspace) {
  FakeGit fake = MakeRepoFake();
  SetAnswer(fake, kFlagsKeyword, Ok(L"true\nfalse\ntrue\n"));
  // 裸仓库的路径组只给两行就以 “must be run in a work tree” 中止，退出码非 0。
  SetAnswer(fake, kPathsKeyword,
            Fail(128, L"fatal: this operation must be run in a work tree", L"D:/\u88f8\u4ed3\u5e93.git\n.\n"));

  const RepoDetection detection = Detect(fake, L"D:\\\u88f8\u4ed3\u5e93.git");

  GC_CHECK(detection.kind == RepoKind::bare);
  GC_CHECK(detection.error == RepoError::none);
  // 裸仓库没有工作区：root 留空，界面改报 Git 目录。
  GC_CHECK(detection.root.empty());
  GC_CHECK(detection.absoluteGitDir == L"D:\\\u88f8\u4ed3\u5e93.git");
  GC_CHECK(!gc::git::KindHasWorkspace(detection.kind));
  GC_CHECK(detection.message.find(L"\u88f8\u4ed3\u5e93") != std::wstring::npos);
  GC_CHECK(detection.message.find(L"Git \u76ee\u5f55") != std::wstring::npos);
  GC_CHECK(detection.message.find(L"\u4e0d\u80fd\u5728\u8fd9\u91cc\u6682\u5b58\u6216\u521b\u5efa\u63d0\u4ea4") !=
           std::wstring::npos);
  // 裸仓库没有工作区，不再追问 HEAD。
  GC_CHECK(detection.branch.empty());
  GC_CHECK(fake.boundDirectory() == L"D:\\\u88f8\u4ed3\u5e93.git");
}

GC_TEST(detect_inside_git_directory_is_not_a_workspace) {
  FakeGit fake = MakeRepoFake();
  SetAnswer(fake, kFlagsKeyword, Ok(L"false\nfalse\ntrue\n"));
  SetAnswer(fake, kPathsKeyword,
            Fail(128, L"fatal: this operation must be run in a work tree", L"D:/repo/.git\n.\n"));

  const RepoDetection detection = Detect(fake, L"D:\\repo\\.git");

  GC_CHECK(detection.kind == RepoKind::insideGitDir);
  GC_CHECK(detection.root.empty());
  GC_CHECK(detection.message.find(L"Git \u76ee\u5f55") != std::wstring::npos);
  GC_CHECK(!gc::git::KindHasWorkspace(detection.kind));
}

GC_TEST(detect_submodule_and_linked_worktree_keep_their_origin) {
  // 子模块：Git 报出父仓库工作区，绝对 git 目录住在父仓库 .git/modules 下。
  FakeGit submodule = MakeRepoFake();
  SetAnswer(submodule, kPathsKeyword, Ok(L"D:/repo/.git/modules/child\nD:/repo/.git/modules/child\nD:/repo/child\n"));
  SetAnswer(submodule, kSuperKeyword, Ok(L"D:/repo\n"));
  const RepoDetection asSubmodule = Detect(submodule, L"D:\\repo\\child");
  GC_CHECK(asSubmodule.kind == RepoKind::submodule);
  GC_CHECK(asSubmodule.superprojectTree == L"D:\\repo");
  GC_CHECK(asSubmodule.message.find(L"\u5b50\u6a21\u5757") != std::wstring::npos);

  // 链接工作树：绝对 git 目录与公共 git 目录不同。
  FakeGit linked = MakeRepoFake();
  SetAnswer(linked, kPathsKeyword,
            Ok(L"D:/repo/.git/worktrees/WT-\u5206\u652f\nD:/repo/.git\nD:/WT \u5206\u652f\n"));
  const RepoDetection asLinked = Detect(linked, L"D:\\WT \u5206\u652f");
  GC_CHECK(asLinked.kind == RepoKind::linkedWorktree);
  GC_CHECK(asLinked.linked);

  // 链接工作树上还没有提交时，来历不能被“尚无提交”覆盖掉。
  FakeGit linkedUnborn = MakeRepoFake(L"refs/heads/new", L"", L"");
  SetAnswer(linkedUnborn, kPathsKeyword, Ok(L"D:/repo/.git/worktrees/fix\nD:/repo/.git\nD:/wt/fix\n"));
  const RepoDetection both = Detect(linkedUnborn, L"D:\\wt\\fix");
  GC_CHECK(both.kind == RepoKind::noCommits);
  GC_CHECK(both.linked);
  GC_CHECK(both.KindLabel().find(L"\u94fe\u63a5\u5de5\u4f5c\u6811") != std::wstring::npos);
}

GC_TEST(detect_non_repository_directory_is_reported_as_such) {
  FakeGit fake = MakeRepoFake();
  SetAnswer(fake, kFlagsKeyword, Fail(128, L"fatal: not a git repository (or any of the parent directories): .git"));

  const RepoDetection detection = Detect(fake, L"D:\\\u666e\u901a\u76ee\u5f55");

  GC_CHECK(detection.kind == RepoKind::notRepository);
  GC_CHECK(detection.error == RepoError::notRepository);
  GC_CHECK(!gc::git::KindHasWorkspace(detection.kind));
  GC_CHECK(detection.message.find(L"\u4e0d\u662f Git \u4ed3\u5e93") != std::wstring::npos);
}

GC_TEST(detect_reports_dubious_ownership_without_trying_to_fix_it) {
  FakeGit fake = MakeRepoFake();
  SetAnswer(fake, kFlagsKeyword,
            Fail(128, L"fatal: detected dubious ownership in repository at 'P:/yashi/\u4ed3\u5e93'"));

  const RepoDetection detection = Detect(fake, L"P:\\yashi\\\u4ed3\u5e93");

  GC_CHECK(detection.kind == RepoKind::failed);
  GC_CHECK(detection.error == RepoError::dubiousOwnership);
  GC_CHECK(detection.message.find(L"safe.directory") != std::wstring::npos);
  GC_CHECK(detection.message.find(L"\u4e0d\u4f1a\u6539\u52a8\u4f60\u7684 Git \u914d\u7f6e") != std::wstring::npos);
}

GC_TEST(detect_reports_unparsable_shape_as_bad_output) {
  FakeGit fake = MakeRepoFake();
  SetAnswer(fake, kFlagsKeyword, Ok(L"true\nD:/something\nfalse\n"));  // 布尔位上出现路径

  const RepoDetection detection = Detect(fake, L"D:\\repo");

  GC_CHECK(detection.error == RepoError::badOutput);
  GC_CHECK(detection.kind == RepoKind::failed);
}

GC_TEST(detect_validates_inputs_before_running_git) {
  FakeGit fake = MakeRepoFake();

  GC_CHECK(Detect(fake, L"").error == RepoError::inputEmpty);

  gc::platform::RepoDetectDeps missingDirectory = MakeDeps(fake);
  missingDirectory.directoryExists = [](const std::wstring&) { return false; };
  const RepoDetection badDirectory =
      gc::platform::DetectRepository(MakeRequest(L"D:\\\u4e0d\u5b58\u5728"), missingDirectory);
  GC_CHECK(badDirectory.error == RepoError::inputNotDirectory);

  gc::platform::RepoDetectDeps badExe = MakeDeps(fake);
  badExe.gitExeIsFile = [](const std::wstring&) { return false; };
  const RepoDetection noGit = gc::platform::DetectRepository(MakeRequest(L"D:\\repo"), badExe);
  GC_CHECK(noGit.error == RepoError::gitUnavailable);
  GC_CHECK(noGit.message.find(L"git.exe") != std::wstring::npos);
  // 输入不可用时不应启动任何查询。
  GC_CHECK(fake.lastArguments.empty());
}

GC_TEST(detect_labels_cover_every_kind_and_error) {
  for (RepoKind kind : {RepoKind::plainWorktree, RepoKind::linkedWorktree, RepoKind::submodule, RepoKind::bare,
                        RepoKind::insideGitDir, RepoKind::notRepository, RepoKind::detached, RepoKind::noCommits,
                        RepoKind::failed}) {
    RepoDetection detection;
    detection.kind = kind;
    GC_CHECK(!detection.KindLabel().empty());
  }
  for (RepoError error :
       {RepoError::none, RepoError::inputEmpty, RepoError::inputNotDirectory, RepoError::gitUnavailable,
        RepoError::notRepository, RepoError::dubiousOwnership, RepoError::accessDenied, RepoError::gitTimeout,
        RepoError::gitLaunchFailed, RepoError::gitFailed, RepoError::badOutput}) {
    GC_CHECK(!gc::git::RepoErrorLabel(error).empty());
    GC_CHECK(!gc::git::BuildRepoErrorDetail(error, L"\u7ec6\u8282").empty());
  }
}

GC_TEST(split_lines_keeps_positional_fields) {
  const std::vector<std::wstring> lines = gc::git::SplitLines(L"true\r\n\r\nD:/a b\r\n");
  GC_CHECK(lines.size() == 3);
  GC_CHECK(lines[0] == L"true");
  GC_CHECK(lines[1].empty());
  GC_CHECK(lines[2] == L"D:/a b");
  GC_CHECK(gc::git::SplitLines(L"").empty());
}
