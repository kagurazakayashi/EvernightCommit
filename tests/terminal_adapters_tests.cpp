// 终端适配器参数与候选选择的纯逻辑测试：不启动任何终端、不碰文件系统。
// 重点是“每个终端只用自己的明确参数”“xfce 的 -x 不接收会被二次解释的字符”
// 以及“没有候选时绝不悄悄改成隐藏执行”。
#include <string>
#include <vector>

#include "platform/terminal.h"
#include "platform/terminal_adapters.h"
#include "support/tiny_test.h"

namespace {

using gc::platform::BuildTerminalLaunchPlan;
using gc::platform::SelectTerminalFromCandidates;
using gc::platform::TerminalAdapterKind;
using gc::platform::TerminalCandidate;
using gc::platform::TerminalPreferences;

TerminalCandidate Candidate(TerminalAdapterKind kind, std::wstring path) {
  TerminalCandidate candidate;
  candidate.kind = kind;
  candidate.programPath = std::move(path);
  candidate.discoveredAs = std::wstring(gc::platform::TerminalAdapterKindId(kind));
  return candidate;
}

bool Contains(const std::vector<std::wstring>& values, std::wstring_view wanted) {
  for (const std::wstring& value : values) {
    if (value == wanted) {
      return true;
    }
  }
  return false;
}

}  // namespace

GC_TEST(terminal_adapter_ids_round_trip_and_unknown_is_rejected) {
  for (const TerminalAdapterKind kind :
       {TerminalAdapterKind::xterm, TerminalAdapterKind::xfce4Terminal,
        TerminalAdapterKind::gnomeTerminal, TerminalAdapterKind::konsole,
        TerminalAdapterKind::alacritty, TerminalAdapterKind::kitty,
        TerminalAdapterKind::windowsConsole}) {
    gc::platform::TerminalAdapterKind parsed = TerminalAdapterKind::unknown;
    GC_CHECK(gc::platform::ParseTerminalAdapterKind(gc::platform::TerminalAdapterKindId(kind),
                                                    &parsed));
    GC_CHECK(parsed == kind);
  }
  gc::platform::TerminalAdapterKind parsed = TerminalAdapterKind::xterm;
  GC_CHECK(!gc::platform::ParseTerminalAdapterKind(L"not-a-terminal", &parsed));
  GC_CHECK(parsed == TerminalAdapterKind::xterm);
}

GC_TEST(terminal_xterm_uses_dash_e_and_passes_argv_verbatim) {
  const TerminalCandidate candidate = Candidate(TerminalAdapterKind::xterm, L"/usr/bin/xterm");
  const auto plan = BuildTerminalLaunchPlan(
      candidate, L"EvernightCommit - status - GcOp1xabcdef",
      L"/opt/Evernight Commit/EvernightCommit",
      {L"--gc-terminal-helper", L"/tmp/GcOp1xabcdef", L"0123456789abcdef"});
  GC_REQUIRE(plan.failureReason.empty(), "xterm 参数应可构造");
  GC_CHECK(plan.program == L"/usr/bin/xterm");
  GC_REQUIRE(plan.arguments.size() == 7, "xterm 参数数量");
  GC_CHECK(plan.arguments[0] == L"-T");
  GC_CHECK(plan.arguments[1] == L"EvernightCommit - status - GcOp1xabcdef");
  GC_CHECK(plan.arguments[2] == L"-e");
  GC_CHECK(plan.arguments[3] == L"/opt/Evernight Commit/EvernightCommit");
  GC_CHECK(plan.arguments[4] == L"--gc-terminal-helper");
  // 仓库路径、分支与提交内容不在终端参数里；这里只应有操作目录与口令。
  GC_CHECK(plan.arguments[5] == L"/tmp/GcOp1xabcdef");
  GC_CHECK(plan.arguments[6] == L"0123456789abcdef");
}

GC_TEST(terminal_other_adapters_keep_their_own_option_forms) {
  const std::vector<std::wstring> arguments = {L"--gc-terminal-helper", L"/tmp/GcOp1xabcdef",
                                               L"0123456789abcdef"};
  const TerminalCandidate gnome = Candidate(TerminalAdapterKind::gnomeTerminal, L"/usr/bin/gnome-terminal");
  const auto gnomePlan = BuildTerminalLaunchPlan(gnome, L"title", L"/bin/helper", arguments);
  GC_REQUIRE(gnomePlan.failureReason.empty(), "gnome-terminal 参数应可构造");
  GC_CHECK(Contains(gnomePlan.arguments, L"--wait"));
  GC_CHECK(Contains(gnomePlan.arguments, L"--"));

  const TerminalCandidate konsole = Candidate(TerminalAdapterKind::konsole, L"/usr/bin/konsole");
  const auto konsolePlan = BuildTerminalLaunchPlan(konsole, L"title", L"/bin/helper", arguments);
  GC_REQUIRE(konsolePlan.failureReason.empty(), "konsole 参数应可构造");
  GC_CHECK(Contains(konsolePlan.arguments, L"-e"));

  const TerminalCandidate alacritty = Candidate(TerminalAdapterKind::alacritty, L"/usr/bin/alacritty");
  const auto alacrittyPlan = BuildTerminalLaunchPlan(alacritty, L"title", L"/bin/helper", arguments);
  GC_REQUIRE(alacrittyPlan.failureReason.empty(), "alacritty 参数应可构造");
  GC_CHECK(Contains(alacrittyPlan.arguments, L"-e"));

  const TerminalCandidate kitty = Candidate(TerminalAdapterKind::kitty, L"/usr/bin/kitty");
  const auto kittyPlan = BuildTerminalLaunchPlan(kitty, L"title", L"/bin/helper", arguments);
  GC_REQUIRE(kittyPlan.failureReason.empty(), "kitty 参数应可构造");
  GC_CHECK(kittyPlan.arguments[0] == L"--title");
}

GC_TEST(terminal_xfce_refuses_tokens_its_dash_x_would_reinterpret) {
  const TerminalCandidate candidate =
      Candidate(TerminalAdapterKind::xfce4Terminal, L"/usr/bin/xfce4-terminal");
  const auto simple = BuildTerminalLaunchPlan(
      candidate, L"title", L"/opt/evernight/helper",
      {L"--gc-terminal-helper", L"/tmp/GcOp1xabcdef", L"0123456789abcdef"});
  GC_REQUIRE(simple.failureReason.empty(), "简单 ASCII 参数应可交给 xfce4-terminal");
  GC_CHECK(Contains(simple.arguments, L"-x"));

  // -x 会重新解析剩余命令行：含空白的程序路径必须被拒绝，而不是被拼成一条 shell 命令。
  const auto spaced = BuildTerminalLaunchPlan(
      candidate, L"title", L"/opt/evernight commit/helper",
      {L"--gc-terminal-helper", L"/tmp/GcOp1xabcdef", L"0123456789abcdef"});
  GC_CHECK(!spaced.failureReason.empty());
  GC_CHECK(spaced.arguments.empty());

  const auto injected = BuildTerminalLaunchPlan(
      candidate, L"title", L"/opt/helper",
      {L"--gc-terminal-helper", L"/tmp/GcOp1xabcdef;touch pwned", L"0123456789abcdef"});
  GC_CHECK(!injected.failureReason.empty());
  GC_CHECK(injected.arguments.empty());
}

GC_TEST(terminal_selection_honours_explicit_choice_and_reports_fallback) {
  const std::vector<TerminalCandidate> candidates = {
      Candidate(TerminalAdapterKind::xterm, L"/usr/bin/xterm"),
      Candidate(TerminalAdapterKind::xfce4Terminal, L"/usr/bin/xfce4-terminal"),
  };
  TerminalPreferences preferences;
  preferences.preferredKind = TerminalAdapterKind::xfce4Terminal;
  const auto selected = SelectTerminalFromCandidates(candidates, preferences);
  GC_REQUIRE(selected.usable, "显式选择应命中");
  GC_CHECK(selected.selected.kind == TerminalAdapterKind::xfce4Terminal);

  preferences.preferredKind = TerminalAdapterKind::konsole;
  preferences.allowFallback = true;
  const auto fallback = SelectTerminalFromCandidates(candidates, preferences);
  GC_REQUIRE(fallback.usable, "允许回退时应选默认终端");
  GC_CHECK(fallback.selected.kind == TerminalAdapterKind::xterm);
  GC_CHECK(!fallback.note.empty());

  preferences.allowFallback = false;
  const auto refused = SelectTerminalFromCandidates(candidates, preferences);
  GC_CHECK(!refused.usable);
  GC_CHECK(!refused.failureReason.empty());
  GC_CHECK(refused.guidance.find(L"不会改用隐藏方式") != std::wstring::npos);
}

GC_TEST(terminal_selection_without_candidates_explains_that_hidden_execution_is_not_used) {
  const auto selection =
      SelectTerminalFromCandidates({}, TerminalPreferences{});
  GC_CHECK(!selection.usable);
  GC_CHECK(selection.failureReason.find(L"没有找到") != std::wstring::npos);
  GC_CHECK(selection.guidance.find(L"不会把需要交互的 Git 操作改成隐藏执行") !=
           std::wstring::npos);
}

GC_TEST(terminal_windows_console_does_not_pretend_to_have_simulator_arguments) {
  const TerminalCandidate candidate =
      Candidate(TerminalAdapterKind::windowsConsole, L"");
  const auto plan = BuildTerminalLaunchPlan(candidate, L"title", L"C:\\helper.exe",
                                            {L"--gc-console-helper"});
  GC_CHECK(!plan.failureReason.empty());
  GC_CHECK(plan.arguments.empty());
}
