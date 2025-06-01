#include "platform/windows/git_toolchain.h"

#include <utility>

#include "git/git_locator.h"
#include "platform/windows/git_query_result.h"
#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

constexpr const wchar_t* kGitExecutableName = L"git.exe";

git::FsProbe MakeFsProbe() {
  return git::FsProbe{
      [](const std::wstring& path) { return IsExistingRegularFile(path); },
      [](const std::wstring& path) { return ToAbsolutePath(path); },
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
  return !trimmed.empty() && trimmed.find(L'\\') == std::wstring_view::npos &&
         trimmed.find(L'/') == std::wstring_view::npos;
}

}  // namespace

std::vector<std::wstring> DiscoverGitCandidates() {
  // 收集按 PATH 顺序，交给界面之前按档位排一次：cmd 入口在最前，也就是下拉列表的第一项与默认值；
  // msys / usr\bin 那一套留在列表末尾，可用但不抢默认。
  auto candidates = git::SearchPathForExecutable(GetPathVariable(), kGitExecutableName, MakeFsProbe());
  git::SortGitCandidatesByPreference(candidates);
  return candidates;
}

std::wstring NormalizeGitExeInput(std::wstring_view input) {
  return git::ResolveExecutableInput(input, GetPathVariable(), MakeFsProbe());
}

GitExecutableResolution ResolveGitExeInput(std::wstring_view input) {
  GitExecutableResolution result;
  const bool bareName = LooksLikeBareName(input);
  result.path = NormalizeGitExeInput(input);
  if (result.path.empty()) {
    result.failureReason = L"没有填写 Git 程序路径。";
    return result;
  }
  if (bareName && !IsAbsolutePath(result.path)) {
    // PATH 没命中时 NormalizeGitExeInput 会原样返回裸名；不能拿它去探测当前目录，
    // 否则“PATH 查找失败”会悄悄变成“执行工作目录里的同名文件”。
    result.failureReason = L"PATH 中没有找到名为 " + result.path + L" 的可执行文件。";
    return result;
  }
  result.exists = IsExistingRegularFile(result.path);
  if (!result.exists) {
    result.failureReason = L"这个路径不是存在的普通文件：" + result.path;
    return result;
  }
  // Windows 没有 POSIX 执行位；本工程要求能由 CreateProcessW 直接启动的可执行文件。
  result.executable = IsExecutableRegularFile(result.path);
  if (!result.executable) {
    result.failureReason = L"这个文件没有可执行扩展名（.exe/.com）：" + result.path;
    return result;
  }
  result.resolved = true;
  return result;
}

GitExeVerification VerifyGitExe(std::wstring_view exePath, unsigned long timeoutMilliseconds) {
  GitExeVerification verification;
  verification.path = std::wstring(exePath);

  git::VersionProbeInput probeInput;
  probeInput.pathIsFile = !exePath.empty() && IsExistingRegularFile(exePath);
  if (probeInput.pathIsFile) {
    // 连 --version 也走集中环境策略：候选程序拿到的环境与后续每一次查询同源。
    const SubprocessRunResult run =
        RunGitCaptured(exePath, {L"--version"}, /*workingDirectory=*/{}, timeoutMilliseconds,
                       /*environmentNotice=*/nullptr);
    probeInput.launched = run.started;
    probeInput.launchErrorText = run.launchErrorText;
    probeInput.timedOut = run.timedOut;
    probeInput.exited = run.exited;
    probeInput.exitCode = run.exitCode;
    probeInput.outputComplete = run.AllStreamsComplete();
    probeInput.incompleteReason = DescribeIncompleteStreams(run);
    probeInput.utf8Output = run.stdoutCapture.bytes + run.stderrCapture.bytes;
  }

  const git::GitProbeResult probed = git::VerifyGitVersionProbe(probeInput);
  verification.outcome = probed.outcome;
  const std::wstring_view summary = git::ProbeOutcomeSummary(probed.outcome);

  switch (probed.outcome) {
    case git::GitProbeOutcome::verified:
      verification.version = Utf8ToUtf16(probed.versionUtf8);
      verification.message = std::wstring(summary) + L"：" + verification.path + L"（git version " +
                             verification.version + L"）";
      break;
    case git::GitProbeOutcome::fileMissing:
      verification.message = std::wstring(summary) + L"：" + verification.path;
      break;
    case git::GitProbeOutcome::launchFailed:
      verification.message =
          std::wstring(summary) + L"：" + verification.path + L"（" + probed.launchErrorText + L"）";
      break;
    case git::GitProbeOutcome::timedOut:
      verification.message = std::wstring(summary) + L"（限定 " +
                             std::to_wstring(timeoutMilliseconds / 1000ULL) + L" 秒）：" + verification.path;
      break;
    case git::GitProbeOutcome::badExit:
      verification.message = std::wstring(summary) + L"（退出码 " + std::to_wstring(probed.exitCode) +
                             L"）：" + verification.path;
      break;
    case git::GitProbeOutcome::notGitOutput:
      verification.message =
          std::wstring(summary) + L"：" + verification.path + L"（输出开头：" +
          Utf8ToUtf16(probed.outputSampleUtf8) + L"）";
      break;
    case git::GitProbeOutcome::incompleteOutput:
      verification.message = std::wstring(summary) + L"：" + verification.path + L"（" +
                             probed.incompleteReason + L"）";
      break;
  }
  return verification;
}

}  // namespace gc::platform
