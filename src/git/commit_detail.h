#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_history.h"  // CommitTimeFormatter / LooksLikeFullObjectId：同一套字段约定与校验

namespace gc::git {

// 「提交详情」面板的可移植逻辑（本模块不碰任何 Win32 API）：
//   1) 构造一次只读 git log 单条提交的参数数组；
//   2) 按约定把那一条切成七个字段（完整对象 ID、作者姓名与邮箱、两个时间、标题、描述正文）；
//   3) 拼出面板显示与剪贴板复制共用的那份文本（排版只在这里，界面不拼字符串）。
//
// 为什么单独再问一次，而不是把「最近提交」那条读取的格式串加长：
//   列表一次要取 kRecentCommitLimit 条，把 %ae、%ct、%b 塞进去等于把每条提交的完整描述
//   一起读回来（合并提交动辄上百行），字节上限与解析失败的概率都被推高，而那一段失败会让
//   整个提交历史列显示不出来。这里只在「选中某一条」之后问那一条，代价是一条毫秒级的本地查询。
//
// 查询形态（本机 Git 2.55 实测核对，不是按记忆推测）：
//   git -C <目录> --no-optional-locks --no-replace-objects -c log.showSignature=false
//       log --no-decorate --format=%H%x00%an%x00%ae%x00%at%x00%ct%x00%s%x00%b -z -n 1 <对象 ID>
//   * 七个字段以 %x00（NUL）分隔，`-z` 让这一条记录以 NUL 结束：描述里出现任何换行以外的
//     字符都不参与切分，中文、制表符、引号都能原样回来。
//   * 没有描述时 %b 是一个空段（实测尾部形如 `…\0\0`），有描述时末尾自带一个 LF（实测
//     `-m 标题 -m 正文` 造出来的对象里正文段以单个 LF 结束）：展示前去掉结尾换行，内部换行原样保留。
//   * `%n` 不参与字段切分（只用于展示），因此这里完全不用它；描述内部一律只认 Git 记下的字节。
//   * `%at` 与 `%ct` 是作者时间与提交者时间的 Unix 秒：不解析 Git 打印的日期文本，
//     展示由本机时区换算（与「最近提交」列的时间列同一个换算依据，两处不会给出两个时刻）。
//   * `-c log.showSignature=false`：签名提交默认往输出里插入校验块，会破坏按字段分组的约定。
//   * 对象 ID 放在参数最后、不套 `--`（实测 `git log -- <id>` 会把它当成路径限定，输出变成空）。
//     前提是它已通过 LooksLikeFullObjectId：纯小写十六进制，不可能被当成选项，也不含通配字符。

// 每条记录 7 个字段：对象 ID、作者姓名、作者邮箱、作者时间、提交者时间、标题、描述正文。
inline constexpr size_t kCommitDetailFieldCount = 7;

// 描述正文的展示上限（字符数）。为什么要有上限：这块面板是只读编辑框，不是差异查看器——
// 几十万字节的合并说明既读不动也不该被当成「已经读完」。取值刻意留在只读编辑框默认
// 文本上限（30000 字）以下，因此截断后的整段文本一定能完整显示，不会在控件里被二次截掉。
// 截断这件事会写进展示文本，剪贴板拿到的也是同一份文本，不假装这就是全部描述。
inline constexpr size_t kCommitDetailDescriptionLimit = 16384;

// 一次「选中提交的完整信息」读取的结果。字段全部来自 Git 对那一个对象的回答，
// 不由列表条目补齐：列表那一份只用来确定「问的是哪一条」。
struct CommitDetail {
  std::wstring objectId;
  std::wstring authorName;
  std::wstring authorEmail;
  std::wstring authoredAt;    // 作者时间的本机时区展示文本
  std::wstring committedAt;   // 提交者时间的本机时区展示文本
  std::wstring summary;       // 标题（提交消息第一行，可以为空）
  std::wstring description;   // 描述正文：结尾换行已去掉，内部换行原样保留
  bool descriptionTruncated = false;  // 描述超过展示上限：只有前面这一段
};

struct CommitDetailParseResult {
  bool ok = false;
  CommitDetail detail;
  // 非空表示这份输出不能当成那一条提交的完整回答。与「最近提交」同一原则：
  // 宁可整条不显示，也不能显示一份「哈希是半截、或描述缺了后半段」的详情——
  // 面板上的文本是要被用户复制去别处用的。
  std::wstring error;
};

// 一次「读某条提交的完整信息」的查询参数。objectId 必须是完整对象 ID
// （界面先经 LooksLikeFullObjectId 校验过，这里不再放宽）。
[[nodiscard]] std::vector<std::wstring> BuildCommitDetailArguments(
    std::wstring_view repositoryDirectory, std::wstring_view objectId);

// 解析 `--format=<七字段> -z -n 1` 的输出。formatTime 为 null 时两个时间留空，只填展示文本以外的字段。
// requestedObjectId 是界面发起这次读取时那一条的完整对象 ID：Git 答回来的对象 ID 必须与它逐字符相同，
// 否则说明行在读取期间换成了另一条提交（或输出被污染），整份作废。
[[nodiscard]] CommitDetailParseResult ParseCommitDetail(std::wstring_view nulSeparatedOutput,
                                                        std::wstring_view requestedObjectId,
                                                        const CommitTimeFormatter& formatTime);

// 送进 Win32 编辑框（以及剪贴板）之前的分行统一：单个 LF 与单个 CR 都算换行，
// 已经是 CRLF 的那一对不重复补。Git 记下的消息正文只有 LF，而编辑框见到单个 LF 只回到行首、
// 不换行，粘到记事本里同样是一整行——展示与复制要的都是 CRLF。
[[nodiscard]] std::wstring NormalizeTextForEdit(std::wstring_view text);

// 面板显示与「复制」按钮共用的那份文本：每行一个字段，`标签：值`，描述正文放在最后原样铺开。
// 换行一律 CRLF（由 NormalizeTextForEdit 统一），末尾不带空行。
[[nodiscard]] std::wstring FormatCommitDetailText(const CommitDetail& detail);

}  // namespace gc::git
