#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// Linux 交互式操作目录的共用实现。
//
// 目录名 GcOp<pid>x<随机段>，只在 mkdirat 真的新建成功时才算认领；父目录必须是
// ResolveScratchDirectory() 验证过的临时根，且以 O_DIRECTORY|O_NOFOLLOW 打开。
// 目录建成后立刻收紧到 0700，并在里面独占创建 lease.txt（flock 持有到操作落账）。
// 之后所有说明书/标记/结果文件都通过 openat(dirfd, ...) + O_NOFOLLOW 访问，
// 不跟随符号链接，也不接受路径在两步之间被换掉。
//
// helper 只接受“临时根 + 本程序生成的目录名”这一个形态；它核对目录属主、
// 权限与说明书的 token/nonce，任何一项不符都不执行。

struct ClaimedOperationDirectory {
  bool ok = false;
  int dirFd = -1;    // 操作目录句柄（调用方持有）
  int leaseFd = -1;  // lease.txt 上的 flock 句柄（调用方持有到落账）
  std::string name;  // 目录名（ASCII）
  std::string pathBytes;
  std::wstring path;
  std::wstring rootPath;  // 认领时使用的临时根（传给 helper，避免终端服务环境不同）
  std::wstring failureReason;
};

// 在临时根下认领一个新目录。撞名时换随机段重试，绝不复用已存在的目录。
[[nodiscard]] ClaimedOperationDirectory ClaimOperationDirectoryLinux();

// 关闭并释放认领结果。leaseFd 先关，目录句柄后关。
void CloseOperationDirectory(ClaimedOperationDirectory* directory);

// helper 侧：按绝对路径打开执行器交给它的操作目录，并核对父目录就是临时根。
struct OpenedOperationDirectory {
  bool ok = false;
  int dirFd = -1;
  std::string name;
  std::string pathBytes;
  std::wstring path;
  std::wstring failureReason;
};
// scratchRootOverride 非空时按它校验父目录（执行器把认领时用的临时根显式交给 helper，
// 以兼容复用已有终端服务、服务端 TMPDIR/XDG_RUNTIME_DIR 与本进程不同的情形）；
// 为空时使用 helper 自己解析出的临时根。
[[nodiscard]] OpenedOperationDirectory OpenOperationDirectoryForHelper(
    std::wstring_view path, std::wstring_view scratchRootOverride = {});

void CloseOpenedOperationDirectory(OpenedOperationDirectory* directory);

// 独占创建（O_EXCL|O_NOFOLLOW、0600）一个文件并写入全部字节；写完 fsync。
[[nodiscard]] bool WriteExclusiveFileAt(int dirFd, std::string_view name, std::string_view bytes,
                                        std::wstring* failureReason);

// 结果发布：写 tmp（独占创建；若残留则只删同名 temp）→ fsync → renameat 覆盖 result.txt
// → fsync 目录。观察端只会看到“没有结果”或“完整的一条结果”。
[[nodiscard]] bool PublishResultFileAt(int dirFd, std::string_view tempName,
                                       std::string_view finalName, std::string_view bytes,
                                       std::wstring* failureReason);

// 读一个普通文件。不存在返回 true 且 exists=false；超过上限、非普通文件、属主不是
// 当前用户或无法无损读取都返回 false。
[[nodiscard]] bool ReadOperationFileAt(int dirFd, std::string_view name, size_t maxBytes,
                                       std::string* bytes, bool* exists,
                                       std::wstring* failureReason);

// 文件锁探测：返回 held 表示另一个进程正持有 flock。文件不存在是 absent；
// 其它无法判断的情况一律 blocked（调用方必须按“可能仍在使用”处理）。
enum class OperationFileLockState {
  absent,
  free,
  held,
  blocked,
};
[[nodiscard]] OperationFileLockState ProbeOperationFileLockAt(int dirFd, std::string_view name);

// helper 专用：独占创建并持有 start.txt 的 flock，返回句柄（调用方持有到结果发布完成）。
[[nodiscard]] int CreateHeldStartMarkerAt(int dirFd, std::string_view text,
                                          std::wstring* failureReason);

// 回收一个本程序生成的目录：只删除 git::kOperationFileNames 名单内的文件，
// 目录里还有别的内容就保留。requireSpecConsumed 用于“刚刚结束”的路径：
// 说明书还在而开始标记还没出现时，不排除 helper 尚未读它，因此保留。
[[nodiscard]] bool TryReclaimOperationDirectory(int parentDirFd, std::string_view name,
                                                bool requireSpecConsumed,
                                                std::wstring* preservedReason);

// 按绝对路径回收：打开父目录（O_DIRECTORY|O_NOFOLLOW）后调用上面的例程。
// 只用于本进程亲手认领的路径；目录名形态、属主、权限与文件锁仍会逐项复核。
[[nodiscard]] bool ReclaimOperationDirectoryByPath(std::wstring_view path,
                                                   bool requireSpecConsumed,
                                                   std::wstring* preservedReason);

// 目录最后写入时间是否已经超过给定秒数；时间戳不可信时保守返回 false。
[[nodiscard]] bool IsOperationDirectoryOlderThan(int parentDirFd, std::string_view name,
                                                 std::uint64_t seconds);

}  // namespace gc::platform
