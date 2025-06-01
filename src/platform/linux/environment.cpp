#include "platform/linux/environment.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/text_encoding.h"

extern char** environ;

namespace gc::platform {
namespace {

bool IsValidOverrideName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const unsigned char c : name) {
    if (c == '=' || c < 0x20u || c == 0x7Fu) {
      return false;
    }
  }
  return true;
}

bool IsValidOverrideValue(std::string_view value) {
  // POSIX 环境值可以是任意非 NUL 字节；换行也允许（Git 的某些值确实带换行）。
  return value.find('\0') == std::string_view::npos;
}

bool EntryName(std::string_view entry, std::string_view* outName) {
  if (entry.empty() || entry.front() == '=') {
    return false;  // POSIX 不定义空名或 '=' 开头的项；它们不参与覆盖匹配。
  }
  const size_t equals = entry.find('=');
  if (equals == std::string_view::npos) {
    return false;
  }
  *outName = entry.substr(0, equals);
  return true;
}

bool EncodeWide(std::wstring_view text, std::string* out, std::wstring* failureReason,
                const wchar_t* what) {
  if (!TryWideToUtf8Strict(text, *out)) {
    *failureReason = std::wstring(what) + L"无法无损编码成 UTF-8。";
    return false;
  }
  return true;
}

}  // namespace

bool MergePosixEnvironmentEntries(const std::vector<std::string>& baseEntries,
                                  const std::vector<git::EnvironmentOverride>& overrides,
                                  std::vector<std::string>* mergedEntries,
                                  std::wstring* failureReason) {
  if (mergedEntries == nullptr) {
    return false;
  }
  const auto fail = [&](std::wstring reason) {
    if (failureReason != nullptr) {
      *failureReason = std::move(reason);
    }
    return false;
  };

  std::vector<std::string> merged = baseEntries;
  for (const git::EnvironmentOverride& override : overrides) {
    std::string name;
    std::wstring encodeFailure;
    if (!EncodeWide(override.name, &name, &encodeFailure, L"环境变量名")) {
      return fail(encodeFailure);
    }
    if (!IsValidOverrideName(name)) {
      return fail(L"环境变量名不合法：" + std::wstring(override.name));
    }
    if (override.value.has_value()) {
      std::string value;
      if (!EncodeWide(*override.value, &value, &encodeFailure, L"环境变量值")) {
        return fail(encodeFailure);
      }
      if (!IsValidOverrideValue(value)) {
        return fail(L"环境变量值含 NUL，不能交给子进程：" + std::wstring(override.name));
      }
      const std::string replacement = name + "=" + value;
      bool replaced = false;
      for (std::string& entry : merged) {
        std::string_view existingName;
        if (EntryName(entry, &existingName) && existingName == name) {
          if (!replaced) {
            entry = replacement;
            replaced = true;
          } else {
            entry.clear();  // 重复同名项合并成一条：环境里同一个名字出现两次只会制造歧义。
          }
        }
      }
      if (!replaced) {
        merged.push_back(replacement);
      } else {
        std::erase_if(merged, [](const std::string& entry) { return entry.empty(); });
      }
    } else {
      // POSIX 名字大小写敏感：GIT_DIR 与 Git_Dir 是两个不同的变量，不能一起删。
      std::erase_if(merged, [&name](const std::string& entry) {
        std::string_view existingName;
        return EntryName(entry, &existingName) && existingName == name;
      });
    }
  }
  *mergedEntries = std::move(merged);
  return true;
}

std::vector<std::string> GetCurrentPosixEnvironmentEntries(bool* readFailed) {
  std::vector<std::string> entries;
  if (readFailed != nullptr) {
    *readFailed = false;
  }
  if (environ == nullptr) {
    if (readFailed != nullptr) {
      *readFailed = true;
    }
    return entries;
  }
  for (char** item = environ; *item != nullptr; ++item) {
    entries.emplace_back(*item);
  }
  return entries;
}

PosixChildEnvironment BuildGitChildEnvironment(
    git::GitRunPurpose purpose, const std::vector<git::EnvironmentOverride>& operationOverrides) {
  PosixChildEnvironment result;
  const auto fail = [&result](std::wstring reason) {
    if (reason.size() > 200) {
      reason.resize(200);
    }
    result.failureReason = std::move(reason);
    return result;
  };

  bool readFailed = false;
  const std::vector<std::string> inherited = GetCurrentPosixEnvironmentEntries(&readFailed);
  if (readFailed) {
    return fail(L"无法读取当前进程环境（environ 不可用），不能装配 Git 子进程环境。");
  }
  if (inherited.empty()) {
    return fail(L"当前进程环境为空，无法为 Git 子进程装配可用环境。");
  }

  // 数字后缀配置注入项无法按名字枚举，先按 POSIX 大小写敏感规则逐项剔除。
  std::vector<std::string> base;
  base.reserve(inherited.size());
  for (const std::string& entry : inherited) {
    std::string_view name;
    if (!EntryName(entry, &name)) {
      base.push_back(entry);
      continue;
    }
    std::wstring wideName;
    if (TryUtf8ToWideStrict(name, wideName) &&
        git::IsNumberedConfigInjectionName(wideName, git::EnvironmentNameComparison::posixCaseSensitive)) {
      continue;
    }
    base.push_back(entry);
  }

  const git::GitEnvironmentPlan plan = git::MakeGitEnvironmentPlan(purpose, operationOverrides);
  std::vector<std::string> merged;
  std::wstring mergeFailure;
  if (!MergePosixEnvironmentEntries(base, plan.overrides, &merged, &mergeFailure)) {
    return fail(mergeFailure);
  }

  // 告知只核对“名字字节”：值可能是非 UTF-8 的旧字节，不能因为整条项解不开就
  // 把“这个重定向变量确实存在过、已被移除”这件事一起丢掉。
  std::vector<std::wstring> found;
  for (const std::wstring& wanted : plan.redirectNames) {
    std::string wantedBytes;
    if (!TryWideToUtf8Strict(wanted, wantedBytes)) {
      continue;
    }
    for (const std::string& entry : inherited) {
      std::string_view name;
      if (EntryName(entry, &name) && name == wantedBytes) {
        found.push_back(wanted);
        break;
      }
    }
  }
  result.notice = git::BuildRedirectNoticeText(found);
  result.entries = std::move(merged);
  return result;
}

}  // namespace gc::platform
