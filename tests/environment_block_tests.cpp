#include "support/tiny_test.h"

#include <string>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/environment_block.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::EnvironmentOverride;

std::vector<std::wstring> BlockToEntries(std::wstring_view block) {
  std::vector<std::wstring> entries;
  size_t start = 0;
  while (start < block.size()) {
    const size_t end = block.find(L'\0', start);
    if (end == std::wstring::npos) {
      entries.emplace_back(block.substr(start));
      break;
    }
    if (end == start) {
      break;  // 空项 = 环境块结束。
    }
    entries.emplace_back(block.substr(start, end - start));
    start = end + 1;
  }
  return entries;
}

std::vector<std::wstring> Merge(const std::vector<std::wstring>& base,
                                const std::vector<EnvironmentOverride>& overrides, bool* ok = nullptr,
                                std::wstring* reason = nullptr) {
  std::vector<std::wstring> merged;
  const bool built = gc::platform::MergeEnvironmentEntries(base, overrides, &merged, reason);
  if (ok != nullptr) {
    *ok = built;
  }
  return merged;
}

}  // namespace

GC_TEST(environment_block_replaces_case_insensitively) {
  const std::vector<std::wstring> merged =
      Merge({L"Path=C:\\bin", L"HOME=C:\\home"},
            {EnvironmentOverride{L"home", std::wstring(L"C:\\isolated")}});

  GC_CHECK(merged.size() == 2);
  GC_CHECK(merged[0] == L"Path=C:\\bin");
  GC_CHECK(merged[1] == L"home=C:\\isolated");  // 替换保留原大小写写法
}

GC_TEST(environment_block_appends_missing_names) {
  const std::vector<std::wstring> merged =
      Merge({L"PATH=C:\\bin"}, {EnvironmentOverride{L"GIT_TERMINAL_PROMPT", std::wstring(L"1")}});

  GC_CHECK(merged.size() == 2);
  GC_CHECK(merged[1] == L"GIT_TERMINAL_PROMPT=1");
}

GC_TEST(environment_block_deletes_without_value) {
  const std::vector<std::wstring> merged =
      Merge({L"PATH=C:\\bin", L"GIT_ASKPASS=C:\\askpass.exe", L"=C:=C:\\Temp"},
            {EnvironmentOverride{L"git_askpass", std::nullopt}});

  // 盘符联动项 “=C:” 不参与删除，其余同名项移除。
  GC_CHECK(merged.size() == 2);
  GC_CHECK(merged[0] == L"PATH=C:\\bin");
  GC_CHECK(merged[1] == L"=C:=C:\\Temp");
}

GC_TEST(environment_block_rejects_invalid_names) {
  bool ok = true;
  std::wstring reason;
  Merge({L"PATH=C:\\bin"}, {EnvironmentOverride{L"BAD=NAME", std::wstring(L"x")}}, &ok, &reason);
  GC_CHECK(!ok);
  GC_CHECK(!reason.empty());

  Merge({L"PATH=C:\\bin"}, {EnvironmentOverride{L"", std::wstring(L"x")}}, &ok, &reason);
  GC_CHECK(!ok);

  Merge({L"PATH=C:\\bin"}, {EnvironmentOverride{L"NAME", std::wstring(L"bad\tvalue")}}, &ok, &reason);
  GC_CHECK(!ok);
}

GC_TEST(environment_block_serializes_with_terminator) {
  const std::wstring block = gc::platform::MakeUnicodeEnvironmentBlock({L"A=1", L"B=2"});

  // 每项自带终止符，块尾再补一个空项：A=1 NUL B=2 NUL NUL —— 共 9 个 wchar_t。
  GC_CHECK(block.size() == 9);
  GC_CHECK(block == std::wstring(L"A=1\0B=2\0\0", 9));
  const std::vector<std::wstring> roundTrip = BlockToEntries(block);
  GC_CHECK(roundTrip.size() == 2);
  GC_CHECK(roundTrip[0] == L"A=1");
  GC_CHECK(roundTrip[1] == L"B=2");
}

GC_TEST(environment_block_of_current_process_is_not_empty) {
  const std::vector<std::wstring> entries = gc::platform::GetCurrentEnvironmentEntries();
  // 任何 Windows 进程都至少有 PATH 或系统目录类变量；空结果说明读取实现有问题。
  GC_CHECK(!entries.empty());
  std::wstring failureReason;
  const std::wstring block = gc::platform::BuildChildEnvironmentBlock(
      {EnvironmentOverride{L"GC_TEST_ONLY", std::wstring(L"1")}}, &failureReason);
  GC_CHECK_MESSAGE(!block.empty(), gc::platform::Utf16ToUtf8(failureReason));
  GC_CHECK(block.back() == L'\0');
}
