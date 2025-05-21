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

GC_TEST(classify_git_candidate_families) {
  using Family = gc::git::GitCandidateFamily;
  // Git for Windows 面向命令行的入口优先；判断只看目录段，不看大小写，也不把 "Git" 当成 msys。
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:\\Program Files\\Git\\cmd\\git.exe") == Family::cmdShim);
  GC_CHECK(gc::git::ClassifyGitCandidate(L"c:\\program files\\git\\CMD\\git.exe") == Family::cmdShim);
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:/Program Files/Git/cmd/git.exe") == Family::cmdShim);
  // MSYS2 那一套：靠 msys64 这个目录段认出来，哪怕它装在 C:\tools 下。
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:\\tools\\msys64\\usr\\bin\\git.exe") == Family::msys);
  // <...>\usr\bin 是 MSYS 运行时的布局（同目录就是 msys-2.0.dll），装在 Git 里也一样垫后。
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:\\Program Files\\Git\\usr\\bin\\git.exe") == Family::msys);
  // msys 树里的 cmd 目录仍然是 msys 的那一个：来源比目录名更有决定性。
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:\\msys64\\cmd\\git.exe") == Family::msys);
  // 其余形态（原生 64 位实体、第三方前端口、只有文件名）留在中间档。
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:\\Program Files\\Git\\ucrt64\\bin\\git.exe") == Family::other);
  GC_CHECK(gc::git::ClassifyGitCandidate(L"C:\\Program Files\\Git\\bin\\git.exe") == Family::other);
  GC_CHECK(gc::git::ClassifyGitCandidate(L"git.exe") == Family::other);
  GC_CHECK(gc::git::ClassifyGitCandidate(L"") == Family::other);
}

GC_TEST(sort_git_candidates_prefers_cmd_and_defers_msys) {
  // 输入按这台机器 PATH 的真实形态：msys 的 git 排在最前，Git for Windows 的 cmd 入口在后面。
  std::vector<std::wstring> candidates{L"C:\\tools\\msys64\\usr\\bin\\git.exe",
                                       L"C:\\Program Files\\Git\\ucrt64\\bin\\git.exe",
                                       L"C:\\Program Files\\Git\\cmd\\git.exe",
                                       L"C:\\Program Files\\Git\\bin\\git.exe",
                                       L"C:\\tools\\msys64\\mingw64\\bin\\git.exe"};
  gc::git::SortGitCandidatesByPreference(candidates);

  // 档位决定先后，同档内保持原来的 PATH 顺序（稳定排序，不重新发明第二套排序规则）。
  const std::vector<std::wstring> expected{L"C:\\Program Files\\Git\\cmd\\git.exe",
                                           L"C:\\Program Files\\Git\\ucrt64\\bin\\git.exe",
                                           L"C:\\Program Files\\Git\\bin\\git.exe",
                                           L"C:\\tools\\msys64\\usr\\bin\\git.exe",
                                           L"C:\\tools\\msys64\\mingw64\\bin\\git.exe"};
  GC_CHECK(candidates == expected);

  // 空表与单元素表都是原地无操作。
  std::vector<std::wstring> empty;
  gc::git::SortGitCandidatesByPreference(empty);
  GC_CHECK(empty.empty());
  std::vector<std::wstring> single{L"C:\\tools\\msys64\\usr\\bin\\git.exe"};
  gc::git::SortGitCandidatesByPreference(single);
  GC_CHECK(single.size() == 1 && single.front() == L"C:\\tools\\msys64\\usr\\bin\\git.exe");
}

GC_TEST(resolve_bare_name_prefers_cmd_over_msys_ordering) {
  // 用户只输「git」时没有表达过偏好：PATH 里 msys 的在前，也要按档位选 cmd 的那一个。
  const auto probe = MakeFakeFs({L"C:\\tools\\msys64\\usr\\bin\\git.exe", L"C:\\Program Files\\Git\\cmd\\git.exe"});
  const std::wstring pathEnv = L"C:\\tools\\msys64\\usr\\bin;C:\\Program Files\\Git\\cmd";

  GC_CHECK(gc::git::ResolveExecutableInput(L"git", pathEnv, probe) == L"C:\\Program Files\\Git\\cmd\\git.exe");
}

GC_TEST(search_path_still_reports_raw_path_order) {
  // 排序是显式的一步，不悄悄改变 SearchPathForExecutable 的语义（它对应 `where git` 的作答顺序）。
  const auto probe = MakeFakeFs({L"C:\\tools\\msys64\\usr\\bin\\git.exe", L"C:\\Program Files\\Git\\cmd\\git.exe"});
  const auto found = gc::git::SearchPathForExecutable(L"C:\\tools\\msys64\\usr\\bin;C:\\Program Files\\Git\\cmd",
                                                     L"git.exe", probe);
  GC_CHECK(found.size() == 2 && found.front() == L"C:\\tools\\msys64\\usr\\bin\\git.exe");
}
