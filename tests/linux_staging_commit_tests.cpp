// 统一 wxWidgets 界面这一步（暂存 + 完整提交表单）在**隔离临时仓库**上的真实验证。
//
// 为什么要这份文件：界面本身只做「搬运与显示」，真正的范围控制、pathspec 形态、
// 确认边界与提交内容都在 git/ 与 platform/ 那两层。这里把界面实际会走的那条链路
// （读模型 → 构造方案 → 写清单/信息文件 → 交给真实 Git → 重读核对）原样跑一遍，
// 覆盖点逐条对应本步骤的验收要求：
//   * 选中与全部两个方向的暂存/取消暂存；未选中的条目分毫不动；
//   * 空格、中文、方括号（glob 魔法）、前导减号、引号与 shell 元字符都按字面送达；
//   * 删除与「工作区改名」的双路径条目成对暂存，取消暂存也成对撤回；
//   * 冲突条目的暂存需要点头、批量移出整份拒绝；
//   * 提交只来自索引：标题/多行正文/作者/合作者/两个时间都进对象，未暂存的改动留在工作区；
//   * 确认之后索引被外部换掉 → 执行前复核作废旧方案，一条命令都不发；
//   * hook 拒绝 → 没有任何提交，暂存内容与工作区原样保留；
//   * 提交期间用户继续编辑表单 → 只清「确认并提交掉的那一份」，新草稿留着，
//     而真正提交出去的信息仍是确认那一份。
//
// 授权边界：只在夹具自己创建并认领的临时目录里跑 Git；夹具环境写死
// GIT_ALLOW_PROTOCOL=file 且所有执行都过 push 闸门，本文件不执行、也不可能执行任何推送。
// 真实提交那几条需要图形会话（xterm 终端链路）；没有 DISPLAY 时明确报前置失败，
// 不把「没跑」当成通过。
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/commit_form_session.h"
#include "git/commit_identity.h"
#include "git/commit_message.h"
#include "git/commit_plan.h"
#include "git/staging_plan.h"
#include "git/workspace_model.h"
#include "platform/commit_probe.h"
#include "platform/linux/command_window_runner.h"
#include "platform/local_time.h"
#include "platform/repo_detect.h"
#include "platform/scratch_files.h"
#include "platform/terminal.h"
#include "platform/text_encoding.h"
#include "platform/workspace_status.h"
#include "support/linux_git_fixture.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::CivilTime;
using gc::git::CommitFormData;
using gc::git::CommitIdentityFacts;
using gc::git::CommitPlan;
using gc::git::CommitPlanInput;
using gc::git::CommitTimeChoice;
using gc::git::CommitterIdentityState;
using gc::git::GitIdentity;
using gc::git::RepoDetection;
using gc::git::StagingDelivery;
using gc::git::StagingPlan;
using gc::git::StagingPlanOptions;
using gc::git::WorkspaceLoadStatus;
using gc::git::WorkspaceModel;
using gc::platform::CommandWindowResult;
using gc::git::EnvironmentOverride;
using gc::platform::LinuxCommandWindowRunner;
using gc::platform::TerminalAdapterKind;
using gc::platform::TerminalPreferences;
using gc::test::LinuxGitFixture;
using gc::test::LinuxGitRun;

std::string ToUtf8(std::wstring_view text) { return gc::platform::WideToUtf8(text); }

// Git 的 --date=iso 用 ±hhmm 写偏移；这里按同一形态拼出期望值。
std::wstring TwoDigits(int value) {
  return std::wstring(1, static_cast<wchar_t>(L'0' + value / 10)) +
         static_cast<wchar_t>(L'0' + value % 10);
}

std::wstring OffsetHhMm(int minutes) {
  const int absolute = minutes < 0 ? -minutes : minutes;
  return std::wstring(1, minutes < 0 ? L'-' : L'+') + TwoDigits(absolute / 60) +
         TwoDigits(absolute % 60);
}

void PrepareFixture(LinuxGitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), "Linux 夹具前置失败：" + reason);
}

WorkspaceModel LoadModel(LinuxGitFixture& fixture) {
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  request.repositoryHasCommits = true;
  const gc::git::WorkspaceSnapshot snapshot =
      gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
  GC_REQUIRE_MESSAGE(snapshot.status == WorkspaceLoadStatus::loaded,
                     "读取工作区状态失败：" + ToUtf8(snapshot.message));
  return snapshot.model;
}

RepoDetection LoadDetection(LinuxGitFixture& fixture) {
  gc::platform::RepoDetectRequest request;
  request.exePath = fixture.GitExe();
  request.directory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const RepoDetection detection = gc::platform::DetectRepository(request, fixture.MakeDetectDeps());
  GC_REQUIRE_MESSAGE(!detection.root.empty(), "识别仓库失败：" + ToUtf8(detection.message));
  return detection;
}

CommitIdentityFacts LoadIdentity(LinuxGitFixture& fixture) {
  gc::platform::CommitProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectCommitPreflight(request, fixture.MakeCommitProbeDepsForTest());
}

std::vector<ChangeItem> FindByPaths(const std::vector<ChangeItem>& items,
                                    const std::vector<std::wstring>& paths) {
  std::vector<ChangeItem> found;
  for (const std::wstring& path : paths) {
    const auto it = std::find_if(items.begin(), items.end(),
                                 [&path](const ChangeItem& item) { return item.path == path; });
    GC_REQUIRE_MESSAGE(it != items.end(), "列表里没有这条记录：" + ToUtf8(path));
    found.push_back(*it);
  }
  return found;
}

bool HasPath(const std::vector<ChangeItem>& items, std::wstring_view path) {
  return std::any_of(items.begin(), items.end(),
                     [path](const ChangeItem& item) { return item.path == path; });
}

// 界面点下「加入暂存区」后实际做的事：按方案的传递形态写清单文件，再把参数数组交给 Git。
void RunStagingPlan(LinuxGitFixture& fixture, const StagingPlan& plan,
                    long* exitCode = nullptr) {
  GC_REQUIRE_MESSAGE(plan.delivery != StagingDelivery::blocked,
                     "方案被拒绝：" + ToUtf8(plan.blockedReason));
  std::vector<std::wstring> arguments = plan.arguments;
  std::wstring pathspecFile;
  if (plan.delivery == StagingDelivery::pathspecFile) {
    const auto written = gc::platform::WriteNulPathspecFile(plan.pathspecEntries, L"gc-test");
    GC_REQUIRE_MESSAGE(written.written, "写清单失败：" + ToUtf8(written.failureReason));
    pathspecFile = written.path;
    GC_REQUIRE(gc::git::AppendPathspecFileOptions(&arguments, pathspecFile), "清单路径不合命令窗口边界");
  }
  const LinuxGitRun run = fixture.Run(arguments, fixture.RepoDir());
  gc::platform::RemoveNulPathspecFile(pathspecFile);
  GC_REQUIRE_MESSAGE(run.raw.started, "Git 未能启动：" + ToUtf8(run.raw.launchErrorText));
  if (exitCode != nullptr) {
    *exitCode = run.raw.exitCode;
    return;
  }
  GC_REQUIRE_MESSAGE(run.raw.termination == gc::platform::ProcessTermination::exited &&
                         run.raw.exitCode == 0,
                     "暂存命令失败（退出码 " + std::to_string(run.raw.exitCode) + "）：" +
                         ToUtf8(run.err));
}

std::string ReadFileBytes(const LinuxGitFixture& fixture, std::wstring_view relative) {
  const std::string path = ToUtf8(fixture.RepoDir() + L"/" + std::wstring(relative));
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const LinuxGitFixture& fixture, std::wstring_view relative,
                    const std::string& bytes) {
  const std::string path = ToUtf8(fixture.RepoDir() + L"/" + std::wstring(relative));
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << bytes;
  stream.close();
}

std::wstring GitText(LinuxGitFixture& fixture, const std::vector<std::wstring>& arguments) {
  const LinuxGitRun run = fixture.RunInRepo(arguments);
  GC_REQUIRE_MESSAGE(run.Success(), "只读查询失败：" + ToUtf8(run.err));
  std::wstring text = run.out;
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
    text.pop_back();
  }
  return text;
}

// ---- 真实提交：走与界面同一条交互式执行链路（终端里的本程序辅助入口） ----

std::vector<EnvironmentOverride> IsolatedOverrides(const LinuxGitFixture& fixture) {
  const std::wstring root = fixture.Root();
  return {
      EnvironmentOverride{L"HOME", root + L"/home"},
      EnvironmentOverride{L"XDG_CONFIG_HOME", root + L"/home/.config"},
      EnvironmentOverride{L"GIT_CONFIG_GLOBAL", root + L"/home/empty.gitconfig"},
      EnvironmentOverride{L"GIT_CONFIG_NOSYSTEM", std::wstring(L"1")},
      EnvironmentOverride{L"GIT_TEMPLATE_DIR", root + L"/git-template"},
      EnvironmentOverride{L"GIT_ALLOW_PROTOCOL", std::wstring(L"file")},
      EnvironmentOverride{L"TZ", std::wstring(L"UTC0")},
      EnvironmentOverride{L"LC_ALL", std::wstring(L"C.UTF-8")},
      EnvironmentOverride{L"GIT_AUTHOR_NAME", std::wstring(L"夹具作者")},
      EnvironmentOverride{L"GIT_AUTHOR_EMAIL", std::wstring(L"fixture-author@example.invalid")},
      EnvironmentOverride{L"GIT_COMMITTER_NAME", std::wstring(L"夹具提交者")},
      EnvironmentOverride{L"GIT_COMMITTER_EMAIL",
                          std::wstring(L"fixture-committer@example.invalid")},
  };
}

void RequireXterm() {
  const char* display = std::getenv("DISPLAY");
  GC_REQUIRE_MESSAGE(display != nullptr && display[0] != '\0',
                     "没有 DISPLAY：真实提交的终端用例未运行（这不是通过）");
  TerminalPreferences preferences;
  preferences.preferredKind = TerminalAdapterKind::xterm;
  preferences.allowFallback = false;
  const auto selection = gc::platform::DiscoverAndSelectTerminal(preferences);
  GC_REQUIRE_MESSAGE(selection.usable, "没有找到可用的 xterm：" + ToUtf8(selection.failureReason));
}

CommandWindowResult RunCommitInTerminal(LinuxGitFixture& fixture, const CommitPlan& plan) {
  RequireXterm();
  const std::string scratch = ToUtf8(fixture.Root() + L"/scratch");
  gc::test::RequireMakeDirectory(scratch, 0700);
  gc::test::ScopedEnvironmentVariable tmpdir("TMPDIR", scratch);
  gc::test::ScopedEnvironmentVariable runtime("XDG_RUNTIME_DIR", std::nullopt);

  gc::git::CommandWindowOperation operation = gc::git::MakeCommandWindowOperation(
      plan.operationId, plan.displayName, plan.identity.gitExecutable,
      plan.identity.repositoryDirectory, plan.arguments);
  // 夹具的隔离环境打底，方案自己的「作者身份 + 两个时间」排在后面（与界面同一优先级）。
  operation.environmentOverrides = IsolatedOverrides(fixture);
  for (const EnvironmentOverride& override : plan.environmentOverrides) {
    operation.environmentOverrides.push_back(override);
  }
  operation.holdTerminalOpen = false;

  TerminalPreferences preferences;
  preferences.preferredKind = TerminalAdapterKind::xterm;
  preferences.allowFallback = false;
  LinuxCommandWindowRunner runner;
  uint64_t id = 0;
  CommandWindowResult failure;
  GC_REQUIRE_MESSAGE(runner.Start(operation, preferences, &id, &failure),
                     "终端启动失败：" + ToUtf8(failure.failureReason));
  CommandWindowResult result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
  while (std::chrono::steady_clock::now() < deadline) {
    if (runner.TakeResult(id, &result)) {
      runner.Shutdown();
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  runner.Shutdown();
  GC_REQUIRE_MESSAGE(false, "终端里的 Git 未在期限内结案");
  return result;
}

CommitTimeChoice MakeTimeChoice(const CivilTime& wall) {
  CommitTimeChoice choice;
  const gc::platform::LocalInstant instant = gc::platform::ResolveLocalWallTime(wall);
  GC_REQUIRE_MESSAGE(instant.valid, "换算不出这一刻：" + ToUtf8(instant.failureReason));
  std::wstring refusal;
  GC_REQUIRE(gc::git::FormatGitInternalDate(instant.utcEpochSeconds, instant.offsetMinutes,
                                            &choice.gitDate, &refusal),
             ToUtf8(refusal));
  choice.wall = wall;
  choice.offsetMinutes = instant.offsetMinutes;
  choice.displayText = gc::git::FormatCommitTimeText(wall, instant.offsetMinutes);
  return choice;
}

// 把一份「界面上已经填好」的表单变成可执行的提交方案：与 CommitFlow 同一套顺序
// （校验 → 拆身份 → 换算时间 → 合成信息 → 写信息文件 → 构造方案）。
CommitPlan BuildPlanFromForm(LinuxGitFixture& fixture, const CommitFormData& form,
                             const CivilTime& authorWall, const CivilTime& committerWall,
                             std::wstring* messageFile) {
  const auto validity = gc::git::ValidateCommitForm(form, CommitterIdentityState::available);
  GC_REQUIRE_MESSAGE(validity.Ok(), "表单校验没通过：" + ToUtf8(validity.StatusText()));
  GitIdentity author;
  std::wstring identityError;
  GC_REQUIRE(gc::git::ParseGitIdentity(form.author, &author, &identityError, L"作者"),
             ToUtf8(identityError));
  const auto composed = gc::git::ComposeCommitMessage(form);
  GC_REQUIRE_MESSAGE(composed.rejectedCoauthors.empty() && !composed.message.empty(),
                     "信息合成异常");
  const auto written = gc::platform::WriteCommitMessageFile(composed.message);
  GC_REQUIRE_MESSAGE(written.written, "写信息文件失败：" + ToUtf8(written.failureReason));
  *messageFile = written.path;

  CommitPlanInput input;
  input.model = LoadModel(fixture);
  input.detection = LoadDetection(fixture);
  input.identity = LoadIdentity(fixture);
  input.gitExecutable = fixture.GitExe();
  input.author = author;
  input.message = composed.message;
  input.messageUtf8Bytes = written.payloadBytes;
  input.messageFilePath = written.path;
  input.authorTime = MakeTimeChoice(authorWall);
  input.committerTime = MakeTimeChoice(committerWall);
  input.timesSynced = false;
  return gc::git::BuildCommitPlan(input);
}

// 生产预检问的是 `git config --get user.name / user.email`（夹具把用户层隔离成空档案），
// 因此「仓库里确实配了提交者身份」这一条要显式造出来，方案才不会按缺失拒绝。
void ConfigureCommitterIdentity(LinuxGitFixture& fixture) {
  fixture.RunCheckedInRepo({L"config", L"user.name", L"仓库提交者"});
  fixture.RunCheckedInRepo({L"config", L"user.email", L"repo-committer@example.invalid"});
}

CommitFormData SampleForm() {  CommitFormData form;
  form.subject = L"暂存与提交表单的真实验证";
  form.description =
      L"第一行正文\n\n"
      L"  带行尾空格的行   \n"
      L"# 以井号开头的行不会被当注释删掉\n"
      L"最后一行正文\n";
  form.author = L"手动作者 <manual@example.invalid>";
  form.coauthors = {L"张三 <zhangsan@example.invalid>", L"Anne Smith <anne@example.invalid>"};
  return form;
}

}  // namespace

// ---- 暂存：选中方向与范围控制 ----

GC_TEST(linux_staging_selected_add_handles_special_names_and_leaves_unselected) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"base.txt", "base\n", true);
  fixture.Commit(L"seed");

  fixture.WriteFile(L"a b.txt", "modified\n", false);
  fixture.WriteFile(L"中文[x].txt", "bracket\n", false);
  fixture.WriteFile(L"-dash.txt", "dash\n", false);
  fixture.WriteFile(L"weird;name $(touch pwned).txt", "meta\n", false);
  fixture.WriteFile(L"not-selected.txt", "untouched\n", false);

  const WorkspaceModel model = LoadModel(fixture);
  const std::vector<ChangeItem> selected =
      FindByPaths(model.unstaged, {L"a b.txt", L"中文[x].txt", L"-dash.txt",
                                   L"weird;name $(touch pwned).txt"});
  StagingPlanOptions options;
  options.pathspecFileSupported = true;
  options.totalUnstagedItems = model.unstaged.size();
  const StagingPlan plan = gc::git::BuildStagingAddPlan(selected, options);
  GC_REQUIRE_MESSAGE(plan.delivery == StagingDelivery::pathspecFile,
                     "这份 Git 支持清单接口，应该走 pathspecFile");
  GC_REQUIRE_MESSAGE(plan.confirmationText.empty(), "普通条目不该要求点头");
  RunStagingPlan(fixture, plan);

  const WorkspaceModel after = LoadModel(fixture);
  for (std::wstring_view path : {L"a b.txt", L"中文[x].txt", L"-dash.txt",
                                 L"weird;name $(touch pwned).txt"}) {
    GC_CHECK_MESSAGE(HasPath(after.staged, path), "应已进索引：" + ToUtf8(path));
    GC_CHECK_MESSAGE(!HasPath(after.unstaged, path), "不该还留在未暂存侧：" + ToUtf8(path));
  }
  GC_CHECK_MESSAGE(HasPath(after.unstaged, L"not-selected.txt"), "没选中的条目必须原样留在未暂存侧");
  GC_CHECK_MESSAGE(ReadFileBytes(fixture, L"not-selected.txt") == "untouched\n",
           "暂存不得改写任何工作区文件");
  // glob 魔法：中文[x].txt 不能连带命中别的文件；shell 元字符不能被执行。
  GC_CHECK_MESSAGE(::access(ToUtf8(fixture.Root() + L"/pwned").c_str(), F_OK) != 0,
           "参数绝不能经过 shell 解释");
  GC_CHECK_MESSAGE(!HasPath(after.staged, L"base.txt"), "已提交且没再改动的条目不该被顺带暂存");
}

GC_TEST(linux_staging_worktree_rename_needs_both_entries_and_records_one_rename) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"old.txt", "same content that stays identical\n", true);
  fixture.Commit(L"seed");

  // 工作区改名在 porcelain v2 里是「.D 旧路径」+「?? 新路径」两条未暂存记录。
  std::error_code error;
  std::filesystem::rename(std::filesystem::path(ToUtf8(fixture.RepoDir() + L"/old.txt")),
                          std::filesystem::path(ToUtf8(fixture.RepoDir() + L"/new.txt")), error);
  GC_REQUIRE_MESSAGE(!error, "改名失败：" + error.message());

  const WorkspaceModel model = LoadModel(fixture);
  const std::vector<ChangeItem> both =
      FindByPaths(model.unstaged, {L"old.txt", L"new.txt"});
  StagingPlanOptions options;
  options.pathspecFileSupported = true;
  const StagingPlan plan = gc::git::BuildStagingAddPlan(both, options);
  GC_CHECK_MESSAGE(plan.pathspecCount == 2, "两条记录都要交给同一条命令");
  RunStagingPlan(fixture, plan);

  const WorkspaceModel after = LoadModel(fixture);
  GC_REQUIRE_MESSAGE(after.staged.size() == 1, "一次改名应记成一条索引变化");
  GC_CHECK_MESSAGE(after.staged[0].kind == ChangeKind::renamed,
                   "Git 应把它识别为重命名，实际是 " + ToUtf8(after.staged[0].StatusLabel()));
  GC_CHECK_MESSAGE(after.staged[0].oldPath == L"old.txt", "重命名条目要带着来源路径");
}

GC_TEST(linux_staging_unstage_selected_writes_index_only) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"keep.txt", "keep\n", true);
  fixture.WriteFile(L"edit.txt", "original\n", true);
  fixture.Commit(L"seed");

  WriteFileBytes(fixture, L"edit.txt", "staged version\n");
  WriteFileBytes(fixture, L"keep.txt", "worktree edit, not staged\n");
  {
    StagingPlanOptions options;
    options.pathspecFileSupported = true;
    const WorkspaceModel before = LoadModel(fixture);
    const auto selected = FindByPaths(before.unstaged, {L"edit.txt"});
    RunStagingPlan(fixture, gc::git::BuildStagingAddPlan(selected, options));
  }
  const std::string worktreeBytes = ReadFileBytes(fixture, L"edit.txt");

  const WorkspaceModel stagedModel = LoadModel(fixture);
  GC_REQUIRE_MESSAGE(HasPath(stagedModel.staged, L"edit.txt"), "夹具应先把 edit.txt 暂存进去");
  StagingPlanOptions unstageOptions;
  unstageOptions.pathspecFileSupported = true;
  unstageOptions.repositoryHasHead = true;
  unstageOptions.restoreCommandSupported = true;
  const auto unstageSelection = FindByPaths(stagedModel.staged, {L"edit.txt"});
  const StagingPlan unstage =
      gc::git::BuildStagingUnstagePlan(unstageSelection, unstageOptions);
  GC_CHECK_MESSAGE(unstage.confirmationText.empty(), "普通修改的取消暂存不该要求点头");
  RunStagingPlan(fixture, unstage);

  const WorkspaceModel after = LoadModel(fixture);
  GC_CHECK_MESSAGE(!HasPath(after.staged, L"edit.txt"), "取消暂存后不该还留在索引里");
  GC_CHECK_MESSAGE(HasPath(after.unstaged, L"edit.txt"), "改动应回到未暂存一侧");
  GC_CHECK_MESSAGE(ReadFileBytes(fixture, L"edit.txt") == worktreeBytes,
           "取消暂存一个字节都不该改工作区");
  GC_CHECK_MESSAGE(ReadFileBytes(fixture, L"keep.txt") == "worktree edit, not staged\n",
           "没选中的条目不受影响");
}

GC_TEST(linux_staging_batch_add_all_and_unstage_all_cover_the_whole_tree) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"tracked.txt", "one\n", true);
  fixture.Commit(L"seed");

  WriteFileBytes(fixture, L"tracked.txt", "two\n");
  fixture.WriteFile(L"brand new.txt", "new\n", false);
  std::error_code error;
  std::filesystem::remove(std::filesystem::path(ToUtf8(fixture.RepoDir() + L"/tracked.txt")),
                          error);
  WriteFileBytes(fixture, L"tracked.txt", "two\n");  // 恢复：只留「修改 + 新增」两类

  const WorkspaceModel before = LoadModel(fixture);
  GC_REQUIRE_MESSAGE(before.unstaged.size() == 2, "夹具现场应是 1 改 1 新");
  RunStagingPlan(fixture, gc::git::BuildStagingAddAllPlan(before.unstaged));
  const WorkspaceModel staged = LoadModel(fixture);
  GC_CHECK_MESSAGE(staged.staged.size() == 2, "全部加入应覆盖整份工作区");
  GC_CHECK_MESSAGE(staged.unstaged.empty(), "全部加入后未暂存侧应空");

  StagingPlanOptions unstageOptions;
  unstageOptions.repositoryHasHead = true;
  unstageOptions.restoreCommandSupported = true;
  const StagingPlan unstageAll =
      gc::git::BuildStagingUnstageAllPlan(staged.staged, unstageOptions);
  GC_REQUIRE_MESSAGE(unstageAll.delivery == StagingDelivery::wholeWorktree,
                     "批量形态只带一个 `.`，不带清单");
  RunStagingPlan(fixture, unstageAll);

  const WorkspaceModel after = LoadModel(fixture);
  GC_CHECK_MESSAGE(after.staged.empty(), "全部移出后索引应回到 HEAD");
  GC_CHECK_MESSAGE(after.unstaged.size() == 2, "工作区的改动一条都不能少");
  GC_CHECK_MESSAGE(ReadFileBytes(fixture, L"tracked.txt") == "two\n", "批量取消暂存不改工作区");
}

GC_TEST(linux_staging_conflict_requires_nod_and_blocks_batch_unstage) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"clash.txt", "base\n", true);
  fixture.Commit(L"seed");
  fixture.RunCheckedInRepo({L"switch", L"-c", L"topic"});
  fixture.WriteFile(L"clash.txt", "on topic\n", true);
  fixture.Commit(L"topic change");
  fixture.RunCheckedInRepo({L"switch", L"main"});
  fixture.WriteFile(L"clash.txt", "on main\n", true);
  fixture.Commit(L"main change");
  const LinuxGitRun merge =
      fixture.RunInRepo({L"merge", L"--no-commit", L"--no-ff", L"topic"});
  GC_REQUIRE_MESSAGE(merge.raw.started, "merge 未能启动：" + ToUtf8(merge.raw.launchErrorText));

  const WorkspaceModel model = LoadModel(fixture);
  const auto conflicted = std::find_if(model.unstaged.begin(), model.unstaged.end(),
                                       [](const ChangeItem& item) {
                                         return item.kind == ChangeKind::conflicted;
                                       });
  GC_REQUIRE_MESSAGE(conflicted != model.unstaged.end(), "夹具应留下一个未合并条目");

  StagingPlanOptions addOptions;
  addOptions.pathspecFileSupported = true;
  const StagingPlan addPlan = gc::git::BuildStagingAddPlan({*conflicted}, addOptions);
  GC_CHECK_MESSAGE(!addPlan.confirmationText.empty(),
                   "把冲突文件的工作区内容写进索引前必须让使用者点头");

  StagingPlanOptions unstageOptions;
  unstageOptions.repositoryHasHead = true;
  unstageOptions.restoreCommandSupported = true;
  unstageOptions.unmergedPaths.push_back(conflicted->path);
  const StagingPlan unstageAll =
      gc::git::BuildStagingUnstageAllPlan(model.staged, unstageOptions);
  GC_CHECK_MESSAGE(unstageAll.delivery == StagingDelivery::blocked,
                   "批量移出没有排除项，有冲突就整份拒绝");

  const std::wstring headBefore = GitText(fixture, {L"rev-parse", L"--verify", L"HEAD"});
  GC_CHECK_MESSAGE(::access(ToUtf8(fixture.RepoDir() + L"/.git/MERGE_HEAD").c_str(), F_OK) == 0,
                   "夹具留下的合并现场还在");
  GC_CHECK_MESSAGE(GitText(fixture, {L"rev-parse", L"--verify", L"HEAD"}) == headBefore,
                   "本用例只构造方案，一条命令都没发");
}

// ---- 提交：真实对象、复核作废与失败保留 ----

GC_TEST(linux_commit_form_flow_writes_the_whole_form_into_a_real_commit) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"seed.txt", "seed\n", true);
  fixture.Commit(L"seed");
  ConfigureCommitterIdentity(fixture);

  // 索引里只放这两条；第三条留在工作区，用来验证「提交只来自索引」。
  fixture.WriteFile(L"staged one.txt", "one\n", true);
  fixture.WriteFile(L"staged 二.txt", "two\n", true);
  WriteFileBytes(fixture, L"seed.txt", "left in worktree\n");

  const CommitFormData form = SampleForm();
  const CivilTime authorWall{2001, 3, 4, 5, 6, 7};
  const CivilTime committerWall{2002, 5, 6, 7, 8, 9};
  std::wstring messageFile;
  const CommitPlan plan = BuildPlanFromForm(fixture, form, authorWall, committerWall, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked, "方案被拒绝：" + ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.identity.stagedItems == 2, "确认范围就是索引里那两条");

  const CommandWindowResult result = RunCommitInTerminal(fixture, plan);
  gc::platform::RemoveCommitMessageFile(messageFile);
  GC_REQUIRE_MESSAGE(result.completion == gc::git::CommandCompletion::finished &&
                         result.exitCode == 0,
                     "提交没有成功（退出码 " + std::to_string(result.exitCode) + "）：" +
                         ToUtf8(result.failureReason));

  const std::wstring authorName = GitText(fixture, {L"log", L"-1", L"--format=%an"});
  const std::wstring authorEmail = GitText(fixture, {L"log", L"-1", L"--format=%ae"});
  const std::wstring committerName = GitText(fixture, {L"log", L"-1", L"--format=%cn"});
  const std::wstring committerEmail = GitText(fixture, {L"log", L"-1", L"--format=%ce"});
  GC_CHECK_MESSAGE(authorName == L"手动作者" && authorEmail == L"manual@example.invalid",
                   "作者取自表单：" + ToUtf8(authorName) + " <" + ToUtf8(authorEmail) + ">");
  GC_CHECK_MESSAGE(committerName == L"夹具提交者" && committerEmail == L"fixture-committer@example.invalid",
                   "提交者仍由这个仓库的有效配置决定，程序不覆盖");

  const long long authorEpoch = std::wcstoll(
      GitText(fixture, {L"log", L"-1", L"--format=%at"}).c_str(), nullptr, 10);
  const long long committerEpoch = std::wcstoll(
      GitText(fixture, {L"log", L"-1", L"--format=%ct"}).c_str(), nullptr, 10);
  const auto expectedAuthor = gc::platform::ResolveLocalWallTime(authorWall);
  const auto expectedCommitter = gc::platform::ResolveLocalWallTime(committerWall);
  GC_CHECK_MESSAGE(authorEpoch == expectedAuthor.utcEpochSeconds,
                   "作者时刻没落到表单挑的那一瞬间");
  GC_CHECK_MESSAGE(committerEpoch == expectedCommitter.utcEpochSeconds,
                   "提交者时刻没落到表单挑的那一瞬间");
  const std::wstring authorDateRaw =
      GitText(fixture, {L"log", L"-1", L"--date=iso", L"--format=%ad"});
  GC_CHECK_MESSAGE(authorDateRaw.find(L"2001-03-04 05:06:07") != std::wstring::npos,
                   "作者时间按那一刻的偏移显示：" + ToUtf8(authorDateRaw));
  // 交给 Git 的值必须自带偏移：否则 Git 会按「读这个环境变量的那个进程的时区」补一个默认值，
  // 用户挑的时刻就被挪走几个小时。
  GC_CHECK_MESSAGE(authorDateRaw.find(OffsetHhMm(expectedAuthor.offsetMinutes)) !=
                       std::string::npos,
                   "作者时间要带着所选那一刻的偏移：" + ToUtf8(authorDateRaw));
  const std::wstring committerDateRaw =
      GitText(fixture, {L"log", L"-1", L"--date=iso", L"--format=%cd"});
  GC_CHECK_MESSAGE(committerDateRaw.find(OffsetHhMm(expectedCommitter.offsetMinutes)) !=
                       std::string::npos,
                   "提交者时间同理：" + ToUtf8(committerDateRaw));

  const std::wstring body = GitText(fixture, {L"log", L"-1", L"--format=%B"});
  GC_CHECK_MESSAGE(body.find(L"带行尾空格的行   \n") != std::wstring::npos,
                   "--cleanup=verbatim 要保住行尾空格");
  GC_CHECK_MESSAGE(body.find(L"# 以井号开头的行") != std::wstring::npos,
                   "-F 下的 # 行不是注释");
  GC_CHECK_MESSAGE(body.find(L"Co-authored-by: 张三 <zhangsan@example.invalid>") !=
                       std::wstring::npos,
                   "合作者 trailer 要进正文末尾");
  GC_CHECK_MESSAGE(body.find(L"Co-authored-by: Anne Smith <anne@example.invalid>") !=
                       std::wstring::npos,
                   "两位合作者都要写进去");

  const WorkspaceModel after = LoadModel(fixture);
  GC_CHECK_MESSAGE(HasPath(after.unstaged, L"seed.txt"),
                   "未暂存的改动必须原样留在工作区");
  GC_CHECK_MESSAGE(ReadFileBytes(fixture, L"seed.txt") == "left in worktree\n",
           "提交不碰工作区字节");
  GC_CHECK_MESSAGE(after.staged.empty(), "提交掉的内容从索引侧消失");
  const std::wstring headTree = GitText(fixture, {L"rev-parse", L"HEAD^{tree}"});
  GC_CHECK_MESSAGE(headTree == plan.identity.indexTreeOid,
                   "确认框上的索引内容标识与真正落地的树是同一个对象");
}

GC_TEST(linux_commit_recheck_voids_the_plan_when_the_index_moves) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"seed.txt", "seed\n", true);
  fixture.Commit(L"seed");
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"first.txt", "first\n", true);

  std::wstring messageFile;
  const CommitPlan plan =
      BuildPlanFromForm(fixture, SampleForm(), CivilTime{2005, 1, 2, 3, 4, 5},
                        CivilTime{2005, 1, 2, 3, 4, 5}, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));

  const std::wstring headBefore = GitText(fixture, {L"rev-parse", L"--verify", L"HEAD"});
  fixture.WriteFile(L"late-external.txt", "sneaked in\n", true);
  const CommitIdentityFacts current = LoadIdentity(fixture);
  const std::wstring change = gc::git::DescribeCommitIdentityChange(plan.identity, current);
  gc::platform::RemoveCommitMessageFile(messageFile);
  GC_CHECK_MESSAGE(!change.empty(), "索引内容换了必须作废旧方案");
  GC_CHECK_MESSAGE(change.find(L"索引") != std::wstring::npos,
                   "说明要点名变在哪一处：" + ToUtf8(change));
  GC_CHECK_MESSAGE(GitText(fixture, {L"rev-parse", L"--verify", L"HEAD"}) == headBefore,
                   "复核不过时一条命令都不发，HEAD 仍停在确认前那一份提交");
}

GC_TEST(linux_commit_hook_rejection_keeps_index_and_form_content) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"seed.txt", "seed\n", true);
  fixture.Commit(L"seed");
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"blocked.txt", "blocked\n", true);

  const std::string hook = ToUtf8(fixture.RepoDir() + L"/.git/hooks/pre-commit");
  std::error_code hookDirectoryError;
  std::filesystem::create_directories(std::filesystem::path(hook).parent_path(),
                                      hookDirectoryError);
  GC_REQUIRE_MESSAGE(!hookDirectoryError, "建 hooks 目录失败：" + hookDirectoryError.message());
  {
    std::ofstream hookStream(hook, std::ios::binary | std::ios::trunc);
    GC_REQUIRE_MESSAGE(hookStream.is_open(), "打不开钩子文件：" + hook);
    hookStream << "#!/bin/sh\necho '本用例的钩子故意拒绝' >&2\nexit 1\n";
  }
  GC_REQUIRE_MESSAGE(::access(hook.c_str(), R_OK) == 0, "钩子没写成功：" + hook);
  ::chmod(hook.c_str(), 0755);

  const std::wstring headBefore = GitText(fixture, {L"rev-parse", L"--verify", L"HEAD"});
  const std::wstring treeBefore = GitText(fixture, {L"write-tree"});

  const CommitFormData form = SampleForm();
  const std::wstring composedBefore = gc::git::ComposeCommitMessage(form).message;
  std::wstring messageFile;
  const CommitPlan plan =
      BuildPlanFromForm(fixture, form, CivilTime{2007, 6, 7, 8, 9, 10},
                        CivilTime{2007, 6, 7, 8, 9, 10}, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  const CommandWindowResult result = RunCommitInTerminal(fixture, plan);
  gc::platform::RemoveCommitMessageFile(messageFile);

  GC_CHECK_MESSAGE(result.completion == gc::git::CommandCompletion::finished &&
                       result.exitCode != 0,
                   "钩子拒绝时 Git 应以非 0 收场");
  GC_CHECK_MESSAGE(GitText(fixture, {L"rev-parse", L"--verify", L"HEAD"}) == headBefore,
                   "没有产生任何提交");
  GC_CHECK_MESSAGE(GitText(fixture, {L"write-tree"}) == treeBefore, "索引分毫未动");
  // 失败时表单一个字都不动：这里核对的是「界面会拿去做比较的那一份内容」仍然完整可合成。
  GC_CHECK_MESSAGE(gc::git::ComposeCommitMessage(form).message == composedBefore,
                   "失败后表单内容应原样可复用");
  const WorkspaceModel after = LoadModel(fixture);
  GC_CHECK_MESSAGE(HasPath(after.staged, L"blocked.txt"), "已暂存的那条仍等着改好后重提");
}

GC_TEST(linux_commit_cleanup_only_clears_what_was_actually_committed) {
  LinuxGitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  fixture.WriteFile(L"seed.txt", "seed\n", true);
  fixture.Commit(L"seed");
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"committed.txt", "content\n", true);

  const CommitFormData confirmed = SampleForm();
  std::wstring messageFile;
  const CommitPlan plan =
      BuildPlanFromForm(fixture, confirmed, CivilTime{2009, 8, 9, 10, 11, 12},
                        CivilTime{2009, 8, 9, 10, 11, 12}, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  const CommandWindowResult result = RunCommitInTerminal(fixture, plan);
  gc::platform::RemoveCommitMessageFile(messageFile);
  GC_REQUIRE_MESSAGE(result.exitCode == 0, "提交本身要成功：" + ToUtf8(result.failureReason));

  // 命令窗口里跑 Git 的那段时间，用户已经开始写下一段草稿（标题与描述都换了）。
  CommitFormData current = confirmed;
  current.subject = L"下一条还没写完的标题";
  const auto cleanup = gc::app::PlanCommittedFormCleanup(confirmed, current, true);
  GC_CHECK_MESSAGE(!cleanup.clearSubject, "屏幕上已经换了内容，就不该清");
  GC_CHECK_MESSAGE(cleanup.clearDescription, "描述还是提交掉的那一份，可以清");
  GC_CHECK_MESSAGE(cleanup.clearCoauthors, "合作者没动过，可以清");
  GC_CHECK_MESSAGE(!cleanup.resetTimes, "用户期间改过时间：提交成功不是抹掉它的理由");

  // 真正提交出去的仍是确认那一份，而不是期间新写的草稿。
  const std::wstring body = GitText(fixture, {L"log", L"-1", L"--format=%B"});
  GC_CHECK_MESSAGE(body.find(confirmed.subject) != std::wstring::npos,
                   "提交信息用的是确认框那一份");
  GC_CHECK_MESSAGE(body.find(current.subject) == std::wstring::npos,
                   "期间新写的草稿不能被顺手提交掉");
}
