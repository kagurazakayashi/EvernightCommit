// 「创建提交」方案层的纯逻辑测试：不起 Git、不开窗口、不碰文件系统，
// 只把「刚刚读回的仓库现状 + 同一趟问回的身份事实 + 表单结论」交给 BuildCommitPlan，
// 断言它给的是拒绝还是命令，以及那份命令绑定的身份是否就是确认框上写的那一份。
//
// 覆盖：拒绝条件（没有暂存内容、还有冲突、合并/变基没走完、提交者身份缺失或未知、
// 消息文件路径不安全、时间没换算出来、作者身份残缺、仓库形态没有工作区）逐条各自给出原因；
// 通过时命令形态只有 `commit --cleanup=verbatim -F <文件>`（绝不会出现 -a、--no-verify 或路径参数）、
// 环境覆盖只有作者身份与两个时间（提交者身份一概不覆盖），
// 以及确认文字里的范围/身份/两个时间、界面摘要与刚读回的现状不一致时那句先说在前面的提示。
// 身份层另外两组：判读一原始查询（InterpretCommitIdentity）与确认之后的复核比对
// （DescribeCommitIdentityChange）。
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
using gc::git::CommitIdentityFacts;
using gc::git::CommitIdentityQueries;
using gc::git::CommitPlan;
using gc::git::CommitPlanIdentity;
using gc::git::CommitPlanInput;
using gc::git::CommitTimeChoice;
using gc::git::CommitterIdentityState;
using gc::git::GitIdentity;
using gc::git::GitQueryResult;
using gc::git::RepoKind;
using gc::git::RepositoryWorkflowState;
using gc::git::WorkspaceModel;

constexpr std::wstring_view kMessageFile = L"C:\\Temp\\gc-msg-1234-1.txt";

// 完整对象 ID 形状的桩值（40 个十六进制字符）。
constexpr std::wstring_view kHeadA = L"1111111111111111111111111111111111111111";
constexpr std::wstring_view kHeadB = L"2222222222222222222222222222222222222222";
constexpr std::wstring_view kTreeA = L"3333333333333333333333333333333333333333";
constexpr std::wstring_view kTreeB = L"4444444444444444444444444444444444444444";

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

// 一次「Git 给出了回答」的桩：只给定退出码与两路输出，完整度按默认（可信）。
GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
  return result;
}

// 进程根本没跑起来：与「跑完了但退出码非 0」是两种不同的非答复。
GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
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

// `git config --null --get` 的记录以空字节收尾，缺了它就不算一份答复。
std::wstring Nul(std::wstring_view text) {
  std::wstring value(text);
  value.push_back(L'\0');
  return value;
}

// 一次「正常问齐」的原始查询桩：结构与 git/commit_plan 里那四条参数一一对应，
// 只是把执行结果直接给定，判读层因此可以脱离 Git 测试。
CommitIdentityQueries HealthyQueries() {
  CommitIdentityQueries queries;
  queries.requestedRepositoryDirectory = L"P:\\repo";
  queries.worktree = Answer(0, L"P:\\repo\\.git\nP:\\repo\n");
  queries.branchRef = Answer(0, L"refs/heads/main\n");
  queries.headObject = Answer(0, std::wstring(kHeadA) + L"\n");
  queries.indexTree = Answer(0, std::wstring(kTreeA) + L"\n");
  queries.configName = Answer(0, Nul(L"仓库配置身份"));
  queries.configEmail = Answer(0, Nul(L"cfg@example.invalid"));
  queries.workflowProbed = true;
  return queries;
}

// 判读后的「什么都没问题」的身份事实：与 BaseInput 里那份 detection 属于同一趟查询。
CommitIdentityFacts HealthyIdentity() {
  return gc::git::InterpretCommitIdentity(HealthyQueries());
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
  input.identity = HealthyIdentity();
  input.gitExecutable = L"C:\\Program Files\\Git\\bin\\git.exe";
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

  // 确认框摆的是「这一次提交绑定的那一份现状」：索引内容标识与它的来历都要看得见。
  GC_CHECK(Contains(plan.previewText, L"索引内容标识"));
  GC_CHECK(Contains(plan.previewText, std::wstring(kTreeA)));
  GC_CHECK(Contains(plan.previewText, L"完整分支引用：refs/heads/main"));
  GC_CHECK(Contains(plan.previewText, L"HEAD 完整对象 ID"));
  GC_CHECK(Contains(plan.previewText, L"git write-tree"));
  // 边界要说清楚，不能包装成事务级保证。
  GC_CHECK(Contains(plan.previewText, L"index.lock"));
  GC_CHECK(Contains(plan.previewText, L"不是事务级的隔离保证"));
  GC_CHECK(Contains(plan.notice, L"索引内容标识"));

  // 启动绑定的那一份身份：界面之后换没换仓库都不影响这条命令属于谁。
  GC_CHECK(plan.identity.gitExecutable == L"C:\\Program Files\\Git\\bin\\git.exe");
  GC_CHECK(plan.identity.repositoryDirectory == L"P:\\repo");
  GC_CHECK(plan.identity.absoluteGitDir == L"P:\\repo\\.git");
  GC_CHECK(plan.identity.branchRef == L"refs/heads/main");
  GC_CHECK(plan.identity.headObjectId == std::wstring(kHeadA));
  GC_CHECK(plan.identity.indexTreeOid == std::wstring(kTreeA));
  GC_CHECK(plan.identity.messageFilePath == std::wstring(kMessageFile));
  GC_CHECK(plan.identity.messageUtf8Bytes == 128);
  GC_CHECK(plan.identity.author.name == L"张三");
  GC_CHECK(plan.identity.authorGitDate == "@1790784000 +0800");
  GC_CHECK(plan.identity.committerGitDate == "@1790784000 +0800");
  GC_CHECK(plan.identity.committerIdentityText == L"仓库配置身份 <cfg@example.invalid>");
  GC_CHECK(plan.identity.stagedItems == 2);
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
    entry.second(input.identity.workflow);
    const CommitPlan plan = gc::git::BuildCommitPlan(input);
    GC_CHECK_MESSAGE(plan.blocked, "这种流程没走完时不该提交：" + Tag(entry.first));
    GC_CHECK(Contains(plan.blockedReason, L"Git 流程还没走完"));
    // 拒绝里必须说清不替用户终止那种流程（AGENTS 的边界：不擅自 reset/abort）。
    GC_CHECK(Contains(plan.blockedReason, L"也不会替你终止它"));
  }

  // 几种流程同时存在时全列出来，只报一种会让人以为别的无所谓。
  CommitPlanInput both = BaseInput();
  both.identity.workflow.mergeInProgress = true;
  both.identity.workflow.rebaseInProgress = true;
  const std::wstring text = both.identity.workflow.SpecialFlowText();
  GC_CHECK(Contains(text, L"MERGE_HEAD"));
  GC_CHECK(Contains(text, L"rebase-merge"));
  GC_CHECK(Contains(gc::git::BuildCommitPlan(both).blockedReason, L"变基"));
}

GC_TEST(commit_plan_explains_missing_committer_identity) {
  CommitPlanInput missing = BaseInput();
  missing.identity.committer = CommitterIdentityState::missing;
  missing.identity.committerIdentityText.clear();
  const CommitPlan missingPlan = gc::git::BuildCommitPlan(missing);
  GC_CHECK(missingPlan.blocked);
  GC_CHECK(Contains(missingPlan.blockedReason, L"user.name"));
  GC_CHECK(Contains(missingPlan.blockedReason, L"不替你写配置"));

  CommitPlanInput unknown = BaseInput();
  unknown.identity.committer = CommitterIdentityState::unknown;
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
  unborn.identity.headResolved = false;
  unborn.identity.headObjectId.clear();
  const CommitPlan unbornPlan = gc::git::BuildCommitPlan(unborn);
  GC_CHECK_MESSAGE(!unbornPlan.blocked, Tag(unbornPlan.blockedReason));
  GC_CHECK(Contains(unbornPlan.previewText, L"尚无提交"));
  GC_CHECK(Contains(unbornPlan.previewText, L"这次是仓库的第一个提交"));

  CommitPlanInput detached = BaseInput();
  detached.detection.kind = RepoKind::detached;
  detached.detection.branch.clear();
  detached.identity.onBranch = false;
  detached.identity.branchRef.clear();
  detached.identity.branchName.clear();
  const CommitPlan detachedPlan = gc::git::BuildCommitPlan(detached);
  GC_CHECK_MESSAGE(!detachedPlan.blocked, Tag(detachedPlan.blockedReason));
  GC_CHECK(Contains(detachedPlan.previewText, L"不在分支上（游离 HEAD）"));
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
  input.identity.workflow.indexLocked = true;
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

GC_TEST(commit_identity_interpretation_needs_every_answer) {
  const CommitIdentityFacts healthy = HealthyIdentity();
  GC_CHECK_MESSAGE(healthy.queryOk, Tag(healthy.queryFailure));
  GC_CHECK(healthy.repositoryDirectory == L"P:\\repo");
  GC_CHECK(healthy.absoluteGitDir == L"P:\\repo\\.git");
  GC_CHECK(healthy.onBranch);
  GC_CHECK(healthy.branchRef == L"refs/heads/main");
  GC_CHECK(healthy.branchName == L"main");
  GC_CHECK(healthy.headResolved && healthy.headObjectId == std::wstring(kHeadA));
  GC_CHECK(healthy.indexTreeResolved && healthy.indexTreeOid == std::wstring(kTreeA));
  GC_CHECK(healthy.committer == CommitterIdentityState::available);
  GC_CHECK(healthy.committerIdentityText == L"仓库配置身份 <cfg@example.invalid>");
  GC_CHECK(healthy.workflowProbed);

  // 问的不是这块工作区：整份事实作废，绝不能拿它给确认框或复核放行。
  CommitIdentityQueries elsewhere = HealthyQueries();
  elsewhere.worktree = Answer(0, L"Q:\\other\\.git\nQ:\\other\n");
  const CommitIdentityFacts wrong = gc::git::InterpretCommitIdentity(elsewhere);
  GC_CHECK(!wrong.queryOk);
  GC_CHECK(Contains(wrong.queryFailure, L"Git 却把工作区根回答成"));

  // 形态路径只回一行（裸仓库、.git 内部）：没有工作区根就没有可绑定的落点。
  CommitIdentityQueries oneLine = HealthyQueries();
  oneLine.worktree = Answer(0, L"P:\\repo\\.git\n");
  GC_CHECK(!gc::git::InterpretCommitIdentity(oneLine).queryOk);

  // 游离 HEAD 与「尚无提交」是 Git 的明确回答（退出码 1、无输出），不是查询失败。
  CommitIdentityQueries detached = HealthyQueries();
  detached.branchRef = Answer(1, L"");
  const CommitIdentityFacts detachedFacts = gc::git::InterpretCommitIdentity(detached);
  GC_CHECK_MESSAGE(detachedFacts.queryOk, Tag(detachedFacts.queryFailure));
  GC_CHECK(!detachedFacts.onBranch);

  CommitIdentityQueries unborn = HealthyQueries();
  unborn.branchRef = Answer(1, L"");
  unborn.headObject = Answer(1, L"");
  const CommitIdentityFacts unbornFacts = gc::git::InterpretCommitIdentity(unborn);
  GC_CHECK_MESSAGE(unbornFacts.queryOk, Tag(unbornFacts.queryFailure));
  GC_CHECK(!unbornFacts.onBranch);
  GC_CHECK(!unbornFacts.headResolved);

  // 索引算不出树对象（最常见是里面出现了未解决冲突）：这一条没有「明确没有」这一档。
  CommitIdentityQueries noTree = HealthyQueries();
  noTree.indexTree = Answer(128, L"", L"error: unmerged files\n");
  const CommitIdentityFacts noTreeFacts = gc::git::InterpretCommitIdentity(noTree);
  GC_CHECK(!noTreeFacts.queryOk);
  GC_CHECK(Contains(noTreeFacts.queryFailure, L"write-tree"));

  // 对象 ID 的形状先验过再进比较：残缺或短 ID 都会让「一样的内容」判断失真。
  CommitIdentityQueries shortHead = HealthyQueries();
  shortHead.headObject = Answer(0, L"abc1234\n");
  GC_CHECK(!gc::git::InterpretCommitIdentity(shortHead).queryOk);
  CommitIdentityQueries shortTree = HealthyQueries();
  shortTree.indexTree = Answer(0, L"abc1234\n");
  GC_CHECK(!gc::git::InterpretCommitIdentity(shortTree).queryOk);

  // 超时、没跑起来、输出没读全都不是「答案」：一律按读不回来处理。
  CommitIdentityQueries timedOut = HealthyQueries();
  timedOut.worktree.timedOut = true;
  GC_CHECK(!gc::git::InterpretCommitIdentity(timedOut).queryOk);
  CommitIdentityQueries notStarted = HealthyQueries();
  notStarted.headObject = LaunchFailed();
  GC_CHECK(!gc::git::InterpretCommitIdentity(notStarted).queryOk);
  CommitIdentityQueries truncated = HealthyQueries();
  truncated.configEmail.outputComplete = false;
  GC_CHECK(!gc::git::InterpretCommitIdentity(truncated).queryOk);

  // 退出码成功却一个字没有：那不是完整答复。
  CommitIdentityQueries silent = HealthyQueries();
  silent.branchRef = Answer(0, L"");
  GC_CHECK(!gc::git::InterpretCommitIdentity(silent).queryOk);

  // 「这个 key 没设过」是明确答复（退出码 1、无输出）：仍然 queryOk，只是提交者凑不齐。
  CommitIdentityQueries noConfig = HealthyQueries();
  noConfig.configName = Answer(1, L"");
  const CommitIdentityFacts noConfigFacts = gc::git::InterpretCommitIdentity(noConfig);
  GC_CHECK_MESSAGE(noConfigFacts.queryOk, Tag(noConfigFacts.queryFailure));
  GC_CHECK(noConfigFacts.committer == CommitterIdentityState::missing);
  GC_CHECK(noConfigFacts.committerIdentityText.empty());
}

GC_TEST(commit_plan_blocks_when_preflight_disagrees_with_screen) {
  // 预检与界面列表是两趟只读查询，中间照样可能被人改了仓库。两处对不上时不给确认框：
  // 那种框写的会是「一半旧的一半新的」拼出来的承诺。
  const auto blockedWith = [](CommitPlanInput input, std::wstring_view needle) {
    const CommitPlan plan = gc::git::BuildCommitPlan(input);
    GC_CHECK_MESSAGE(plan.blocked, "这一份身份与界面现状对不上时不该给出方案");
    GC_CHECK_MESSAGE(Contains(plan.blockedReason, needle),
                     "拒绝里要指明对不上的是哪一处：" + Tag(plan.blockedReason));
    GC_CHECK(Contains(plan.blockedReason, L"没有给出确认框"));
  };

  CommitPlanInput moved = BaseInput();
  moved.identity.repositoryDirectory = L"P:\\other-repo";
  blockedWith(moved, L"工作区根");

  CommitPlanInput relinked = BaseInput();
  relinked.identity.absoluteGitDir = L"D:\\elsewhere\\.git";
  blockedWith(relinked, L"Git 目录");

  CommitPlanInput headGone = BaseInput();
  headGone.identity.headResolved = false;
  headGone.identity.headObjectId.clear();
  blockedWith(headGone, L"HEAD 能不能解析");

  CommitPlanInput otherBranch = BaseInput();
  otherBranch.identity.branchName = L"topic";
  otherBranch.identity.branchRef = L"refs/heads/topic";
  blockedWith(otherBranch, L"分支名");

  CommitPlanInput nowDetached = BaseInput();
  nowDetached.identity.onBranch = false;
  nowDetached.identity.branchRef.clear();
  nowDetached.identity.branchName.clear();
  blockedWith(nowDetached, L"在不在分支上");

  // 预检整份没读回来：原样转述它的原因，同样不出确认框。
  CommitPlanInput failed = BaseInput();
  failed.identity = CommitIdentityFacts{};
  failed.identity.queryFailure = L"预检没能问齐：git 未返回。";
  const CommitPlan failedPlan = gc::git::BuildCommitPlan(failed);
  GC_CHECK(failedPlan.blocked);
  GC_CHECK(Contains(failedPlan.blockedReason, L"预检没能问齐"));
}

// 「确认框点头时那一份身份」：直接由一份正常方案取出来，比对用例只改「现在这一份」。
CommitPlanIdentity ConfirmedIdentity() {
  const CommitPlan plan = gc::git::BuildCommitPlan(BaseInput());
  GC_CHECK_MESSAGE(!plan.blocked, "基准方案不该被拒绝：" + Tag(plan.blockedReason));
  return plan.identity;
}

GC_TEST(commit_identity_change_is_silent_when_nothing_moved) {
  GC_CHECK(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), HealthyIdentity()).empty());
}

GC_TEST(commit_identity_change_reports_staged_content_swapped) {
  // 条目数一点没变、内容却换了：只有树 ID 看得见这种改动，这正是它存在的理由。
  CommitIdentityFacts current = HealthyIdentity();
  current.indexTreeOid = std::wstring(kTreeB);
  const std::wstring text = gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), current);
  GC_CHECK(Contains(text, L"索引内容从树"));
  GC_CHECK(Contains(text, std::wstring(kTreeA)));
  GC_CHECK(Contains(text, std::wstring(kTreeB)));
  GC_CHECK(Contains(text, L"条目数可以一点没变"));

  // 复核时索引里出现了未解决冲突（树算不出来）：同样是作废。
  CommitIdentityFacts unmerged = HealthyIdentity();
  unmerged.indexTreeResolved = false;
  unmerged.indexTreeOid.clear();
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), unmerged),
                    L"算不出树对象"));
}

GC_TEST(commit_identity_change_reports_branch_switch_at_the_same_head) {
  CommitIdentityFacts switched = HealthyIdentity();
  switched.branchRef = L"refs/heads/topic";
  switched.branchName = L"topic";
  const std::wstring text = gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), switched);
  GC_CHECK(Contains(text, L"refs/heads/main"));
  GC_CHECK(Contains(text, L"refs/heads/topic"));
  GC_CHECK(Contains(text, L"就算 HEAD 还是同一份提交"));

  CommitIdentityFacts detached = HealthyIdentity();
  detached.onBranch = false;
  detached.branchRef.clear();
  detached.branchName.clear();
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), detached),
                    L"不在分支上（游离 HEAD）"));
}

GC_TEST(commit_identity_change_reports_head_and_repository_moves) {
  CommitIdentityFacts committed = HealthyIdentity();
  committed.headObjectId = std::wstring(kHeadB);
  const std::wstring headText = gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), committed);
  GC_CHECK(Contains(headText, L"HEAD 从"));
  GC_CHECK(Contains(headText, std::wstring(kHeadA)));
  GC_CHECK(Contains(headText, std::wstring(kHeadB)));

  // HEAD 变成不可解析（外部把最后一条提交撤掉了）：两次「能不能解析」不同就该作废。
  CommitIdentityFacts becameUnborn = HealthyIdentity();
  becameUnborn.headResolved = false;
  becameUnborn.headObjectId.clear();
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), becameUnborn),
                    L"（读不到）"));

  CommitIdentityFacts moved = HealthyIdentity();
  moved.repositoryDirectory = L"P:\\other-repo";
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), moved), L"工作区根从"));

  CommitIdentityFacts relinked = HealthyIdentity();
  relinked.absoluteGitDir = L"D:\\elsewhere\\.git";
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), relinked),
                    L"换仓库、换工作树"));
}

GC_TEST(commit_identity_change_reports_flows_started_after_confirmation) {
  // 点头之后才开始的 merge / rebase：那种状态下的提交与确认框上写的不是一件事。
  CommitIdentityFacts rebasing = HealthyIdentity();
  rebasing.workflow.rebaseInProgress = true;
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), rebasing), L"变基"));

  // index.lock 只在「确认时没有、现在有了」时算变化：确认框上已经交代过的那把锁不是新情况，
  // 那种场合 Git 自己会拒绝，程序不替它删锁、也不在这里把已经说过的话再说成「仓库又变了」。
  CommitIdentityFacts locked = HealthyIdentity();
  locked.workflow.indexLocked = true;
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), locked), L"index.lock"));
  CommitPlanIdentity confirmedLocked = ConfirmedIdentity();
  confirmedLocked.workflow.indexLocked = true;
  GC_CHECK(gc::git::DescribeCommitIdentityChange(confirmedLocked, locked).empty());

  // 提交者身份换了人：这次提交会记在谁名下是确认框上写明的一项。
  CommitIdentityFacts otherCommitter = HealthyIdentity();
  otherCommitter.committerIdentityText = L"别人 <o@example.invalid>";
  const std::wstring text =
      gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), otherCommitter);
  GC_CHECK(Contains(text, L"提交者身份从"));
  GC_CHECK(Contains(text, L"别人 <o@example.invalid>"));
}

GC_TEST(commit_identity_change_refuses_when_the_recheck_cannot_read) {
  // 「读不回来」与「读回来不一样」都得作废旧方案：前者连现在是什么都不知道。
  CommitIdentityFacts unreadable;
  unreadable.queryFailure = L"预检没能问齐：Git 未在限定时间内返回。";
  const std::wstring text = gc::git::DescribeCommitIdentityChange(ConfirmedIdentity(), unreadable);
  GC_CHECK(Contains(text, L"执行前复核没能读回这个仓库的现状"));
  GC_CHECK(Contains(text, L"Git 未在限定时间内返回"));
  GC_CHECK(Contains(text, L"表单里的内容一个字都没动"));
  // 作废说明里必须交代「命令没发、信息文件删了、要重新确认」。
  GC_CHECK(Contains(gc::git::DescribeCommitIdentityChange(
                        ConfirmedIdentity(),
                        [] {
                          CommitIdentityFacts changed = HealthyIdentity();
                          changed.indexTreeOid = std::wstring(kTreeB);
                          return changed;
                        }()),
                    L"这条提交命令没有发出"));
}

}  // namespace
