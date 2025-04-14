// 用真实临时仓库驱动生产读取路径（platform::LoadWorkspaceStatus 里的 git log 追问与
// git/commit_history 解析），断言的是「解析出来的提交条目」，不是命令文本。
// 覆盖：空历史不追问 log、多笔提交的顺序与字段、合并提交、中文/制表符/长标题原样回来、
// 刷新后新提交出现、达到上限的截断、detached HEAD 照常读取，以及 show 方案的对象 ID
// 在真实仓库里可被 git show 执行。远端与仓库来源只能是本机临时目录。
#include <string>
#include <vector>

#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include "app/list_view_memory.h"
#include "git/commit_history.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/workspace_status.h"

namespace {

using gc::git::CommitItem;
using gc::git::WorkspaceSnapshot;
using gc::test::GitFixture;

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
}

std::string Narrow(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

// 直接调用生产读取路径；夹具只提供隔离执行器，不参与解析。
WorkspaceSnapshot Load(GitFixture& fixture, bool hasCommits) {
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  request.repositoryHasCommits = hasCommits;
  return gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
}

std::wstring BaseCommit(GitFixture& fixture, std::wstring_view subject) {
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(subject);
  return fixture.HeadSha();
}

const CommitItem* FindById(const std::vector<CommitItem>& commits, const std::wstring& objectId) {
  for (const CommitItem& item : commits) {
    if (item.objectId == objectId) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace

GC_TEST(commit_history_fixture_empty_repository_skips_log) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();

  // 尚无提交（识别层回答 headResolved=false）：这次读取只跑 git status。
  const WorkspaceSnapshot unborn = Load(fixture, false);
  GC_REQUIRE_MESSAGE(unborn.status == gc::git::WorkspaceLoadStatus::loaded, Narrow(unborn.message));
  GC_CHECK(unborn.model.recentCommits.empty());
  GC_CHECK(unborn.historyError == gc::git::RepoError::none);
  // 界面按这套事实给历史列的说明：是「还没有任何提交」，不是空列表也不是失败。
  const gc::git::EmptyStateTexts texts =
      gc::git::WorkspaceEmptyTexts(gc::git::WorkspaceLoadStatus::loaded, L"", false, L"");
  GC_CHECK(texts.history.find(L"还没有任何提交") != std::wstring::npos);

  // 第一次提交之后：历史立刻可读。
  BaseCommit(fixture, L"基线提交");
  const WorkspaceSnapshot loaded = Load(fixture, true);
  GC_REQUIRE_MESSAGE(loaded.status == gc::git::WorkspaceLoadStatus::loaded, Narrow(loaded.message));
  GC_REQUIRE(loaded.model.recentCommits.size() == 1, "一次提交应读到一条");
  GC_CHECK(loaded.model.recentCommits[0].summary == L"基线提交");
}

GC_TEST(commit_history_fixture_orders_and_fields_match_real_git) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  const std::wstring first = BaseCommit(fixture, L"第一笔 提交");
  fixture.WriteFile(L"a.txt", "a\n");
  fixture.StageAll();
  fixture.Commit(L"带\t制表符 与  % 空格的标题");
  const std::wstring second = fixture.HeadSha();
  fixture.WriteFile(L"b.txt", "b\n");
  fixture.StageAll();
  fixture.Commit(std::wstring(200, L'长标题汉字'));
  const std::wstring third = fixture.HeadSha();

  const WorkspaceSnapshot snapshot = Load(fixture, true);
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Narrow(snapshot.message));
  const std::vector<CommitItem>& commits = snapshot.model.recentCommits;
  GC_REQUIRE(commits.size() == 3, "三笔提交都应读到");
  // 从当前 HEAD 可达、新在前：第 0 行必须是 HEAD。
  GC_CHECK(commits[0].objectId == third);
  GC_CHECK(commits[1].objectId == second);
  GC_CHECK(commits[2].objectId == first);
  GC_CHECK(commits[0].summary == std::wstring(200, L'长标题汉字'));
  // 标题里的制表符、空格与 % 都按原样保留：切分只认 NUL，不拆列。
  GC_CHECK(commits[1].summary == L"带\t制表符 与  % 空格的标题");
  GC_CHECK(commits[2].summary == L"第一笔 提交");
  for (const CommitItem& item : commits) {
    GC_CHECK_MESSAGE(item.author == L"Evernight Test", Narrow(item.author));
    GC_CHECK(item.objectId.size() == 40);  // 本机 SHA-1 仓库：完整 ID 原样保存。
    GC_CHECK(!item.isMergeCommit);
    // 时间列已按本机时区填出非空文本（真机换算，见 platform::FormatLocalEpochSeconds）。
    GC_CHECK(!item.authoredAt.empty());
  }
  // 作者秒数与 Git 自己记录的完全一致。
  const std::wstring headEpoch = gc::git::TrimWide(fixture.RunCheckedInRepo({L"log", L"-1", L"--format=%at"}).out);
  GC_CHECK_MESSAGE(std::to_wstring(commits[0].authorEpochSeconds) == headEpoch,
                   "epoch " + Narrow(std::to_wstring(commits[0].authorEpochSeconds)) + " vs " +
                       Narrow(headEpoch));
  GC_CHECK(snapshot.message.find(L"提交历史 3 条") != std::wstring::npos);
}

GC_TEST(commit_history_fixture_merge_commit_recognized) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  BaseCommit(fixture, L"基线提交");
  fixture.RunCheckedInRepo({L"checkout", L"-qb", L"theirs"});
  fixture.WriteFile(L"theirs.txt", "theirs\n");
  fixture.StageAll();
  fixture.Commit(L"分支修改");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"main"});
  fixture.WriteFile(L"ours.txt", "ours\n");
  fixture.StageAll();
  fixture.Commit(L"本方修改");
  fixture.RunCheckedInRepo({L"merge", L"--no-ff", L"-m", L"合并分支", L"theirs"});

  const WorkspaceSnapshot snapshot = Load(fixture, true);
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Narrow(snapshot.message));
  const std::vector<CommitItem>& commits = snapshot.model.recentCommits;
  GC_REQUIRE(commits.size() == 4, "含合并提交在内四笔都应读到");
  GC_CHECK(commits[0].isMergeCommit);  // --no-ff 的合并提交在 HEAD。
  GC_CHECK(commits[0].summary == L"合并分支");
  GC_CHECK(!commits[1].isMergeCommit);
  GC_CHECK(!commits[3].isMergeCommit);  // 根提交没有父，不能误判成合并。

  // 双击该条提交所构造的 show 方案：合并提交要带上「组合差异可能为空」的范围说明。
  const gc::git::CommitShowPlan plan = gc::git::BuildCommitShowPlan(commits[0]);
  GC_REQUIRE(plan.allowed, "合并提交同样应给出可执行方案");
  GC_CHECK(plan.notice.find(L"合并提交") != std::wstring::npos);
}

GC_TEST(commit_history_fixture_refresh_picks_up_new_commits) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  BaseCommit(fixture, L"第一笔");

  WorkspaceSnapshot snapshot = Load(fixture, true);
  GC_REQUIRE(snapshot.model.recentCommits.size() == 1, "第一次读取应有一笔");
  const std::wstring firstId = snapshot.model.recentCommits[0].objectId;

  fixture.WriteFile(L"next.txt", "n\n");
  fixture.StageAll();
  fixture.Commit(L"刷新后要看到的新提交");
  snapshot = Load(fixture, true);  // 同一次刷新的第二次读取：新提交出现在最前。
  GC_REQUIRE(snapshot.model.recentCommits.size() == 2, "刷新后应读到两笔");
  GC_CHECK(snapshot.model.recentCommits[0].summary == L"刷新后要看到的新提交");
  // 旧条目的对象 ID 不变：界面据此保留仍存在的选中提交（按 objectId 记忆，见 list_view_memory）。
  const CommitItem* stillThere = FindById(snapshot.model.recentCommits, firstId);
  GC_CHECK_MESSAGE(stillThere != nullptr && stillThere->summary == L"第一笔",
                   "上一轮的提交应原样保留");
  GC_CHECK_MESSAGE(gc::app::ViewKeyForItem(snapshot.model.recentCommits[1]) == firstId,
                   "历史列表的选中记忆键必须是完整对象 ID");
}

GC_TEST(commit_history_fixture_detached_head_still_readable) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  const std::wstring first = BaseCommit(fixture, L"基线提交");
  fixture.WriteFile(L"next.txt", "n\n");
  fixture.StageAll();
  fixture.Commit(L"第二笔");
  const std::wstring second = fixture.HeadSha();

  fixture.RunCheckedInRepo({L"checkout", L"-q", L"--detach", first});
  const WorkspaceSnapshot snapshot = Load(fixture, true);
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Narrow(snapshot.message));
  GC_REQUIRE(snapshot.model.recentCommits.size() == 1, "游离 HEAD 只含它自己可达的一笔");
  GC_CHECK(snapshot.model.recentCommits[0].objectId == first);
  (void)second;
}

GC_TEST(commit_history_fixture_capped_at_display_limit) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  BaseCommit(fixture, L"基线提交");
  // 超过上限的空提交（仍在夹具自己的临时仓库里）：读取按 -n 截断，摘要必须注明。
  for (int index = 0; index < 102; ++index) {
    fixture.RunCheckedInRepo({L"commit", L"--quiet", L"--allow-empty", L"-m",
                              L"填充提交 " + std::to_wstring(index)});
  }
  const std::wstring head = fixture.HeadSha();

  const WorkspaceSnapshot snapshot = Load(fixture, true);
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Narrow(snapshot.message));
  GC_REQUIRE(snapshot.model.recentCommits.size() == gc::git::kRecentCommitLimit,
             "最多只展示 " + std::to_string(gc::git::kRecentCommitLimit) + " 条");
  GC_CHECK(snapshot.model.recentCommits[0].objectId == head);
  GC_CHECK(snapshot.message.find(std::wstring(L"已达上限 ") +
                                 std::to_wstring(gc::git::kRecentCommitLimit)) != std::wstring::npos);
  GC_CHECK(snapshot.message.find(L"更早的历史未读取") != std::wstring::npos);
}

GC_TEST(commit_history_fixture_show_plan_resolves_in_real_repo) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  BaseCommit(fixture, L"可被 show 的提交");
  fixture.WriteFile(L"next.txt", "n\n");
  fixture.StageAll();
  fixture.Commit(L"HEAD 这一笔");

  const WorkspaceSnapshot snapshot = Load(fixture, true);
  GC_REQUIRE(snapshot.model.recentCommits.size() == 2, "两笔都应读到");
  const CommitItem& head = snapshot.model.recentCommits[0];

  const gc::git::CommitShowPlan plan = gc::git::BuildCommitShowPlan(head);
  GC_REQUIRE(plan.allowed, "合法条目应给出方案");
  // 把方案原样交给真实 Git：命令窗口执行的就是这一串参数，退出码 0、输出含完整对象 ID。
  const gc::test::GitRun run = fixture.RunInRepo(plan.arguments);
  GC_REQUIRE_MESSAGE(run.Success(), "git show 方案应成功：exit=" + std::to_string(run.exitCode) +
                                        "\n" +
                                        Narrow(run.err));
  GC_CHECK(run.out.find(head.objectId) != std::wstring::npos);
  GC_CHECK(run.out.find(head.summary) != std::wstring::npos);
  GC_CHECK(run.out.find(L"Commit:") != std::wstring::npos);  // --format=fuller 的提交者元数据。

  // 被污染的 ID 一律拒绝执行；即便拿真实仓库的旧 ID 加上杂质，也只能停留在拒绝分支。
  CommitItem tampered = head;
  tampered.objectId += L" HEAD";
  const gc::git::CommitShowPlan refused = gc::git::BuildCommitShowPlan(tampered);
  GC_CHECK(!refused.allowed);
  GC_CHECK(refused.arguments.empty());
}
