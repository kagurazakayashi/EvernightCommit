#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "git/commit_plan.h"
#include "git/diff_view.h"

namespace gc::platform {

// ---------- 路径与文件系统探测契约 ----------
//
// 这一层只描述“路径当前是什么”，不启动 Git。两种平台语义不同，但事实名称一致：
//   * Windows：路径是 UTF-16，大小写不敏感；权限位不适用（permissions 为 0）。
//   * Linux：路径是字节，本工程只接受能无损表示成 UTF-8 的名字；区分大小写，
//     符号链接与目标分开记录，dev/inode 就是“原始文件身份”。
//
// 三态存在性（PathExistence）是本契约的重点：不存在（ENOENT/ENOTDIR）、存在但读不到
// （EACCES/EPERM）与其它失败（EIO/ELOOP/名字过长……）必须能分辨。调用方不得把
// unknown 当成 missing 去放行写操作，也不得把 inaccessible 说成“文件已消失”。

enum class PathProbeStatus {
  ok,               // 至少 lstat/GetFileAttributes 成功，事实可用
  missing,          // 明确不存在（Linux: ENOENT/ENOTDIR）
  inaccessible,     // 权限不足，无法确认存在性（Linux: EACCES/EPERM）
  unrepresentable,  // 路径不能无损表示成 UTF-8/UTF-16，未发起系统调用
  failed,           // 其它系统错误（Linux: EIO/ELOOP/ENAMETOOLONG 等）
};

enum class PathExistence {
  missing,
  present,
  unknown,  // inaccessible/failed：不能据此下“不存在”的结论
};

enum class PathEntryKind {
  missing,
  regularFile,
  directory,
  symbolicLink,
  other,  // FIFO、socket、设备等
};

struct PathFacts {
  PathProbeStatus status = PathProbeStatus::failed;
  PathExistence existence = PathExistence::unknown;
  PathEntryKind kind = PathEntryKind::missing;        // lstat 看到的条目本身
  PathEntryKind targetKind = PathEntryKind::missing;  // 跟随最终符号链接后的目标
  // 目标侧的存在性：符号链接目标 EACCES/ELOOP 时是 unknown，不能当成 missing。
  PathExistence targetExistence = PathExistence::unknown;
  bool isSymlink = false;
  bool targetExists = false;
  bool readable = false;    // 当前用户可读（readable 与 targetExists 独立）
  bool executable = false;  // 普通文件且当前用户可执行
  bool ownerIsCurrentUser = false;
  unsigned int permissions = 0;  // POSIX mode & 07777；Windows 为 0
  unsigned long long device = 0;
  unsigned long long inode = 0;
  unsigned long long targetDevice = 0;
  unsigned long long targetInode = 0;
  std::wstring failureReason;
};

// 探测一条路径。followFinalSymlink 只控制“目标”几项；符号链接本身永远由 lstat 记录，
// 不会被折叠成一个普通文件。中间目录上的符号链接由内核照常跟随（本层不解析路径）。
[[nodiscard]] PathFacts ProbePath(std::wstring_view path, bool followFinalSymlink = true);

// 便捷布尔判断：只看“是否存在的普通文件/目录/可执行普通文件”。无法区分“不存在”、
// “权限不足”与“无法无损表示”——需要这些区别的调用方必须用 ProbePath。
[[nodiscard]] bool IsExistingRegularFile(std::wstring_view path);
[[nodiscard]] bool IsExistingDirectory(std::wstring_view path);
[[nodiscard]] bool IsExecutableRegularFile(std::wstring_view path);

// 规范化为绝对路径：折叠 "."、".."、多余分隔符。Linux 不解析符号链接，也绝不把
// 反斜杠改成分隔符（那是 Windows 规则，Linux 上反斜杠是合法文件名字符）。
[[nodiscard]] std::wstring ToAbsolutePath(std::wstring_view path);
[[nodiscard]] std::wstring ToAbsolutePathInDirectory(std::wstring_view baseDirectory,
                                                     std::wstring_view path);

// Windows：把正斜杠统一成反斜杠。Linux：原样返回（反斜杠不是分隔符）。
[[nodiscard]] std::wstring NormalizePathSeparators(std::wstring_view path);

[[nodiscard]] bool IsAbsolutePath(std::wstring_view path);

// 当前进程的 PATH。取不到（或 Linux 上含非法 UTF-8）时返回空串。
[[nodiscard]] std::wstring GetPathVariable();

// 采集“未跟踪文件能不能按文本显示”所需的事实。只读：不创建、不改写、不改时间戳。
[[nodiscard]] git::WorktreeFileFacts ProbeWorktreeFileForPreview(std::wstring_view absolutePath);

// 三态流程痕迹探测：每个痕迹问“在 / 明确不在 / 问不成”。任何一个问不成，
// readable 就是 false，调用方必须按“不能确定”处理。
[[nodiscard]] git::WorkflowProbeResult ProbeRepositoryWorkflowStateStrict(
    std::wstring_view absoluteGitDir);

// 宽松流程痕迹探测：读不到就按“没有痕迹”，只用于不据此放行写命令的展示链路。
[[nodiscard]] git::RepositoryWorkflowState ProbeRepositoryWorkflowState(
    std::wstring_view absoluteGitDir);

}  // namespace gc::platform
