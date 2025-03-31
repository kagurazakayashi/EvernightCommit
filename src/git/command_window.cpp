#include "git/command_window.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace gc::git {
namespace {

bool IsPrintableAscii(std::string_view bytes) {
  if (bytes.empty()) {
    return false;
  }
  for (const char c : bytes) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u > 0x7Eu) {
      return false;
    }
  }
  return true;
}

bool IsSafeOperationId(std::wstring_view id) {
  if (id.empty() || id.size() > kMaxOperationIdLength) {
    return false;
  }
  for (const wchar_t c : id) {
    if (c < 0x21 || c > 0x7E) {
      return false;  // 必须可打印 ASCII
    }
    const wchar_t lower = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    const bool alnum = (lower >= L'a' && lower <= L'z') || (c >= L'0' && c <= L'9');
    if (alnum || lower == L'-' || lower == L'_') {
      continue;
    }
    return false;  // 其余字符（含 & | ^ " % 与空格）一律不许进 ID
  }
  return true;
}

bool HasQuote(std::wstring_view text) { return text.find(L'"') != std::wstring_view::npos; }

bool HasControl(std::wstring_view text) {
  for (const wchar_t c : text) {
    if (c < 0x20 || c == 0x7F) {
      return true;
    }
  }
  return false;
}

// 程序参数引用规则（MSVC CRT / CommandLineToArgvW 同源）：一律加引号；
// 引号前的连续反斜杠加倍，嵌入引号写作 \"，结尾连续反斜杠加倍。
// 本执行器在边界上已拒绝含引号的不可信输入，因此 \" 分支不会因用户数据触发，
// 但保留完整规则，使转义语义可独立测试、可被其他调用方安全复用。
void AppendQuoted(std::wstring& line, std::wstring_view value) {
  line.push_back(L'"');
  size_t pendingBackslashes = 0;
  for (const wchar_t c : value) {
    if (c == L'\\') {
      ++pendingBackslashes;
      continue;
    }
    if (c == L'"') {
      line.append(pendingBackslashes * 2 + 1, L'\\');
      line.push_back(c);
    } else {
      line.append(pendingBackslashes, L'\\');
      line.push_back(c);
    }
    pendingBackslashes = 0;
  }
  line.append(pendingBackslashes * 2, L'\\');
  line.push_back(L'"');
}

std::wstring BuildQuotedLine(std::wstring_view program, const std::vector<std::wstring>& arguments) {
  std::wstring line;
  AppendQuoted(line, program);
  for (const std::wstring& argument : arguments) {
    line.push_back(L' ');
    AppendQuoted(line, argument);
  }
  return line;
}

// 校验“将要写进 cmd 脚本的一行”：引号区域之外不得出现 cmd 元字符。
// 每个 “ 都被视作区域切换（cmd 的引号逐字符切换，与 CRT 的转义语义不同），
// 因此关闭引号后若紧跟非空格字符，说明存在字面量引号（可能被 cmd 误判成区域切换），拒绝。
bool CommandLineRegionSafe(std::wstring_view line) {
  bool inQuote = false;
  for (size_t index = 0; index < line.size(); ++index) {
    const wchar_t c = line[index];
    if (c == L'"') {
      if (inQuote) {
        const size_t next = index + 1;
        if (next < line.size() && line[next] != L' ') {
          return false;
        }
      }
      inQuote = !inQuote;
      continue;
    }
    if (inQuote) {
      continue;  // 引号区域内 & | < > ^ % ! 都是字面量，交由 git 自行处理。
    }
    if (c == L'&' || c == L'|' || c == L'<' || c == L'>' || c == L'^') {
      return false;
    }
  }
  return !inQuote;  // 引号必须成对闭合。
}

std::wstring AsciiToUtf16(std::string_view bytes) {
  std::wstring wide;
  wide.reserve(bytes.size());
  for (const char c : bytes) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return wide;
}

std::wstring TrimTrailingSpaces(std::wstring_view text) {
  size_t end = text.size();
  while (end > 0 && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
    --end;
  }
  return std::wstring(text.substr(0, end));
}

std::vector<std::wstring> SplitAsciiWords(std::string_view line) {
  std::vector<std::wstring> words;
  size_t start = 0;
  while (start < line.size()) {
    const size_t space = line.find(' ', start);
    const std::string_view word =
        (space == std::string_view::npos) ? line.substr(start) : line.substr(start, space - start);
    if (!word.empty()) {
      words.push_back(AsciiToUtf16(word));
    }
    if (space == std::string_view::npos) {
      break;
    }
    start = space + 1;
  }
  return words;
}

}  // namespace

std::wstring_view CommandPlanRejectLabel(CommandPlanReject reject) noexcept {
  switch (reject) {
    case CommandPlanReject::none:
      return L"无";
    case CommandPlanReject::emptyOperationId:
      return L"操作 ID 为空";
    case CommandPlanReject::illegalOperationId:
      return L"操作 ID 含不安全字符（只允许 ASCII 字母、数字、连字符与下划线）";
    case CommandPlanReject::emptyExecutable:
      return L"Git 程序路径为空";
    case CommandPlanReject::emptyWorkingDirectory:
      return L"仓库目录为空";
    case CommandPlanReject::quoteInPath:
      return L"路径含双引号，无法安全交给命令窗口解释";
    case CommandPlanReject::controlCharacterInPath:
      return L"路径含控制字符，无法安全写入脚本";
    case CommandPlanReject::illegalArgument:
      return L"参数含双引号或控制字符";
    case CommandPlanReject::tooManyArguments:
      return L"参数数量超过上限";
    case CommandPlanReject::commandTooLong:
      return L"命令行长度超过上限";
    case CommandPlanReject::illegalScriptDirectory:
      return L"临时脚本目录不是纯 ASCII 路径";
    case CommandPlanReject::nonEncodableCommand:
      return L"命令行含系统 ANSI 码页无法表示的字符";
  }
  return L"未知原因";
}

bool BuildGitCommandLine(const CommandWindowOperation& operation, std::wstring* gitLine,
                         CommandPlanReject* reject, std::wstring* detail) {
  const auto fail = [&](CommandPlanReject reason, std::wstring message) {
    if (reject != nullptr) {
      *reject = reason;
    }
    if (detail != nullptr) {
      *detail = std::move(message);
    }
    return false;
  };
  if (gitLine == nullptr) {
    return fail(CommandPlanReject::emptyOperationId, L"内部错误：缺少输出对象");
  }
  *gitLine = L"";
  if (operation.operationId.empty()) {
    return fail(CommandPlanReject::emptyOperationId,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::emptyOperationId)));
  }
  if (!IsSafeOperationId(operation.operationId)) {
    return fail(CommandPlanReject::illegalOperationId,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::illegalOperationId)) + L"：" +
                    operation.operationId);
  }
  if (operation.gitExecutable.empty()) {
    return fail(CommandPlanReject::emptyExecutable,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::emptyExecutable)));
  }
  if (operation.repositoryDirectory.empty()) {
    return fail(CommandPlanReject::emptyWorkingDirectory,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::emptyWorkingDirectory)));
  }
  if (operation.arguments.size() > kMaxArguments) {
    return fail(CommandPlanReject::tooManyArguments,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::tooManyArguments)) + L"（上限 " +
                    std::to_wstring(kMaxArguments) + L"）");
  }
  for (const std::wstring& path : {operation.gitExecutable, operation.repositoryDirectory}) {
    if (HasQuote(path)) {
      return fail(CommandPlanReject::quoteInPath,
                  std::wstring(CommandPlanRejectLabel(CommandPlanReject::quoteInPath)) + L"：" + path);
    }
    if (HasControl(path)) {
      return fail(CommandPlanReject::controlCharacterInPath,
                  std::wstring(CommandPlanRejectLabel(CommandPlanReject::controlCharacterInPath)) + L"：" + path);
    }
  }
  for (const std::wstring& argument : operation.arguments) {
    if (HasQuote(argument) || HasControl(argument)) {
      return fail(CommandPlanReject::illegalArgument,
                  std::wstring(CommandPlanRejectLabel(CommandPlanReject::illegalArgument)) + L"：" + argument);
    }
  }

  const std::wstring line = BuildQuotedLine(operation.gitExecutable, operation.arguments);
  if (!CommandLineRegionSafe(line)) {
    return fail(CommandPlanReject::quoteInPath, L"命令行存在无法安全交给 cmd 的引号区域：" + line);
  }
  if (line.size() > kMaxDisplayCommandLength) {
    return fail(CommandPlanReject::commandTooLong,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::commandTooLong)) + L"：" + line);
  }
  *gitLine = line;
  return true;
}

std::wstring MakeSafeConsoleTitle(std::wstring_view prefix, std::wstring_view displayName,
                                  std::wstring_view operationId) {
  // title 行里会改变 cmd 解析的字符：引号与控制字符已在区域校验里禁止，这里再剔除
  // & | < > ^ % （% 还可能被当成变量引用）。中文、空格、= 与 ! 都是安全的字面量。
  static constexpr std::wstring_view kUnsafe = L"\"&|<>^%\r\n\t";
  std::wstring cleaned;
  cleaned.reserve(displayName.size());
  for (const wchar_t c : displayName) {
    cleaned.push_back(kUnsafe.find(c) == std::wstring_view::npos ? c : L' ');
  }
  const size_t firstNonSpace = cleaned.find_first_not_of(L' ');
  cleaned = firstNonSpace == std::wstring::npos ? std::wstring() : cleaned.substr(firstNonSpace);
  cleaned = TrimTrailingSpaces(cleaned);
  if (cleaned.size() > 80) {
    cleaned.resize(80);
  }
  std::wstring title(prefix);
  if (!cleaned.empty()) {
    title += L" - " + cleaned;
  }
  title += L" - ";
  title += operationId;  // 纯 ASCII 且唯一：保证窗口标题互不相同，可被按标题查找。
  return title;
}

bool AssembleCommandWindowScript(std::wstring_view operationId, std::string_view scriptDirectoryAnsi,
                                 std::wstring_view titleWide, std::string_view titleAnsi,
                                 std::string_view programAnsi, std::string_view argumentsAnsi,
                                 CommandWindowPlan* plan) {
  if (plan == nullptr || !IsSafeOperationId(operationId)) {
    return false;
  }
  // 标记与结果文件由重定向创建：目录名会在码页往返中被破坏，只允许纯 ASCII 可打印。
  if (!IsPrintableAscii(scriptDirectoryAnsi)) {
    return false;
  }
  // 标题的合法性必须按宽字符判定，不能按编码后的字节判定：
  // 多字节码页（GBK 等）的第二字节可以落进 ASCII 区间，按字节检查会把合法中文误判成元字符
  // —— 实测因此把标题整体退化成占位文字，丢掉了用于定位窗口的唯一标记。
  // 中文本身不危险；危险的是引号、控制字符，以及能改写行结构的 & | < > ^ %。
  // 编码字节只需再确认没有控制字节（多字节码页的组成字节都 >= 0x40，不会误报）。
  std::string titleBytes(titleAnsi);
  const bool titleUnsafe = HasControl(titleWide) || HasQuote(titleWide) ||
                           titleWide.find_first_of(L"&|<>^%") != std::wstring::npos ||
                           HasControl(AsciiToUtf16(titleAnsi));
  if (titleUnsafe) {
    // 回退标题同样带上操作 ID：否则多个窗口同名，按标题找窗口就会关错目标。
    // operationId 已由 IsSafeOperationId 保证是纯 ASCII，因此逐字符窄化不会丢信息。
    titleBytes = "Git Command Window - ";
    for (const wchar_t c : operationId) {
      titleBytes.push_back(static_cast<char>(c));
    }
  }
  // 程序段与参数段的字节形态必须再次通过区域校验：
  // 宽字符版已在 BuildGitCommandLine 判定，这里防止码页往返产生新的引号或元字符。
  const std::wstring gitLineWide = AsciiToUtf16(std::string(programAnsi) +
                                                (argumentsAnsi.empty() ? std::string()
                                                                       : " " + std::string(argumentsAnsi)));
  if (HasControl(gitLineWide) || !CommandLineRegionSafe(gitLineWide)) {
    return false;
  }

  const std::string directory(scriptDirectoryAnsi);
  std::string gitLine = std::string(programAnsi);
  if (!argumentsAnsi.empty()) {
    gitLine.push_back(' ');
    gitLine.append(argumentsAnsi);
  }

  std::string script;
  script.reserve(gitLine.size() + directory.size() + 512);
  script += "@echo off\r\n";
  script += "title " + titleBytes + "\r\n";
  // 行 1 写开始标记：证明 cmd 确在执行本脚本（区别于“cmd 启动即失败”）。
  // 标记与结果文件都用“行首重定向”：单一重定向才可靠，组合写法（>nul echo x>"file"）
  // 实测会让 cmd 报“找不到路径”且不建文件；数字紧邻 > 又会被当成句柄重定向。
  script += ">\"" + directory + "\\" + kStartMarkerFileName + "\" echo start\r\n";
  // 回显即将执行的真实命令，让用户在窗口里看到程序做了什么；下一行才是真正执行。
  script += "echo " + gitLine + "\r\n";
  // Git 进程调用。程序路径与参数都已通过引号区域校验，
  // 因此这一行的引号区域与 cmd 的解析一致，不存在元字符二次解释的空间。
  script += "call " + gitLine + "\r\n";
  // %ERRORLEVEL% 在同一物理行会在解析期展开，拿不到刚执行完的 Git 退出码，
  // 所以这一行必须与 call 分成两条物理命令（cmd 逐行解析）。
  // 重定向必须写在行首：`echo %ERRORLEVEL%>"file"` 展开成 `echo 0>"file"` 后，
  // 数字紧邻 > 会被 cmd 当作“句柄 0 的重定向”，于是 echo 没有参数、只把
  // “ECHO is off.” 打到屏幕，结果文件留下 0 字节（实测踩坑）。
  script += ">\"" + directory + "\\" + kResultFileName + "\" echo %ERRORLEVEL%\r\n";
  script += "echo [EvernightCommit] Git 已退出，窗口保持打开，可继续查看上方输出。\r\n";

  plan->scriptAnsi = std::move(script);
  return true;
}

bool ParseCommandWindowResult(std::string_view content, long* exitCode) {
  if (exitCode == nullptr || content.empty()) {
    return false;
  }
  const size_t newline = content.find('\n');
  if (newline == std::string_view::npos) {
    return false;  // 尚未写完：观察端继续等待。
  }
  std::string_view line = content.substr(0, newline);
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  const std::vector<std::wstring> words = SplitAsciiWords(line);
  if (words.size() != 1) {
    return false;
  }
  const std::wstring& exitWord = words[0];
  size_t digitsBegin = 0;
  bool negative = false;
  if (!exitWord.empty() && exitWord[0] == L'-') {
    negative = true;
    digitsBegin = 1;
  }
  if (digitsBegin >= exitWord.size()) {
    return false;
  }
  std::string digits;
  for (size_t index = digitsBegin; index < exitWord.size(); ++index) {
    if (exitWord[index] < L'0' || exitWord[index] > L'9') {
      return false;
    }
    digits.push_back(static_cast<char>(exitWord[index]));
  }
  errno = 0;
  const long magnitude = std::strtol(digits.c_str(), nullptr, 10);
  if (errno == ERANGE) {
    return false;
  }
  *exitCode = negative ? -magnitude : magnitude;
  return true;
}

std::wstring_view CommandCompletionLabel(CommandCompletion completion) noexcept {
  switch (completion) {
    case CommandCompletion::launchFailed:
      return L"启动失败";
    case CommandCompletion::launched:
      return L"已启动，等待脚本执行";
    case CommandCompletion::running:
      return L"执行中";
    case CommandCompletion::finished:
      return L"执行完成";
    case CommandCompletion::gitNotStarted:
      return L"Git 未能启动（脚本执行完毕但没有调用记录）";
    case CommandCompletion::terminated:
      return L"结果未知（命令窗口被提前关闭或 Git 进程被终止）";
    case CommandCompletion::scriptNeverRan:
      return L"启动失败（脚本未能执行）";
    case CommandCompletion::stillUnknown:
      return L"结果未知（超过观察期限）";
  }
  return L"未知状态";
}

CommandWindowObservation ObserveCommandWindow(const CommandWindowFileReader& readFile,
                                              bool createProcessSucceeded, bool processExited) {
  CommandWindowObservation facts;
  facts.createProcessSucceeded = createProcessSucceeded;
  facts.processExited = processExited;
  if (!createProcessSucceeded || !readFile) {
    return facts;
  }
  facts.startMarkerSeen = readFile(kStartMarkerFileName).has_value();
  const std::optional<std::string> result = readFile(kResultFileName);
  if (result.has_value()) {
    facts.resultParsed = ParseCommandWindowResult(*result, &facts.exitCode);
  }
  return facts;
}

CommandCompletion DecideCommandCompletion(const CommandWindowObservation& facts,
                                          long* outExitCode) noexcept {
  if (!facts.createProcessSucceeded) {
    return CommandCompletion::launchFailed;
  }
  if (facts.resultParsed) {
    if (outExitCode != nullptr) {
      *outExitCode = facts.exitCode;
    }
    // cmd 对“要调用的程序不存在”固定返回保留码 9009：脚本跑完了，但 Git 进程从未被创建
    // （典型场景是执行期间 git.exe 被移除）。其余退出码一律视为 Git 自己的回答。
    return facts.exitCode == kCommandNotFoundExitCode ? CommandCompletion::gitNotStarted
                                                      : CommandCompletion::finished;
  }
  if (!facts.processExited) {
    return facts.startMarkerSeen ? CommandCompletion::running : CommandCompletion::launched;
  }
  if (facts.startMarkerSeen) {
    return CommandCompletion::terminated;
  }
  // 进程已结束却没有 start.txt：cmd 从未执行到脚本第一行。
  // 可能是脚本无法被 cmd 运行，也可能是窗口在启动瞬间就被关掉 —— 两者都无法取得退出码，
  // 一律报“脚本未能执行”，绝不谎报成功，也不留在“执行中”。
  return CommandCompletion::scriptNeverRan;
}

}  // namespace gc::git
