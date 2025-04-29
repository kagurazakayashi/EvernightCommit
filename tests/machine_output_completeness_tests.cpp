#include "support/tiny_test.h"

#include <initializer_list>
#include <string>
#include <vector>

#include "git/author_config.h"
#include "git/commit_history.h"
#include "git/pull_plan.h"
#include "git/push_plan.h"
#include "git/repository.h"
#include "git/undo_commit_plan.h"
#include "git/workspace_status.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::GitQueryResult;

const std::wstring kObjectId(40, L'1');
const std::wstring kParentId(40, L'2');

// 把片段按 Git 的 -z / --null 形态连起来：每条记录（含最后一个片段）都以 NUL 收尾。
// 一律显式 push_back，不用字面量里的 \0 —— 那个会被当成 C 字符串的结尾，测试自己就先失真了。
std::wstring NulJoined(const std::vector<std::wstring>& records) {
  std::wstring text;
  for (const std::wstring& record : records) {
    text.append(record);
    text.push_back(L'\0');
  }
  return text;
}

// 去掉最后那个终止 NUL：模拟「输出在记录中间就断了」。
std::wstring DropTerminator(std::wstring terminated) {
  if (!terminated.empty() && terminated.back() == L'\0') {
    terminated.pop_back();
  }
  return terminated;
}

// 一条「Git 正常回答」的结果：退出码 0 加给定输出；完整度默认为 true，残缺时显式改。
GitQueryResult Answered(std::wstring output, int exitCode = 0) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::move(output);
  return result;
}

GitQueryResult IncompleteAnswer(std::wstring partialOutput, std::wstring reason, int exitCode = 0) {
  GitQueryResult result = Answered(std::move(partialOutput), exitCode);
  result.outputComplete = false;
  result.incompleteReason = std::move(reason);
  return result;
}

// 形态与本机 Git 的 --porcelain=v2 -z 一致：普通记录 8 个字段后接路径，
// 重命名记录 9 个字段后接目标路径，来源路径是紧随其后的另一个 NUL 片段。
std::wstring PorcelainModified(std::wstring_view path) {
  return L"1 .M N... 100644 100644 100644 " + kObjectId + L" " + kParentId + L" " +
         std::wstring(path);
}

// 重命名/复制在 -z 下是「两条连着算一条记录」：目标路径在前，来源路径紧随其后。
std::vector<std::wstring> PorcelainRename(std::wstring_view target, std::wstring_view source) {
  return {L"2 RM N... 100644 100644 100644 " + kObjectId + L" " + kParentId + L" R100 " +
              std::wstring(target),
          std::wstring(source)};
}

std::vector<std::wstring> HistoryFields(std::wstring_view summary) {
  return {std::wstring(kObjectId), L" tester <t@example.com>", L"1700000000", std::wstring(summary),
          std::wstring(kParentId)};
}

std::wstring ConfigFields(std::wstring_view key, std::wstring_view value) {
  return std::wstring(key) + L"\n" + std::wstring(value);
}

// 断言失败时把宽字符说明带进报告（tiny_test 的报告只收 std::string）。
std::string Utf8Of(std::wstring_view text) {
  return gc::platform::Utf16ToUtf8(text);
}

}  // namespace

// ---- NUL 分隔的记录边界约定 ----

GC_TEST(completeness_nul_records_accept_empty_and_terminated_output) {
  std::wstring reason;
  // 空输出是合法答复（干净工作区、零条目），不能当成读取失败。
  GC_CHECK_MESSAGE(gc::git::NulRecordsAreComplete(std::wstring(), reason), "空输出应判为完整");
  GC_CHECK(reason.empty());
  GC_CHECK(gc::git::NulRecordsAreComplete(NulJoined({L"first", L"second"}), reason));
  GC_CHECK(gc::git::NulRecordsAreComplete(std::wstring(1, L'\0'), reason));  // 单条空记录交给解析器判
}

GC_TEST(completeness_nul_records_refuse_unterminated_last_record) {
  std::wstring reason;
  GC_CHECK_MESSAGE(!gc::git::NulRecordsAreComplete(DropTerminator(NulJoined({L"first", L"second"})),
                                                   reason),
                   "最后一条缺终止 NUL 必须判为残缺");
  GC_CHECK(!reason.empty());
  // 残缺内容本身不进说明文字：那可能是半个路径，或半条带凭据的配置值。
  GC_CHECK_MESSAGE(reason.find(L"second") == std::wstring::npos, "说明里不该抄出残缺片段的原文");
}

GC_TEST(completeness_porcelain_rejects_half_a_filename) {
  // 最后一条只有半个路径名（缺终止 NUL）：整份作废，不能交出「少一条」的列表。
  const std::wstring output =
      DropTerminator(NulJoined({PorcelainModified(L"alpha.txt"), PorcelainModified(L"beta.txt")}));
  const gc::git::WorkspaceStatusParseResult parsed = gc::git::ParseWorkspacePorcelainV2(output);
  GC_CHECK(!parsed.error.empty());
  GC_CHECK_MESSAGE(parsed.model.unstaged.empty() && parsed.model.staged.empty(),
                   "残缺输出不得留下半套模型");
}

GC_TEST(completeness_porcelain_empty_output_is_a_clean_workspace) {
  const gc::git::WorkspaceStatusParseResult parsed = gc::git::ParseWorkspacePorcelainV2(std::wstring());
  GC_CHECK(parsed.error.empty());
  GC_CHECK(parsed.model.unstaged.empty() && parsed.model.staged.empty());
}

GC_TEST(completeness_porcelain_rename_without_source_path_is_refused) {
  // 记录边界是完整的（目标路径带终止 NUL），但紧随其后的来源那一段没了：
  // 不能当成「单边改名」进方案，否则暂存只会挪走一边。
  const std::wstring output = NulJoined({PorcelainRename(L"new/here.txt", L"old/there.txt").front()});
  const gc::git::WorkspaceStatusParseResult parsed = gc::git::ParseWorkspacePorcelainV2(output);
  GC_CHECK_MESSAGE(!parsed.error.empty(), Utf8Of(parsed.error));
  GC_CHECK_MESSAGE(parsed.error.find(L"来源路径") != std::wstring::npos, "说明要点名缺的是来源路径");
  GC_CHECK(parsed.model.staged.empty());
}

GC_TEST(completeness_porcelain_rename_with_both_paths_is_kept) {
  const std::wstring output = NulJoined(PorcelainRename(L"new/here.txt", L"old/there.txt"));
  const gc::git::WorkspaceStatusParseResult parsed = gc::git::ParseWorkspacePorcelainV2(output);
  GC_CHECK_MESSAGE(parsed.error.empty(), Utf8Of(parsed.error));
  GC_CHECK_MESSAGE(parsed.model.staged.size() == 1, "完整的一条重命名应解析出一项目标变化");
  if (!parsed.model.staged.empty()) {
    GC_CHECK(parsed.model.staged.front().path == L"new/here.txt");
    GC_CHECK(parsed.model.staged.front().oldPath == L"old/there.txt");
  }
}

GC_TEST(completeness_history_rejects_record_without_terminating_nul) {
  // 截断恰好落在最后一个字段（父提交）时，字段数仍是 5 的倍数：只能靠记录边界拦住。
  const std::wstring output =
      DropTerminator(NulJoined(HistoryFields(L"标题"))) + std::wstring(kObjectId) + L"\n";
  const gc::git::CommitHistoryParseResult parsed =
      gc::git::ParseRecentCommits(output, gc::git::kRecentCommitLimit, {});
  GC_CHECK_MESSAGE(!parsed.error.empty(), "缺终止 NUL 的提交记录必须整体拒绝");
  GC_CHECK(parsed.commits.empty());
}

GC_TEST(completeness_history_accepts_terminated_records_and_empty_output) {
  std::wstring reason;
  GC_CHECK(gc::git::NulRecordsAreComplete(NulJoined(HistoryFields(L"标题")), reason));
  const gc::git::CommitHistoryParseResult parsed =
      gc::git::ParseRecentCommits(NulJoined(HistoryFields(L"标题")), gc::git::kRecentCommitLimit, {});
  GC_CHECK(parsed.error.empty());
  GC_CHECK(parsed.commits.size() == 1);

  const gc::git::CommitHistoryParseResult empty =
      gc::git::ParseRecentCommits(std::wstring(), gc::git::kRecentCommitLimit, {});
  GC_CHECK_MESSAGE(empty.error.empty(), "空历史（仓库尚无提交）不是读取失败");
  GC_CHECK(empty.commits.empty());
}

// ---- 配置与清单类输出 ----

GC_TEST(completeness_push_config_refuses_truncated_value) {
  // 最后一条配置的值被截断（缺终止 NUL）：半条 URL 也像一条 URL，不能据此定发布地点。
  const GitQueryResult listing = Answered(
      DropTerminator(NulJoined({ConfigFields(L"remote.origin.url", L"file:///d:/repo.git"),
                                ConfigFields(L"branch.main.remote", L"git@host:path/one")})));
  const gc::git::PushConfigListing parsed = gc::git::ParsePushConfigListing(listing);
  GC_CHECK_MESSAGE(!parsed.readOk && !parsed.readFailure.empty(), "残缺配置清单不得采信");
  GC_CHECK(parsed.entries.empty());
}

GC_TEST(completeness_push_config_accepts_whole_records) {
  const GitQueryResult listing = Answered(NulJoined({ConfigFields(L"remote.origin.url", L"origin"),
                                                     ConfigFields(L"remote.origin.url",
                                                                  L"file:///d:/second.git")}));
  const gc::git::PushConfigListing parsed = gc::git::ParsePushConfigListing(listing);
  GC_CHECK_MESSAGE(parsed.readOk, Utf8Of(parsed.readFailure));
  GC_CHECK(parsed.entries.size() == 2);
}

GC_TEST(completeness_push_config_refusal_does_not_dump_the_record) {
  // 认不出来的那条记录里可能带凭据：拒绝说明只能给限长且掩码过的摘要。
  // 注意「无分隔符」本身是合法形态（省略取值的布尔裸键），真正认不出的是
  // 变量名段（最后一个点之后）带空格的记录——Git 不可能输出那种键。
  const std::wstring secret = L"https://pusher:secr3t-token@host.example/private";
  const GitQueryResult listing = Answered(std::wstring(L"leak ") + secret +
                                          std::wstring(1, L'\0'));
  const gc::git::PushConfigListing parsed = gc::git::ParsePushConfigListing(listing);
  GC_CHECK_MESSAGE(!parsed.readOk, "变量名段含非法字符的无分隔记录应被拒绝");
  GC_CHECK_MESSAGE(parsed.readFailure.find(L"secr3t-token") == std::wstring::npos,
                   "拒绝说明不得抄出可能的口令");
}

GC_TEST(completeness_push_config_accepts_valueless_boolean_record) {
  // 省略取值的布尔裸键（`key<NUL>`，本机 `git config --list --null` 实测形态）是合法记录，
  // 不得因为「既没有换行也没有 `=`」把整份配置判死——那正是旧实现把合法仓库拒之门外的缺陷。
  const GitQueryResult listing =
      Answered(std::wstring(L"remote.origin.mirror") + std::wstring(1, L'\0') +
               ConfigFields(L"remote.origin.url", L"file:///d:/repo.git") +
               std::wstring(1, L'\0'));
  const gc::git::PushConfigListing parsed = gc::git::ParsePushConfigListing(listing);
  GC_CHECK_MESSAGE(parsed.readOk, Utf8Of(parsed.readFailure));
  GC_CHECK(parsed.entries.size() == 2);
  const gc::git::PushConfigEntry* mirror = parsed.LastEntry(L"remote.origin.mirror");
  GC_CHECK(mirror != nullptr);
  GC_CHECK(mirror != nullptr && mirror->valueOmitted);
}

GC_TEST(completeness_identity_config_refuses_truncated_value) {
  const GitQueryResult query = Answered(L"dev@example.or");  // --null 的结尾 NUL 没读到
  const gc::git::ConfigValueRead read = gc::git::ParseIdentityConfigQuery(query, L"user.email");
  GC_CHECK(read.state == gc::git::ConfigValueState::failed);
  GC_CHECK(read.error == gc::git::RepoError::badOutput);
}

GC_TEST(completeness_identity_config_accepts_nul_terminated_value) {
  const GitQueryResult query = Answered(std::wstring(L" dev@example.com ") + std::wstring(1, L'\0'));
  const gc::git::ConfigValueRead read = gc::git::ParseIdentityConfigQuery(query, L"user.email");
  GC_CHECK(read.state == gc::git::ConfigValueState::present);
  GC_CHECK(read.value == L" dev@example.com ");  // 头尾空白原样保留，由使用方决定修剪
}

// ---- 完整度在各类判读点之前拦住 ----

GC_TEST(completeness_gate_refuses_incomplete_stdout) {
  std::wstring detail;
  const gc::git::RepoError failure =
      gc::git::ClassifyGitFailure(IncompleteAnswer(std::wstring(), L"标准输出超过上限被截断"), detail);
  GC_CHECK_MESSAGE(failure == gc::git::RepoError::outputIncomplete,
                   "读不全的 stdout 先判为输出读取不完整，而不是「成功且无输出」");
  GC_CHECK(detail.find(L"截断") != std::wstring::npos);

  const gc::git::UndoQueryRead read =
      gc::git::ReadUndoQuery(IncompleteAnswer(L"refs/heads/ma", L"标准输出未能读到结尾"));
  GC_CHECK(read.outcome == gc::git::UndoQueryOutcome::failed);
  GC_CHECK(read.error == gc::git::RepoError::outputIncomplete);
}

GC_TEST(completeness_incomplete_stdout_is_not_read_as_absent_config) {
  // 退出码 1 加上读不全的输出：只能说「不知道」，不能说「这个 key 没设过」。
  const GitQueryResult query = IncompleteAnswer(std::wstring(), L"标准输出读取失败", 1);
  const gc::git::ConfigValueRead read = gc::git::ParseIdentityConfigQuery(query, L"user.name");
  GC_CHECK(read.state == gc::git::ConfigValueState::failed);
  GC_CHECK(read.error == gc::git::RepoError::outputIncomplete);
}

GC_TEST(completeness_labels_separate_incomplete_from_unparsable) {
  const std::wstring_view incomplete = gc::git::RepoErrorLabel(gc::git::RepoError::outputIncomplete);
  const std::wstring_view badOutput = gc::git::RepoErrorLabel(gc::git::RepoError::badOutput);
  GC_CHECK(incomplete != badOutput);
  GC_CHECK(incomplete.find(L"不完整") != std::wstring_view::npos);
  // 界面文案要给出可操作的说明，而不是只留一个空列表。
  const std::wstring text =
      gc::git::BuildRepoErrorDetail(gc::git::RepoError::outputIncomplete, L"标准输出超过上限被截断");
  GC_CHECK(text.find(L"重新读取") != std::wstring::npos);
}

GC_TEST(completeness_launch_failure_reason_reaches_the_caller) {
  GitQueryResult failed;  // 没启动
  failed.launchDetail = L"系统找不到指定的文件。 (2)";
  std::wstring detail;
  GC_CHECK(gc::git::ClassifyGitFailure(failed, detail) == gc::git::RepoError::gitLaunchFailed);
  GC_CHECK_MESSAGE(detail.find(L"系统找不到指定的文件") != std::wstring::npos,
                   "启动失败的 Win32 说明要传播给调用方");
  const gc::git::UndoQueryRead read = gc::git::ReadUndoQuery(failed);
  GC_CHECK(read.detail.find(L"系统找不到指定的文件") != std::wstring::npos);
}

GC_TEST(completeness_pull_incoming_list_refuses_unterminated_record) {
  gc::git::PullRelationshipQueries queries;
  queries.aheadBehind = Answered(L"0\t1\n");
  queries.incomingRan = true;
  queries.incoming = Answered(DropTerminator(NulJoined({L"one.txt", L"two.txt"})));
  const gc::git::PullRelationshipFacts facts = gc::git::InterpretPullRelationship(queries);
  GC_CHECK_MESSAGE(!facts.incomingOk, "残缺的文件清单不能当作完整答复");
  GC_CHECK(facts.incomingPaths.empty());
  GC_CHECK(!facts.incomingDetail.empty());
}

GC_TEST(completeness_pull_conflict_list_refuses_unterminated_record) {
  const gc::git::PullConflictState state = gc::git::InterpretPullConflictState(
      Answered(DropTerminator(NulJoined({L"conflicted.txt", L"another file"}))),
      Answered(L"ref: refs/heads/main\n"), Answered(kObjectId + L"\n"));
  GC_CHECK_MESSAGE(!state.readOk && !state.readFailure.empty(), "冲突清单残缺时不得宣布读取成功");
  GC_CHECK(state.conflictPaths.empty());
}

GC_TEST(completeness_pull_merge_tree_conflict_list_needs_complete_output) {
  // 退出码 1 本身可以说「有冲突」，但要列出哪几个文件就得靠完整输出。
  gc::git::PullRelationshipQueries queries;
  queries.aheadBehind = Answered(L"0\t1\n");
  queries.mergeTreeRan = true;
  GitQueryResult mergeTree = IncompleteAnswer(
      L"3333333333333333333333333333333333333333\nconflicted.txt\nCONFL", L"标准输出超过上限被截断", 1);
  queries.mergeTree = std::move(mergeTree);
  const gc::git::PullRelationshipFacts facts = gc::git::InterpretPullRelationship(queries);
  GC_CHECK(facts.dryRun == gc::git::PullMergeDryRun::failed);
  GC_CHECK_MESSAGE(facts.dryRunConflicts.empty(), "读不全时不交出可能缺条目的冲突表");
  GC_CHECK(facts.dryRunDetail.find(L"完整") != std::wstring::npos);
}
