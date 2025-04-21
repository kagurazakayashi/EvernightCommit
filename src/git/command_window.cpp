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

std::wstring TrimTrailingSpaces(std::wstring_view text) {
  size_t end = text.size();
  while (end > 0 && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
    --end;
  }
  return std::wstring(text.substr(0, end));
}

void AppendSpecField(std::wstring& text, std::wstring_view name, std::wstring_view value) {
  text.append(name);
  text.push_back(kFieldSeparator);
  text.append(value);
  text.append(kLineBreak);
}

// 开始标记与结果行都是“一行、TAB 分隔”的 ASCII 字节文本：与说明书同一种行形态，
// 但值只有口令与十进制退出码，因此这里不做任何码页或 UTF 处理。
constexpr std::string_view kStartMarkerKeyword = "start";
constexpr std::string_view kResultKeyword = "result";
constexpr char kByteFieldSeparator = '\t';
constexpr std::string_view kByteLineBreak = "\r\n";

// 取首行并按 TAB 拆分。没有行尾表示“对方还在写”，返回 false 让观察端继续等；
// 首行之后还残留别的内容也返回 false —— 一次操作只发布一条记录，多出来的一律视为不可信。
bool FirstLineFields(std::string_view content, std::vector<std::string_view>* fields) {
  fields->clear();
  const size_t newline = content.find('\n');
  if (newline == std::string_view::npos) {
    return false;
  }
  if (content.find_first_not_of("\r\n", newline + 1) != std::string_view::npos) {
    return false;
  }
  std::string_view line = content.substr(0, newline);
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  size_t cursor = 0;
  for (;;) {
    const size_t tab = line.find(kByteFieldSeparator, cursor);
    if (tab == std::string_view::npos) {
      fields->push_back(line.substr(cursor));
      break;
    }
    fields->push_back(line.substr(cursor, tab - cursor));
    cursor = tab + 1;
  }
  return true;
}

// 十进制整数（允许一个前导负号，CRT 语义下退出码原值写回）；超出 long 范围视为不合格。
bool ParseAsciiInteger(std::string_view text, long* outValue) {
  if (text.empty() || outValue == nullptr) {
    return false;
  }
  size_t index = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    index = 1;
  }
  if (index >= text.size()) {
    return false;
  }
  std::string digits;
  digits.reserve(text.size());
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c < '0' || c > '9') {
      return false;
    }
    digits.push_back(c);
  }
  errno = 0;
  const long magnitude = std::strtol(digits.c_str(), nullptr, 10);
  if (errno == ERANGE) {
    return false;
  }
  *outValue = negative ? -magnitude : magnitude;
  return true;
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
      return L"操作目录名不符合执行器生成的形态（GcOp<进程ID>x<随机段>）";
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
  // 执行器只用 “GcOp<进程ID>x<随机十六进制>” 这一种形态：左边是十进制进程号，
  // 右边是不可预测的小写十六进制随机段，整段不含分隔符、引号或通配字符。
  // 辅助进程与回收例程都据此核对“说明书/目录是本程序这次造出来的”。
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
  if (left.empty() || left.size() > kOperationDirectoryPidMaxLength) {
    return false;
  }
  if (right.size() < kOperationDirectoryRandomMinLength ||
      right.size() > kOperationDirectoryRandomMaxLength) {
    return false;
  }
  for (const wchar_t c : left) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
  }
  for (const wchar_t c : right) {
    if (IsAsciiDigit(c) || (c >= L'a' && c <= L'f')) {
      continue;
    }
    return false;  // 随机段只有小写十六进制这一种形态：大写、g-z、符号都不接受
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

bool NonceToAscii(std::wstring_view nonce, std::string* outBytes) {
  if (outBytes == nullptr || !IsSafeNonce(nonce)) {
    return false;
  }
  std::string bytes;
  bytes.reserve(nonce.size());
  for (const wchar_t c : nonce) {
    bytes.push_back(static_cast<char>(static_cast<unsigned char>(c)));  // IsSafeNonce 已保证 ASCII
  }
  *outBytes = std::move(bytes);
  return true;
}

std::string BuildCommandWindowStartMarkerText(std::string_view asciiNonce) {
  std::string text(kStartMarkerKeyword);
  text.push_back(kByteFieldSeparator);
  text.append(asciiNonce);
  text.append(kByteLineBreak);
  return text;
}

std::string BuildCommandWindowResultText(std::string_view asciiNonce, long exitCode) {
  std::string text(kResultKeyword);
  text.push_back(kByteFieldSeparator);
  text.append(asciiNonce);
  text.push_back(kByteFieldSeparator);
  text.append(std::to_string(exitCode));
  text.append(kByteLineBreak);
  return text;
}

bool ParseCommandWindowStartMarker(std::string_view content, std::string_view expectedAsciiNonce) {
  std::vector<std::string_view> fields;
  if (!FirstLineFields(content, &fields)) {
    return false;
  }
  return fields.size() == 2 && fields[0] == kStartMarkerKeyword && fields[1] == expectedAsciiNonce;
}

bool ParseCommandWindowResult(std::string_view content, std::string_view expectedAsciiNonce,
                              long* exitCode) {
  std::vector<std::string_view> fields;
  if (!FirstLineFields(content, &fields)) {
    return false;
  }
  if (fields.size() != 3 || fields[0] != kResultKeyword || fields[1] != expectedAsciiNonce) {
    return false;
  }
  return ParseAsciiInteger(fields[2], exitCode);
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
                                              bool createProcessSucceeded, bool processExited,
                                              std::string_view expectedAsciiNonce) {
  CommandWindowObservation facts;
  facts.createProcessSucceeded = createProcessSucceeded;
  facts.processExited = processExited;
  if (!createProcessSucceeded || !readFile) {
    return facts;
  }
  const std::optional<std::string> marker = readFile(kStartMarkerFileName);
  facts.startMarkerSeen =
      marker.has_value() && ParseCommandWindowStartMarker(*marker, expectedAsciiNonce);
  const std::optional<std::string> result = readFile(kResultFileName);
  if (result.has_value()) {
    facts.resultParsed = ParseCommandWindowResult(*result, expectedAsciiNonce, &facts.exitCode);
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
  // 进程已结束却没有合格的 start.txt（内容合格且口令相符）：辅助进程从未执行到“写开始标记”那一行，
  // 或者那个痕迹属于别的操作。
  // 可能是它没能被创建后运行，也可能是窗口在启动瞬间就被关掉 —— 两者都无法取得退出码，
  // 一律报“未能开始执行”，绝不谎报成功，也不留在“执行中”。
  return CommandCompletion::helperNeverStarted;
}

}  // namespace gc::git
