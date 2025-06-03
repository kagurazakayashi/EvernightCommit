#include "platform/terminal_adapters.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace gc::platform {
namespace {

bool HasControlOrNul(std::wstring_view text) {
  for (const wchar_t c : text) {
    if (c == L'\0' || c < 0x20 || c == 0x7F) {
      return true;
    }
  }
  return false;
}

bool IsShellSimpleToken(std::wstring_view token) {
  if (token.empty()) {
    return false;
  }
  for (const wchar_t c : token) {
    const bool asciiAlnum = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                            (c >= L'0' && c <= L'9');
    if (asciiAlnum || c == L'.' || c == L'_' || c == L'/' || c == L'+' || c == L'-' ||
        c == L':' || c == L',' || c == L'=' || c == L'@') {
      continue;
    }
    return false;
  }
  return true;
}

TerminalLaunchPlan Fail(TerminalAdapterKind kind, std::wstring reason) {
  TerminalLaunchPlan plan;
  plan.kind = kind;
  plan.failureReason = std::move(reason);
  return plan;
}

}  // namespace

bool IsKnownTerminalAdapter(TerminalAdapterKind kind) noexcept {
  switch (kind) {
    case TerminalAdapterKind::windowsConsole:
    case TerminalAdapterKind::xterm:
    case TerminalAdapterKind::xfce4Terminal:
    case TerminalAdapterKind::gnomeTerminal:
    case TerminalAdapterKind::konsole:
    case TerminalAdapterKind::alacritty:
    case TerminalAdapterKind::kitty:
      return true;
    case TerminalAdapterKind::unknown:
      return false;
  }
  return false;
}

std::wstring_view TerminalAdapterKindId(TerminalAdapterKind kind) noexcept {
  switch (kind) {
    case TerminalAdapterKind::windowsConsole:
      return L"windows-console";
    case TerminalAdapterKind::xterm:
      return L"xterm";
    case TerminalAdapterKind::xfce4Terminal:
      return L"xfce4-terminal";
    case TerminalAdapterKind::gnomeTerminal:
      return L"gnome-terminal";
    case TerminalAdapterKind::konsole:
      return L"konsole";
    case TerminalAdapterKind::alacritty:
      return L"alacritty";
    case TerminalAdapterKind::kitty:
      return L"kitty";
    case TerminalAdapterKind::unknown:
      return L"unknown";
  }
  return L"unknown";
}

std::wstring_view TerminalAdapterDisplayName(TerminalAdapterKind kind) noexcept {
  switch (kind) {
    case TerminalAdapterKind::windowsConsole:
      return L"Windows 控制台（辅助进程自建）";
    case TerminalAdapterKind::xterm:
      return L"xterm";
    case TerminalAdapterKind::xfce4Terminal:
      return L"xfce4-terminal";
    case TerminalAdapterKind::gnomeTerminal:
      return L"GNOME Terminal";
    case TerminalAdapterKind::konsole:
      return L"Konsole";
    case TerminalAdapterKind::alacritty:
      return L"Alacritty";
    case TerminalAdapterKind::kitty:
      return L"kitty";
    case TerminalAdapterKind::unknown:
      return L"未知终端";
  }
  return L"未知终端";
}

bool ParseTerminalAdapterKind(std::wstring_view id, TerminalAdapterKind* out) noexcept {
  if (out == nullptr) {
    return false;
  }
  if (id == L"windows-console") {
    *out = TerminalAdapterKind::windowsConsole;
    return true;
  }
  if (id == L"xterm") {
    *out = TerminalAdapterKind::xterm;
    return true;
  }
  if (id == L"xfce4-terminal") {
    *out = TerminalAdapterKind::xfce4Terminal;
    return true;
  }
  if (id == L"gnome-terminal") {
    *out = TerminalAdapterKind::gnomeTerminal;
    return true;
  }
  if (id == L"konsole") {
    *out = TerminalAdapterKind::konsole;
    return true;
  }
  if (id == L"alacritty") {
    *out = TerminalAdapterKind::alacritty;
    return true;
  }
  if (id == L"kitty") {
    *out = TerminalAdapterKind::kitty;
    return true;
  }
  return false;
}

TerminalSelection SelectTerminalFromCandidates(const std::vector<TerminalCandidate>& candidates,
                                               const TerminalPreferences& preferences) {
  TerminalSelection result;
  for (const TerminalCandidate& candidate : candidates) {
    if (IsKnownTerminalAdapter(candidate.kind)) {
      result.candidates.push_back(candidate);
    }
  }

  const auto select = [&result](const TerminalCandidate& candidate) {
    result.usable = true;
    result.selected = candidate;
  };

  const bool hasExplicitChoice =
      !preferences.preferredProgramPath.empty() ||
      preferences.preferredKind != TerminalAdapterKind::unknown;

  if (!preferences.preferredProgramPath.empty()) {
    for (const TerminalCandidate& candidate : result.candidates) {
      if (candidate.programPath == preferences.preferredProgramPath) {
        select(candidate);
        return result;
      }
    }
  }
  if (preferences.preferredKind != TerminalAdapterKind::unknown) {
    for (const TerminalCandidate& candidate : result.candidates) {
      if (candidate.kind == preferences.preferredKind) {
        select(candidate);
        return result;
      }
    }
  }

  if (hasExplicitChoice && !preferences.allowFallback) {
    result.failureReason =
        L"设置的终端不可用：没有找到匹配的已验证终端适配器。本程序不会改用隐藏方式执行交互式 Git。";
    result.guidance =
        L"请安装或重新指定终端（xterm / xfce4-terminal / gnome-terminal / konsole / "
        L"alacritty / kitty），或在设置里允许回退到其它已安装终端；"
        L"本程序不会改用隐藏方式执行交互式 Git。";
    return result;
  }

  if (!result.candidates.empty()) {
    select(result.candidates.front());
    if (hasExplicitChoice) {
      result.note = L"设置的终端不可用，已回退到 " +
                    std::wstring(TerminalAdapterDisplayName(result.selected.kind)) + L"。";
    }
    return result;
  }

  result.failureReason = L"没有找到可用的交互式终端。";
  result.guidance =
      L"请安装 xterm、xfce4-terminal、gnome-terminal、konsole、alacritty 或 kitty 之一，"
      L"并在图形桌面会话中启动本程序。本程序不会把需要交互的 Git 操作改成隐藏执行；"
      L"没有终端时只能用只读查询。";
  return result;
}

bool IsSafeTerminalTitle(std::wstring_view title) noexcept {
  if (title.empty()) {
    return false;
  }
  return !HasControlOrNul(title);
}

TerminalLaunchPlan BuildTerminalLaunchPlan(const TerminalCandidate& candidate,
                                           std::wstring_view title,
                                           std::wstring_view program,
                                           const std::vector<std::wstring>& arguments) {
  const TerminalAdapterKind kind = candidate.kind;
  if (!IsKnownTerminalAdapter(kind)) {
    return Fail(kind, L"未知的终端适配器，拒绝启动。");
  }
  if (!IsSafeTerminalTitle(title)) {
    return Fail(kind, L"终端标题为空或含控制字符，拒绝启动。");
  }
  if (program.empty() || HasControlOrNul(program)) {
    return Fail(kind, L"要启动的程序为空或含控制字符，拒绝启动。");
  }
  for (const std::wstring& argument : arguments) {
    if (HasControlOrNul(argument)) {
      return Fail(kind, L"启动参数含控制字符，拒绝交给终端。");
    }
  }

  TerminalLaunchPlan plan;
  plan.kind = kind;
  plan.program = std::wstring(candidate.programPath);
  if (plan.program.empty()) {
    return Fail(kind, L"终端程序路径为空，拒绝启动。");
  }

  switch (kind) {
    case TerminalAdapterKind::xterm:
    case TerminalAdapterKind::konsole:
    case TerminalAdapterKind::alacritty: {
      plan.arguments = {L"-T", std::wstring(title), L"-e", std::wstring(program)};
      if (kind == TerminalAdapterKind::konsole) {
        plan.arguments = {L"--title", std::wstring(title), L"-e", std::wstring(program)};
      } else if (kind == TerminalAdapterKind::alacritty) {
        plan.arguments = {L"--title", std::wstring(title), L"-e", std::wstring(program)};
      }
      plan.arguments.insert(plan.arguments.end(), arguments.begin(), arguments.end());
      plan.note = L"终端在 -e 之后直接按参数数组启动程序，不经过 shell。";
      return plan;
    }
    case TerminalAdapterKind::gnomeTerminal: {
      plan.arguments = {L"--title", std::wstring(title), L"--wait", L"--", std::wstring(program)};
      plan.arguments.insert(plan.arguments.end(), arguments.begin(), arguments.end());
      plan.note = L"gnome-terminal 可能把窗口交给已有服务，--wait 只约束客户端；"
                  L"Git 结果仍以操作目录里的结果文件为准。";
      return plan;
    }
    case TerminalAdapterKind::kitty: {
      if (!program.empty() && program.front() == L'-') {
        return Fail(kind, L"kitty 需要程序名不以减号开头（否则会被当成选项）。");
      }
      plan.arguments = {L"--title", std::wstring(title), std::wstring(program)};
      plan.arguments.insert(plan.arguments.end(), arguments.begin(), arguments.end());
      plan.note = L"kitty 在选项之后直接按参数数组启动程序。";
      return plan;
    }
    case TerminalAdapterKind::xfce4Terminal: {
      // xfce4-terminal 的 -x 会把剩余命令行按自己的规则再拆一次。进入这里的值只有
      // 辅助程序路径、操作目录与随机口令；仍要求它们是 shell 简单形态，避免任何
      // 空白、引号或元字符在终端内部被二次解释。
      if (!IsShellSimpleToken(program)) {
        return Fail(kind,
                    L"xfce4-terminal 的 -x 会重新解析剩余命令行，而本程序路径含空白或特殊字符；"
                    L"已拒绝该适配器，请改用 xterm 或在设置里指定直接按参数数组启动的终端。");
      }
      for (const std::wstring& argument : arguments) {
        if (!IsShellSimpleToken(argument)) {
          return Fail(kind,
                      L"xfce4-terminal 的 -x 会重新解析剩余命令行，而启动参数含特殊字符；"
                      L"已拒绝该适配器，不会把这些字符拼成一条 shell 命令。");
        }
      }
      plan.arguments = {L"--title", std::wstring(title), L"-x", std::wstring(program)};
      plan.arguments.insert(plan.arguments.end(), arguments.begin(), arguments.end());
      plan.note = L"xfce4-terminal 的 -x 仅接收简单 ASCII 参数：辅助程序路径、操作目录与口令。";
      return plan;
    }
    case TerminalAdapterKind::windowsConsole:
      return Fail(kind,
                  L"Windows 控制台由 CommandWindowRunner 直接创建辅助进程，不生成终端模拟器参数。");
    case TerminalAdapterKind::unknown:
      break;
  }
  return Fail(kind, L"未知的终端适配器，拒绝启动。");
}

}  // namespace gc::platform
