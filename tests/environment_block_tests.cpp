#include "support/tiny_test.h"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"
#include "git/git_environment.h"
#include "platform/windows/environment_block.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::git::EnvironmentOverride;

// 在测试进程环境里临时落一个值并在用例结束后精确复原（原本不存在则删除）。
// 集中策略的装配读的就是进程环境，验证它必须能安全地操纵这里。
class ScopedEnvironmentVariable {
public:
  explicit ScopedEnvironmentVariable(std::wstring name, std::optional<std::wstring> value)
      : name_(std::move(name)) {
    wchar_t buffer[4096];
    const DWORD got =
        ::GetEnvironmentVariableW(name_.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (got == 0 && ::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
      hadPrevious_ = false;
    } else if (got > 0 && got < std::size(buffer)) {
      hadPrevious_ = true;
      previous_ = std::wstring(buffer, got);
    } else {
      // 变量存在但装不进采样缓冲：不去动它，构造后保持原样（用例自己会 GC_REQUIRE 失败）。
      unusable_ = true;
      return;
    }
    Apply(value);
  }
  ~ScopedEnvironmentVariable() {
    if (unusable_) {
      return;
    }
    Apply(hadPrevious_ ? std::optional<std::wstring>(previous_) : std::optional<std::wstring>());
  }
  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

  [[nodiscard]] bool usable() const noexcept { return !unusable_; }

private:
  void Apply(const std::optional<std::wstring>& value) {
    if (value.has_value()) {
      ::SetEnvironmentVariableW(name_.c_str(), value->c_str());
    } else {
      ::SetEnvironmentVariableW(name_.c_str(), nullptr);
    }
  }

  std::wstring name_;
  bool hadPrevious_ = false;
  std::wstring previous_;
  bool unusable_ = false;
};

std::wstring ToUpperHere(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'a' && c <= L'z') {
      c = static_cast<wchar_t>(c - 32);
    }
  }
  return result;
}

// 在项列表里找名字（大小写不敏感）；返回完整项，找不到返回 nullopt。
std::optional<std::wstring> FindEntry(const std::vector<std::wstring>& entries, std::wstring_view name) {
  const std::wstring wanted = ToUpperHere(name);
  for (const std::wstring& entry : entries) {
    if (!entry.empty() && entry.front() == L'=') {
      continue;
    }
    const size_t equals = entry.find(L'=');
    if (equals != std::wstring::npos && ToUpperHere(std::wstring_view(entry).substr(0, equals)) == wanted) {
      return entry;
    }
  }
  return std::nullopt;
}

size_t CountEntriesNamed(const std::vector<std::wstring>& entries, std::wstring_view name) {
  const std::wstring wanted = ToUpperHere(name);
  size_t count = 0;
  for (const std::wstring& entry : entries) {
    if (!entry.empty() && entry.front() == L'=') {
      continue;
    }
    const size_t equals = entry.find(L'=');
    if (equals != std::wstring::npos && ToUpperHere(std::wstring_view(entry).substr(0, equals)) == wanted) {
      ++count;
    }
  }
  return count;
}

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

GC_TEST(environment_block_merges_case_duplicate_entries) {
  // 块里同时有 PATH 与 Path（大小写同名项）：替换后必须只剩一条，
  // 否则子进程取到哪一条就成了环境块解析器的偶然行为。
  const std::vector<std::wstring> merged =
      Merge({L"PATH=A", L"Path=B", L"HOME=H"},
            {EnvironmentOverride{L"PATH", std::wstring(L"C")}});

  GC_CHECK(merged.size() == 2);
  GC_CHECK(CountEntriesNamed(merged, L"PATH") == 1);
  GC_CHECK(*FindEntry(merged, L"PATH") == L"PATH=C");
  GC_CHECK(merged[merged.size() - 1] == L"HOME=H");
}

GC_TEST(environment_block_empty_environment_still_terminates) {
  // 空环境也要给出结构合法的块：只剩结束标记。
  const std::wstring block = gc::platform::MakeUnicodeEnvironmentBlock({});
  GC_CHECK(block.size() == 1 && block.front() == L'\0');
  GC_CHECK(BlockToEntries(block).empty());
}

GC_TEST(environment_block_read_failure_is_not_empty_environment) {
  bool readFailed = true;
  const std::vector<std::wstring> entries = gc::platform::GetCurrentEnvironmentEntries(&readFailed);
  // 任何真实 Windows 进程都能读到环境；这里钉住“读取成功”与“读取失败”是两种答复。
  GC_CHECK(!readFailed);
  GC_CHECK(!entries.empty());
  static_cast<void>(gc::platform::GetCurrentEnvironmentEntries());  // 允许不关心失败原因的调用形态
}

GC_TEST(git_environment_block_background_keeps_user_variables) {
  // 在测试进程环境里布置一套“从终端启动程序会继承到”的重定向与用户变量。
  const ScopedEnvironmentVariable gitDir{L"GIT_DIR", std::wstring(L"C:\\wrong\\repo\\.git")};
  const ScopedEnvironmentVariable indexFile{L"GIT_INDEX_FILE", std::wstring(L"C:\\wrong\\index")};
  const ScopedEnvironmentVariable authorName{L"git_author_name", std::wstring(L"继承劫持")};
  const ScopedEnvironmentVariable configCount{L"GIT_CONFIG_COUNT", std::wstring(L"1")};
  const ScopedEnvironmentVariable configKey{L"GIT_CONFIG_KEY_0", std::wstring(L"core.hidden")};
  const ScopedEnvironmentVariable configValue{L"GIT_CONFIG_VALUE_0", std::wstring(L"yes")};
  const ScopedEnvironmentVariable customKey{L"GIT_CONFIG_KEY_NAME", std::wstring(L"not-a-number")};
  const ScopedEnvironmentVariable askPass{L"GIT_ASKPASS", std::wstring(L"C:/fake/askpass.exe")};
  const ScopedEnvironmentVariable sshCommand{L"GIT_SSH_COMMAND", std::wstring(L"ssh -o KeepAlive=yes")};
  const ScopedEnvironmentVariable sshSock{L"SSH_AUTH_SOCK", std::wstring(L"\\\\.\\pipe\\fake-agent")};
  const ScopedEnvironmentVariable keepMe{L"GC_TEST_ENV_KEEP", std::wstring(L"keep-me")};
  GC_REQUIRE(gitDir.usable() && indexFile.usable() && authorName.usable() && configCount.usable() &&
                 configKey.usable() && configValue.usable() && customKey.usable() && askPass.usable() &&
                 sshCommand.usable() && sshSock.usable() && keepMe.usable(),
             "测试进程环境变量无法布置（超出采样缓冲或权限）");

  // 操作自己的覆盖排在策略之后：表单作者重新落回同一个名字。
  const gc::platform::GitChildEnvironment environment = gc::platform::BuildGitChildEnvironment(
      gc::git::GitRunPurpose::backgroundProbe,
      {EnvironmentOverride{L"GIT_AUTHOR_NAME", std::wstring(L"表单作者")}});
  GC_REQUIRE_MESSAGE(!environment.block.empty(),
                     gc::platform::Utf16ToUtf8(environment.failureReason));
  const std::vector<std::wstring> entries = BlockToEntries(environment.block);

  // 重定向与身份：全部移除，且块里不残留它们的值。
  GC_CHECK(!FindEntry(entries, L"GIT_DIR").has_value());
  GC_CHECK(!FindEntry(entries, L"GIT_INDEX_FILE").has_value());
  GC_CHECK(!FindEntry(entries, L"GIT_CONFIG_COUNT").has_value());
  GC_CHECK(!FindEntry(entries, L"GIT_CONFIG_KEY_0").has_value());
  GC_CHECK(!FindEntry(entries, L"GIT_CONFIG_VALUE_0").has_value());
  GC_CHECK(CountEntriesNamed(entries, L"GIT_AUTHOR_NAME") == 1);
  GC_CHECK(*FindEntry(entries, L"GIT_AUTHOR_NAME") == L"GIT_AUTHOR_NAME=表单作者");
  GC_CHECK(environment.notice.find(L"继承劫持") == std::wstring::npos);
  GC_CHECK(environment.notice.find(L"C:\\wrong") == std::wstring::npos);

  // 后台探测：不许停在看不见的问题上，但用户的凭据助手与工具链照常保留。
  GC_CHECK(*FindEntry(entries, L"GIT_TERMINAL_PROMPT") == L"GIT_TERMINAL_PROMPT=0");
  GC_CHECK(FindEntry(entries, L"GIT_ASKPASS").has_value());
  GC_CHECK(*FindEntry(entries, L"GIT_SSH_COMMAND") == L"GIT_SSH_COMMAND=ssh -o KeepAlive=yes");
  GC_CHECK(FindEntry(entries, L"SSH_AUTH_SOCK").has_value());
  GC_CHECK(*FindEntry(entries, L"GC_TEST_ENV_KEEP") == L"GC_TEST_ENV_KEEP=keep-me");
  // 同前缀的自定义变量不被数字项规则误删。
  GC_CHECK(*FindEntry(entries, L"GIT_CONFIG_KEY_NAME") == L"GIT_CONFIG_KEY_NAME=not-a-number");

  // HOME/USERPROFILE/PATH 是 Git 找用户配置与工具链的地基，必须原样在场。
  GC_CHECK(FindEntry(entries, L"PATH").has_value());
  GC_CHECK(FindEntry(entries, L"HOMEDRIVE").has_value() || FindEntry(entries, L"HOMEPATH").has_value() ||
           FindEntry(entries, L"HOME").has_value() || FindEntry(entries, L"USERPROFILE").has_value());

  // 告知只列被移除的名字。
  GC_CHECK(environment.notice.find(L"GIT_DIR") != std::wstring::npos);
  GC_CHECK(environment.notice.find(L"GIT_INDEX_FILE") != std::wstring::npos);
  GC_CHECK(environment.notice.find(L"GIT_AUTHOR_NAME") != std::wstring::npos);
}

GC_TEST(git_environment_block_command_window_enforces_window_contract) {
  const ScopedEnvironmentVariable askPass{L"GIT_ASKPASS", std::wstring(L"C:/fake/askpass.exe")};
  const ScopedEnvironmentVariable pager{L"GIT_PAGER", std::wstring(L"less")};
  GC_REQUIRE(askPass.usable() && pager.usable(), "测试进程环境变量无法布置");

  const gc::platform::GitChildEnvironment environment =
      gc::platform::BuildGitChildEnvironment(gc::git::GitRunPurpose::commandWindow, {});
  GC_REQUIRE_MESSAGE(!environment.block.empty(),
                     gc::platform::Utf16ToUtf8(environment.failureReason));
  const std::vector<std::wstring> entries = BlockToEntries(environment.block);

  GC_CHECK(*FindEntry(entries, L"GIT_TERMINAL_PROMPT") == L"GIT_TERMINAL_PROMPT=1");
  GC_CHECK(*FindEntry(entries, L"GIT_PAGER") == L"GIT_PAGER=cat");
  GC_CHECK(!FindEntry(entries, L"GIT_ASKPASS").has_value());
  // askpass 属于交互契约，不在“重定向”告知名单里：命令窗口移除它不需要向用户解释成仓库搬家。
  GC_CHECK(environment.notice.find(L"GIT_ASKPASS") == std::wstring::npos);
}
