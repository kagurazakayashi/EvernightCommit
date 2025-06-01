#include "platform/linux/scratch_files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/scratch_files.h"
#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

constexpr int kCreateRetryLimit = 8;

std::wstring ErrnoText(int error) {
  return L"errno " + std::to_wstring(error) + L"（" + Utf8ToWide(std::strerror(error)) + L"）";
}

bool IsAbsoluteBytes(std::string_view path) { return !path.empty() && path.front() == '/'; }

// 去掉结尾斜杠并拒绝 "." / ".." 段。结尾斜杠会让 lstat("link/") 跟随链接，
// 从而绕过“临时目录不能是符号链接”的检查，因此必须在探测前做词法清理。
bool CleanDirectoryPath(const std::string& input, std::string* cleaned, std::wstring* failureReason) {
  if (input.empty() || !IsAbsoluteBytes(input)) {
    *failureReason = L"临时目录必须是绝对路径：" + Utf8ToWide(input);
    return false;
  }
  std::string path = input;
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  size_t cursor = 0;
  while (cursor <= path.size()) {
    const size_t slash = path.find('/', cursor);
    const std::string_view segment =
        std::string_view(path).substr(cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
    if (segment == "." || segment == "..") {
      *failureReason = L"临时目录路径不能含 \".\" 或 \"..\" 段：" + Utf8ToWide(input);
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    cursor = slash + 1;
  }
  *cleaned = std::move(path);
  return true;
}

// 校验目录本身的 stat 事实。privateOnly 用于 XDG_RUNTIME_DIR：
// 它按约定只属于当前用户，组/其他人不得有任何权限。
bool ValidateDirectoryInfo(const struct stat& info, const std::string& bytes, bool privateOnly,
                           std::wstring* failureReason) {
  if (!S_ISDIR(info.st_mode)) {
    *failureReason = L"临时目录不是目录：" + Utf8ToWide(bytes);
    return false;
  }
  if (info.st_uid != ::geteuid()) {
    // /tmp 这类共享目录属主是 root，但必须带 sticky 位；其余情况只接受当前用户自己的目录。
    const bool sharedSticky = (info.st_mode & S_ISVTX) != 0 && (info.st_mode & 0002u) != 0;
    if (!sharedSticky) {
      *failureReason = L"临时目录不属于当前用户，也不具备 sticky 共享目录形态：" + Utf8ToWide(bytes);
      return false;
    }
  }
  if (privateOnly) {
    if ((info.st_mode & 0077u) != 0) {
      *failureReason = L"XDG_RUNTIME_DIR 必须只对当前用户开放：" + Utf8ToWide(bytes);
      return false;
    }
  } else {
    const bool writableByOthers = (info.st_mode & 0022u) != 0;
    if (writableByOthers && (info.st_mode & S_ISVTX) == 0) {
      *failureReason = L"临时目录可被组/其他人写入却没有 sticky 位，拒绝在其中创建文件：" +
                       Utf8ToWide(bytes);
      return false;
    }
  }
  return true;
}

// 校验目录本身（不跟随最终符号链接）。
bool ValidateDirectory(const std::string& bytes, bool privateOnly, std::wstring* failureReason) {
  if (bytes.empty() || !IsAbsoluteBytes(bytes)) {
    *failureReason = L"临时目录必须是绝对路径：" + Utf8ToWide(bytes);
    return false;
  }
  struct stat info {};
  if (::lstat(bytes.c_str(), &info) != 0) {
    *failureReason = L"读不到临时目录：" + Utf8ToWide(bytes) + L"（" + ErrnoText(errno) + L"）";
    return false;
  }
  if (S_ISLNK(info.st_mode)) {
    *failureReason = L"临时目录不能是符号链接（拒绝不可信指向）：" + Utf8ToWide(bytes);
    return false;
  }
  return ValidateDirectoryInfo(info, bytes, privateOnly, failureReason);
}

// 打开并再次校验目录句柄。后续文件用 openat(dirfd, ...) 创建，避免“校验的路径”
// 与“实际写入的目录”在两步之间被换掉。
int OpenScratchDirectory(const std::string& bytes, bool privateOnly, std::wstring* failureReason) {
  const int descriptor = ::open(bytes.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *failureReason = L"无法打开临时目录：" + Utf8ToWide(bytes) + L"（" + ErrnoText(errno) + L"）";
    return -1;
  }
  struct stat info {};
  if (::fstat(descriptor, &info) != 0 || !ValidateDirectoryInfo(info, bytes, privateOnly, failureReason)) {
    if (failureReason->empty()) {
      *failureReason = L"无法读取临时目录属性：" + Utf8ToWide(bytes) + L"（" + ErrnoText(errno) + L"）";
    }
    ::close(descriptor);
    return -1;
  }
  return descriptor;
}

bool ResolveBytes(std::string* outBytes, bool* outPrivateOnly, std::wstring* failureReason) {
  // 1) 首选 XDG_RUNTIME_DIR：按规范它已经是 0700 的用户私有目录。
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime != nullptr && runtime[0] != '\0') {
    std::string cleaned;
    if (!CleanDirectoryPath(runtime, &cleaned, failureReason)) {
      return false;
    }
    std::wstring ignored;
    if (ValidateDirectory(cleaned, /*privateOnly=*/true, &ignored)) {
      *outBytes = std::move(cleaned);
      *outPrivateOnly = true;
      return true;
    }
    // 设置了但不可信：不回退到更差的位置，由调用方看到明确失败。
    *failureReason = std::move(ignored);
    return false;
  }

  // 2) TMPDIR：显式设置时为唯一候选；不可信就失败，绝不悄悄换成 /tmp。
  const char* tmpdir = std::getenv("TMPDIR");
  if (tmpdir != nullptr && tmpdir[0] != '\0') {
    std::string cleaned;
    if (!CleanDirectoryPath(tmpdir, &cleaned, failureReason)) {
      return false;
    }
    if (!ValidateDirectory(cleaned, /*privateOnly=*/false, failureReason)) {
      return false;
    }
    *outBytes = std::move(cleaned);
    *outPrivateOnly = false;
    return true;
  }

  // 3) 标准回退。
  constexpr const char* kFallback = "/tmp";
  std::string cleaned;
  if (!CleanDirectoryPath(kFallback, &cleaned, failureReason) ||
      !ValidateDirectory(cleaned, /*privateOnly=*/false, failureReason)) {
    return false;
  }
  *outBytes = std::move(cleaned);
  *outPrivateOnly = false;
  return true;
}

// 本进程创建且尚未回收的文件。删除只认这张表：路径、dev 与 inode 三者都要对上，
// 即使同名路径被换成另一个文件（含符号链接），也不会去 unlink 它。
struct OwnedScratchFile {
  unsigned long long device = 0;
  unsigned long long inode = 0;
};

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, OwnedScratchFile>& Registry() {
  static std::map<std::string, OwnedScratchFile> files;
  return files;
}

unsigned long NextSequence() {
  static std::atomic<unsigned long> counter{0};
  return counter.fetch_add(1) + 1;
}

bool WriteAll(int descriptor, std::string_view payload, int* outError) {
  size_t written = 0;
  while (written < payload.size()) {
    const ssize_t count = ::write(descriptor, payload.data() + written, payload.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      *outError = errno;
      return false;
    }
    if (count == 0) {
      *outError = EIO;
      return false;
    }
    written += static_cast<size_t>(count);
  }
  return true;
}

// 在已打开并复核过的临时目录里独占创建。名字全部由本模块生成，不含用户输入的路径段。
int CreateExclusive(int directoryFd, const std::string& directory, const std::string& name,
                    std::string* outPath, OwnedScratchFile* outIdentity, int* outError) {
  const int descriptor = ::openat(directoryFd, name.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                  static_cast<mode_t>(0600));
  if (descriptor < 0) {
    *outError = errno;
    return -1;
  }
  // umask 只会收窄权限；显式 fchmod 保证结果就是 0600（不给组/其他人读的机会）。
  if (::fchmod(descriptor, static_cast<mode_t>(0600)) != 0) {
    *outError = errno;
    ::close(descriptor);
    static_cast<void>(::unlinkat(directoryFd, name.c_str(), 0));
    return -1;
  }
  struct stat info {};
  if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
    *outError = errno == 0 ? EINVAL : errno;
    ::close(descriptor);
    static_cast<void>(::unlinkat(directoryFd, name.c_str(), 0));
    return -1;
  }
  outIdentity->device = static_cast<unsigned long long>(info.st_dev);
  outIdentity->inode = static_cast<unsigned long long>(info.st_ino);
  *outPath = directory + "/" + name;
  return descriptor;
}

bool EncodePathForReport(const std::string& bytes, std::wstring* outText) {
  return TryWidePathFromBytes(bytes, *outText);
}

void RemoveRegistered(std::wstring_view path) {
  std::string bytes;
  if (!TryPathBytesFromWide(path, bytes)) {
    return;  // 无法无损表示的路径不可能是本模块写出的。
  }
  std::lock_guard<std::mutex> lock(RegistryMutex());
  auto& registry = Registry();
  const auto found = registry.find(bytes);
  if (found == registry.end()) {
    return;  // 不是本进程创建的文件：绝不动它。
  }
  struct stat info {};
  const bool sameIdentity =
      ::lstat(bytes.c_str(), &info) == 0 &&
      static_cast<unsigned long long>(info.st_dev) == found->second.device &&
      static_cast<unsigned long long>(info.st_ino) == found->second.inode;
  // 只移除我们创建的那一个 inode；路径被换名/替换后不再认，绝不跟随符号链接。
  if (sameIdentity && (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) &&
      info.st_uid == ::geteuid()) {
    static_cast<void>(::unlink(bytes.c_str()));
  }
  registry.erase(found);
}

}  // namespace

ScratchDirectory ResolveScratchDirectory() {
  ScratchDirectory result;
  std::string bytes;
  bool privateOnly = false;
  if (!ResolveBytes(&bytes, &privateOnly, &result.failureReason)) {
    return result;
  }
  if (!EncodePathForReport(bytes, &result.path)) {
    result.failureReason = L"临时目录路径不是有效 UTF-8，无法无损报告给界面：" + Utf8ToWide(bytes);
    return result;
  }
  result.ok = true;
  return result;
}

CommitMessageFileWrite WriteCommitMessageFile(std::wstring_view message) {
  CommitMessageFileWrite result;
  if (message.empty()) {
    result.failureReason = L"提交信息是空的，没有写出信息文件，也没有执行任何命令。";
    return result;
  }
  std::string payload;
  if (!TryWideToUtf8Strict(message, payload)) {
    result.failureReason = L"提交信息含有无法无损编码成 UTF-8 的字符，已拒绝写出。";
    return result;
  }
  if (payload.empty()) {
    result.failureReason = L"提交信息编码成 UTF-8 之后是空的，已拒绝写出。";
    return result;
  }
  if (payload.find('\0') != std::string::npos) {
    result.failureReason = L"提交信息含内嵌 NUL，Git 无法按文本读取，已拒绝写出。";
    return result;
  }
  if (payload.back() != '\n') {
    payload.push_back('\n');
  }

  std::string directory;
  bool privateOnly = false;
  if (!ResolveBytes(&directory, &privateOnly, &result.failureReason)) {
    return result;
  }
  const int directoryFd = OpenScratchDirectory(directory, privateOnly, &result.failureReason);
  if (directoryFd < 0) {
    return result;
  }
  struct DirectoryGuard {
    int descriptor;
    ~DirectoryGuard() {
      if (descriptor >= 0) {
        ::close(descriptor);
      }
    }
  } directoryGuard{directoryFd};

  const unsigned long pid = static_cast<unsigned long>(::getpid());
  for (int attempt = 0; attempt < kCreateRetryLimit; ++attempt) {
    const std::string name = "gc-msg-" + std::to_string(pid) + "-" + std::to_string(NextSequence()) + ".txt";
    std::string path;
    OwnedScratchFile identity;
    int error = 0;
    const int descriptor = CreateExclusive(directoryFd, directory, name, &path, &identity, &error);
    if (descriptor < 0) {
      if (error == EEXIST) {
        continue;  // 同名文件已存在：换下一个序号，绝不覆盖。
      }
      result.failureReason = L"无法独占创建提交信息文件（" + ErrnoText(error) + L"）。";
      return result;
    }
    int writeError = 0;
    const bool ok = WriteAll(descriptor, payload, &writeError);
    const int closeResult = ::close(descriptor);
    if (!ok || closeResult != 0) {
      const int failure = ok ? errno : writeError;
      static_cast<void>(::unlinkat(directoryFd, name.c_str(), 0));
      result.failureReason = L"写入提交信息文件失败（" + ErrnoText(failure) + L"）。";
      return result;
    }
    if (!EncodePathForReport(path, &result.path)) {
      static_cast<void>(::unlinkat(directoryFd, name.c_str(), 0));
      result.failureReason = L"提交信息文件路径不是有效 UTF-8，无法无损交给界面。";
      return result;
    }
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      Registry().emplace(path, identity);
    }
    result.written = true;
    result.payloadBytes = payload.size();
    return result;
  }
  result.failureReason = L"临时目录里已连续 " + std::to_wstring(kCreateRetryLimit) +
                         L" 个候选文件名都无法独占创建，已放弃写出（不覆盖任何现有文件）。";
  return result;
}

void RemoveCommitMessageFile(std::wstring_view path) { RemoveRegistered(path); }

PathspecFileWrite WriteNulPathspecFile(const std::vector<std::wstring>& pathspecElements,
                                       std::wstring_view filePrefix) {
  PathspecFileWrite result;
  if (pathspecElements.empty()) {
    result.failureReason = L"路径清单为空：没有范围的可执行命令一律拒绝，不写清单文件。";
    return result;
  }

  std::string prefix;
  if (!TryWideToUtf8Strict(filePrefix, prefix) || prefix.empty() || prefix.size() > 16) {
    result.failureReason = L"清单文件名前缀不符合纯字母与减号的约定，已拒绝写出。";
    return result;
  }
  for (const char c : prefix) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) {
      result.failureReason = L"清单文件名前缀不符合纯字母与减号的约定，已拒绝写出。";
      return result;
    }
  }

  std::string payload;
  for (const std::wstring& element : pathspecElements) {
    if (element.empty()) {
      result.failureReason = L"路径清单里出现空条目，已拒绝写出。";
      return result;
    }
    std::string encoded;
    if (!TryWideToUtf8Strict(element, encoded)) {
      result.failureReason = L"路径清单里有无法无损编码成 UTF-8 的条目，已拒绝写出。";
      return result;
    }
    if (encoded.find('\0') != std::string::npos) {
      result.failureReason = L"路径清单条目含内嵌 NUL，已拒绝写出。";
      return result;
    }
    payload += encoded;
    payload.push_back('\0');
  }

  std::string directory;
  bool privateOnly = false;
  if (!ResolveBytes(&directory, &privateOnly, &result.failureReason)) {
    return result;
  }
  const int directoryFd = OpenScratchDirectory(directory, privateOnly, &result.failureReason);
  if (directoryFd < 0) {
    return result;
  }
  struct DirectoryGuard {
    int descriptor;
    ~DirectoryGuard() {
      if (descriptor >= 0) {
        ::close(descriptor);
      }
    }
  } directoryGuard{directoryFd};

  const unsigned long pid = static_cast<unsigned long>(::getpid());
  for (int attempt = 0; attempt < kCreateRetryLimit; ++attempt) {
    const std::string name = prefix + "-" + std::to_string(pid) + "-" + std::to_string(NextSequence()) + ".nul";
    std::string path;
    OwnedScratchFile identity;
    int error = 0;
    const int descriptor = CreateExclusive(directoryFd, directory, name, &path, &identity, &error);
    if (descriptor < 0) {
      if (error == EEXIST) {
        continue;
      }
      result.failureReason = L"无法独占创建路径清单文件（" + ErrnoText(error) + L"）。";
      return result;
    }
    int writeError = 0;
    const bool ok = WriteAll(descriptor, payload, &writeError);
    const int closeResult = ::close(descriptor);
    if (!ok || closeResult != 0) {
      const int failure = ok ? errno : writeError;
      static_cast<void>(::unlinkat(directoryFd, name.c_str(), 0));
      result.failureReason = L"写入路径清单失败（" + ErrnoText(failure) + L"）。";
      return result;
    }
    if (!EncodePathForReport(path, &result.path)) {
      static_cast<void>(::unlinkat(directoryFd, name.c_str(), 0));
      result.failureReason = L"路径清单文件路径不是有效 UTF-8，无法无损交给界面。";
      return result;
    }
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      Registry().emplace(path, identity);
    }
    result.written = true;
    return result;
  }
  result.failureReason = L"临时目录里已连续 " + std::to_wstring(kCreateRetryLimit) +
                         L" 个候选文件名都无法独占创建，已放弃写出路径清单（不覆盖任何现有文件）。";
  return result;
}

void RemoveNulPathspecFile(std::wstring_view path) { RemoveRegistered(path); }

}  // namespace gc::platform
