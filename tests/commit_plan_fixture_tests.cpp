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
//   * 合并流程没走完时（真实冲突现场），方案层直接拒绝，不发任何命令；
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
  input.workflow = gc::platform::ProbeRepositoryWorkflowState(input.detection.absoluteGitDir);
  input.committer = CommitterIdentityState::available;
  input.committerIdentityText = L"Evernight Test <test@example.invalid>";
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
  const auto workflow = gc::platform::ProbeRepositoryWorkflowState(detection.absoluteGitDir);
  GC_CHECK(workflow.mergeInProgress, "Git 目录里的 MERGE_HEAD 应当被探测到");
  GC_CHECK(Contains(workflow.SpecialFlowText(), L"MERGE_HEAD"));

  const ComposedCommitMessage composed = gc::git::ComposeCommitMessage(SampleForm());
  CommitPlanInput input;
  input.model = LoadModel(fixture);
  input.detection = detection;
  input.workflow = workflow;
  input.committer = CommitterIdentityState::available;
  input.committerIdentityText = L"Evernight Test <test@example.invalid>";
  input.author = GitIdentity{L"张三", L"zs@example.invalid"};
  input.message = composed.message;
  input.messageUtf8Bytes = composed.message.size();
  // 冲突场景不需要真的写出信息文件：拒绝必须发生在命令与文件之前，这里给一个合法路径即可。
  input.messageFilePath = L"C:\\Temp\\gc-msg-sample.txt";
  input.authorTime = ChosenTime("@1700000000 +0530", L"2023-09-14 10:13:20（UTC+05:30）");
  input.committerTime = ChosenTime("@1700000000 +0530", L"2023-09-14 10:13:20（UTC+05:30）");
  const CommitPlan plan = gc::git::BuildCommitPlan(input);
  GC_CHECK(plan.blocked, "合并没走完 + 有冲突时不该交出一条命令");
  // 先报「流程没走完」：那种状态下连冲突都还不算解决完，终止流程是用户自己的事。
  GC_CHECK(Contains(plan.blockedReason, L"Git 流程还没走完"));
  GC_CHECK(plan.arguments.empty(), "拒绝时不产生任何参数");
  GC_CHECK(plan.environmentOverrides.empty(), "拒绝时不产生任何环境覆盖");
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
