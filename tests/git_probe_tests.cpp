#include "support/tiny_test.h"

#include <string>

#include "git/git_probe.h"

namespace {

gc::git::VersionProbeInput LaunchedOk(std::string output) {
  gc::git::VersionProbeInput input;
  input.pathIsFile = true;
  input.launched = true;
  input.exited = true;
  input.exitCode = 0;
  input.utf8Output = std::move(output);
  return input;
}

}  // namespace

GC_TEST(probe_accepts_real_git_version_line) {
  const auto result = gc::git::VerifyGitVersionProbe(LaunchedOk("git version 2.55.0.windows.5\r\n"));

  GC_CHECK(result.outcome == gc::git::GitProbeOutcome::verified);
  GC_CHECK(result.versionUtf8 == "2.55.0.windows.5");
}

GC_TEST(probe_skips_leading_blank_lines) {
  const auto result = gc::git::VerifyGitVersionProbe(LaunchedOk("\r\n\ngit version 2.39.3.windows.2\n"));

  GC_CHECK(result.outcome == gc::git::GitProbeOutcome::verified);
  GC_CHECK(result.versionUtf8 == "2.39.3.windows.2");
}

GC_TEST(probe_rejects_non_git_output) {
  const auto wrong = gc::git::VerifyGitVersionProbe(LaunchedOk("7-Zip [64] 21.07"));
  GC_CHECK(wrong.outcome == gc::git::GitProbeOutcome::notGitOutput);
  GC_CHECK(wrong.outputSampleUtf8.find("7-Zip") != std::string::npos);

  const auto empty = gc::git::VerifyGitVersionProbe(LaunchedOk(""));
  GC_CHECK(empty.outcome == gc::git::GitProbeOutcome::notGitOutput);

  // “git versionfoo”不是版本行。
  const auto tricky = gc::git::VerifyGitVersionProbe(LaunchedOk("git versionfoo 1.0"));
  GC_CHECK(tricky.outcome == gc::git::GitProbeOutcome::notGitOutput);

  // 只有“git version”没有版本数字也不行。
  const auto bare = gc::git::VerifyGitVersionProbe(LaunchedOk("git version"));
  GC_CHECK(bare.outcome == gc::git::GitProbeOutcome::notGitOutput);
}

GC_TEST(probe_classifies_failure_stages_in_order) {
  // 文件不存在优先于其他一切判定（不应启动进程）。
  gc::git::VersionProbeInput missing;
  missing.pathIsFile = false;
  GC_CHECK(gc::git::VerifyGitVersionProbe(missing).outcome == gc::git::GitProbeOutcome::fileMissing);

  gc::git::VersionProbeInput launch = LaunchedOk("");
  launch.launched = false;
  launch.launchErrorText = L"不是有效的 Win32 应用程序。";
  const auto launchedFail = gc::git::VerifyGitVersionProbe(launch);
  GC_CHECK(launchedFail.outcome == gc::git::GitProbeOutcome::launchFailed);
  GC_CHECK(launchedFail.launchErrorText == L"不是有效的 Win32 应用程序。");

  gc::git::VersionProbeInput timeout = LaunchedOk("");
  timeout.timedOut = true;
  timeout.exited = false;
  GC_CHECK(gc::git::VerifyGitVersionProbe(timeout).outcome == gc::git::GitProbeOutcome::timedOut);

  gc::git::VersionProbeInput badExit = LaunchedOk("git version 2.55.0.windows.5");
  badExit.exitCode = 3010;
  GC_CHECK(gc::git::VerifyGitVersionProbe(badExit).outcome == gc::git::GitProbeOutcome::badExit);
}

GC_TEST(probe_truncates_output_sample) {
  const std::string huge(100000, 'x');
  const auto result = gc::git::VerifyGitVersionProbe(LaunchedOk(huge));

  GC_CHECK(result.outcome == gc::git::GitProbeOutcome::notGitOutput);
  GC_CHECK(result.outputSampleUtf8.size() <= 200);
}

GC_TEST(probe_outcome_summaries_exist_for_every_case) {
  using gc::git::GitProbeOutcome;
  for (GitProbeOutcome outcome : {GitProbeOutcome::verified, GitProbeOutcome::fileMissing,
                                  GitProbeOutcome::launchFailed, GitProbeOutcome::timedOut,
                                  GitProbeOutcome::badExit, GitProbeOutcome::notGitOutput}) {
    GC_CHECK(!gc::git::ProbeOutcomeSummary(outcome).empty());
  }
}
