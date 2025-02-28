#include "support/tiny_test.h"

#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>

#include "platform/windows/subprocess.h"

namespace {

// 用系统解析器验证序列化结果能还原出原始程序与参数数组（转义正确性的最终判据）。
bool RoundTrips(const std::wstring& commandLine, const std::wstring& program,
                const std::vector<std::wstring>& arguments) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(commandLine.c_str(), &argc);
  if (argv == nullptr) {
    return false;
  }
  std::vector<std::wstring> parsed;
  for (int index = 0; index < argc; ++index) {
    parsed.emplace_back(argv[index]);
  }
  ::LocalFree(argv);
  if (parsed.size() != arguments.size() + 1 || parsed.front() != program) {
    return false;
  }
  for (size_t index = 0; index < arguments.size(); ++index) {
    if (parsed[index + 1] != arguments[index]) {
      return false;
    }
  }
  return true;
}

}  // namespace

GC_TEST(command_line_quotes_program_path_with_spaces) {
  const std::wstring line =
      gc::platform::BuildCommandLine(L"C:\\Program Files\\Git\\cmd\\git.exe", {L"--version"});

  GC_CHECK(line == L"\"C:\\Program Files\\Git\\cmd\\git.exe\" --version");
}

GC_TEST(command_line_handles_chinese_and_spaces) {
  const std::vector<std::wstring> arguments{L"--version"};
  const std::wstring program = L"C:\\软件 目录\\Git\\bin\\git.exe";
  const std::wstring line = gc::platform::BuildCommandLine(program, arguments);

  GC_CHECK(line == L"\"C:\\软件 目录\\Git\\bin\\git.exe\" --version");
}

GC_TEST(command_line_escapes_embedded_quotes_and_backslashes) {
  const std::vector<std::wstring> arguments{L"他说的\"引号\"", L"尾反斜杠\\", L"a\\\"b", L"", L"plain"};
  const std::wstring line = gc::platform::BuildCommandLine(L"C:\\git.exe", arguments);

  // 无空格/引号的参数不加引号（尾反斜杠后不邻接引号时按字面解析）；
  // 含引号的参数中：引号前反斜杠加倍再转义，结尾反斜杠加倍。
  GC_CHECK(line == L"\"C:\\git.exe\" \"他说的\\\"引号\\\"\" 尾反斜杠\\ \"a\\\\\\\"b\" \"\" plain");
  GC_CHECK_MESSAGE(
      RoundTrips(line, L"C:\\git.exe", {L"他说的\"引号\"", L"尾反斜杠\\", L"a\\\"b", L"", L"plain"}),
      "引号/反斜杠转义未能通过 CommandLineToArgvW 回环");
}

GC_TEST(command_line_roundtrips_through_system_parser) {
  const std::wstring program = L"D:\\Program Files\\git 中文\\bin\\git.exe";
  const std::vector<std::wstring> arguments{L"--version", L"-c", L"core.quotepath=\"", L"带 空格 与\"引号\"的参"};
  const std::wstring line = gc::platform::BuildCommandLine(program, arguments);

  GC_CHECK(RoundTrips(line, program, arguments));
}
