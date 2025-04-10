#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// 一次「git add 路径清单文件」写入的结果。
struct PathspecFileWrite {
  bool written = false;
  std::wstring path;        // 成功时：清单文件的绝对路径
  std::wstring failureReason;  // 失败时：具体原因（含 Windows 错误文本），界面原样转述
};

// 把 Git 的 pathspec 元素按 UTF-8 原样写出、每条以 NUL 结尾，供
// `git add --pathspec-from-file=<本文件> --pathspec-file-nul` 读取。
//
// 为什么用文件而不是把路径逐个写成参数：
//   * Windows 命令行约 8 千字符上限，选中上千个文件时参数形态必然放不下；
//     实测 1200 条路径一次 add 约 1 秒，且命令里只有文件名。
//   * 内容逐条原样写入，不需要任何转义。但实测要点：`--pathspec-file-nul` 只关闭
//     “引号／C 转义”这一层，pathspec 的 glob 魔法仍然生效（清单里直接写 brk[x].txt
//     会连带命中 brkx.txt）。所以进来的每条都必须是带 :(literal) 前缀的字面元素，
//     这一层由 git/staging_plan 保证；本函数不补前缀，也不做任何改写。
//
// 落点在系统临时目录，文件名含进程号与递增序号并用 CREATE_NEW 独占创建：
// 已存在同名文件时换一个序号重试，绝不覆盖别人的档案。清单只含路径，不含提交正文。
// 空列表不会被写成空文件：那是「没有范围」，界面应在构造方案阶段就拒绝执行。
PathspecFileWrite WriteNulPathspecFile(const std::vector<std::wstring>& pathspecElements);

// 删除清单文件。只在 Git 已经退出（拿到退出码或判为结果未知）之后调用 ——
// 命令窗口保留期间 Git 可能还在读它。不存在的档案视为已清理，不报错。
void RemoveNulPathspecFile(std::wstring_view path);

}  // namespace gc::platform
