#include "platform/git_toolchain.h"

#include <utility>

#include "platform/git_query.h"
#include "platform/text_encoding.h"

namespace gc::platform {

GitExeVerification VerifyGitExe(std::wstring_view exePath, unsigned long timeoutMilliseconds,
                                StopFlag cancel) {
  GitExeVerification verification;
  verification.path = std::wstring(exePath);

  const GitExecutableResolution resolution = ResolveGitExeInput(exePath);
  git::VersionProbeInput probeInput;
  probeInput.pathIsFile = resolution.resolved;
  if (!resolution.resolved) {
    probeInput.launchErrorText = resolution.failureReason;
  } else {
    verification.path = resolution.path;
    // 连 --version 也走集中环境策略：候选程序拿到的环境与后续每一次查询同源。
    const ProcessRunResult run =
        RunGitCaptured(resolution.path, {L"--version"}, /*workingDirectory=*/{},
                       timeoutMilliseconds, std::move(cancel), /*environmentNotice=*/nullptr);
    probeInput.launched = run.started;
    probeInput.launchErrorText = run.launchErrorText;
    probeInput.timedOut = run.timedOut || run.canceled;
    probeInput.exited = run.exited;
    probeInput.exitCode = static_cast<unsigned long>(run.exitCode);
    probeInput.outputComplete = run.AllStreamsComplete();
    probeInput.incompleteReason = DescribeIncompleteStreams(run);
    probeInput.utf8Output = run.stdoutCapture.bytes + run.stderrCapture.bytes;
  }

  const git::GitProbeResult probed = git::VerifyGitVersionProbe(probeInput);
  verification.outcome = probed.outcome;
  const std::wstring_view summary = git::ProbeOutcomeSummary(probed.outcome);

  switch (probed.outcome) {
    case git::GitProbeOutcome::verified:
      verification.version = Utf8ToWide(probed.versionUtf8);
      verification.message = std::wstring(summary) + L"：" + verification.path + L"（git version " +
                             verification.version + L"）";
      break;
    case git::GitProbeOutcome::fileMissing:
      verification.message = std::wstring(summary) + L"：" + verification.path;
      if (!resolution.failureReason.empty()) {
        verification.message += L"（" + resolution.failureReason + L"）";
      }
      break;
    case git::GitProbeOutcome::launchFailed:
      verification.message =
          std::wstring(summary) + L"：" + verification.path + L"（" + probed.launchErrorText + L"）";
      break;
    case git::GitProbeOutcome::timedOut:
      verification.message = std::wstring(summary) + L"（限定 " +
                             std::to_wstring(timeoutMilliseconds / 1000ULL) + L" 秒）：" +
                             verification.path;
      break;
    case git::GitProbeOutcome::badExit:
      verification.message = std::wstring(summary) + L"（退出码 " + std::to_wstring(probed.exitCode) +
                             L"）：" + verification.path;
      break;
    case git::GitProbeOutcome::notGitOutput:
      verification.message =
          std::wstring(summary) + L"：" + verification.path + L"（输出开头：" +
          Utf8ToWide(probed.outputSampleUtf8) + L"）";
      break;
    case git::GitProbeOutcome::incompleteOutput:
      verification.message = std::wstring(summary) + L"：" + verification.path + L"（" +
                             probed.incompleteReason + L"）";
      break;
  }
  return verification;
}

}  // namespace gc::platform
