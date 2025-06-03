#include "platform/linux/operation_directory.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "git/command_window.h"
#include "platform/linux/scratch_files.h"
#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

constexpr int kClaimAttempts = 8;
constexpr mode_t kOperationDirectoryMode = 0700;
constexpr mode_t kOperationFileMode = 0600;

std::wstring ErrnoText(int error) {
  return L"errno " + std::to_wstring(error) + L"（" + Utf8ToWide(std::strerror(error)) + L"）";
}

std::string TrimTrailingSlashes(std::string_view path) {
  std::string trimmed(path);
  while (trimmed.size() > 1 && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  return trimmed;
}

bool IsPlainDirectoryOwnedByUser(const struct stat& info) {
  return S_ISDIR(info.st_mode) && info.st_uid == ::geteuid();
}

// 临时根本身可以是 /tmp 这类 root 拥有的 sticky 共享目录；操作目录必须是当前用户
// 拥有且 0700。两者不能混用同一判据。
bool IsTrustedScratchRoot(const struct stat& info) {
  if (!S_ISDIR(info.st_mode)) {
    return false;
  }
  if (info.st_uid == ::geteuid()) {
    // 自己拥有的临时根：组/其他人可写会让别人有机会替换其中条目；只有目录本身
    // 可写不算问题，但“别人也能写”必须拒绝。
    return (info.st_mode & 0022u) == 0;
  }
  return (info.st_mode & S_ISVTX) != 0 && (info.st_mode & 0002u) != 0;
}

bool OpenScratchRootPath(std::wstring_view wantedPath, int* outFd, std::string* outPathBytes,
                         std::wstring* failureReason) {
  *outFd = -1;
  std::string bytes;
  if (!TryPathBytesFromWide(wantedPath, bytes) || bytes.empty() || bytes.front() != '/') {
    *failureReason = L"临时目录路径必须是能无损表示成 UTF-8 的绝对路径：" + std::wstring(wantedPath);
    return false;
  }
  bytes = TrimTrailingSlashes(bytes);
  const int fd = ::open(bytes.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    *failureReason = L"无法打开临时目录：" + std::wstring(wantedPath) + L"（" + ErrnoText(errno) + L"）";
    return false;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !IsTrustedScratchRoot(info)) {
    *failureReason = L"临时目录不是当前用户拥有或带 sticky 位的共享普通目录：" +
                     std::wstring(wantedPath);
    ::close(fd);
    return false;
  }
  *outFd = fd;
  *outPathBytes = std::move(bytes);
  return true;
}

bool OpenScratchRoot(int* outFd, std::string* outPathBytes, std::wstring* failureReason) {
  const ScratchDirectory scratch = ResolveScratchDirectory();
  if (!scratch.ok) {
    *failureReason = scratch.failureReason;
    return false;
  }
  return OpenScratchRootPath(scratch.path, outFd, outPathBytes, failureReason);
}

bool FillRandomHex(size_t byteCount, std::string* outHex) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::vector<unsigned char> bytes(byteCount);
  size_t filled = 0;
  while (filled < byteCount) {
    const ssize_t got = ::getrandom(bytes.data() + filled, byteCount - filled, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    filled += static_cast<size_t>(got);
  }
  if (filled != byteCount) {
    // getrandom 不可用时退回 /dev/urandom；仍读不到就明确失败，不用 PID/时间冒充随机。
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    filled = 0;
    while (filled < byteCount) {
      const ssize_t got = ::read(fd, bytes.data() + filled, byteCount - filled);
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        ::close(fd);
        return false;
      }
      if (got == 0) {
        ::close(fd);
        return false;
      }
      filled += static_cast<size_t>(got);
    }
    ::close(fd);
  }
  outHex->clear();
  outHex->reserve(byteCount * 2);
  for (const unsigned char byte : bytes) {
    outHex->push_back(kHex[byte >> 4u]);
    outHex->push_back(kHex[byte & 0x0Fu]);
  }
  return true;
}

bool WriteAll(int fd, std::string_view bytes) {
  size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (count == 0) {
      return false;
    }
    written += static_cast<size_t>(count);
  }
  return true;
}

bool CloseAndReport(int fd, int* closeError) {
  const int result = ::close(fd);
  if (result != 0) {
    *closeError = errno;
    return false;
  }
  return true;
}

bool OpenDirectoryAt(int parentFd, std::string_view name, int* outFd, std::wstring* failureReason) {
  const std::string nameBytes(name);
  const int fd = ::openat(parentFd, nameBytes.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    *failureReason = L"无法打开操作目录：" + Utf8ToWide(nameBytes) + L"（" + ErrnoText(errno) + L"）";
    return false;
  }
  *outFd = fd;
  return true;
}

bool FileExistsAt(int dirFd, std::string_view name, bool* exists) {
  struct stat info {};
  const std::string nameBytes(name);
  if (::fstatat(dirFd, nameBytes.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
    *exists = true;
    return true;
  }
  if (errno == ENOENT) {
    *exists = false;
    return true;
  }
  return false;
}

}  // namespace

void CloseOperationDirectory(ClaimedOperationDirectory* directory) {
  if (directory == nullptr) {
    return;
  }
  if (directory->leaseFd >= 0) {
    ::close(directory->leaseFd);
    directory->leaseFd = -1;
  }
  if (directory->dirFd >= 0) {
    ::close(directory->dirFd);
    directory->dirFd = -1;
  }
  directory->ok = false;
}

ClaimedOperationDirectory ClaimOperationDirectoryLinux() {
  ClaimedOperationDirectory result;
  int rootFd = -1;
  std::string rootPath;
  std::wstring failure;
  if (!OpenScratchRoot(&rootFd, &rootPath, &failure)) {
    result.failureReason = std::move(failure);
    return result;
  }
  struct RootGuard {
    int fd;
    ~RootGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } rootGuard{rootFd};

  const unsigned long pid = static_cast<unsigned long>(::getpid());
  for (int attempt = 0; attempt < kClaimAttempts; ++attempt) {
    std::string randomHex;
    if (!FillRandomHex(8, &randomHex)) {
      result.failureReason = L"无法取得系统随机源，本次操作没有创建目录。";
      return result;
    }
    std::string name = "GcOp" + std::to_string(pid) + "x" + randomHex;
    if (!git::IsSafeOperationDirectoryName(Utf8ToWide(name))) {
      result.failureReason = L"内部错误：生成的操作目录名不符合约定。";
      return result;
    }
    if (::mkdirat(rootFd, name.c_str(), kOperationDirectoryMode) != 0) {
      if (errno == EEXIST) {
        continue;  // 撞名就换一个猜不到的名字；绝不复用已有目录。
      }
      result.failureReason = L"创建操作目录失败：" + ErrnoText(errno);
      return result;
    }

    int dirFd = -1;
    if (!OpenDirectoryAt(rootFd, name, &dirFd, &failure)) {
      static_cast<void>(::unlinkat(rootFd, name.c_str(), AT_REMOVEDIR));
      result.failureReason = std::move(failure);
      return result;
    }
    struct DirGuard {
      int fd;
      ~DirGuard() {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    } dirGuard{dirFd};

    struct stat info {};
    if (::fstat(dirFd, &info) != 0 || !IsPlainDirectoryOwnedByUser(info)) {
      static_cast<void>(::unlinkat(rootFd, name.c_str(), AT_REMOVEDIR));
      result.failureReason = L"新建的操作目录不是当前用户拥有的普通目录。";
      return result;
    }
    if ((info.st_mode & 0077u) != 0 && ::fchmod(dirFd, kOperationDirectoryMode) != 0) {
      static_cast<void>(::unlinkat(rootFd, name.c_str(), AT_REMOVEDIR));
      result.failureReason = L"无法把操作目录权限收紧到 0700：" + ErrnoText(errno);
      return result;
    }

    int leaseFd = ::openat(dirFd, git::kLeaseFileName,
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                           kOperationFileMode);
    if (leaseFd < 0) {
      static_cast<void>(::unlinkat(rootFd, name.c_str(), AT_REMOVEDIR));
      result.failureReason = L"创建操作目录租约失败：" + ErrnoText(errno);
      return result;
    }
    if (::flock(leaseFd, LOCK_EX | LOCK_NB) != 0) {
      ::close(leaseFd);
      static_cast<void>(::unlinkat(rootFd, name.c_str(), AT_REMOVEDIR));
      result.failureReason = L"无法锁住操作目录租约：" + ErrnoText(errno);
      return result;
    }
    const std::string leaseText = "lease\t" + name + "\n";
    if (!WriteAll(leaseFd, leaseText) || ::fsync(leaseFd) != 0) {
      ::close(leaseFd);
      static_cast<void>(::unlinkat(rootFd, name.c_str(), AT_REMOVEDIR));
      result.failureReason = L"写入操作目录租约失败：" + ErrnoText(errno);
      return result;
    }

    result.ok = true;
    result.dirFd = dirFd;
    result.leaseFd = leaseFd;
    result.name = std::move(name);
    result.pathBytes = rootPath + "/" + result.name;
    std::wstring wideRoot;
    std::wstring widePath;
    if (!TryWidePathFromBytes(rootPath, wideRoot) ||
        !TryWidePathFromBytes(result.pathBytes, widePath)) {
      ::close(leaseFd);
      result.dirFd = -1;
      result.ok = false;
      result.failureReason = L"操作目录路径无法无损表示成界面文本。";
      return result;
    }
    result.rootPath = std::move(wideRoot);
    result.path = std::move(widePath);
    dirGuard.fd = -1;  // 所有权交给调用方
    return result;
  }

  result.failureReason = L"连续 " + std::to_wstring(kClaimAttempts) +
                         L" 个随机目录名都已被占用，本次操作没有启动。";
  return result;
}

OpenedOperationDirectory OpenOperationDirectoryForHelper(std::wstring_view path,
                                                        std::wstring_view scratchRootOverride) {
  OpenedOperationDirectory result;
  std::string pathBytes;
  if (!TryPathBytesFromWide(path, pathBytes) || pathBytes.empty() || pathBytes.front() != '/') {
    result.failureReason = L"操作目录必须是能无损表示成 UTF-8 的绝对路径。";
    return result;
  }
  pathBytes = TrimTrailingSlashes(pathBytes);

  int rootFd = -1;
  std::string rootPath;
  if (scratchRootOverride.empty()) {
    if (!OpenScratchRoot(&rootFd, &rootPath, &result.failureReason)) {
      return result;
    }
  } else if (!OpenScratchRootPath(scratchRootOverride, &rootFd, &rootPath,
                                  &result.failureReason)) {
    return result;
  }
  struct RootGuard {
    int fd;
    ~RootGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } rootGuard{rootFd};

  const std::string prefix = rootPath + "/";
  if (pathBytes.size() <= prefix.size() || pathBytes.compare(0, prefix.size(), prefix) != 0) {
    result.failureReason = L"操作目录不在当前用户的临时目录根下：" + std::wstring(path);
    return result;
  }
  const std::string name = pathBytes.substr(prefix.size());
  if (name.find('/') != std::string::npos) {
    result.failureReason = L"操作目录必须是临时根下的一级目录。";
    return result;
  }
  const std::wstring nameWide = Utf8ToWide(name);
  if (!git::IsSafeOperationDirectoryName(nameWide)) {
    result.failureReason = L"操作目录名不符合本程序生成的形态：" + nameWide;
    return result;
  }

  int dirFd = -1;
  if (!OpenDirectoryAt(rootFd, name, &dirFd, &result.failureReason)) {
    return result;
  }
  struct stat info {};
  if (::fstat(dirFd, &info) != 0 || !IsPlainDirectoryOwnedByUser(info) ||
      (info.st_mode & 0077u) != 0) {
    result.failureReason = L"操作目录必须是当前用户拥有、且只对自己开放的普通目录。";
    ::close(dirFd);
    return result;
  }
  result.ok = true;
  result.dirFd = dirFd;
  result.name = name;
  result.pathBytes = pathBytes;
  result.path = std::wstring(path);
  return result;
}

void CloseOpenedOperationDirectory(OpenedOperationDirectory* directory) {
  if (directory == nullptr) {
    return;
  }
  if (directory->dirFd >= 0) {
    ::close(directory->dirFd);
    directory->dirFd = -1;
  }
  directory->ok = false;
}

bool WriteExclusiveFileAt(int dirFd, std::string_view name, std::string_view bytes,
                          std::wstring* failureReason) {
  const std::string nameBytes(name);
  const int fd = ::openat(dirFd, nameBytes.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          kOperationFileMode);
  if (fd < 0) {
    if (failureReason != nullptr) {
      *failureReason = L"无法独占创建 " + Utf8ToWide(nameBytes) + L"：" + ErrnoText(errno);
    }
    return false;
  }
  if (::fchmod(fd, kOperationFileMode) != 0 || !WriteAll(fd, bytes) || ::fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    static_cast<void>(::unlinkat(dirFd, nameBytes.c_str(), 0));
    if (failureReason != nullptr) {
      *failureReason = L"写入 " + Utf8ToWide(nameBytes) + L" 失败：" + ErrnoText(error);
    }
    return false;
  }
  int closeError = 0;
  if (!CloseAndReport(fd, &closeError)) {
    static_cast<void>(::unlinkat(dirFd, nameBytes.c_str(), 0));
    if (failureReason != nullptr) {
      *failureReason = L"关闭 " + Utf8ToWide(nameBytes) + L" 失败：" + ErrnoText(closeError);
    }
    return false;
  }
  return true;
}

bool PublishResultFileAt(int dirFd, std::string_view tempName, std::string_view finalName,
                         std::string_view bytes, std::wstring* failureReason) {
  const std::string tempBytes(tempName);
  const std::string finalBytes(finalName);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const int fd = ::openat(dirFd, tempBytes.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            kOperationFileMode);
    if (fd < 0) {
      if (errno == EEXIST && attempt == 0) {
        // 上一次没发布成功的暂存文件：只删这个已知名字，不碰正式结果。
        static_cast<void>(::unlinkat(dirFd, tempBytes.c_str(), 0));
        continue;
      }
      if (failureReason != nullptr) {
        *failureReason = L"无法创建结果暂存文件：" + ErrnoText(errno);
      }
      return false;
    }
    if (::fchmod(fd, kOperationFileMode) != 0 || !WriteAll(fd, bytes) || ::fsync(fd) != 0) {
      const int error = errno;
      ::close(fd);
      static_cast<void>(::unlinkat(dirFd, tempBytes.c_str(), 0));
      if (failureReason != nullptr) {
        *failureReason = L"写结果暂存文件失败：" + ErrnoText(error);
      }
      return false;
    }
    int closeError = 0;
    if (!CloseAndReport(fd, &closeError)) {
      static_cast<void>(::unlinkat(dirFd, tempBytes.c_str(), 0));
      if (failureReason != nullptr) {
        *failureReason = L"关闭结果暂存文件失败：" + ErrnoText(closeError);
      }
      return false;
    }
    if (::renameat(dirFd, tempBytes.c_str(), dirFd, finalBytes.c_str()) != 0) {
      const int error = errno;
      static_cast<void>(::unlinkat(dirFd, tempBytes.c_str(), 0));
      if (failureReason != nullptr) {
        *failureReason = L"发布结果文件失败：" + ErrnoText(error);
      }
      return false;
    }
    static_cast<void>(::fsync(dirFd));
    return true;
  }
  if (failureReason != nullptr) {
    *failureReason = L"结果暂存文件无法创建（同名残留反复出现）。";
  }
  return false;
}

bool ReadOperationFileAt(int dirFd, std::string_view name, size_t maxBytes, std::string* bytes,
                         bool* exists, std::wstring* failureReason) {
  if (bytes == nullptr || exists == nullptr) {
    return false;
  }
  bytes->clear();
  *exists = false;
  const std::string nameBytes(name);
  const int fd = ::openat(dirFd, nameBytes.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) {
      return true;
    }
    if (failureReason != nullptr) {
      *failureReason = L"无法打开 " + Utf8ToWide(nameBytes) + L"：" + ErrnoText(errno);
    }
    return false;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 0077u) != 0 || static_cast<size_t>(info.st_size) > maxBytes) {
    if (failureReason != nullptr) {
      *failureReason = L"文件 " + Utf8ToWide(nameBytes) +
                       L" 不是当前用户独占的普通文件，或超过大小上限。";
    }
    ::close(fd);
    return false;
  }
  *exists = true;
  std::string content;
  content.reserve(static_cast<size_t>(info.st_size));
  char buffer[4096];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (failureReason != nullptr) {
        *failureReason = L"读取 " + Utf8ToWide(nameBytes) + L" 失败：" + ErrnoText(errno);
      }
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    content.append(buffer, static_cast<size_t>(got));
    if (content.size() > maxBytes) {
      if (failureReason != nullptr) {
        *failureReason = L"文件 " + Utf8ToWide(nameBytes) + L" 超过大小上限。";
      }
      ::close(fd);
      return false;
    }
  }
  ::close(fd);
  *bytes = std::move(content);
  return true;
}

OperationFileLockState ProbeOperationFileLockAt(int dirFd, std::string_view name) {
  const std::string nameBytes(name);
  const int fd = ::openat(dirFd, nameBytes.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) {
      return OperationFileLockState::absent;
    }
    return OperationFileLockState::blocked;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return OperationFileLockState::blocked;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
    static_cast<void>(::flock(fd, LOCK_UN));
    ::close(fd);
    return OperationFileLockState::free;
  }
  const int error = errno;
  ::close(fd);
  if (error == EWOULDBLOCK || error == EAGAIN) {
    return OperationFileLockState::held;
  }
  return OperationFileLockState::blocked;
}

int CreateHeldStartMarkerAt(int dirFd, std::string_view text, std::wstring* failureReason) {
  const int fd = ::openat(dirFd, git::kStartMarkerFileName,
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          kOperationFileMode);
  if (fd < 0) {
    if (failureReason != nullptr) {
      *failureReason = L"创建开始标记失败：" + ErrnoText(errno);
    }
    return -1;
  }
  if (::fchmod(fd, kOperationFileMode) != 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0 ||
      !WriteAll(fd, text) || ::fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    static_cast<void>(::unlinkat(dirFd, git::kStartMarkerFileName, 0));
    if (failureReason != nullptr) {
      *failureReason = L"写入开始标记失败：" + ErrnoText(error);
    }
    return -1;
  }
  return fd;
}

bool TryReclaimOperationDirectory(int parentDirFd, std::string_view name, bool requireSpecConsumed,
                                  std::wstring* preservedReason) {
  const std::string nameBytes(name);
  const int dirFd = ::openat(parentDirFd, nameBytes.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dirFd < 0) {
    if (errno == ENOENT) {
      return true;
    }
    if (preservedReason != nullptr) {
      *preservedReason = L"无法打开操作目录（不是普通目录或权限不足），保留：" +
                         Utf8ToWide(nameBytes) + L"（" + ErrnoText(errno) + L"）";
    }
    return false;
  }
  struct DirGuard {
    int fd;
    ~DirGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } guard{dirFd};

  struct stat info {};
  if (::fstat(dirFd, &info) != 0 || !IsPlainDirectoryOwnedByUser(info)) {
    if (preservedReason != nullptr) {
      *preservedReason = L"目录不是当前用户拥有的普通目录，保留：" + Utf8ToWide(nameBytes);
    }
    return false;
  }
  if ((info.st_mode & 0077u) != 0) {
    if (preservedReason != nullptr) {
      *preservedReason = L"目录权限不是 0700，形态不可信，保留：" + Utf8ToWide(nameBytes);
    }
    return false;
  }

  for (const std::string_view fileName : {std::string_view(git::kLeaseFileName),
                                          std::string_view(git::kStartMarkerFileName)}) {
    const OperationFileLockState state = ProbeOperationFileLockAt(dirFd, fileName);
    if (state == OperationFileLockState::held) {
      if (preservedReason != nullptr) {
        *preservedReason = L"名单内的文件仍被持有（" + Utf8ToWide(std::string(fileName)) +
                           L"），操作可能还在进行，目录保留：" + Utf8ToWide(nameBytes);
      }
      return false;
    }
    if (state == OperationFileLockState::blocked) {
      if (preservedReason != nullptr) {
        *preservedReason = L"无法判断名单内的文件是否仍被使用，目录保留：" + Utf8ToWide(nameBytes);
      }
      return false;
    }
  }

  if (requireSpecConsumed) {
    bool specExists = false;
    bool startExists = false;
    if (!FileExistsAt(dirFd, git::kSpecFileName, &specExists) ||
        !FileExistsAt(dirFd, git::kStartMarkerFileName, &startExists)) {
      if (preservedReason != nullptr) {
        *preservedReason = L"无法确认说明书与开始标记的消费状态，目录保留：" + Utf8ToWide(nameBytes);
      }
      return false;
    }
    if (specExists && !startExists) {
      if (preservedReason != nullptr) {
        *preservedReason = L"说明书还在而开始标记尚未出现，不能排除辅助程序尚未读取，目录保留：" +
                           Utf8ToWide(nameBytes);
      }
      return false;
    }
  }

  for (const std::string_view fileName : git::kOperationFileNames) {
    if (::unlinkat(dirFd, std::string(fileName).c_str(), 0) != 0 && errno != ENOENT) {
      if (preservedReason != nullptr) {
        *preservedReason = L"删除名单内文件失败（" + Utf8ToWide(std::string(fileName)) +
                           L"），目录保留：" + Utf8ToWide(nameBytes);
      }
      return false;
    }
  }
  if (::unlinkat(parentDirFd, nameBytes.c_str(), AT_REMOVEDIR) != 0) {
    if (preservedReason != nullptr) {
      *preservedReason = L"目录里还有名单之外的内容或仍被引用，保留：" + Utf8ToWide(nameBytes) +
                         L"（" + ErrnoText(errno) + L"）";
    }
    return false;
  }
  return true;
}

bool ReclaimOperationDirectoryByPath(std::wstring_view path, bool requireSpecConsumed,
                                     std::wstring* preservedReason) {
  std::string bytes;
  if (!TryPathBytesFromWide(path, bytes) || bytes.empty() || bytes.front() != '/') {
    if (preservedReason != nullptr) {
      *preservedReason = L"操作目录路径无法无损表示，未尝试回收。";
    }
    return false;
  }
  bytes = TrimTrailingSlashes(bytes);
  const size_t slash = bytes.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    if (preservedReason != nullptr) {
      *preservedReason = L"操作目录路径缺少父目录，未尝试回收。";
    }
    return false;
  }
  const std::string parent = bytes.substr(0, slash);
  const std::string name = bytes.substr(slash + 1);
  if (name.empty() || !git::IsSafeOperationDirectoryName(Utf8ToWide(name))) {
    if (preservedReason != nullptr) {
      *preservedReason = L"操作目录名不符合本程序生成的形态，未尝试回收。";
    }
    return false;
  }
  const int parentFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parentFd < 0) {
    if (preservedReason != nullptr) {
      *preservedReason = L"无法打开操作目录的父目录：" + ErrnoText(errno);
    }
    return false;
  }
  struct ParentGuard {
    int fd;
    ~ParentGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } guard{parentFd};

  // 只清理“当前用户的临时根”下的目录：路径字符串可以被构造成任意位置，
  // 但 dev/inode 必须与 ResolveScratchDirectory() 打开的那个根一致。
  int rootFd = -1;
  std::string rootPath;
  std::wstring rootFailure;
  if (!OpenScratchRoot(&rootFd, &rootPath, &rootFailure)) {
    if (preservedReason != nullptr) {
      *preservedReason = rootFailure;
    }
    return false;
  }
  struct RootGuard {
    int fd;
    ~RootGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } rootGuard{rootFd};
  struct stat parentInfo {};
  struct stat rootInfo {};
  if (::fstat(parentFd, &parentInfo) != 0 || ::fstat(rootFd, &rootInfo) != 0 ||
      parentInfo.st_dev != rootInfo.st_dev || parentInfo.st_ino != rootInfo.st_ino) {
    if (preservedReason != nullptr) {
      *preservedReason = L"父目录不是本程序验证过的临时根，保留：" + Utf8ToWide(parent);
    }
    return false;
  }
  return TryReclaimOperationDirectory(parentFd, name, requireSpecConsumed, preservedReason);
}

bool IsOperationDirectoryOlderThan(int parentDirFd, std::string_view name, std::uint64_t seconds) {  const std::string nameBytes(name);
  struct stat info {};
  if (::fstatat(parentDirFd, nameBytes.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) {
    return false;
  }
  const std::time_t now = std::time(nullptr);
  if (now <= 0 || info.st_mtime <= 0 || info.st_mtime >= now) {
    return false;  // 时间戳不可信：宁可留给下一次，也不误删正在用的目录。
  }
  return static_cast<std::uint64_t>(now - info.st_mtime) >= seconds;
}

}  // namespace gc::platform
