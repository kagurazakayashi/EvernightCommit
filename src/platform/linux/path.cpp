#include "platform/path.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

std::wstring ErrnoText(int error) {
  // strerror 的文本受 locale 影响；这里只在显示路径使用，宽松转换即可（路径本身仍严格）。
  return L"errno " + std::to_wstring(error) + L"（" + Utf8ToWide(std::strerror(error)) + L"）";
}

bool ToPathBytes(std::wstring_view path, std::string& bytes, std::wstring* failureReason) {
  if (!TryPathBytesFromWide(path, bytes)) {
    if (failureReason != nullptr) {
      *failureReason = L"路径含有无法无损表示成 UTF-8 的字符，未发起任何文件系统操作。";
    }
    return false;
  }
  if (bytes.empty()) {
    if (failureReason != nullptr) {
      *failureReason = L"路径为空。";
    }
    return false;
  }
  if (bytes.find('\0') != std::string::npos) {
    if (failureReason != nullptr) {
      *failureReason = L"路径含有 NUL 字节，不是合法文件系统路径。";
    }
    return false;
  }
  return true;
}

PathProbeStatus StatusFromErrno(int error) {
  switch (error) {
    case ENOENT:
    case ENOTDIR:
      return PathProbeStatus::missing;
    case EACCES:
    case EPERM:
      return PathProbeStatus::inaccessible;
    default:
      return PathProbeStatus::failed;
  }
}

PathEntryKind KindFromMode(mode_t mode) {
  if (S_ISREG(mode)) {
    return PathEntryKind::regularFile;
  }
  if (S_ISDIR(mode)) {
    return PathEntryKind::directory;
  }
  if (S_ISLNK(mode)) {
    return PathEntryKind::symbolicLink;
  }
  return PathEntryKind::other;
}

std::wstring DescribeStatusFailure(PathProbeStatus status, int error, std::wstring_view path) {
  switch (status) {
    case PathProbeStatus::missing:
      return L"路径不存在：" + std::wstring(path);
    case PathProbeStatus::inaccessible:
      return L"权限不足，无法确认路径是否存在：" + std::wstring(path) + L"（" + ErrnoText(error) + L"）";
    case PathProbeStatus::unrepresentable:
      return L"路径无法无损表示：" + std::wstring(path);
    case PathProbeStatus::failed:
      return L"读取路径属性失败：" + std::wstring(path) + L"（" + ErrnoText(error) + L"）";
    case PathProbeStatus::ok:
      break;
  }
  return {};
}

bool AccessAllowed(const std::string& bytes, int mode) {
  // AT_EACCESS 按有效用户/组判断，与 Git 子进程实际能做什么一致。
  return ::faccessat(AT_FDCWD, bytes.c_str(), mode, AT_EACCESS) == 0;
}

// 从 base（绝对 Git 目录）构造一个痕迹路径。base 必须已通过严格转换。
std::string JoinMarker(const std::string& base, const char* name) {
  std::string result = base;
  if (!result.empty() && result.back() != '/') {
    result.push_back('/');
  }
  result.append(name);
  return result;
}

enum class MarkerAnswer { absent, present, unreadable };

MarkerAnswer AskMarker(const std::string& path, bool wantDirectory) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    const int error = errno;
    if (error == ENOENT || error == ENOTDIR) {
      return MarkerAnswer::absent;
    }
    return MarkerAnswer::unreadable;
  }
  const bool isDirectory = S_ISDIR(info.st_mode);
  // 符号链接也算“痕迹存在”：它可能让 Git 认为流程正在进行；宁可多拒绝一次，
  // 也不能把一条指向别处的链接当成“没有流程”。
  if (S_ISLNK(info.st_mode)) {
    return MarkerAnswer::present;
  }
  return isDirectory == wantDirectory ? MarkerAnswer::present : MarkerAnswer::absent;
}

}  // namespace

PathFacts ProbePath(std::wstring_view path, bool followFinalSymlink) {
  PathFacts facts;
  std::string bytes;
  if (!ToPathBytes(path, bytes, &facts.failureReason)) {
    facts.status = PathProbeStatus::unrepresentable;
    return facts;
  }

  struct stat linkInfo {};
  if (::lstat(bytes.c_str(), &linkInfo) != 0) {
    const int error = errno;
    facts.status = StatusFromErrno(error);
    facts.existence = facts.status == PathProbeStatus::missing ? PathExistence::missing : PathExistence::unknown;
    facts.failureReason = DescribeStatusFailure(facts.status, error, path);
    return facts;
  }

  facts.status = PathProbeStatus::ok;
  facts.existence = PathExistence::present;
  facts.kind = KindFromMode(linkInfo.st_mode);
  facts.isSymlink = S_ISLNK(linkInfo.st_mode);
  facts.permissions = static_cast<unsigned int>(linkInfo.st_mode & 07777u);
  facts.ownerIsCurrentUser = linkInfo.st_uid == ::geteuid();
  facts.device = static_cast<unsigned long long>(linkInfo.st_dev);
  facts.inode = static_cast<unsigned long long>(linkInfo.st_ino);

  struct stat targetInfo {};
  bool haveTarget = false;
  if (facts.isSymlink && !followFinalSymlink) {
    facts.targetExists = true;
    facts.targetKind = PathEntryKind::symbolicLink;
    facts.targetExistence = PathExistence::present;
    facts.targetDevice = facts.device;
    facts.targetInode = facts.inode;
    facts.readable = true;  // 链接本身可 lstat；是否可跟随由调用方决定。
    return facts;
  }

  if (facts.isSymlink) {
    if (::stat(bytes.c_str(), &targetInfo) == 0) {
      haveTarget = true;
    } else {
      const int error = errno;
      facts.targetKind = PathEntryKind::missing;
      facts.targetExistence = (error == ENOENT || error == ENOTDIR) ? PathExistence::missing
                                                                   : PathExistence::unknown;
      // 目标明确不存在与“目标权限不足/链接成环”不是一回事；后者仍要如实报告。
      if (facts.targetExistence == PathExistence::unknown) {
        facts.failureReason = L"符号链接目标无法读取：" + ErrnoText(error);
      }
    }
  } else {
    targetInfo = linkInfo;
    haveTarget = true;
  }

  if (haveTarget) {
    facts.targetExists = true;
    facts.targetExistence = PathExistence::present;
    facts.targetKind = KindFromMode(targetInfo.st_mode);
    facts.targetDevice = static_cast<unsigned long long>(targetInfo.st_dev);
    facts.targetInode = static_cast<unsigned long long>(targetInfo.st_ino);
    facts.readable = AccessAllowed(bytes, R_OK);
    facts.executable = facts.targetKind == PathEntryKind::regularFile &&
                       (targetInfo.st_mode & 0111u) != 0 && AccessAllowed(bytes, X_OK);
  }
  return facts;
}

bool IsExistingRegularFile(std::wstring_view path) {
  std::string bytes;
  if (!ToPathBytes(path, bytes, nullptr)) {
    return false;
  }
  struct stat info {};
  return ::stat(bytes.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool IsExistingDirectory(std::wstring_view path) {
  std::string bytes;
  if (!ToPathBytes(path, bytes, nullptr)) {
    return false;
  }
  struct stat info {};
  return ::stat(bytes.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsExecutableRegularFile(std::wstring_view path) {
  std::string bytes;
  if (!ToPathBytes(path, bytes, nullptr)) {
    return false;
  }
  struct stat info {};
  if (::stat(bytes.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return false;
  }
  if ((info.st_mode & 0111u) == 0) {
    return false;
  }
  return AccessAllowed(bytes, X_OK);
}

std::wstring ToAbsolutePath(std::wstring_view path) {
  std::string bytes;
  if (!ToPathBytes(path, bytes, nullptr)) {
    return {};
  }
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::absolute(std::filesystem::path(bytes), error).lexically_normal();
  if (error) {
    return {};
  }
  std::wstring result;
  if (!TryWidePathFromBytes(absolute.native(), result)) {
    return {};
  }
  // lexically_normal 可能给根目录留下结尾斜杠；其余路径去掉多余结尾斜杠（根 "/" 保留）。
  while (result.size() > 1 && result.back() == L'/') {
    result.pop_back();
  }
  return result;
}

std::wstring ToAbsolutePathInDirectory(std::wstring_view baseDirectory, std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  if (baseDirectory.empty() || IsAbsolutePath(path)) {
    return ToAbsolutePath(path);
  }
  std::string baseBytes;
  std::string pathBytes;
  if (!ToPathBytes(baseDirectory, baseBytes, nullptr) || !ToPathBytes(path, pathBytes, nullptr)) {
    return {};
  }
  std::string combined = baseBytes;
  if (combined.back() != '/') {
    combined.push_back('/');
  }
  combined += pathBytes;
  std::wstring wide;
  if (!TryWidePathFromBytes(combined, wide)) {
    return {};
  }
  return ToAbsolutePath(wide);
}

std::wstring NormalizePathSeparators(std::wstring_view path) {
  // Linux 上反斜杠是合法文件名字符，绝不能按 Windows 规则改写。
  return std::wstring(path);
}

bool IsAbsolutePath(std::wstring_view path) { return !path.empty() && path.front() == L'/'; }

std::wstring GetPathVariable() {
  const char* value = std::getenv("PATH");
  if (value == nullptr) {
    return {};
  }
  std::wstring decoded;
  if (!TryWidePathFromBytes(value, decoded)) {
    return {};  // 环境里的 PATH 不是 UTF-8：不猜码页，让候选发现明确地空着。
  }
  return decoded;
}

git::WorktreeFileFacts ProbeWorktreeFileForPreview(std::wstring_view absolutePath) {
  git::WorktreeFileFacts facts;
  if (absolutePath.empty()) {
    facts.failureReason = L"路径为空。";
    return facts;
  }
  const PathFacts probe = ProbePath(absolutePath);
  switch (probe.status) {
    case PathProbeStatus::missing:
      facts.probed = true;
      facts.exists = false;
      facts.failureReason = probe.failureReason;
      return facts;
    case PathProbeStatus::inaccessible:
      facts.probed = false;  // 权限不足时不能把“看不见”说成“不存在”。
      facts.exists = false;
      facts.failureReason = probe.failureReason;
      return facts;
    case PathProbeStatus::unrepresentable:
    case PathProbeStatus::failed:
      facts.probed = false;
      facts.exists = false;
      facts.failureReason = probe.failureReason;
      return facts;
    case PathProbeStatus::ok:
      break;
  }

  facts.probed = true;
  facts.exists = true;
  facts.isDirectory = probe.targetKind == PathEntryKind::directory;
  if (facts.isDirectory) {
    return facts;
  }
  if (probe.targetExistence != PathExistence::present) {
    facts.readable = false;
    if (probe.targetExistence == PathExistence::missing) {
      facts.failureReason = L"符号链接指向的目标不存在。";
    } else {
      facts.failureReason = probe.failureReason.empty()
                                ? L"无法确认符号链接目标（权限不足或链接成环）。"
                                : probe.failureReason;
    }
    return facts;
  }
  if (probe.targetKind != PathEntryKind::regularFile) {
    facts.readable = false;
    facts.failureReason = L"该路径不是普通文件（管道、套接字或设备没有可预览的文本内容）。";
    return facts;
  }

  std::string bytes;
  if (!ToPathBytes(absolutePath, bytes, &facts.failureReason)) {
    facts.probed = false;
    facts.exists = false;
    return facts;
  }
  const int descriptor = ::open(bytes.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) {
    const int error = errno;
    facts.readable = false;
    facts.failureReason = L"文件存在但无法打开读取（" + ErrnoText(error) + L"）。";
    return facts;
  }
  struct CloseGuard {
    int descriptor;
    ~CloseGuard() {
      if (descriptor >= 0) {
        ::close(descriptor);
      }
    }
  } guard{descriptor};

  struct stat info {};
  if (::fstat(descriptor, &info) != 0 || info.st_size < 0) {
    facts.readable = false;
    facts.failureReason = L"文件存在但读不出大小（" + ErrnoText(errno) + L"）。";
    return facts;
  }
  // 探测与打开之间路径可能被换掉：拿到句柄后再确认它仍是普通文件，避免把 FIFO/设备
  // 当成文本文件读（O_NONBLOCK 已保证打开本身不会挂在没有写者的管道上）。
  if (!S_ISREG(info.st_mode)) {
    facts.readable = false;
    facts.failureReason = L"该路径在打开时已不是普通文件，没有可预览的文本内容。";
    return facts;
  }
  facts.readable = true;
  facts.sizeBytes = static_cast<unsigned long long>(info.st_size);
  if (facts.sizeBytes == 0) {
    return facts;
  }

  std::vector<char> buffer(git::kBinaryProbeBytes);
  const size_t wanted =
      std::min<size_t>(buffer.size(), static_cast<size_t>(facts.sizeBytes));
  size_t got = 0;
  while (got < wanted) {
    const ssize_t count = ::read(descriptor, buffer.data() + got, wanted - got);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      facts.readable = false;
      facts.failureReason = L"文件存在但读不出内容（" + ErrnoText(errno) + L"）。";
      return facts;
    }
    if (count == 0) {
      break;
    }
    got += static_cast<size_t>(count);
  }
  facts.containsNullByte = std::find(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got), '\0') !=
                           buffer.begin() + static_cast<std::ptrdiff_t>(got);
  return facts;
}

git::WorkflowProbeResult ProbeRepositoryWorkflowStateStrict(std::wstring_view absoluteGitDir) {
  git::WorkflowProbeResult result;
  if (absoluteGitDir.empty()) {
    result.failure = L"没有可用的绝对 Git 目录，无从判断仓库里停没停着流程。";
    return result;
  }
  std::string base;
  std::wstring reason;
  if (!ToPathBytes(absoluteGitDir, base, &reason)) {
    result.failure = reason;
    return result;
  }
  struct stat info {};
  if (::lstat(base.c_str(), &info) != 0) {
    result.failure = L"读不到这个仓库的 Git 目录（" + ErrnoText(errno) + L"）。无从知道里面停没停着流程。";
    return result;
  }
  if (!S_ISDIR(info.st_mode)) {
    result.failure = L"指定的 Git 目录不是一个目录：" + std::wstring(absoluteGitDir);
    return result;
  }

  const auto refuse = [&result](const char* name) {
    result.readable = false;
    result.failure = std::wstring(L"读不到 Git 目录里的 ") + Utf8ToWide(name) +
                     L" 这一项，无法确定仓库里停着什么样的流程。";
  };
  const auto check = [&result, &refuse](const std::string& baseBytes, const char* name, bool wantDirectory,
                                        auto setter) {
    const MarkerAnswer answer = AskMarker(JoinMarker(baseBytes, name), wantDirectory);
    if (answer == MarkerAnswer::unreadable) {
      refuse(name);
      return false;
    }
    if (answer == MarkerAnswer::present) {
      setter(result.state);
    }
    return true;
  };

  const auto markMerge = [](git::RepositoryWorkflowState& state) { state.mergeInProgress = true; };
  const auto markRevert = [](git::RepositoryWorkflowState& state) { state.revertInProgress = true; };
  const auto markCherryPick = [](git::RepositoryWorkflowState& state) { state.cherryPickInProgress = true; };
  const auto markBisect = [](git::RepositoryWorkflowState& state) { state.bisectInProgress = true; };
  const auto markIndexLock = [](git::RepositoryWorkflowState& state) { state.indexLocked = true; };
  const auto markRebase = [](git::RepositoryWorkflowState& state) { state.rebaseInProgress = true; };

  const bool answered = check(base, "MERGE_HEAD", false, markMerge) &&
                        check(base, "REVERT_HEAD", false, markRevert) &&
                        check(base, "CHERRY_PICK_HEAD", false, markCherryPick) &&
                        check(base, "BISECT_LOG", false, markBisect) &&
                        check(base, "index.lock", false, markIndexLock) &&
                        check(base, "rebase-merge", true, markRebase) &&
                        check(base, "rebase-apply", true, markRebase);
  if (!answered) {
    return result;
  }
  result.readable = true;
  return result;
}

git::RepositoryWorkflowState ProbeRepositoryWorkflowState(std::wstring_view absoluteGitDir) {
  git::RepositoryWorkflowState state;
  if (absoluteGitDir.empty()) {
    return state;
  }
  std::string base;
  if (!ToPathBytes(absoluteGitDir, base, nullptr)) {
    return state;
  }
  const auto hasFile = [&base](const char* name) {
    return AskMarker(JoinMarker(base, name), false) == MarkerAnswer::present;
  };
  const auto hasDirectory = [&base](const char* name) {
    return AskMarker(JoinMarker(base, name), true) == MarkerAnswer::present;
  };
  state.mergeInProgress = hasFile("MERGE_HEAD");
  state.revertInProgress = hasFile("REVERT_HEAD");
  state.cherryPickInProgress = hasFile("CHERRY_PICK_HEAD");
  state.bisectInProgress = hasFile("BISECT_LOG");
  state.rebaseInProgress = hasDirectory("rebase-merge") || hasDirectory("rebase-apply");
  state.indexLocked = hasFile("index.lock");
  return state;
}

}  // namespace gc::platform
