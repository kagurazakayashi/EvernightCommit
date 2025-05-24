// 操作历史文件层与剪贴板的真实 Windows 集成测试（windows 组：起真实文件系统 / Win32 调用，
// 不启动 Git）。全部落在用例自己拥有的隔离临时目录里：绝不读写用户真实的 %APPDATA%。
// 覆盖：override 路径解析且不创建任何东西、写读往返不留临时文件、损坏原件改名保留后重写、
// 更高版本一个字节不动、另一实例持锁时「本次不写」、崩溃残留的半条记录被改名保留绝不当完成、
// 删除幂等，以及剪贴板往返。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "app/operation_history.h"
#include "git/repository.h"
#include "platform/windows/clipboard.h"
#include "platform/windows/operation_history_store.h"

namespace {

using gc::app::HistoryWriteIntents;
using gc::app::OperationLog;
using gc::app::OperationRecord;
using gc::platform::HistoryReadKind;
using gc::platform::HistorySaveStatus;
using gc::platform::HistoryStorePaths;
using gc::test::TempDirectory;

std::wstring FullOid(wchar_t filler) { return std::wstring(40, filler); }

OperationLog SingleRecordHistory() {
  OperationLog log;
  log.historyEnabled = true;
  OperationRecord record;
  record.id = L"op-file-1";
  record.startedEpoch = 1700000000;
  record.terminalEpoch = 1700000010;
  record.flow = gc::app::HistoryFlow::undo;
  record.workTreeRoot = L"P:\\仓库\\proj";
  record.repositoryKey = gc::git::CanonicalPathKey(record.workTreeRoot);
  record.operationLabel = L"撤回最近提交";
  record.restoreKind = gc::app::HistoryRestoreKind::refMove;
  record.restoreBranchRef = L"refs/heads/main";
  record.restoreUndoToObjectId = FullOid(L'c');
  record.restoreExpectedCurrentId = FullOid(L'b');
  record.terminal = gc::app::HistoryTerminal::succeeded;
  std::wstring refusal;
  gc::app::AppendHistoryRecord(&log, record, &refusal);
  return log;
}

HistoryWriteIntents AppendIntents() {
  HistoryWriteIntents intents;
  intents.policy = true;
  intents.append = true;
  return intents;
}

void WriteBytes(const std::wstring& path, const std::string& bytes) {
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  GC_REQUIRE_MESSAGE(handle != INVALID_HANDLE_VALUE, "测试写文件失败：CreateFileW");
  DWORD written = 0;
  const BOOL ok = bytes.empty() || ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                                               &written, nullptr) != 0;
  GC_REQUIRE_MESSAGE(ok && written == static_cast<DWORD>(bytes.size()), "测试写文件失败：WriteFile");
  ::CloseHandle(handle);
}

std::string ReadBytes(const std::wstring& path, bool* exists) {
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *exists = false;
    return {};
  }
  std::string bytes;
  for (;;) {
    char buffer[4096];
    DWORD gotten = 0;
    if (::ReadFile(handle, buffer, sizeof(buffer), &gotten, nullptr) == 0 || gotten == 0) {
      break;
    }
    bytes.append(buffer, gotten);
  }
  ::CloseHandle(handle);
  *exists = true;
  return bytes;
}

std::vector<std::wstring> ListNames(const std::wstring& directory) {
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW found{};
  const HANDLE search = ::FindFirstFileW((directory + L"\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) {
    return names;
  }
  do {
    const std::wstring name = found.cFileName;
    if (name != L"." && name != L"..") {
      names.push_back(name);
    }
  } while (::FindNextFileW(search, &found) != 0);
  ::FindClose(search);
  return names;
}

void PrepareRoot(TempDirectory& temp) {
  std::string reason;
  GC_REQUIRE_MESSAGE(temp.Create(L"gc-history-test-", reason), "创建隔离临时目录失败: " + reason);
}

// ---- 恢复链路的生命周期用例共用的小工具 ----
//
// 界面那一路（启动当场落在途记录 → 终态用同一个 ID 更新 → 拿不到终态时补「结果未知」→ 保存落盘）
// 要的是 HWND 和真实命令窗口，单测驱动不到那一步。这里逐步复刻它的**数据动作**，但发布一律走
// 真实的文件层：装配一条记录 → 并进本会话那份日志 → SaveOperationHistory（含重新读盘与多实例合并）
// → 再从盘上读回来断言。用例因此钉住的是「事后查得到什么」，而不是某个函数自己返回了什么。

namespace h = gc::app;

const std::wstring kDemoRoot = L"P:\\HistoryLifecycle\\demo";
const std::wstring kBranchRef = L"refs/heads/main";
const std::wstring kOidBefore(40, L'a');   // 恢复之前那条分支指着的对象
const std::wstring kOidAfter(40, L'b');    // 恢复要把它挪去的那个对象
const std::wstring kInFlightId = L"op-lifecycle-1";
constexpr long long kStartEpoch = 1700000000;

// 恢复控制器交给命令窗口带走的那份「已确认事实」：与 RestoreFlow::Launch 同一个装配点，
// 仓库工作区根按位置传入——漏填那一项正是让整条记录被拒的原缺陷。
h::HistoryCapture RestoreCapture() {
  h::HistoryCapture capture =
      h::MakeWriteHistoryCapture(h::HistoryFlow::restore, kDemoRoot, L"按记录恢复引用");
  capture.sourceRef = kBranchRef;
  capture.sourceObjectId = kOidBefore;
  capture.targetRef = kBranchRef;
  capture.targetObjectId = kOidAfter;
  capture.restoreKind = h::HistoryRestoreKind::refMove;
  capture.restoreBranchRef = kBranchRef;
  capture.restoreExpectedCurrentId = kOidAfter;
  capture.restoreUndoToObjectId = kOidBefore;
  capture.restoreNote = L"如需再反悔，可把它挪回原值（本程序不会自动执行）。";
  return capture;
}

h::HistoryTerminalInfo TerminalAt(h::HistoryOutcome outcome, long long terminalEpoch,
                                  bool exitCodeKnown, long long exitCode, std::wstring_view completion,
                                  std::wstring_view conclusion) {
  h::HistoryTerminalInfo terminal;
  terminal.outcome = outcome;
  terminal.startedEpoch = kStartEpoch;
  terminal.terminalEpoch = terminalEpoch;
  terminal.exitCodeKnown = exitCodeKnown;
  terminal.exitCode = exitCode;
  terminal.completionLabel = std::wstring(completion);
  terminal.conclusion = std::wstring(conclusion);
  return terminal;
}

// MainWindow::MarkHistoryRecordUnknown 的那一份补记：只有 ID、终态与一句原因，没有仓库身份。
h::OperationRecord UnknownNote(long long terminalEpoch, std::wstring_view why) {
  h::OperationRecord note;
  note.id = kInFlightId;
  note.terminal = h::HistoryTerminal::unknown;
  note.terminalEpoch = terminalEpoch;
  note.completionLabel = L"没有收到完成通知";
  note.outcomeNote = std::wstring(why);
  return note;
}

h::OperationLog EnabledLog() {
  h::OperationLog log;
  log.historyEnabled = true;
  return log;
}

// 落账（AppendHistoryRecord 拒绝即用例失败；被拒原因本身另有用例钉住）。
h::OperationLog AppendRow(h::OperationLog log, const h::OperationRecord& record, std::string_view why) {
  std::wstring refusal;
  const bool ok = h::AppendHistoryRecord(&log, record, &refusal);
  GC_REQUIRE_MESSAGE(ok, std::string("落账失败：") + std::string(why));
  GC_CHECK(refusal.empty());  // 收下就不带原因：*refusal 只在被拒时有话
  return log;
}

// 发布到真实文件层：与 MainWindow::RunHistorySave 同一套（重新读盘、按 ID 合并、原子写），
// 返回本会话应当以此为准的那一份，行为与界面里的 historyLog_ = outcome.merged 一致。
h::OperationLog Publish(const h::OperationLog& ours, const HistoryStorePaths& paths, long long nowEpoch,
                        h::HistoryMergeReport* report = nullptr) {
  const auto outcome = gc::platform::SaveOperationHistory(paths, ours, AppendIntents(), nowEpoch);
  GC_REQUIRE_MESSAGE(outcome.status == HistorySaveStatus::saved ||
                             outcome.status == HistorySaveStatus::recoveredCorrupt,
                     "发布状态: " + std::to_string(static_cast<int>(outcome.status)));
  if (report != nullptr) {
    *report = outcome.report;
  }
  return outcome.merged;
}

// 从盘上读回来（文件层 + 整份严格解析），断言的是「这次运行之后能查到什么」。
h::OperationLog ReadBack(const HistoryStorePaths& paths) {
  const auto read = gc::platform::ReadHistoryFile(paths.historyFile);
  GC_REQUIRE_MESSAGE(read.kind == HistoryReadKind::loaded,
                     "读回状态: " + std::to_string(static_cast<int>(read.kind)));
  const auto parsed = h::ParseOperationLog(read.text);
  GC_REQUIRE_MESSAGE(parsed.status == h::HistoryLoadStatus::loaded,
                     "解析状态: " + std::to_string(static_cast<int>(parsed.status)));
  return parsed.log;
}

const h::OperationRecord* FindById(const h::OperationLog& log, std::wstring_view id) {
  for (const h::OperationRecord& record : log.records) {
    if (record.id == id) {
      return &record;
    }
  }
  return nullptr;
}

}  // namespace

GC_TEST(operation_history_store_paths_resolve_under_override_and_create_nothing) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_CHECK(paths.valid);
  GC_CHECK(paths.directory == temp.Path() + L"\\EvernightCommit");
  GC_CHECK(paths.historyFile == paths.directory + L"\\history.prefs");
  GC_CHECK(paths.lockFile == paths.directory + L"\\history.prefs.lock");
  // 解析只算路径：目录在用户开启记录之前不会被创建。
  GC_CHECK(::GetFileAttributesW(paths.directory.c_str()) == INVALID_FILE_ATTRIBUTES);
  const HistoryStorePaths badBase =
      gc::platform::ResolveHistoryStorePaths(temp.Path() + L"\\does-not-exist");
  GC_CHECK(!badBase.valid && !badBase.failureReason.empty());
}

GC_TEST(operation_history_store_save_then_read_parses_back) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const auto outcome =
      gc::platform::SaveOperationHistory(paths, SingleRecordHistory(), AppendIntents(), 1700000100);
  GC_CHECK_MESSAGE(outcome.status == HistorySaveStatus::saved,
                   "save status: " + std::to_string(static_cast<int>(outcome.status)));
  const auto read = gc::platform::ReadHistoryFile(paths.historyFile);
  GC_CHECK(read.kind == HistoryReadKind::loaded);
  const auto parsed = gc::app::ParseOperationLog(read.text);
  GC_CHECK(parsed.status == gc::app::HistoryLoadStatus::loaded);
  GC_REQUIRE(parsed.log.records.size() == 1, "往返应带回一条记录");
  GC_CHECK(parsed.log.records[0].id == L"op-file-1");
  GC_CHECK(parsed.log.records[0].restoreUndoToObjectId == FullOid(L'c'));
  GC_CHECK(parsed.log.historyEnabled);
  // 不留发布用的临时文件。
  for (const std::wstring& name : ListNames(paths.directory)) {
    GC_CHECK_MESSAGE(name.rfind(L"history.tmp-", 0) != 0, "残留临时文件");
  }
}

GC_TEST(operation_history_store_renames_corrupt_and_refuses_half_record) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  // 一份「半条 record」的坏文件：版本头与 meta 成立，但 record 行字段数不对——
  // 崩溃残留绝不能被当成一次已完成的操作，整份按损坏改名保留。
  const std::string half =
      "evernightcommit.history|1\nmeta|1,90,1000\nrecord|opX|100,200|1,2,1,0|root\n";
  WriteBytes(paths.historyFile, half);
  const auto outcome =
      gc::platform::SaveOperationHistory(paths, SingleRecordHistory(), AppendIntents(), 1700000200);
  GC_CHECK(outcome.status == HistorySaveStatus::recoveredCorrupt);
  bool backupFound = false;
  for (const std::wstring& name : ListNames(paths.directory)) {
    if (name.find(L"corrupt-") != std::wstring::npos) {
      backupFound = true;
      bool exists = false;
      GC_CHECK(ReadBytes(paths.directory + L"\\" + name, &exists) == half);
    }
  }
  GC_CHECK(backupFound);
  const auto read = gc::platform::ReadHistoryFile(paths.historyFile);
  GC_CHECK(gc::app::ParseOperationLog(read.text).log.records.size() == 1);
}

GC_TEST(operation_history_store_refuses_newer_version_untouched) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  const std::string future = "evernightcommit.history|99\nmeta|1,90,1000\n";
  WriteBytes(paths.historyFile, future);
  const auto outcome =
      gc::platform::SaveOperationHistory(paths, SingleRecordHistory(), AppendIntents(), 1700000300);
  GC_CHECK(outcome.status == HistorySaveStatus::refusedTooNew);
  bool exists = false;
  GC_CHECK(ReadBytes(paths.historyFile, &exists) == future);  // 一个字节都不动
}

GC_TEST(operation_history_store_lock_busy_leaves_target_untouched) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  const HANDLE held = ::CreateFileW(paths.lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  GC_REQUIRE_MESSAGE(held != INVALID_HANDLE_VALUE, "测试持有写锁失败");
  const auto outcome =
      gc::platform::SaveOperationHistory(paths, SingleRecordHistory(), AppendIntents(), 1700000400);
  GC_CHECK(outcome.status == HistorySaveStatus::busy);
  bool exists = true;
  ReadBytes(paths.historyFile, &exists);
  GC_CHECK(!exists);  // 被挡时不写目标
  ::CloseHandle(held);
  const auto retry =
      gc::platform::SaveOperationHistory(paths, SingleRecordHistory(), AppendIntents(), 1700000401);
  GC_CHECK(retry.status == HistorySaveStatus::saved);
}

GC_TEST(operation_history_store_delete_treats_absent_as_done) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  std::wstring reason;
  GC_CHECK(gc::platform::DeleteHistoryFile(paths, &reason));  // 本来就没有
  GC_CHECK(gc::platform::SaveOperationHistory(paths, SingleRecordHistory(), AppendIntents(), 1700000500)
               .status == HistorySaveStatus::saved);
  GC_CHECK(gc::platform::DeleteHistoryFile(paths, &reason));
  bool exists = true;
  ReadBytes(paths.historyFile, &exists);
  GC_CHECK(!exists);
}

GC_TEST(operation_history_restore_success_settles_the_same_row) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  // 启动当场：一条「已启动，未见结果」。这一行必须当场查得到，且带着仓库归属与恢复线索——
  // 原缺陷正是恢复链路漏填工作区根，规范键为空，整条记录被写盘那一侧拒掉：操作做了，历史没有。
  h::OperationLog window = Publish(
      AppendRow(EnabledLog(),
                h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::inProgress, 0, false, 0, L"执行中",
                                                   L"命令窗口已经启动，本程序还没有拿到它的结果。")),
                "在途记录"),
      paths, kStartEpoch);
  const h::OperationLog afterStart = ReadBack(paths);
  GC_REQUIRE(afterStart.records.size() == 1, "盘上应当有一条在途记录");
  GC_CHECK(afterStart.records[0].terminal == h::HistoryTerminal::inProgress);
  GC_CHECK(afterStart.records[0].repositoryKey == gc::git::CanonicalPathKey(kDemoRoot));
  GC_CHECK(afterStart.records[0].restoreUndoToObjectId == kOidBefore);

  // 终态回来：用同一个 ID 更新那一条，而不是新起一行。
  window = Publish(
      AppendRow(window,
                h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::succeeded, kStartEpoch + 20, true, 0,
                                                   L"退出码 0", L"按记录恢复已完成（只移动了那一个分支引用）。")),
                "成功终态"),
      paths, kStartEpoch + 20);
  const h::OperationLog done = ReadBack(paths);
  GC_REQUIRE_MESSAGE(done.records.size() == 1, "终态更新不得另起一行");
  GC_CHECK(done.records[0].id == kInFlightId);
  GC_CHECK(done.records[0].terminal == h::HistoryTerminal::succeeded);
  GC_CHECK(done.records[0].exitCodeKnown && done.records[0].exitCode == 0);
  GC_CHECK(done.records[0].startedEpoch == kStartEpoch);
  GC_CHECK(done.records[0].terminalEpoch == kStartEpoch + 20);
  GC_CHECK(done.records[0].flow == h::HistoryFlow::restore);
  GC_CHECK(done.records[0].restoreKind == h::HistoryRestoreKind::refMove);
  GC_CHECK(done.records[0].restoreBranchRef == kBranchRef);
  GC_CHECK(done.records[0].restoreExpectedCurrentId == kOidAfter);
  // 「再反悔一次」的入口靠的就是这条记录：恢复线索必须读得回来，而不只是当时在内存里。
  GC_CHECK(!done.records[0].restoreNote.empty());
}

GC_TEST(operation_history_restore_git_failure_is_recorded_as_failure) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  Publish(AppendRow(EnabledLog(),
                    h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                            TerminalAt(h::HistoryOutcome::inProgress, 0, false, 0, L"执行中",
                                                       L"命令窗口已经启动。")),
                    "在途记录"),
          paths, kStartEpoch);
  Publish(AppendRow(ReadBack(paths),
                    h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                            TerminalAt(h::HistoryOutcome::failed, kStartEpoch + 7, true, 1,
                                                       L"退出码 1", L"按记录恢复没有成功：Git 自己报的非 0。")),
                    "失败终态"),
          paths, kStartEpoch + 7);

  const h::OperationLog done = ReadBack(paths);
  GC_REQUIRE(done.records.size() == 1, "失败也只更新那一条在途记录");
  GC_CHECK(done.records[0].terminal == h::HistoryTerminal::failed);
  GC_CHECK_MESSAGE(done.records[0].exitCodeKnown && done.records[0].exitCode == 1,
                   "失败要带 Git 真正给过的退出码");
  GC_CHECK(done.records[0].outcomeNote.find(L"没有成功") != std::wstring::npos);
  // 失败记录照旧保留恢复线索：引用到底被挪到哪儿，只有现场重读知道，但线索不能因此消失。
  GC_CHECK(done.records[0].restoreKind == h::HistoryRestoreKind::refMove);
  // 失败绝不写成「结果未知」，也绝不写成「已撤销」。
  GC_CHECK(h::HistoryTerminalLabel(done.records[0].terminal) !=
           h::HistoryTerminalLabel(h::HistoryTerminal::unknown));
}

GC_TEST(operation_history_restore_launch_failure_never_claimed_ran) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  // 启动失败：命令窗口根本没打开，因此从来没有在途那一条，直接落一条明确的启动失败。
  const h::OperationLog window = Publish(
      AppendRow(EnabledLog(),
                h::ComposeHistoryRecord(L"op-launch-1", capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::launchFailed, kStartEpoch, false, 0,
                                                   L"启动失败", L"命令窗口未能打开，没有运行任何 Git 命令。")),
                "启动失败"),
      paths, kStartEpoch);
  GC_CHECK(window.records.size() == 1);

  const h::OperationLog done = ReadBack(paths);
  GC_REQUIRE(done.records.size() == 1, "启动失败也是一条记录：不能查无此事");
  GC_CHECK(done.records[0].terminal == h::HistoryTerminal::launchFailed);
  GC_CHECK_MESSAGE(!done.records[0].exitCodeKnown, "没有跑起来的命令没有退出码可报");
  GC_CHECK(h::HistoryTerminalLabel(done.records[0].terminal).find(L"未运行任何 Git") !=
           std::wstring::npos);
  GC_CHECK(done.records[0].restoreKind == h::HistoryRestoreKind::refMove);
}

GC_TEST(operation_history_close_before_terminal_shows_result_unknown) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  h::OperationLog window = Publish(
      AppendRow(EnabledLog(),
                h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::inProgress, 0, false, 0, L"执行中",
                                                   L"命令窗口已经启动。")),
                "在途记录"),
      paths, kStartEpoch);

  // 关窗收尾与「完成通知丢失」走的是同一句补记：只带 ID 与终态，没有仓库身份（原缺陷就在这里——
  // 当时要求补记也带齐完整身份，于是这条永远写不进去，在途那一行会一直停在「执行中」）。
  window = Publish(AppendRow(window, UnknownNote(kStartEpoch + 60, L"界面在关闭前没有收到这次操作的完成通知。"),
                             "关窗收尾补记"),
                   paths, kStartEpoch + 60);
  const h::OperationLog closed = ReadBack(paths);
  GC_REQUIRE(closed.records.size() == 1, "补记不新增行");
  GC_CHECK(closed.records[0].terminal == h::HistoryTerminal::unknown);
  GC_CHECK(closed.records[0].terminalEpoch == kStartEpoch + 60);
  GC_CHECK(h::TerminalHasSettled(closed.records[0].terminal));
  GC_CHECK(closed.records[0].outcomeNote.find(L"关闭前") != std::wstring::npos);
  // 「不猜」的部分：既不是成功也不是失败，更不是已撤销；而归属与恢复线索一条都没丢。
  GC_CHECK(!closed.records[0].exitCodeKnown);
  GC_CHECK(closed.records[0].repositoryKey == gc::git::CanonicalPathKey(kDemoRoot));
  GC_CHECK(closed.records[0].restoreUndoToObjectId == kOidBefore);
  GC_CHECK(h::HistoryTerminalLabel(closed.records[0].terminal) == L"结果未知");

  // 之后重新核实拿到了确定的一次观察：未知可以被升级（AppendHistoryRecord 只禁止降级），
  // 但这一次仍然是同一条记录。
  Publish(AppendRow(closed,
                    h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                            TerminalAt(h::HistoryOutcome::succeeded, kStartEpoch + 90, true,
                                                       0, L"退出码 0", L"事后按仓库现状核实：那条引用已经挪过去了。")),
                    "迟到终态"),
          paths, kStartEpoch + 90);
  const h::OperationLog upgraded = ReadBack(paths);
  GC_REQUIRE(upgraded.records.size() == 1, "迟到终态并入同一条");
  GC_CHECK(upgraded.records[0].terminal == h::HistoryTerminal::succeeded);
  GC_CHECK(upgraded.records[0].outcomeNote.find(L"事后按仓库现状核实") != std::wstring::npos);
}

GC_TEST(operation_history_duplicate_notifications_settle_exactly_once) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  h::OperationLog window = Publish(
      AppendRow(EnabledLog(),
                h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::inProgress, 0, false, 0, L"执行中",
                                                   L"命令窗口已经启动。")),
                "在途记录"),
      paths, kStartEpoch);
  window = Publish(
      AppendRow(window,
                h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::succeeded, kStartEpoch + 11, true, 0,
                                                   L"退出码 0", L"第一次送达的终态。")),
                "终态"),
      paths, kStartEpoch + 11);

  // 重复通知／迟到的第二份通知：同一条记录已经结案，后面那一次一个字都改不了。
  window = Publish(
      AppendRow(window,
                h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                        TerminalAt(h::HistoryOutcome::failed, kStartEpoch + 999, true, 128,
                                                   L"退出码 128", L"重复送来的一次终态。")),
                "重复终态"),
      paths, kStartEpoch + 999);
  const h::OperationLog done = ReadBack(paths);
  GC_REQUIRE(done.records.size() == 1, "重复通知不得再生成一行");
  GC_CHECK(done.records[0].terminal == h::HistoryTerminal::succeeded);
  GC_CHECK(done.records[0].terminalEpoch == kStartEpoch + 11);
  GC_CHECK(done.records[0].exitCode == 0);
  GC_CHECK(done.records[0].outcomeNote == L"第一次送达的终态。");

  // 「更弱的一次观察」（含补记结果未知）同样不能把已定的成功挤掉。
  Publish(AppendRow(done, UnknownNote(kStartEpoch + 1000, L"收尾时又补了一次未知。"), "弱观察"),
          paths, kStartEpoch + 1000);
  const h::OperationLog still = ReadBack(paths);
  GC_REQUIRE(still.records.size() == 1, "仍然一条");
  GC_CHECK_MESSAGE(still.records[0].terminal == h::HistoryTerminal::succeeded,
                   "已确定的终态不被更弱的一次观察降级");
}

GC_TEST(operation_history_in_flight_row_survives_a_killed_process) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  // 进程被强杀／断电：盘上留下的就是在途那一条，没有任何收尾会替它改写。
  Publish(AppendRow(EnabledLog(),
                    h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                            TerminalAt(h::HistoryOutcome::inProgress, 0, false, 0, L"执行中",
                                                       L"命令窗口已经启动。")),
                    "在途记录"),
          paths, kStartEpoch);

  // 下一次启动读回来：仍然是「已启动，未见结果」——不是成功、不是失败、也不是「没做过这个操作」。
  const h::OperationLog reopened = ReadBack(paths);
  GC_REQUIRE(reopened.records.size() == 1, "重启后读得回那一条在途记录");
  GC_CHECK(!h::TerminalHasSettled(reopened.records[0].terminal));
  const std::wstring_view label = h::HistoryTerminalLabel(reopened.records[0].terminal);
  GC_CHECK(label.find(L"未见结果") != std::wstring::npos);
  GC_CHECK(label.find(L"不猜成败") != std::wstring::npos);
  GC_CHECK(reopened.records[0].restoreKind == h::HistoryRestoreKind::refMove);

  // 保留策略：在途记录照样按时间参与淘汰，不会因为没有终态就永远赖着不走。
  h::OperationLog aged = reopened;
  gc::app::ApplyHistoryRetention(&aged, kStartEpoch + 400LL * 24 * 60 * 60);
  GC_CHECK_MESSAGE(aged.records.empty(), "超过保留期限的在途记录也要退场，历史不能无限增长");
}

GC_TEST(operation_history_multi_instance_keeps_the_settled_row) {
  TempDirectory temp;
  PrepareRoot(temp);
  const HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const h::HistoryCapture capture = RestoreCapture();

  // 另一个窗口已经看到并写下了那条操作的终态。
  Publish(AppendRow(EnabledLog(),
                    h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                            TerminalAt(h::HistoryOutcome::succeeded, kStartEpoch + 5, true, 0,
                                                       L"退出码 0", L"另一个窗口记下的终态。")),
                    "另一实例的终态"),
          paths, kStartEpoch + 5);

  // 本窗口手上只有同 ID 的一条在途记录（它自己没拿到通知），另外还有一条属于本窗口的操作。
  h::OperationLog ours = AppendRow(EnabledLog(),
                                   h::ComposeHistoryRecord(kInFlightId, capture.workTreeRoot, capture,
                                                            TerminalAt(h::HistoryOutcome::inProgress, 0, false,
                                                                       0, L"执行中", L"本窗口还在等结果。")),
                                   "本窗口的在途");
  ours = AppendRow(ours,
                   h::ComposeHistoryRecord(L"op-lifecycle-2", capture.workTreeRoot, capture,
                                           TerminalAt(h::HistoryOutcome::failed, kStartEpoch + 9, true, 1,
                                                      L"退出码 1", L"本窗口自己那一条操作。")),
                   "本窗口的失败");
  h::HistoryMergeReport report;
  const h::OperationLog merged = Publish(ours, paths, kStartEpoch + 10, &report);

  const h::OperationLog done = ReadBack(paths);
  GC_REQUIRE(done.records.size() == 2, "并集：另一窗口那条 + 本窗口那条");
  const h::OperationRecord* settled = FindById(done, kInFlightId);
  GC_REQUIRE(settled != nullptr, "盘上那条要在");
  GC_CHECK_MESSAGE(settled->terminal == h::HistoryTerminal::succeeded,
                   "本窗口较弱的在途不得盖掉盘上已确定的终态");
  GC_CHECK(settled->outcomeNote == L"另一个窗口记下的终态。");
  GC_REQUIRE(report.keptFromDiskStrongerTerminal.size() == 1, "被留下的那条要报告出来");
  GC_CHECK(report.keptFromDiskStrongerTerminal[0] == kInFlightId);
  GC_CHECK(merged.records.size() == done.records.size());
  GC_CHECK(FindById(done, L"op-lifecycle-2") != nullptr);
}

GC_TEST(clipboard_round_trip_copies_utf16_text) {
  const std::wstring command = L"git update-ref refs/heads/main abcdef0123456789 def";
  std::wstring failure;
  const bool copied = gc::platform::CopyTextToClipboard(nullptr, command, &failure);
  GC_CHECK_MESSAGE(copied, "复制到剪贴板应成功");
  if (!copied) {
    return;
  }
  if (::OpenClipboard(nullptr) == 0) {
    GC_CHECK_MESSAGE(false, "读回时打不开剪贴板");
    return;
  }
  const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
  bool matched = false;
  if (data != nullptr) {
    const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(data));
    if (text != nullptr) {
      matched = std::wstring(text) == command;
      ::GlobalUnlock(data);
    }
  }
  ::CloseClipboard();
  GC_CHECK_MESSAGE(matched, "剪贴板内容应与复制的原文一致");
}
