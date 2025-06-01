// Linux PATH 解析与 Git 候选发现（linux 组）。
// 只建真实可执行文件并检查文件/执行位，绝不启动它们——版本验证属于后续执行通道。
#include <sys/stat.h>

#include <string>
#include <vector>

#include "platform/git_discovery.h"
#include "platform/path.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::test::PosixTempDirectory;
using gc::test::RequireMakeDirectory;
using gc::test::RequireWriteFile;
using gc::test::ScopedEnvironmentVariable;

std::string MakeExecutable(const std::string& directory, const std::string& name) {
  const std::string path = directory + "/" + name;
  RequireWriteFile(path, "#!/bin/sh\nexit 0\n", 0755);
  return path;
}

}  // namespace

GC_TEST(linux_git_discovery_follows_path_and_filters_execute_permission) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string bin1 = temp.PathIn("bin1");
  const std::string bin2 = temp.PathIn("bin 中文");
  const std::string bin3 = temp.PathIn("bin3");
  RequireMakeDirectory(bin1);
  RequireMakeDirectory(bin2);
  RequireMakeDirectory(bin3);
  const std::string first = MakeExecutable(bin1, "git");
  const std::string second = MakeExecutable(bin2, "git");
  RequireWriteFile(bin3 + "/git", "#!/bin/sh\nexit 0\n", 0644);  // 有文件但没有执行位

  ScopedEnvironmentVariable path("PATH", bin1 + ":" + bin2 + ":" + bin3 + ":" + bin1);
  const std::vector<std::wstring> candidates = gc::platform::DiscoverGitCandidates();
  GC_REQUIRE_MESSAGE(candidates.size() == 2, "只应保留两个可执行候选（非执行位目录被过滤）");
  GC_CHECK(candidates[0] == gc::platform::Utf8ToWide(first));
  GC_CHECK(candidates[1] == gc::platform::Utf8ToWide(second));

  const gc::platform::GitExecutableResolution resolved = gc::platform::ResolveGitExeInput(L"git");
  GC_CHECK_MESSAGE(resolved.resolved, "PATH 中第一个可执行候选应可用");
  GC_CHECK(resolved.path == gc::platform::Utf8ToWide(first));
  GC_CHECK(resolved.exists && resolved.executable);

  const gc::platform::GitExecutableResolution nonExecutable =
      gc::platform::ResolveGitExeInput(gc::platform::Utf8ToWide(bin3 + "/git"));
  GC_CHECK(!nonExecutable.resolved);
  GC_CHECK(nonExecutable.exists);
  GC_CHECK_MESSAGE(!nonExecutable.executable, "缺少执行位必须被判为不可用");
  GC_CHECK(!nonExecutable.failureReason.empty());
}

GC_TEST(linux_git_discovery_is_case_sensitive_and_keeps_path_order) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string lower = temp.PathIn("gitdir");
  const std::string upper = temp.PathIn("GitDir");
  RequireMakeDirectory(lower);
  RequireMakeDirectory(upper);
  const std::string lowerGit = MakeExecutable(lower, "git");
  const std::string upperGit = MakeExecutable(upper, "git");

  ScopedEnvironmentVariable path("PATH", lower + ":" + upper);
  const std::vector<std::wstring> candidates = gc::platform::DiscoverGitCandidates();
  GC_REQUIRE(candidates.size() == 2, "大小写不同的两条路径是两个不同候选");
  GC_CHECK(candidates[0] == gc::platform::Utf8ToWide(lowerGit));
  GC_CHECK(candidates[1] == gc::platform::Utf8ToWide(upperGit));

  // 裸名按 PATH 顺序取第一个，不套用 Windows 的 cmd/msys 档位。
  GC_CHECK(gc::platform::NormalizeGitExeInput(L"git") == gc::platform::Utf8ToWide(lowerGit));
}

GC_TEST(linux_git_manual_path_supports_special_names_without_shell) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string directory = temp.PathIn("带 空格 的目录");
  RequireMakeDirectory(directory);
  // 前导减号、引号、分号、$、中文都在文件名里：它们只作为路径字节，不经过 shell。
  const std::string special = MakeExecutable(directory, "-git 中文'\";$&");

  const gc::platform::GitExecutableResolution resolved =
      gc::platform::ResolveGitExeInput(gc::platform::Utf8ToWide(special));
  GC_CHECK_MESSAGE(resolved.resolved, "手动路径必须支持空格/引号/前导减号");
  GC_CHECK(resolved.path == gc::platform::Utf8ToWide(special));
  GC_CHECK(resolved.executable);

  const gc::platform::GitExecutableResolution directoryInput =
      gc::platform::ResolveGitExeInput(gc::platform::Utf8ToWide(directory));
  GC_CHECK(!directoryInput.resolved);
  GC_CHECK(!directoryInput.failureReason.empty());

  const gc::platform::GitExecutableResolution missing =
      gc::platform::ResolveGitExeInput(gc::platform::Utf8ToWide(temp.PathIn("no-such-git")));
  GC_CHECK(!missing.resolved);
  GC_CHECK(!missing.exists);
  GC_CHECK(!missing.failureReason.empty());

  const gc::platform::GitExecutableResolution bareMissing =
      gc::platform::ResolveGitExeInput(L"definitely-not-a-real-git-name");
  GC_CHECK(!bareMissing.resolved);
  GC_CHECK(!bareMissing.failureReason.empty());
}

GC_TEST(linux_git_bare_name_does_not_fall_back_to_current_directory) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string cwd = temp.PathIn("cwd");
  const std::string emptyPath = temp.PathIn("empty-path");
  RequireMakeDirectory(cwd);
  RequireMakeDirectory(emptyPath);
  MakeExecutable(cwd, "git");  // 工作目录里放一个同名可执行文件

  gc::test::ScopedCurrentDirectory chdir(cwd);
  GC_REQUIRE_MESSAGE(chdir.Valid(), "切换测试工作目录失败");
  ScopedEnvironmentVariable path("PATH", emptyPath);

  const gc::platform::GitExecutableResolution resolved = gc::platform::ResolveGitExeInput(L"git");
  GC_CHECK_MESSAGE(!resolved.resolved, "PATH 没命中时不能把当前目录里的 git 当成候选");
  GC_CHECK(!resolved.failureReason.empty());
}

GC_TEST(linux_git_invalid_utf8_path_is_reported_as_unavailable_not_guessed) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string directory = temp.PathIn("bin");
  RequireMakeDirectory(directory);
  const std::string gitPath = MakeExecutable(directory, "git");

  // PATH 不是 UTF-8 时本工程不猜码页：裸名明确失败，候选为空。
  ScopedEnvironmentVariable path("PATH", std::string("/usr/bin:\xFF", 10));
  GC_CHECK(gc::platform::GetPathVariable().empty());
  GC_CHECK(gc::platform::DiscoverGitCandidates().empty());
  const gc::platform::GitExecutableResolution resolved = gc::platform::ResolveGitExeInput(L"git");
  GC_CHECK(!resolved.resolved);
  GC_CHECK_MESSAGE(resolved.failureReason.find(L"UTF-8") != std::wstring::npos,
                   "应明确说 PATH 不是有效 UTF-8，而不是谎称没找到");

  // 但手动给出的绝对路径不查 PATH，不能因为 PATH 坏了就连它一起拒绝。
  const gc::platform::GitExecutableResolution manual =
      gc::platform::ResolveGitExeInput(gc::platform::Utf8ToWide(gitPath));
  GC_CHECK_MESSAGE(manual.resolved, "绝对路径不应受无效 PATH 影响");
  GC_CHECK(manual.executable);
}
