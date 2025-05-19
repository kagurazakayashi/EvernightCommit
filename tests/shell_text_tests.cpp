// git/shell_text.h 的纯逻辑测试：一条命令的三种表示必须分开，而「能不能粘」按目标 shell 的
// 真实规则判定。存在的理由（R3）：Git 允许分支名里带 cmd 的语法字符（实测合法名
// `refs/heads/demo&calc&rem`），把这种名字裸拼进行命令文本后，`&` 就不再是分支名——
// 风险落在用户粘贴执行的那一刻。这里只用无害的 argv 断言，绝不执行任何 shell 或外部程序。
#include <string>
#include <vector>

#include "git/shell_text.h"
#include "support/tiny_test.h"

namespace {

using gc::git::FormatCommandPreview;
using gc::git::FormatShellCommand;
using gc::git::FormatStructuredCommandFacts;
using gc::git::ShellCommandText;
using gc::git::ShellDialect;

const std::wstring kRepo = L"P:\\仓库\\proj";

std::vector<std::wstring> Args(std::vector<std::wstring> values) { return values; }

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

}  // namespace

GC_TEST(shell_text_cmd_quotes_metacharacters_instead_of_dropping_them) {
  // 不需要引号的段保持裸形：简单路径/选项粘出去最接近人眼看到的命令。
  const ShellCommandText plain = FormatShellCommand(
      L"git", Args({L"-C", kRepo, L"update-ref", L"--no-deref", L"refs/heads/main"}),
      ShellDialect::cmdInteractive);
  GC_REQUIRE(plain.expressible, "常规参数应可表达");
  GC_CHECK(Contains(plain.text, L"update-ref --no-deref refs/heads/main"));
  GC_CHECK(Contains(plain.text, L"-C"));

  // 需要引号的段整体加引号：空格、中文、cmd 的语法字符（& | < > ^ ( ) = , ; 反引号）。
  const ShellCommandText meta =
      FormatShellCommand(L"git",
                         Args({L"-C", L"P:\\我 的 仓库", L"update-ref", L"refs/heads/demo&calc&rem",
                               L"refs/heads/a|b", L"refs/heads/c(d)", L"refs/heads/e^f",
                               L"refs/heads/g,h;i=j"}),
                         ShellDialect::cmdInteractive);
  GC_REQUIRE(meta.expressible, "这些字符在 cmd 的双引号内都是字面量，应当能表达");
  GC_CHECK(Contains(meta.text, L"\"P:\\我 的 仓库\""));
  GC_CHECK(Contains(meta.text, L"\"refs/heads/demo&calc&rem\""));
  GC_CHECK(Contains(meta.text, L"\"refs/heads/a|b\""));
  GC_CHECK(Contains(meta.text, L"\"refs/heads/c(d)\""));
  GC_CHECK(Contains(meta.text, L"\"refs/heads/e^f\""));
  GC_CHECK(Contains(meta.text, L"\"refs/heads/g,h;i=j\""));
  // 关键：数据一个字符都没被改动（不静默替换、不删、不转义成别的值）。
  GC_CHECK(Contains(meta.text, L"demo&calc&rem"));
  GC_CHECK(Contains(meta.text, L"a|b"));
}

GC_TEST(shell_text_cmd_refuses_what_it_cannot_quote) {
  // % 在引号内仍会被当变量展开的边界；! 在启用延迟扩展的窗口里会二次展开——
  // 本程序无法确认用户那个状态，所以两者都不猜写法，只拒绝。
  const ShellCommandText percent =
      FormatShellCommand(L"git", Args({L"-C", kRepo, L"refs/heads/100%DONE%"}),
                         ShellDialect::cmdInteractive);
  GC_CHECK(!percent.expressible);
  GC_CHECK(percent.text.empty());
  GC_CHECK(Contains(percent.refusal, L"%"));

  const ShellCommandText bang =
      FormatShellCommand(L"git", Args({L"refs/heads/a!b"}), ShellDialect::cmdInteractive);
  GC_CHECK(!bang.expressible);
  GC_CHECK(Contains(bang.refusal, L"!"));

  // 引号本身：cmd 的引用形态里没法表达「引号里的引号」。
  const ShellCommandText quote =
      FormatShellCommand(L"git", Args({L"refs/heads/a\"b"}), ShellDialect::cmdInteractive);
  GC_CHECK(!quote.expressible);

  // 结尾反斜杠 + 需要加引号：`"…\"` 里那个闭引号会被当转义对象，粘出去意思就变了。
  const ShellCommandText trailing = FormatShellCommand(
      L"git", Args({L"P:\\目录 (x)\\"}), ShellDialect::cmdInteractive);
  GC_CHECK(!trailing.expressible);
  GC_CHECK(Contains(trailing.refusal, L"反斜杠"));
  // 不需要加引号的结尾反斜杠（裸路径）不在此列：原样粘出去是对的。
  const ShellCommandText trailingBare =
      FormatShellCommand(L"git", Args({L"P:\\repo"}), ShellDialect::cmdInteractive);
  GC_CHECK(trailingBare.expressible);

  // 控制字符与空参数：一行命令表达不了，也不允许「省略掉算了」。
  GC_CHECK(!FormatShellCommand(L"git", Args({L"refs/heads/a\nb"}), ShellDialect::cmdInteractive)
                .expressible);
  GC_CHECK(!FormatShellCommand(L"git", Args({L"refs/heads/a\tb"}), ShellDialect::cmdInteractive)
                .expressible);
  GC_CHECK(!FormatShellCommand(L"git", Args({L""}), ShellDialect::cmdInteractive).expressible);
  GC_CHECK(!FormatShellCommand(L"git", Args({}), ShellDialect::cmdInteractive).expressible);
}

GC_TEST(shell_text_powershell_handles_what_cmd_cannot) {
  // PowerShell 的单引号字符串按字面处理：% 与 ! 都能表达，' 靠翻倍写。
  const ShellCommandText percent = FormatShellCommand(
      L"git", Args({L"-C", kRepo, L"refs/heads/100%DONE%", L"refs/heads/a!b"}),
      ShellDialect::powershell);
  GC_REQUIRE(percent.expressible, "PowerShell 应能表达 % 与 !");
  GC_CHECK(Contains(percent.text, L"'refs/heads/100%DONE%'"));
  GC_CHECK(Contains(percent.text, L"'refs/heads/a!b'"));

  const ShellCommandText apostrophe =
      FormatShellCommand(L"git", Args({L"it's a branch"}), ShellDialect::powershell);
  GC_REQUIRE(apostrophe.expressible, "单引号翻倍后应可表达");
  GC_CHECK(Contains(apostrophe.text, L"'it''s a branch'"));

  // 控制字符仍然拒绝；结尾反斜杠在 PowerShell 里反而是问题（`'…\'` 里是字面量，能表达）。
  GC_CHECK(!FormatShellCommand(L"git", Args({L"a\nb"}), ShellDialect::powershell).expressible);
  GC_CHECK(FormatShellCommand(L"git", Args({L"P:\\目录 (x)\\"}), ShellDialect::powershell)
               .expressible);
}

GC_TEST(shell_text_formats_never_mutate_the_argument_data) {
  // 三种表示都从同一份数据数组出发，且绝不回写：GUI 链路送进 CreateProcessW 的还是原值。
  std::vector<std::wstring> arguments{L"-C", kRepo, L"update-ref", L"refs/heads/a%b",
                                      L"1111111111111111111111111111111111111111"};
  const std::vector<std::wstring> before = arguments;
  const ShellCommandText cmd = FormatShellCommand(L"git", arguments, ShellDialect::cmdInteractive);
  const ShellCommandText ps = FormatShellCommand(L"git", arguments, ShellDialect::powershell);
  const std::wstring preview = FormatCommandPreview(L"git", arguments);
  const std::wstring facts = FormatStructuredCommandFacts(L"git", arguments);
  GC_CHECK(arguments == before);
  GC_CHECK(!cmd.expressible);  // cmd 拒了这条，但数据没有被改
  GC_CHECK(ps.expressible);
  GC_CHECK(Contains(preview, L"[refs/heads/a%b]"));
  GC_CHECK(Contains(facts, L"refs/heads/a%b"));
  GC_CHECK(Contains(facts, L"4)"));
}
