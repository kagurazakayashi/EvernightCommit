#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "git/git_locator.h"
#include "platform/git_discovery.h"
#include "platform/path.h"
#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

constexpr const wchar_t* kGitExecutableName = L"git";

git::FsProbe MakeFsProbe() {
  return git::FsProbe{
      [](const std::wstring& path) { return IsExistingRegularFile(path); },
      [](const std::wstring& path) { return ToAbsolutePath(path); },
      [](const std::wstring& path) { return IsExecutableRegularFile(path); },
  };
}

bool LooksLikeBareName(std::wstring_view input) {
  size_t begin = 0;
  size_t end = input.size();
  while (begin < end && (input[begin] == L' ' || input[begin] == L'\t')) {
    ++begin;
  }
  while (end > begin && (input[end - 1] == L' ' || input[end - 1] == L'\t')) {
    --end;
  }
  const std::wstring_view trimmed = input.substr(begin, end - begin);
  return !trimmed.empty() && trimmed.find(L'/') == std::wstring_view::npos;
}

}  // namespace

std::vector<std::wstring> DiscoverGitCandidates() {
  auto candidates =
      git::SearchPathForExecutable(GetPathVariable(), kGitExecutableName, MakeFsProbe(), git::PathFlavor::posix);
  // POSIX 没有 cmd/msys 档位；保留 PATH 顺序（稳定排序在 posix 下是无操作）。
  git::SortGitCandidatesByPreference(candidates, git::PathFlavor::posix);
  return candidates;
}

std::wstring NormalizeGitExeInput(std::wstring_view input) {
  return git::ResolveExecutableInput(input, GetPathVariable(), MakeFsProbe(), git::PathFlavor::posix);
}

GitExecutableResolution ResolveGitExeInput(std::wstring_view input) {
  GitExecutableResolution result;
  const bool bareName = LooksLikeBareName(input);
  if (bareName) {
    const char* rawPath = std::getenv("PATH");
    if (rawPath != nullptr && !IsValidUtf8(rawPath)) {
      // 裸名必须查 PATH；PATH 是路径字节，不能替换成 U+FFFD 后去搜索别的位置。
      result.failureReason = L"PATH 环境变量不是有效 UTF-8，无法无损解析候选路径。";
      return result;
    }
  }
  result.path = NormalizeGitExeInput(input);
  if (result.path.empty()) {
    result.failureReason = L"没有填写 Git 程序路径。";
    return result;
  }
  if (bareName && !IsAbsolutePath(result.path)) {
    // PATH 没命中时 NormalizeGitExeInput 会原样返回裸名；绝不能拿它去探测当前目录，
    // 否则“PATH 查找失败”会悄悄变成“执行工作目录里的同名文件”。
    result.failureReason = L"PATH 中没有找到名为 " + result.path + L" 的可执行文件。";
    return result;
  }

  const PathFacts facts = ProbePath(result.path);
  switch (facts.status) {
    case PathProbeStatus::missing:
      result.failureReason = bareName ? L"PATH 中没有找到名为 " + result.path + L" 的可执行文件。"
                                      : L"这个路径不存在：" + result.path;
      return result;
    case PathProbeStatus::inaccessible:
      result.failureReason = L"权限不足，无法确认这个路径是否可用：" + result.path;
      return result;
    case PathProbeStatus::unrepresentable:
    case PathProbeStatus::failed:
      result.failureReason = facts.failureReason;
      return result;
    case PathProbeStatus::ok:
      break;
  }

  result.exists = facts.targetExists && facts.targetKind == PathEntryKind::regularFile;
  if (!result.exists) {
    if (facts.targetExistence == PathExistence::unknown) {
      result.failureReason = L"无法确认这个路径（权限不足或符号链接错误）：" + result.path;
    } else if (facts.targetKind == PathEntryKind::directory) {
      result.failureReason = L"这个路径是一个目录，不是 Git 可执行文件：" + result.path;
    } else {
      result.failureReason = L"这个路径不是普通文件：" + result.path;
    }
    return result;
  }

  result.executable = facts.executable;
  if (!result.executable) {
    result.failureReason = L"这个文件没有执行权限（缺少执行位），不能作为 Git 程序启动：" + result.path;
    return result;
  }
  result.resolved = true;
  return result;
}

}  // namespace gc::platform
