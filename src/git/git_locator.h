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
std::vector<std::wstring> SearchPathForExecutable(std::wstring_view pathEnv, std::wstring_view exeName,
                                                  const FsProbe& fs);

// 把用户输入规范化为绝对路径：不含目录分隔符的裸名（如 "git"）按 PATH 解析；
// 含目录分隔符（或 UNC）时按当前目录折叠为绝对路径。空输入返回空串。
std::wstring ResolveExecutableInput(std::wstring_view input, std::wstring_view pathEnv, const FsProbe& fs);

}  // namespace gc::git
