#include "platform/windows/operation_history_store.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "platform/windows/persistent_store.h"  // 复用同一处应用数据目录解析
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

bool PathExists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring JoinPath(std::wstring_view directory, std::wstring_view name) {
  std::wstring out(directory);
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
    out.push_back(L'\\');
  }
  out += name;
  return out;
}

// 与 persistent_store 同一套独占句柄租约写锁：同一个锁文件以共享模式 0 被本进程独占打开，
// 持有者进程退出（含崩溃）时句柄由系统回收，锁立刻可被下一个实例拿到。锁文件常驻不删除。
class SaveLock {
public:
  SaveLock() = default;
  SaveLock(const SaveLock&) = delete;
  SaveLock& operator=(const SaveLock&) = delete;
  ~SaveLock() { Release(); }

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
          *reason = L"无法在数据目录里建立历史写锁：" + FormatWindowsError(error) + L"（" + lockFile + L"）。";
        }
        *busy = false;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    *reason = L"另一个 EvernightCommit 窗口正持有这份历史记录的写锁。本次没有写任何东西，"
              L"内存里的历史原样保留，稍后的保存会再试。";
    *busy = true;
    return false;
  }

private:
  static constexpr int kAcquireAttempts = 12;  // 12 × 50ms ≈ 0.6 秒

  void Release() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

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
    return true;
  }
  if (reason != nullptr) {
    *reason = L"无法创建应用数据目录：" + FormatWindowsError(error) + L"（" + directory + L"）";
  }
  return false;
}

// 把读不成的原件改名保留（MoveFileEx 无 REPLACE：目标已存在就失败，换下一个候选名重试）。
bool QuarantineCorruptFile(const std::wstring& historyFile, long long nowEpoch, std::wstring* backupPath,
                           std::wstring* reason) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::wstring candidate =
        historyFile + L".corrupt-" + std::to_wstring(nowEpoch < 0 ? 0 : nowEpoch) + L"-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(attempt);
    if (::MoveFileExW(historyFile.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
      *backupPath = candidate;
      return true;
    }
    const unsigned long error = ::GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_NOT_FOUND) {
      if (reason != nullptr) {
        *reason = L"损坏的历史文件无法改名保留：" + FormatWindowsError(error) +
                  L"。为避免覆盖任何现有内容，本次没有写入。";
      }
      return false;
    }
    if (error == ERROR_FILE_NOT_FOUND) {
      backupPath->clear();  // 原件已不在（被另一实例处理过）：不需要保留动作也算成功。
      return true;
    }
  }
  if (reason != nullptr) {
    *reason = L"损坏的历史文件连续 8 个候选名都放不下，本次不覆盖它，也不写入新记录。";
  }
  return false;
}

// 原子发布：独占创建唯一临时文件 → 全量写入 → FlushFileBuffers → 关闭 → MoveFileEx 替换。
bool PublishHistoryFile(const std::wstring& directory, const std::wstring& historyFile,
                        const std::string& payload, std::wstring* reason) {
  static std::atomic<unsigned long> sequence{0};
  std::wstring tempPath;
  bool created = false;
  for (int attempt = 0; attempt < 8 && !created; ++attempt) {
    const unsigned long nonce = static_cast<unsigned long>(
        (::GetTickCount64() ^ (static_cast<unsigned long long>(::GetCurrentProcessId()) << 16)) +
        sequence.fetch_add(1) + attempt * 4096u);
    tempPath = JoinPath(directory,
                        L"history.tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                            std::to_wstring(nonce));
    const HANDLE handle = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      continue;  // CREATE_NEW 撞名就换名重试，绝不采用已存在的东西。
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
          *reason = L"磁盘已满，历史没有写入（内存里的历史原样保留）。";
        }
      } else if (reason != nullptr) {
        *reason = L"写入历史临时文件失败：" + FormatWindowsError(used) + L"；本次没有覆盖原历史。";
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
    if (::MoveFileExW(tempPath.c_str(), historyFile.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return true;
    }
    const unsigned long error = ::GetLastError();
    if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_BUSY) {
      static_cast<void>(::DeleteFileW(tempPath.c_str()));
      if (reason != nullptr) {
        *reason = L"历史文件无法发布：" + FormatWindowsError(error) + L"（临时文件已回收，原历史保持不动）。";
      }
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
  }
  static_cast<void>(::DeleteFileW(tempPath.c_str()));
  if (reason != nullptr) {
    *reason = L"历史文件被其他程序占用，本次保存没有发布；内存里的历史原样保留。";
  }
  return false;
}

}  // namespace

HistoryStorePaths ResolveHistoryStorePaths(std::wstring_view baseDirectoryOverride) {
  HistoryStorePaths paths;
  // 目录解析与持久化偏好共用同一处：同一个 %APPDATA%\EvernightCommit，同样的存在性/再分析点校验。
  const PersistentStorePaths base = ResolvePersistentStorePaths(baseDirectoryOverride);
  if (!base.valid) {
    paths.failureReason = base.failureReason.empty()
                              ? L"当前用户的漫游应用数据目录取不到，操作历史已停用——不会写到别的位置。"
                              : base.failureReason;
    return paths;
  }
  paths.directory = base.directory;
  paths.historyFile = JoinPath(base.directory, kHistoryFileName);
  paths.lockFile = JoinPath(base.directory, kHistoryLockFileName);
  paths.valid = true;
  return paths;
}

HistoryReadResult ReadHistoryFile(const std::wstring& historyFile) {
  HistoryReadResult result;
  const DWORD attributes = ::GetFileAttributesW(historyFile.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const unsigned long error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      result.kind = HistoryReadKind::absent;
    } else {
      result.kind = HistoryReadKind::unreadable;
      result.detail = L"读取历史文件前先看不到它：" + FormatWindowsError(error);
    }
    return result;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    result.kind = HistoryReadKind::unreadable;
    result.detail = L"历史文件是一个再分析点，按约定不跟随，整份视为不可读。";
    return result;
  }
  const HANDLE handle = ::CreateFileW(historyFile.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    result.kind = HistoryReadKind::unreadable;
    result.detail = L"打开历史文件失败：" + FormatWindowsError(::GetLastError());
    return result;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(handle, &info) == 0) {
    result.kind = HistoryReadKind::unreadable;
    result.detail = L"读不出历史文件的大小：" + FormatWindowsError(::GetLastError());
    ::CloseHandle(handle);
    return result;
  }
  const unsigned long long fileSize =
      (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  if (fileSize > kHistoryFileMaxBytes) {
    result.kind = HistoryReadKind::oversized;
    result.detail = L"历史文件超过 " + std::to_wstring(kHistoryFileMaxBytes / (1024 * 1024)) +
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
    result.kind = HistoryReadKind::unreadable;
    result.detail = L"历史文件没能完整读回：" + FormatWindowsError(::GetLastError()) +
                    L"（大小 " + std::to_wstring(fileSize) + L" 字节，读到 " + std::to_wstring(bytes.size()) +
                    L" 字节）。残缺的一半绝不当成历史使用。";
    return result;
  }
  std::wstring text;
  if (!TryUtf8ToUtf16Strict(bytes, text)) {
    result.kind = HistoryReadKind::invalidUtf8;
    result.detail = L"历史文件里有不是合法 UTF-8 的字节序列，整份不可信。";
    return result;
  }
  result.kind = HistoryReadKind::loaded;
  result.text = std::move(text);
  return result;
}

HistorySaveOutcome SaveOperationHistory(const HistoryStorePaths& paths, const app::OperationLog& ours,
                                        const app::HistoryWriteIntents& intents, long long nowEpoch) {
  HistorySaveOutcome outcome;
  if (!paths.valid) {
    outcome.status = HistorySaveStatus::failed;
    outcome.detail = paths.failureReason.empty() ? L"操作历史路径没有解析成功。" : paths.failureReason;
    return outcome;
  }
  std::wstring directoryReason;
  if (!EnsureDataDirectory(paths.directory, &directoryReason)) {
    outcome.status = HistorySaveStatus::failed;
    outcome.detail = directoryReason;
    return outcome;
  }
  SaveLock lock;
  bool busy = false;
  std::wstring lockReason;
  if (!lock.TryAcquire(paths.lockFile, &busy, &lockReason)) {
    outcome.status = busy ? HistorySaveStatus::busy : HistorySaveStatus::failed;
    outcome.detail = lockReason;
    return outcome;
  }
  // 拿锁之后重新读盘：基线是「此刻盘上的历史」，不是本窗口启动时看到的那一份。
  const HistoryReadResult read = ReadHistoryFile(paths.historyFile);
  app::OperationLog disk;
  bool recoveredCorrupt = false;
  switch (read.kind) {
    case HistoryReadKind::absent:
      break;
    case HistoryReadKind::loaded: {
      const app::HistoryLoadResult parsed = app::ParseOperationLog(read.text);
      switch (parsed.status) {
        case app::HistoryLoadStatus::tooNew:
          outcome.status = HistorySaveStatus::refusedTooNew;
          outcome.detail = L"盘上的历史文件由更新版本的程序写出（" + parsed.reason +
                           L"）。本版本不读取、也不覆盖它。";
          return outcome;
        case app::HistoryLoadStatus::corrupt: {
          std::wstring backupPath;
          std::wstring quarantineReason;
          if (!QuarantineCorruptFile(paths.historyFile, nowEpoch, &backupPath, &quarantineReason)) {
            outcome.status = HistorySaveStatus::failed;
            outcome.detail = quarantineReason;
            return outcome;
          }
          recoveredCorrupt = true;
          outcome.detail = L"原来的历史文件读不成立（" + parsed.reason + L"），原件已保留为：" +
                           (backupPath.empty() ? L"（原件已不在）" : backupPath) + L"；现在开始写入本窗口这份新的历史。";
          break;
        }
        case app::HistoryLoadStatus::empty:
        case app::HistoryLoadStatus::loaded:
          disk = parsed.log;
          break;
      }
      break;
    }
    case HistoryReadKind::oversized:
    case HistoryReadKind::invalidUtf8: {
      std::wstring backupPath;
      std::wstring quarantineReason;
      if (!QuarantineCorruptFile(paths.historyFile, nowEpoch, &backupPath, &quarantineReason)) {
        outcome.status = HistorySaveStatus::failed;
        outcome.detail = quarantineReason;
        return outcome;
      }
      recoveredCorrupt = true;
      outcome.detail = L"原来的历史文件不成立（" + read.detail + L"），原件已保留为：" +
                       (backupPath.empty() ? L"（原件已不在）" : backupPath) + L"；现在开始写入新的历史。";
      break;
    }
    case HistoryReadKind::unreadable:
      outcome.status = HistorySaveStatus::failed;
      outcome.detail = read.detail + L"（读不成时绝不覆盖着写。）";
      return outcome;
  }

  app::OperationLog oursSnapshot = ours;
  oursSnapshot.formatVersion = disk.formatVersion;  // 版本一致性由合并里统一到当前版本
  const app::OperationLog merged =
      app::MergeHistoryForWrite(disk, oursSnapshot, intents, nowEpoch, &outcome.report);
  const std::wstring serialized = app::SerializeOperationLog(merged);
  std::string payload;
  if (!TryUtf16ToUtf8Strict(serialized, payload)) {
    outcome.status = HistorySaveStatus::failed;
    outcome.detail = L"历史内容里有无法编码成 UTF-8 的字符（孤立代理项），整份没有写出。";
    return outcome;
  }
  std::wstring publishReason;
  if (!PublishHistoryFile(paths.directory, paths.historyFile, payload, &publishReason)) {
    outcome.status = HistorySaveStatus::failed;
    outcome.detail = publishReason;
    return outcome;
  }
  outcome.status = recoveredCorrupt ? HistorySaveStatus::recoveredCorrupt : HistorySaveStatus::saved;
  outcome.merged = merged;
  return outcome;
}

bool DeleteHistoryFile(const HistoryStorePaths& paths, std::wstring* reason) {
  if (!paths.valid) {
    if (reason != nullptr) {
      *reason = paths.failureReason.empty() ? L"操作历史路径没有解析成功，没有可删除的文件。" : paths.failureReason;
    }
    return false;
  }
  if (::DeleteFileW(paths.historyFile.c_str()) != 0) {
    return true;
  }
  const unsigned long error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return true;  // 已经不在了（连目录都没建起来过）：删除的目标状态已达成。
  }
  if (reason != nullptr) {
    *reason = L"删除历史文件失败：" + FormatWindowsError(error);
  }
  return false;
}

bool ExportOperationHistoryFile(const HistoryStorePaths& paths, const app::OperationLog& log,
                                 long long nowEpoch, std::wstring* outPath, std::wstring* reason) {
  if (!paths.valid || paths.directory.empty()) {
    if (reason != nullptr) {
      *reason = paths.failureReason.empty() ? L"操作历史路径没有解析成功，无法导出。" : paths.failureReason;
    }
    return false;
  }
  std::wstring directoryReason;
  if (!EnsureDataDirectory(paths.directory, &directoryReason)) {
    if (reason != nullptr) {
      *reason = directoryReason;
    }
    return false;
  }
  // 导出是一次性快照：与状态文件同目录、换个名字，不碰那条被反复读写的文件，也不占写锁。
  const std::wstring exportPath =
      JoinPath(paths.directory, L"history.export-" + std::to_wstring(nowEpoch < 0 ? 0 : nowEpoch) + L".txt");
  const std::wstring serialized = app::SerializeOperationLog(log);  // 落账时已逐字段掩码与限长
  std::string payload;
  if (!TryUtf16ToUtf8Strict(serialized, payload)) {
    if (reason != nullptr) {
      *reason = L"历史里有无法编码成 UTF-8 的字符（孤立代理项），导出没有写出。";
    }
    return false;
  }
  const HANDLE handle = ::CreateFileW(exportPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (reason != nullptr) {
      *reason = L"创建导出文件失败：" + FormatWindowsError(::GetLastError());
    }
    return false;
  }
  bool ok = true;
  DWORD written = 0;
  if (!payload.empty()) {
    ok = ::WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) != 0 &&
         written == static_cast<DWORD>(payload.size());
  }
  const unsigned long writeError = ::GetLastError();
  if (ok) {
    ok = ::FlushFileBuffers(handle) != 0;  // 交给用户自行打开的文件，落盘才算写完
  }
  ::CloseHandle(handle);
  if (!ok) {
    static_cast<void>(::DeleteFileW(exportPath.c_str()));
    if (reason != nullptr) {
      *reason = L"写入导出文件失败：" + FormatWindowsError(writeError);
    }
    return false;
  }
  if (outPath != nullptr) {
    *outPath = exportPath;
  }
  return true;
}

}  // namespace gc::platform
