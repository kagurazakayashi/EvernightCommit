#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/workspace_model.h"

namespace gc::git {

// 「提交历史」选项卡里那份「文件变化」列表的可移植逻辑（本模块不碰任何 Win32 API）：
//   1) 构造一次只读查询，问出一条提交相对其第一个父提交改了哪些文件；
//   2) 按 Git 的 name-status 约定把输出切成一条条文件变化（沿用 ChangeItem 的字段形态，
//      与工作区那两个列表共用同一套「条目 ⇄ 行」约定，界面不对提交里的条目另立一套模型）；
//   3) 校验「双击文件变化 → 查看该文件详细差异」要交给命令窗口的 git show 参数。
//
// 查询参数（在本机 Git 2.55 实测核对，不是按记忆推測）：
//   git show --format= --name-status --find-renames -z --first-parent <oid>
//   * `--format=` 关掉提交头：本查询只要文件名清单，不要差异正文，输出因此只有 name-status 记录。
//   * `--name-status` 对每条变化给出「状态字母 <TAB> 路径」；在 -z 下分隔符改成 NUL：
//     记录形态实测为 `M\0a.txt\0R100\0old\0new\0A\0n.txt\0`——状态字母自成一段，
//     重命名/复制另带旧、新两条路径，其余条目只带一条路径。整段末尾只有一个 NUL，不补空记录。
//   * `--find-renames` 显式开启重命名检测（不依赖 diff.renames 的版本默认值）。
//   * `--first-parent` 让合并提交给出相对第一个父提交的变化。实测：默认形态下 `git show`
//     对合并提交一个 name-status 记录都不输出（组合差异只在给出 `-m`/`--cc` 时才有），
//     那样「选中一条合并提交却看不到任何文件」会被误读成「这条提交没改东西」。
//     对非合并提交，`--first-parent` 是无副作用的同义项。
//   * `--no-ext-diff --no-textconv`：不调用使用者配置的外部 diff / textconv。
//   * 对象 ID 放在参数最后、不套 `--`（实测 `git show <oid> -- <path>` 会把 ID 当路径限定）；
//     前提是 ID 已通过 LooksLikeFullObjectId：纯小写十六进制，不可能被当成选项或通配。

// 一条提交的文件变化清单：changes 为空且 error 为空表示这条提交相对第一个父提交没有变化
// （空提交，或合并提交的变化都已在第一个父提交里）。
struct CommitChangesParseResult {
  std::vector<ChangeItem> changes;
  // 非空表示输出不符合 name-status 约定。与 porcelain v2 同一原则：宁可整体报告读取失败，
  // 也不能展示一份「少了几条、或某条路径是半截」的清单——双击一条看不准的记录会 show 错文件。
  std::wstring error;
};

// 一次「某条提交改了哪些文件」读取的查询参数。objectId 必须是完整对象 ID。
[[nodiscard]] std::vector<std::wstring> BuildCommitChangesArguments(
    std::wstring_view repositoryDirectory, std::wstring_view objectId);

// 解析 `git show --format= --name-status -z` 的 NUL 分隔输出。
// 记录边界与字段分组任一不合约定即整份作废（见头文件说明）。
[[nodiscard]] CommitChangesParseResult ParseCommitChanges(std::wstring_view nulSeparatedOutput);

// ---- 双击「查看该文件在本条提交里的详细变化」的方案 ----

// 一次 `git show <oid> -- <pathspec>` 的构造结果。allowed 为 false 时 arguments 为空，
// 界面把 refusalReason 讲给用户，绝不弹出命令窗口，也绝不对仓库做任何改动。
struct CommitFileDiffPlan {
  bool allowed = false;
  std::vector<std::wstring> arguments;  // 交给 CommandWindowOperation 的参数数组，不拼 shell 字符串
  std::wstring displayName;             // 进窗口标题与状态栏
  std::wstring operationId;             // 固定 ASCII：commit-file-show
  std::wstring notice;                  // 随状态显示的范围说明（合并提交、重命名等）
  std::wstring refusalReason;
};

// 用列表项里保存的完整对象 ID 与那条文件变化构造 git show。ID 与仓库相对路径都要再校验一次：
// 送出去的必须是合法对象 ID 与工作区内的相对路径，不能是空串、绝对路径或含 `..` 的逃逸路径。
[[nodiscard]] CommitFileDiffPlan BuildCommitFileDiffPlan(const CommitItem& commit,
                                                        const ChangeItem& change);

}  // namespace gc::git
