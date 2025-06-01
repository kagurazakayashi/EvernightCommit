#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// ---------- 交给 Git 子进程读取的临时文件 ----------
//
// 提交信息与 pathspec 清单都不能塞进命令行：前者含换行与用户原文，后者可能上千条并触发
// 长度上限。两者都写成临时文件，命令里只出现一个路径。
//
// 统一约定（各平台实现必须满足）：
//   * 落点是系统临时目录；路径来自平台规则，不接受调用方指定任意目录；
//   * 文件名含进程号与进程内递增序号，独占创建，绝不覆盖已有文件；
//   * 文件只对当前用户可读（Linux 0600；Windows 使用用户临时目录与独占句柄）；
//   * 只由调用方在确认 Git 已经退出后回收；提前删除会让一次合法操作变成 Git 报错；
//   * 内容严格 UTF-8；无法无损编码的正文/路径一律拒绝写出，不用 U+FFFD 代替。

struct CommitMessageFileWrite {
  bool written = false;
  std::wstring path;         // written 时的绝对路径（会原样出现在 git commit -F 后面）
  size_t payloadBytes = 0;   // 实际写出的 UTF-8 字节数
  std::wstring failureReason;
};

struct PathspecFileWrite {
  bool written = false;
  std::wstring path;
  std::wstring failureReason;
};

// 把提交信息写成 UTF-8 文件。内容只做编码与“末尾补一个换行”，不改行尾、不修剪。
// 空信息拒绝写出。
[[nodiscard]] CommitMessageFileWrite WriteCommitMessageFile(std::wstring_view message);

// 删除本模块写出的提交信息文件。路径为空或文件已不存在时什么都不做；不抛异常。
void RemoveCommitMessageFile(std::wstring_view path);

// 把 pathspec 元素逐条写成 UTF-8 + NUL 分隔，供
// `--pathspec-from-file=<path> --pathspec-file-nul` 读取。
// 空列表、空条目、含内嵌 NUL 的条目一律拒绝；本函数不补 `:(literal)` 前缀。
[[nodiscard]] PathspecFileWrite WriteNulPathspecFile(const std::vector<std::wstring>& pathspecElements,
                                                     std::wstring_view filePrefix = L"gc-add");

void RemoveNulPathspecFile(std::wstring_view path);

}  // namespace gc::platform
