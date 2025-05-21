#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// PATH 搜索与路径规范化需要的文件系统能力，以回调注入，使本模块保持纯逻辑、可单元测试。
struct FsProbe {
  // 判断路径是否为存在的普通文件（目录与不存在都返回 false）。
  std::function<bool(const std::wstring&)> fileExists;
  // 规范化为绝对路径（折叠 "."、".." 与斜杠）；失败时返回空串，调用方回退原值。
  std::function<std::wstring(const std::wstring&)> toAbsolute;
};

// 效果对应 `where git` 的搜索意图：只按 PATH 环境变量条目顺序查找 exeName，不扫描磁盘、
// 不查注册表。返回规范化后的绝对路径列表；Windows 路径大小写不敏感，重复候选只保留第一次出现。
// 顺序就是 PATH 的书写顺序，不代表任何偏好；界面上的候选请用 SortGitCandidatesByPreference 排档位。
std::vector<std::wstring> SearchPathForExecutable(std::wstring_view pathEnv, std::wstring_view exeName,
                                                  const FsProbe& fs);

// 把用户输入规范化为绝对路径：不含目录分隔符的裸名（如 "git"）按 PATH 解析；
// 含目录分隔符（或 UNC）时按当前目录折叠为绝对路径。空输入返回空串。
std::wstring ResolveExecutableInput(std::wstring_view input, std::wstring_view pathEnv, const FsProbe& fs);

// 一台机器上常常同时装着好几个 git.exe：Git for Windows 的 cmd 入口、它的 bin / ucrt64 / mingw64
// 实体，以及 MSYS2（或 Git 自带 usr\bin）那套依赖 msys-2.0.dll 的 git。它们都能跑，但界面默认该选
// 哪一个、下拉列表里谁排在前面，是有讲究的：cmd 入口是面向命令行用法的那一个，msys 那一套按 POSIX
// 路径习惯工作，拿来驱动本程序的参数数组最容易出岔子。分档只看路径文本，不碰文件系统。
enum class GitCandidateFamily {
  cmdShim,  // <...>\cmd\git.exe：排在最前，也是默认值
  other,    // 其它形态：保持中间
  msys,     // msys / msys2 树里的、或落在 <...>\usr\bin 下的那一个：排在最后
};

[[nodiscard]] GitCandidateFamily ClassifyGitCandidate(std::wstring_view path);

// 就地按档位做稳定排序：同档之间保持原有顺序（也就是 PATH 顺序），只把档位挪到该去的位置。
void SortGitCandidatesByPreference(std::vector<std::wstring>& candidates);

}  // namespace gc::git
