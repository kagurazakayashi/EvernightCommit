// Linux 环境变量语义（linux 组）：名字大小写敏感、只构造子进程专用列表、不改全局环境。
#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "git/git_environment.h"
#include "platform/linux/environment.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::git::EnvironmentNameComparison;
using gc::git::EnvironmentOverride;
using gc::git::GitRunPurpose;
using gc::test::ScopedEnvironmentVariable;

bool HasEntry(const std::vector<std::string>& entries, std::string_view wanted) {
  return std::find(entries.begin(), entries.end(), wanted) != entries.end();
}

}  // namespace

GC_TEST(linux_environment_merge_is_case_sensitive) {
  const std::vector<std::string> base{"GIT_DIR=/real", "Git_Dir=keep", "PATH=/bin", "DUP=1", "DUP=2"};
  const std::vector<EnvironmentOverride> overrides{
      EnvironmentOverride{L"GIT_DIR", std::nullopt},
      EnvironmentOverride{L"PATH", std::wstring(L"/usr/bin")},
      EnvironmentOverride{L"FOO", std::wstring(L"bar")},
  };
  std::vector<std::string> merged;
  std::wstring failure;
  GC_REQUIRE_MESSAGE(gc::platform::MergePosixEnvironmentEntries(base, overrides, &merged, &failure),
                     gc::platform::WideToUtf8(failure));

  GC_CHECK_MESSAGE(!HasEntry(merged, "GIT_DIR=/real"), "精确同名删除必须生效");
  GC_CHECK_MESSAGE(HasEntry(merged, "Git_Dir=keep"), "POSIX 名字区分大小写，Git_Dir 不能被连带删除");
  GC_CHECK(HasEntry(merged, "PATH=/usr/bin"));
  GC_CHECK(!HasEntry(merged, "PATH=/bin"));
  GC_CHECK(HasEntry(merged, "FOO=bar"));
  GC_CHECK_MESSAGE(std::count(merged.begin(), merged.end(), "DUP=1") == 1 &&
                       std::count(merged.begin(), merged.end(), "DUP=2") == 1,
                   "没有覆盖的重复项原样保留，不能因合并被误删");
}

GC_TEST(linux_environment_name_rules_do_not_copy_windows_case_folding) {
  GC_CHECK(gc::git::IsNumberedConfigInjectionName(
      L"GIT_CONFIG_KEY_0", EnvironmentNameComparison::posixCaseSensitive));
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(
      L"git_config_key_0", EnvironmentNameComparison::posixCaseSensitive));
  GC_CHECK(gc::git::IsNumberedConfigInjectionName(
      L"git_config_key_0", EnvironmentNameComparison::windowsCaseInsensitive));

  const std::vector<std::wstring> base{L"GIT_DIR=/real", L"git_dir=decoy"};
  const std::vector<std::wstring> names{L"GIT_DIR"};
  const std::vector<std::wstring> found = gc::git::FindInheritedRedirects(
      base, names, EnvironmentNameComparison::posixCaseSensitive);
  GC_CHECK(found.size() == 1 && found.front() == L"GIT_DIR");
}

GC_TEST(linux_environment_builds_child_only_and_isolates_redirects) {
  ScopedEnvironmentVariable gitDir("GIT_DIR", std::string("/some/other/repo"));
  ScopedEnvironmentVariable mixedCase("Git_Dir", std::string("keep-me"));
  ScopedEnvironmentVariable numberedKey("GIT_CONFIG_KEY_0", std::string("user.name"));
  ScopedEnvironmentVariable numberedValue("GIT_CONFIG_VALUE_0", std::string("intruder"));
  ScopedEnvironmentVariable lowerNumbered("git_config_key_1", std::string("keep-lowercase"));
  ScopedEnvironmentVariable askpass("GIT_ASKPASS", std::string("/tmp/askpass"));

  const gc::platform::PosixChildEnvironment background =
      gc::platform::BuildGitChildEnvironment(GitRunPurpose::backgroundProbe, {});
  GC_REQUIRE_MESSAGE(background.failureReason.empty(),
                     gc::platform::WideToUtf8(background.failureReason));
  GC_CHECK(!HasEntry(background.entries, "GIT_DIR=/some/other/repo"));
  GC_CHECK(HasEntry(background.entries, "Git_Dir=keep-me"));
  GC_CHECK(!HasEntry(background.entries, "GIT_CONFIG_KEY_0=user.name"));
  GC_CHECK(!HasEntry(background.entries, "GIT_CONFIG_VALUE_0=intruder"));
  GC_CHECK(HasEntry(background.entries, "git_config_key_1=keep-lowercase"));
  GC_CHECK(HasEntry(background.entries, "GIT_TERMINAL_PROMPT=0"));
  GC_CHECK(!HasEntry(background.entries, "GIT_TERMINAL_PROMPT=1"));

  // 告知只含变量名，不含值。
  GC_CHECK_MESSAGE(background.notice.find(L"GIT_DIR") != std::wstring::npos, "应告知 GIT_DIR 被移除");
  GC_CHECK(background.notice.find(L"/some/other/repo") == std::wstring::npos);
  GC_CHECK_MESSAGE(background.notice.find(L"Git_Dir") == std::wstring::npos,
                   "大小写不同的变量没有被移除，不应出现在告知里");

  // GUI 进程自己的环境一个字都没改。
  const char* stillThere = std::getenv("GIT_DIR");
  GC_CHECK(stillThere != nullptr && std::string(stillThere) == "/some/other/repo");

  const gc::platform::PosixChildEnvironment window =
      gc::platform::BuildGitChildEnvironment(GitRunPurpose::commandWindow, {});
  GC_REQUIRE_MESSAGE(window.failureReason.empty(), gc::platform::WideToUtf8(window.failureReason));
  GC_CHECK(HasEntry(window.entries, "GIT_TERMINAL_PROMPT=1"));
  GC_CHECK(HasEntry(window.entries, "GIT_PAGER=cat"));
  GC_CHECK_MESSAGE(!HasEntry(window.entries, "GIT_ASKPASS=/tmp/askpass"),
                   "命令窗口里提问必须留在窗口里");
  GC_CHECK_MESSAGE(std::getenv("GIT_ASKPASS") != nullptr, "删除只发生在子进程列表里");
}

GC_TEST(linux_environment_rejects_invalid_override_names_and_values) {
  const std::vector<std::string> base{"PATH=/bin"};
  std::vector<std::string> merged;
  std::wstring failure;

  GC_CHECK(!gc::platform::MergePosixEnvironmentEntries(
      base, {EnvironmentOverride{L"BAD=NAME", std::wstring(L"x")}}, &merged, &failure));
  GC_CHECK(!failure.empty());

  std::wstring withNul = L"a";
  withNul.push_back(L'\0');
  withNul.push_back(L'b');
  failure.clear();
  GC_CHECK(!gc::platform::MergePosixEnvironmentEntries(
      base, {EnvironmentOverride{L"SAFE_NAME", withNul}}, &merged, &failure));
  GC_CHECK(!failure.empty());
  GC_CHECK(merged.empty() || merged == base);
}

GC_TEST(linux_environment_notice_keeps_non_utf8_values_out_but_names_in) {
  // 值可能是旧的非 UTF-8 字节：变量仍会被移除，告知仍必须只含名字。
  ScopedEnvironmentVariable gitDir("GIT_DIR", std::string("\xFF\xFE", 2));
  const gc::platform::PosixChildEnvironment environment =
      gc::platform::BuildGitChildEnvironment(GitRunPurpose::backgroundProbe, {});
  GC_REQUIRE_MESSAGE(environment.failureReason.empty(),
                     gc::platform::WideToUtf8(environment.failureReason));
  GC_CHECK(environment.notice.find(L"GIT_DIR") != std::wstring::npos);
  GC_CHECK(environment.notice.find(L"\xFFFD") == std::wstring::npos);
}

GC_TEST(linux_environment_rejects_lossy_override_encoding) {
  std::wstring lone;
  lone.push_back(static_cast<wchar_t>(0xD800));
  const std::vector<std::string> base{"PATH=/bin"};
  std::vector<std::string> merged;
  std::wstring failure;
  GC_CHECK_MESSAGE(!gc::platform::MergePosixEnvironmentEntries(
                       base, {EnvironmentOverride{lone, std::wstring(L"x")}}, &merged, &failure),
                   "无法无损编码的名字必须整次失败，不得替换后继续");
  GC_CHECK(!failure.empty());
}
