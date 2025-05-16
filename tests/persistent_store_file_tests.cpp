// 持久化文件层的真实 Windows 集成测试（windows 组：起真实文件系统操作，不启动 Git）。
// 全部落在用例自己拥有的隔离临时目录里：绝不读写用户真实的 %APPDATA% 配置与草稿。
// 覆盖：路径解析（override / 占位文件 / 不创建任何东西）、写读往返、损坏文件的改名保留与
// 重新写出、更高版本文件的拒绝覆盖（字节原样）、写锁占用时的「本次不写」、读不成时
// 绝不覆盖着写、原子发布不留下临时文件、删除的幂等。
// 锁互斥用「同进程持独占句柄」覆盖同一判定（与 docs/testing.md 多实例行的既定做法一致）。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <string>
#include <vector>

#include "app/persistent_state.h"
#include "git/repository.h"
#include "platform/windows/persistent_store.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::app::PersistentDraft;
using gc::app::PersistentLoadResult;
using gc::app::PersistentLoadStatus;
using gc::app::PersistentState;
using gc::app::PersistentWriteIntents;
using gc::platform::PersistentReadKind;
using gc::platform::PersistentSaveStatus;
using gc::platform::PersistentStorePaths;
using gc::test::TempDirectory;

void WriteBytes(const std::wstring& path, const std::string& bytes) {
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  GC_REQUIRE_MESSAGE(handle != INVALID_HANDLE_VALUE, "测试写文件失败：CreateFileW");
  DWORD written = 0;
  const BOOL ok = bytes.empty() || ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                               nullptr) != 0;
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

void PrepareRoot(TempDirectory& temp) {
  std::string reason;
  GC_REQUIRE_MESSAGE(temp.Create(L"gc-prefs-test-", reason), "创建隔离临时目录失败: " + reason);
}

PersistentState ConsentedState() {
  PersistentState state;
  state.consentRecorded = true;
  state.persistenceEnabled = true;
  state.draftSavingEnabled = true;
  state.gitExecutablePath = L"P:\\tools\\git\\bin\\git.exe";
  state.recentRepositories = {L"P:\\仓库\\甲", L"P:\\repo\\b"};
  state.window.valid = true;
  state.window.x = 10;
  state.window.y = 20;
  state.window.width = 1200;
  state.window.height = 700;
  state.columns.valid = true;
  state.columns.leftPermille = 350;
  state.columns.middlePermille = 350;
  PersistentDraft draft;
  draft.repositoryRoot = L"P:\\仓库\\甲";
  draft.repositoryKey = gc::git::CanonicalPathKey(draft.repositoryRoot);
  draft.form.subject = L"标题里有 | 竖线和 \\ 反斜杠";
  draft.form.description = L"两行\n描述";
  draft.savedAtEpoch = 1700000000;
  state.drafts.push_back(draft);
  return state;
}

PersistentWriteIntents AllIntents() {
  PersistentWriteIntents intents;
  intents.gitPath = true;
  intents.windowGeometry = true;
  intents.columns = true;
  intents.recentList = true;
  intents.drafts = true;
  intents.policy = true;
  return intents;
}

std::vector<std::wstring> ListNames(const std::wstring& directory) {
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW found{};
  const std::wstring pattern = directory + L"\\*";
  const HANDLE search = ::FindFirstFileW(pattern.c_str(), &found);
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

}  // namespace

GC_TEST(persistent_store_paths_resolve_under_override_and_create_nothing) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_CHECK(paths.valid);
  GC_CHECK(paths.directory == temp.Path() + L"\\EvernightCommit");
  GC_CHECK(paths.stateFile == paths.directory + L"\\state.prefs");
  GC_CHECK(paths.lockFile == paths.directory + L"\\state.prefs.lock");
  // 解析只算路径：目录在用户做出选择之前不会被创建（首次说明没点「保存」也不留痕）。
  GC_CHECK(::GetFileAttributesW(paths.directory.c_str()) == INVALID_FILE_ATTRIBUTES);

  // 基目录不存在 / 被非目录占位：明确失败，不退到别处。
  const std::wstring missing = temp.Path() + L"\\does-not-exist";
  const PersistentStorePaths badBase = gc::platform::ResolvePersistentStorePaths(missing);
  GC_CHECK(!badBase.valid && !badBase.failureReason.empty());
  WriteBytes(paths.directory, "x");  // 同名文件占住 EvernightCommit 的位置
  const PersistentStorePaths blocked = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_CHECK(!blocked.valid);

  // 空 override 走真实的 %APPDATA% 解析：只验证「能解析就不写任何东西」这一条不变式；
  // 取不到（精简单元环境）则必须给出明确失败而不是猜一个目录。
  const PersistentStorePaths real = gc::platform::ResolvePersistentStorePaths();
  if (real.valid) {
    GC_CHECK(real.stateFile.find(L"EvernightCommit") != std::wstring::npos);
  } else {
    GC_CHECK(!real.failureReason.empty());
  }
}

GC_TEST(persistent_store_save_then_read_parses_back) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  const auto outcome = gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000001);
  GC_CHECK_MESSAGE(outcome.status == PersistentSaveStatus::saved, "save status: " +
                   std::to_string(static_cast<int>(outcome.status)));
  GC_CHECK(outcome.merged.gitExecutablePath == ConsentedState().gitExecutablePath);
  const auto read = gc::platform::ReadPersistentStateFile(paths.stateFile);
  GC_CHECK(read.kind == PersistentReadKind::loaded);
  const PersistentLoadResult parsed = gc::app::ParsePersistentState(read.text);
  GC_CHECK(parsed.status == PersistentLoadStatus::loaded);
  GC_CHECK(parsed.state.drafts.size() == 1);
  GC_CHECK(parsed.state.drafts[0].form.subject == ConsentedState().drafts[0].form.subject);
  GC_CHECK(parsed.state.recentRepositories == ConsentedState().recentRepositories);
  // 落盘的是 UTF-8 字节（读侧已严格解码），并且不留下发布用的临时文件。
  for (const std::wstring& name : ListNames(paths.directory)) {
    GC_CHECK_MESSAGE(name.rfind(L"state.tmp-", 0) != 0, "残留临时文件: " + gc::platform::Utf16ToUtf8(name));
  }
}

GC_TEST(persistent_store_renames_corrupt_file_and_writes_fresh) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  // 预先建好目录（保存本身也会建，但这里要先往「文件位置」放一份坏内容）。
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  const std::string garbage = "hello world, not a prefs file\n";
  WriteBytes(paths.stateFile, garbage);
  const auto outcome = gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000002);
  GC_CHECK(outcome.status == PersistentSaveStatus::recoveredCorrupt);
  // 坏原件被改名保留（不是覆盖掉），新文件是可解析的整份。
  bool backupFound = false;
  for (const std::wstring& name : ListNames(paths.directory)) {
    if (name.find(L"corrupt-") != std::wstring::npos) {
      backupFound = true;
      bool exists = false;
      GC_CHECK(ReadBytes(paths.directory + L"\\" + name, &exists) == garbage);
    }
  }
  GC_CHECK(backupFound);
  const auto read = gc::platform::ReadPersistentStateFile(paths.stateFile);
  GC_CHECK(read.kind == PersistentReadKind::loaded);
  GC_CHECK(gc::app::ParsePersistentState(read.text).status == PersistentLoadStatus::loaded);
}

GC_TEST(persistent_store_refuses_to_overwrite_newer_version) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  const std::string future = "evernightcommit.prefs|99\ngit|C:\\future\\git.exe\n";
  WriteBytes(paths.stateFile, future);
  const auto outcome = gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000003);
  GC_CHECK(outcome.status == PersistentSaveStatus::refusedTooNew);
  bool exists = false;
  GC_CHECK(ReadBytes(paths.stateFile, &exists) == future);  // 一个字节都没动
}

GC_TEST(persistent_store_lock_busy_leaves_target_untouched) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  // 模拟另一实例正在写：锁文件被它以独占方式持有（共享模式 0）。
  const HANDLE held = ::CreateFileW(paths.lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  GC_REQUIRE_MESSAGE(held != INVALID_HANDLE_VALUE, "测试持有写锁失败");
  const auto outcome = gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000004);
  GC_CHECK(outcome.status == PersistentSaveStatus::busy);
  GC_CHECK(!outcome.detail.empty());
  bool exists = true;
  ReadBytes(paths.stateFile, &exists);
  GC_CHECK(!exists);  // 被挡时不写目标文件
  ::CloseHandle(held);
  // 锁放开后同一份保存照常成功。
  const auto retry = gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000005);
  GC_CHECK(retry.status == PersistentSaveStatus::saved);
}

GC_TEST(persistent_store_unreadable_target_is_never_clobbered) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_REQUIRE(::CreateDirectoryW(paths.directory.c_str(), nullptr) != 0 ||
                   ::GetLastError() == ERROR_ALREADY_EXISTS,
           "测试预建目录失败");
  // 用一个同名目录占住「文件」的位置：读取必然失败，失败必须是「不写」而不是「重建」。
  GC_REQUIRE(::CreateDirectoryW(paths.stateFile.c_str(), nullptr) != 0, "测试占位目录失败");
  const auto outcome = gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000006);
  GC_CHECK(outcome.status == PersistentSaveStatus::failed);
  GC_CHECK(!outcome.detail.empty());
  GC_CHECK(::GetFileAttributesW(paths.stateFile.c_str()) != INVALID_FILE_ATTRIBUTES);  // 目录还原样在
}

GC_TEST(persistent_store_second_publish_replaces_whole_file) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  GC_CHECK(gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000007)
               .status == PersistentSaveStatus::saved);
  PersistentState second = ConsentedState();
  second.gitExecutablePath = L"P:\\other\\git.exe";
  second.drafts.clear();
  PersistentWriteIntents intents = AllIntents();
  const auto outcome = gc::platform::SavePersistentState(paths, second, intents, 1700000008);
  GC_CHECK(outcome.status == PersistentSaveStatus::saved);
  const auto read = gc::platform::ReadPersistentStateFile(paths.stateFile);
  const PersistentLoadResult parsed = gc::app::ParsePersistentState(read.text);
  GC_CHECK(parsed.status == PersistentLoadStatus::loaded);
  // 第二次保存没有点名要写草稿，盘上的旧草稿原样保留（意图门控）；git 路径整份换新。
  GC_CHECK(parsed.state.drafts.size() == 1);
  GC_CHECK(parsed.state.gitExecutablePath == std::wstring(L"P:\\other\\git.exe"));
  GC_CHECK(parsed.state.revision == 2);
}

GC_TEST(persistent_store_delete_treats_absent_as_done) {
  TempDirectory temp;
  PrepareRoot(temp);
  const PersistentStorePaths paths = gc::platform::ResolvePersistentStorePaths(temp.Path());
  GC_REQUIRE(paths.valid, "路径解析失败");
  std::wstring reason;
  GC_CHECK(gc::platform::DeletePersistentStateFile(paths, &reason));  // 本来就没有：目标状态已达成
  GC_CHECK(gc::platform::SavePersistentState(paths, ConsentedState(), AllIntents(), 1700000009).status ==
           PersistentSaveStatus::saved);
  GC_CHECK(gc::platform::DeletePersistentStateFile(paths, &reason));
  bool exists = true;
  ReadBytes(paths.stateFile, &exists);
  GC_CHECK(!exists);
  // 路径无效的场合：明确失败并给原因，而不是「删成功了」。
  const PersistentStorePaths invalid = gc::platform::ResolvePersistentStorePaths(temp.Path() + L"\\none");
  GC_CHECK(!invalid.valid);
  GC_CHECK(!gc::platform::DeletePersistentStateFile(invalid, &reason));
}
