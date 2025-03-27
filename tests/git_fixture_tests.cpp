#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <string>
#include <vector>

#include "git/repository.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"

namespace {

using gc::git::PathIsWithin;
using gc::git::PathsEqualFolded;
using gc::test::GitFixture;
using gc::test::PrerequisiteFailure;

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
}

bool IsSha(const std::wstring& text) {
  if (text.size() != 40) {
    return false;
  }
  for (const wchar_t c : text) {
    const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}

bool ContainsLine(const std::vector<std::wstring>& lines, const std::wstring& expected) {
  for (const std::wstring& line : lines) {
    if (line == expected) {
      return true;
    }
  }
  return false;
}

}  // namespace

GC_TEST(fixture_temp_root_is_created_and_cleaned_up) {
  std::wstring root;
  {
    GitFixture fixture;
    PrepareFixture(fixture);
    root = fixture.Root();
    GC_CHECK_MESSAGE(gc::platform::IsExistingDirectory(root), "临时根目录在用例期间应当存在");
    fixture.InitRepository();
    fixture.WriteFile(L"a.txt", "alpha\n");
    fixture.StageAll();
    fixture.Commit(L"初始提交");
    GC_CHECK_MESSAGE(IsSha(fixture.HeadSha()), "HEAD 应为真实的 40 位提交 ID");
  }
  // 析构清理必须删干净；若设了 GC_TEST_KEEP_TEMP=1 本用例允许失败（保留现场是显式行为）。
  GC_CHECK_MESSAGE(!gc::platform::IsExistingDirectory(root), "用例结束后临时根目录必须已被清理");
}

GC_TEST(fixture_refuses_directories_outside_temp_root) {
  GitFixture fixture;
  PrepareFixture(fixture);

  bool outsideThrew = false;
  try {
    fixture.PathInRoot(L"C:\\Windows");
  } catch (const PrerequisiteFailure&) {
    outsideThrew = true;
  }
  GC_CHECK_MESSAGE(outsideThrew, "夹具必须拒绝临时根之外的绝对路径");

  bool escapeThrew = false;
  try {
    fixture.PathInRoot(L"..\\..\\Program Files");
  } catch (const PrerequisiteFailure&) {
    escapeThrew = true;
  }
  GC_CHECK_MESSAGE(escapeThrew, "夹具必须折叠 .. 后再判定，拒绝借相对路径越界");

  const std::wstring inside = fixture.PathInRoot(L"repo");
  GC_CHECK_MESSAGE(PathIsWithin(inside, fixture.Root()), "根内相对路径应解析为根内绝对路径");
}

GC_TEST(empty_repository_has_unborn_head_and_clean_status) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();

  GC_CHECK_MESSAGE(fixture.HeadSha().empty(), "空仓库的 HEAD 不应可解析");
  GC_CHECK(fixture.CommitCount() == -1);
  GC_CHECK_MESSAGE(fixture.StatusPorcelain().empty(), "空仓库工作区应无变更");

  const auto revParse = fixture.RunInRepo({L"rev-parse", L"--verify", L"HEAD"});
  GC_CHECK_MESSAGE(!revParse.Success(), "Git 应对无提交的 HEAD 明确报非 0 退出");

  const auto branch = fixture.RunCheckedInRepo({L"symbolic-ref", L"HEAD"});
  GC_CHECK_MESSAGE(gc::git::TrimWide(branch.out) == L"refs/heads/main", "默认分支必须显式固定为 main");
}

GC_TEST(initial_commit_creates_real_objects) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"notes/source.txt", "第一行内容\nsecond line\n");
  fixture.Stage({L"notes/source.txt"});

  GC_CHECK_MESSAGE(ContainsLine(fixture.StatusPorcelain(), L"A  notes/source.txt"),
                   "暂存后应出现新增（A）状态行");

  fixture.Commit(L"初始提交：新增说明文件");

  GC_CHECK_MESSAGE(IsSha(fixture.HeadSha()), "提交后 HEAD 应指向真实提交对象");
  GC_CHECK(fixture.CommitCount() == 1);
  GC_CHECK(fixture.HeadSubject() == L"初始提交：新增说明文件");
  GC_CHECK(fixture.HeadAuthorIdentity() == L"Evernight Test <test@example.invalid>");
  GC_CHECK(fixture.ShowFileAtHead(L"notes/source.txt") == "第一行内容\nsecond line\n");
  GC_CHECK_MESSAGE(fixture.StatusPorcelain().empty(), "提交后工作区应回到干净");
  // 真实对象库验证：HEAD 是 commit 对象，而不是夹具自报的字符串。
  const auto objectType = fixture.RunCheckedInRepo({L"cat-file", L"-t", L"HEAD"});
  GC_CHECK_MESSAGE(objectType.out.rfind(L"commit", 0) == 0, "git cat-file 应确认 HEAD 是 commit 对象");
}

GC_TEST(second_commit_links_to_first_as_parent) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"a.txt", "one\n");
  fixture.StageAll();
  fixture.Commit(L"第一次提交");
  const std::wstring firstSha = fixture.HeadSha();

  fixture.WriteFile(L"a.txt", "two\n");
  fixture.StageAll();
  fixture.Commit(L"第二次提交");
  const std::wstring secondSha = fixture.HeadSha();

  GC_CHECK_MESSAGE(!firstSha.empty() && !secondSha.empty(), "两次提交都应有 HEAD");
  GC_CHECK(firstSha != secondSha);
  GC_CHECK(fixture.CommitCount() == 2);
  GC_CHECK_MESSAGE(PathsEqualFolded(fixture.ParentShaOfHead(), firstSha),
                   "第二次提交的父提交必须是第一次提交");
  GC_CHECK(fixture.HeadSubject() == L"第二次提交");
  GC_CHECK(fixture.ShowFileAtHead(L"a.txt") == "two\n");
  GC_CHECK(fixture.StatusPorcelain().empty());
}

GC_TEST(working_tree_distinguishes_unstaged_staged_and_untracked) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"a.txt", "one\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");

  fixture.WriteFile(L"a.txt", "changed but not staged\n");
  fixture.WriteFile(L"b.txt", "brand new\n");
  const auto beforeStage = fixture.StatusPorcelain();
  GC_CHECK_MESSAGE(ContainsLine(beforeStage, L" M a.txt"), "已跟踪文件的修改应标记为未暂存（第二列 M）");
  GC_CHECK_MESSAGE(ContainsLine(beforeStage, L"?? b.txt"), "新文件应标记为未跟踪");

  fixture.Stage({L"a.txt"});
  const auto afterStage = fixture.StatusPorcelain();
  GC_CHECK_MESSAGE(ContainsLine(afterStage, L"M  a.txt"), "暂存后应标记为已暂存（第一列 M）");
  GC_CHECK_MESSAGE(ContainsLine(afterStage, L"?? b.txt"), "未暂存的 b.txt 不受影响");
}

GC_TEST(fixture_hides_user_git_configuration) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();

  // 所有可见配置项的来源文件都必须落在本次临时根内：
  // 用户的 .gitconfig、系统 config 一旦泄漏就会出现在 --show-origin 里。
  const auto listRun = fixture.RunCheckedInRepo({L"config", L"--list", L"--show-origin"});
  const std::vector<std::wstring> lines = gc::git::SplitLines(listRun.out);
  std::wstring offendingOrigin;
  bool everyOriginOwned = !lines.empty();
  for (const std::wstring& line : lines) {
    const size_t tab = line.find(L'\t');
    const std::wstring origin = gc::git::TrimWide(line.substr(0, tab));
    if (origin.rfind(L"file:", 0) != 0) {
      everyOriginOwned = false;
      offendingOrigin = origin;
      continue;
    }
    // --show-origin 可能给出相对写法（如 .git/config），按仓库目录展开后再比对。
    const std::wstring path = gc::platform::ToAbsolutePathInDirectory(
        fixture.RepoDir(), gc::platform::NormalizePathSeparators(origin.substr(5)));
    if (!PathIsWithin(path, fixture.Root())) {
      everyOriginOwned = false;
      offendingOrigin = path;
    }
  }
  GC_CHECK_MESSAGE(everyOriginOwned,
                   "配置来源必须全部位于夹具临时根内（越界项：" +
                       gc::platform::Utf16ToUtf8(offendingOrigin) + "）");

  // 用户全局身份不应可见（夹具身份改由环境变量固定注入）。
  const auto globalEmail = fixture.RunInRepo({L"config", L"--global", L"--get", L"user.email"});
  GC_CHECK_MESSAGE(!globalEmail.Success(), "用户全局 user.email 不应泄漏进测试仓库");

  fixture.WriteFile(L"x.txt", "x\n");
  fixture.StageAll();
  fixture.Commit(L"提交");
  GC_CHECK(fixture.HeadAuthorIdentity() == L"Evernight Test <test@example.invalid>");
}

GC_TEST(fixture_blocks_network_protocols_before_connecting) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();

  const auto lsRemote = fixture.RunInRepo({L"ls-remote", L"https://example.invalid/repo.git"});
  GC_CHECK_MESSAGE(!lsRemote.Success(), "网络协议必须在夹具里被拒绝");
  GC_CHECK_MESSAGE(lsRemote.err.find(L"not allowed") != std::wstring::npos,
                   "失败原因应是 Git 的协议白名单（GIT_ALLOW_PROTOCOL=file），而不是 DNS 超时");
}

GC_TEST(commit_timestamps_are_deterministic) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"a.txt", "one\n");
  fixture.StageAll();
  fixture.Commit(L"第一次提交");
  const std::wstring firstDate = gc::git::TrimWide(fixture.RunCheckedInRepo({L"log", L"-1", L"--format=%ct"}).out);

  fixture.WriteFile(L"a.txt", "two\n");
  fixture.StageAll();
  fixture.Commit(L"第二次提交");
  const std::wstring secondDate = gc::git::TrimWide(fixture.RunCheckedInRepo({L"log", L"-1", L"--format=%ct"}).out);

  // 基准 1700000000 + 每次 60 秒：与本机时钟无关，重复运行取值一致。
  GC_CHECK(firstDate == L"1700000060");
  GC_CHECK(secondDate == L"1700000120");
}
