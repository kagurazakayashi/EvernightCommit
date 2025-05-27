// 「提交历史」里那份「文件变化」清单的纯逻辑测试：查询参数按可解析约定构造、
// name-status 的 NUL 分组解析（含重命名/复制的旧、新两条路径）、非法输出整体作废，
// 以及双击「查看该文件详细差异」的 git show 方案校验。
// 一律不碰 Win32、不启子进程：真实 Git 的输出形态由 commit_changes_fixture_tests 负责核对。
#include "support/tiny_test.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_changes.h"
#include "git/commit_history.h"
#include "git/workspace_model.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::CommitChangesParseResult;
using gc::git::CommitFileDiffPlan;
using gc::git::CommitItem;

// 与本工程其它解析测试同一套 40/64 位十六进制对象 ID 样本：不硬编码 SHA-1 长度。
const std::wstring kId40 = L"0123456789abcdef0123456789abcdef01234567";
const std::wstring kId64 =
    L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::wstring Nul() {
  std::wstring text;
  text.push_back(L'\0');
  return text;
}

// 一条「状态字母 + 单一路径」记录（普通条目），与实测的 `M\0a.txt\0` 同形。
std::wstring Record(std::wstring_view status, std::wstring_view path) {
  std::wstring text(status);
  text += Nul();
  text += path;
  text += Nul();
  return text;
}

// 一条重命名/复制记录：`R100\0old\0new\0`（相似度与字母同段，旧、新各占一条路径字段）。
std::wstring RenameRecord(std::wstring_view status, std::wstring_view oldPath, std::wstring_view newPath) {
  std::wstring text(status);
  text += Nul();
  text += oldPath;
  text += Nul();
  text += newPath;
  text += Nul();
  return text;
}

CommitItem MakeCommit(std::wstring_view objectId = kId40, bool merge = false) {
  CommitItem item;
  item.objectId = std::wstring(objectId);
  item.summary = L"一次提交";
  item.author = L"测试者 <t@example.com>";
  item.authorEpochSeconds = 1'700'000'000;
  item.authoredAt = L"T1700000000";
  item.isMergeCommit = merge;
  return item;
}

bool ArgumentsContain(const std::vector<std::wstring>& arguments, std::wstring_view needle) {
  for (const std::wstring& argument : arguments) {
    if (argument.find(needle) != std::wstring::npos) {
      return true;
    }
  }
  return false;
}

size_t CountOf(const std::vector<std::wstring>& arguments, std::wstring_view exact) {
  size_t count = 0;
  for (const std::wstring& argument : arguments) {
    if (argument == exact) {
      ++count;
    }
  }
  return count;
}

std::string Narrow(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

}  // namespace

GC_TEST(commit_changes_arguments_follow_name_status_contract) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildCommitChangesArguments(L"C:\\仓库 目录", kId40);
  GC_CHECK(ArgumentsContain(arguments, L"-C"));
  GC_CHECK(ArgumentsContain(arguments, L"仓库 目录"));
  GC_CHECK(ArgumentsContain(arguments, L"--no-optional-locks"));
  GC_CHECK(ArgumentsContain(arguments, L"--no-replace-objects"));
  GC_CHECK(ArgumentsContain(arguments, L"core.quotepath=off"));
  GC_CHECK(ArgumentsContain(arguments, L"log.showSignature=false"));
  GC_CHECK(ArgumentsContain(arguments, L"show"));
  // 只要文件名清单：--format= 关掉提交头，--name-status 列状态与路径。
  GC_CHECK(CountOf(arguments, L"--format=") == 1);
  GC_CHECK(CountOf(arguments, L"--name-status") == 1);
  GC_CHECK(CountOf(arguments, L"--find-renames") == 1);
  // 合并提交相对第一个父提交给出变化；对普通提交是无副作用的同义项。
  GC_CHECK(CountOf(arguments, L"--first-parent") == 1);
  GC_CHECK(CountOf(arguments, L"-z") == 1);
  // 对象 ID 必须排在最后、且不套 `--`：git show <oid> -- <path> 会把 ID 当路径限定。
  GC_REQUIRE(!arguments.empty(), "参数不应为空");
  GC_CHECK(arguments.back() == kId40);
  GC_CHECK(CountOf(arguments, L"--") == 0);
}

GC_TEST(commit_changes_parses_modified_added_deleted_typed) {
  std::wstring output = Record(L"M", L"a.txt");
  output += Record(L"A", L"新建 文件.txt");
  output += Record(L"D", L"gone.txt");
  output += Record(L"T", L"link");
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(output);
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.changes.size() == 4, "应解析出四条文件变化");
  GC_CHECK(parsed.changes[0].kind == ChangeKind::modified);
  GC_CHECK(parsed.changes[0].path == L"a.txt");
  GC_CHECK(parsed.changes[0].statusCode == L"M");
  GC_CHECK(parsed.changes[1].kind == ChangeKind::added);
  GC_CHECK(parsed.changes[1].path == L"新建 文件.txt");
  GC_CHECK(parsed.changes[2].kind == ChangeKind::deleted);
  GC_CHECK(parsed.changes[3].kind == ChangeKind::typeChange);
}

GC_TEST(commit_changes_parses_rename_with_old_and_new_paths) {
  const std::wstring output = RenameRecord(L"R100", L"旧 名称.txt", L"new name.txt");
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(output);
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.changes.size() == 1, "应解析出一条重命名记录");
  const ChangeItem& item = parsed.changes[0];
  GC_CHECK(item.kind == ChangeKind::renamed);
  GC_CHECK(item.oldPath == L"旧 名称.txt");
  GC_CHECK(item.path == L"new name.txt");
  GC_CHECK(item.similarity == L"R100");
  // 展示文本与工作区列表同一套约定：重命名显示「旧 → 新」（分隔符是 U+2192，不是 ASCII 的 ->）。
  GC_CHECK(item.PathLabel() == L"旧 名称.txt → new name.txt");
  GC_CHECK(item.PathLabel().find(L"\u2192") != std::wstring::npos);
}

GC_TEST(commit_changes_parses_copy_record) {
  const std::wstring output = RenameRecord(L"C75", L"src.txt", L"dst.txt");
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(output);
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.changes.size() == 1, "应解析出一条复制记录");
  GC_CHECK(parsed.changes[0].kind == ChangeKind::copied);
  GC_CHECK(parsed.changes[0].similarity == L"C75");
  GC_CHECK(parsed.changes[0].oldPath == L"src.txt");
  GC_CHECK(parsed.changes[0].path == L"dst.txt");
}

GC_TEST(commit_changes_keeps_special_path_bytes_verbatim) {
  // 制表符、连续空格、通配符字符、中文：切分只认 NUL，路径必须原样回来（core.quotepath=off）。
  const std::wstring tricky = L"目录/a\tb  两空格 [x] *.txt";
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(Record(L"M", tricky));
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_REQUIRE(parsed.changes.size() == 1, "应解析出一条记录");
  GC_CHECK(parsed.changes[0].path == tricky);
}

GC_TEST(commit_changes_empty_output_is_legitimately_no_changes) {
  // 空清单是合法事实（空提交，或合并提交的变化都在第一个父提交里），不是读取失败。
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(L"");
  GC_CHECK_MESSAGE(parsed.error.empty(), Narrow(parsed.error));
  GC_CHECK(parsed.changes.empty());
}

GC_TEST(commit_changes_rejects_record_missing_trailing_nul) {
  // 最后一条没有结尾 NUL：可能是半个文件名，整份作废。
  const std::wstring truncated = std::wstring(L"M") + Nul() + L"a.txt";
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(truncated);
  GC_CHECK(!parsed.error.empty());
  GC_CHECK(parsed.changes.empty());
}

GC_TEST(commit_changes_rejects_unknown_status_letter) {
  // 未合并（U）不会出现在某个提交的树里；遇到它说明输出形态变了，整份作废。
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(Record(L"U", L"a.txt"));
  GC_CHECK(!parsed.error.empty());
  GC_CHECK(parsed.changes.empty());
}

GC_TEST(commit_changes_rejects_partial_record_and_empties_earlier_ones) {
  // 前一条是好的，后一条缺字段：不能「留下好的那半份」——一旦开不了口就整份作废。
  std::wstring output = Record(L"M", L"good.txt");
  output += RenameRecord(L"R100", L"only-old.txt", L"");
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(output);
  GC_CHECK(!parsed.error.empty());
  GC_CHECK(parsed.changes.empty());
}

GC_TEST(commit_changes_rejects_rename_missing_new_path) {
  std::wstring output = std::wstring(L"R100") + Nul() + L"old.txt" + Nul();
  const CommitChangesParseResult parsed = gc::git::ParseCommitChanges(output);
  GC_CHECK(!parsed.error.empty());
  GC_CHECK(parsed.changes.empty());
}

GC_TEST(commit_changes_rejects_empty_path_field) {
  const CommitChangesParseResult parsed =
      gc::git::ParseCommitChanges(std::wstring(L"M") + Nul() + Nul());
  GC_CHECK(!parsed.error.empty());
  GC_CHECK(parsed.changes.empty());
}

GC_TEST(commit_changes_plan_allows_valid_non_rename_change) {
  const CommitFileDiffPlan plan =
      gc::git::BuildCommitFileDiffPlan(MakeCommit(), ChangeItem{ChangeKind::modified, L"M", L"a b.txt"});
  GC_REQUIRE_MESSAGE(plan.allowed, Narrow(plan.refusalReason));
  GC_CHECK(ArgumentsContain(plan.arguments, L"show"));
  GC_CHECK(ArgumentsContain(plan.arguments, L"--format=fuller"));
  GC_CHECK(ArgumentsContain(plan.arguments, L"--submodule=short"));
  // 对象 ID 是修订，排在 `--` 之前；只有一条字面 pathspec 落在 `--` 之后。
  const size_t separator = static_cast<size_t>(
      std::find(plan.arguments.begin(), plan.arguments.end(), std::wstring(L"--")) -
      plan.arguments.begin());
  GC_REQUIRE(separator < plan.arguments.size(), "参数里应有 `--` 分隔符");
  GC_REQUIRE(separator + 1 < plan.arguments.size(), "`--` 之后应有 pathspec");
  GC_CHECK(plan.arguments[separator - 1] == kId40);
  GC_CHECK(plan.arguments[separator + 1] == L":(literal)a b.txt");
  GC_CHECK(plan.arguments.size() == separator + 2);
  GC_CHECK(plan.operationId == L"commit-file-show");
  GC_CHECK(plan.notice.empty());
}

GC_TEST(commit_changes_plan_limits_rename_to_both_old_and_new_paths) {
  ChangeItem change{ChangeKind::renamed, L"R100", L"new.txt"};
  change.oldPath = L"old.txt";
  change.similarity = L"R100";
  const CommitFileDiffPlan plan = gc::git::BuildCommitFileDiffPlan(MakeCommit(), change);
  GC_REQUIRE_MESSAGE(plan.allowed, Narrow(plan.refusalReason));
  GC_CHECK(CountOf(plan.arguments, L":(literal)new.txt") == 1);
  GC_CHECK(CountOf(plan.arguments, L":(literal)old.txt") == 1);
}

GC_TEST(commit_changes_plan_rejects_object_id_not_lowercase_hex) {
  CommitItem uppercase = MakeCommit();
  uppercase.objectId = L"0123456789ABCDEF0123456789ABCDEF01234567";
  const CommitFileDiffPlan plan =
      gc::git::BuildCommitFileDiffPlan(uppercase, ChangeItem{ChangeKind::modified, L"M", L"a.txt"});
  GC_CHECK(!plan.allowed);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(!plan.refusalReason.empty());
}

GC_TEST(commit_changes_plan_rejects_escaping_and_absolute_paths) {
  const std::vector<std::wstring> bad = {L"../escape.txt", L"/abs.txt", L"C:/abs.txt",
                                         L"dir/../../x", L"", L"a\\..\\b"};
  for (const std::wstring& path : bad) {
    const CommitFileDiffPlan plan = gc::git::BuildCommitFileDiffPlan(
        MakeCommit(), ChangeItem{ChangeKind::modified, L"M", path});
    GC_CHECK_MESSAGE(!plan.allowed, "路径不应被接受：" + Narrow(path));
    GC_CHECK(plan.arguments.empty());
  }
}

GC_TEST(commit_changes_plan_notes_merge_commit) {
  const CommitFileDiffPlan plan = gc::git::BuildCommitFileDiffPlan(
      MakeCommit(kId40, /*merge=*/true), ChangeItem{ChangeKind::modified, L"M", L"a.txt"});
  GC_REQUIRE_MESSAGE(plan.allowed, Narrow(plan.refusalReason));
  GC_CHECK(ArgumentsContain(plan.arguments, L"--first-parent"));
  GC_CHECK(!plan.notice.empty());
}

GC_TEST(commit_changes_plan_accepts_sha256_object_id) {
  const CommitFileDiffPlan plan = gc::git::BuildCommitFileDiffPlan(
      MakeCommit(kId64), ChangeItem{ChangeKind::added, L"A", L"新增.txt"});
  GC_REQUIRE_MESSAGE(plan.allowed, Narrow(plan.refusalReason));
  GC_CHECK(ArgumentsContain(plan.arguments, kId64));
}
