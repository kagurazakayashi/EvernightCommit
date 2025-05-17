// 操作历史文件层与剪贴板的真实 Windows 集成测试（windows 组：起真实文件系统 / Win32 调用，
// 不启动 Git）。全部落在用例自己拥有的隔离临时目录里：绝不读写用户真实的 %APPDATA%。
// 覆盖：override 路径解析且不创建任何东西、写读往返不留临时文件、损坏原件改名保留后重写、
// 更高版本一个字节不动、另一实例持锁时「本次不写」、崩溃残留的半条记录被改名保留绝不当完成、
// 删除幂等，以及剪贴板往返。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <string>
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
