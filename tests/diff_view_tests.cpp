// 双击查看差异/内容的纯逻辑测试：按侧别与条目类别核对构造出来的 Git 参数、
// 未跟踪文件的预检裁决（不存在/不可读/目录/二进制/过大）、路径越界防护与退出码文案。
// 全部不依赖 Win32 与真实仓库；真实 Git 行为的验证见 diff_view_fixture_tests.cpp。
#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"
#include "git/diff_view.h"
#include "git/workspace_model.h"
#include "platform/windows/utf_text.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::ChangeSide;
using gc::git::DiffViewKind;
using gc::git::DiffViewPlan;
using gc::git::WorktreeFileFacts;

std::string Narrow(std::wstring_view text) { return gc::platform::Utf16ToUtf8(std::wstring(text)); }

ChangeItem Make(ChangeKind kind, std::wstring_view path, std::wstring_view oldPath = {}) {
  ChangeItem item;
  item.kind = kind;
  item.path = std::wstring(path);
  item.oldPath = std::wstring(oldPath);
  item.statusCode = L"1";
  return item;
}

bool Has(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

size_t CountOf(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return static_cast<size_t>(
      std::count(arguments.begin(), arguments.end(), std::wstring(value)));
}

size_t IndexOf(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  const auto found = std::find(arguments.begin(), arguments.end(), std::wstring(value));
  return found == arguments.end() ? std::string::npos : static_cast<size_t>(found - arguments.begin());
}

// 正常可读的文本文件事实（未跟踪分支才会读这些字段）。
WorktreeFileFacts ReadableText(unsigned long long size = 1024ULL) {
  WorktreeFileFacts facts;
  facts.probed = true;
  facts.exists = true;
  facts.readable = true;
  facts.sizeBytes = size;
  return facts;
}

// 把方案里的参数拼成一行，便于用「包含」断言读命令形态。
std::string Flatten(const DiffViewPlan& plan) {
  std::wstring line;
  for (const std::wstring& argument : plan.arguments) {
    if (!line.empty()) {
      line += L' ';
    }
    line += argument;
  }
  return Narrow(line);
}

// 交给命令窗口执行器的输入必须过它自己的校验：这里把方案直接送进 BuildGitCommandLine，
// 这样「界面构造出来的参数」与「执行器愿意接受的参数」永远不会各自漂移。
void ExpectAcceptedByExecutor(const DiffViewPlan& plan, const std::string& label) {
  gc::git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = L"diff";
  operation.gitExecutable = L"C:\\git\\bin\\git.exe";
  operation.repositoryDirectory = L"C:\\repo";
  operation.arguments = plan.arguments;
  std::wstring line;
  gc::git::CommandPlanReject reject = gc::git::CommandPlanReject::none;
  std::wstring detail;
  const bool accepted = gc::git::BuildGitCommandLine(operation, &line, &reject, &detail);
  GC_REQUIRE_MESSAGE(accepted, label + " 应被命令窗口执行器接受，实际被拒：" + Narrow(detail));
}

}  // namespace

GC_TEST(diff_view_unstaged_uses_worktree_vs_index) {
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::modified, L"a/b.txt"), {});
  GC_CHECK_MESSAGE(plan.kind == DiffViewKind::trackedDiff, Narrow(plan.blockedReason));
  GC_CHECK(Has(plan.arguments, L"diff"));
  GC_CHECK(!Has(plan.arguments, L"--cached"));  // 未暂存侧绝不能比较索引与 HEAD
  GC_CHECK(Has(plan.arguments, L"--no-ext-diff"));
  GC_CHECK(Has(plan.arguments, L"--no-textconv"));
  GC_CHECK(Has(plan.arguments, L"--no-optional-locks"));
  GC_CHECK(Has(plan.arguments, L"--no-replace-objects"));
  GC_CHECK(Has(plan.arguments, L":(literal)a/b.txt"));
  // 参数终止符必须排在所有路径之前，否则以减号开头的档名会被当成选项。
  GC_CHECK(IndexOf(plan.arguments, L"--") < IndexOf(plan.arguments, L":(literal)a/b.txt"));
  GC_CHECK(plan.operationId == L"diff-worktree");
  ExpectAcceptedByExecutor(plan, "未暂存差异参数");
}

GC_TEST(diff_view_staged_uses_index_vs_head) {
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::staged, Make(ChangeKind::modified, L"a/b.txt"), {});
  GC_CHECK(plan.kind == DiffViewKind::trackedDiff);
  GC_CHECK(Has(plan.arguments, L"--cached"));
  GC_CHECK(!Has(plan.arguments, L"--find-renames"));  // 普通修改不需要配对
  GC_CHECK(Has(plan.arguments, L":(literal)a/b.txt"));
  GC_CHECK(plan.operationId == L"diff-index");
  ExpectAcceptedByExecutor(plan, "已暂存差异参数");
}

GC_TEST(diff_view_staged_rename_carries_both_paths) {
  const DiffViewPlan plan = gc::git::BuildDiffViewPlan(
      ChangeSide::staged, Make(ChangeKind::renamed, L"new/name.txt", L"old/name.txt"), {});
  GC_CHECK(plan.kind == DiffViewKind::trackedDiff);
  // 只给新路径时 Git 把重命名显示成「新增文件」，与列表上的「重命名」自相矛盾（实测）。
  GC_CHECK(Has(plan.arguments, L"--find-renames"));
  GC_CHECK(Has(plan.arguments, L":(literal)old/name.txt"));
  GC_CHECK(Has(plan.arguments, L":(literal)new/name.txt"));
  // 旧路径在前，读起来与 diff 输出的 rename from/to 顺序一致。
  GC_CHECK(IndexOf(plan.arguments, L":(literal)old/name.txt") <
           IndexOf(plan.arguments, L":(literal)new/name.txt"));
  ExpectAcceptedByExecutor(plan, "重命名差异参数");
}

GC_TEST(diff_view_copied_carries_source_path_too) {
  const DiffViewPlan plan = gc::git::BuildDiffViewPlan(
      ChangeSide::staged, Make(ChangeKind::copied, L"copy.txt", L"origin.txt"), {});
  GC_CHECK(Has(plan.arguments, L"--find-renames"));
  GC_CHECK(CountOf(plan.arguments, L":(literal)origin.txt") == 1);
  GC_CHECK(CountOf(plan.arguments, L":(literal)copy.txt") == 1);
}

GC_TEST(diff_view_deleted_file_still_uses_tracked_diff) {
  // 删除项没有「可读的文件」：差异由 Git 给出，程序不去打开已不存在的路径，
  // 因此这条分支完全不读档案事实（传空 facts 也必须正常出参数）。
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::deleted, L"gone.txt"), {});
  GC_CHECK(plan.kind == DiffViewKind::trackedDiff);
  GC_CHECK(!Has(plan.arguments, L"--no-index"));
  GC_CHECK(Has(plan.arguments, L":(literal)gone.txt"));
  GC_CHECK(plan.notice.find(L"不会去打开已经不存在的路径") != std::wstring::npos);
  ExpectAcceptedByExecutor(plan, "删除项差异参数");
}

GC_TEST(diff_view_conflicted_item_uses_combined_diff) {
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::conflicted, L"c.txt"), {});
  GC_CHECK(plan.kind == DiffViewKind::trackedDiff);
  GC_CHECK(Has(plan.arguments, L":(literal)c.txt"));
  GC_CHECK(plan.notice.find(L"diff --cc") != std::wstring::npos);
  ExpectAcceptedByExecutor(plan, "冲突条目差异参数");
}

GC_TEST(diff_view_submodule_is_pointer_only) {
  ChangeItem item = Make(ChangeKind::submodule, L"sub/module");
  item.submodule.commitChanged = true;
  item.submodule.untrackedChanges = true;
  const DiffViewPlan unstaged = gc::git::BuildDiffViewPlan(ChangeSide::unstaged, item, {});
  GC_CHECK(unstaged.kind == DiffViewKind::trackedDiff);
  GC_CHECK(Has(unstaged.arguments, L"--ignore-submodules=none"));
  // --submodule=short 覆盖使用者的 diff.submodule 配置：父仓库的一次查看绝不递归进子模块内部。
  GC_CHECK(Has(unstaged.arguments, L"--submodule=short"));
  GC_CHECK(!Has(unstaged.arguments, L"--submodule=diff"));
  GC_CHECK(unstaged.notice.find(L"提交指针") != std::wstring::npos);
  GC_CHECK(unstaged.notice.find(L"内部有未跟踪文件") != std::wstring::npos);

  const DiffViewPlan staged = gc::git::BuildDiffViewPlan(ChangeSide::staged, item, {});
  GC_CHECK(Has(staged.arguments, L"--cached"));
  GC_CHECK(Has(staged.arguments, L"--submodule=short"));
  ExpectAcceptedByExecutor(staged, "子模块差异参数");
}

GC_TEST(diff_view_untracked_content_compares_against_empty_side) {
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, L"new/file.txt"),
                                 ReadableText());
  GC_CHECK(plan.kind == DiffViewKind::untrackedContent);
  GC_CHECK(Has(plan.arguments, L"--no-index"));
  GC_CHECK(!Has(plan.arguments, L"--cached"));
  GC_CHECK(!Has(plan.arguments, L"--stat"));
  // --no-index 的兩條路徑是檔案系統路徑，不能套 magic pathspec。
  GC_CHECK(!Has(plan.arguments, L":(literal)new/file.txt"));
  GC_CHECK(Has(plan.arguments, L"/dev/null"));
  GC_CHECK(Has(plan.arguments, L"new/file.txt"));
  GC_CHECK(IndexOf(plan.arguments, L"--") < IndexOf(plan.arguments, L"/dev/null"));
  // 未跟踪只出现在未暂存侧；已暂存侧的 --no-index 形态同样能出参数（防御式）。
  GC_CHECK(plan.operationId == L"preview-content");
  ExpectAcceptedByExecutor(plan, "未跟踪内容参数");
}

GC_TEST(diff_view_untracked_binary_keeps_notice_without_dumping) {
  WorktreeFileFacts facts = ReadableText();
  facts.containsNullByte = true;
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, L"asset.bin"), facts);
  // 仍是内容分支：由 Git 自己写「Binary files differ」，窗口里留下的是一句话而不是一屏字节。
  GC_CHECK(plan.kind == DiffViewKind::untrackedContent);
  GC_CHECK(plan.notice.find(L"二进制") != std::wstring::npos);
  GC_CHECK(Flatten(plan).find("--no-index") != std::string::npos);
}

GC_TEST(diff_view_untracked_oversized_falls_back_to_stat_summary) {
  const unsigned long long oversized = gc::git::kUntrackedContentPreviewLimitBytes + 1ULL;
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, L"huge.txt"),
                                 ReadableText(oversized));
  GC_CHECK(plan.kind == DiffViewKind::untrackedSummary);
  GC_CHECK(Has(plan.arguments, L"--stat"));
  GC_CHECK(Has(plan.arguments, L"--no-index"));
  GC_CHECK(Has(plan.arguments, L"/dev/null"));
  GC_CHECK(plan.notice.find(L"--stat 摘要") != std::wstring::npos);
  GC_CHECK(plan.notice.find(std::to_wstring(oversized)) != std::wstring::npos);
  ExpectAcceptedByExecutor(plan, "大文件摘要参数");
}

GC_TEST(diff_view_untracked_huge_is_refused_before_launch) {
  const unsigned long long absurd = gc::git::kUntrackedSummaryLimitBytes + 1ULL;
  const DiffViewPlan plan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, L"image.iso"),
                                 ReadableText(absurd));
  GC_CHECK(plan.kind == DiffViewKind::blocked);
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.blockedReason.find(L"没有打开命令窗口") != std::wstring::npos);
}

GC_TEST(diff_view_untracked_blocked_cases_each_state_the_reason) {
  struct Case {
    WorktreeFileFacts facts;
    std::wstring_view expected;
    std::string label;
  };
  WorktreeFileFacts probeFailed;  // probed=false：连属性都没取到
  WorktreeFileFacts missing;
  missing.probed = true;
  WorktreeFileFacts directory;
  directory.probed = true;
  directory.exists = true;
  directory.isDirectory = true;
  WorktreeFileFacts unreadable;
  unreadable.probed = true;
  unreadable.exists = true;
  unreadable.readable = false;
  unreadable.failureReason = L"模拟：拒绝访问";

  const std::vector<Case> cases = {
      {probeFailed, L"无法读取该文件的属性", "probe-failed"},
      {missing, L"文件已不在工作区", "missing"},
      {directory, L"目录而不是文件", "directory"},
      {unreadable, L"没有读取该文件的权限", "unreadable"},
  };
  for (const Case& one : cases) {
    const DiffViewPlan plan =
        gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, L"x.txt"), one.facts);
    GC_REQUIRE_MESSAGE(plan.kind == DiffViewKind::blocked, "预检应拒绝：" + one.label);
    GC_CHECK_MESSAGE(plan.blockedReason.find(one.expected) != std::wstring::npos,
                     one.label + " 的原因说明不符：" + Narrow(plan.blockedReason));
    GC_CHECK(plan.arguments.empty());
  }
}

GC_TEST(diff_view_rejects_paths_outside_worktree) {
  const std::vector<std::pair<std::wstring, std::wstring>> bad = {
      {L"../escape.txt", L".."},
      {L"a/../../escape.txt", L".."},
      {L"/absolute.txt", L"分隔符"},
      {L"C:/outside.txt", L"冒号"},
      {L"stream.txt:secret", L"冒号"},
      {L"back\\slash.txt", L"反斜杠"},
      {L"", L"路径为空"},
  };
  for (const auto& [path, expected] : bad) {
    // 未跟踪条目走 --no-index（真正会打开档案系统路径），越界防护必须挡住它。
    const DiffViewPlan untracked =
        gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, path), ReadableText());
    GC_REQUIRE_MESSAGE(untracked.kind == DiffViewKind::blocked,
                       "越界路径应被拒绝：" + Narrow(path));
    GC_CHECK(untracked.blockedReason.find(expected) != std::wstring::npos);

    // 已跟踪条目同样拒绝：pathspec 由 Git 解释，但我们不送出违背约定的东西。
    const DiffViewPlan tracked =
        gc::git::BuildDiffViewPlan(ChangeSide::staged, Make(ChangeKind::modified, path), {});
    GC_CHECK_MESSAGE(tracked.kind == DiffViewKind::blocked, "已跟踪条目也应拒绝：" + Narrow(path));
  }
  // 空路径的 reason 里含「路径为空」，上面用 ".."/"分隔符" 等关键字匹配不到，单独确认一次。
  const DiffViewPlan emptyPath =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::modified, L""), {});
  GC_CHECK(emptyPath.blockedReason.find(L"路径为空") != std::wstring::npos);
}

GC_TEST(diff_view_accepts_nasty_but_in_worktree_names) {
  // Windows 合法、但对 shell/Git 敏感的档名：路径本身不算越界，必须由参数形态而不是拒绝来兜住。
  const std::vector<std::wstring> names = {
      L"-leading.txt",
      L"100%.txt",
      L"a&b^c.txt",
      L"x[1].txt",
      L"中文 文件.txt",
      L"sub/dir/带 空格 & 符号.txt",
      L"quote'single.txt",
  };
  for (const std::wstring& name : names) {
    const DiffViewPlan tracked = gc::git::BuildDiffViewPlan(ChangeSide::unstaged,
                                                            Make(ChangeKind::modified, name), {});
    GC_REQUIRE_MESSAGE(tracked.kind == DiffViewKind::trackedDiff,
                       "合法档名不该被拒绝：" + Narrow(name));
    GC_CHECK(Has(tracked.arguments, gc::git::MakeLiteralPathspec(name)));
    ExpectAcceptedByExecutor(tracked, Narrow(name) + " 的差异参数");

    const DiffViewPlan untracked =
        gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, name), ReadableText());
    GC_CHECK(untracked.kind == DiffViewKind::untrackedContent);
    ExpectAcceptedByExecutor(untracked, Narrow(name) + " 的内容参数");
  }
}

GC_TEST(diff_view_literal_pathspec_shape) {
  GC_CHECK(gc::git::MakeLiteralPathspec(L"a b.txt") == L":(literal)a b.txt");
  GC_CHECK(gc::git::MakeLiteralPathspec(L"").empty());
  // 已带 magic 前缀的输入不重复包装（我们的条目不会产生这种形态，这里只防二次包装）。
  GC_CHECK(gc::git::MakeLiteralPathspec(L":(glob)x") == L":(glob)x");
}

GC_TEST(diff_view_join_worktree_path) {
  GC_CHECK(gc::git::JoinWorktreeFilePath(L"C:\\repo", L"a/b.txt") == L"C:\\repo\\a\\b.txt");
  GC_CHECK(gc::git::JoinWorktreeFilePath(L"C:\\repo\\", L"a/b.txt") == L"C:\\repo\\a\\b.txt");
  GC_CHECK(gc::git::JoinWorktreeFilePath(L"\\\\srv\\share\\repo", L"中文/文 件.txt") ==
           L"\\\\srv\\share\\repo\\中文\\文 件.txt");
}

GC_TEST(diff_view_exit_code_wording_does_not_call_differences_failure) {
  // `git diff` 有差异时也是 0（实测），而 --no-index 用 1 表示「有差异」：
  // 两种形态的文案都必须让人一眼看懂，不能把正常完成说成失败。
  GC_CHECK(gc::git::DescribeDiffViewExitCode(DiffViewKind::trackedDiff, 0).find(L"没有差异") !=
           std::wstring::npos);
  const std::wstring previewZero = gc::git::DescribeDiffViewExitCode(DiffViewKind::untrackedContent, 0);
  const std::wstring previewOne = gc::git::DescribeDiffViewExitCode(DiffViewKind::untrackedContent, 1);
  GC_CHECK(previewOne.find(L"不是失败") != std::wstring::npos);
  GC_CHECK(previewZero != previewOne);
  GC_CHECK(gc::git::DescribeDiffViewExitCode(DiffViewKind::untrackedSummary, 2).find(L"错误") !=
           std::wstring::npos);
  GC_CHECK(gc::git::DescribeDiffViewExitCode(DiffViewKind::blocked, 0).empty());
}

GC_TEST(diff_view_operation_ids_are_executor_safe) {
  const std::vector<DiffViewPlan> plans = {
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::modified, L"a.txt"), {}),
      gc::git::BuildDiffViewPlan(ChangeSide::staged, Make(ChangeKind::renamed, L"b.txt", L"a.txt"), {}),
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, Make(ChangeKind::untracked, L"c.bin"),
                                 ReadableText()),
      gc::git::BuildDiffViewPlan(
          ChangeSide::unstaged, Make(ChangeKind::untracked, L"d.txt"),
          ReadableText(gc::git::kUntrackedContentPreviewLimitBytes + 1ULL)),
  };
  for (const DiffViewPlan& plan : plans) {
    GC_REQUIRE_MESSAGE(!plan.operationId.empty(), "操作 ID 不应为空");
    GC_CHECK(plan.operationId.size() <= gc::git::kMaxOperationIdLength);
    for (const wchar_t c : plan.operationId) {
      const bool asciiWord = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
      GC_CHECK_MESSAGE(asciiWord, "操作 ID 含非 ASCII 字符：" + Narrow(plan.operationId));
    }
    GC_CHECK(plan.arguments.size() <= gc::git::kMaxArguments);
    GC_CHECK(!plan.displayName.empty());
  }
}

GC_TEST(diff_view_display_name_and_notice_survive_title_sanitizing) {
  // displayName 会进控制台标题：里面带档名，必须能被 MakeSafeConsoleTitle 处理成可用标题，
  // 而不是因为「展示文字不好」被拒绝（标题不合格时执行器退回占位标题，仍保留唯一标记）。
  const ChangeItem item = Make(ChangeKind::modified, L"中文 & 空格.txt");
  const DiffViewPlan plan = gc::git::BuildDiffViewPlan(ChangeSide::staged, item, {});
  const std::wstring title =
      gc::git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", plan.displayName, L"GcOp9");
  GC_CHECK(title.find(L"GcOp9") != std::wstring::npos);
  GC_CHECK(title.find(L'"') == std::wstring::npos);
  GC_CHECK(title.find(L'%') == std::wstring::npos);
  GC_CHECK(plan.displayName.find(item.PathLabel()) != std::wstring::npos);
}
