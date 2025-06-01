#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// Git 可执行文件的发现结果。这里只回答“哪些候选路径可能是 Git 程序”，
// 版本验证（`--version` 与输出解析）属于执行通道，不在这里伪造结论。
struct GitExecutableResolution {
  bool resolved = false;    // path 是存在的普通文件且具备执行条件
  bool exists = false;      // 路径存在（regular file）
  bool executable = false;  // 可执行（Linux: 执行位 + X_OK；Windows: 可执行扩展名）
  std::wstring path;        // 规范化后的路径（失败时保留整理后的输入）
  std::wstring failureReason;
};

// 按当前进程 PATH 发现候选：Linux 用 ':' 分隔且区分大小写，Windows 用 ';' 且大小写不敏感。
// 顺序就是 PATH 顺序；Windows 侧还会按 cmd/msys 档位排序（见 git/git_locator.h）。
[[nodiscard]] std::vector<std::wstring> DiscoverGitCandidates();

// 把用户输入整理成绝对路径。裸名（不带分隔符）按 PATH 查找；找不到时保留整理后的输入，
// 由 ResolveGitExeInput 报告具体原因。
[[nodiscard]] std::wstring NormalizeGitExeInput(std::wstring_view input);

// 手动路径/裸名的可用性检查：是否存在、是否可执行。绝不启动进程，也不宣称版本可用。
[[nodiscard]] GitExecutableResolution ResolveGitExeInput(std::wstring_view input);

}  // namespace gc::platform
