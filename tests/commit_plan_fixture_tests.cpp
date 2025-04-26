// 「创建提交」的集成测试：在夹具自建的临时仓库里，把**生产代码**合成出来的
// 命令参数与环境覆盖原样交给真实 Git 执行，然后读回真正的提交对象逐项核对。
//
// 覆盖的验收点（对应 AGENTS.md 与本步骤要求）：
//   * 提交对象里的作者身份、作者时间、提交者时间与偏移，就是方案交出去的那一对值；
//     提交者身份没有被覆盖，仍由仓库的有效配置/环境决定；
//   * 提交信息一字不改：中文、行尾空格、正文中间连续空行、以 # 开头的行都不被 cleanup 收拾；
//   * 合作者 trailer 只有一条，排在末尾；
//   * 提交范围只限索引里那一份：未暂存的那条改动既不进气泡树，也原样留在工作区；
//   * pre-commit hook 拒绝时：没有任何提交产生，暂存内容分毫未动（表单保留是界面行为，见 README 3.9）；
//   * 真实冲突索引里 write-tree 问不出内容标识，预检当场失败，一条命令都不交出；
//   * 「确认框绑定的那一份索引内容」与「Git 真正写进仓库的那棵树」是同一个对象：
//     提交落地后 HEAD^{tree} 必须逐字符等于方案里的索引内容标识；
//   * 身份预检问到的各项（工作区根／Git 目录／完整分支引用／HEAD 完整 ID／提交者配置身份／
//     流程痕迹）都由真实 Git 回答，且除了 write-tree 写出的树对象以外不改变仓库状态；
//   * 索引内容标识只跟着索引走：只改工作区不动它，暂存后条目数不变而内容换了就一定变；
//   * 确认之后的三种真实变化都会作废旧方案——同一 HEAD 上换分支、暂存内容被换掉、合并开始；
//   * UTF-8 信息文件的写出、编码正确性与用后回收。
// 只在夹具自己创建并认领所有权的临时目录里跑 Git，绝不接触开发仓库、用户仓库或远端。
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_date.h"
#include "git/commit_message.h"
#include "git/commit_plan.h"
#include "git/workspace_model.h"
#include "platform/windows/commit_message_file.h"
#include "platform/windows/commit_probe.h"
#include "platform/windows/repo_detect.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"
#include "platform/windows/workspace_status.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeKind;
using gc::git::CommitFormData;
using gc::git::CommitPlan;
using gc::git::CommitPlanInput;
using gc::git::CommitTimeChoice;
using gc::git::CommitterIdentityState;
using gc::git::ComposedCommitMessage;
using gc::git::GitIdentity;
using gc::git::RepoDetection;
using gc::git::WorkspaceLoadStatus;
using gc::git::WorkspaceModel;
using gc::test::GitFixture;
using gc::test::GitRun;

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  GC_REQUIRE_MESSAGE(fixture.Prepare(reason), reason);
}

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

std::wstring ToWide(std::string_view text) { return gc::platform::Utf8ToUtf16(std::string(text)); }

// 真实工作区快照：条目全部来自生产读取路径（一条只读 git status），不是手搭的桩。
WorkspaceModel LoadModel(GitFixture& fixture) {
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const gc::git::WorkspaceSnapshot snapshot =
      gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
  GC_REQUIRE_MESSAGE(snapshot.status == WorkspaceLoadStatus::loaded,
                     "夹具读取工作区状态失败：" + ToUtf8(snapshot.message));
  return snapshot.model;
}

RepoDetection LoadDetection(GitFixture& fixture) {
  gc::platform::RepoDetectRequest request;
  request.exePath = fixture.GitExe();
  request.directory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  const RepoDetection detection = gc::platform::DetectRepository(request, fixture.MakeDetectDeps());
  GC_REQUIRE_MESSAGE(!detection.root.empty(),
                     "夹具识别仓库失败：" + ToUtf8(detection.message));
  return detection;
}

// 真实身份事实：直接驱动生产那条只读预检编排（rev-parse／write-tree／config --null --get
// 全部交给夹具里的真实 Git），测试不自己拼一份「看起来一样」的桩。
// 这里不检查 queryOk：冲突索引本来就问不出树对象，那正是有的用例要考察的形态。
gc::git::CommitIdentityFacts LoadIdentity(GitFixture& fixture) {
  gc::platform::CommitProbeRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  return gc::platform::CollectCommitPreflight(request, fixture.MakeCommitProbeDepsForTest());
}

// 把提交者身份写进这座临时仓库自己的配置文件。
// 夹具原本只用环境变量固定身份，而生产预检问的是 `git config --get user.name / user.email`
// （用户层已被夹具隔离成临时档案）：补上这一条，测到的才是「仓库里确实配了身份」的真实形态。
void ConfigureCommitterIdentity(GitFixture& fixture) {
  fixture.RunCheckedInRepo({L"config", L"user.name", L"Evernight Test"});
  fixture.RunCheckedInRepo({L"config", L"user.email", L"test@example.invalid"});
}

CommitTimeChoice ChosenTime(std::string_view gitDate, std::wstring_view display) {
  CommitTimeChoice choice;
  choice.gitDate = std::string(gitDate);
  choice.displayText = std::wstring(display);
  choice.wall = gc::git::CivilTime{2023, 9, 14, 10, 13, 20};
  choice.offsetMinutes = 330;
  return choice;
}

// 一个「什么都齐了」的表单：标题 + 带各种坑的描述 + 两位合作者。
CommitFormData SampleForm() {
  CommitFormData form;
  form.subject = L"修复简繁混排与中文路径";
  // 刻意留下：行尾空格、正文中间的连续空行、以 # 开头的一行、缩进行。
  // cleanup=verbatim 之后这些都必须一字不改地进提交对象。
  form.description =
      L"第一段结尾留了两个空格  \n"
      L"\n"
      L"\n"
      L"# 这一行不是注释，是正文\n"
      L"    缩进也算正文\n";
  form.author = L"张三 <zs@example.invalid>";
  form.coauthors = {L"李四 <ls@example.invalid>"};
  return form;
}

// 用生产代码把表单走完整条路：合成 → 写 UTF-8 文件 → 构造方案。
// 返回方案与信息文件路径；*messageFile 由调用方负责回收（模拟界面的所有权交接）。
CommitPlan BuildRealPlan(GitFixture& fixture, const ComposedCommitMessage& composed,
                         std::wstring* messageFile, std::string_view authorDate = "@1700000000 +0530",
                         std::string_view committerDate = "@1700003600 -0700") {
  const gc::platform::CommitMessageFileWrite written =
      gc::platform::WriteCommitMessageFile(composed.message);
  GC_REQUIRE_MESSAGE(written.written, "生产代码写提交信息文件失败：" + ToUtf8(written.failureReason));
  *messageFile = written.path;

  CommitPlanInput input;
  input.model = LoadModel(fixture);
  input.detection = LoadDetection(fixture);
  // 身份与流程痕迹来自同一趟只读预检：方案层的提交者身份、索引内容标识都不再由测试手工给定，
  // 「确认框上写的那一份」因此就是真实 Git 刚回答的那一份。
  input.identity = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(input.identity.queryOk,
                     "夹具预检没能问齐：" + ToUtf8(input.identity.queryFailure));
  input.gitExecutable = fixture.GitExe();
  input.author = GitIdentity{L"张三", L"zs@example.invalid"};
  input.message = composed.message;
  input.messageUtf8Bytes = written.payloadBytes;
  input.messageFilePath = written.path;
  input.authorTime = ChosenTime(authorDate, L"2023-09-14 10:13:20（UTC+05:30）");
  input.committerTime = ChosenTime(committerDate, L"2023-09-14 11:13:20（UTC-07:00）");
  input.timesSynced = false;
  return gc::git::BuildCommitPlan(input);
}

// 造一处「已暂存 + 未暂存 + 未跟踪」共存的工作区，返回已暂存那条的新内容。
void PrepareMixedWorkspace(GitFixture& fixture) {
  fixture.WriteFile(L"staged.txt", "baseline\n");
  fixture.WriteFile(L"unstaged.txt", "baseline\n");
  fixture.WriteFile(L"docs/说明 与 [标记].md", "baseline\n");
  fixture.StageAll();
  fixture.Commit(L"baseline");
  fixture.WriteFile(L"staged.txt", "已暂存的新内容\n第二行\n");
  fixture.WriteFile(L"docs/说明 与 [标记].md", "标题改过了\n");
  fixture.Stage({L"staged.txt", L"docs/说明 与 [标记].md"});
  fixture.WriteFile(L"unstaged.txt", "这行改动不该进这次提交\n");
  fixture.WriteFile(L"brand-new.txt", "未跟踪，同样不该进这次提交\n");
}

GC_TEST(commit_plan_fixture_real_commit_records_identities_times_and_scope) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  PrepareMixedWorkspace(fixture);

  const ComposedCommitMessage composed = gc::git::ComposeCommitMessage(SampleForm());
  GC_REQUIRE(composed.coauthorTrailers == 1, "样例表单应产出一条合作者 trailer");

  std::wstring messageFile;
  const CommitPlan plan = BuildRealPlan(fixture, composed, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked,
                     "生产方案不该拒绝：" + ToUtf8(plan.blockedReason));
  // 命令里只出现文件路径，绝不带仓库路径、-a、--no-verify 之类的扩大范围形态。
  GC_REQUIRE(plan.arguments.size() == 4, "参数形态应是 commit --cleanup=verbatim -F <文件>");
  GC_REQUIRE(plan.arguments[0] == L"commit" && plan.arguments[1] == L"--cleanup=verbatim" &&
                 plan.arguments[2] == L"-F" && plan.arguments[3] == messageFile,
             "参数内容与预期不符");

  const std::wstring beforeSha = fixture.HeadSha();
  const long beforeCount = fixture.CommitCount();
  const GitRun run = fixture.RunWithOverrides(plan.arguments, plan.environmentOverrides);
  gc::platform::RemoveCommitMessageFile(messageFile);
  GC_REQUIRE_MESSAGE(run.Success(),
                     "真实 Git 提交应成功，退出码 " + std::to_string(run.exitCode) +
                         "：" + ToUtf8(run.err));

  const std::wstring commitObject = fixture.HeadCommitObject();
  GC_REQUIRE(!commitObject.empty(), "读不回提交对象");

  // 1) 作者身份 + 作者时间与偏移（对象里就是方案交出去的那一对值）。
  GC_CHECK_MESSAGE(Contains(commitObject, L"author 张三 <zs@example.invalid> 1700000000 +0530"),
                   "author 行不符：" + ToUtf8(commitObject.substr(0, 200)));
  // 2) 提交者时间被单独设成另一个瞬间；提交者身份没有覆盖，仍是夹具环境里的配置身份。
  GC_CHECK_MESSAGE(Contains(commitObject, L"committer Evernight Test <test@example.invalid> 1700003600 -0700"),
                   "committer 行不符");
  GC_CHECK(!Contains(commitObject, L"committer 张三"), "提交者身份不该被表单作者顶替");
  // 3) 信息一字不改：中文、以 # 开头的一行、行尾空格、正文中间连续空行、缩进都保留。
  GC_CHECK(Contains(commitObject, L"修复简繁混排与中文路径"));
  GC_CHECK(Contains(commitObject, L"# 这一行不是注释，是正文"));
  GC_CHECK(Contains(commitObject, L"第一段结尾留了两个空格  \n"), "行尾空格被收拾掉了");
  GC_CHECK(Contains(commitObject, L"两个空格  \n\n\n# 这一行不是注释"),
           "正文中间的连续空行被收拢了");
  GC_CHECK(Contains(commitObject, L"    缩进也算正文"));
  // 4) 合作者 trailer 一条、排在最末。
  GC_CHECK(Contains(commitObject, L"Co-authored-by: 李四 <ls@example.invalid>"));
  GC_CHECK(!Contains(commitObject, L"Co-authored-by: 张三"));

  // 5) 提交范围只限索引里那一份。
  GC_CHECK(fixture.HeadSha() != beforeSha, "HEAD 应当前进到新提交");
  GC_CHECK(fixture.CommitCount() == beforeCount + 1, "只该多出一个提交");
  // 这一条是本步骤的核心证据：提交对象里的树，与确认框上那个「索引内容标识」是同一个对象。
  // 换句话说，用户点头时看到的那份索引内容，就是 Git 真正写进仓库的那一份。
  GC_CHECK_MESSAGE(fixture.RevParseVerified(L"HEAD^{tree}") == plan.identity.indexTreeOid,
                   "实际落地的树与确认时绑定的索引内容标识不是同一份");
  GC_CHECK(fixture.ShowFileAtHead(L"staged.txt") == "已暂存的新内容\n第二行\n",
           "已暂存的修改应进这次提交");
  GC_CHECK(fixture.ShowFileAtHead(L"docs/说明 与 [标记].md") == "标题改过了\n",
           "中文名带方括号的路径应同样进入");
  GC_CHECK(fixture.ShowFileAtHead(L"unstaged.txt") == "baseline\n",
           "未暂存的改动绝不能被顺带提交");
  const WorkspaceModel after = LoadModel(fixture);
  GC_CHECK_MESSAGE(after.unstaged.size() == 2,
                   "提交后未暂存一侧应留下「unstaged.txt 的改动 + 未跟踪文件」，实际 " +
                       std::to_string(after.unstaged.size()));
  GC_CHECK(after.staged.empty(), "已提交的内容应当从索引侧清空");
  // 工作区里的未暂存文件内容分毫未动（不是被提交后又改回去的）。
  // PathInRoot 相对的是夹具临时根，仓库在根下的 repo\ 目录里（InitRepository 的默认名）。
  {
    std::string content;
    {
      std::ifstream stream(
          std::filesystem::path(std::wstring(fixture.PathInRoot(L"repo/unstaged.txt"))),
          std::ios::binary);
      GC_REQUIRE(stream.is_open(), "读不到工作区里的 unstaged.txt");
      content.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }
    GC_CHECK(content == "这行改动不该进这次提交\n");
  }
  // 父提交仍指向原来那一条：这次是普通提交，没有多出第二个父。
  GC_CHECK(fixture.ParentShaOfHead() == beforeSha, "普通提交只应有一个父");
}

GC_TEST(commit_plan_fixture_hook_rejection_creates_no_commit) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  PrepareMixedWorkspace(fixture);

  // 装一个必定失败的 pre-commit hook：本程序从不加 --no-verify，hooks 照常生效，
  // 失败必须由真实退出码如实上报，而不是「窗口打开了」就当成功。
  fixture.WriteFile(L".git/hooks/pre-commit", "#!/bin/sh\necho 'hook refuses' >&2\nexit 7\n");
  fixture.RunCheckedInRepo({L"config", L"core.hooksPath", L".git/hooks"});

  const ComposedCommitMessage composed = gc::git::ComposeCommitMessage(SampleForm());
  std::wstring messageFile;
  const CommitPlan plan = BuildRealPlan(fixture, composed, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked, ToUtf8(plan.blockedReason));
  std::wstring joined;
  for (const std::wstring& argument : plan.arguments) {
    joined += argument + L' ';
  }
  GC_CHECK(!Contains(joined, L"--no-verify"), "命令里绝不出现 --no-verify：hooks 必须照常生效");

  const std::wstring beforeSha = fixture.HeadSha();
  const std::vector<std::wstring> stagedBefore = fixture.StatusPorcelain();
  const GitRun run = fixture.RunWithOverrides(plan.arguments, plan.environmentOverrides);
  gc::platform::RemoveCommitMessageFile(messageFile);

  GC_CHECK_MESSAGE(!run.Success(), "hook 拒绝时 Git 必须给出非 0 退出码");
  GC_CHECK(run.exitCode != 0, "退出码应非 0");
  GC_CHECK(Contains(run.err, L"hook refuses") || Contains(run.err, L"pre-commit"),
           "Git 的原始报错应留在输出里：" + ToUtf8(run.err));
  GC_CHECK(fixture.HeadSha() == beforeSha, "hook 拒绝后 HEAD 不该前进");
  GC_CHECK(fixture.HeadSubject() == L"baseline", "最近一次提交应还是原来那一条");
  // 索引现场完整保留：用户可以直接改好表单再点一次（界面那侧「表单保留」依赖的正是这个）。
  GC_CHECK(fixture.StatusPorcelain() == stagedBefore, "暂存内容必须分毫未动");
  {
    const WorkspaceModel after = LoadModel(fixture);
    GC_CHECK(after.staged.size() == 2, "提交失败后索引里仍是原来那两条");
  }
}

GC_TEST(commit_plan_fixture_refuses_real_unresolved_merge_conflict) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"both.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"base");

  // 造一个真实的冲突合并现场：分支两侧改同一行，merge 失败留下 MERGE_HEAD 与 U 条目。
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"-b", L"feature"});
  fixture.WriteFile(L"both.txt", "feature 这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"feature 改动");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"main"});
  fixture.WriteFile(L"both.txt", "main 这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"main 改动");
  const GitRun merge = fixture.RunInRepo({L"merge", L"--no-edit", L"feature"});
  GC_REQUIRE(!merge.Success(), "这次合并应当留下冲突");

  const RepoDetection detection = LoadDetection(fixture);
  const ComposedCommitMessage composed = gc::git::ComposeCommitMessage(SampleForm());

  // 真实冲突现场里 write-tree 以退出码 128 失败：这一趟预检问不出「索引内容是哪一份」，
  // 于是方案层在预检这一步就拒绝——连确认框需要的内容标识都没有，谈不上给出命令。
  // 「流程没走完」那条拒绝理由本身由纯逻辑用例（commit_plan_tests）覆盖，这里核的是真实 Git
  // 确实会让预检失败、以及失败后一条参数都不产出。
  CommitPlanInput input;
  input.model = LoadModel(fixture);
  input.detection = detection;
  input.identity = LoadIdentity(fixture);
  input.gitExecutable = fixture.GitExe();
  GC_CHECK_MESSAGE(!input.identity.queryOk, "带未解决冲突的索引不该问得出树对象");
  GC_CHECK_MESSAGE(Contains(input.identity.queryFailure, L"write-tree"),
           "预检失败原因应指明是哪一条查询：" + ToUtf8(input.identity.queryFailure));
  // 流程痕迹独立于树对象：探测按 Git 目录里的 MERGE_HEAD 判定，不受 write-tree 失败影响。
  GC_CHECK_MESSAGE(input.identity.workflowProbed, "预检应在自己刚问到的 Git 目录上探流程痕迹");
  GC_CHECK_MESSAGE(input.identity.workflow.mergeInProgress, "Git 目录里的 MERGE_HEAD 应当被探测到");
  GC_CHECK(Contains(input.identity.workflow.SpecialFlowText(), L"MERGE_HEAD"));
  // 未合并条目也必须在工作区模型里看得见（拒绝文案的第二重依据）。
  GC_CHECK_MESSAGE(input.model.HasConflicts(), "冲突条目应出现在工作区模型里");

  input.author = GitIdentity{L"张三", L"zs@example.invalid"};
  input.message = composed.message;
  input.messageUtf8Bytes = composed.message.size();
  // 冲突场景不需要真的写出信息文件：拒绝必须发生在命令与文件之前，这里给一个合法路径即可。
  input.messageFilePath = L"C:\\Temp\\gc-msg-sample.txt";
  input.authorTime = ChosenTime("@1700000000 +0530", L"2023-09-14 10:13:20（UTC+05:30）");
  input.committerTime = ChosenTime("@1700000000 +0530", L"2023-09-14 10:13:20（UTC+05:30）");
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK_MESSAGE(plan.blocked, "合并没走完 + 有冲突时不该交出一条命令");
  GC_CHECK_MESSAGE(Contains(plan.blockedReason, L"write-tree"),
           "拒绝里要说清是哪一条事实问不出来：" + ToUtf8(plan.blockedReason));
  GC_CHECK_MESSAGE(plan.arguments.empty(), "拒绝时不产生任何参数");
  GC_CHECK_MESSAGE(plan.environmentOverrides.empty(), "拒绝时不产生任何环境覆盖");
}

GC_TEST(commit_plan_fixture_index_lock_is_detected) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  const RepoDetection detection = LoadDetection(fixture);
  GC_REQUIRE(!detection.absoluteGitDir.empty(), "识别结果里应有绝对 Git 目录");

  auto probe = [&detection]() {
    return gc::platform::ProbeRepositoryWorkflowState(detection.absoluteGitDir);
  };
  GC_CHECK(!probe().indexLocked, "干净仓库不该报 index.lock");
  GC_CHECK(!probe().HasSpecialFlowInProgress());

  const std::wstring lockPath = gc::platform::ToAbsolutePath(detection.absoluteGitDir + L"\\index.lock");
  {
    std::ofstream stream(std::filesystem::path(lockPath), std::ios::binary);
    GC_REQUIRE(stream.is_open(), "测试无法创建 index.lock");
  }
  GC_CHECK(probe().indexLocked, "存在的 index.lock 应被探测到");
  // 探测本身是只读的：不许把别人的锁删掉（锁归 Git 自己管）。
  GC_CHECK(std::filesystem::exists(std::filesystem::path(lockPath)), "探测不得删除仓库里的文件");
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(lockPath), ec);
  GC_CHECK(!probe().indexLocked);

  // 空 Git 目录、不存在的目录：一律「没有痕迹」，绝不凭空拒绝一次合法操作。
  GC_CHECK(!gc::platform::ProbeRepositoryWorkflowState(L"").HasSpecialFlowInProgress());
  GC_CHECK(!gc::platform::ProbeRepositoryWorkflowState(L"R:\\一定不存在\\x").indexLocked);
}

// 真实 Git 回答出来的身份事实：确认框要绑的每一项都能逐字核对，且一条提交都没产生。
// 本用例只跑 init / add / write-tree / 只读查询，不执行 commit。
GC_TEST(commit_identity_fixture_reads_the_facts_a_commit_plan_has_to_bind) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"src/a.cpp", "第一版内容\n");
  fixture.WriteFile(L"docs/说明.md", "说明\n");
  fixture.StageAll();

  const gc::git::CommitIdentityFacts facts = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(facts.queryOk, "预检失败：" + ToUtf8(facts.queryFailure));

  // 工作区根与 Git 目录：与另一条生产读取路径（仓库识别）各问一次，两处必须落在同一个地方。
  // 比的是折叠后的同一路径：Git 用正斜杠回这行，识别那侧走的是 Windows 绝对路径，
  // 而界面与复核实际使用的判据也正是这条折叠相等（方案层拿它筛「换仓库」）。
  GC_CHECK(gc::git::PathsEqualFolded(facts.repositoryDirectory, fixture.RepoDir()));
  GC_CHECK_MESSAGE(
      gc::git::PathsEqualFolded(facts.absoluteGitDir, LoadDetection(fixture).absoluteGitDir),
      "预检问到的 Git 目录与仓库识别不一致：" + ToUtf8(facts.absoluteGitDir));

  // 完整分支引用而不是短名：夹具显式把默认分支写死成 main，不靠本机 init.defaultBranch。
  GC_CHECK(facts.onBranch);
  GC_CHECK(facts.branchRef == L"refs/heads/main");
  GC_CHECK(facts.branchName == L"main");

  // 「尚无提交」是 Git 的明确回答（退出码 1、无输出），不是查询失败：整份事实照样可用。
  GC_CHECK(!facts.headResolved);
  GC_CHECK(facts.headObjectId.empty());

  // 索引内容标识必须是完整的树对象 ID（复核比对靠逐字符相等，短 ID 会误判）。
  GC_CHECK(facts.indexTreeResolved);
  GC_CHECK_MESSAGE(facts.indexTreeOid.size() == 40,
                   "write-tree 应当回一个完整的对象 ID：" + ToUtf8(facts.indexTreeOid));

  // 提交者身份走的是 `git config --get user.name / user.email`：这里读回配置文件里那一份，
  // 而不是夹具环境变量里另设的一份——两者内容一致，但取用来源由这条查询决定。
  GC_CHECK(facts.committer == gc::git::CommitterIdentityState::available);
  GC_CHECK(facts.committerIdentityText == L"Evernight Test <test@example.invalid>");

  // 流程痕迹探在本趟刚问到的 Git 目录上。
  GC_CHECK(facts.workflowProbed);
  GC_CHECK(!facts.workflow.HasSpecialFlowInProgress());
  GC_CHECK(!facts.workflow.indexLocked);

  // 预检除了 write-tree 写出的那棵树以外不产生任何仓库状态变化：仍然没有提交、没有锁。
  GC_CHECK_MESSAGE(fixture.CommitCount() == -1, "只读预检不该造出提交");
  GC_CHECK_MESSAGE(fixture.HeadSha().empty(), "HEAD 应当仍然解析不出来");
  GC_CHECK(gc::git::PathsEqualFolded(LoadIdentity(fixture).branchRef, L"refs/heads/main"));
}

// 「索引内容标识」真正的敏感方向：工作区里改内容不算，暂存才算——而且条目数可以一点没变。
// 同样只跑 init / add / write-tree，不执行 commit。
GC_TEST(commit_identity_fixture_index_tree_moves_only_when_the_index_moves) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"a.txt", "第一版\n");
  fixture.StageAll();

  const gc::git::CommitIdentityFacts staged = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(staged.queryOk, "预检失败：" + ToUtf8(staged.queryFailure));
  GC_REQUIRE(LoadModel(fixture).staged.size() == 1, "索引里应当只有一条 a.txt");

  // 只改工作区、不 git add：索引一字未动，内容标识必须原地不动。
  // 「数条目」在这种时刻完全看不出差别，而这正是本次改动把它换成树 ID 的理由。
  fixture.WriteFile(L"a.txt", "改了但没暂存\n");
  const gc::git::CommitIdentityFacts worktree_only = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(worktree_only.queryOk, "预检失败：" + ToUtf8(worktree_only.queryFailure));
  GC_CHECK_MESSAGE(worktree_only.indexTreeOid == staged.indexTreeOid,
           "未暂存的工作区改动不该改变索引内容标识");
  GC_CHECK(worktree_only.headObjectId.empty());

  // 现在把它暂存：条目数还是 1，内容换了 → 标识必须换。
  fixture.Stage({L"a.txt"});
  const gc::git::CommitIdentityFacts restaged = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(restaged.queryOk, "预检失败：" + ToUtf8(restaged.queryFailure));
  GC_CHECK(LoadModel(fixture).staged.size() == 1);
  GC_CHECK_MESSAGE(restaged.indexTreeOid != staged.indexTreeOid,
                   "同样一条暂存条目换了内容，索引内容标识却一模一样");

  // 这个 ID 不是随手算的摘要：Git 自己承认它是一座树对象，就写在这座仓库的对象库里
  // （这正是确认框上交代过的那唯一副作用）。
  GC_CHECK_MESSAGE(fixture.RevParseVerified(restaged.indexTreeOid + L"^{tree}") == restaged.indexTreeOid,
           "write-tree 回的内容在这个仓库里不是一个树对象");
}

// 走一遍「确认框点头时」的生产路径，取出被确认的那一份身份。
// 信息文件当场回收：这一轮只是把方案读出来当比对基准，并不真的提交。
// 注意：这些用例为了造出可提交的现场会调用夹具自己的 Commit，因此属于「会真跑 git commit」
// 的落地测试，只在夹具自建的临时仓库里执行，绝不接触开发仓库或任何远端。
gc::git::CommitPlanIdentity ConfirmedPlanIdentity(GitFixture& fixture) {
  const ComposedCommitMessage composed = gc::git::ComposeCommitMessage(SampleForm());
  std::wstring messageFile;
  const CommitPlan plan = BuildRealPlan(fixture, composed, &messageFile);
  GC_REQUIRE_MESSAGE(!plan.blocked, "生产方案不该拒绝：" + ToUtf8(plan.blockedReason));
  gc::platform::RemoveCommitMessageFile(messageFile);
  return plan.identity;
}

GC_TEST(commit_identity_fixture_voids_confirmed_plan_on_branch_switch_at_same_head) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  PrepareMixedWorkspace(fixture);

  const gc::git::CommitPlanIdentity confirmed = ConfirmedPlanIdentity(fixture);

  // 切到另一条分支，而 HEAD 一个字符都没动：这种变化只有完整分支引用看得出来。
  fixture.RunCheckedInRepo({L"branch", L"topic"});
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"topic"});

  const gc::git::CommitIdentityFacts current = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(current.queryOk, "预检失败：" + ToUtf8(current.queryFailure));
  GC_CHECK_MESSAGE(current.headObjectId == confirmed.headObjectId, "HEAD 本该原地不动");
  const std::wstring text = gc::git::DescribeCommitIdentityChange(confirmed, current);
  GC_CHECK(Contains(text, L"refs/heads/main"));
  GC_CHECK(Contains(text, L"refs/heads/topic"));
  GC_CHECK(Contains(text, L"就算 HEAD 还是同一份提交"));
  // 作废说明要交代清楚后果：命令没发、文件删了、表单留着、要重新确认。
  GC_CHECK(Contains(text, L"这条提交命令没有发出"));
  GC_CHECK(Contains(text, L"表单里的标题、描述、作者与合作者一个字都没动"));
}

GC_TEST(commit_identity_fixture_voids_confirmed_plan_when_staged_content_swaps) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  PrepareMixedWorkspace(fixture);

  const gc::git::CommitPlanIdentity confirmed = ConfirmedPlanIdentity(fixture);

  // 外部工具把已暂存那条换成另一份内容：条目数一点没变（还是 2 条），只有树会暴露。
  fixture.WriteFile(L"staged.txt", "换了一份完全不同的内容\n");
  fixture.Stage({L"staged.txt"});

  const WorkspaceModel now = LoadModel(fixture);
  GC_REQUIRE_MESSAGE(now.staged.size() == 2,
                    "现场应当还是两条暂存条目，实际 " + std::to_string(now.staged.size()));
  const gc::git::CommitIdentityFacts current = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(current.queryOk, "预检失败：" + ToUtf8(current.queryFailure));
  GC_CHECK(current.indexTreeOid != confirmed.indexTreeOid);

  const std::wstring text = gc::git::DescribeCommitIdentityChange(confirmed, current);
  GC_CHECK(Contains(text, L"索引内容从树"));
  GC_CHECK(Contains(text, L"条目数可以一点没变"));
  GC_CHECK(Contains(text, L"这条提交命令没有发出"));
  // 分支与 HEAD 都没动：作废理由里不该混进没发生过的变化。
  GC_CHECK_MESSAGE(!Contains(text, L"HEAD 从"), "不该把没发生的变化也说进去：" + ToUtf8(text));
}

GC_TEST(commit_identity_fixture_voids_confirmed_plan_after_a_merge_starts) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository();
  ConfigureCommitterIdentity(fixture);
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"base");

  // 另一条分支只改 feature.txt：与本仓库里待提交的改动不相干，合并能真正开始。
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"-b", L"feature"});
  fixture.WriteFile(L"feature.txt", "feature 这一侧\n");
  fixture.StageAll();
  fixture.Commit(L"feature 改动");
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"main"});
  fixture.WriteFile(L"base.txt", "本地准备提交的改动\n");
  fixture.Stage({L"base.txt"});

  const gc::git::CommitPlanIdentity confirmed = ConfirmedPlanIdentity(fixture);

  // 真实 Git 先在这里挡了一道：索引与 HEAD 不一致时根本不让合并开始
  // （报错 "Your local changes to the following files would be overwritten by merge"，
  // 而且列出的正是合并压根没碰的那个文件）。这条拒绝值得单独钉住——它说明「确认之后
  // 被人起了合并」在现场上必然连带索引本身也被人动过，复核两样都看得见。
  const GitRun refused = fixture.RunInRepo({L"merge", L"--no-commit", L"--no-ff", L"feature"});
  GC_CHECK_MESSAGE(!refused.Success(), "带着已暂存的内容时 Git 本该拒绝起合并");
  GC_CHECK_MESSAGE(Contains(refused.err, L"local changes"),
                   "拒绝理由应当是本地未提交改动：" + ToUtf8(refused.err));

  // 外部工具把这份暂存先收起来（stash），索引回到 HEAD，合并这才真的开始并停在半途。
  fixture.RunCheckedInRepo({L"stash", L"push", L"--include-untracked", L"-m", L"外部工具收起了暂存"});

  // --no-commit：合并开始但不自己收尾，MERGE_HEAD 留在 Git 目录里。
  const GitRun merge = fixture.RunInRepo({L"merge", L"--no-commit", L"--no-ff", L"feature"});
  GC_REQUIRE(merge.Success(), "合并应当顺利开始（无冲突）：" + ToUtf8(merge.err));
  const gc::git::CommitIdentityFacts current = LoadIdentity(fixture);
  GC_REQUIRE_MESSAGE(current.queryOk, "预检失败：" + ToUtf8(current.queryFailure));
  GC_REQUIRE(current.workflow.mergeInProgress, "真实的 MERGE_HEAD 应当被预检探测到");

  const std::wstring text = gc::git::DescribeCommitIdentityChange(confirmed, current);
  GC_CHECK(Contains(text, L"还没走完的 Git 流程"));
  GC_CHECK(Contains(text, L"MERGE_HEAD"));
  GC_CHECK(Contains(text, L"这条提交命令没有发出"));
}

GC_TEST(message_file_fixture_writes_utf8_and_is_removed_afterwards) {
  GitFixture fixture;
  PrepareFixture(fixture);  // 只借夹具的用例隔离与临时根守卫，本用例不起 Git 进程。

  // 正文自己已经以换行结尾：写出的是原样字节，不再补第二个换行。
  const std::wstring message = L"标题\n\n中文正文，含 emoji 😀 与引号 \" ’ 、反斜杠 \\\n";
  const gc::platform::CommitMessageFileWrite first =
      gc::platform::WriteCommitMessageFile(message);
  GC_REQUIRE_MESSAGE(first.written, ToUtf8(first.failureReason));
  GC_CHECK(!first.path.empty());
  std::string bytes;
  {
    std::ifstream stream(std::filesystem::path(first.path), std::ios::binary);
    GC_REQUIRE(stream.is_open(), "信息文件写出来了却读不回来");
    bytes.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  }  // 先关流再删文件：删不掉的不是实现，而是这个用例自己还占着句柄。
  GC_CHECK(bytes == ToUtf8(message), "正文必须一字不差：不改行尾、不修剪、不多补换行");
  GC_CHECK(first.payloadBytes == bytes.size(), "回报的字节数应就是实际写出的长度");
  // UTF-8 形态单独核对（不借实现里的同一条转换当预期）：中文三字节、emoji 四字节。
  GC_CHECK(bytes.find("\xE4\xB8\xAD\xE6\x96\x87") != std::string::npos, "中文应按 UTF-8 落盘");
  GC_CHECK(bytes.find("\xF0\x9F\x98\x80") != std::string::npos, "增补平面字符（emoji）应按四字节 UTF-8 落盘");

  // 结尾没有换行的正文：补且只补一个换行，让它是一份正常文本。
  const gc::platform::CommitMessageFileWrite appended =
      gc::platform::WriteCommitMessageFile(L"末尾没有换行");
  GC_REQUIRE(appended.written, ToUtf8(appended.failureReason));
  std::string appendedBytes;
  {
    std::ifstream stream(std::filesystem::path(appended.path), std::ios::binary);
    GC_REQUIRE(stream.is_open(), "读不回补换行的信息文件");
    appendedBytes.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  }
  GC_CHECK(appendedBytes == ToUtf8(L"末尾没有换行") + "\n", "应只补一个结尾换行");

  // 第二次写出绝不覆盖第一份：Git 可能还在读它，两份必须各自独立。
  GC_CHECK(appended.path != first.path, "两份信息文件不能同名");
  GC_CHECK(std::filesystem::exists(std::filesystem::path(first.path)), "第一份不该被覆盖掉");

  // 空信息一律拒绝写出：没有正文的可执行命令不该被发出去。
  const gc::platform::CommitMessageFileWrite empty = gc::platform::WriteCommitMessageFile(L"");
  GC_CHECK(!empty.written);
  GC_CHECK(empty.path.empty());
  GC_CHECK(!empty.failureReason.empty());

  gc::platform::RemoveCommitMessageFile(first.path);
  gc::platform::RemoveCommitMessageFile(appended.path);
  GC_CHECK(!std::filesystem::exists(std::filesystem::path(first.path)), "用完要删掉");
  GC_CHECK(!std::filesystem::exists(std::filesystem::path(appended.path)));
  // 空路径与不存在的删除都必须是「什么都不发生」，不抛也不报错。
  gc::platform::RemoveCommitMessageFile(L"");
  gc::platform::RemoveCommitMessageFile(L"R:\\一定不存在\\gc-msg.txt");
}

}  // namespace
