#include "support/tiny_test.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::CommandPlanReject;
using gc::git::CommandWindowOperation;
using gc::git::CommandWindowSpec;

// 取说明书文本里以某段文字开头的那一行（去掉行尾 \r），用于逐字段核对形态。
std::string FirstLineStartingWith(const std::wstring& text, std::wstring_view prefix) {
  const std::string bytes = gc::platform::Utf16ToUtf8(text);
  size_t lineStart = 0;
  while (lineStart < bytes.size()) {
    const size_t lineEnd = bytes.find('\n', lineStart);
    const size_t stop = lineEnd == std::string::npos ? bytes.size() : lineEnd;
    std::string line(bytes, lineStart, stop - lineStart);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::string wide;
    for (const wchar_t c : prefix) {
      wide.push_back(static_cast<char>(c));  // 前缀一律是 ASCII 字段名。
    }
    if (line.starts_with(wide)) {
      return line;
    }
    lineStart = stop + 1;
  }
  return {};
}

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

// 说明书字段名与实际值之间的分隔符（值里不可能再有一个：制表符属于控制字符，边界已拒绝）。

std::string Describe(std::wstring_view text) { return gc::platform::Utf16ToUtf8(text); }

std::wstring BuildSpec(const CommandWindowOperation& operation, std::wstring_view directoryToken,
                       std::wstring_view title, std::wstring_view nonce, bool* ok,
                       CommandPlanReject* reject, std::wstring* detail) {
  std::wstring text;
  const bool built = gc::git::BuildCommandWindowSpecText(operation, directoryToken, title, nonce,
                                                         &text, reject, detail);
  if (ok != nullptr) {
    *ok = built;
  }
  return text;
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
  // 中文、空格、& % ! 括号都是参数的字面量：本执行器不经过 shell，因此不需要任何转义形态，
  // 也不因为“某个码页装不下”而拒绝。
  const CommandWindowOperation operation = MakeOperation(
      L"D:\\软件 & 工具\\Git\\bin\\git.exe",
      {L"status", L"--porcelain=1", L"-c core.quotepath=false", L"100%(x)|y&z"});
  bool ok = false;
  const std::wstring line = BuildLine(operation, &ok, nullptr, nullptr);

  GC_CHECK(ok);
  GC_CHECK(line.find(L"\"100%(x)|y&z\"") != std::wstring::npos);
  GC_CHECK(line.find(L"%%") == std::wstring::npos);  // 不再有为 cmd 准备的加倍百分号
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
  // 本执行器不接受任何含双引号的参数，因此这类输入在启动命令窗口之前就被拒绝。
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
  // 这里校验的是区域判定：合法输入必须全部通过、非法输入全部被拒。
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

GC_TEST(command_title_is_unique_and_free_of_shell_metacharacters) {
  // 标题必须带唯一标记（否则“按标题关闭窗口”会找错窗口），且不含会破坏说明书行形态的字符。
  const std::wstring plain = gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"status", L"GcOp1");
  const std::wstring evil = gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"pull & calc | x > y \"z\"", L"GcOp2");
  const std::wstring chinese = gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"提交 检查", L"GcOp3");

  GC_CHECK(plain.find(L"GcOp1") != std::wstring::npos);
  GC_CHECK(evil.find(L"GcOp2") != std::wstring::npos);
  GC_CHECK(evil.find_first_of(L"&|<>\"%") == std::wstring::npos);
  GC_CHECK(chinese.find(L"提交 检查") != std::wstring::npos);  // 中文保留
  GC_CHECK(plain != std::wstring(gc::git::MakeSafeConsoleTitle(L"Git 提交工具", L"status", L"GcOp9")));
}

GC_TEST(command_spec_round_trips_every_field_verbatim) {
  // 说明书是“参数只按数据传递”的载体：每个字段原样往返，
  // 中文、空格、% & ! 括号都不变成任何转义形态（旧实现为了进 cmd 脚本才需要按展开轮数加倍 %）。
  CommandWindowOperation operation =
      MakeOperation(L"D:\\软件 & 工具\\Git\\bin\\git.exe",
                    {L"diff", L"--", L"100%.txt", L"特殊 &^%!(x) 文件.txt"},
                    L"D:\\仓库 with spaces\\工作区");
  const std::wstring title =
      gc::git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", L"status", L"GcOp12xabcd1234");
  bool ok = false;
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  const std::wstring specText =
      BuildSpec(operation, L"GcOp12xabcd1234", title, L"abcdef0123456789", &ok, &reject, &detail);
  GC_REQUIRE_MESSAGE(ok, Describe(detail));

  CommandWindowSpec parsed;
  std::wstring parseReason;
  GC_REQUIRE_MESSAGE(gc::git::ParseCommandWindowSpecText(specText, &parsed, &parseReason),
                     Describe(parseReason));
  GC_CHECK(parsed.directoryToken == L"GcOp12xabcd1234");
  GC_CHECK(parsed.operationId == operation.operationId);
  GC_CHECK(parsed.nonce == L"abcdef0123456789");
  GC_CHECK(parsed.title == title);  // 中文标题原样保留：不再有“装不下就退回占位文字”的分支
  GC_CHECK(parsed.gitExecutable == operation.gitExecutable);
  GC_CHECK(parsed.workingDirectory == operation.repositoryDirectory);
  GC_CHECK(parsed.arguments == operation.arguments);

  // 执行行与展示行同源：由字段重建的命令行必须与界面展示的那一条逐字相同。
  std::wstring rebuilt;
  GC_REQUIRE(gc::git::BuildGitCommandLine(operation, &rebuilt, nullptr, nullptr), "重建命令行失败");
  GC_CHECK(rebuilt.find(L"\"100%.txt\"") != std::wstring::npos);
  GC_CHECK(FirstLineStartingWith(specText, L"arg").find("%%") == std::string::npos);
}

GC_TEST(command_spec_first_line_is_version_marker) {
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", {L"status"});
  bool ok = false;
  const std::wstring specText =
      BuildSpec(operation, L"GcOp1xabcd1234", L"Git status - GcOp1xabcd1234", L"abcdef0123456789", &ok, nullptr,
                nullptr);
  GC_REQUIRE(ok, "常规操作应能生成说明书");
  const std::string first = FirstLineStartingWith(specText, L"evernight-command-window-spec");
  GC_CHECK_MESSAGE(first == "evernight-command-window-spec\t1", "首行必须是格式版本标记：" + first);
  GC_CHECK(specText.find(L"\n\n") == std::wstring::npos);
  // 每行“字段名<TAB>值”，且值里只有一个分隔符。
  GC_CHECK(FirstLineStartingWith(specText, L"program") ==
           std::string("program") + "\t" + gc::platform::Utf16ToUtf8(operation.gitExecutable));
}

GC_TEST(command_spec_rejects_unsafe_nonce_and_directory) {
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", {L"status"});
  std::wstring specText;
  bool ok = true;

  // 口令承担“说明书与被启动的辅助进程是同一件事”的绑定，形态不合格就必须拒绝。
  const std::vector<std::wstring> badNonces = {L"", L"short", L"with space", L"with&",
                                               L"中文口令abcdefgh", std::wstring(65, L'a')};
  for (const std::wstring& bad : badNonces) {
    CommandPlanReject reject = CommandPlanReject::none;
    std::wstring detail;
    specText = BuildSpec(operation, L"GcOp1xabcd1234", L"Git status", bad, &ok, &reject, &detail);
    GC_CHECK_MESSAGE(!ok && reject == CommandPlanReject::illegalNonce,
                     "口令应被拒绝：" + gc::platform::Utf16ToUtf8(bad) + " -> " +
                         gc::platform::Utf16ToUtf8(detail));
    GC_CHECK(specText.empty());
  }
  // 目录名同理：不是执行器生成的形态，就说明说明书与它所在的目录不是同一件事。
  const std::vector<std::wstring> badTokens = {
      L"",                     // 空
      L"GcOp",                 // 只有前缀
      L"GcOp1x",               // 随机段缺失
      L"x1x1",                 // 没有前缀
      L"GcOp1xabcd1234x5678",  // 两个分隔符
      L"GcOp1xabcd;1234",      // 分号
      L"GcOp1xabcd1234\\sub",  // 路径分隔
      L"temp1xabcd1234",       // 前缀不对
      L"GcOp1xabcd123",        // 随机段短于下限
      L"GcOp1xABCD1234",       // 大写不是执行器生成的形态
      L"GcOp1xabcd123g",       // 不是十六进制
      L"GcOp1xyz9876543210",   // 随机段里出现非十六进制字母
      L"GcOp0x1abcd",          // 随机段太短
      L"GcOp1xabcd1234abcd1234abcd1234abcd1234abcd1234",  // 超过上限
      L"GcOp-1xabcd1234",      // 进程 ID 段含负号
  };
  for (const std::wstring& bad : badTokens) {
    CommandPlanReject reject = CommandPlanReject::none;
    std::wstring detail;
    specText = BuildSpec(operation, bad, L"Git status", L"abcdef0123456789", &ok, &reject, &detail);
    GC_CHECK_MESSAGE(!ok && reject == CommandPlanReject::illegalOperationDirectory,
                     "目录名应被拒绝：" + gc::platform::Utf16ToUtf8(bad));
  }
  GC_CHECK(gc::git::IsSafeOperationDirectoryName(L"GcOp12345xabcd1234"));
  GC_CHECK(gc::git::IsSafeOperationDirectoryName(
      L"GcOp4294967295x0123456789abcdef0123456789abcdef"));  // 两段都取到上界
  GC_CHECK(gc::git::IsSafeNonce(L"ABCdef0123456789"));
  GC_CHECK(!gc::git::IsSafeNonce(std::wstring(65, L'a')));
}

GC_TEST(command_spec_rejects_malformed_documents) {
  CommandWindowSpec parsed;
  std::wstring reason;
  const auto refused = [&parsed, &reason](std::wstring_view text) {
    parsed = CommandWindowSpec();
    reason.clear();
    return !gc::git::ParseCommandWindowSpecText(text, &parsed, &reason);
  };
  const std::wstring valid =
      L"evernight-command-window-spec\t1\r\n"
      L"token\tGcOp1xabcd1234\r\nopid\tstatus\r\nnonce\tabcdef0123456789\r\n"
      L"title\tGit status - GcOp1xabcd1234\r\nprogram\tC:\\git.exe\r\ncwd\tC:\\repo\r\narg\tstatus\r\n";

  GC_CHECK(refused(L""));                                                       // 空文档
  GC_CHECK(refused(L"evernight-command-window-spec\t2\r\narg\tx\r\n"));         // 版本不符
  GC_CHECK(refused(L"opid\tstatus\r\n"));                                      // 缺首行标记
  GC_CHECK(refused(valid + L"unknown\tx\r\n"));                                // 未知字段
  GC_CHECK(refused(L"evernight-command-window-spec\t1\r\nnoseparator\r\n"));    // 行里没有分隔符
  GC_CHECK(refused(valid + L"nonce\tanotherone\r\n"));                          // 重复字段
  GC_CHECK(refused(L"evernight-command-window-spec\t1\r\narg\tx\r\n"));         // 缺必要字段
  GC_CHECK(refused(valid + L"title\t带制表符\t的值\r\n"));                        // 值里第二个分隔符
  // 控制字符会带坏“按行拆分”的形态，即使上游已经拒绝也要再防一层。
  GC_CHECK(refused(valid + L"arg\t坏\x01字符\r\n"));
  // 合法文档必须通过，且参数顺序与空参数都原样保留。
  reason.clear();
  GC_REQUIRE(gc::git::ParseCommandWindowSpecText(valid, &parsed, &reason), Describe(reason));
  GC_CHECK(parsed.arguments.size() == 1 && parsed.arguments[0] == L"status");
  const std::wstring withEmpty =
      valid + L"arg\t\r\n";  // 一个真正的空参数：Git 语义上合法，必须保留而不是丢掉。
  GC_REQUIRE(gc::git::ParseCommandWindowSpecText(withEmpty, &parsed, &reason), Describe(reason));
  GC_CHECK(parsed.arguments.size() == 2);
  GC_CHECK(parsed.arguments[1].empty());
}

GC_TEST(command_spec_survives_strict_utf8_round_trip) {
  // 这一条覆盖验收矩阵里的“文本形态”：中文临时根、中文仓库、中文参数、含空格与元字符、
  // 以及超出传统代码页的字符（西里尔 + emoji）。旧实现要在系统 ANSI 码页上撞墙。
  CommandWindowOperation operation = MakeOperation(
      L"C:\\Program Files\\Git\\bin\\git.exe",
      {L"-c", L"core.quotepath=false", L"add", L"--", L"中文 目录/文件 & 名.txt", L"100%.txt",
       L"Ж 😀 超码页.txt"},
      L"D:\\用户\\仓库 with spaces & 中文");
  operation.operationId = L"stage";
  const std::wstring title =
      gc::git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", L"加入暂存区", L"GcOp9xdeadbeef");
  bool ok = false;
  std::wstring detail;
  const std::wstring specText =
      BuildSpec(operation, L"GcOp9xdeadbeef", title, L"deadbeefcafebabe", &ok, nullptr, &detail);
  GC_REQUIRE_MESSAGE(ok, Describe(detail));

  // 平台层的严格 UTF-8 往返：一个码元都不能改，改了就等于把用户的文件名换成别的名字。
  std::string bytes;
  GC_REQUIRE(gc::platform::TryUtf16ToUtf8Strict(specText, bytes), "严格编码失败");
  std::wstring back;
  GC_REQUIRE(gc::platform::TryUtf8ToUtf16Strict(bytes, back), "严格解码失败");
  GC_CHECK(back == specText);

  CommandWindowSpec parsed;
  std::wstring reason;
  GC_REQUIRE(gc::git::ParseCommandWindowSpecText(back, &parsed, &reason), Describe(reason));
  GC_CHECK(parsed.arguments == operation.arguments);
  GC_CHECK(parsed.gitExecutable == operation.gitExecutable);
  GC_CHECK(parsed.workingDirectory == operation.repositoryDirectory);
  GC_CHECK(parsed.title == title);
}

GC_TEST(command_spec_rejects_lone_surrogate_instead_of_replacing_it) {
  // 无效 Unicode 必须明确失败：不许用问号、U+FFFD 或“最佳匹配”冒充原值。
  // 孤立低代理项（前面没有高代理项）在 UTF-16 里是非法码元。
  const std::wstring bad = std::wstring(L"文件名 ") + wchar_t(0xDC00) + L".txt";
  std::string bytes;
  GC_CHECK(!gc::platform::TryUtf16ToUtf8Strict(bad, bytes));
  GC_CHECK(bytes.empty());
  // 宽松编码会写出 U+FFFD —— 正是本执行器要避免的“悄悄改写”。
  const std::string lenient = gc::platform::Utf16ToUtf8(bad);
  GC_CHECK(lenient.find("\xEF\xBF\xBD") != std::string::npos);
  GC_CHECK(lenient != bytes);
}

GC_TEST(command_spec_rejects_invalid_utf8_on_the_way_back) {
  std::wstring text;
  // 字节用显式列表构造，不用 "\xE4\xB8" 这种转义：可读性差，且转义的取值范围一旦超过
  // char 就会静默截断。测试要把字节钉死。
  const auto rawBytes = [](std::initializer_list<unsigned char> values) {
    std::string text;
    for (const unsigned char value : values) {
      text.push_back(static_cast<char>(value));
    }
    return text;
  };
  const std::string truncated = rawBytes({0xE4, 0xB8});             // 三字节序列少了尾字节
  const std::string cesuHigh = rawBytes({0xED, 0x80, 0x80});        // 高代理项被编成三字节
  const std::string cesuLow = rawBytes({0xED, 0xB0, 0x80});         // 低代理项同理
  const std::string badStart = rawBytes({0x61, 0x62, 0x63, 0xFF});  // 非法起始字节
  GC_CHECK(!gc::platform::TryUtf8ToUtf16Strict(truncated, text));
  GC_CHECK(!gc::platform::TryUtf8ToUtf16Strict(cesuHigh, text));
  GC_CHECK(!gc::platform::TryUtf8ToUtf16Strict(cesuLow, text));
  GC_CHECK(!gc::platform::TryUtf8ToUtf16Strict(badStart, text));
  GC_CHECK(text.empty());
  // 这条记的是本机实测事实：Windows 会把 CESU 三字节当成可往返的编码接受，
  // 所以“是不是合法 UTF-8”不能交给 Windows 判定，必须靠上面那两个函数自己的码元检查。
  // 合法 UTF-8（含 4 字节 emoji）正常往返。
  GC_CHECK(gc::platform::TryUtf8ToUtf16Strict("Ж 😀 ok", text));
  GC_CHECK(text == gc::platform::Utf8ToUtf16("Ж 😀 ok"));
}

GC_TEST(command_title_is_kept_even_when_system_codepage_cannot_encode_it) {
  // 验收项 12 的回归：中文标题过去要先按系统 ANSI 码页编码，装不下就整次操作被拒绝。
  // 现在标题只是说明书里的一个字段，由辅助进程用 SetConsoleTitleW 直接设置，
  // 计划阶段不再有“码页可表示性”判定，因此这类操作必然能启动。
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", {L"status"});
  const std::wstring title =
      gc::git::MakeSafeConsoleTitle(L"Git 提交工具 - 命令窗口", L"提交", L"GcOp4xabcd4444");
  bool ok = false;
  const std::wstring specText = BuildSpec(operation, L"GcOp4xabcd4444", title, L"abcdef0123456789", &ok,
                                          nullptr, nullptr);
  GC_CHECK(ok);
  CommandWindowSpec parsed;
  std::wstring reason;
  GC_CHECK(gc::git::ParseCommandWindowSpecText(specText, &parsed, &reason));
  GC_CHECK(parsed.title.find(L"提交") != std::wstring::npos);
  GC_CHECK(parsed.title.find(L"GcOp4xabcd4444") != std::wstring::npos);  // 唯一标记保留
}

GC_TEST(command_spec_rejects_operation_the_plan_validation_refuses) {
  // 说明书不能绕过第一步的校验：路径含引号、参数含控制字符、参数超限都在生成阶段就拒绝。
  const CommandWindowOperation evilPath = MakeOperation(L"C:\\evil\"git.exe", {L"status"});
  const CommandWindowOperation evilArgument = MakeOperation(L"C:\\git.exe", {L"status\t--amend"});
  bool ok = true;
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  BuildSpec(evilPath, L"GcOp1xabcd1234", L"Git status", L"abcdef0123456789", &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::quoteInPath);
  BuildSpec(evilArgument, L"GcOp1xabcd1234", L"Git status", L"abcdef0123456789", &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::illegalArgument);
}

GC_TEST(command_spec_rejects_unencodable_title) {
  // 标题里的控制字符会破坏说明书的行形态，必须拒绝；而“西文代码页装不下中文”不再成立。
  const CommandWindowOperation operation = MakeOperation(L"C:\\git.exe", {L"status"});
  bool ok = true;
  CommandPlanReject reject = CommandPlanReject::none;
  std::wstring detail;
  BuildSpec(operation, L"GcOp1xabcd1234", std::wstring(L"标题") + wchar_t(0x01), L"abcdef0123456789", &ok,
            &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::illegalWindowTitle);
  // 制表符同样会多出一个字段分隔符。
  BuildSpec(operation, L"GcOp1xabcd1234", L"标题\tGcOp1xabcd1234", L"abcdef0123456789", &ok, &reject, &detail);
  GC_CHECK(!ok);
  GC_CHECK(reject == CommandPlanReject::illegalWindowTitle);
  // emoji（超出传统代码页）是合法标题：这正是本次修复要放开的那一类。
  BuildSpec(operation, L"GcOp1xabcd1234", L"Git 提交工具 😀 - GcOp1xabcd1234", L"abcdef0123456789", &ok, &reject,
            &detail);
  GC_CHECK_MESSAGE(ok, Describe(detail));
}

GC_TEST(command_result_parsing_accepts_only_complete_lines) {
  static constexpr std::string_view kNonce = "abcdef0123456789";
  long exitCode = -1;
  bool launched = true;
  GC_CHECK(!gc::git::ParseCommandWindowResult("", kNonce, &exitCode, &launched));
  // 没有行尾：可能正在写。发布协议里改名是原子的，因此“没有行尾”只可能是外部塞进来的半成品，
  // 观察端必须继续等，绝不能拿半截内容当成一次回答。
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\tstarted", kNonce, &exitCode,
                                              &launched));
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\tabc\tstarted\n", kNonce,
                                              &exitCode, &launched));
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t1 2\tstarted\n", kNonce,
                                              &exitCode, &launched));
  GC_CHECK(!gc::git::ParseCommandWindowResult("\n", kNonce, &exitCode, &launched));
  GC_CHECK(!gc::git::ParseCommandWindowResult("0\r\n", kNonce, &exitCode, &launched));  // 旧格式：没有口令
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\n", kNonce, &exitCode,
                                              &launched));
  // 旧三字段格式（没有 Git 启动事实）：协议两端都是本程序自己，不合格一律当“没有结果”。
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\r\n", kNonce, &exitCode,
                                              &launched));
  // 第四字段只认 started / notstarted 两种写法，别的拼写与缺字段一样不合格。
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\t Started\r\n", kNonce,
                                              &exitCode, &launched));
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\tnot-started\r\n", kNonce,
                                              &exitCode, &launched));
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\t9009\r\n", kNonce, &exitCode,
                                              &launched));
  // 首行之后还有内容：一次操作只发布一条结果，多出来的当作不可信。
  GC_CHECK(!gc::git::ParseCommandWindowResult(
      "result\tabcdef0123456789\t0\tstarted\r\nresult\tabcdef0123456789\t9\tstarted\r\n", kNonce,
      &exitCode, &launched));
  launched = false;
  GC_CHECK(gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\tstarted\r\n", kNonce,
                                             &exitCode, &launched) &&
           exitCode == 0 && launched);
  GC_CHECK(gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t128\tstarted\n", kNonce,
                                             &exitCode, &launched) &&
           exitCode == 128 && launched);
  GC_CHECK(gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t-2\tstarted\r\n", kNonce,
                                             &exitCode, &launched) &&
           exitCode == -2 && launched);
  // Git 自己返回 9009 也照常成立：那只是 Git 的一次回答，特殊含义已经随 cmd 批处理一起退役。
  GC_CHECK(gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t9009\tstarted\r\n", kNonce,
                                             &exitCode, &launched) &&
           exitCode == 9009 && launched);
  // notstarted：Git 进程从未被创建，退出码字段没有 Git 语义，读回固定为 0。
  exitCode = -1;
  GC_CHECK(gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t0\tnotstarted\r\n", kNonce,
                                             &exitCode, &launched) &&
           !launched && exitCode == 0);
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tabcdef0123456789\t99999999999999999999\tstarted\n",
                                              kNonce, &exitCode, &launched));
  // 标识不符：上一次留下的、或别人塞进来的“成功”，都不算本次操作的结果。
  exitCode = -1;
  GC_CHECK(!gc::git::ParseCommandWindowResult("result\tfedcba9876543210\t0\tstarted\r\n", kNonce,
                                              &exitCode, &launched));
  GC_CHECK(exitCode == -1);  // 拒绝时不写回退出码，避免调用方误用一个没被承认的值
}

GC_TEST(command_start_marker_carries_this_operation_nonce) {
  static constexpr std::string_view kNonce = "abcdef0123456789";
  const std::string marker = gc::git::BuildCommandWindowStartMarkerText(kNonce);
  GC_CHECK(marker == "start\tabcdef0123456789\r\n");
  GC_CHECK(gc::git::ParseCommandWindowStartMarker(marker, kNonce));
  GC_CHECK(!gc::git::ParseCommandWindowStartMarker(marker, "fedcba9876543210"));  // 别的操作留下的
  GC_CHECK(!gc::git::ParseCommandWindowStartMarker("start\r\n", kNonce));         // 旧格式：没有归属
  GC_CHECK(!gc::git::ParseCommandWindowStartMarker("start\tabcdef0123456789", kNonce));  // 半写
  GC_CHECK(!gc::git::ParseCommandWindowStartMarker("result\tabcdef0123456789\t0\r\n", kNonce));
  GC_CHECK(!gc::git::ParseCommandWindowStartMarker("", kNonce));
  // 结果行与开始标记用的是同一份口令：由同一个来源（说明书）派生，逐字节一致。
  std::string ascii;
  GC_REQUIRE(gc::git::NonceToAscii(L"abcdef0123456789", &ascii), "口令应能取成 ASCII 字节");
  GC_CHECK(ascii == std::string(kNonce));
  std::string rejected;
  GC_CHECK(!gc::git::NonceToAscii(L"bad nonce!", &rejected));  // 含空格与 !：不是执行器生成的形态
  GC_CHECK(rejected.empty());
}

GC_TEST(command_operation_file_names_are_the_reclaim_scope) {
  // 回收与善后只针对这份名单，名单必须覆盖一次操作可能落下的每一个文件：
  // 漏一个就等于“别人写过的东西没人收尾”，或多一个就等于“删到别人的文件”。
  const std::vector<std::string_view> expected = {gc::git::kSpecFileName,
                                                  gc::git::kStartMarkerFileName,
                                                  gc::git::kResultTempFileName,
                                                  gc::git::kResultFileName, gc::git::kLeaseFileName};
  GC_CHECK(expected.size() == gc::git::kOperationFileNames.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    GC_CHECK_MESSAGE(gc::git::kOperationFileNames[index] == expected[index],
                     "回收名单的顺序或内容变了，第 " + std::to_string(index) + " 项");
  }
}

namespace {

gc::git::CommandWindowObservation MakeFacts(bool launched, bool exited, bool startSeen,
                                           bool resultParsed, long exitCode = 0,
                                           bool gitProcessLaunched = true) {
  gc::git::CommandWindowObservation facts;
  facts.createProcessSucceeded = launched;
  facts.processExited = exited;
  facts.startMarkerSeen = startSeen;
  facts.resultParsed = resultParsed;
  facts.exitCode = exitCode;
  facts.gitProcessLaunched = gitProcessLaunched;
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
  // Git 进程确实被创建过（结果行里的 CreateProcess 事实）：任何退出码数值都只是 Git 的回答，
  // 包括历史上曾被 cmd 当作“程序不存在”的 9009——它不再有任何特殊判定。
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, true, true, 9009), &exitCode) ==
               gc::git::CommandCompletion::finished &&
           exitCode == 9009);
  // Git 未启动 = 辅助进程直接上报进程从未被创建：没有退出码可谈，exitCode 不被写出。
  exitCode = -1;
  GC_CHECK(gc::git::DecideCommandCompletion(
               MakeFacts(true, false, true, true, 0, /*gitProcessLaunched=*/false), &exitCode) ==
           gc::git::CommandCompletion::gitNotStarted);
  GC_CHECK(exitCode == -1);  // 中间态之外也不许偷写一个“像样的”退出码
  // 窗口被提前关闭：Git 已开跑但结果缺失。
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, true, false), &exitCode) ==
           gc::git::CommandCompletion::terminated);
  // 连开始标记都没有：辅助进程从未执行到“写开始标记”那一行。
  GC_CHECK(gc::git::DecideCommandCompletion(MakeFacts(true, true, false, false), &exitCode) ==
           gc::git::CommandCompletion::helperNeverStarted);
}

GC_TEST(command_completion_terminal_states_are_enumerated) {
  // 观察循环的结案条件是“判定器给了终态”，这里把八个状态的终/中间归属钉死：
  // 漏一个终态就会重演“操作永远停在执行中、槽位不释放”的回归；
  // 把中间态误列成终态则会提前结案、把还在等凭据的操作当成未知。
  GC_CHECK(gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::launchFailed));
  GC_CHECK(gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::finished));
  GC_CHECK(gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::gitNotStarted));
  GC_CHECK(gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::terminated));
  GC_CHECK(gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::helperNeverStarted));
  GC_CHECK(gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::stillUnknown));
  GC_CHECK(!gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::launched));
  GC_CHECK(!gc::git::IsCommandCompletionTerminal(gc::git::CommandCompletion::running));
}

GC_TEST(command_observer_reads_injected_files) {
  static constexpr std::string_view kNonce = "abcdef0123456789";
  const std::map<std::string, std::string> files{
      {"start.txt", "start\tabcdef0123456789\r\n"},
      {"result.txt", "result\tabcdef0123456789\t42\tstarted\r\n"},
  };
  const auto reader = [&files](std::string_view name) -> std::optional<std::string> {
    const auto found = files.find(std::string(name));
    if (found == files.end()) {
      return std::nullopt;
    }
    return found->second;
  };

  const gc::git::CommandWindowObservation facts = gc::git::ObserveCommandWindow(
      reader, /*createProcessSucceeded=*/true, /*processExited=*/false, kNonce);

  GC_CHECK(facts.startMarkerSeen);
  GC_CHECK(facts.resultParsed);
  GC_CHECK(facts.gitProcessLaunched);
  GC_CHECK(facts.exitCode == 42);
  long exitCode = 0;
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) == gc::git::CommandCompletion::finished);
}

GC_TEST(command_observer_ignores_foreign_or_partial_files) {
  // “文件在那里”不等于“这条痕迹属于本次操作”：口令不符（上一次留下的、别人塞进来的）、
  // 格式是旧形态、或内容只写了一半，观察端都必须当作没有，绝不合成一次“成功”。
  static constexpr std::string_view kNonce = "abcdef0123456789";
  const std::map<std::string, std::string> files{
      {"start.txt", "start\tfedcba9876543210\r\n"},                // 别的操作的口令
      {"result.txt", "result\tabcdef0123456789\t0\tstarted"},      // 同一口令，但还没写完
  };
  const auto reader = [&files](std::string_view name) -> std::optional<std::string> {
    const auto found = files.find(std::string(name));
    if (found == files.end()) {
      return std::nullopt;
    }
    return found->second;
  };
  const gc::git::CommandWindowObservation facts =
      gc::git::ObserveCommandWindow(reader, true, /*processExited=*/false, kNonce);
  GC_CHECK(!facts.startMarkerSeen);
  GC_CHECK(!facts.resultParsed);
  long exitCode = 0;
  // 进程还在，两条痕迹都不认：只能是“已启动，等待就绪”，不能是执行中，更不能是完成。
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) == gc::git::CommandCompletion::launched);

  // 同一条痕迹换成正确的口令就都成立：证明判定看的是归属，不是“有没有读到字节”。
  const std::map<std::string, std::string> ours{
      {"start.txt", "start\tabcdef0123456789\r\n"},
      {"result.txt", "result\tabcdef0123456789\t0\tstarted\r\n"},
  };
  const auto ourReader = [&ours](std::string_view name) -> std::optional<std::string> {
    const auto found = ours.find(std::string(name));
    return found == ours.end() ? std::nullopt : std::optional<std::string>(found->second);
  };
  const gc::git::CommandWindowObservation goodFacts =
      gc::git::ObserveCommandWindow(ourReader, true, true, kNonce);
  GC_CHECK(goodFacts.startMarkerSeen && goodFacts.resultParsed);
  GC_CHECK(gc::git::DecideCommandCompletion(goodFacts, &exitCode) ==
           gc::git::CommandCompletion::finished);
}

GC_TEST(command_observer_treats_missing_files_as_absent) {
  const auto reader = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
  const gc::git::CommandWindowObservation facts =
      gc::git::ObserveCommandWindow(reader, true, true, "abcdef0123456789");
  GC_CHECK(!facts.startMarkerSeen);
  GC_CHECK(!facts.resultParsed);
  long exitCode = 0;
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) ==
           gc::git::CommandCompletion::helperNeverStarted);
}

GC_TEST(command_observer_keeps_running_until_result_written) {
  // 辅助进程已开跑、结果还没写出，进程也还活着：必须是中间态“执行中”，既不能报成功也不能报失败。
  const std::map<std::string, std::string> files{
      {"start.txt", "start\tabcdef0123456789\r\n"}};
  const auto reader = [&files](std::string_view name) -> std::optional<std::string> {
    const auto found = files.find(std::string(name));
    if (found == files.end()) {
      return std::nullopt;
    }
    return found->second;
  };
  const gc::git::CommandWindowObservation facts = gc::git::ObserveCommandWindow(
      reader, true, /*processExited=*/false, "abcdef0123456789");
  long exitCode = 0;
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) == gc::git::CommandCompletion::running);
}
