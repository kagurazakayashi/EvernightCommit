#include "git/shell_text.h"

#include <string>
#include <vector>

namespace gc::git {
namespace {

constexpr wchar_t kSpace = L' ';

// cmd 的双引号内仍是元字符 / 会被二次处理的字符，包裹也救不回来：
//   %  —— 变量展开（`%NAME%` 在引号内照样展开），翻倍成 `%%` 只在批处理里才是转义，
//        交互行里没有可靠的「字面 %」写法，除非改数据；
//   !  —— 延迟展开开启时会被二次展开，而这个开关本程序无法替用户确认；
//   "  —— 引号本身：cmd 的参数里没有「引号内的引号」的可移植写法。
// 这三个字符出现时不提供粘贴文本，改为复制结构化信息（由调用方决定措辞），
// 因为任何「换一个写法」的做法都在改用户的数据。
bool CmdUnrepresentable(wchar_t c, std::wstring_view* why) {
  if (c == L'"') {
    *why = L"含双引号：cmd 的引用形态里没法表达引号本身";
    return true;
  }
  if (c == L'%') {
    *why = L"含 %：交互式 cmd 会把它当作变量展开的边界（引号内也展开）";
    return true;
  }
  if (c == L'!') {
    *why = L"含 !：启用延迟扩展的窗口里会被二次展开，而这个状态本程序无法替你确认";
    return true;
  }
  if (c <= 0x20 && c != kSpace) {
    *why = L"含控制字符（含制表与换行），一行命令里表达不出来";
    return true;
  }
  if (c == 0x7F) {
    *why = L"含删除符（0x7F）";
    return true;
  }
  return false;
}

// cmd 的双引号内会被当字面量、但裸露时会被当语法的字符：出现就要整体加引号。
bool CmdNeedsQuoting(std::wstring_view token) {
  for (const wchar_t c : token) {
    if (c == kSpace || c == L'&' || c == L'|' || c == L'<' || c == L'>' || c == L'(' ||
        c == L')' || c == L'^' || c == L';' || c == L'=' || c == L',' || c == L'`' ||
        c == L'\'' || c == L'{' || c == L'}' || c == L'[' || c == L']') {
      return true;
    }
  }
  return false;
}

// PowerShell 的单引号字符串按字面处理（`$`、`&`、`%`、`!` 都不展开），
// 唯一要转义的是单引号本身（翻倍写）。控制字符仍然无法放进一行粘贴里。
bool PowerShellUnrepresentable(wchar_t c, std::wstring_view* why) {
  if (c <= 0x20 && c != kSpace) {
    *why = L"含控制字符（含制表与换行），一行命令里表达不出来";
    return true;
  }
  if (c == 0x7F) {
    *why = L"含删除符（0x7F）";
    return true;
  }
  return false;
}

std::wstring QuoteForPowerShell(std::wstring_view token) {
  std::wstring quoted = L"'";
  for (const wchar_t c : token) {
    if (c == L'\'') {
      quoted += L"''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted += L"'";
  return quoted;
}

// cmd 的引号里，反斜杠是字面量，但**结尾**的反斜杠会把紧跟的闭引号当成转义对象
// （`"abc\"` 不是参数 `abc\`），也没有可移植的写法能同时保住结尾反斜杠和闭引号。
// 所以「以反斜杠结尾 + 需要加引号」这一组合直接判为不能表达。
bool TokenNeedsQuotesButEndsWithBackslash(std::wstring_view token) {
  return CmdNeedsQuoting(token) && !token.empty() && token.back() == L'\\';
}

struct Reject {
  bool rejected = false;
  std::wstring reason;
};

Reject RejectForCmd(std::wstring_view token, std::wstring_view label) {
  if (token.empty()) {
    return Reject{true, std::wstring(label) + L"里有一段是空参数：粘贴进 cmd 后它会消失，"
                                          L"参数位置就变了，因此不提供粘贴文本"};
  }
  for (const wchar_t c : token) {
    std::wstring_view why;
    if (CmdUnrepresentable(c, &why)) {
      return Reject{true, std::wstring(label) + L"「" + std::wstring(token) + L"」" + std::wstring(why)};
    }
  }
  if (TokenNeedsQuotesButEndsWithBackslash(token)) {
    return Reject{true,
                  std::wstring(label) + L"「" + std::wstring(token) +
                      L"」以反斜杠结尾又必须加引号：cmd 里结尾反斜杠会吃掉闭引号"};
  }
  return Reject{};
}

std::wstring RenderCmdToken(std::wstring_view token) {
  if (!CmdNeedsQuoting(token)) {
    return std::wstring(token);
  }
  return L'"' + std::wstring(token) + L'"';
}

}  // namespace

std::wstring_view ShellDialectLabel(ShellDialect dialect) noexcept {
  switch (dialect) {
    case ShellDialect::cmdInteractive:
      return L"cmd（命令提示符）";
    case ShellDialect::powershell:
      return L"PowerShell";
  }
  return L"cmd（命令提示符）";
}

ShellCommandText FormatShellCommand(std::wstring_view executable,
                                    const std::vector<std::wstring>& arguments,
                                    ShellDialect dialect) {
  ShellCommandText result;
  if (arguments.empty()) {
    result.refusal = L"参数表是空的：一个参数都没有的行不构成任何一条命令，不提供粘贴文本。";
    return result;
  }
  std::wstring line;
  if (dialect == ShellDialect::cmdInteractive) {
    const Reject exeReject = RejectForCmd(executable.empty() ? std::wstring_view(L"git") : executable,
                                          L"程序路径");
    if (exeReject.rejected) {
      result.refusal = exeReject.reason;
      return result;
    }
    line = RenderCmdToken(executable.empty() ? std::wstring_view(L"git") : executable);
    for (size_t index = 0; index < arguments.size(); ++index) {
      const Reject reject = RejectForCmd(arguments[index], L"第 " + std::to_wstring(index + 1) + L" 个参数");
      if (reject.rejected) {
        result.refusal = reject.reason;
        return result;
      }
      line.push_back(kSpace);
      line += RenderCmdToken(arguments[index]);
    }
    result.expressible = true;
    result.text = std::move(line);
    result.note =
        L"这一行按交互式 cmd 的规则编排：含空格或 cmd 语法字符的段整体加双引号，"
        L"分支名与路径都保持原样、没有任何字符被替换。";
    return result;
  }

  // PowerShell：单引号字符串按字面处理，因此 cmd 那三个「救不回来」的字符在这里都能表达。
  const std::wstring exeToken = executable.empty() ? std::wstring(L"git") : std::wstring(executable);
  for (const wchar_t c : exeToken) {
    std::wstring_view why;
    if (PowerShellUnrepresentable(c, &why)) {
      result.refusal = std::wstring(L"程序路径") + std::wstring(why);
      return result;
    }
  }
  line = QuoteForPowerShell(exeToken);
  for (size_t index = 0; index < arguments.size(); ++index) {
    const std::wstring& token = arguments[index];
    if (token.empty()) {
      result.refusal = L"第 " + std::to_wstring(index + 1) +
                       L" 个参数是空串：粘贴后位置会变，因此不提供粘贴文本。";
      return result;
    }
    for (const wchar_t c : token) {
      std::wstring_view why;
      if (PowerShellUnrepresentable(c, &why)) {
        result.refusal =
            L"第 " + std::to_wstring(index + 1) + L" 个参数「" + token + std::wstring(why);
        return result;
      }
    }
    line.push_back(kSpace);
    line += QuoteForPowerShell(token);
  }
  result.expressible = true;
  result.text = std::move(line);
  result.note = L"这一行按 PowerShell 的规则编排：每段都用单引号按字面包裹，"
                L"分支名与路径保持原样、没有任何字符被替换。";
  return result;
}

std::wstring FormatCommandPreview(std::wstring_view executable,
                                  const std::vector<std::wstring>& arguments) {
  // 预览只用来「看清楚会跑什么」：每段用 [ ] 框住，边界一眼可见，不带任何 shell 语义，
  // 因此它绝不能被当成可粘贴的命令——那是 FormatShellCommand 的事。
  std::wstring text;
  text += executable.empty() ? std::wstring(L"git") : std::wstring(executable);
  for (const std::wstring& argument : arguments) {
    text += L" [";
    text += argument;
    text += L"]";
  }
  return text;
}

std::wstring FormatStructuredCommandFacts(std::wstring_view executable,
                                          const std::vector<std::wstring>& arguments) {
  std::wstring text = L"程序：";
  text += executable.empty() ? std::wstring(L"git（由终端按 PATH 找）") : std::wstring(executable);
  text += L"\n参数（逐条，按顺序）：";
  if (arguments.empty()) {
    text += L"（一个都没有）";
    return text;
  }
  for (size_t index = 0; index < arguments.size(); ++index) {
    text += L"\n  " + std::to_wstring(index + 1) + L") [" + arguments[index] + L"]";
  }
  return text;
}

}  // namespace gc::git
