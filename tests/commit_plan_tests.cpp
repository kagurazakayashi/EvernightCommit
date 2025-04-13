// 「创建提交」方案层的纯逻辑测试：不起 Git、不开窗口、不碰文件系统，
// 只把「刚刚读回的仓库现状 + 表单结论」交给 BuildCommitPlan，断言它给的是拒绝还是命令。
//
// 覆盖：拒绝条件（没有暂存内容、还有冲突、合并/变基没走完、提交者身份缺失或未知、
// 消息文件路径不安全、时间没换算出来、作者身份残缺、仓库形态没有工作区）逐条各自给出原因；
// 通过时命令形态只有 `commit --cleanup=verbatim -F <文件>`（绝不会出现 -a、--no-verify 或路径参数）、
// 环境覆盖只有作者身份与两个时间（提交者身份一概不覆盖），
// 以及确认文字里的范围/身份/两个时间、界面摘要与刚读回的现状不一致时那句先说在前面的提示。
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/commit_plan.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::CivilTime;
using gc::git::CommitPlan;
using gc::git::CommitPlanInput;
using gc::git::CommitTimeChoice;
using gc::git::CommitterIdentityState;
using gc::git::GitIdentity;
using gc::git::RepoKind;
using gc::git::RepositoryWorkflowState;
using gc::git::WorkspaceModel;

constexpr std::wstring_view kMessageFile = L"C:\\Temp\\gc-msg-1234-1.txt";

bool Contains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

// 断言失败时把宽字符原因搬进 std::string 太啰嗦，这里只取一个短标签：
// 用例本身已经指明在考察哪一条，出问题时看标签 + 计划内容即可定位。
std::string Tag(std::wstring_view text) {
  std::string result;
  for (const wchar_t c : text.substr(0, 40)) {
    result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return result;
}

ChangeItem Item(ChangeKind kind, std::wstring_view statusCode, std::wstring_view path) {
  ChangeItem item;
  item.kind = kind;
  item.statusCode = std::wstring(statusCode);
  item.path = std::wstring(path);
  return item;
}

CommitTimeChoice Choice(std::string gitDate, std::wstring_view display) {
  CommitTimeChoice choice;
  choice.gitDate = std::move(gitDate);
  choice.displayText = std::wstring(display);
  choice.wall = CivilTime{2026, 10, 1, 12, 30, 15};
  choice.offsetMinutes = 480;
  return choice;
}

// 一份「什么都没问题」的输入：用例只改自己要考察的那一项，其余照旧。
CommitPlanInput BaseInput() {
  CommitPlanInput input;
  input.model.staged = {Item(ChangeKind::modified, L"M ", L"src/a.cpp"),
                        Item(ChangeKind::added, L"A ", L"docs/说明.md")};
  input.model.unstaged = {Item(ChangeKind::modified, L" M", L"src/b.cpp")};
  input.detection.kind = RepoKind::plainWorktree;
  input.detection.root = L"P:\\repo";
  input.detection.absoluteGitDir = L"P:\\repo\\.git";
  input.detection.branch = L"main";
  input.detection.shortSha = L"abc1234";
  input.detection.headResolved = true;
  input.committer = CommitterIdentityState::available;
  input.committerIdentityText = L"仓库配置身份 <cfg@example.invalid>";
  input.author = GitIdentity{L"张三", L"zs@example.invalid"};
  input.message = L"标题\n\n描述第一行\n描述第二行\n\nCo-authored-by: 李四 <ls@example.invalid>";
  input.messageUtf8Bytes = 128;
  input.messageFilePath = std::wstring(kMessageFile);
  input.authorTime = Choice("@1790784000 +0800", L"2026-09-30 16:00:00（UTC+08:00）");
  input.committerTime = Choice("@1790784000 +0800", L"2026-09-30 16:00:00（UTC+08:00）");
  input.timesSynced = true;
  return input;
}

std::wstring EnvironmentValue(const CommitPlan& plan, std::wstring_view name) {
  for (const gc::git::EnvironmentOverride& entry : plan.environmentOverrides) {
    if (entry.name == name) {
      return entry.value.has_value() ? *entry.value : std::wstring();
    }
  }
  return std::wstring(L"<没有这一项>");
}

bool HasEnvironment(const CommitPlan& plan, std::wstring_view name) {
  for (const gc::git::EnvironmentOverride& entry : plan.environmentOverrides) {
    if (entry.name == name) {
      return true;
    }
  }
  return false;
}

std::wstring JoinedArguments(const CommitPlan& plan) {
  std::wstring text;
  for (const std::wstring& argument : plan.arguments) {
    if (!text.empty()) {
      text += L' ';
    }
    text += argument;
  }
  return text;
}

GC_TEST(commit_plan_happy_path_submits_index_only) {
  const CommitPlan plan = gc::git::BuildCommitPlan(BaseInput());
  GC_CHECK_MESSAGE(!plan.blocked, "不该被拒绝：" + Tag(plan.blockedReason));

  // 命令形态：只有 commit / --cleanup=verbatim / -F / 文件。
  const std::vector<std::wstring> expected{L"commit", L"--cleanup=verbatim", L"-F",
                                           std::wstring(kMessageFile)};
  GC_CHECK(plan.arguments == expected);
  GC_CHECK(plan.operationId == L"commit");
  GC_CHECK(plan.displayName == L"创建提交");

  const std::wstring arguments = JoinedArguments(plan);
  // 这三条是这个步骤的底线：不扩大提交范围、不绕过 hooks、不隐式暂存。
  GC_CHECK(arguments.find(L"-a") == std::wstring::npos);
  GC_CHECK(arguments.find(L"--all") == std::wstring::npos);
  GC_CHECK(arguments.find(L"--no-verify") == std::wstring::npos);
  GC_CHECK(arguments.find(L"--amend") == std::wstring::npos);
  GC_CHECK(arguments.find(L"pathspec") == std::wstring::npos);
  GC_CHECK(arguments.find(std::wstring_view(L"P:\\repo")) == std::wstring::npos);

  // 环境覆盖只有作者身份与两个时间：提交者身份由有效配置决定，一概不覆盖。
  GC_CHECK(plan.environmentOverrides.size() == 4);
  GC_CHECK(EnvironmentValue(plan, L"GIT_AUTHOR_NAME") == L"张三");
  GC_CHECK(EnvironmentValue(plan, L"GIT_AUTHOR_EMAIL") == L"zs@example.invalid");
  GC_CHECK(EnvironmentValue(plan, L"GIT_AUTHOR_DATE") == L"@1790784000 +0800");
  GC_CHECK(EnvironmentValue(plan, L"GIT_COMMITTER_DATE") == L"@1790784000 +0800");
  GC_CHECK(!HasEnvironment(plan, L"GIT_COMMITTER_NAME"));
  GC_CHECK(!HasEnvironment(plan, L"GIT_COMMITTER_EMAIL"));

  // 确认文字里要看得见：范围、身份、两个时间与偏移、hooks 照常生效。
  GC_CHECK(Contains(plan.previewText, L"已暂存的 2 项"));
  GC_CHECK(Contains(plan.previewText, L"src/a.cpp"));
  GC_CHECK(Contains(plan.previewText, L"docs/说明.md"));
  GC_CHECK(Contains(plan.previewText, L"未暂存的 1 项改动不会进入这次提交"));
  GC_CHECK(Contains(plan.previewText, L"标题"));
  GC_CHECK(Contains(plan.previewText, L"Co-authored-by: 李四 <ls@example.invalid>"));
  GC_CHECK(Contains(plan.previewText, L"2026-09-30 16:00:00（UTC+08:00）"));
  GC_CHECK(Contains(plan.previewText, L"张三 <zs@example.invalid>"));
  GC_CHECK(Contains(plan.previewText, L"仓库配置身份 <cfg@example.invalid>"));
  GC_CHECK(Contains(plan.previewText, L"--no-verify"));  // 「命令里没有 --no-verify」那句说明。
  GC_CHECK(Contains(plan.previewText, L"128 字节"));
  // 确认框里列出的命令就是真正执行的那一条（参数逐个拼出，不重复写程序名）。
  GC_CHECK(Contains(plan.previewText, L"git commit --cleanup=verbatim -F"));
  GC_CHECK(!Contains(plan.previewText, L"git git"));
  GC_CHECK(Contains(plan.previewText, L"时间同步修改"));
  GC_CHECK(plan.stagedItems == 2);
  GC_CHECK(plan.stateChangeNote.empty());
  GC_CHECK(Contains(plan.notice, L"只包含刚刚读回的 2 项"));
}

GC_TEST(commit_plan_refuses_when_nothing_is_staged) {
  CommitPlanInput input = BaseInput();
  input.model.staged.clear();
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK(plan.blocked);
  GC_CHECK(Contains(plan.blockedReason, L"没有任何条目"));
  GC_CHECK(Contains(plan.blockedReason, L"不会用 git commit -a"));
  GC_CHECK(plan.arguments.empty());
  GC_CHECK(plan.environmentOverrides.empty());
  GC_CHECK(plan.previewText.empty());
}

GC_TEST(commit_plan_refuses_unresolved_conflicts) {
  CommitPlanInput input = BaseInput();
  input.model.unstaged.push_back(Item(ChangeKind::conflicted, L"UU", L"src/conflict.cpp"));
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK(plan.blocked);
  GC_CHECK(Contains(plan.blockedReason, L"没解决的冲突"));
  GC_CHECK(Contains(plan.blockedReason, L"src/conflict.cpp"));
}

GC_TEST(commit_plan_refuses_conflicted_entry_on_staged_side_too) {
  // porcelain v2 把未合并条目归到未暂存那一侧，但模型两侧都问一遍：
  // 万一另一侧也出现冲突条目，提交绝不能带着它一起写进索引结果。
  CommitPlanInput input = BaseInput();
  input.model.staged.push_back(Item(ChangeKind::conflicted, L"AA", L"src/both.cpp"));
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK(plan.blocked);
  GC_CHECK(Contains(plan.blockedReason, L"src/both.cpp"));
}

GC_TEST(commit_plan_refuses_special_flow_in_progress) {
  using FlowSetter = void (*)(RepositoryWorkflowState&);
  const std::vector<std::pair<std::wstring, FlowSetter>> cases = {
      {L"merge", [](RepositoryWorkflowState& state) { state.mergeInProgress = true; }},
      {L"rebase", [](RepositoryWorkflowState& state) { state.rebaseInProgress = true; }},
      {L"cherry-pick", [](RepositoryWorkflowState& state) { state.cherryPickInProgress = true; }},
      {L"bisect", [](RepositoryWorkflowState& state) { state.bisectInProgress = true; }},
      {L"revert", [](RepositoryWorkflowState& state) { state.revertInProgress = true; }},
  };
  for (const auto& entry : cases) {
    CommitPlanInput input = BaseInput();
    entry.second(input.workflow);
    const CommitPlan plan = gc::git::BuildCommitPlan(input);
    GC_CHECK_MESSAGE(plan.blocked, "这种流程没走完时不该提交：" + Tag(entry.first));
    GC_CHECK(Contains(plan.blockedReason, L"Git 流程还没走完"));
    // 拒绝里必须说清不替用户终止那种流程（AGENTS 的边界：不擅自 reset/abort）。
    GC_CHECK(Contains(plan.blockedReason, L"也不会替你终止它"));
  }

  // 几种流程同时存在时全列出来，只报一种会让人以为别的无所谓。
  CommitPlanInput both = BaseInput();
  both.workflow.mergeInProgress = true;
  both.workflow.rebaseInProgress = true;
  const std::wstring text = both.workflow.SpecialFlowText();
  GC_CHECK(Contains(text, L"MERGE_HEAD"));
  GC_CHECK(Contains(text, L"rebase-merge"));
  GC_CHECK(Contains(gc::git::BuildCommitPlan(both).blockedReason, L"变基"));
}

GC_TEST(commit_plan_explains_missing_committer_identity) {
  CommitPlanInput missing = BaseInput();
  missing.committer = CommitterIdentityState::missing;
  missing.committerIdentityText.clear();
  const CommitPlan missingPlan = gc::git::BuildCommitPlan(missing);
  GC_CHECK(missingPlan.blocked);
  GC_CHECK(Contains(missingPlan.blockedReason, L"user.name"));
  GC_CHECK(Contains(missingPlan.blockedReason, L"不替你写配置"));

  CommitPlanInput unknown = BaseInput();
  unknown.committer = CommitterIdentityState::unknown;
  const CommitPlan unknownPlan = gc::git::BuildCommitPlan(unknown);
  GC_CHECK(unknownPlan.blocked);
  GC_CHECK(Contains(unknownPlan.blockedReason, L"还没读到"));
}

GC_TEST(commit_plan_refuses_unusable_time_or_message_file) {
  CommitPlanInput noAuthorDate = BaseInput();
  noAuthorDate.authorTime.gitDate.clear();
  GC_CHECK(gc::git::BuildCommitPlan(noAuthorDate).blocked);

  CommitPlanInput noCommitterDate = BaseInput();
  noCommitterDate.committerTime.gitDate.clear();
  const CommitPlan committerPlan = gc::git::BuildCommitPlan(noCommitterDate);
  GC_CHECK(committerPlan.blocked);
  GC_CHECK(Contains(committerPlan.blockedReason, L"恢复当前时间"));

  // 消息文件路径会进 cmd 脚本里被引号包住：含双引号与控制字符一律不交出去。
  CommitPlanInput quotedPath = BaseInput();
  quotedPath.messageFilePath = L"C:\\Temp\\带\"引号\"的消息.txt";
  const CommitPlan quotedPlan = gc::git::BuildCommitPlan(quotedPath);
  GC_CHECK(quotedPlan.blocked);
  GC_CHECK(Contains(quotedPlan.blockedReason, L"双引号"));

  CommitPlanInput controlPath = BaseInput();
  controlPath.messageFilePath = std::wstring(L"C:\\Temp\n换行.txt");
  GC_CHECK(gc::git::BuildCommitPlan(controlPath).blocked);

  CommitPlanInput emptyPath = BaseInput();
  emptyPath.messageFilePath.clear();
  GC_CHECK(gc::git::BuildCommitPlan(emptyPath).blocked);

  CommitPlanInput emptyMessage = BaseInput();
  emptyMessage.message.clear();
  GC_CHECK(gc::git::BuildCommitPlan(emptyMessage).blocked);
}

GC_TEST(commit_plan_refuses_broken_author_or_unusable_repository) {
  CommitPlanInput noName = BaseInput();
  noName.author.name.clear();
  const CommitPlan noNamePlan = gc::git::BuildCommitPlan(noName);
  GC_CHECK(noNamePlan.blocked);
  GC_CHECK(Contains(noNamePlan.blockedReason, L"姓名 <邮箱>"));

  // 身份里塞进换行 = 在提交信息里凭空多出一行 trailer，环境块也不接受控制字符。
  CommitPlanInput newline = BaseInput();
  newline.author.email =
      std::wstring(L"a@x.invalid\nCo-authored-by: 冒名 <m@x.invalid>");
  GC_CHECK(gc::git::BuildCommitPlan(newline).blocked);

  CommitPlanInput noRoot = BaseInput();
  noRoot.detection.root.clear();
  const CommitPlan noRootPlan = gc::git::BuildCommitPlan(noRoot);
  GC_CHECK(noRootPlan.blocked);
  GC_CHECK(Contains(noRootPlan.blockedReason, L"刷新"));

  CommitPlanInput bare = BaseInput();
  bare.detection.kind = RepoKind::bare;
  GC_CHECK(gc::git::BuildCommitPlan(bare).blocked);
}

GC_TEST(commit_plan_allows_first_commit_and_detached_head) {
  // 仓库里第一个提交、以及 detached HEAD 上的提交都是合法的普通提交：
  // 这里不因 HEAD 不可解析而拒绝，只在确认文字里如实说明当前处于什么状态。
  CommitPlanInput unborn = BaseInput();
  unborn.detection.headResolved = false;
  unborn.detection.shortSha.clear();
  const CommitPlan unbornPlan = gc::git::BuildCommitPlan(unborn);
  GC_CHECK_MESSAGE(!unbornPlan.blocked, Tag(unbornPlan.blockedReason));
  GC_CHECK(Contains(unbornPlan.previewText, L"尚无提交"));

  CommitPlanInput detached = BaseInput();
  detached.detection.kind = RepoKind::detached;
  detached.detection.branch.clear();
  const CommitPlan detachedPlan = gc::git::BuildCommitPlan(detached);
  GC_CHECK_MESSAGE(!detachedPlan.blocked, Tag(detachedPlan.blockedReason));
  GC_CHECK(!detachedPlan.previewText.empty());
}

GC_TEST(commit_plan_reports_state_changed_since_click) {
  CommitPlanInput input = BaseInput();
  input.captured.valid = true;
  input.captured.hasHead = true;
  input.captured.shortSha = L"abc1234";
  input.captured.stagedItems = 5;  // 点下按钮时列表里是 5 项，现在只剩 2 项。
  CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Tag(plan.blockedReason));
  GC_CHECK(!plan.stateChangeNote.empty());
  GC_CHECK(Contains(plan.stateChangeNote, L"已暂存的条目从 5 项变成 2 项"));
  GC_CHECK(Contains(plan.previewText, L"以刚刚读回的为准"));
  // 那句提示必须排在确认文字最前面，用户第一眼就看到。
  GC_CHECK(plan.previewText.rfind(plan.stateChangeNote, 0) == 0);

  // HEAD 也变了（外部提交了什么又被撤回）：两条一起说。
  input.captured.shortSha = L"fffffff";
  input.captured.stagedItems = 2;
  plan = gc::git::BuildCommitPlan(input);
  GC_CHECK(Contains(plan.stateChangeNote, L"HEAD 从"));

  // 没变化时一个字都不多说。
  input.captured.shortSha = L"abc1234";
  plan = gc::git::BuildCommitPlan(input);
  GC_CHECK(plan.stateChangeNote.empty());

  // 摘要无效（第一次点击、或界面当时没显示 HEAD）：不做无根据的比较。
  CommitPlanInput fresh = BaseInput();
  fresh.captured.valid = false;
  GC_CHECK(gc::git::BuildCommitPlan(fresh).stateChangeNote.empty());
}

GC_TEST(commit_plan_warns_about_index_lock_without_refusing) {
  // 锁归 Git 自己管：本程序不去删、也不等，只在范围说明里提醒一次；
  // 真被 Git 拒绝时，用户看到的是命令窗口里的原始报错与退出码。
  CommitPlanInput input = BaseInput();
  input.workflow.indexLocked = true;
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Tag(plan.blockedReason));
  GC_CHECK(Contains(plan.notice, L"index.lock"));
}

GC_TEST(commit_plan_bounds_the_confirmation_text) {
  // MessageBox 不能滚动：条目与消息行都只列出一部分，其余折成「另有几项／几行」。
  CommitPlanInput input = BaseInput();
  std::vector<ChangeItem> staged;
  for (int index = 0; index < 40; ++index) {
    staged.push_back(Item(ChangeKind::modified, L"M ", L"f" + std::to_wstring(index) + L".txt"));
  }
  input.model.staged = staged;
  std::wstring message = L"标题";
  for (int index = 0; index < 60; ++index) {
    message += L"\n正文第 " + std::to_wstring(index) + L" 行";
  }
  input.message = message;
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK_MESSAGE(!plan.blocked, Tag(plan.blockedReason));
  GC_CHECK(Contains(plan.previewText, L"已暂存的 40 项"));
  GC_CHECK(Contains(plan.previewText, L"另有 25 项未列出"));
  // 消息一共 61 行（标题 + 60 行正文），只列 20 行。
  GC_CHECK(Contains(plan.previewText, L"另有 41 行未列出"));
  GC_CHECK(plan.stagedItems == 40);
}

}  // namespace
