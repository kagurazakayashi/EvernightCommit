// 工作区状态解析的纯逻辑测试：用桩化的 porcelain v2 输出驱动生产解析器，
// 覆盖 XY 两栏拆分、重命名双路径、未合并、子模块内部状态、含空格与 shell 元字符的路径，
// 以及非法输出必须整体报告失败而不是交出半套列表。全部不依赖 Win32 与真实仓库。
#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/utf_text.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::WorkspaceLoadStatus;
using gc::git::WorkspaceModel;
using gc::git::WorkspaceStatusParseResult;

// 把若干记录拼成 -z 的真实形状：每条记录以 NUL 结尾（重命名的第二路径本身就是单独一条片段）。
std::wstring JoinRecords(std::initializer_list<std::wstring_view> records) {
  std::wstring text;
  for (std::wstring_view record : records) {
    text += record;
    text.push_back(L'\0');
  }
  return text;
}

const ChangeItem* Find(const std::vector<ChangeItem>& items, std::wstring_view path) {
  for (const ChangeItem& item : items) {
    if (item.path == path) {
      return &item;
    }
  }
  return nullptr;
}

// 断言描述走 UTF-8（测试报告按窄字符输出）。
std::string Narrow(std::wstring_view text) { return gc::platform::Utf16ToUtf8(std::wstring(text)); }

std::wstring Ordinary(std::wstring_view xy, std::wstring_view sub, std::wstring_view path) {
  return std::wstring(L"1 ") + std::wstring(xy) + L" " + std::wstring(sub) +
         L" 100644 100644 100644 0123456789012345678901234567890123456789 "
         L"9876543210987654321098765432109876543210 " +
         std::wstring(path);
}

}  // namespace

GC_TEST(status_arguments_are_an_array_with_machine_readable_options) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildWorkspaceStatusArguments(L"D:\\工作区 目录 & 特殊");
  // 目录作为独立参数出现，绝不拼进任何 shell 字符串。
  GC_CHECK_MESSAGE(arguments.size() == 9, "参数个数应与约定一致，实际 " + std::to_string(arguments.size()));
  GC_CHECK(arguments[0] == L"-C");
  GC_CHECK(arguments[1] == L"D:\\工作区 目录 & 特殊");
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"--porcelain=v2") != arguments.end());
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"-z") != arguments.end());
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"--untracked-files=all") != arguments.end());
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"--ignore-submodules=none") != arguments.end());
  // 只读：不允许可选锁写入，也不请求 --ignored（被忽略的文件默认不进列表）。
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"--no-optional-locks") != arguments.end());
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"--ignored") == arguments.end());
}

GC_TEST(parse_splits_index_and_worktree_columns_of_one_record) {
  const WorkspaceStatusParseResult result = gc::git::ParseWorkspacePorcelainV2(
      JoinRecords({Ordinary(L"MM", L"N...", L"both.txt"), Ordinary(L".D", L"N...", L"gone.txt"),
                   Ordinary(L"A.", L"N...", L"fresh.txt")}));
  GC_CHECK_MESSAGE(result.error.empty(), "合法输出不应报错：" + gc::platform::Utf16ToUtf8(result.error));
  // 同一记录的两侧分别进入两个列表，同一路径允许同时出现在左右两侧。
  GC_CHECK(result.model.staged.size() == 2);
  GC_CHECK(result.model.unstaged.size() == 2);
  const ChangeItem* bothStaged = Find(result.model.staged, L"both.txt");
  const ChangeItem* bothUnstaged = Find(result.model.unstaged, L"both.txt");
  GC_CHECK(bothStaged != nullptr && bothStaged->kind == ChangeKind::modified);
  GC_CHECK(bothUnstaged != nullptr && bothUnstaged->kind == ChangeKind::modified);
  GC_CHECK(bothStaged != nullptr && bothStaged->statusCode == L"MM");
  GC_CHECK(Find(result.model.staged, L"gone.txt") == nullptr);
  GC_CHECK(Find(result.model.unstaged, L"gone.txt")->kind == ChangeKind::deleted);
  GC_CHECK(Find(result.model.staged, L"fresh.txt")->kind == ChangeKind::added);
  GC_CHECK(Find(result.model.unstaged, L"fresh.txt") == nullptr);
}

GC_TEST(parse_rename_consumes_second_nul_path) {
  // 重命名记录：本片段是目标路径，来源路径是紧随其后的另一个 NUL 片段。
  const std::wstring text = JoinRecords({L"2 RM N... 100644 100644 100644 "
                                         L"0123456789012345678901234567890123456789 "
                                         L"9876543210987654321098765432109876543210 R100 新 名称.txt",
                                         L"旧 名称.txt", Ordinary(L".M", L"N...", L"other.txt")});
  const WorkspaceStatusParseResult result = gc::git::ParseWorkspacePorcelainV2(text);
  GC_CHECK(result.error.empty());
  GC_CHECK(result.model.staged.size() == 1);
  const ChangeItem* renamed = result.model.staged.front().path == L"新 名称.txt"
                                  ? &result.model.staged.front()
                                  : nullptr;
  GC_CHECK_MESSAGE(renamed != nullptr, "重命名条目应以目标路径入库");
  if (renamed != nullptr) {
    GC_CHECK(renamed->kind == ChangeKind::renamed);
    GC_CHECK(renamed->oldPath == L"旧 名称.txt");
    GC_CHECK(renamed->similarity == L"R100");
    GC_CHECK(renamed->statusCode == L"RM");
    // 目标文件暂存后又继续编辑：未暂存侧出现同一路径的修改，且不带来源路径。
    const ChangeItem* also = Find(result.model.unstaged, L"新 名称.txt");
    GC_CHECK(also != nullptr && also->kind == ChangeKind::modified && also->oldPath.empty());
  }
  // 路径含空格必须原样保留，不能按空格拆断。
  GC_CHECK(Find(result.model.unstaged, L"other.txt") != nullptr);
}

GC_TEST(parse_unmerged_as_conflict_not_plain_delete) {
  const WorkspaceStatusParseResult result = gc::git::ParseWorkspacePorcelainV2(JoinRecords({
      L"u UU N... 100644 100644 100644 100644 "
      L"0123456789012345678901234567890123456789 1111111111111111111111111111111111111111 "
      L"2222222222222222222222222222222222222222 both-sides.txt",
      L"u UD N... 100644 100644 000000 100644 "
      L"0123456789012345678901234567890123456789 1111111111111111111111111111111111111111 "
      L"0000000000000000000000000000000000000000 deleted-by-them.txt",
  }));
  GC_CHECK(result.error.empty());
  // 冲突条目一条记录只产出一条条目，落在未暂存侧（要在工作区解决），且带明确冲突标识。
  GC_CHECK(result.model.unstaged.size() == 2);
  GC_CHECK(result.model.staged.empty());
  const ChangeItem* kept = Find(result.model.unstaged, L"both-sides.txt");
  GC_CHECK(kept != nullptr && kept->kind == ChangeKind::conflicted && kept->statusCode == L"UU");
  const ChangeItem* removed = Find(result.model.unstaged, L"deleted-by-them.txt");
  GC_CHECK_MESSAGE(removed != nullptr && removed->kind == ChangeKind::conflicted,
                   "UD 是冲突，不能当成普通删除");
  GC_CHECK(removed == nullptr || removed->statusCode == L"UD");
  GC_CHECK(result.model.HasConflicts());
}

GC_TEST(parse_submodule_keeps_internal_state_separate_from_files) {
  // <sub> 是固定 4 字元的「S<c><m><u>」：提交指针已移动 + 内部有改动 + 内部有未跟踪文件。
  const WorkspaceStatusParseResult result = gc::git::ParseWorkspacePorcelainV2(
      JoinRecords({L"1 .M SCMU 160000 160000 160000 "
                   L"0123456789012345678901234567890123456789 "
                   L"9876543210987654321098765432109876543210 libs/shared"}));
  GC_CHECK(result.error.empty());
  GC_CHECK(result.model.staged.empty());
  GC_CHECK(result.model.unstaged.size() == 1);
  const ChangeItem& item = result.model.unstaged.front();
  GC_CHECK(item.kind == ChangeKind::submodule);
  GC_CHECK(item.path == L"libs/shared");
  GC_CHECK(item.submodule.commitChanged);
  GC_CHECK(item.submodule.trackedChanges);
  GC_CHECK(item.submodule.untrackedChanges);
  GC_CHECK(item.StatusLabel() == L"子模块");
  // 子模块内部的改动不会变成父仓库里可暂存的单个文件。
  const std::wstring note = gc::git::SubmoduleExplanationText(result.model);
  GC_CHECK_MESSAGE(note.find(L"提交指针") != std::wstring::npos, "说明需点出父仓库只记录提交指针");
  GC_CHECK(note.find(L"进入子模块工作区") != std::wstring::npos);
}

GC_TEST(parse_untracked_and_skips_headers_and_ignored) {
  const WorkspaceStatusParseResult result = gc::git::ParseWorkspacePorcelainV2(JoinRecords({
      L"# branch.oid 0123456789012345678901234567890123456789",
      L"# branch.head main",
      Ordinary(L".M", L"N...", L"tracked.txt"),
      L"? 目录 名/文件 &^%$.txt",
      L"? deep/a.txt",
      L"? deep/b.txt",
      L"! ignored/build.log",
  }));
  GC_CHECK(result.error.empty());
  GC_CHECK(result.skippedRecords == 3);  // 两条头部记录 + 一条被忽略条目
  GC_CHECK(result.model.unstaged.size() == 4);
  // 「.M」只有工作区侧有变化，索引侧与 HEAD 一致，因此已暂存侧应为空。
  GC_CHECK(result.model.staged.empty());
  const ChangeItem* odd = Find(result.model.unstaged, L"目录 名/文件 &^%$.txt");
  GC_CHECK_MESSAGE(odd != nullptr, "含空格、&、^、%、$ 的路径必须原样保留");
  GC_CHECK(odd != nullptr && odd->kind == ChangeKind::untracked);
  GC_CHECK(odd != nullptr && odd->statusCode == L"?");
  // 未跟踪目录已由 -uall 展开成单个文件，这里只验证解析保留每条文件。
  GC_CHECK(Find(result.model.unstaged, L"deep/a.txt") != nullptr);
  GC_CHECK(Find(result.model.unstaged, L"deep/b.txt") != nullptr);
  GC_CHECK(Find(result.model.unstaged, L"ignored/build.log") == nullptr);
}

GC_TEST(parse_rejects_malformed_records_without_partial_model) {
  const struct {
    std::wstring_view name;
    std::wstring text;
  } cases[] = {
      {L"字段不足", JoinRecords({L"1 .M N... 100644"})},
      {L"未知记录类型", JoinRecords({L"3 ?? N... 1 2 3 4 5 6 path"})},
      {L"重命名缺少来源路径",
       JoinRecords({L"2 R. N... 100644 100644 100644 1 2 R100 target.txt"})},
      {L"XY 不合约定", JoinRecords({Ordinary(L"ZZ", L"N...", L"weird.txt")})},
      {L"子模块状态字段异常", JoinRecords({Ordinary(L".M", L"S.XY", L"mod")})},
      {L"记录缺少空格", JoinRecords({L"1x"})},
  };
  for (const auto& item : cases) {
    const WorkspaceStatusParseResult result = gc::git::ParseWorkspacePorcelainV2(item.text);
    GC_CHECK_MESSAGE(!result.error.empty(), "非法输出应报错：" + Narrow(item.name));
    // 关键是绝不交出解析出来的一半：否则界面会显示一份缺文件的列表。
    GC_CHECK_MESSAGE(result.model.unstaged.empty() && result.model.staged.empty(),
                     "非法输出必须丢弃半套模型：" + Narrow(item.name));
  }
}

GC_TEST(parse_empty_and_trailing_nul_output) {
  const WorkspaceStatusParseResult clean = gc::git::ParseWorkspacePorcelainV2(L"");
  GC_CHECK(clean.error.empty());
  GC_CHECK(clean.model.unstaged.empty() && clean.model.staged.empty());
  GC_CHECK(!clean.model.HasAnyItems());
  // 只有结尾 NUL 也不算记录（Git 每条记录都以 NUL 结束）。
  const WorkspaceStatusParseResult trailing = gc::git::ParseWorkspacePorcelainV2(L"\0");
  GC_CHECK(trailing.error.empty());
  GC_CHECK(trailing.model.unstaged.empty());
}

GC_TEST(labels_and_summary_describe_model_without_losing_raw_data) {
  std::wstring text = JoinRecords({L"2 C. N... 100644 100644 100644 1 2 C85 new.txt", L"old.txt",
                                   Ordinary(L".T", L"N...", L"link.txt")});
  WorkspaceModel model = gc::git::ParseWorkspacePorcelainV2(text).model;
  GC_CHECK_MESSAGE(model.staged.size() == 1 && model.unstaged.size() == 1,
                   "复制只在索引侧、类型变化只在未暂存侧");
  const ChangeItem& copied = model.staged.front();
  GC_CHECK(copied.kind == ChangeKind::copied);
  // 显示文本给“旧 → 新”，但原始两个路径仍在字段里，操作参数只从字段取。
  GC_CHECK(copied.PathLabel() == L"old.txt → new.txt");
  GC_CHECK(copied.path == L"new.txt" && copied.oldPath == L"old.txt");
  GC_CHECK(copied.StatusLabel() == L"复制");
  GC_CHECK(Find(model.unstaged, L"link.txt")->kind == ChangeKind::typeChange);

  const std::wstring summary = gc::git::BuildWorkspaceSummary(model);
  GC_CHECK(summary.find(L"未暂存 1 项") != std::wstring::npos);
  GC_CHECK(summary.find(L"已暂存 1 项") != std::wstring::npos);
  GC_CHECK(gc::git::SubmoduleExplanationText(model).empty());
}

GC_TEST(empty_state_texts_distinguish_lifecycle_states) {
  const gc::git::EmptyStateTexts unloaded =
      gc::git::WorkspaceEmptyTexts(WorkspaceLoadStatus::unloaded, L"", true);
  GC_CHECK(unloaded.unstaged.find(L"尚未选择可用仓库") != std::wstring::npos);
  // 正在读取：不能显示成“没有更改”。
  const gc::git::EmptyStateTexts loading =
      gc::git::WorkspaceEmptyTexts(WorkspaceLoadStatus::loading, L"", true);
  GC_CHECK(loading.unstaged.find(L"正在读取") != std::wstring::npos);
  GC_CHECK(loading.staged.find(L"正在读取") != std::wstring::npos);
  // 读取成功且确实干净：说明是“没有更改”，同时保留历史列尚未接入 git log 的说法。
  const gc::git::EmptyStateTexts loaded =
      gc::git::WorkspaceEmptyTexts(WorkspaceLoadStatus::loaded, L"", true);
  GC_CHECK(loaded.unstaged.find(L"没有未暂存的更改") != std::wstring::npos);
  GC_CHECK(loaded.staged.find(L"没有已暂存的更改") != std::wstring::npos);
  GC_CHECK(loaded.history.find(L"git log") != std::wstring::npos);
  // 无提交仓库：历史列说明“还没有任何提交”，与“尚未接入”区分开。
  const gc::git::EmptyStateTexts noCommits =
      gc::git::WorkspaceEmptyTexts(WorkspaceLoadStatus::loaded, L"", false);
  GC_CHECK(noCommits.history.find(L"还没有任何提交") != std::wstring::npos);
  // 读取失败：把已归类的原因作为第二行，界面不会看起来像空仓库。
  const gc::git::EmptyStateTexts failed =
      gc::git::WorkspaceEmptyTexts(WorkspaceLoadStatus::failed, L"Git 查询超时", true);
  GC_CHECK(failed.unstaged.find(L"读取失败") != std::wstring::npos);
  GC_CHECK(failed.unstaged.find(L"Git 查询超时") != std::wstring::npos);
}
