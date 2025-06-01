#pragma once

#include <string>
#include <vector>

#include "git/git_environment.h"

namespace gc::platform {

// POSIX 环境项的字节表示：每项是 "NAME=VALUE"。名字大小写敏感；继承项的值原样保留
// （可能是非 UTF-8 的旧字节），覆盖项的名字/值由 std::wstring 严格编码成 UTF-8。
// 这里不构造 Windows 的 UTF-16 环境块，也不调用 setenv/putenv：所有覆盖只存在返回的
// 子进程专用列表里，GUI 进程自己的环境一个字都不改。
struct PosixChildEnvironment {
  std::vector<std::string> entries;  // 可直接作为 execve 的 envp
  std::wstring notice;               // 已移除的重定向变量名（只含名字，绝不含值）
  std::wstring failureReason;
};

// 基块是继承环境项。覆盖按 git::EnvironmentOverride 的语义合并：
//   value 有值 —— 精确同名（区分大小写）替换，重复同名项合并成一条；
//   value 无值 —— 精确同名删除。
// 覆盖名不合法（空、含 '=' 或 NUL）或值含 NUL 时整次合并失败。
[[nodiscard]] bool MergePosixEnvironmentEntries(const std::vector<std::string>& baseEntries,
                                                const std::vector<git::EnvironmentOverride>& overrides,
                                                std::vector<std::string>* mergedEntries,
                                                std::wstring* failureReason);

// 读取当前进程环境（environ）。readFailed 区分“调用失败/环境不可见”与“环境真的为空”。
// 继承项原样复制，不重新编码值：非 UTF-8 的旧值也必须原样交给子进程。
[[nodiscard]] std::vector<std::string> GetCurrentPosixEnvironmentEntries(bool* readFailed = nullptr);

// 集中 Git 环境策略的 Linux 装配点：继承环境 − 数字后缀配置注入项 + MakeGitEnvironmentPlan。
// 失败时 entries 为空、failureReason 非空，且只含变量名与说明，不含任何变量值。
[[nodiscard]] PosixChildEnvironment BuildGitChildEnvironment(
    git::GitRunPurpose purpose, const std::vector<git::EnvironmentOverride>& operationOverrides);

}  // namespace gc::platform
