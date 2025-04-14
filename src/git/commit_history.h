#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "git/workspace_model.h"

namespace gc::git {

// 「最近提交」列表的可移植逻辑（本模块不碰任何 Win32 API）：
//   1) 构造一次只读 git log 的参数数组；
//   2) 按约定把输出切成一条条提交记录（完整对象 ID、作者、作者时间、标题、父提交）；
//   3) 校验双击「查看提交详情」要交给命令窗口的 git show 参数。
//
// 展示范围固定为「从当前 HEAD 可达的最近 kRecentCommitLimit 条」：
// 不按 --all 混合所有分支（那会把无关分支的历史当成当前历史），也不一次读取大型仓库的全部历史。
// detached HEAD 时 HEAD 仍可解析，读取照常；尚无提交时调用方根本不发这条查询。
//
// 输出格式的选定依据，在本机 Git 2.55 实测核对：
//   --format=%H%x00%an%x00%at%x00%s%x00%p 配合 -z：
//     %H 完整对象 ID（SHA-1 为 40、SHA-256 仓库为 64 个十六进制字符，一律原样保存）；
//     %an 作者姓名（可以含制表符、空格与任何非 NUL 字符）；
//     %at 作者时间的 Unix 秒（不解析 Git 打印的日期文本，展示由本机时区换算，见 CommitTimeFormatter）；
//     %s 标题（提交消息第一行，可能为空——--allow-empty-message 的提交）；
//     %p 父提交列表（空格分隔；根提交为空、合并提交有两个以上，据此识别合并提交）。
//   -z 让每条记录以 NUL 结束而不是换行：标题、作者里出现的任何换行以外的分隔符都不参与切分，
//     因此绝不按空格/制表符拆列，中文、特殊字符、长标题都能原样回来。
//   -c log.showSignature=false 覆盖用户配置：签名提交默认往输出里插入校验块，会破坏按字段分组的约定。
//   --no-decorate 关闭分支装饰（%d 没用到，但 Git 5.x 起 log.decorate 的默认值可能随版本变化，
//     显式关掉最稳妥——装饰文本会混进标题之前的位置）。

// 默认展示上限：再多也只是让列表更长，读取时间线性上涨，而界面只需要「最近这一段」。
// 达到上限时快照摘要会明确说明更早的历史没有读取，不假装这就是全部。
inline constexpr size_t kRecentCommitLimit = 100;

// 展示用短 ID 的长度：十六进制前缀在任何仓库里都能唯一指向那条提交吗？不保证——
// 它只用于阅读与标题展示，一切命令都用完整对象 ID。
inline constexpr size_t kShortObjectIdLength = 8;

// 对象 ID 长度下限：Git 的可信完整对象 ID 只可能是 40（SHA-1）或 64（SHA-256），
// 取 24 作下限只为「明显不完整的残段」兜底，不硬编码 SHA-1 长度。
inline constexpr size_t kMinObjectIdLength = 24;
inline constexpr size_t kMaxObjectIdLength = 64;

// 每条记录 5 个字段：对象 ID、作者、作者时间（Unix 秒文本）、标题、父提交列表。
inline constexpr size_t kCommitFieldCount = 5;

// 时间字段解析结果：Unix 秒。非法（非数字、越界）时返回 false，由调用方判为输出不合约定。
[[nodiscard]] bool ParseCommitEpochSeconds(std::wstring_view field, long long* outSeconds);

// 是否形如完整的十六进制对象 ID：仅小写字母数字，长度在 [kMinObjectIdLength, kMaxObjectIdLength]。
// %H 的输出永远小写；出现别的形式说明输出被破坏，绝不能把这段文本原样交给 git show。
[[nodiscard]] bool LooksLikeFullObjectId(std::wstring_view objectId);

// 展示用短 ID：取前 kShortObjectIdLength 个字符；比它更短的对象 ID 原样返回。
[[nodiscard]] std::wstring ShortObjectId(std::wstring_view objectId);

// 一次「最近提交」读取的查询参数。limit 允许测试缩小，界面固定传 kRecentCommitLimit。
[[nodiscard]] std::vector<std::wstring> BuildRecentCommitsArguments(
    std::wstring_view repositoryDirectory, size_t limit = kRecentCommitLimit);

// 作者时间（Unix 秒）→ 展示文本的回调。时区换算依赖操作系统（同一个秒值在界面上要显示成
// 本机那一天的时刻），因此由平台层注入；纯逻辑测试注入确定性的桩。
using CommitTimeFormatter = std::function<std::wstring(long long epochSeconds)>;

struct CommitHistoryParseResult {
  std::vector<CommitItem> commits;
  // 非空表示输出不符合字段分组约定。与 porcelain v2 同一原则：宁可整体报告读取失败，
  // 也不能展示一份「少了几条、或某条的 ID 是半截」的历史——双击一条看不准的记录会 show 错提交。
  std::wstring error;
  // 返回的记录条数达到 limit：更早的历史没有读取。
  bool truncated = false;
};

// 解析「-z + 五字段 %x00 分隔」的 git log 输出。formatTime 为 null 时展示文本留空、只填秒数字段。
// truncatedAt 传请求的条数上限；输出条数不足 5 的整数倍、或任一记录的关键字段不合约定即报错。
[[nodiscard]] CommitHistoryParseResult ParseRecentCommits(std::wstring_view nulSeparatedOutput,
                                                           size_t limit,
                                                           const CommitTimeFormatter& formatTime);

// ---- 双击「查看提交详情」的方案 ----

// 一次 git show 的构造结果。allowed 为 false 时 arguments 为空，界面把 refusalReason 讲给用户，
// 绝不弹出命令窗口，也绝不因为查看失败而对仓库做任何改动（show 本身就是只读命令，
// 更不会有「查看失败把分支切过去」的可能——我们从不构造 checkout/reset）。
struct CommitShowPlan {
  bool allowed = false;
  std::vector<std::wstring> arguments;  // 交给 CommandWindowOperation 的参数数组，不拼 shell 字符串
  std::wstring displayName;             // 进窗口标题与状态栏
  std::wstring operationId;             // 固定 ASCII：commit-show
  std::wstring notice;                  // 随状态显示的范围说明（合并提交的组合差异可能为空）
  std::wstring refusalReason;
};

// 用列表项里保存的完整对象 ID 构造 git show。ID 会再校验一次（LooksLikeFullObjectId）：
// 界面到执行器之间隔着好几层，最后关头仍要确认送出去的是一个合法的十六进制对象 ID，
// 而不是被污染的输出、空串或含引号的任意文本。
[[nodiscard]] CommitShowPlan BuildCommitShowPlan(const CommitItem& item);

// 摘要文本（拼进读取成功后的状态说明）：条数，以及达到上限时「更早历史未读取」的声明。
// count 为 0 时返回空串——那时界面靠历史列的「还没有任何提交」说明即可，不必重复一句。
[[nodiscard]] std::wstring BuildRecentCommitsNotice(size_t count, bool truncated);

}  // namespace gc::git
