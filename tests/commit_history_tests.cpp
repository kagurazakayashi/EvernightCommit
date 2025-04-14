// 「最近提交」纯逻辑测试：参数按可解析约定构造、NUL 五字段输出的分组解析、
// 非法输出整体作废，以及 git show 方案的参数校验。时间格式化一律注入确定性的桩，
// 真机时区换算由平台层（local_time）与夹具测试负责。
#include "support/tiny_test.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/commit_history.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/workspace_status.h"

namespace {

using gc::git::CommitItem;
using gc::git::CommitShowPlan;
using gc::git::RepoError;

// 40 位与 64 位十六进制对象 ID：分别对应 SHA-1 与 SHA-256 仓库的完整 ID，
// 解析与校验都不许硬编码 40。
const std::wstring kId40 = L"0123456789abcdef0123456789abcdef01234567";
const std::wstring kId64 =
    L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::wstring Nul() {
  std::wstring text;
  text.push_back(L'\0');
  return text;
}

// 一条记录：五个字段各以 NUL 结束（与 git log -z --format=%H%x00%an%x00%at%x00%s%x00%p 同形）。
std::wstring Record(std::wstring_view objectId, std::wstring_view author, std::wstring_view epoch,
                    std::wstring_view subject, std::wstring_view parents) {
  std::wstring text(objectId);
  text += Nul();
  text += author;
  text += Nul();
  text += epoch;
  text += Nul();
  text += subject;
  text += Nul();
  text += parents;
  text += Nul();
  return text;
}

// 确定性的时间桩：把秒数原样标出来，测试不依赖本机时区。
const gc::git::CommitTimeFormatter& StubFormatter() {
  static const gc::git::CommitTimeFormatter formatter =
      [](long long epochSeconds) { return L"T" + std::to_wstring(epochSeconds); };
  return formatter;
}

std::string Narrow(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

CommitItem MakeValidItem() {
  CommitItem item;
  item.objectId = kId40;
  item.author = L"测试者 <t@example.com>";
  item.summary = L"一次提交";
  item.authorEpochSeconds = 1'700'000'000;
  item.authoredAt = L"T1700000000";
  return item;
}

}  // namespace

GC_TEST(commit_history_arguments_follow_parsed_format_contract) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildRecentCommitsArguments(L"C:\\仓库 目录", 100);
  auto contains = [&](std::wstring_view needle) {
    for (const std::wstring& argument : arguments) {
      if (argument.find(needle) != std::wstring::npos) {
        return true;
      }
    }
    return false;
  };
  GC_CHECK(contains(L"-C"));
  GC_CHECK(contains(L"仓库 目录"));
  GC_CHECK(contains(L"--no-optional-locks"));
  GC_CHECK(contains(L"--no-replace-objects"));
  GC_CHECK(contains(L"log.showSignature=false"));
  GC_CHECK(contains(L"--no-decorate"));
  GC_CHECK(contains(L"-z"));
  GC_CHECK(contains(L"-n"));
  GC_CHECK(contains(L"100"));
  // 字段分隔必须是 %x00（NUL）：任何按空格/制表符拆列的形态都不允许出现在这个格式串里。
  GC_CHECK(contains(L"%H%x00%an%x00%at%x00%s%x00%p"));
}

GC_TEST(commit_history_parses_five_field_nul_records) {
  const std::wstring output = Record(kId40, L"张三", L"1700000000", L"基线提交", L"");
  const gc::git::CommitHistoryParseResult parsed =
      gc::git::ParseRecentCommits(output, gc::git::kRecentCommitLimit, StubFormatter());
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.commits.size() == 1, "应解析出一条提交");
  const CommitItem& item = parsed.commits[0];
  GC_CHECK(item.objectId == kId40);
  GC_CHECK(item.author == L"张三");
  GC_CHECK(item.summary == L"基线提交");
  GC_CHECK(item.authorEpochSeconds == 1'700'000'000LL);
  GC_CHECK(item.authoredAt == L"T1700000000");
  GC_CHECK(!item.isMergeCommit);
  GC_CHECK(!parsed.truncated);
}

GC_TEST(commit_history_keeps_chinese_tabs_and_long_text_verbatim) {
  // 标题含制表符、连续空格与引号形态的字符：切分只认 NUL，这些都必须原样回来。
  std::wstring longSubject(300, L'汉');
  longSubject = L"fix: a\tb  两个空格 \"q\" 's' %p & | <>" + longSubject;
  const std::wstring output = Record(kId40, L"名 字\t含制表", L"1700000001", longSubject,
                                     L"") + Record(kId64, L"", L"-120", L"", L"");
  const gc::git::CommitHistoryParseResult parsed =
      gc::git::ParseRecentCommits(output, gc::git::kRecentCommitLimit, StubFormatter());
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.commits.size() == 2, "两条记录都应按字段分组还原");
  GC_CHECK(parsed.commits[0].summary == longSubject);
  GC_CHECK(parsed.commits[0].author == L"名 字\t含制表");
  // SHA-256 仓库的 64 位完整 ID 同样合法：不硬编码 SHA-1 长度。
  GC_CHECK(parsed.commits[1].objectId == kId64);
  // 空标题、空作者、负秒数都是 Git 允许的真实提交，不算解析错误。
  GC_CHECK(parsed.commits[1].summary.empty());
  GC_CHECK(parsed.commits[1].author.empty());
  GC_CHECK(parsed.commits[1].authorEpochSeconds == -120);
}

GC_TEST(commit_history_detects_merge_commits) {
  const std::wstring firstParent = L"1111111111111111111111111111111111111111";
  const std::wstring secondParent = L"2222222222222222222222222222222222222222";
  const std::wstring output =
      Record(kId40, L"张三", L"1700000000", L"Merge branch 'x'",
             firstParent + L" " + secondParent) +
      Record(kId40, L"张三", L"1700000000", L"普通提交", firstParent) +
      Record(kId40, L"张三", L"1700000000", L"根提交", L"");
  const gc::git::CommitHistoryParseResult parsed =
      gc::git::ParseRecentCommits(output, gc::git::kRecentCommitLimit, StubFormatter());
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.commits.size() == 3, "三条记录都应解析成功");
  GC_CHECK(parsed.commits[0].isMergeCommit);
  GC_CHECK(!parsed.commits[1].isMergeCommit);
  GC_CHECK(!parsed.commits[2].isMergeCommit);
}

GC_TEST(commit_history_rejects_malformed_output_as_whole) {
  struct Case {
    const char* name;
    std::wstring text;
  };
  const std::wstring upperCaseId = L"0123456789ABCDEF0123456789ABCDEF01234567";
  const std::wstring shortId(23, L'a');
  const std::wstring tooLongId(65, L'a');
  const std::vector<Case> cases = {
      {"字段数不足一整条", Record(kId40, L"a", L"1700000000", L"s", L"") + kId40 + Nul() + L"b"},
      {"对象 ID 含大写", Record(upperCaseId, L"a", L"1700000000", L"s", L"")},
      {"对象 ID 过短", Record(shortId, L"a", L"1700000000", L"s", L"")},
      {"对象 ID 过长", Record(tooLongId, L"a", L"1700000000", L"s", L"")},
      {"时间字段非数字", Record(kId40, L"a", L"abc", L"s", L"")},
      {"时间字段为空", Record(kId40, L"a", L"", L"s", L"")},
      {"时间超出可判范围", Record(kId40, L"a", L"99999999999999999999", L"s", L"")},
  };
  for (const Case& item : cases) {
    const gc::git::CommitHistoryParseResult parsed =
        gc::git::ParseRecentCommits(item.text, gc::git::kRecentCommitLimit, StubFormatter());
    GC_CHECK_MESSAGE(!parsed.error.empty(), std::string("非法输出应报错：") + item.name);
    // 与 porcelain v2 同一原则：绝不交出解析出来的一半，否则界面会把一份看不准的历史摆给用户。
    GC_CHECK_MESSAGE(parsed.commits.empty(), std::string("非法输出必须丢弃半套列表：") + item.name);
  }
}

GC_TEST(commit_history_reports_truncation_at_limit) {
  const std::wstring two = Record(kId40, L"a", L"1", L"一", L"") + Record(kId40, L"a", L"2", L"二", L"");
  const gc::git::CommitHistoryParseResult capped = gc::git::ParseRecentCommits(two, 2, StubFormatter());
  GC_CHECK(capped.error.empty());
  GC_REQUIRE(capped.commits.size() == 2, "两条都应解析");
  GC_CHECK(capped.truncated);  // 恰好返回满额：更早的历史没有读取，界面必须注明。

  const gc::git::CommitHistoryParseResult below = gc::git::ParseRecentCommits(two, 100, StubFormatter());
  GC_CHECK(below.error.empty());
  GC_CHECK(!below.truncated);

  const gc::git::CommitHistoryParseResult empty =
      gc::git::ParseRecentCommits(L"", gc::git::kRecentCommitLimit, StubFormatter());
  GC_CHECK(empty.error.empty());
  GC_CHECK(empty.commits.empty());
  GC_CHECK(!empty.truncated);
}

GC_TEST(commit_history_time_formatter_may_be_absent) {
  const std::wstring output = Record(kId40, L"a", L"1700000000", L"s", L"");
  const gc::git::CommitHistoryParseResult parsed =
      gc::git::ParseRecentCommits(output, gc::git::kRecentCommitLimit, {});
  GC_CHECK(parsed.error.empty());
  GC_REQUIRE(parsed.commits.size() == 1, "无桩格式化也应解析出条目");
  GC_CHECK(parsed.commits[0].authoredAt.empty());
  GC_CHECK(parsed.commits[0].authorEpochSeconds == 1'700'000'000LL);
}

GC_TEST(object_id_helpers_respect_hash_lengths) {
  GC_CHECK(gc::git::LooksLikeFullObjectId(kId40));
  GC_CHECK(gc::git::LooksLikeFullObjectId(kId64));
  GC_CHECK(!gc::git::LooksLikeFullObjectId(L""));
  GC_CHECK(!gc::git::LooksLikeFullObjectId(std::wstring(23, L'a')));
  GC_CHECK(!gc::git::LooksLikeFullObjectId(std::wstring(65, L'a')));
  GC_CHECK(!gc::git::LooksLikeFullObjectId(L"0123456789ABCDEF0123456789ABCDEF01234567"));
  GC_CHECK(!gc::git::LooksLikeFullObjectId(L"0123456789abcdef0123456789abcdef0123456;"));

  GC_CHECK(gc::git::ShortObjectId(kId40) == L"01234567");
  GC_CHECK(gc::git::ShortObjectId(L"abc123") == L"abc123");  // 比展示长度更短：原样返回。
}

GC_TEST(commit_show_plan_builds_native_git_show) {
  CommitItem item = MakeValidItem();
  const CommitShowPlan plan = gc::git::BuildCommitShowPlan(item);
  GC_REQUIRE(plan.allowed, "合法对象 ID 应给出可执行方案");
  GC_CHECK(plan.operationId == L"commit-show");
  GC_CHECK(plan.arguments.back() == item.objectId);  // 用完整 ID，绝不拿短 ID 去执行。
  auto contains = [&](std::wstring_view needle) {
    for (const std::wstring& argument : plan.arguments) {
      if (argument.find(needle) != std::wstring::npos) {
        return true;
      }
    }
    return false;
  };
  GC_CHECK(contains(L"show"));
  GC_CHECK(contains(L"--format=fuller"));  // 元数据：作者与提交者、两个时间与偏移。
  GC_CHECK(contains(L"--no-ext-diff"));
  GC_CHECK(contains(L"--no-textconv"));
  GC_CHECK(contains(L"--submodule=short"));
  // 绝不能出现选项终止符：實測 `git show -- <id>` 把 ID 当路径，窗口里什么都不显示。
  GC_CHECK(std::find(plan.arguments.begin(), plan.arguments.end(), L"--") == plan.arguments.end());
  GC_CHECK(plan.notice.empty());

  item.isMergeCommit = true;
  const CommitShowPlan mergePlan = gc::git::BuildCommitShowPlan(item);
  GC_CHECK(mergePlan.allowed);
  GC_CHECK(mergePlan.notice.find(L"合并提交") != std::wstring::npos);
}

GC_TEST(commit_show_plan_refuses_untrusted_object_id) {
  CommitItem item = MakeValidItem();
  item.objectId = L"0123456789abcdef0123456789abcdef01234567 HEAD";  // 被污染的一段文本
  const CommitShowPlan plan = gc::git::BuildCommitShowPlan(item);
  GC_CHECK(!plan.allowed);
  GC_CHECK(plan.arguments.empty());  // 拒绝时一个参数都不给：不存在「顺手执行一半」。
  GC_CHECK(!plan.refusalReason.empty());

  CommitItem empty = MakeValidItem();
  empty.objectId.clear();
  GC_CHECK(!gc::git::BuildCommitShowPlan(empty).allowed);
}

GC_TEST(recent_commits_notice_texts) {
  GC_CHECK(gc::git::BuildRecentCommitsNotice(0, false).empty());
  const std::wstring few = gc::git::BuildRecentCommitsNotice(7, false);
  GC_CHECK(few.find(L"7 条") != std::wstring::npos);
  GC_CHECK(few.find(L"上限") == std::wstring::npos);
  const std::wstring capped =
      gc::git::BuildRecentCommitsNotice(gc::git::kRecentCommitLimit, true);
  GC_CHECK(capped.find(std::wstring(L"已达上限 ") + std::to_wstring(gc::git::kRecentCommitLimit)) !=
           std::wstring::npos);
  GC_CHECK(capped.find(L"更早的历史未读取") != std::wstring::npos);
}

GC_TEST(workspace_load_skips_log_without_commits_and_survives_log_failure) {
  // 用桩执行器验证装配规则：仓库识别说 HEAD 不可解析时根本不追问 git log；
  // log 那一段单独失败时，文件列表照常落地，失败只记在 historyError/historyMessage 上。
  int logCalls = 0;
  gc::platform::WorkspaceStatusDeps stub;
  stub.runner = [&](const std::wstring&, const std::wstring&,
                    const std::vector<std::wstring>& arguments) {
    bool isLog = false;
    for (const std::wstring& argument : arguments) {
      if (argument == L"log") {
        isLog = true;
      }
    }
    gc::git::GitQueryResult result;
    result.started = true;
    result.exited = true;
    if (isLog) {
      ++logCalls;
      result.exitCode = 128;  // 真实形态：log 自身失败（例如对象库损坏）。
      result.utf16Error = L"fatal: bad object HEAD";
      return result;
    }
    result.exitCode = 0;
    std::wstring status(L"1 .M N... 100644 100644 100644 0 0 base.txt");
    status += Nul();
    result.utf16Output = status;
    return result;
  };

  gc::platform::WorkspaceStatusRequest request;
  request.exePath = L"git.exe";
  request.repositoryDirectory = L"C:\\仓库";
  request.timeoutMilliseconds = 1000;

  request.repositoryHasCommits = false;
  gc::git::WorkspaceSnapshot unborn = gc::platform::LoadWorkspaceStatus(request, stub);
  GC_CHECK(unborn.status == gc::git::WorkspaceLoadStatus::loaded);
  GC_CHECK(unborn.model.unstaged.size() == 1);
  GC_CHECK(unborn.model.recentCommits.empty());
  GC_CHECK(unborn.historyError == RepoError::none);
  GC_CHECK(logCalls == 0);  // 空仓库不发 git log：那条命令在那种仓库里必然非 0 退出。

  request.repositoryHasCommits = true;
  gc::git::WorkspaceSnapshot failed = gc::platform::LoadWorkspaceStatus(request, stub);
  GC_CHECK(failed.status == gc::git::WorkspaceLoadStatus::loaded);
  GC_CHECK(logCalls == 1);
  GC_CHECK(failed.model.unstaged.size() == 1);               // 列表不受牵连。
  GC_CHECK(failed.model.recentCommits.empty());
  GC_CHECK(failed.historyError != RepoError::none);           // 失败单独归在这段读取上。
  GC_CHECK(!failed.historyMessage.empty());
  GC_CHECK(failed.message.find(L"提交历史") != std::wstring::npos ||
          failed.message.find(L"提交") != std::wstring::npos);

  // 解析失败的输出同样只作废历史：字段数不是一整条的输出 → badOutput，列表照常。
  stub.runner = [&](const std::wstring&, const std::wstring&,
                    const std::vector<std::wstring>& arguments) {
    gc::git::GitQueryResult result;
    result.started = true;
    result.exited = true;
    result.exitCode = 0;
    bool isLog = false;
    for (const std::wstring& argument : arguments) {
      if (argument == L"log") {
        isLog = true;
      }
    }
    if (isLog) {
      result.utf16Output = kId40 + Nul() + L"a";  // 半条记录
    } else {
      std::wstring status(L"1 .M N... 100644 100644 100644 0 0 base.txt");
      status += Nul();
      result.utf16Output = status;
    }
    return result;
  };
  gc::git::WorkspaceSnapshot badOutput = gc::platform::LoadWorkspaceStatus(request, stub);
  GC_CHECK(badOutput.status == gc::git::WorkspaceLoadStatus::loaded);
  GC_CHECK(badOutput.model.unstaged.size() == 1);
  GC_CHECK(badOutput.historyError == gc::git::RepoError::badOutput);
}
