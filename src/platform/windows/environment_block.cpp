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
    bool replaced = false;
    if (!override.value.has_value()) {
      // 删除：移除所有同名项。
      std::erase_if(merged, [&wanted](const std::wstring& entry) {
        const std::optional<std::wstring> name = EntryName(entry);
        return name.has_value() && ToUpperAscii(*name) == wanted;
      });
      continue;
    }
    const std::wstring replacement = override.name + L"=" + *override.value;
    for (std::wstring& entry : merged) {
      const std::optional<std::wstring> name = EntryName(entry);
      if (name.has_value() && ToUpperAscii(*name) == wanted) {
        entry = replacement;
        replaced = true;
      }
    }
    if (!replaced) {
      merged.push_back(replacement);
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

std::vector<std::wstring> GetCurrentEnvironmentEntries() {
  std::vector<std::wstring> entries;
  LPWCH raw = ::GetEnvironmentStringsW();
  if (raw == nullptr) {
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

}  // namespace gc::platform
