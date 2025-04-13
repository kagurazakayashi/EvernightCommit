#pragma once

#include <string>
#include <string_view>

#include "git/commit_plan.h"
#include "git/diff_view.h"

namespace gc::platform {

// 路径是否为存在的普通文件（目录与不存在都返回 false）。能否执行由 --version 探测判定。
[[nodiscard]] bool IsExistingRegularFile(std::wstring_view path);

// 路径是否为存在的目录（仓库输入的基本可用性检查；文件与不存在都返回 false）。
[[nodiscard]] bool IsExistingDirectory(std::wstring_view path);

// GetFullPathNameW 规范化：折叠 "."、".."、多余斜杠，相对路径按进程当前目录展开。失败返回空串。
[[nodiscard]] std::wstring ToAbsolutePath(std::wstring_view path);

// 以 baseDirectory 为基准展开相对路径（Git 的 --git-common-dir 在旧版本可能返回相对路径）。
// path 已是绝对路径时只做折叠；baseDirectory 为空时按进程当前目录展开。失败返回空串。
[[nodiscard]] std::wstring ToAbsolutePathInDirectory(std::wstring_view baseDirectory, std::wstring_view path);

// 把正斜杠统一成反斜杠（Git 在 Windows 上输出正斜杠路径；两者在本系统等价）。
[[nodiscard]] std::wstring NormalizePathSeparators(std::wstring_view path);

// 是否已是绝对路径（盘符或 UNC 前缀）；用于判断是否需要按仓库目录展开。
[[nodiscard]] bool IsAbsolutePath(std::wstring_view path);

// 当前进程的 PATH 环境变量（UTF-16）。未定义时返回空串。
[[nodiscard]] std::wstring GetPathVariable();

// 采集“未跟踪文件能不能在命令窗口里按文本显示”所需的事实：是否存在、是否目录、
// 能否打开读取、大小，以及开头若干字节里有没有 NUL（有即按二进制对待）。
// 只做只读探测：不创建、不改写、不改时间戳，也不 Git 自己去猜路径。
// 采样长度上限由 git::kBinaryProbeBytes 决定，超大文件因此同样只需极短一次读取。
[[nodiscard]] git::WorktreeFileFacts ProbeWorktreeFileForPreview(std::wstring_view absolutePath);

// 探测「这个仓库里有没有特殊的 Git 流程正在走」与「索引是否正被别人写着」。
//
// 依据只有 Git 自己留在 Git 目录里的那些痕迹，全部是只读的「存不存在」判断：
//   MERGE_HEAD / REVERT_HEAD / CHERRY_PICK_HEAD / BISECT_LOG 这些档案，
//   rebase-merge\ 与 rebase-apply\ 这两个目录，以及 index.lock。
// absoluteGitDir 必须是仓库识别给出的「绝对 Git 目录」（链接工作树下是
// <主仓>\.git\worktrees\<名字>，那些状态档案就落在这一层，不能拿主仓的 .git 去猜）。
// 目录为空或读不到时一律按「没有痕迹」返回：本程序从不因为「看不见」而拒绝一次合法操作，
// 真正能不能提交由 Git 自己判定，界面看的是退出码。
[[nodiscard]] git::RepositoryWorkflowState ProbeRepositoryWorkflowState(
    std::wstring_view absoluteGitDir);

}  // namespace gc::platform
