#include "platform/windows/persistent_store.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "platform/windows/raii.h"
#include "platform/windows/utf_text.h"

namespace gc::platform {
namespace {

std::wstring FormatWindowsError(unsigned long errorCode) {
  LPWSTR buffer = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD written =
      ::FormatMessageW(flags, nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring text;
  if (written != 0 && buffer != nullptr) {
    text.assign(buffer, written);
    ::LocalFree(buffer);
  }
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
    text.pop_back();
  }
  if (text.empty()) {
    text = L"Windows 错误码 " + std::to_wstring(errorCode);
  }
  return text;
}

bool IsDirectoryButNotReparse(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;  // 再分析点不跟随：目录必须是它自己。
  }
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool PathExists(const std::wstring& path) { return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring JoinPath(std::wstring_view directory, std::wstring_view name) {
  std::wstring out(directory);
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
    out.push_back(L'\\');
  }
  out += name;
  return out;
}

// 漫游应用数据目录（%APPDATA%）：SHGetKnownFolderPath 为准，环境变量兜底。
// 两者都给不出时返回空串——调用方据此明确失败，绝不退到别处。
std::wstring RoamingAppDataDirectory() {
  PWSTR known = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &known)) && known != nullptr) {
    std::wstring path(known);
    ::CoTaskMemFree(known);
    if (!path.empty()) {
      return path;
    }
  } else if (known != nullptr) {
    ::CoTaskMemFree(known);
  }
  std::wstring buffer(32768, L'\0');
  const DWORD length = ::GetEnvironmentVariableW(L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  buffer.resize(length);
  return buffer;
}

// 保存用的独占写锁：同一个锁文件以「共享模式 0」被本进程独占打开，本身就是一条句柄租约。
// 另一个实例无论怎么打开（连只读都要共享许可）都会被挡回来，所以互斥与存活性由系统一起保证：
// 持有者进程退出（含崩溃）时句柄被回收，锁立刻可以被下一个实例拿到。
// 锁文件本身常驻目录、不删除——删除动作反而制造「删掉别人刚建的新锁」这种竞态。
class SaveLock {
public:
  SaveLock() = default;
  SaveLock(const SaveLock&) = delete;
  SaveLock& operator=(const SaveLock&) = delete;
  ~SaveLock() { Release(); }

  // 在有限预算内重试拿锁；预算耗尽仍未拿到就是「另一个实例正持锁」，busy 为 true，
  // 调用方如实报告「本次没有写任何东西」，不做年龄猜测，也不硬抢。
  bool TryAcquire(const std::wstring& lockFile, bool* busy, std::wstring* reason) {
    for (int attempt = 0; attempt < kAcquireAttempts; ++attempt) {
      const HANDLE handle = ::CreateFileW(lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle != INVALID_HANDLE_VALUE) {
        handle_ = handle;
        *busy = false;
        return true;
      }
      const unsigned long error = ::GetLastError();
      if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
        if (reason != nullptr) {
          *reason = L"无法在数据目录里建立写锁：" + FormatWindowsError(error) + L"（" + lockFile + L"）。";
        }
        *busy = false;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    *reason = L"另一个 EvernightCommit 窗口正持有这份记录的写锁。本次没有写任何东西，"
              L"屏幕与内存里的内容原样保留，稍后的保存会再试。";
    *busy = true;
    return false;
  }

private:
  static constexpr int kAcquireAttempts = 12;  // 12 × 50ms ≈ 0.6 秒，够另一实例完成一次小文件写。

  void Release() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// 确认（必要时创建）数据目录。已存在但不是目录、或是再分析点：明确失败。
bool EnsureDataDirectory(const std::wstring& directory, std::wstring* reason) {
  if (IsDirectoryButNotReparse(directory)) {
    return true;
  }
  if (PathExists(directory)) {
    if (reason != nullptr) {
      *reason = L"数据目录位置被一个不是目录（或是再分析点）的东西占着，已拒绝写入：" + directory;
    }
    return false;
  }
  if (::CreateDirectoryW(directory.c_str(), nullptr) != 0) {
    return true;
  }
  const unsigned long error = ::GetLastError();
  if (error == ERROR_ALREADY_EXISTS && IsDirectoryButNotReparse(directory)) {
    return true;  // 两个实例同时首建：谁建成都一样，另一边的存在性同样可用。
  }
  if (reason != nullptr) {
    *reason = L"无法创建应用数据目录：" + FormatWindowsError(error) + L"（" + directory + L"）";
  }
  return false;
}

// 把读不成的原件改名保留（MoveFileEx 无 REPLACE：目标已存在就失败，换下一个候选名重试）。
// 改名失败时返回 false——那种场合绝不覆盖原文件，宁可这次不保存。
bool QuarantineCorruptFile(const std::wstring& stateFile, long long nowEpoch, std::wstring* backupPath,
                           std::wstring* reason) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::wstring candidate =
        stateFile + L".corrupt-" + std::to_wstring(nowEpoch < 0 ? 0 : nowEpoch) + L"-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(attempt);
    if (::MoveFileExW(stateFile.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
      *backupPath = candidate;
      return true;
    }
    const unsigned long error = ::GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_NOT_FOUND) {
      if (reason != nullptr) {
        *reason = L"损坏的记录文件无法改名保留：" + FormatWindowsError(error) +
                  L"。为避免覆盖任何现有内容，本次没有写入。";
      }
      return false;
    }
    if (error == ERROR_FILE_NOT_FOUND) {
      backupPath->clear();  // 原件已经不在了（被另一个实例处理过）：不需要保留动作也算成功。
      return true;
    }
  }
  if (reason != nullptr) {
    *reason = L"损坏的记录文件连续 8 个候选名都放不下，本次不覆盖它，也不写入新记录。";
  }
  return false;
}

// 原子发布：独占创建唯一的临时文件 → 全量写入 → FlushFileBuffers → 关闭 → MoveFileEx 替换。
// 任何一步失败都只清理自己创建的临时文件，目标文件要么是旧要么是新的整份，不会半新半旧。
bool PublishStateFile(const std::wstring& directory, const std::wstring& stateFile,
                      const std::string& payload, std::wstring* reason) {
  static std::atomic<unsigned long> sequence{0};
  std::wstring tempPath;
  bool created = false;
  for (int attempt = 0; attempt < 8 && !created; ++attempt) {
    const unsigned long nonce = static_cast<unsigned long>(
        (::GetTickCount64() ^ (static_cast<unsigned long long>(::GetCurrentProcessId()) << 16)) +
        sequence.fetch_add(1) + attempt * 4096u);
    tempPath = JoinPath(directory, L"state.tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                   std::to_wstring(nonce));
    // 临时文件也必须是「本调用创建」的：CREATE_NEW 撞名就换名重试，绝不采用已存在的东西。
    const HANDLE handle = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      continue;
    }
    DWORD written = 0;
    BOOL ok = TRUE;
    if (!payload.empty()) {
      ok = ::WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    }
    const unsigned long writeError = ::GetLastError();
    if (ok != 0 && written == static_cast<DWORD>(payload.size())) {
      ok = ::FlushFileBuffers(handle);
    }
    const unsigned long flushError = ::GetLastError();
    ::CloseHandle(handle);
    if (ok == 0) {
      static_cast<void>(::DeleteFileW(tempPath.c_str()));
      const unsigned long used = writeError != ERROR_SUCCESS ? writeError : flushError;
      if (used == ERROR_DISK_FULL) {
        if (reason != nullptr) {
          *reason = L"磁盘已满，记录没有写入（屏幕上与内存里的内容都原样保留）。";
        }
      } else if (reason != nullptr) {
        *reason = L"写入记录临时文件失败：" + FormatWindowsError(used) + L"；本次没有覆盖原记录。";
      }
      return false;
    }
    created = true;
  }
  if (!created) {
    if (reason != nullptr) {
      *reason = L"连续 8 个临时文件名都无法独占创建，已放弃本次保存（不采用任何已存在的文件）。";
    }
    return false;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (::MoveFileExW(tempPath.c_str(), stateFile.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return true;
    }
    const unsigned long error = ::GetLastError();
    if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_BUSY) {
      static_cast<void>(::DeleteFileW(tempPath.c_str()));
      if (reason != nullptr) {
        *reason = L"记录文件无法发布：" + FormatWindowsError(error) +
                  L"（临时文件已回收，原记录保持不动）。";
      }
      return false;
    }
    // 另一个实例正在读它：短暂等待后重试，仍失败就保持原样报告，绝不硬来。
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
  }
  static_cast<void>(::DeleteFileW(tempPath.c_str()));
  if (reason != nullptr) {
    *reason = L"记录文件被其他程序占用，本次保存没有发布；内存与屏幕上的内容原样保留。";
  }
  return false;
}

}  // namespace

PersistentStorePaths ResolvePersistentStorePaths(std::wstring_view baseDirectoryOverride) {
  PersistentStorePaths paths;
  std::wstring base = baseDirectoryOverride.empty() ? RoamingAppDataDirectory()
                                                    : std::wstring(baseDirectoryOverride);
  if (base.empty()) {
    paths.failureReason =
        L"当前用户的漫游应用数据目录（%APPDATA%）取不到，持久化已停用——不会写到项目目录或任何别的位置。";
    return paths;
  }
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
    base.pop_back();
  }
  if (!IsDirectoryButNotReparse(base)) {
    paths.failureReason = L"应用数据基目录不可用（不存在、不是目录或是再分析点）：" + base;
    return paths;
  }
  paths.directory = JoinPath(base, kPersistentDirectoryName);
  if (PathExists(paths.directory) && !IsDirectoryButNotReparse(paths.directory)) {
    paths.failureReason = L"数据目录位置被一个不是目录（或是再分析点）的东西占着：" + paths.directory;
    return paths;
  }
  paths.stateFile = JoinPath(paths.directory, kPersistentStateFileName);
  paths.lockFile = JoinPath(paths.directory, kPersistentLockFileName);
  paths.valid = true;
  return paths;
}

PersistentReadResult ReadPersistentStateFile(const std::wstring& stateFile) {
  PersistentReadResult result;
  const DWORD attributes = ::GetFileAttributesW(stateFile.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const unsigned long error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      result.kind = PersistentReadKind::absent;
    } else {
      result.kind = PersistentReadKind::unreadable;
      result.detail = L"读取记录文件前先看不到它：" + FormatWindowsError(error);
    }
    return result;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    result.kind = PersistentReadKind::unreadable;
    result.detail = L"记录文件是一个再分析点，按约定不跟随，整份视为不可读。";
    return result;
  }
  const HANDLE handle = ::CreateFileW(stateFile.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    result.kind = PersistentReadKind::unreadable;
    result.detail = L"打开记录文件失败：" + FormatWindowsError(::GetLastError());
    return result;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(handle, &info) == 0) {
    result.kind = PersistentReadKind::unreadable;
    result.detail = L"读不出记录文件的大小：" + FormatWindowsError(::GetLastError());
    ::CloseHandle(handle);
    return result;
  }
  const unsigned long long fileSize =
      (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  if (fileSize > kPersistentFileMaxBytes) {
    result.kind = PersistentReadKind::oversized;
    result.detail = L"记录文件超过 " + std::to_wstring(kPersistentFileMaxBytes / (1024 * 1024)) +
                    L" MiB 上限（实际约 " + std::to_wstring(fileSize / 1024) + L" KiB），整份不可信。";
    ::CloseHandle(handle);
    return result;
  }
  std::string bytes;
  bytes.reserve(static_cast<size_t>(fileSize));
  bool readOk = true;
  for (unsigned long long remaining = fileSize; remaining > 0;) {
    const DWORD chunk = static_cast<DWORD>(remaining > 65536 ? 65536 : remaining);
    char buffer[65536];
    DWORD gotten = 0;
    if (::ReadFile(handle, buffer, chunk, &gotten, nullptr) == 0 || gotten == 0) {
      readOk = false;
      break;
    }
    bytes.append(buffer, gotten);
    remaining -= gotten;
  }
  ::CloseHandle(handle);
  if (!readOk) {
    result.kind = PersistentReadKind::unreadable;
    result.detail = L"记录文件没能完整读回：" + FormatWindowsError(::GetLastError()) +
                    L"（大小 " + std::to_wstring(fileSize) + L" 字节，读到 " +
                    std::to_wstring(bytes.size()) + L" 字节）。残缺的一半绝不当成快照使用。";
    return result;
  }
  std::wstring text;
  if (!TryUtf8ToUtf16Strict(bytes, text)) {
    result.kind = PersistentReadKind::invalidUtf8;
    result.detail = L"记录文件里有不是合法 UTF-8 的字节序列，整份不可信。";
    return result;
  }
  result.kind = PersistentReadKind::loaded;
  result.text = std::move(text);
  return result;
}

PersistentSaveOutcome SavePersistentState(const PersistentStorePaths& paths, const app::PersistentState& ours,
                                          const app::PersistentWriteIntents& intents, long long nowEpoch) {
  PersistentSaveOutcome outcome;
  if (!paths.valid) {
    outcome.status = PersistentSaveStatus::failed;
    outcome.detail = paths.failureReason.empty() ? L"持久化路径没有解析成功。" : paths.failureReason;
    return outcome;
  }
  std::wstring directoryReason;
  if (!EnsureDataDirectory(paths.directory, &directoryReason)) {
    outcome.status = PersistentSaveStatus::failed;
    outcome.detail = directoryReason;
    return outcome;
  }
  SaveLock lock;
  bool busy = false;
  std::wstring lockReason;
  if (!lock.TryAcquire(paths.lockFile, &busy, &lockReason)) {
    outcome.status = busy ? PersistentSaveStatus::busy : PersistentSaveStatus::failed;
    outcome.detail = lockReason;
    return outcome;
  }
  // 拿锁之后重新读盘：基线是「此刻盘上的内容」，不是本窗口启动时看到的那一份。
  const PersistentReadResult read = ReadPersistentStateFile(paths.stateFile);
  app::PersistentState disk;
  bool recoveredCorrupt = false;
  switch (read.kind) {
    case PersistentReadKind::absent:
      break;
    case PersistentReadKind::loaded: {
      const app::PersistentLoadResult parsed = app::ParsePersistentState(read.text);
      switch (parsed.status) {
        case app::PersistentLoadStatus::tooNew:
          outcome.status = PersistentSaveStatus::refusedTooNew;
          outcome.detail = L"盘上的记录文件由更新版本的程序写出（" + parsed.reason +
                           L"）。本版本不读取、也不覆盖它。";
          return outcome;
        case app::PersistentLoadStatus::corrupt: {
          std::wstring backupPath;
          std::wstring quarantineReason;
          if (!QuarantineCorruptFile(paths.stateFile, nowEpoch, &backupPath, &quarantineReason)) {
            outcome.status = PersistentSaveStatus::failed;
            outcome.detail = quarantineReason;
            return outcome;
          }
          recoveredCorrupt = true;
          outcome.detail = L"原来的记录文件读不成立（" + parsed.reason +
                           L"），原件已保留为：" + (backupPath.empty() ? L"（原件已不在）" : backupPath) +
                           L"；现在开始写入本窗口这份新的记录。";
          break;
        }
        case app::PersistentLoadStatus::empty:
        case app::PersistentLoadStatus::loaded:
        case app::PersistentLoadStatus::migrated:
          disk = parsed.state;
          break;
      }
      break;
    }
    case PersistentReadKind::oversized:
    case PersistentReadKind::invalidUtf8: {
      std::wstring backupPath;
      std::wstring quarantineReason;
      if (!QuarantineCorruptFile(paths.stateFile, nowEpoch, &backupPath, &quarantineReason)) {
        outcome.status = PersistentSaveStatus::failed;
        outcome.detail = quarantineReason;
        return outcome;
      }
      recoveredCorrupt = true;
      outcome.detail = L"原来的记录文件不成立（" + read.detail + L"），原件已保留为：" +
                       (backupPath.empty() ? L"（原件已不在）" : backupPath) + L"；现在开始写入新的记录。";
      break;
    }
    case PersistentReadKind::unreadable:
      outcome.status = PersistentSaveStatus::failed;
      outcome.detail = read.detail + L"（读不成时绝不覆盖着写。）";
      return outcome;
  }

  app::PersistentState oursSnapshot = ours;
  oursSnapshot.revision = disk.revision;  // 修订号以盘上为准，合并里统一 +1。
  const app::PersistentState merged = app::MergeForWrite(disk, oursSnapshot, intents, &outcome.report);
  const std::wstring serialized = app::SerializePersistentState(merged);
  std::string payload;
  if (!TryUtf16ToUtf8Strict(serialized, payload)) {
    outcome.status = PersistentSaveStatus::failed;
    outcome.detail = L"记录内容里有无法编码成 UTF-8 的字符（孤立代理项），整份没有写出。";
    return outcome;
  }
  std::wstring publishReason;
  if (!PublishStateFile(paths.directory, paths.stateFile, payload, &publishReason)) {
    outcome.status = PersistentSaveStatus::failed;
    outcome.detail = publishReason;
    return outcome;
  }
  outcome.status = recoveredCorrupt ? PersistentSaveStatus::recoveredCorrupt : PersistentSaveStatus::saved;
  outcome.merged = merged;
  return outcome;
}

bool DeletePersistentStateFile(const PersistentStorePaths& paths, std::wstring* reason) {
  if (!paths.valid) {
    if (reason != nullptr) {
      *reason = paths.failureReason.empty() ? L"持久化路径没有解析成功，没有可删除的文件。" : paths.failureReason;
    }
    return false;
  }
  if (::DeleteFileW(paths.stateFile.c_str()) != 0) {
    return true;
  }
  const unsigned long error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return true;  // 已经不在了（连目录都没建起来过）：删除的目标状态已达成。
  }
  if (reason != nullptr) {
    *reason = L"删除记录文件失败：" + FormatWindowsError(error);
  }
  return false;
}

bool IsWindowRectReachable(int x, int y, int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const RECT rc{x, y, x + width, y + height};
  HMONITOR monitor = ::MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
  if (monitor == nullptr) {
    return false;  // 与任何显示器都不相交：不采用。
  }
  MONITORINFO info{sizeof(info)};
  if (::GetMonitorInfoW(monitor, &info) == 0) {
    return false;
  }
  RECT overlap{};
  return ::IntersectRect(&overlap, &rc, &info.rcMonitor) != FALSE;
}

}  // namespace gc::platform
