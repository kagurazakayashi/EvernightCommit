#include "platform/windows/win_path.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "platform/windows/raii.h"

namespace gc::platform {
namespace {

std::wstring CollapseWithFullPathName(std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  const std::wstring value(path);
  const DWORD needed = ::GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
  if (needed == 0 || needed > 32768) {
    return {};
  }
  std::wstring buffer(needed, L'\0');
  const DWORD written = ::GetFullPathNameW(value.c_str(), needed, buffer.data(), nullptr);
  if (written == 0 || written >= needed) {
    return {};
  }
  buffer.resize(written);
  return buffer;
}

}  // namespace

bool IsExistingRegularFile(std::wstring_view path) {
  if (path.empty()) {
    return false;
  }
  const std::wstring value(path);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  // 目录与不存在都视为不可用；能否执行由后续 `--version` 探测给出确切原因。
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsExistingDirectory(std::wstring_view path) {
  if (path.empty()) {
    return false;
  }
  const std::wstring value(path);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ToAbsolutePath(std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  const std::wstring normalized = NormalizePathSeparators(path);
  return CollapseWithFullPathName(normalized);
}

std::wstring ToAbsolutePathInDirectory(std::wstring_view baseDirectory, std::wstring_view path) {
  if (path.empty()) {
    return {};
  }
  const std::wstring normalized = NormalizePathSeparators(path);
  if (baseDirectory.empty() || IsAbsolutePath(normalized)) {
    return CollapseWithFullPathName(normalized);
  }
  const std::wstring base = CollapseWithFullPathName(NormalizePathSeparators(baseDirectory));
  if (base.empty()) {
    return CollapseWithFullPathName(normalized);
  }
  std::wstring combined(base);
  if (combined.back() != L'\\') {
    combined.push_back(L'\\');
  }
  // 组合后再折叠，"D:\repo" + "..\repo2" 这类相对段才能正确上溯。
  combined.append(normalized);
  const std::wstring collapsed = CollapseWithFullPathName(combined);
  return collapsed.empty() ? normalized : collapsed;
}

std::wstring NormalizePathSeparators(std::wstring_view path) {
  std::wstring normalized(path);
  for (wchar_t& c : normalized) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  return normalized;
}

bool IsAbsolutePath(std::wstring_view path) {
  if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
    return true;  // UNC：\\server\share
  }
  if (path.size() < 3) {
    return false;
  }
  const bool driveLetter = (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
  return driveLetter && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring GetPathVariable() {
  const DWORD needed = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring buffer(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(L"PATH", buffer.data(), needed);
  if (written == 0 || written >= needed) {
    return {};
  }
  buffer.resize(written);
  return buffer;
}

git::WorktreeFileFacts ProbeWorktreeFileForPreview(std::wstring_view absolutePath) {
  git::WorktreeFileFacts facts;
  if (absolutePath.empty()) {
    facts.failureReason = L"路径为空。";
    return facts;  // probed 保持 false：连要探测哪个路径都不知道，一律按不可用处理。
  }
  const std::wstring value(absolutePath);
  const DWORD attributes = ::GetFileAttributesW(value.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    // GetFileAttributesW 失败时，「为什么失败」决定了能不能对存在性下结论：
    // 只有「明确找不到」这两个错误码才是「不存在」；权限、网络、设备、名称非法等
    // 都答不了这个问题，必须按「读不到属性」返回，绝不能伪装成文件消失了，也不能
    // 反过来把「不存在」标成存在（此前正是这个反向判读）。
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      facts.probed = true;
      facts.exists = false;
      facts.failureReason = error == ERROR_FILE_NOT_FOUND
                                ? L"该路径下没有这个文件（Windows 错误码 2）。"
                                : L"该路径的上级目录就不存在（Windows 错误码 3）。";
      return facts;
    }
    facts.probed = false;  // 无法判定：存在与否都未知。
    facts.exists = false;
    facts.failureReason = L"读取文件属性失败（Windows 错误码 " + std::to_wstring(error) + L"）。";
    return facts;
  }
  facts.probed = true;
  facts.exists = true;
  facts.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (facts.isDirectory) {
    return facts;
  }

  // 目录資訊之外還要確認「打得開、讀得動」：僅有存在性不足以判斷 Git 会不会报
  // “Could not access”，而那個錯誤的退出碼與「有差異」完全相同，只能在这里提前问清楚。
  const HANDLE file =
      ::CreateFileW(value.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    facts.readable = false;
    facts.failureReason = L"无法打开该文件用于读取（Windows 错误码 " +
                          std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return facts;
  }
  const UniqueHandle guard(file);
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(guard.get(), &size) == 0 || size.QuadPart < 0) {
    facts.readable = false;
    facts.failureReason = L"无法取得该文件的大小（Windows 错误码 " +
                          std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return facts;
  }
  facts.readable = true;
  facts.sizeBytes = static_cast<unsigned long long>(size.QuadPart);
  if (facts.sizeBytes == 0) {
    return facts;  // 空文件沒有採樣可讀，按文字（零內容）對待。
  }

  std::vector<char> bytes(git::kBinaryProbeBytes);
  DWORD got = 0;
  const DWORD want =
      static_cast<DWORD>(std::min<size_t>(bytes.size(), static_cast<size_t>(facts.sizeBytes)));
  if (::ReadFile(guard.get(), bytes.data(), want, &got, nullptr) == 0) {
    // 大小拿到了但讀不出一個字節：視為不可讀，宁可不開窗口也不讓窗口里出現一句看不懂的話。
    facts.readable = false;
    facts.failureReason = L"文件存在但读不出内容（Windows 错误码 " +
                          std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）。";
    return facts;
  }
  facts.containsNullByte = std::find(bytes.begin(), bytes.begin() + got, '\0') != (bytes.begin() + got);
  return facts;
}

git::WorkflowProbeResult ProbeRepositoryWorkflowStateStrict(std::wstring_view absoluteGitDir) {
  // 逐个痕迹问「在 / 明确不在 / 问不成」。只有前两种是答案；第三种一次都不许被当成「没有」。
  // 判定依据与上面的宽松版本同源（GetFileAttributesW），区别只在对失败的处理。
  git::WorkflowProbeResult result;
  if (absoluteGitDir.empty()) {
    result.failure = L"没有可用的绝对 Git 目录，无从判断仓库里停没停着流程。";
    return result;
  }
  std::wstring base(absoluteGitDir);
  if (base.back() != L'\\' && base.back() != L'/') {
    base.push_back(L'\\');
  }
  // Git 目录本身要读得动：连它都不在（或访问不了），六个痕迹就一个都没问过。
  const DWORD directoryAttributes = ::GetFileAttributesW(base.c_str());
  if (directoryAttributes == INVALID_FILE_ATTRIBUTES) {
    result.failure = L"读不到这个仓库的 Git 目录（Windows 错误码 " +
                     std::to_wstring(static_cast<unsigned long>(::GetLastError())) +
                     L"）。无从知道里面停没停着流程。";
    return result;
  }

  enum class MarkerAnswer { absent, present, unreadable };
  const auto ask = [&base](const wchar_t* name, bool wantDirectory) -> MarkerAnswer {
    const std::wstring path = base + name;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      // 与预览探测同一套口径：只有「明确找不到」才是「不在」，其余都是「问不成」。
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return MarkerAnswer::absent;
      }
      return MarkerAnswer::unreadable;
    }
    const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return isDirectory == wantDirectory ? MarkerAnswer::present : MarkerAnswer::absent;
  };
  const auto refuse = [&result](const wchar_t* name) {
    result.readable = false;
    result.failure =
        std::wstring(L"读不到 Git 目录里的 ") + name + L" 这一项，无法确定仓库里停着什么样的流程。";
  };

  // 逐项判定并直接落到 state 的对应字段上（不用成员指针：MSVC 对限定名成员指针的
  // 声明写法接受度差，直接赋值最好读）。
  auto check = [&ask, &result, &refuse](const wchar_t* name, bool wantDirectory,
                                         auto setter) {
    const MarkerAnswer answer = ask(name, wantDirectory);
    if (answer == MarkerAnswer::unreadable) {
      refuse(name);
      return false;
    }
    if (answer == MarkerAnswer::present) {
      setter(result.state);
    }
    return true;
  };
  const auto markMerge = [](git::RepositoryWorkflowState& s) { s.mergeInProgress = true; };
  const auto markRevert = [](git::RepositoryWorkflowState& s) { s.revertInProgress = true; };
  const auto markCherryPick = [](git::RepositoryWorkflowState& s) { s.cherryPickInProgress = true; };
  const auto markBisect = [](git::RepositoryWorkflowState& s) { s.bisectInProgress = true; };
  const auto markIndexLock = [](git::RepositoryWorkflowState& s) { s.indexLocked = true; };
  const auto markRebase = [](git::RepositoryWorkflowState& s) { s.rebaseInProgress = true; };
  const bool allAnswered =
      check(L"MERGE_HEAD", false, markMerge) && check(L"REVERT_HEAD", false, markRevert) &&
      check(L"CHERRY_PICK_HEAD", false, markCherryPick) && check(L"BISECT_LOG", false, markBisect) &&
      check(L"index.lock", false, markIndexLock) && check(L"rebase-merge", true, markRebase) &&
      check(L"rebase-apply", true, markRebase);
  if (!allAnswered) {
    return result;  // refuse() 已经写好 failure，readable 保持 false。
  }
  result.readable = true;
  return result;
}

git::RepositoryWorkflowState ProbeRepositoryWorkflowState(std::wstring_view absoluteGitDir) {
  git::RepositoryWorkflowState state;
  if (absoluteGitDir.empty()) {
    return state;  // 没有可读的落点：按「没有痕迹」返回，判断交给 Git 自己。
  }
  std::wstring base(absoluteGitDir);
  if (!base.empty() && base.back() != L'\\' && base.back() != L'/') {
    base.push_back(L'\\');
  }
  const auto hasFile = [&base](const wchar_t* name) {
    return IsExistingRegularFile(base + name);
  };
  const auto hasDirectory = [&base](const wchar_t* name) {
    return IsExistingDirectory(base + name);
  };
  state.mergeInProgress = hasFile(L"MERGE_HEAD");
  state.revertInProgress = hasFile(L"REVERT_HEAD");
  state.cherryPickInProgress = hasFile(L"CHERRY_PICK_HEAD");
  state.bisectInProgress = hasFile(L"BISECT_LOG");
  state.rebaseInProgress = hasDirectory(L"rebase-merge") || hasDirectory(L"rebase-apply");
  state.indexLocked = hasFile(L"index.lock");
  return state;
}

}  // namespace gc::platform
