#include "git/commit_detail.h"

#include <algorithm>

#include "git/repository.h"  // NulRecordsAreComplete：记录边界约定与「最近提交」共用同一处判定

namespace gc::git {
namespace {

// 与 commit_history / commit_changes 同一套切分：记录以 NUL 结束，Git 每条记录的结尾 NUL
// 会多出一个空片段，它不是一段字段。
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

// 片段的可读摘要：只用于说明「哪一段读不懂」，取前 40 个字符。
[[nodiscard]] std::wstring RecordSample(std::wstring_view record) {
  const size_t limit = std::min<size_t>(record.size(), 40);
  return std::wstring(record.substr(0, limit));
}

// Git 记下的消息正文总是以一个 LF 收尾（实测），展示时那一句不是「描述里空了一行」。
void TrimTrailingNewlines(std::wstring* text) {
  while (!text->empty() && (text->back() == L'\n' || text->back() == L'\r')) {
    text->pop_back();
  }
}

}  // namespace

std::vector<std::wstring> BuildCommitDetailArguments(std::wstring_view repositoryDirectory,
                                                     std::wstring_view objectId) {
  const std::wstring format = L"%H%x00%an%x00%ae%x00%at%x00%ct%x00%s%x00%b";
  return std::vector<std::wstring>{
      L"-C",
      std::wstring(repositoryDirectory),
      L"--no-optional-locks",
      L"--no-replace-objects",
      L"-c",
      L"log.showSignature=false",
      L"log",
      // --no-decorate 是 log 的选项，必须排在子命令之后（与「最近提交」那条读取同一处实测结论）。
      // 本格式串没用到 %d，装饰文本进不了字段，但两条读取的形态保持一致，将来改格式不用担心其中一条。
      L"--no-decorate",
      L"--format=" + format,
      L"-z",
      L"-n",
      L"1",
      std::wstring(objectId)};
}

CommitDetailParseResult ParseCommitDetail(std::wstring_view nulSeparatedOutput,
                                          std::wstring_view requestedObjectId,
                                          const CommitTimeFormatter& formatTime) {
  CommitDetailParseResult result;
  const auto refuse = [&](std::wstring reason) {
    result.error = std::move(reason);
    result.detail = CommitDetail{};
    return result;
  };

  if (!LooksLikeFullObjectId(requestedObjectId)) {
    return refuse(L"发起读取时那条提交的对象 ID 不符合约定：" + RecordSample(requestedObjectId));
  }

  // 与「最近提交」同一条记录边界约定：缺结尾 NUL 的记录是残缺的（半个描述），整份作废。
  std::wstring recordReason;
  if (!NulRecordsAreComplete(nulSeparatedOutput, recordReason)) {
    return refuse(L"提交详情输出的记录不完整：" + recordReason);
  }
  const std::vector<std::wstring_view> tokens = SplitNulTokens(nulSeparatedOutput);
  if (tokens.empty()) {
    // Git 确实跑完了却没有输出：这条对象读不出记录。不能显示成「描述为空」的那种空。
    return refuse(L"Git 没有答出这条提交的任何字段（输出为空）。");
  }
  // `-n 1` 只该答一条：多出来的段说明切分约定已经不对上了（版本行为变化、串流或截断）。
  if (tokens.size() != kCommitDetailFieldCount) {
    return refuse(L"提交详情输出的字段数不是 " + std::to_wstring(kCommitDetailFieldCount) +
                  L" 段：" + std::to_wstring(tokens.size()) + L" 段，首段「" + RecordSample(tokens.front()) +
                  L"」");
  }

  CommitDetail detail;
  if (!LooksLikeFullObjectId(tokens[0])) {
    return refuse(L"提交详情的对象 ID 不合约定：" + RecordSample(tokens[0]));
  }
  // 答的就是问的那一条吗：这一步把「面板上的文本」与「列表里被选中的那行」钉在同一枚对象上。
  // 行在读取期间被刷新换成别的提交时，这里就不一致，整份作废由界面重新对一次，绝不显示上一条。
  if (tokens[0] != requestedObjectId) {
    return refuse(L"Git 答出的对象 ID 与本次读取的那一条不是同一枚提交。");
  }
  long long authorEpoch = 0;
  if (!ParseCommitEpochSeconds(tokens[3], &authorEpoch)) {
    return refuse(L"提交详情的作者时间字段不合约定：" + RecordSample(tokens[3]));
  }
  long long committerEpoch = 0;
  if (!ParseCommitEpochSeconds(tokens[4], &committerEpoch)) {
    return refuse(L"提交详情的提交者时间字段不合约定：" + RecordSample(tokens[4]));
  }

  detail.objectId = std::wstring(tokens[0]);
  detail.authorName = std::wstring(tokens[1]);
  detail.authorEmail = std::wstring(tokens[2]);
  detail.authoredAt = formatTime ? formatTime(authorEpoch) : std::wstring();
  detail.committedAt = formatTime ? formatTime(committerEpoch) : std::wstring();
  detail.summary = std::wstring(tokens[5]);
  // 作者为空、标题为空、没有描述都是 Git 允许的真实提交（--allow-empty-message 等），不算解析错误。
  detail.description = std::wstring(tokens[6]);
  TrimTrailingNewlines(&detail.description);
  if (detail.description.size() > kCommitDetailDescriptionLimit) {
    detail.description.resize(kCommitDetailDescriptionLimit);
    // 截点正好落在代理对中间会留下一个孤立的半字符：编辑框与剪贴板都会拿到非法的 UTF-16。
    if (!detail.description.empty()) {
      const wchar_t last = detail.description.back();
      if (last >= 0xD800 && last <= 0xDBFF) {
        detail.description.pop_back();
      }
    }
    detail.descriptionTruncated = true;
  }

  result.ok = true;
  result.detail = std::move(detail);
  return result;
}

std::wstring NormalizeTextForEdit(std::wstring_view text) {
  std::wstring out;
  out.reserve(text.size() + 16);
  for (size_t index = 0; index < text.size(); ++index) {
    const wchar_t c = text[index];
    if (c == L'\r') {
      out.append(L"\r\n");
      if (index + 1 < text.size() && text[index + 1] == L'\n') {
        ++index;  // 本来就是 CRLF：只留一份行结束，不写成两个
      }
    } else if (c == L'\n') {
      out.append(L"\r\n");
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::wstring FormatCommitDetailText(const CommitDetail& detail) {
  std::wstring text;
  text.reserve(detail.description.size() + 256);
  // 先按 LF 拼，最后一次统一成 CRLF：描述正文里的换行形态由 Git 决定，不在这里逐段判断。
  const auto appendLine = [&text](std::wstring_view value) {
    text.append(value);
    text.push_back(L'\n');
  };

  appendLine(L"提交哈希：" + detail.objectId);
  appendLine(L"标题：" + (detail.summary.empty() ? std::wstring(L"（这条提交没有标题）") : detail.summary));

  std::wstring author;
  if (!detail.authorName.empty() && !detail.authorEmail.empty()) {
    author = detail.authorName + L" <" + detail.authorEmail + L">";
  } else if (!detail.authorName.empty()) {
    author = detail.authorName;
  } else if (!detail.authorEmail.empty()) {
    author = L"<" + detail.authorEmail + L">";
  } else {
    author = L"（没有作者信息）";
  }
  appendLine(L"作者：" + author);
  appendLine(L"作者时间：" +
             (detail.authoredAt.empty() ? std::wstring(L"（未能换算成本机时间）") : detail.authoredAt));
  appendLine(L"提交者时间：" +
             (detail.committedAt.empty() ? std::wstring(L"（未能换算成本机时间）") : detail.committedAt));

  if (detail.description.empty()) {
    appendLine(L"描述：（无描述）");
  } else {
    appendLine(L"描述：");
    appendLine(detail.description);
  }
  if (detail.descriptionTruncated) {
    appendLine(L"（描述超过 " + std::to_wstring(kCommitDetailDescriptionLimit) +
               L" 字，以上只是开头那一段；完整描述请双击这条提交，在命令窗口里用 git show 查看。）");
  }
  text = NormalizeTextForEdit(text);
  // 末尾那句多余的换行去掉：显示时下面不留空行，复制出去也不带一个空行。
  if (text.size() >= 2 && text.compare(text.size() - 2, 2, L"\r\n") == 0) {
    text.erase(text.size() - 2);
  }
  return text;
}

}  // namespace gc::git
