#include "git/git_locator.h"

#include <algorithm>
#include <cctype>

namespace gc::git {
namespace {

// Windows 文件系统路径大小写不敏感（对 ASCII 而言）；纯逻辑层只做 ASCII 折叠，
// 避免依赖平台排序函数，测试环境结果可复现。
wchar_t FoldAscii(wchar_t c) noexcept {
  if (c >= L'A' && c <= L'Z') {
    return static_cast<wchar_t>(c - L'A' + L'a');
  }
  return c;
}

bool EqualsFolded(const std::wstring& a, const std::wstring& b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t index = 0; index < a.size(); ++index) {
    if (FoldAscii(a[index]) != FoldAscii(b[index])) {
      return false;
    }
  }
  return true;
}

void AppendUnique(std::vector<std::wstring>& out, std::wstring candidate) {
  if (candidate.empty()) {
    return;
  }
  if (std::none_of(out.begin(), out.end(), [&candidate](const std::wstring& existing) {
        return EqualsFolded(existing, candidate);
      })) {
    out.push_back(std::move(candidate));
  }
}

std::wstring TrimWide(std::wstring_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
    --end;
  }
  return std::wstring(text.substr(begin, end - begin));
}

bool HasSeparator(std::wstring_view path) noexcept {
  return path.find(L'\\') != std::wstring_view::npos || path.find(L'/') != std::wstring_view::npos;
}

// 按目录分隔符切成段，丢掉空段（连续分隔符、结尾分隔符）；不区分大小写由调用方折叠完成。
std::vector<std::wstring> SplitPathSegments(std::wstring_view path) {
  std::vector<std::wstring> segments;
  size_t cursor = 0;
  while (cursor <= path.size()) {
    const size_t separator = path.find_first_of(L"\\/", cursor);
    const std::wstring_view piece =
        path.substr(cursor, separator == std::wstring_view::npos ? std::wstring_view::npos : separator - cursor);
    if (!piece.empty()) {
      segments.emplace_back(piece);
    }
    if (separator == std::wstring_view::npos) {
      break;
    }
    cursor = separator + 1;
  }
  return segments;
}

std::wstring FoldSegment(std::wstring_view segment) {
  std::wstring folded(segment);
  for (wchar_t& c : folded) {
    c = FoldAscii(c);
  }
  return folded;
}

std::wstring CombineDirAndFile(std::wstring_view dir, std::wstring_view file) {
  std::wstring result(dir);
  if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
    result.push_back(L'\\');
  }
  result.append(file);
  return result;
}

}  // namespace

std::vector<std::wstring> SearchPathForExecutable(std::wstring_view pathEnv, std::wstring_view exeName,
                                                  const FsProbe& fs) {
  std::vector<std::wstring> found;
  size_t cursor = 0;
  while (cursor <= pathEnv.size()) {
    const size_t separator = pathEnv.find(L';', cursor);
    const std::wstring_view entry =
        pathEnv.substr(cursor, separator == std::wstring_view::npos ? std::wstring_view::npos : separator - cursor);
    cursor = (separator == std::wstring_view::npos) ? pathEnv.size() + 1 : separator + 1;

    // PATH 中空条目按约定代表当前目录；扫描整盘与隐式当前目录都不是本功能的行为，直接跳过。
    std::wstring dir = TrimWide(entry);
    if (dir.empty()) {
      continue;
    }
    AppendUnique(found, fs.toAbsolute(CombineDirAndFile(dir, exeName)));
  }
  // 只保留真实存在的文件；过滤放在收集后，使 toAbsolute 的失败回退（空串）也一并剔除。
  std::erase_if(found, [&fs](const std::wstring& candidate) { return candidate.empty() || !fs.fileExists(candidate); });
  return found;
}

GitCandidateFamily ClassifyGitCandidate(std::wstring_view path) {
  const std::vector<std::wstring> segments = SplitPathSegments(path);
  // 按「段」判断而不是按整条子串判断：C:\Program Files\Git\cmd 里的 "Git" 不该被当成 msys，
  // 而 C:\tools\msys64\usr\bin 与 C:\msys64\...\git.exe 都靠 msys64 这一段认出来。
  const bool inMsysTree = std::any_of(segments.begin(), segments.end(), [](const std::wstring& segment) {
    return FoldSegment(segment).find(L"msys") != std::wstring::npos;
  });
  if (inMsysTree) {
    return GitCandidateFamily::msys;
  }
  if (segments.size() >= 2) {
    const std::wstring parent = FoldSegment(segments[segments.size() - 2]);
    if (parent == L"cmd") {
      return GitCandidateFamily::cmdShim;
    }
    // <...>\usr\bin\git.exe：MSYS 运行时那一套（同目录就有 msys-2.0.dll），哪怕它装在 Git for
    // Windows 自己的目录里，也不是给命令行直用的那一个。
    if (parent == L"bin" && segments.size() >= 3 &&
        FoldSegment(segments[segments.size() - 3]) == L"usr") {
      return GitCandidateFamily::msys;
    }
  }
  return GitCandidateFamily::other;
}

void SortGitCandidatesByPreference(std::vector<std::wstring>& candidates) {
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const std::wstring& left, const std::wstring& right) {
                     return ClassifyGitCandidate(left) < ClassifyGitCandidate(right);
                   });
}

std::wstring ResolveExecutableInput(std::wstring_view input, std::wstring_view pathEnv, const FsProbe& fs) {
  const std::wstring trimmed = TrimWide(input);
  if (trimmed.empty()) {
    return {};
  }
  if (!HasSeparator(trimmed)) {
    // 裸名字（"git"）补上 .exe 后按 PATH 解析；找不到就原样返回，由后续验证步骤报告具体原因。
    std::wstring name = trimmed;
    if (name.find(L'.') == std::wstring::npos) {
      name += L".exe";
    }
    auto candidates = SearchPathForExecutable(pathEnv, name, fs);
    // 用户只输了名字时没有表达过偏好，那就按档位挑：cmd 入口优先，msys 那套垫后。
    SortGitCandidatesByPreference(candidates);
    return candidates.empty() ? trimmed : candidates.front();
  }
  const std::wstring absolute = fs.toAbsolute(trimmed);
  return absolute.empty() ? trimmed : absolute;
}

}  // namespace gc::git
