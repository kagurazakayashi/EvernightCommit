// 「提交详情」的真实 Git 集成测试：把生产代码构造的那套参数原样交给本机 Git 执行，
// 核对「Git 收不收这个形态」「答回来的七个字段解析成什么」，以及读不出对象时的说法。
// 用例一律用 plumbing（write-tree / commit-tree）在夹具自建的临时仓库里造对象：
// 移动引用、真实 commit 与 push 都不在这里发生。绝不接触开发仓库或远端。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_detail.h"
#include "git/repository.h"
#include "platform/commit_detail.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::CommitDetailParseResult;
using gc::platform::CommitDetailOutcome;
using gc::test::GitFixture;
using gc::test::GitRun;

std::string Narrow(std::wstring_view text) { return gc::platform::Utf16ToUtf8(std::wstring(text)); }

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

bool Has(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

// 复用夹具的隔离执行器装配查询依赖：CommitDetailDeps 的 runner 与 git status 那一条同一形态
// （显式 -C + 受守卫的工作目录 + 隔离环境块），因此不必另建第二套执行器。
gc::platform::CommitDetailDeps MakeDeps(GitFixture& fixture) {
  gc::platform::CommitDetailDeps deps;
  deps.runner = fixture.MakeStatusDeps().runner;
  return deps;
}

gc::platform::CommitDetailRequest MakeRequest(GitFixture& fixture, std::wstring_view objectId) {
  gc::platform::CommitDetailRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.objectId = std::wstring(objectId);
  request.timeoutMilliseconds = 20000;
  return request;
}

// 取首行并去掉行尾空白：commit-tree 与 write-tree 都只答一行 ID。
std::wstring FirstLine(std::wstring_view text) {
  const size_t newline = text.find_first_of(L"\r\n");
  std::wstring line(text.substr(0, newline));
  while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) {
    line.pop_back();
  }
  return line;
}

// 用 plumbing 造一枚真实的提交对象：树取自当前索引，消息由 commit-tree 的多个 -m 分段
// （Git 因此把它写成「标题 + 空行 + 正文」，正文段就是 %b 要读回来的那一段）。
// 不移动 HEAD、不动索引与工作区，也不执行 git commit / git push。
std::wstring SeedCommitWithBody(GitFixture& fixture, std::wstring_view subject,
                                std::wstring_view body) {
  const GitRun tree = fixture.RunCheckedInRepo({L"write-tree"});
  std::vector<std::wstring> arguments{L"commit-tree", FirstLine(tree.out), L"-m",
                                      std::wstring(subject)};
  if (!body.empty()) {
    arguments.push_back(L"-m");
    arguments.push_back(std::wstring(body));
  }
  const GitRun created = fixture.RunCheckedInRepo(arguments);
  return FirstLine(created.out);
}

std::string Describe(const GitRun& run) {
  return "exit=" + std::to_string(run.exitCode) + " out=[" + Narrow(run.out) + "] err=[" +
         Narrow(run.err) + "]";
}

}  // namespace

GC_TEST(commit_detail_arguments_run_on_real_git_and_split_into_seven_fields) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"note.txt", "note\n");
  fixture.StageAll();
  const std::wstring objectId =
      SeedCommitWithBody(fixture, L"详情读取的标题", L"描述第一行\n描述第二行");

  // 这一条是「参数形态在真实 Git 上跑得住」的证据：纯逻辑用例永远证明不了 Git 收不收。
  const std::vector<std::wstring> arguments =
      gc::git::BuildCommitDetailArguments(fixture.RepoDir(), objectId);
  const GitRun run = fixture.RunInRepo(arguments);
  GC_REQUIRE_MESSAGE(run.Success(), Describe(run));
  const CommitDetailParseResult parsed =
      gc::git::ParseCommitDetail(run.out, objectId, [](long long seconds) {
        return std::to_wstring(seconds);
      });
  GC_REQUIRE_MESSAGE(parsed.ok, Narrow(parsed.error));
  GC_CHECK(parsed.detail.objectId == objectId);
  GC_CHECK(parsed.detail.summary == L"详情读取的标题");
  GC_CHECK(parsed.detail.description == L"描述第一行\n描述第二行");
  GC_CHECK(Has(parsed.detail.authorName, L"Evernight Test"));
  GC_CHECK(parsed.detail.authorEmail == L"test@example.invalid");
  // 两个时间是 Git 记下的秒数换算出来的：这里只断言「问得到、不是空」，换算本身属本机时区。
  GC_CHECK(!parsed.detail.authoredAt.empty());
  GC_CHECK(!parsed.detail.committedAt.empty());
}

GC_TEST(commit_detail_platform_load_reads_the_selected_commit) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"note.txt", "note\n");
  fixture.StageAll();
  const std::wstring objectId =
      SeedCommitWithBody(fixture, L"生产路径读详情", L"一段\n两段");

  const CommitDetailOutcome outcome =
      gc::platform::LoadCommitDetail(MakeRequest(fixture, objectId), MakeDeps(fixture));
  GC_REQUIRE_MESSAGE(outcome.error == gc::git::RepoError::none, Narrow(outcome.message));
  GC_CHECK(outcome.detail.objectId == objectId);
  // 生产路径用的是本机时区换算：展示文本必须带日期分隔符，而不是裸秒数。
  GC_CHECK(Has(outcome.detail.authoredAt, L"-"));
  GC_CHECK(Has(outcome.text, L"提交哈希：" + std::wstring(objectId)));
  GC_CHECK(Has(outcome.text, L"标题：生产路径读详情"));
  GC_CHECK(Has(outcome.text, L"描述："));
  GC_CHECK(Has(outcome.text, L"一段"));
  GC_CHECK(!Has(outcome.text, L"以上只是开头那一段"));

  // 身份回显由调用方填：LoadCommitDetail 本身不动这三项，RunCommitDetailLoad 才带回。
  GC_CHECK(outcome.objectId.empty());
}

GC_TEST(commit_detail_distinguishes_no_description_from_unreadable_object) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"note.txt", "note\n");
  fixture.StageAll();
  const std::wstring onlySubject = SeedCommitWithBody(fixture, L"没有描述的提交", L"");

  // 「确实没有描述」是事实：读成功、正文空，文本里写「（无描述）」而不是留白。
  const CommitDetailOutcome read =
      gc::platform::LoadCommitDetail(MakeRequest(fixture, onlySubject), MakeDeps(fixture));
  GC_REQUIRE_MESSAGE(read.error == gc::git::RepoError::none, Narrow(read.message));
  GC_CHECK(read.detail.description.empty());
  GC_CHECK(Has(read.text, L"描述：（无描述）"));

  // 「读不到这个对象」是另一回事：形似合法但不存在的 ID，Git 自己报错，界面拿到失败原因。
  const std::wstring missing =
      L"0000000000000000000000000000000000000000";
  const CommitDetailOutcome absent =
      gc::platform::LoadCommitDetail(MakeRequest(fixture, missing), MakeDeps(fixture));
  GC_CHECK(absent.error != gc::git::RepoError::none);
  GC_CHECK(!absent.message.empty());
  GC_CHECK(absent.text.empty());
  GC_CHECK(!Has(absent.message, L"（无描述）"));

  // 不可信的 ID 在发起查询之前就被拒：一条 Git 都不必启动。
  const gc::platform::CommitDetailRequest rejectedByGuard = MakeRequest(fixture, L"--help");
  const CommitDetailOutcome refused =
      gc::platform::LoadCommitDetail(rejectedByGuard, MakeDeps(fixture));
  GC_CHECK(refused.error != gc::git::RepoError::none);
  GC_CHECK(Has(refused.message, L"对象 ID 不符合约定"));
}
