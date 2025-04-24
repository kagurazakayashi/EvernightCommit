#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"
#include "git/git_environment.h"

namespace gc::platform {

// 受控环境块构造（纯逻辑，可脱离 Win32 测试）：
// 基块是 GetEnvironmentStringsW 同格式的 “NAME=VALUE” 项列表（不含结尾空项），
// 覆盖表按 git::EnvironmentOverride 的语义合并：
//   value 有值 —— 大小写不敏感替换同名项（只保留第一个改后的项，其余大小写同名项一并
//                  合并进来，块里每个名字至多一条；不存在则追加到末尾）；
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
// readFailed（可空）区分两种"看不到任何项"：函数调用本身失败（true，环境是未知数）
// 与进程环境真的为空（false）——前者绝不能被当成后者去装配一个空环境子进程。
[[nodiscard]] std::vector<std::wstring> GetCurrentEnvironmentEntries(bool* readFailed = nullptr);

// 一步构造“继承 + 覆盖”的 Unicode 环境块；覆盖不合法时返回空串并给出原因。
[[nodiscard]] std::wstring BuildChildEnvironmentBlock(
    const std::vector<git::EnvironmentOverride>& overrides, std::wstring* failureReason);

// 集中 Git 环境策略的唯一生产装配点（策略内容见 git/git_environment.h）：
// 基块 = 当前进程环境 − 数字后缀配置注入项；覆盖 = MakeGitEnvironmentPlan(purpose, ...)。
// 装配失败的情形：环境读取调用失败、进程环境为空、覆盖名/值不合法。失败时 block 为空、
// failureReason 非空且只含变量名与说明，绝不含任何变量值。
// notice 收到“确实存在于继承环境、已被移除”的重定向变量告知（只含名字），没有移除则为空。
struct GitChildEnvironment {
  std::wstring block;
  std::wstring notice;
  std::wstring failureReason;
};
[[nodiscard]] GitChildEnvironment BuildGitChildEnvironment(
    git::GitRunPurpose purpose, const std::vector<git::EnvironmentOverride>& operationOverrides);

}  // namespace gc::platform
