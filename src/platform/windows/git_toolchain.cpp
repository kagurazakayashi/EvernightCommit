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

}  // namespace

std::vector<std::wstring> DiscoverGitCandidates() {
  return git::SearchPathForExecutable(GetPathVariable(), kGitExecutableName, MakeFsProbe());
}

std::wstring NormalizeGitExeInput(std::wstring_view input) {
  return git::ResolveExecutableInput(input, GetPathVariable(), MakeFsProbe());
}

GitExeVerification VerifyGitExe(std::wstring_view exePath, unsigned long timeoutMilliseconds) {
  GitExeVerification verification;
  verification.path = std::wstring(exePath);

  git::VersionProbeInput probeInput;
  probeInput.pathIsFile = !exePath.empty() && IsExistingRegularFile(exePath);
  if (probeInput.pathIsFile) {
    const SubprocessRunResult run =
        RunHiddenCaptured(exePath, {L"--version"}, /*workingDirectory=*/{}, timeoutMilliseconds);
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
