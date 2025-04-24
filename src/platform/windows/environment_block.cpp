#include "platform/windows/environment_block.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace gc::platform {
namespace {

std::wstring ToUpperAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'a' && c <= L'z') {
      c = static_cast<wchar_t>(c - 32);
    }
  }
  return result;
}

// 从 “NAME=VALUE” 取名字；盘符联动项（以 '=' 开头，如 “=C:=C:\Temp”）返回 nullopt，
// 表示“不参与覆盖匹配，原样保留”。
std::optional<std::wstring> EntryName(const std::wstring& entry) {
  if (!entry.empty() && entry.front() == L'=') {
    return std::nullopt;
  }
  const size_t equals = entry.find(L'=');
  if (equals == std::wstring::npos) {
    return std::nullopt;
  }
  return entry.substr(0, equals);
}

bool IsValidOverrideName(std::wstring_view name) {
  if (name.empty()) {
    return false;
  }
  for (const wchar_t c : name) {
    if (c == L'=' || c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  return true;
}

bool IsValidEntryValue(std::wstring_view value) {
  for (const wchar_t c : value) {
    if (c == L'\0' || c < 0x20) {
      // 允许 CR/LF（少数变量确实带换行），其余控制字符会破坏环境块结构。
      if (c != L'\r' && c != L'\n') {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool MergeEnvironmentEntries(const std::vector<std::wstring>& baseEntries,
                             const std::vector<git::EnvironmentOverride>& overrides,
                             std::vector<std::wstring>* mergedEntries, std::wstring* failureReason) {
  if (mergedEntries == nullptr) {
    return false;
  }
  const auto fail = [&](std::wstring reason) {
    if (failureReason != nullptr) {
      *failureReason = std::move(reason);
    }
    return false;
  };

  std::vector<std::wstring> merged = baseEntries;
  for (const git::EnvironmentOverride& override : overrides) {
    if (!IsValidOverrideName(override.name)) {
      return fail(L"环境变量名不合法：" + override.name);
    }
    if (override.value.has_value() && !IsValidEntryValue(*override.value)) {
      return fail(L"环境变量值不合法：" + override.name);
    }
    const std::wstring wanted = ToUpperAscii(override.name);
    if (!override.value.has_value()) {
      // 删除：移除所有同名项。
      std::erase_if(merged, [&wanted](const std::wstring& entry) {
        const std::optional<std::wstring> name = EntryName(entry);
        return name.has_value() && ToUpperAscii(*name) == wanted;
      });
      continue;
    }
    const std::wstring replacement = override.name + L"=" + *override.value;
    bool replaced = false;
    for (std::wstring& entry : merged) {
      const std::optional<std::wstring> name = EntryName(entry);
      if (name.has_value() && ToUpperAscii(*name) == wanted) {
        if (!replaced) {
          entry = replacement;
          replaced = true;
        } else {
          // 大小写同名项（如 Path 与 PATH 同时在块里）合并：留改后的第一条就够了，
          // 否则同一个名字在环境块里出现两次，子进程取哪一条就成了实现细节。
          entry.clear();
        }
      }
    }
    if (!replaced) {
      merged.push_back(replacement);
    } else {
      std::erase_if(merged, [](const std::wstring& entry) { return entry.empty(); });
    }
  }
  *mergedEntries = std::move(merged);
  return true;
}

std::wstring MakeUnicodeEnvironmentBlock(const std::vector<std::wstring>& entries) {
  std::wstring block;
  size_t total = 0;
  for (const std::wstring& entry : entries) {
    total += entry.size() + 1;
  }
  block.reserve(total + 1);
  for (const std::wstring& entry : entries) {
    block += entry;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');  // 环境块结束标记。
  return block;
}

std::vector<std::wstring> GetCurrentEnvironmentEntries(bool* readFailed) {
  std::vector<std::wstring> entries;
  if (readFailed != nullptr) {
    *readFailed = false;
  }
  LPWCH raw = ::GetEnvironmentStringsW();
  if (raw == nullptr) {
    if (readFailed != nullptr) {
      *readFailed = true;  // 调用失败：环境是未知数，不是“没有任何项”。
    }
    return entries;
  }
  // 块格式：“NAME=VALUE\0”若干项，后跟一个空项（连续两个 \0）结束。
  // 用索引扫描而不是 string_view::remove_suffix：避免在最后一项上越过块尾。
  size_t index = 0;
  for (;;) {
    const size_t length = ::wcslen(raw + index);
    if (length == 0) {
      break;
    }
    entries.emplace_back(raw + index, length);
    index += length + 1;
  }
  ::FreeEnvironmentStringsW(raw);
  return entries;
}

std::wstring BuildChildEnvironmentBlock(const std::vector<git::EnvironmentOverride>& overrides,
                                        std::wstring* failureReason) {
  std::vector<std::wstring> merged;
  if (!MergeEnvironmentEntries(GetCurrentEnvironmentEntries(), overrides, &merged, failureReason)) {
    return {};
  }
  return MakeUnicodeEnvironmentBlock(merged);
}

GitChildEnvironment BuildGitChildEnvironment(git::GitRunPurpose purpose,
                                             const std::vector<git::EnvironmentOverride>& operationOverrides) {
  GitChildEnvironment result;
  const auto fail = [&result](std::wstring reason) {
    // 失败原因会进界面与错误提示：限长，而且这里能出现在 reason 里的只有变量名，
    // 任何路径、任何值都不得经由本函数外泄。
    if (reason.size() > 200) {
      reason.resize(200);
    }
    result.failureReason = std::move(reason);
    return result;
  };

  bool readFailed = false;
  const std::vector<std::wstring> inherited = GetCurrentEnvironmentEntries(&readFailed);
  if (readFailed) {
    return fail(L"无法读取当前进程环境（GetEnvironmentStringsW 失败），不能装配 Git 子进程环境。");
  }
  if (inherited.empty()) {
    // 真实空环境的进程跑不了任何依赖 PATH/HOME 的 Git；与其让子进程神秘失踪，
    // 不如在这里明确拒绝装配。
    return fail(L"当前进程环境为空，无法为 Git 子进程装配可用环境。");
  }

  // 数字后缀配置注入项无法按名字枚举（条数随注入变化），先逐项从基块剔除；
  // 剩下的删除与注入都走同一个合并函数，操作覆盖排在最后。
  std::vector<std::wstring> base = inherited;
  std::erase_if(base, [](const std::wstring& entry) {
    if (!entry.empty() && entry.front() == L'=') {
      return false;  // 盘符联动项不参与任何名字匹配。
    }
    const size_t equals = entry.find(L'=');
    return equals != std::wstring::npos &&
           git::IsNumberedConfigInjectionName(std::wstring_view(entry).substr(0, equals));
  });

  const git::GitEnvironmentPlan plan = git::MakeGitEnvironmentPlan(purpose, operationOverrides);
  std::vector<std::wstring> merged;
  std::wstring mergeFailure;
  if (!MergeEnvironmentEntries(base, plan.overrides, &merged, &mergeFailure)) {
    return fail(mergeFailure);
  }
  // 告知用名字来自对“继承原块”的核对：数字项即便存在也不单列（COUNT 已被移除，它们不构成事实）。
  result.notice = git::BuildRedirectNoticeText(git::FindInheritedRedirects(inherited, plan.redirectNames));
  result.block = MakeUnicodeEnvironmentBlock(merged);
  return result;
}

}  // namespace gc::platform
