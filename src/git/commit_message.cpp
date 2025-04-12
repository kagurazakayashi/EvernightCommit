#include "git/commit_message.h"

#include <algorithm>

namespace gc::git {
namespace {

// Git 的 trailer 鍵比較是不區分大小寫的（實測 `git interpret-trailers` 與
// `git log --format=%(trailers)` 都把 Co-authored-by 與 co-authored-by 視為同一鍵）。
constexpr std::wstring_view kCoauthorTrailerKey = L"co-authored-by";
constexpr wchar_t kLineFeed = L'\n';

bool IsBlankCharacter(wchar_t value) noexcept {
  return value == L' ' || value == L'\t' || value == 0x00A0 || value == 0x3000;
}

bool IsBlankLine(std::wstring_view line) {
  return std::all_of(line.begin(), line.end(), IsBlankCharacter);
}

// CR、CRLF 一律變成 LF：多行編輯控件（ES_MULTILINE）插入的就是 CRLF，
// 而提交訊息裡混著 CR 會讓 Git 與協作者的編輯器都多出一個看不見的字元。
std::wstring NormalizeLineEndings(std::wstring_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    const wchar_t value = text[index];
    if (value == L'\r') {
      if (index + 1 < text.size() && text[index + 1] == kLineFeed) {
        ++index;
      }
      result.push_back(kLineFeed);
      continue;
    }
    result.push_back(value);
  }
  return result;
}

std::vector<std::wstring> SplitToLines(std::wstring_view text) {
  std::vector<std::wstring> lines;
  if (text.empty()) {
    return lines;
  }
  size_t cursor = 0;
  for (;;) {
    const size_t breakAt = text.find(kLineFeed, cursor);
    lines.push_back(std::wstring(text.substr(cursor, breakAt == std::wstring_view::npos
                                                            ? std::wstring_view::npos
                                                            : breakAt - cursor)));
    if (breakAt == std::wstring_view::npos) {
      break;
    }
    cursor = breakAt + 1;
  }
  return lines;
}

void DropBlankLinesFrontBack(std::vector<std::wstring>& lines) {
  while (!lines.empty() && IsBlankLine(lines.front())) {
    lines.erase(lines.begin());
  }
  while (!lines.empty() && IsBlankLine(lines.back())) {
    lines.pop_back();
  }
}

// 續行：以空白開頭，屬於上一條 trailer 的值（Git 允許一個 trailer 跨多行）。
bool IsContinuationLine(std::wstring_view line) {
  return !line.empty() && IsBlankCharacter(line.front());
}

// 「鍵: 值」：鍵到第一個冒號為止，非空且不含空白。
// 全形冒號「：」不是分隔符，因此中文散文行不會被誤判成 trailer。
bool IsTrailerLine(std::wstring_view line) {
  if (line.empty() || IsContinuationLine(line)) {
    return false;
  }
  const size_t colon = line.find(L':');
  if (colon == std::wstring_view::npos || colon == 0) {
    return false;
  }
  return std::none_of(line.begin(), line.begin() + static_cast<ptrdiff_t>(colon), IsBlankCharacter);
}

std::wstring TrailerKeyLower(std::wstring_view line) {
  const size_t colon = line.find(L':');
  if (colon == std::wstring_view::npos) {
    return {};
  }
  std::wstring key(line.substr(0, colon));
  std::transform(key.begin(), key.end(), key.begin(), [](wchar_t value) {
    return (value >= L'A' && value <= L'Z') ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
  });
  return key;
}

std::wstring TrailerValue(std::wstring_view line) {
  const size_t colon = line.find(L':');
  if (colon == std::wstring_view::npos) {
    return {};
  }
  size_t begin = colon + 1;
  while (begin < line.size() && IsBlankCharacter(line[begin])) {
    ++begin;
  }
  return std::wstring(line.substr(begin));
}

// trailer 段的判定的唯一入口：整段都得是「鍵: 值」或續行，且最後一行不能是續行。
bool LooksLikeTrailerParagraph(const std::vector<std::wstring>& lines, size_t begin, size_t end) {
  if (begin >= end) {
    return false;
  }
  if (IsContinuationLine(lines[end - 1])) {
    return false;
  }
  bool anyTrailer = false;
  for (size_t index = begin; index < end; ++index) {
    if (IsTrailerLine(lines[index])) {
      anyTrailer = true;
      continue;
    }
    if (!IsContinuationLine(lines[index])) {
      return false;
    }
  }
  return anyTrailer;
}

// 一條 trailer：原文行（可能是多行）+ 可比對的鍵與值。
struct TrailerEntry {
  std::vector<std::wstring> lines;
  std::wstring keyLower;
  std::wstring value;
};

std::vector<TrailerEntry> GroupTrailerEntries(const std::vector<std::wstring>& lines, size_t begin,
                                             size_t end) {
  std::vector<TrailerEntry> entries;
  for (size_t index = begin; index < end; ++index) {
    const std::wstring& line = lines[index];
    if (entries.empty() || !IsContinuationLine(line)) {
      TrailerEntry entry;
      entry.keyLower = TrailerKeyLower(line);
      entry.value = TrailerValue(line);
      entry.lines.push_back(line);
      entries.push_back(std::move(entry));
      continue;
    }
    entries.back().lines.push_back(line);  // 續行原樣跟在它所屬的條目後面。
  }
  return entries;
}

std::wstring CoauthorTrailer(const GitIdentity& identity) {
  return std::wstring(L"Co-authored-by: ") + identity.Format();
}

}  // namespace

std::wstring CommitFormValidity::StatusText() const {
  if (!Ok()) {
    std::wstring text = L"提交信息校验：";
    constexpr size_t kShownIssues = 3;
    for (size_t index = 0; index < issues.size() && index < kShownIssues; ++index) {
      if (index != 0) {
        text += L" ";
      }
      text += issues[index].message;
    }
    if (issues.size() > kShownIssues) {
      text += L"（另有 " + std::to_wstring(issues.size() - kShownIssues) + L" 处问题）";
    }
    return text;
  }
  std::wstring text = L"提交信息校验通过：标题 " + std::to_wstring(subjectCharacters) + L" 字";
  text += descriptionLines == 0 ? L"，描述为空" : L"，描述 " + std::to_wstring(descriptionLines) + L" 行";
  text += coauthorCount == 0 ? L"，没有合作者" : L"，合作者 " + std::to_wstring(coauthorCount) + L" 位";
  text += L"。";
  if (committer == CommitterIdentityState::unknown) {
    text += L"提交者身份取自有效 Git 配置，尚未读到结果，创建提交前会再确认一次。";
  } else if (committer == CommitterIdentityState::available) {
    text += L"提交者身份可用（来自这个仓库的有效 Git 配置）。";
  }
  return text;
}

CommitFormValidity ValidateCommitForm(const CommitFormData& data, CommitterIdentityState committer) {
  CommitFormValidity validity;
  validity.committer = committer;

  const std::wstring subject = TrimWide(NormalizeLineEndings(data.subject));
  if (subject.empty()) {
    validity.issues.push_back(
        CommitIssue{CommitIssueField::subject, 0, L"标题不能为空，也不全是空白。"});
  } else if (subject.find(kLineFeed) != std::wstring::npos) {
    validity.issues.push_back(
        CommitIssue{CommitIssueField::subject, 0, L"标题必须是单行文字，不能包含换行。"});
  } else {
    bool hasControl = false;
    for (const wchar_t value : subject) {
      if (value < 0x20 || value == 0x7F) {
        hasControl = true;
        break;
      }
    }
    if (hasControl) {
      validity.issues.push_back(
          CommitIssue{CommitIssueField::subject, 0, L"标题里有控制字符，请改成普通文字。"});
    }
  }
  validity.subjectCharacters = subject.size();

  const std::wstring authorError = ValidateGitIdentity(data.author, L"作者");
  if (!authorError.empty()) {
    validity.issues.push_back(CommitIssue{CommitIssueField::author, 0, authorError});
  }

  for (size_t index = 0; index < data.coauthors.size(); ++index) {
    const std::wstring reason = ValidateGitIdentity(data.coauthors[index],
                                                    L"第 " + std::to_wstring(index + 1) + L" 条合作者");
    if (reason.empty()) {
      continue;
    }
    validity.issues.push_back(CommitIssue{CommitIssueField::coauthor, index, reason});
  }
  validity.coauthorCount = data.coauthors.size();

  const std::vector<std::wstring> descriptionLines =
      [&data] {
        std::vector<std::wstring> lines = SplitToLines(NormalizeLineEndings(data.description));
        DropBlankLinesFrontBack(lines);
        return lines;
      }();
  validity.descriptionLines = descriptionLines.size();

  if (committer == CommitterIdentityState::missing) {
    validity.issues.push_back(CommitIssue{
        CommitIssueField::committer, 0,
        L"提交者身份不可用：这个仓库的有效 Git 配置里 user.name 或 user.email 缺失，Git 无法确定提交者。"
        L"本程序不会替你改动配置，请先在 Git 里设好。"});
  }
  return validity;
}

ComposedCommitMessage ComposeCommitMessage(const CommitFormData& data) {
  ComposedCommitMessage composed;

  // 1) 表單內部的重複合作者先收攏：同一位（鍵相同）只保留第一次出現的寫法。
  std::vector<std::wstring> removed;
  const std::vector<std::wstring> coauthors = DeduplicateIdentities(data.coauthors, &removed);
  std::vector<GitIdentity> identities;
  identities.reserve(coauthors.size());
  for (const std::wstring& entry : coauthors) {
    GitIdentity identity;
    std::wstring error;
    if (!ParseGitIdentity(entry, &identity, &error, L"合作者")) {
      composed.rejectedCoauthors.push_back(entry);
      continue;
    }
    identities.push_back(std::move(identity));
  }
  std::vector<bool> consumed(identities.size(), false);

  // 2) 描述：統一行尾、去掉首尾空行，再決定最後一段是不是 trailer 段。
  std::vector<std::wstring> lines = SplitToLines(NormalizeLineEndings(data.description));
  DropBlankLinesFrontBack(lines);

  size_t trailerBegin = lines.size();
  if (!lines.empty()) {
    size_t index = lines.size();
    while (index > 0 && !IsBlankLine(lines[index - 1])) {
      --index;
    }
    if (LooksLikeTrailerParagraph(lines, index, lines.size())) {
      trailerBegin = index;
    }
  }

  std::vector<std::wstring> body(lines.begin(), lines.begin() + static_cast<ptrdiff_t>(trailerBegin));
  DropBlankLinesFrontBack(body);  // trailer 段被摘掉之後，它前面殘留的空行不該變成尾部空行。
  composed.bodyLines = body.size();

  // 3) 已有的 trailer 條目：判定為重複的就地換成表單寫法，其餘一字不動、順序不變。
  std::vector<std::wstring> trailers;
  for (TrailerEntry& entry : GroupTrailerEntries(lines, trailerBegin, lines.size())) {
    if (entry.keyLower != kCoauthorTrailerKey) {
      trailers.insert(trailers.end(), entry.lines.begin(), entry.lines.end());
      continue;
    }
    const std::wstring key = CanonicalIdentityKey(entry.value);
    size_t match = identities.size();
    for (size_t index = 0; index < identities.size(); ++index) {
      if (!consumed[index] && CanonicalIdentityKey(identities[index].Format()) == key) {
        match = index;
        break;
      }
    }
    if (match == identities.size()) {
      // 表單裡沒有這一條：它可能是別人手寫在描述裡的，保留才是忠實。
      trailers.insert(trailers.end(), entry.lines.begin(), entry.lines.end());
      continue;
    }
    consumed[match] = true;
    ++composed.reusedTrailers;
    trailers.push_back(CoauthorTrailer(identities[match]));
  }

  // 4) 沒有被沿用的合作者依表單順序追加。
  for (size_t index = 0; index < identities.size(); ++index) {
    if (consumed[index]) {
      continue;
    }
    trailers.push_back(CoauthorTrailer(identities[index]));
    ++composed.appendedTrailers;
  }
  composed.coauthorTrailers = composed.reusedTrailers + composed.appendedTrailers;

  // 5) 装配：標題、空行、正文、空行、trailer 段。末尾不加換行，寫檔時由後續步驟補。
  std::wstring message = TrimWide(NormalizeLineEndings(data.subject));
  if (!body.empty()) {
    message += kLineFeed;
    message += kLineFeed;
    for (size_t index = 0; index < body.size(); ++index) {
      if (index != 0) {
        message += kLineFeed;
      }
      message += body[index];
    }
  }
  if (!trailers.empty()) {
    message += kLineFeed;
    message += kLineFeed;
    for (const std::wstring& line : trailers) {
      message += line;
      message += kLineFeed;
    }
    message.pop_back();  // 末行不帶換行。
  }

  composed.message = std::move(message);
  return composed;
}

}  // namespace gc::git
