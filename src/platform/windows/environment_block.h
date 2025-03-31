#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"

namespace gc::platform {

// 受控环境块构造（纯逻辑，可脱离 Win32 测试）：
// 基块是 GetEnvironmentStringsW 同格式的 “NAME=VALUE” 项列表（不含结尾空项），
// 覆盖表按 git::EnvironmentOverride 的语义合并：
//   value 有值 —— 大小写不敏感替换同名项，不存在则追加到末尾；
//   value 无值 —— 删除同名项（盘符联动项 “=C:=…” 一律原样保留，不参与匹配）。
// 覆盖名不合法（为空、含 '=' 或控制字符）时整次合并失败并给出原因。
[[nodiscard]] bool MergeEnvironmentEntries(const std::vector<std::wstring>& baseEntries,
                                           const std::vector<git::EnvironmentOverride>& overrides,
                                           std::vector<std::wstring>* mergedEntries,
                                           std::wstring* failureReason);

// 把项列表拼成 CreateProcessW lpEnvironment 需要的 NULL 结尾 Unicode 环境块
// （每项结尾一个 L'\0'，最后再追加一个 L'\0'）。
[[nodiscard]] std::wstring MakeUnicodeEnvironmentBlock(const std::vector<std::wstring>& entries);

// 读取当前进程环境（GetEnvironmentStringsW）并拆成项列表。
[[nodiscard]] std::vector<std::wstring> GetCurrentEnvironmentEntries();

// 一步构造“继承 + 覆盖”的 Unicode 环境块；覆盖不合法时返回空串并给出原因。
[[nodiscard]] std::wstring BuildChildEnvironmentBlock(
    const std::vector<git::EnvironmentOverride>& overrides, std::wstring* failureReason);

}  // namespace gc::platform
