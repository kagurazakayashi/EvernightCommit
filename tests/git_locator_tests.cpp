#include "support/tiny_test.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "git/git_locator.h"

namespace {

// 测试假文件系统：fileExists 查白名单，toAbsolute 在本场景里保持原样
//（测试输入的 PATH 条目本身就是绝对路径形式）。
gc::git::FsProbe MakeFakeFs(std::initializer_list<std::wstring> existingFiles) {
  auto files = std::make_shared<std::vector<std::wstring>>(existingFiles);
  gc::git::FsProbe probe;
  probe.fileExists = [files](const std::wstring& path) {
    return std::find(files->begin(), files->end(), path) != files->end();
  };
  probe.toAbsolute = [](const std::wstring& path) { return path; };
  return probe;
}

}  // namespace

GC_TEST(locator_follows_path_order_and_skips_empty_entries) {
  const auto probe = MakeFakeFs({L"C:\\Program Files\\Git\\cmd\\git.exe", L"C:\\tools\\git.exe"});
  const auto found = gc::git::SearchPathForExecutable(L"C:\\tools;;C:\\Program Files\\Git\\cmd;", L"git.exe", probe);

  std::vector<std::wstring> expected{L"C:\\tools\\git.exe", L"C:\\Program Files\\Git\\cmd\\git.exe"};
  GC_CHECK(found == expected);
}

GC_TEST(locator_dedups_case_insensitively) {
  // 同一目录以不同大小写与结尾斜杠出现在 PATH 两次，只保留第一次。
  const auto probe = MakeFakeFs({L"C:\\Git\\cmd\\git.exe", L"c:\\git\\cmd\\git.exe"});
  const auto found = gc::git::SearchPathForExecutable(L"C:\\Git\\cmd;C:\\Git\\cmd\\;c:\\git\\cmd", L"git.exe", probe);

  GC_CHECK(found.size() == 1);
  GC_CHECK(!found.empty() && found.front() == L"C:\\Git\\cmd\\git.exe");
}

GC_TEST(locator_ignores_directories_without_git) {
  const auto probe = MakeFakeFs({L"D:\\only-tools\\git.exe"});
  const auto found = gc::git::SearchPathForExecutable(L"C:\\Windows;C:\\;D:\\only-tools", L"git.exe", probe);

  GC_CHECK(found.size() == 1);
}

GC_TEST(locator_empty_path_env_finds_nothing) {
  const auto probe = MakeFakeFs({});;
  GC_CHECK(gc::git::SearchPathForExecutable(L"", L"git.exe", probe).empty());
}

GC_TEST(resolve_bare_name_uses_path_lookup) {
  const auto probe = MakeFakeFs({L"C:\\Program Files\\Git\\cmd\\git.exe"});
  const std::wstring pathEnv = L"C:\\Program Files\\Git\\cmd";

  // 裸名 "git" 与显式 "git.exe" 都解析到 PATH 命中的完整路径。
  GC_CHECK(gc::git::ResolveExecutableInput(L"git", pathEnv, probe) == L"C:\\Program Files\\Git\\cmd\\git.exe");
  GC_CHECK(gc::git::ResolveExecutableInput(L"git.exe", pathEnv, probe) == L"C:\\Program Files\\Git\\cmd\\git.exe");
}

GC_TEST(resolve_bare_name_without_hit_returns_input) {
  const auto probe = MakeFakeFs({});
  // PATH 找不到时保持原输入，让后续验证给出“文件不存在”的具体原因。
  GC_CHECK(gc::git::ResolveExecutableInput(L"git", L"C:\\Windows", probe) == L"git");
}

GC_TEST(resolve_keeps_path_with_spaces_and_slashes) {
  const auto probe = MakeFakeFs({});
  const std::wstring mixed = L"C:/Program Files/Git 中文/bin/git.exe";
  // 含目录分隔符（包括正斜杠）时不再按 PATH 解析，交由 toAbsolute 规范化。
  GC_CHECK(gc::git::ResolveExecutableInput(mixed, L"C:\\Windows", probe) == mixed);
  GC_CHECK(gc::git::ResolveExecutableInput(L"   ", L"C:\\Windows", probe).empty());
}
