#include "git/command_window.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace gc::git {
namespace {

// 说明书的行分隔与字段分隔：字段名单词、值可以有任何“非控制字符”，
// 而制表与控制字符在校验里已被拒绝，因此按第一个 TAB 拆分不会歧义。
constexpr std::wstring_view kSpecMagic = L"evernight-command-window-spec";
constexpr wchar_t kFieldSeparator = L'\t';
constexpr std::wstring_view kLineBreak = L"\r\n";

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

bool IsAsciiDigit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

bool IsAsciiLetter(wchar_t c) noexcept {
  return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
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

// 校验“将要交给 CreateProcessW 命令行的一行”：引号区域之外不得出现 shell 元字符。
// 每个 “ 都被视作区域切换（与 CRT 的转义语义不同），因此关闭引号后若紧跟非空格字符，
// 说明存在字面量引号（可能被下游 shell/hooks 误判成区域切换），拒绝。
// 现在的执行链路里没有人再解析这一行（辅助进程直接把字符串交给 CreateProcessW），
// 保留这道判定是为了让“同一份输入在两层都合法”成为可测试的不变量，
// 也防止日后又有人把这些值拼进某条 shell 命令。
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

void AppendSpecField(std::wstring& text, std::wstring_view name, std::wstring_view value) {
  text.append(name);
  text.push_back(kFieldSeparator);
  text.append(value);
  text.append(kLineBreak);
}

[[nodiscard]] bool RejectAs(CommandPlanReject reason, std::wstring message, CommandPlanReject* reject,
                             std::wstring* detail) {
  if (reject != nullptr) {
    *reject = reason;
  }
  if (detail != nullptr) {
    *detail = std::move(message);
  }
  return false;
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
      return L"路径含双引号，无法安全交给命令窗口执行";
    case CommandPlanReject::controlCharacterInPath:
      return L"路径含控制字符，无法安全写入操作说明书";
    case CommandPlanReject::illegalArgument:
      return L"参数含双引号或控制字符";
    case CommandPlanReject::tooManyArguments:
      return L"参数数量超过上限";
    case CommandPlanReject::commandTooLong:
      return L"命令行长度超过上限";
    case CommandPlanReject::illegalNonce:
      return L"操作口令缺失或含不安全字符（只允许 ASCII 字母与数字）";
    case CommandPlanReject::illegalOperationDirectory:
      return L"操作目录名不符合执行器生成的形态（GcOp<进程ID>x<序号>）";
    case CommandPlanReject::illegalWindowTitle:
      return L"窗口标题含控制字符或制表符";
  }
  return L"未知原因";
}

bool BuildGitCommandLine(const CommandWindowOperation& operation, std::wstring* gitLine,
                         CommandPlanReject* reject, std::wstring* detail) {
  const auto fail = [&](CommandPlanReject reason, std::wstring message) {
    return RejectAs(reason, std::move(message), reject, detail);
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
    return fail(CommandPlanReject::quoteInPath, L"命令行存在无法安全交给命令窗口的引号区域：" + line);
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
  // 会改变 shell 行解析或说明书行形态的字符：引号、控制字符，以及 & | < > ^ % （% 还可能被
  // 当成变量引用）。中文、空格、= 与 ! 都是安全的字面量，一律原样保留。
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

bool BuildCommandWindowSpecText(const CommandWindowOperation& operation,
                               std::wstring_view directoryToken, std::wstring_view title,
                               std::wstring_view nonce, std::wstring* specText,
                               CommandPlanReject* reject, std::wstring* detail) {
  const auto fail = [&](CommandPlanReject reason, std::wstring message) {
    return RejectAs(reason, std::move(message), reject, detail);
  };
  if (specText == nullptr) {
    return fail(CommandPlanReject::emptyOperationId, L"内部错误：缺少输出对象");
  }
  specText->clear();

  // 第一步的校验原样复用：说明书里能出现的值，必须与界面展示、辅助进程将要执行的完全同源。
  std::wstring displayLine;
  if (!BuildGitCommandLine(operation, &displayLine, reject, detail)) {
    return false;
  }
  if (!IsSafeNonce(nonce)) {
    return fail(CommandPlanReject::illegalNonce,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::illegalNonce)) + L"：" +
                    std::wstring(nonce));
  }
  if (!IsSafeOperationDirectoryName(directoryToken)) {
    return fail(CommandPlanReject::illegalOperationDirectory,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::illegalOperationDirectory)) +
                    L"：" + std::wstring(directoryToken));
  }
  // 标题会原样进说明书并由辅助进程设置控制台标题：只禁止会破坏行形态的字符。
  // 中文、emoji 等“本机码页装不下”的字符不再有任何理由被拒绝——这条链路上没有码页。
  if (title.empty() || HasControl(title) || title.find(kFieldSeparator) != std::wstring::npos) {
    return fail(CommandPlanReject::illegalWindowTitle,
                std::wstring(CommandPlanRejectLabel(CommandPlanReject::illegalWindowTitle)) + L"：" +
                    std::wstring(title));
  }

  std::wstring text;
  text.reserve(displayLine.size() + title.size() + operation.repositoryDirectory.size() + 256);
  AppendSpecField(text, kSpecMagic, L"1");
  AppendSpecField(text, L"token", directoryToken);
  AppendSpecField(text, L"opid", operation.operationId);
  AppendSpecField(text, L"nonce", nonce);
  AppendSpecField(text, L"title", title);
  AppendSpecField(text, L"program", operation.gitExecutable);
  AppendSpecField(text, L"cwd", operation.repositoryDirectory);
  for (const std::wstring& argument : operation.arguments) {
    AppendSpecField(text, L"arg", argument);
  }
  *specText = std::move(text);
  return true;
}

bool ParseCommandWindowSpecText(std::wstring_view specText, CommandWindowSpec* outSpec,
                                std::wstring* failureReason) {
  const auto fail = [&](std::wstring message) {
    if (failureReason != nullptr) {
      *failureReason = std::move(message);
    }
    return false;
  };
  if (outSpec == nullptr) {
    return fail(L"内部错误：缺少说明书输出对象");
  }
  *outSpec = CommandWindowSpec();
  if (specText.empty()) {
    return fail(L"操作说明书为空");
  }

  CommandWindowSpec parsed;
  std::wstring cwd;
  bool versionChecked = false;
  size_t cursor = 0;
  size_t lineNumber = 0;
  while (cursor < specText.size()) {
    ++lineNumber;
    size_t stop = specText.find(L'\n', cursor);
    std::wstring_view line = specText.substr(cursor, stop == std::wstring_view::npos ? std::wstring_view::npos
                                                                                     : stop - cursor);
    cursor = stop == std::wstring_view::npos ? specText.size() : stop + 1;
    if (!line.empty() && line.back() == L'\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;  // 允许结尾换行与空行。
    }
    const size_t separator = line.find(kFieldSeparator);
    if (separator == std::wstring_view::npos) {
      return fail(L"操作说明书第 " + std::to_wstring(lineNumber) + L" 行缺少字段分隔符");
    }
    const std::wstring_view name = line.substr(0, separator);
    const std::wstring value(line.substr(separator + 1));
    if (line.find(kFieldSeparator, separator + 1) != std::wstring_view::npos) {
      return fail(L"操作说明书第 " + std::to_wstring(lineNumber) + L" 行有多个字段分隔符");
    }
    if (HasControl(value)) {
      return fail(L"操作说明书第 " + std::to_wstring(lineNumber) + L" 行含控制字符");
    }
    if (!versionChecked) {
      // 首行必须是魔数与版本号：版本不符就不猜语义，直接拒绝执行。
      if (name != kSpecMagic || value != L"1") {
        return fail(L"操作说明书格式版本不受支持");
      }
      versionChecked = true;
      continue;
    }
    if (name == L"token") {
      if (!parsed.directoryToken.empty()) {
        return fail(L"操作说明书有重复的 token 字段");
      }
      parsed.directoryToken = value;
    } else if (name == L"opid") {
      if (!parsed.operationId.empty()) {
        return fail(L"操作说明书有重复的 opid 字段");
      }
      parsed.operationId = value;
    } else if (name == L"nonce") {
      if (!parsed.nonce.empty()) {
        return fail(L"操作说明书有重复的 nonce 字段");
      }
      parsed.nonce = value;
    } else if (name == L"title") {
      if (!parsed.title.empty()) {
        return fail(L"操作说明书有重复的 title 字段");
      }
      parsed.title = value;
    } else if (name == L"program") {
      if (!parsed.gitExecutable.empty()) {
        return fail(L"操作说明书有重复的 program 字段");
      }
      parsed.gitExecutable = value;
    } else if (name == L"cwd") {
      if (!cwd.empty()) {
        return fail(L"操作说明书有重复的 cwd 字段");
      }
      cwd = value;
    } else if (name == L"arg") {
      parsed.arguments.push_back(value);
    } else {
      return fail(L"操作说明书含未知字段：" + std::wstring(name));
    }
  }

  if (!versionChecked) {
    return fail(L"操作说明书缺少首行格式标记");
  }
  if (parsed.directoryToken.empty() || parsed.operationId.empty() || parsed.nonce.empty() ||
      parsed.title.empty() || parsed.gitExecutable.empty() || cwd.empty()) {
    return fail(L"操作说明书缺少必要字段（token/opid/nonce/title/program/cwd）");
  }
  outSpec->directoryToken = std::move(parsed.directoryToken);
  outSpec->operationId = std::move(parsed.operationId);
  outSpec->nonce = std::move(parsed.nonce);
  outSpec->title = std::move(parsed.title);
  outSpec->gitExecutable = std::move(parsed.gitExecutable);
  outSpec->workingDirectory = std::move(cwd);
  outSpec->arguments = std::move(parsed.arguments);
  return true;
}

bool IsSafeOperationDirectoryName(std::wstring_view directoryName) {
  // 执行器只用 “GcOp<进程ID>x<序号>” 这一种形态：前后都必须是十进制数字，
  // 且整段不含分隔符、引号或通配字符，辅助进程据此核对说明书与目录是同一件事。
  if (directoryName.size() <= kOperationDirectoryPrefix.size() + 2 ||
      directoryName.compare(0, kOperationDirectoryPrefix.size(), kOperationDirectoryPrefix) != 0) {
    return false;
  }
  std::wstring_view body = directoryName.substr(kOperationDirectoryPrefix.size());
  const size_t separator = body.find(L'x');
  if (separator == std::wstring_view::npos || body.find(L'x', separator + 1) != std::wstring_view::npos) {
    return false;
  }
  const std::wstring_view left = body.substr(0, separator);
  const std::wstring_view right = body.substr(separator + 1);
  if (left.empty() || right.empty() || left.size() > 10 || right.size() > 20) {
    return false;
  }
  for (const wchar_t c : left) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
  }
  for (const wchar_t c : right) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
  }
  return true;
}

bool IsSafeNonce(std::wstring_view nonce) {
  if (nonce.size() < 8 || nonce.size() > kMaxNonceLength) {
    return false;
  }
  for (const wchar_t c : nonce) {
    if (!IsAsciiLetter(c) && !IsAsciiDigit(c)) {
      return false;
    }
  }
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
      return L"已启动，等待命令窗口就绪";
    case CommandCompletion::running:
      return L"执行中";
    case CommandCompletion::finished:
      return L"执行完成";
    case CommandCompletion::gitNotStarted:
      return L"Git 未能启动（命令窗口执行完毕但没有调用记录）";
    case CommandCompletion::terminated:
      return L"结果未知（命令窗口被提前关闭或 Git 进程被终止）";
    case CommandCompletion::helperNeverStarted:
      return L"启动失败（命令窗口辅助进程未能开始执行）";
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
    // 保留码 9009 是“要调用的程序不存在”的专用回答：命令跑完了，但 Git 进程从未被创建
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
  // 进程已结束却没有 start.txt：辅助进程从未执行到“写开始标记”那一行。
  // 可能是它没能被创建后运行，也可能是窗口在启动瞬间就被关掉 —— 两者都无法取得退出码，
  // 一律报“未能开始执行”，绝不谎报成功，也不留在“执行中”。
  return CommandCompletion::helperNeverStarted;
}

}  // namespace gc::git
