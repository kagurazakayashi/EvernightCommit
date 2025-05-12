// 「父仓库 ⇄ 子模块」导航的纯逻辑测试：全部用桩化的 GitQueryResult 与手工搭出的事实驱动生产逻辑，
// 不起真实 Git、不碰文件系统、不接触网络。覆盖：
//   * 三条只读查询的参数形态：-C 绑定父根、--no-optional-locks、`ls-files -s -z -- :(literal)<路径>`、
//     `rev-parse --verify --quiet HEAD:<路径>`、以及子模块自己 HEAD 那条与 pull 预检同形态；
//     路径不符合仓库相对路径约定时一条也不构造（宁可不问，也不带着会被重新解释的路径去问）；
//   * 父索引记录的判读：160000 才算 gitlink；空输出是「明确没有这条记录」；缺结尾 NUL、
//     记录形态不对、对象 ID 不是完整 40/64 位、答的是别的路径、进程没起来——全都不算「干净」；
//   * 三份位置的合成：一致/已暂存/未暂存/问不出子模块位置/那条记录已不是 gitlink/问不全，
//     六种结论各有各的措辞，问不全时绝不说「干净」；
//   * 提示文字的边界：该由谁做下一步（用户点按钮，不是程序代办）、每次都重申「没有改父索引、
//     没有提交、没有联网」，以及「父仓库显示干净不证明子模块那次提交已发布」；
//   * 进入的裁决逐条拒绝：不是子模块条目、路径不合约定、目录不存在、是文件不是目录、
//     索引里没有/不是 gitlink、没跑识别、识别失败、裸仓库或 .git 内部、独立仓库、
//     父仓库对不上（被移动过）、以及「Git 往上找到父仓库本身」这种还没初始化的样子；
//   * 允许进入时带上的交代：指针归父仓库、内部已跟踪改动与未跟踪文件归子模块自己，
//     以及这一步只换绑定、不 add/commit/push/init。
// 真实 Git 的落地（真子模块、真 gitlink）在 submodule_navigation_fixture_tests.cpp 验证。
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/pull_plan.h"
#include "git/repository.h"
#include "git/submodule_navigation.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::GitQueryResult;
using gc::git::GitlinkIndexEntry;
using gc::git::RepoDetection;
using gc::git::RepoError;
using gc::git::RepoKind;
using gc::git::SubmoduleEntryDecision;
using gc::git::SubmoduleEntryFacts;
using gc::git::SubmoduleEntryPlan;
using gc::git::SubmodulePathPresence;
using gc::git::SubmodulePointerFacts;
using gc::git::SubmodulePointerQueries;
using gc::git::SubmodulePointerState;
using gc::git::SubmoduleState;

constexpr std::wstring_view kRoot = L"D:\\父 仓库";
constexpr std::wstring_view kChild = L"child";
constexpr std::wstring_view kChildDir = L"D:\\父 仓库\\child";
constexpr std::wstring_view kOldOid = L"1111111111111111111111111111111111111111";
constexpr std::wstring_view kNewOid = L"2222222222222222222222222222222222222222";
constexpr std::wstring_view kOtherOid = L"3333333333333333333333333333333333333333";

GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
  return result;
}

GitQueryResult LaunchFailed() {
  GitQueryResult result;
  result.started = false;
  return result;
}

std::wstring NulJoined(std::initializer_list<std::wstring_view> records) {
  std::wstring joined;
  for (const std::wstring_view record : records) {
    joined += record;
    joined.push_back(L'\0');
  }
  return joined;
}

std::wstring GitlinkRecord(std::wstring_view mode, std::wstring_view oid, std::wstring_view path) {
  return std::wstring(mode) + L" " + std::wstring(oid) + L" 0\t" + std::wstring(path);
}

bool TextContains(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

bool HasArgument(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  return std::find(arguments.begin(), arguments.end(), std::wstring(value)) != arguments.end();
}

std::string Narrow(std::wstring_view text) {
  std::string out;
  for (const wchar_t value : text) {
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
    } else {
      char buffer[16]{};
      std::snprintf(buffer, sizeof(buffer), "<U+%04X>", static_cast<unsigned>(value));
      out += buffer;
    }
  }
  return out;
}

// 一份「目录在、是 gitlink、Git 认它是当前父仓库登记的子模块」的桩事实。
SubmoduleEntryFacts HealthyEntryFacts() {
  SubmoduleEntryFacts facts;
  facts.itemIsSubmodule = true;
  facts.relativePath = kChild;
  facts.pathShapeOk = true;
  facts.resolvedDirectory = kChildDir;
  facts.presence = SubmodulePathPresence::existsDirectory;
  facts.parentRoot = kRoot;
  facts.index.readOk = true;
  facts.index.recordFound = true;
  facts.index.isGitlink = true;
  facts.index.mode = L"160000";
  facts.index.objectId = kOldOid;
  facts.index.recordedPath = kChild;
  facts.childProbed = true;
  facts.child.kind = RepoKind::submodule;
  facts.child.submodule = true;
  facts.child.root = kChildDir;
  facts.child.superprojectTree = kRoot;
  facts.child.branch = L"main";
  facts.child.message = L"已识别仓库：子模块仓库；工作区根目录：D:\\父 仓库\\child";
  return facts;
}

GitlinkIndexEntry GitlinkOf(std::wstring_view oid) {
  GitlinkIndexEntry entry;
  entry.readOk = true;
  entry.recordFound = true;
  entry.isGitlink = true;
  entry.mode = L"160000";
  entry.objectId = std::wstring(oid);
  entry.recordedPath = kChild;
  return entry;
}

SubmodulePointerFacts PointerFor(std::wstring_view indexOid, std::wstring_view headOid,
                                 std::wstring_view submoduleHeadOid) {
  SubmodulePointerQueries queries;
  queries.relativePath = kChild;
  queries.indexListing = Answer(0, NulJoined({GitlinkRecord(L"160000", indexOid, kChild)}));
  queries.headPointer = headOid.empty() ? Answer(1) : Answer(0, std::wstring(headOid) + L"\n");
  queries.submoduleHead =
      submoduleHeadOid.empty() ? Answer(1) : Answer(0, std::wstring(submoduleHeadOid) + L"\n");
  return gc::git::InterpretSubmodulePointer(queries);
}

// ---- 查询参数形态 ----

GC_TEST(submodule_index_pointer_arguments_bind_root_and_use_literal_pathspec) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildSubmoduleIndexPointerArguments(kRoot, kChild);
  GC_CHECK_MESSAGE(!arguments.empty(), "参数没构造出来");
  GC_CHECK(arguments.front() == L"-C" && arguments[1] == kRoot);
  GC_CHECK(HasArgument(arguments, L"--no-optional-locks"));
  GC_CHECK(HasArgument(arguments, L"ls-files"));
  // -z：不带它时 Git 会把含空格/非 ASCII 的路径按 C 引号转写，那种回答不是原样路径，
  // 拿它比对会把一个合法子模块说成「路径对不上」。
  GC_CHECK(HasArgument(arguments, L"-s"));
  GC_CHECK(HasArgument(arguments, L"-z"));
  GC_CHECK(HasArgument(arguments, L"--"));
  GC_CHECK(arguments.back() == std::wstring(L":(literal)") + std::wstring(kChild));
  // 路径是资料：仓库相对路径约定不过就不构造（宁可不问，也不带着会被重新解释的写法去问）。
  GC_CHECK(gc::git::BuildSubmoduleIndexPointerArguments(kRoot, L"../outside").empty());
  GC_CHECK(gc::git::BuildSubmoduleIndexPointerArguments(kRoot, L"/abs").empty());
  GC_CHECK(gc::git::BuildSubmoduleIndexPointerArguments(kRoot, L"D:\\x").empty());
  GC_CHECK(gc::git::BuildSubmoduleIndexPointerArguments(kRoot, L"a:b").empty());
  GC_CHECK(gc::git::BuildSubmoduleIndexPointerArguments(kRoot, {}).empty());
  GC_CHECK(gc::git::BuildSubmoduleIndexPointerArguments({}, kChild).empty());
  // 中文与空格路径照常构造：路径本身不是命令。
  GC_CHECK(!gc::git::BuildSubmoduleIndexPointerArguments(kRoot, L"第 1 层/子 模块").empty());
}

GC_TEST(submodule_head_pointer_and_own_head_argument_shapes) {
  const std::vector<std::wstring> head = gc::git::BuildSubmoduleHeadPointerArguments(kRoot, kChild);
  GC_CHECK(HasArgument(head, L"rev-parse"));
  GC_CHECK(HasArgument(head, L"--verify"));
  GC_CHECK(HasArgument(head, L"--quiet"));
  GC_CHECK(HasArgument(head, L"--no-replace-objects"));
  GC_CHECK(head.back() == std::wstring(L"HEAD:") + std::wstring(kChild));
  GC_CHECK(gc::git::BuildSubmoduleHeadPointerArguments(kRoot, L"../x").empty());

  // 子模块自己 HEAD 的那条与 pull/push 预检问 HEAD 完全同一个形态（一份实现，两处不各写一套）。
  const std::vector<std::wstring> own = gc::git::BuildSubmoduleOwnHeadArguments(kChildDir);
  GC_CHECK(own == gc::git::BuildPullHeadObjectArguments(kChildDir));
  GC_CHECK(gc::git::BuildSubmoduleOwnHeadArguments({}).empty());
}

// ---- 父索引记录的判读 ----

GC_TEST(gitlink_index_entry_reads_the_160000_record) {
  const GitQueryResult listing =
      Answer(0, NulJoined({GitlinkRecord(L"160000", kOldOid, kChild)}));
  const GitlinkIndexEntry entry = gc::git::ParseGitlinkIndexEntry(listing, kChild);
  GC_CHECK_MESSAGE(entry.readOk && entry.recordFound && entry.isGitlink,
                   Narrow(entry.readFailure));
  GC_CHECK(entry.objectId == kOldOid);
  GC_CHECK(entry.recordedPath == kChild);
}

GC_TEST(gitlink_index_entry_empty_output_is_explicit_absence) {
  const GitlinkIndexEntry entry = gc::git::ParseGitlinkIndexEntry(Answer(0), kChild);
  GC_CHECK(entry.readOk);
  GC_CHECK(!entry.recordFound && !entry.isGitlink);
}

GC_TEST(gitlink_index_entry_keeps_wrong_mode_for_the_refusal_text) {
  const GitQueryResult listing =
      Answer(0, NulJoined({GitlinkRecord(L"100644", kOldOid, kChild)}));
  const GitlinkIndexEntry entry = gc::git::ParseGitlinkIndexEntry(listing, kChild);
  GC_CHECK(entry.readOk && entry.recordFound);
  GC_CHECK(!entry.isGitlink);
  GC_CHECK(entry.mode == L"100644");
  GC_CHECK(entry.objectId.empty());
}

GC_TEST(gitlink_index_entry_refuses_incomplete_and_malformed_answers) {
  // 缺结尾 NUL：半条记录看起来也像一条记录，半个对象 ID 会被当成「索引记的就是这一份」。
  const GitQueryResult truncated =
      Answer(0, std::wstring(L"160000 ") + std::wstring(kOldOid) + L" 0\tchild");
  const GitlinkIndexEntry truncation = gc::git::ParseGitlinkIndexEntry(truncated, kChild);
  GC_CHECK(!truncation.readOk);
  GC_CHECK(TextContains(truncation.readFailure, L"NUL"));

  const GitlinkIndexEntry noTab =
      gc::git::ParseGitlinkIndexEntry(Answer(0, NulJoined({L"160000-bogus"})), kChild);
  GC_CHECK(!noTab.readOk);
  GC_CHECK(TextContains(noTab.readFailure, L"形态"));

  // 短 ID 不能当位置用：它可能被别人解释成另一个对象。
  const GitlinkIndexEntry shortOid = gc::git::ParseGitlinkIndexEntry(
      Answer(0, NulJoined({GitlinkRecord(L"160000", L"abc123", kChild)})), kChild);
  GC_CHECK(!shortOid.readOk && !shortOid.isGitlink);
  GC_CHECK(TextContains(shortOid.readFailure, L"完整对象 ID"));

  // 问的是 child，答的是 child/inside.txt：不是同一条记录，采认它就是把另一个路径当这个子模块。
  const GitlinkIndexEntry otherPath = gc::git::ParseGitlinkIndexEntry(
      Answer(0, NulJoined({GitlinkRecord(L"160000", kOldOid, L"child/inside.txt")})), kChild);
  GC_CHECK(otherPath.readOk && !otherPath.recordFound);

  const GitlinkIndexEntry notStarted =
      gc::git::ParseGitlinkIndexEntry(LaunchFailed(), kChild);
  GC_CHECK(!notStarted.readOk && !notStarted.readFailure.empty());
  // --quiet 那套的「退出码 1 + 空输出」在 ls-files 这里不算明确答案：它没答这条查询。
  const GitlinkIndexEntry refused = gc::git::ParseGitlinkIndexEntry(Answer(1), kChild);
  GC_CHECK(!refused.readOk);
}

// ---- 三份位置的合成 ----

GC_TEST(submodule_pointer_states_from_three_positions) {
  const SubmodulePointerFacts clean = PointerFor(kOldOid, kOldOid, kOldOid);
  GC_CHECK_MESSAGE(clean.state == SubmodulePointerState::consistentClean,
                   Narrow(gc::git::SubmodulePointerStateLabel(clean.state)));
  GC_CHECK(clean.headObjectId == kOldOid && clean.submoduleHeadId == kOldOid);

  const SubmodulePointerFacts staged = PointerFor(kNewOid, kOldOid, kNewOid);
  GC_CHECK(staged.state == SubmodulePointerState::pointerStaged);

  const SubmodulePointerFacts unstaged = PointerFor(kOldOid, kOldOid, kNewOid);
  GC_CHECK(unstaged.state == SubmodulePointerState::pointerUnstaged);

  // 新登记的 gitlink：父提交里还没有这条记录，也算「已暂存等提交」，不是「没问出来」。
  const SubmodulePointerFacts fresh = PointerFor(kNewOid, {}, kNewOid);
  GC_CHECK(fresh.state == SubmodulePointerState::pointerStaged);
  GC_CHECK(TextContains(fresh.detail, L"HEAD 树里还没有这条 gitlink"));
}

GC_TEST(submodule_pointer_says_unknown_instead_of_clean) {
  // 子模块那边 HEAD 问不出（尚无提交/引用坏了）：不能说父仓库这一条干净。
  const SubmodulePointerFacts headless = PointerFor(kOldOid, kOldOid, {});
  GC_CHECK(headless.state == SubmodulePointerState::submoduleHeadUnknown);
  GC_CHECK(TextContains(gc::git::ComposeSubmodulePointerReport(headless, kChild),
                        L"不猜它的位置，也不说父仓库这一条「干净」"));

  // 父索引那条记录已经不是 gitlink（路径被换掉/移走）。
  SubmodulePointerQueries notGitlink;
  notGitlink.relativePath = kChild;
  notGitlink.indexListing =
      Answer(0, NulJoined({GitlinkRecord(L"100644", kOldOid, kChild)}));
  notGitlink.headPointer = Answer(0, std::wstring(kOldOid) + L"\n");
  notGitlink.submoduleHead = Answer(0, std::wstring(kNewOid) + L"\n");
  const SubmodulePointerFacts replaced = gc::git::InterpretSubmodulePointer(notGitlink);
  GC_CHECK(replaced.state == SubmodulePointerState::notGitlinkInIndex);
  GC_CHECK(TextContains(replaced.detail, L"100644"));

  // 索引查询压根没起来：位置没问全，什么都不能说。
  SubmodulePointerQueries unanswered;
  unanswered.relativePath = kChild;
  unanswered.indexListing = LaunchFailed();
  const SubmodulePointerFacts unknown = gc::git::InterpretSubmodulePointer(unanswered);
  GC_CHECK(unknown.state == SubmodulePointerState::unknown);
  GC_CHECK(TextContains(gc::git::ComposeSubmodulePointerReport(unknown, kChild),
                        L"不说「该暂存」也不说「已经干净」"));
}

GC_TEST(submodule_pointer_report_prompts_the_user_not_the_program) {
  const std::wstring unstaged =
      gc::git::ComposeSubmodulePointerReport(PointerFor(kOldOid, kOldOid, kNewOid), kChild);
  GC_CHECK(TextContains(unstaged, L"加入暂存区"));
  GC_CHECK(TextContains(unstaged, L"本程序不会替你在导航里 add／commit／push"));
  GC_CHECK(TextContains(unstaged, L"递归处理别的子模块"));
  GC_CHECK(TextContains(unstaged, L"本次导航本身：没有改动父仓库索引、没有产生提交、没有访问远端"));

  const std::wstring staged =
      gc::git::ComposeSubmodulePointerReport(PointerFor(kNewOid, kOldOid, kNewOid), kChild);
  GC_CHECK(TextContains(staged, L"下一步是在父仓库"));

  // 「干净」那句必须同时带上「不等于已发布」：这是父仓库显示给不了的信息。
  const std::wstring clean =
      gc::git::ComposeSubmodulePointerReport(PointerFor(kOldOid, kOldOid, kOldOid), kChild);
  GC_CHECK(TextContains(clean, L"显示干净"));
  GC_CHECK(TextContains(clean, L"子模块那次提交已经推到任何远端"));
}

// ---- 进入的裁决 ----

GC_TEST(submodule_entry_allows_only_a_verified_child_of_this_repository) {
  const SubmoduleEntryPlan plan = gc::git::BuildSubmoduleEntryPlan(HealthyEntryFacts());
  GC_CHECK_MESSAGE(plan.decision == SubmoduleEntryDecision::enter, Narrow(plan.reason));
  GC_CHECK(plan.targetDirectory == kChildDir);
  GC_CHECK(TextContains(plan.reason, L"--show-superproject-working-tree") ||
           TextContains(plan.reason, L"父仓库"));
  GC_CHECK(TextContains(plan.reason, L"不改父仓库索引、不 add、不 commit、不 push、不联网"));
  // 指针、内部已跟踪改动、未跟踪文件三件事必须分开交代。
  SubmoduleEntryFacts dirty = HealthyEntryFacts();
  dirty.flags.commitChanged = true;
  dirty.flags.trackedChanges = true;
  dirty.flags.untrackedChanges = true;
  const SubmoduleEntryPlan dirtyPlan = gc::git::BuildSubmoduleEntryPlan(dirty);
  GC_CHECK(dirtyPlan.decision == SubmoduleEntryDecision::enter);
  GC_CHECK(TextContains(dirtyPlan.disclosure, L"已经变了"));
  GC_CHECK(TextContains(dirtyPlan.disclosure, L"已跟踪文件的改动"));
  GC_CHECK(TextContains(dirtyPlan.disclosure, L"未跟踪文件"));
  GC_CHECK(TextContains(dirtyPlan.disclosure, L"两个不同的操作"));
  GC_CHECK(TextContains(dirtyPlan.disclosure, L"不证明子模块那次提交已经发布"));

  SubmoduleEntryFacts tidy = HealthyEntryFacts();
  tidy.flags.commitChanged = false;
  const SubmoduleEntryPlan tidyPlan = gc::git::BuildSubmoduleEntryPlan(tidy);
  GC_CHECK(TextContains(tidyPlan.disclosure, L"此刻与子模块 HEAD 一致"));
}

GC_TEST(submodule_entry_refuses_uninitialized_by_either_answer) {
  // 情形一：Git 从那个目录往上找到的是父仓库本身（子模块目录里没有 .git 时就是这样）。
  SubmoduleEntryFacts walksUp = HealthyEntryFacts();
  walksUp.child.kind = RepoKind::plainWorktree;
  walksUp.child.submodule = false;
  walksUp.child.superprojectTree.clear();
  walksUp.child.root = kRoot;  // Git 答出的工作区根就是父仓库
  const SubmoduleEntryPlan walkPlan = gc::git::BuildSubmoduleEntryPlan(walksUp);
  GC_CHECK(walkPlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(TextContains(walkPlan.reason, L"还没有初始化"));
  GC_CHECK(TextContains(walkPlan.reason, L"git submodule update --init"));
  GC_CHECK(TextContains(walkPlan.reason, L"初始化会联网并改动工作区"));

  // 情形二：Git 明确说这不是仓库。
  SubmoduleEntryFacts notRepo = HealthyEntryFacts();
  notRepo.child.kind = RepoKind::notRepository;
  notRepo.child.submodule = false;
  notRepo.child.root.clear();
  notRepo.child.message = L"这不是 Git 仓库。";
  const SubmoduleEntryPlan notRepoPlan = gc::git::BuildSubmoduleEntryPlan(notRepo);
  GC_CHECK(notRepoPlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(TextContains(notRepoPlan.reason, L"还没有初始化"));
  GC_CHECK(TextContains(notRepoPlan.reason, L"不在导航里替你做"));
}

GC_TEST(submodule_entry_refuses_gone_and_non_directory_paths) {
  SubmoduleEntryFacts gone = HealthyEntryFacts();
  gone.presence = SubmodulePathPresence::missing;
  gone.childProbed = false;
  const SubmoduleEntryPlan gonePlan = gc::git::BuildSubmoduleEntryPlan(gone);
  GC_CHECK(gonePlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(TextContains(gonePlan.reason, L"那个目录已经不在了"));
  GC_CHECK(TextContains(gonePlan.reason, L"也不会替你 clone"));

  SubmoduleEntryFacts file = HealthyEntryFacts();
  file.presence = SubmodulePathPresence::existsNotDirectory;
  file.childProbed = false;
  const SubmoduleEntryPlan filePlan = gc::git::BuildSubmoduleEntryPlan(file);
  GC_CHECK(TextContains(filePlan.reason, L"不是子模块工作区目录"));

  // 探测压根没做成：不能把「不知道」当成任何一种结论。
  SubmoduleEntryFacts unprobed = HealthyEntryFacts();
  unprobed.presence = SubmodulePathPresence::unknown;
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(unprobed).reason, L"没能确认那个目录在不在"));
}

GC_TEST(submodule_entry_refuses_when_index_says_otherwise) {
  // 界面显示是子模块、索引里却没这条记录（刚被移出暂存区/改名）：事实已经不是同一件事。
  SubmoduleEntryFacts missingRecord = HealthyEntryFacts();
  missingRecord.index = GitlinkIndexEntry{};
  missingRecord.index.readOk = true;
  const SubmoduleEntryPlan missingPlan = gc::git::BuildSubmoduleEntryPlan(missingRecord);
  GC_CHECK(missingPlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(TextContains(missingPlan.reason, L"索引里已经没有"));

  SubmoduleEntryFacts wrongMode = HealthyEntryFacts();
  wrongMode.index = GitlinkIndexEntry{};
  wrongMode.index.readOk = true;
  wrongMode.index.recordFound = true;
  wrongMode.index.mode = L"120000";
  const SubmoduleEntryPlan wrongPlan = gc::git::BuildSubmoduleEntryPlan(wrongMode);
  GC_CHECK(TextContains(wrongPlan.reason, L"120000"));
  GC_CHECK(TextContains(wrongPlan.reason, L"不是 160000 gitlink"));

  // 索引查询没做成时也不进入（即使目录看起来一切正常）。
  SubmoduleEntryFacts unanswered = HealthyEntryFacts();
  unanswered.index.readOk = false;
  unanswered.index.readFailure = L"git ls-files -s 未成功";
  unanswered.presence = SubmodulePathPresence::missing;
  const SubmoduleEntryPlan unansweredPlan = gc::git::BuildSubmoduleEntryPlan(unanswered);
  GC_CHECK(unansweredPlan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(TextContains(unansweredPlan.reason, L"没能问清父仓库索引"));
}

GC_TEST(submodule_entry_refuses_foreign_or_unreadable_repositories) {
  // 一个独立仓库（没有父仓库来历）不是「从这个仓库进去的那个子模块」。
  SubmoduleEntryFacts foreign = HealthyEntryFacts();
  foreign.child.kind = RepoKind::linkedWorktree;
  foreign.child.submodule = false;
  foreign.child.linked = true;
  foreign.child.superprojectTree.clear();
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(foreign).reason,
                        L"不是由父仓库登记的子模块"));

  // 是子模块，但父仓库是另一个目录（被移动过、或另一份克隆）。
  SubmoduleEntryFacts moved = HealthyEntryFacts();
  moved.child.superprojectTree = L"D:\\另外的\\父仓库";
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(moved).reason, L"不是同一个父仓库"));

  SubmoduleEntryFacts bare = HealthyEntryFacts();
  bare.child.kind = RepoKind::bare;
  bare.child.root.clear();
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(bare).reason, L"不能当工作区用"));

  SubmoduleEntryFacts failed = HealthyEntryFacts();
  failed.child.kind = RepoKind::failed;
  failed.child.error = RepoError::dubiousOwnership;
  failed.child.message = L"Git 的目录所有权检查拦下了这个目录。";
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(failed).reason, L"仓库身份没能问出来"));

  // 识别这一步压根没跑：不能说它是谁的子模块。
  SubmoduleEntryFacts notProbed = HealthyEntryFacts();
  notProbed.childProbed = false;
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(notProbed).reason, L"没能对那个目录跑一次仓库识别"));
}

GC_TEST(submodule_entry_requires_an_actual_submodule_item) {
  SubmoduleEntryFacts notSubmodule = HealthyEntryFacts();
  notSubmodule.itemIsSubmodule = false;
  const SubmoduleEntryPlan plan = gc::git::BuildSubmoduleEntryPlan(notSubmodule);
  GC_CHECK(plan.decision == SubmoduleEntryDecision::refuse);
  GC_CHECK(TextContains(plan.reason, L"不是子模块条目"));

  SubmoduleEntryFacts noPath = HealthyEntryFacts();
  noPath.relativePath.clear();
  GC_CHECK(gc::git::BuildSubmoduleEntryPlan(noPath).decision == SubmoduleEntryDecision::refuse);

  SubmoduleEntryFacts badShape = HealthyEntryFacts();
  badShape.pathShapeOk = false;
  badShape.pathShapeProblem = L"条目路径含 ..，会指向仓库之外";
  const SubmoduleEntryPlan badPlan = gc::git::BuildSubmoduleEntryPlan(badShape);
  GC_CHECK(TextContains(badPlan.reason, L"不符合仓库相对路径的约定"));
  GC_CHECK(TextContains(badPlan.reason, L"拒绝进入任何仓库"));

  SubmoduleEntryFacts noDirectory = HealthyEntryFacts();
  noDirectory.resolvedDirectory.clear();
  GC_CHECK(TextContains(gc::git::BuildSubmoduleEntryPlan(noDirectory).reason, L"没有可拼接的落点") ||
           TextContains(gc::git::BuildSubmoduleEntryPlan(noDirectory).reason, L"安全拼成"));
}

GC_TEST(submodule_entry_refusal_names_the_entry_it_refused) {
  SubmoduleEntryFacts facts = HealthyEntryFacts();
  facts.presence = SubmodulePathPresence::missing;
  facts.index = GitlinkIndexEntry{};  // 连索引记录也没问成
  const SubmoduleEntryPlan plan = gc::git::BuildSubmoduleEntryPlan(facts);
  GC_CHECK(TextContains(plan.reason, L"条目：child"));
}

GC_TEST(journey_child_root_compare_never_guesses) {
  GC_CHECK(gc::git::IsJourneyChildRoot(L"D:\\父\\child", L"d:/父/child"));
  GC_CHECK(!gc::git::IsJourneyChildRoot(L"D:\\父\\child", L"D:\\父\\other"));
  // 任何一边不知道都算「不是那个子模块」：不能拿空字符串当相等。
  GC_CHECK(!gc::git::IsJourneyChildRoot(L"", L"D:\\父\\child"));
  GC_CHECK(!gc::git::IsJourneyChildRoot(L"D:\\父\\child", L""));
}

}  // namespace
