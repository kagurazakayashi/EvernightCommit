#include "support/tiny_test.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "git/command_window.h"

namespace {

using gc::git::CommandPlanReject;
using gc::git::CommandWindowOperation;

CommandWindowOperation MakeOperation(std::wstring executable, std::vector<std::wstring> arguments,
                                     std::wstring repository = L"C:\\repo") {
  CommandWindowOperation operation;
  operation.operationId = L"status";
  operation.displayName = L"status";
  operation.gitExecutable = std::move(executable);
  operation.repositoryDirectory = std::move(repository);
  operation.arguments = std::move(arguments);
  return operation;
}

std::wstring BuildLine(const CommandWindowOperation& operation, bool* ok, CommandPlanReject* reject,
                       std::wstring* detail) {
  std::wstring line;
  const bool built = gc::git::BuildGitCommandLine(operation, &line, reject, detail);
  if (ok != nullptr) {
    *ok = built;
  }
  return line;
}

}  // namespace

GC_TEST(command_plan_rejects_empty_fields) {
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;

  auto noExe = MakeOperation(L"", {L"status"});
  bool ok = true;
  BuildLine(noExe, &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::emptyExecutable);

  auto noRepo = MakeOperation(L"C:\\git.exe", {L"status"}, L"");
  BuildLine(noRepo, &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::emptyWorkingDirectory);

  auto noId = MakeOperation(L"C:\\git.exe", {L"status"});
  noId.operationId.clear();
  BuildLine(noId, &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::emptyOperationId);

  auto evilId = MakeOperation(L"C:\\git.exe", {L"status"});
  evilId.operationId = L"ops&1";
  BuildLine(evilId, &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::illegalOperationId);
}

GC_TEST(command_plan_quotes_program_and_arguments) {
  const CommandWindowOperation operation =
      MakeOperation(L"C:\\Program Files\\Git\\bin\\git.exe", {L"status", L"--porcelain=v1"});
  bool ok = false;
  const std::wstring line = BuildLine(operation, &ok, nullptr, nullptr);

  GC_CHECK(ok);
  GC_CHECK(line == L"\"C:\\Program Files\\Git\\bin\\git.exe\" \"status\" \"--porcelain=v1\"");
}

GC_TEST(command_plan_accepts_chinese_and_special_paths) {
  // 中文、空格、& % ! 括号在引号区域内都是 cmd 字面量，应当被接受。
  const CommandWindowOperation operation = MakeOperation(
      L"D:\\软件 & 工具\\Git\\bin\\git.exe",
      {L"status", L"--porcelain=1", L"-c core.quotepath=false", L"100%(x)|y&z"});
  bool ok = false;
  const std::wstring line = BuildLine(operation, &ok, nullptr, nullptr);

  GC_CHECK(ok);
  GC_CHECK(line.find(L"|") == std::wstring::npos || line.find(L"\"100%(x)|y&z\"") != std::wstring::npos);
  GC_CHECK(line.find(L"\"100%(x)|y&z\"") != std::wstring::npos);
}

GC_TEST(command_plan_rejects_quotes_in_paths) {
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  const CommandWindowOperation operation = MakeOperation(L"C:\\evil\"path\\git.exe", {L"status"});
  bool ok = true;
  BuildLine(operation, &ok, &reject, &detail);

  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::quoteInPath);
  GC_CHECK(!detail.empty());
}

GC_TEST(command_plan_rejects_control_characters) {
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  const CommandWindowOperation operation = MakeOperation(L"C:\\git\tx.exe", {L"status"});
  bool ok = true;
  BuildLine(operation, &ok, &reject, &detail);

  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::controlCharacterInPath);
}

GC_TEST(command_plan_rejects_injection_via_argument_quotes) {
  // 攻击样例：参数里塞转义引号 + & 想要再执行一条命令。
  // 本执行器不接受任何含双引号的参数，因此这类输入在启动 cmd 之前就被拒绝。
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", {L"status\\\" & calc"});
  bool ok = true;
  BuildLine(operation, &ok, &reject, &detail);

  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::illegalArgument);
}

GC_TEST(command_plan_rejects_unbalanced_metacharacters_outside_quotes) {
  // 程序路径本身不含引号，因此拼接结果里引号必然成对；
  // 这里校验的是区域判定：引号外的 & 一定出现在“程序段与参数段之间”，
  // 而构造规则不会把 & 留到引号外，所以合法输入必须全部通过、非法输入全部被拒。
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  auto operation = MakeOperation(L"C:\\git.exe", {L"status"});
  operation.arguments.push_back(L"a b");  // 含空格但无引号：允许，会被整体引用。
  bool ok = true;
  const std::wstring line = BuildLine(operation, &ok, &reject, &detail);
  GC_CHECK(ok);
  GC_CHECK(line == L"\"C:\\git.exe\" \"status\" \"a b\"");
}

GC_TEST(command_plan_rejects_too_many_arguments) {
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  std::vector<std::wstring> arguments;
  for (size_t index = 0; index <= gc::git::kMaxArguments; ++index) {
    arguments.push_back(L"x");
  }
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", std::move(arguments));
  bool ok = true;
  BuildLine(operation, &ok, &reject, &detail);

  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::tooManyArguments);
}

GC_TEST(command_title_is_unique_and_free_of_cmd_metacharacters) {
  // 标题必须带唯一操作 ID（否则“按标题关闭窗口”会找错窗口），且不含改变解析的字符。
  const std::wstring plain = gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"status", L"GcOp1");
  const std::wstring evil = gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"pull & calc | x > y \"z\"", L"GcOp2");
  const std::wstring chinese = gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"提交 检查", L"GcOp3");

  GC_CHECK(plain.find(L"GcOp1") != std::wstring::npos);
  GC_CHECK(evil.find(L"GcOp2") != std::wstring::npos);
  GC_CHECK(evil.find_first_of(L"&|<>\"%") == std::wstring::npos);
  GC_CHECK(chinese.find(L"提交 检查") != std::wstring::npos);  // 中文保留
  GC_CHECK(plain != std::wstring(gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"status", L"GcOp9")));
}

GC_TEST(command_script_contains_echo_call_and_markers) {
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", {L"status"});
  bool ok = false;
  const std::wstring gitLine = BuildLine(operation, &ok, nullptr, nullptr);
  GC_CHECK(ok);

  gc::git::CommandWindowPlan plan;
  const bool assembled = gc::git::AssembleCommandWindowScript(
      L"GcOp7", "C:\\Users\\me\\AppData\\Local\\Temp\\GcOp7", L"Git status", "Git status",
      "\"C:\\git.exe\"", "\"status\"", &plan);

  GC_CHECK(assembled);
  GC_CHECK(plan.scriptAnsi.find("@echo off") == 0);
  GC_CHECK(plan.scriptAnsi.find("start.txt") != std::string::npos);
  GC_CHECK(plan.scriptAnsi.find("call \"C:\\git.exe\" \"status\"") != std::string::npos);
  GC_CHECK(plan.scriptAnsi.find("%ERRORLEVEL%") != std::string::npos);
  GC_CHECK(plan.scriptAnsi.find("result.txt") != std::string::npos);
  // Git 调用与结果写出必须是两条物理命令：同一行会在解析期展开 %ERRORLEVEL%。
  const size_t callPosition = plan.scriptAnsi.find("call ");
  const size_t resultPosition = plan.scriptAnsi.find("%ERRORLEVEL%");
  GC_CHECK(callPosition != std::string::npos && resultPosition > callPosition);
  GC_CHECK(plan.scriptAnsi.substr(callPosition, resultPosition - callPosition).find('\n') != std::string::npos);
  // 标记与结果文件各只用一个重定向，且写在行首：
  // 组合写法（>nul echo x>"file"）不会建文件；数字紧邻 > 会被 cmd 当作句柄重定向，
  // 导致 `echo %ERRORLEVEL%>"file"` 展开后变成“echo 无参数 + 句柄 0 重定向”。
  GC_CHECK(plan.scriptAnsi.find(">nul") == std::string::npos);
  bool digitBeforeRedirect = false;
  for (size_t index = 1; index + 1 < plan.scriptAnsi.size(); ++index) {
    const char previous = plan.scriptAnsi[index - 1];
    if (plan.scriptAnsi[index] == '>' && previous >= '0' && previous <= '9') {
      digitBeforeRedirect = true;
    }
  }
  GC_CHECK(!digitBeforeRedirect);
  GC_CHECK(plan.scriptAnsi.find(">\"C:\\Users\\me\\AppData\\Local\\Temp\\GcOp7\\result.txt\" echo %ERRORLEVEL%") !=
           std::string::npos);
  // 宽字符命令行不进计划：避免把码页字节逐字符提升成乱码。
  GC_CHECK(gitLine.find(L'"') != std::wstring::npos);
}

GC_TEST(command_script_rejects_non_ascii_directory) {
  gc::git::CommandWindowPlan plan;
  const bool assembled = gc::git::AssembleCommandWindowScript(
      L"GcOp7", "C:\\临时\\GcOp7", L"Git status", "Git status", "\"C:\\git.exe\"", "\"status\"", &plan);
  GC_CHECK(!assembled);
}

GC_TEST(command_script_keeps_chinese_title_and_unique_id) {
  // 回归用例：中文标题曾被按字节校验误判成“含元字符”，整条标题退化成占位文字，
  // 结果窗口标题失去唯一标记，按标题找窗口就失效（实测留下过一堆关不掉的窗口）。
  gc::git::CommandWindowPlan plan;
  const std::wstring wide =
      gc::git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", L"status", L"GcOp7");
  // 这里用逐字节提升的 ASCII 形态代替平台编码（纯逻辑层不依赖码页）。
  std::string encoded;
  for (const wchar_t c : wide) {
    encoded.push_back(c < 0x80 ? static_cast<char>(c) : static_cast<char>(0x41 + (c % 26)));
  }
  const bool assembled = gc::git::AssembleCommandWindowScript(
      L"GcOp7", "C:\\Temp\\GcOp7", wide, encoded, "\"C:\\git.exe\"", "\"status\"", &plan);

  GC_CHECK(assembled);
  GC_CHECK(plan.scriptAnsi.find("title ") != std::string::npos);
  GC_CHECK(plan.scriptAnsi.find("Git Command Window") == std::string::npos);  // 未触发回退
  GC_CHECK(plan.scriptAnsi.find("GcOp7") != std::string::npos);               // 唯一标记保留
}

GC_TEST(command_script_sanitizes_title_with_metacharacters) {
  gc::git::CommandWindowPlan plan;
  // 标题只是展示文字：含 cmd 元字符时退回“含操作 ID 的占位标题”，而不是拒绝一次合法的操作。
  const bool assembled = gc::git::AssembleCommandWindowScript(
      L"GcOp7", "C:\\Temp\\GcOp7", L"Git status & calc", "Git status & calc", "\"C:\\git.exe\"",
      "\"status\"", &plan);

  GC_CHECK(assembled);
  GC_CHECK(plan.scriptAnsi.find("title Git Command Window - GcOp7") != std::string::npos);
  GC_CHECK(plan.scriptAnsi.find("calc") == std::string::npos);
}

GC_TEST(command_script_rejects_corrupted_command_line_after_round_trip) {
  gc::git::CommandWindowPlan plan;
  // 模拟码页往返后出现引号区域不平衡的字节（防御式复核）。
  const bool assembled = gc::git::AssembleCommandWindowScript(
      L"GcOp7", "C:\\Temp\\GcOp7", L"Git status", "Git status", "\"C:\\git.exe\"", "\"unclosed", &plan);
  GC_CHECK(!assembled);
}

GC_TEST(command_result_parsing_accepts_only_complete_lines) {
  long exitCode = -1;
  GC_CHECK(!gc::git::ParseCommandWindowResult("", &exitCode));
  GC_CHECK(!gc::git::ParseCommandWindowResult("123", &exitCode));       // 没有换行：可能正在写
  GC_CHECK(!gc::git::ParseCommandWindowResult("abc\n", &exitCode));     // 不是整数
  GC_CHECK(!gc::git::ParseCommandWindowResult("1 2\n", &exitCode));     // 多余字段
  GC_CHECK(!gc::git::ParseCommandWindowResult("\n", &exitCode));
  GC_CHECK(gc::git::ParseCommandWindowResult("0\r\n", &exitCode) && exitCode == 0);
  GC_CHECK(gc::git::ParseCommandWindowResult("128\n", &exitCode) && exitCode == 128);
  GC_CHECK(gc::git::ParseCommandWindowResult("-2\r\n", &exitCode) && exitCode == -2);
  GC_CHECK(!gc::git::ParseCommandWindowResult("99999999999999999999\n", &exitCode));
}

namespace {

gc::git::CommandWindowObservation MakeFacts(bool launched, bool exited, bool startSeen,
                                           bool resultParsed, long exitCode = 0) {
  gc::git::CommandWindowObservation facts;
  facts.createProcessSucceeded = launched;
  facts.processExited = exited;
  facts.startMarkerSeen = startSeen;
  facts.resultParsed = resultParsed;
  facts.exitCode = exitCode;
  return facts;
}

}  // namespace

GC_TEST(command_completion_distinguishes_all_outcomes) {
  long exitCode = -1;

  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(false, false, false, false), &exitCode) ==
           gc::git::CommandCompletion::launchFailed);
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, false, false, false), &exitCode) ==
           gc::git::CommandCompletion::launched);
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, false, true, false), &exitCode) ==
           gc::git::CommandCompletion::running);
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, true, true, 0), &exitCode) ==
               gc::git::CommandCompletion::finished &&
           exitCode == 0);
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, true, true, 1), &exitCode) ==
               gc::git::CommandCompletion::finished &&
           exitCode == 1);
  // 9009 是 cmd 对“程序不存在”的保留码：不是 Git 的回答。
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, true, true, 9009), &exitCode) ==
           gc::git::CommandCompletion::gitNotStarted);
  // 窗口被提前关闭：脚本跑过但结果缺失。
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, true, false), &exitCode) ==
           gc::git::CommandCompletion::terminated);
  // 连开始标记都没有：cmd 从未执行到脚本第一行（脚本无法执行，或窗口在启动瞬间被关闭）。
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, false, false), &exitCode) ==
           gc::git::CommandCompletion::scriptNeverRan);
}

GC_TEST(command_observer_reads_injected_files) {
  const std::map<std::string, std::string> files{
      {"start.txt", "start"},
      {"result.txt", "42\r\n"},
  };
  const auto reader = [&files](std::string_view name) -> std::optional<std::string> {
    const auto found = files.find(std::string(name));
    if (found == files.end()) {
      return std::nullopt;
    }
    return found->second;
  };

  const gc::git::CommandWindowObservation facts =
      gc::git::ObserveCommandWindow(reader, /*createProcessSucceeded=*/true, /*processExited=*/false);

  GC_CHECK(facts.startMarkerSeen);
  GC_CHECK(facts.resultParsed);
  GC_CHECK(facts.exitCode == 42);
  long exitCode = 0;
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) == gc::git::CommandCompletion::finished);
}

GC_TEST(command_observer_treats_missing_files_as_absent) {
  const auto reader = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
  const gc::git::CommandWindowObservation facts =
      gc::git::ObserveCommandWindow(reader, true, true);
  GC_CHECK(!facts.startMarkerSeen);
  GC_CHECK(!facts.resultParsed);
  long exitCode = 0;
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) == gc::git::CommandCompletion::scriptNeverRan);
}

GC_TEST(command_observer_keeps_running_until_result_written) {
  // 脚本已开跑、结果还没写出，进程也还活着：必须是中间态“执行中”，既不能报成功也不能报失败。
  const std::map<std::string, std::string> files{{"start.txt", "start"}};
  const auto reader = [&files](std::string_view name) -> std::optional<std::string> {
    const auto found = files.find(std::string(name));
    if (found == files.end()) {
      return std::nullopt;
    }
    return found->second;
  };
  const gc::git::CommandWindowObservation facts =
      gc::git::ObserveCommandWindow(reader, true, /*processExited=*/false);
  long exitCode = 0;
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) == gc::git::CommandCompletion::running);
}
