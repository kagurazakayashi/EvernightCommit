#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// 一次提交信息临时文件的写出结果。
struct CommitMessageFileWrite {
  bool written = false;
  std::wstring path;           // written 时的绝对路径（会原样出现在 git commit -F 后面）
  size_t payloadBytes = 0;     // 实际写出的字节数（UTF-8），确认框里报告提交信息的规模
  std::wstring failureReason;  // 没写成时面向用户的说明
};

// 把表单合成出来的提交信息写成 UTF-8 临时文件，供 `git commit -F <path>` 读取。
//
// 为什么一定要走文件：提交信息是可多行的用户文字，绝不进命令行 —— 命令行要经过 cmd.exe
// 的引号与百分号展开两轮，换行更是直接把脚本行截断。写成 UTF-8 文件后，
// 命令里只出现一个路径，正文内容与 shell 再无接触面。
//
// 写法约定（与本程序的「不润色用户文字」一致）：
//   * 内容就是传入的文字本身，只做 UTF-16 → UTF-8 编码，不改行尾、不修剪、不加注释；
//   * 末尾补一个换行（Git 会自己处理缺失的结尾换行，这里补上让文件是正常文本）；
//   * 文件以 CREATE_NEW 独占创建，名字含进程号与进程内序号，绝不覆盖别人的文件；
//   * 空信息一律拒绝写出（没有正文的可执行命令不该被发出去）。
//
// 回收由调用方在确认 Git 已经退出之后执行（见 RemoveCommitMessageFile）：
// 命令窗口里的 Git 可能还在读它，提前删会让一次合法的提交变成 Git 的报错。
[[nodiscard]] CommitMessageFileWrite WriteCommitMessageFile(std::wstring_view utf16Message);

// 删除本模块写出的信息文件。路径为空时什么都不做；删不掉（被占用、权限）不抛异常也不报错 ——
// 用户要知道的是 Git 的结果，不是一个临时文件的删除状态，残留会被系统临时目录回收。
void RemoveCommitMessageFile(std::wstring_view path);

}  // namespace gc::platform
