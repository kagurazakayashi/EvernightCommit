#include "git/commit_history.h"

#include <algorithm>
#include <cwchar>

#include "git/repository.h"  // NulRecordsAreComplete：记录边界约定与工作区解析共用同一处判定

namespace gc::git {
namespace {

// 与 porcelain v2 解析同一套切分：记录以 NUL 结束，Git 每条记录的结尾 NUL 会多出一个
// 空片段，它不是一条记录，也不参与字段分组。
std::vector<std::wstring_view> SplitNulTokens(std::wstring_view text) {
  std::vector<std::wstring_view> tokens;
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t nul = text.find(L'\0', cursor);
    const size_t end = (nul == std::wstring_view::npos) ? text.size() : nul;
    tokens.push_back(text.substr(cursor, end - cursor));
    if (nul == std::wstring_view::npos) {
      break;
    }
    cursor = nul + 1;
  }
  if (!tokens.empty() && tokens.back().empty()) {
    tokens.pop_back();
  }
  return tokens;
}

bool IsLowerHex(wchar_t c) noexcept {
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
}

// 父提交字段按空格计数：两个以上父提交即合并提交。计数失败不影响展示，
// 父列表本身只是附加信息，字段内容不合约定（空段/超长）按「非合并」处理即可。
size_t CountParents(std::wstring_view parentsField) {
  size_t count = 0;
  size_t cursor = 0;
  while (cursor < parentsField.size()) {
    while (cursor < parentsField.size() && parentsField[cursor] == L' ') {
      ++cursor;
    }
    if (cursor >= parentsField.size()) {
      break;
    }
    const size_t space = parentsField.find(L' ', cursor);
    if (space == std::wstring_view::npos) {
      ++count;
      break;
    }
    ++count;
    cursor = space;
  }
  return count;
}

// 片段的可读摘要：只用于说明「哪一条读不懂」，取前 40 个字符且不显示控制字符。
[[nodiscard]] std::wstring RecordSample(std::wstring_view record) {
  const size_t limit = std::min<size_t>(record.size(), 40);
  return std::wstring(record.substr(0, limit));
}

}  // namespace

bool ParseCommitEpochSeconds(std::wstring_view field, long long* outSeconds) {
  if (field.empty() || outSeconds == nullptr) {
    return false;
  }
  // 1970 之前的提交时间（或仓库里被写坏的秒数）允许负号；数字部分一律十进制。
  size_t cursor = 0;
  bool negative = false;
  if (field[cursor] == L'-') {
    negative = true;
    ++cursor;
  }
  if (cursor >= field.size()) {
    return false;
  }
  long long value = 0;
  for (; cursor < field.size(); ++cursor) {
    const wchar_t c = field[cursor];
    if (c < L'0' || c > L'9') {
      return false;
    }
    // 提前判溢出：超出 9e18 的位数不可能是一秒时间戳，也不能让它回绕成另一个时刻。
    if (value > 9'000'000'000'000'000'000LL / 10) {
      return false;
    }
    value = value * 10 + (c - L'0');
  }
  // 现实提交时间不会超过十位数（公元 2286 年）；更长的取值按损坏处理，同时挡住 64 位回绕。
  if (value > 9'999'999'999LL) {
    return false;
  }
  *outSeconds = negative ? -value : value;
  return true;
}

bool LooksLikeFullObjectId(std::wstring_view objectId) {
  if (objectId.size() < kMinObjectIdLength || objectId.size() > kMaxObjectIdLength) {
    return false;
  }
  return std::all_of(objectId.begin(), objectId.end(), IsLowerHex);
}

std::wstring ShortObjectId(std::wstring_view objectId) {
  if (objectId.size() <= kShortObjectIdLength) {
    return std::wstring(objectId);
  }
  return std::wstring(objectId.substr(0, kShortObjectIdLength));
}

std::vector<std::wstring> BuildRecentCommitsArguments(std::wstring_view repositoryDirectory, size_t limit) {
  // 标题里出现 % 也不会被展开：参数一项一个元素，不经过任何 shell。
  // 格式串是内部常量，仅 %x00（NUL）与字段名，不含用户可控内容。
  const std::wstring format =
      L"%H%x00%an%x00%at%x00%s%x00%p";
  return std::vector<std::wstring>{
      L"-C",
      std::wstring(repositoryDirectory),
      L"--no-optional-locks",
      L"--no-replace-objects",
      L"-c",
      L"log.showSignature=false",
      L"log",
      // --no-decorate 是 log 的选项，必须排在子命令之后（实测排在前面对 git 本体是未知选项）。
      L"--no-decorate",
      L"--format=" + format,
      L"-z",
      L"-n",
      std::to_wstring(limit)};
}

CommitHistoryParseResult ParseRecentCommits(std::wstring_view nulSeparatedOutput, size_t limit,
                                            const CommitTimeFormatter& formatTime) {
  CommitHistoryParseResult result;
  // 与 porcelain v2 同一条记录边界约定：缺结尾 NUL 的最后一条记录是残缺的（半个标题、
  // 少一段父提交列表），按「字段数不是 5 的倍数」判整份作废还不够早。
  std::wstring recordReason;
  if (!NulRecordsAreComplete(nulSeparatedOutput, recordReason)) {
    result.error = L"提交历史输出的最后一条记录不完整：" + recordReason;
    return result;
  }
  const std::vector<std::wstring_view> tokens = SplitNulTokens(nulSeparatedOutput);
  if (limit > 0 && tokens.size() >= limit * kCommitFieldCount) {
    result.truncated = true;
  }
  if (!tokens.empty() && tokens.size() % kCommitFieldCount != 0) {
    // 分组对不上就是约定被破坏（截断、串流、Git 输出形态变化）：整体作废，见头文件说明。
    result.error = L"提交历史输出的字段数不是 " + std::to_wstring(kCommitFieldCount) +
                   L" 的整数倍：" + RecordSample(tokens.front());
    return result;
  }

  for (size_t base = 0; base + kCommitFieldCount <= tokens.size(); base += kCommitFieldCount) {
    const std::wstring_view objectIdField = tokens[base];
    const std::wstring_view authorField = tokens[base + 1];
    const std::wstring_view epochField = tokens[base + 2];
    const std::wstring_view summaryField = tokens[base + 3];
    const std::wstring_view parentsField = tokens[base + 4];

    CommitItem item;
    if (!LooksLikeFullObjectId(objectIdField)) {
      result.error = L"提交记录的对象 ID 不合约定：" + RecordSample(objectIdField);
      result.commits.clear();
      return result;
    }
    long long epoch = 0;
    if (!ParseCommitEpochSeconds(epochField, &epoch)) {
      result.error = L"提交记录的时间字段不合约定：" + RecordSample(objectIdField);
      result.commits.clear();
      return result;
    }
    // 作者为空、标题为空都是 Git 允许的真实提交（--allow-empty-message 等），不算解析错误。
    item.objectId = std::wstring(objectIdField);
    item.author = std::wstring(authorField);
    item.summary = std::wstring(summaryField);
    item.authorEpochSeconds = epoch;
    item.authoredAt = formatTime ? formatTime(epoch) : std::wstring();
    item.isMergeCommit = CountParents(parentsField) >= 2;
    result.commits.push_back(std::move(item));
  }
  return result;
}

CommitShowPlan BuildCommitShowPlan(const CommitItem& item) {
  CommitShowPlan plan;
  plan.operationId = L"commit-show";
  if (!LooksLikeFullObjectId(item.objectId)) {
    plan.refusalReason =
        L"这条提交记录的对象 ID 不符合约定（只允许小写十六进制的完整 ID），"
        L"为避免向 git show 交出不可信的参数，本次没有打开命令窗口，也没有对仓库做任何改动。\n"
        L"请点“刷新”重新读取提交历史。";
    return plan;
  }

  plan.allowed = true;
  plan.displayName = L"查看提交 " + ShortObjectId(item.objectId);
  // --format=fuller：作者与提交者、两个时间与两个时区偏移全部列出；
  // 差异部分与「查看差异」同一套规则：不调用外部 diff 与 textconv，子模块只给提交指针。
  // 对象 ID 放在最后、不套 `--`（實測：`git show -- <id>` 会把 ID 当成路径限定，输出变成空）。
  // 这样做安全的前提是 ID 已通过 LooksLikeFullObjectId：纯小写十六进制、以数字或字母开头，
  // 不可能被解释成选项，也不含任何 shell/通配字符。
  plan.arguments = {L"--no-optional-locks",
                    L"--no-replace-objects",
                    L"-c",
                    L"core.quotepath=off",
                    L"-c",
                    L"log.showSignature=false",
                    L"show",
                    L"--format=fuller",
                    L"--no-ext-diff",
                    L"--no-textconv",
                    L"--ignore-submodules=none",
                    L"--submodule=short",
                    item.objectId};
  if (item.isMergeCommit) {
    plan.notice = L"这是一条合并提交：git show 的组合差异（diff --cc）只列出与所有父提交都不同的内容，"
                  L"两侧一致地带入的文件不会出现在差异里，这是正常的，不是漏读。";
  }
  return plan;
}

std::wstring BuildRecentCommitsNotice(size_t count, bool truncated) {
  if (count == 0) {
    return {};
  }
  std::wstring text = L"提交历史 " + std::to_wstring(count) + L" 条";
  if (truncated) {
    text += L"（已达上限 " + std::to_wstring(kRecentCommitLimit) +
            L" 条，更早的历史未读取）";
  }
  return text;
}

}  // namespace gc::git
