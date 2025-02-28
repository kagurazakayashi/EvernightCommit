#include "git/git_probe.h"

#include <cctype>

namespace gc::git {
namespace {

constexpr size_t kOutputSampleBytes = 200;

std::string TrimBytes(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  const auto isBlank = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (begin < end && isBlank(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && isBlank(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

bool HasPrefixFolded(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t index = 0; index < prefix.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(text[index])) !=
        std::tolower(static_cast<unsigned char>(prefix[index]))) {
      return false;
    }
  }
  return true;
}

std::string FirstNonBlankLine(std::string_view text) {
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t newline = text.find('\n', cursor);
    const std::string_view line =
        text.substr(cursor, newline == std::string_view::npos ? std::string_view::npos : newline - cursor);
    cursor = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;
    const std::string trimmed = TrimBytes(line);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return {};
}

}  // namespace

bool ParseGitVersionLine(std::string_view utf8, std::string& versionUtf8) {
  versionUtf8.clear();
  const std::string line = FirstNonBlankLine(utf8);
  if (!HasPrefixFolded(line, "git version")) {
    return false;
  }
  // "git version" 之后必须紧跟空白，避免把 "git versionfoo" 误判为有效。
  const std::string_view tail(line);
  const std::string_view rest = tail.substr(11);
  if (rest.empty() || (rest.front() != ' ' && rest.front() != '\t')) {
    return false;
  }
  versionUtf8 = TrimBytes(rest);
  return !versionUtf8.empty();
}

GitProbeResult VerifyGitVersionProbe(const VersionProbeInput& input) {
  GitProbeResult result;
  result.outputSampleUtf8 =
      std::string(input.utf8Output.substr(0, std::min(kOutputSampleBytes, input.utf8Output.size())));
  result.launchErrorText = input.launchErrorText;
  result.exitCode = input.exitCode;

  if (!input.pathIsFile) {
    result.outcome = GitProbeOutcome::fileMissing;
    return result;
  }
  if (!input.launched) {
    result.outcome = GitProbeOutcome::launchFailed;
    return result;
  }
  if (input.timedOut || !input.exited) {
    result.outcome = GitProbeOutcome::timedOut;
    return result;
  }
  if (input.exitCode != 0) {
    result.outcome = GitProbeOutcome::badExit;
    return result;
  }
  if (!ParseGitVersionLine(input.utf8Output, result.versionUtf8)) {
    result.outcome = GitProbeOutcome::notGitOutput;
    return result;
  }
  result.outcome = GitProbeOutcome::verified;
  return result;
}

std::wstring_view ProbeOutcomeSummary(GitProbeOutcome outcome) noexcept {
  switch (outcome) {
    case GitProbeOutcome::verified:
      return L"Git 程序验证通过";
    case GitProbeOutcome::fileMissing:
      return L"路径不是存在的可执行文件";
    case GitProbeOutcome::launchFailed:
      return L"无法启动该程序";
    case GitProbeOutcome::timedOut:
      return L"验证超时，程序未在限定时间内退出";
    case GitProbeOutcome::badExit:
      return L"程序以非 0 退出码结束";
    case GitProbeOutcome::notGitOutput:
      return L"该程序没有报告 Git 版本，可能不是 git.exe";
  }
  return L"未知验证结果";
}

}  // namespace gc::git
