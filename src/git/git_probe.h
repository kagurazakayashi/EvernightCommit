#pragma once

#include <string>
#include <string_view>

namespace gc::git {

// `<候选程序> --version` 探测结果的分类。执行层（平台代码）负责填装输入，
// 这里只做与 Win32 无关的判定，便于纯逻辑测试。
enum class GitProbeOutcome {
  verified,      // 程序报告了 Git 版本号
  fileMissing,   // 输入路径不是存在的普通文件
  launchFailed,  // 进程无法启动（CreateProcessW 失败）
  timedOut,      // 超时未退出
  badExit,       // 退出码非 0
  notGitOutput,  // 正常退出但输出不是 "git version ..."
};

struct VersionProbeInput {
  bool pathIsFile = false;
  bool launched = false;
  bool timedOut = false;
  bool exited = false;
  unsigned long exitCode = 0;
  std::string utf8Output;       // stdout+stderr 原始字节
  std::wstring launchErrorText; // 启动失败说明（已是宽字符，仅透传）
};

struct GitProbeResult {
  GitProbeOutcome outcome = GitProbeOutcome::fileMissing;
  std::string versionUtf8;    // 仅 verified 时非空，如 "2.45.0.windows.1"
  std::string outputSampleUtf8; // 输出开头片段（截断），用于失败诊断
  std::wstring launchErrorText;
  unsigned long exitCode = 0;
};

// 判定顺序：文件不存在 → 启动失败 → 超时 → 退出码非 0 → 输出校验。
GitProbeResult VerifyGitVersionProbe(const VersionProbeInput& input);

// 从输出首个非空行解析 "git version <版本号>"；行尾 \r 与空白会被清理。
bool ParseGitVersionLine(std::string_view utf8, std::string& versionUtf8);

// 各分类的中文基础说明（不含路径、退出码等动态细节，由展示层拼接）。
std::wstring_view ProbeOutcomeSummary(GitProbeOutcome outcome) noexcept;

}  // namespace gc::git
