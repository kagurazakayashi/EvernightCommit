#include "app/persistent_state.h"

#include <algorithm>
#include <utility>

#include "git/repository.h"

namespace gc::app {
namespace {

// 参与序列化判定的记录名。v1 内出现名单外的记录名即整份拒绝：
// 「不认识的那一行」意味着这份文件已经不是本程序写的那份，半读半丢最危险。
constexpr std::wstring_view kMagicName = L"evernightcommit.prefs";
constexpr std::wstring_view kPolicyName = L"persistence";
constexpr std::wstring_view kRevisionName = L"revision";
constexpr std::wstring_view kGitName = L"git";
constexpr std::wstring_view kRecentName = L"recent";
constexpr std::wstring_view kWindowName = L"window";
constexpr std::wstring_view kColumnsName = L"columns";
constexpr std::wstring_view kDraftName = L"draft";

bool IsKnownRecordName(std::wstring_view name) {
  return name == kPolicyName || name == kRevisionName || name == kGitName || name == kRecentName ||
         name == kWindowName || name == kColumnsName || name == kDraftName;
}

// ---- 字段转义 ----
// 反斜杠必须最先转义，否则会把后面补出来的转义序列再转一遍。
std::wstring EscapeField(std::wstring_view value) {
  std::wstring out;
  out.reserve(value.size() + 8);
  for (const wchar_t c : value) {
    switch (c) {
      case L'\\':
        out += L"\\\\";
        break;
      case L'|':
        out += L"\\v";
        break;
      case L'\n':
        out += L"\\n";
        break;
      case L'\r':
        out += L"\\r";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

// 严格反转义：'\' 只允许后跟 \vn\r 四种，未知转义序列、行尾半个 '\'、NUL 一律失败。
bool UnescapeField(std::wstring_view value, std::wstring* out) {
  std::wstring decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const wchar_t c = value[i];
    if (c == L'\0') {
      return false;
    }
    if (c != L'\\') {
      decoded.push_back(c);
      continue;
    }
    if (i + 1 >= value.size()) {
      return false;
    }
    const wchar_t next = value[++i];
    switch (next) {
      case L'\\':
        decoded.push_back(L'\\');
        break;
      case L'v':
        decoded.push_back(L'|');
        break;
      case L'n':
        decoded.push_back(L'\n');
        break;
      case L'r':
        decoded.push_back(L'\r');
        break;
      default:
        return false;
    }
  }
  *out = std::move(decoded);
  return true;
}

// ---- 拆分与整数 ----

std::vector<std::wstring_view> SplitPipes(std::wstring_view line) {
  std::vector<std::wstring_view> parts;
  size_t start = 0;
  for (;;) {
    const size_t bar = line.find(L'|', start);
    if (bar == std::wstring_view::npos) {
      parts.push_back(line.substr(start));
      break;
    }
    parts.push_back(line.substr(start, bar - start));
    start = bar + 1;
  }
  return parts;
}

// 严格非负十进制整数：只允许 '0'..'9'，最多 19 位（不猜上界溢出后的值）。
bool ParseNonNegative(std::wstring_view text, long long* out) {
  if (text.empty() || text.size() > 19) {
    return false;
  }
  long long value = 0;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') {
      return false;
    }
    value = value * 10 + static_cast<long long>(c - L'0');
  }
  *out = value;
  return true;
}

// 严格带符号十进制整数（窗口坐标可以为负）。
bool ParseSigned(std::wstring_view text, int* out) {
  if (text.empty() || text.size() > 11) {
    return false;
  }
  size_t index = 0;
  bool negative = false;
  if (text[0] == L'-') {
    negative = true;
    index = 1;
  }
  if (index >= text.size()) {
    return false;
  }
  long long value = 0;
  for (; index < text.size(); ++index) {
    const wchar_t c = text[index];
    if (c < L'0' || c > L'9') {
      return false;
    }
    value = value * 10 + static_cast<long long>(c - L'0');
    if (value > 4294967296LL) {  // 屏幕坐标不可能到这个量级；到了就是坏数据。
      return false;
    }
  }
  const long long signedValue = negative ? -value : value;
  if (signedValue < -2147483648LL || signedValue > 2147483647LL) {
    return false;
  }
  *out = static_cast<int>(signedValue);
  return true;
}

bool ParseFlag(std::wstring_view text, bool* out) {
  if (text == L"0") {
    *out = false;
    return true;
  }
  if (text == L"1") {
    *out = true;
    return true;
  }
  return false;
}

std::wstring FormatCivil(const git::CivilTime& value) {
  const auto num = [](int v) { return std::to_wstring(v); };
  return num(value.year) + L"," + num(value.month) + L"," + num(value.day) + L"," + num(value.hour) + L"," +
         num(value.minute) + L"," + num(value.second);
}

// 墙钟六个整数：`y,m,d,H,M,S`，逗号一个都不能少，取值由 git::CivilTimeLooksValid 裁定。
bool ParseCivilTime(std::wstring_view text, git::CivilTime* out) {
  int values[6] = {0, 0, 0, 0, 0, 0};
  std::wstring_view rest = text;
  for (int i = 0; i < 6; ++i) {
    const size_t comma = rest.find(L',');
    if (i < 5) {
      if (comma == std::wstring_view::npos) {
        return false;  // 六个字段之间的逗号一个都不能少。
      }
      if (!ParseSigned(rest.substr(0, comma), &values[i])) {
        return false;
      }
      rest = rest.substr(comma + 1);
    } else {
      // 最后一个字段后面不该再有逗号。
      if (comma != std::wstring_view::npos || !ParseSigned(rest, &values[5])) {
        return false;
      }
    }
  }
  out->year = values[0];
  out->month = values[1];
  out->day = values[2];
  out->hour = values[3];
  out->minute = values[4];
  out->second = values[5];
  return git::CivilTimeLooksValid(*out);
}

size_t DraftContentChars(const git::CommitFormData& form) {
  size_t total = form.subject.size() + form.description.size() + form.author.size();
  for (const std::wstring& entry : form.coauthors) {
    total += entry.size() + 1;
  }
  return total;
}

// 按「保存时刻」从旧到新排序，用于超限淘汰（同刻保留原有相对顺序）。
void SortDraftsByAge(std::vector<PersistentDraft>* drafts) {
  std::stable_sort(drafts->begin(), drafts->end(),
                   [](const PersistentDraft& left, const PersistentDraft& right) {
                     return left.savedAtEpoch < right.savedAtEpoch;
                   });
}

PersistentState DefaultState() {
  return PersistentState{};
}

// 记录行的公共骨架：名字 + '|' 连接的字段。
std::wstring RecordLine(std::wstring_view name, const std::vector<std::wstring>& fields) {
  std::wstring line(name);
  for (const std::wstring& field : fields) {
    line.push_back(L'|');
    line += field;
  }
  return line;
}

}  // namespace

bool PersistentDraftBodyIsEmpty(const PersistentDraft& draft) noexcept {
  return draft.form.subject.empty() && draft.form.description.empty() && draft.form.coauthors.empty();
}

bool PersistentDraftContentTooLarge(const git::CommitFormData& form) noexcept {
  return DraftContentChars(form) > kMaxDraftContentChars;
}

bool UpsertPersistentDraft(PersistentState* state, PersistentDraft draft, std::wstring* refusal) {
  if (state == nullptr || draft.repositoryKey.empty()) {
    return false;
  }
  // 正文为空的草稿是一次「删除」而不是「保存一张空表」：作者一栏不算正文。
  if (PersistentDraftBodyIsEmpty(draft)) {
    RemovePersistentDraft(state, draft.repositoryKey);
    return true;
  }
  if (PersistentDraftContentTooLarge(draft.form)) {
    if (refusal != nullptr) {
      *refusal = L"草稿没有保存：正文超过 " + std::to_wstring(kMaxDraftContentChars) +
                 L" 字上限（只报告长度这一项元数据，正文一个字都不会写进记录文件）。";
    }
    return false;
  }
  for (PersistentDraft& existing : state->drafts) {
    if (existing.repositoryKey == draft.repositoryKey) {
      existing = std::move(draft);
      return true;
    }
  }
  state->drafts.push_back(std::move(draft));
  if (state->drafts.size() > kMaxPersistedDrafts) {
    SortDraftsByAge(&state->drafts);
    state->drafts.erase(state->drafts.begin(),
                        state->drafts.begin() +
                            static_cast<long long>(state->drafts.size() - kMaxPersistedDrafts));
  }
  return true;
}

bool RecordPersistentDraftSnapshot(PersistentState* state, PersistentDraft draft, std::wstring* refusal) {
  if (state == nullptr || draft.repositoryKey.empty()) {
    return false;
  }
  if (!PersistentDraftBodyIsEmpty(draft) && PersistentDraftContentTooLarge(draft.form)) {
    if (refusal != nullptr) {
      *refusal = L"草稿没有保存：正文超过 " + std::to_wstring(kMaxDraftContentChars) +
                 L" 字上限（屏幕上与内存里已有的那份草稿都没有改动）。";
    }
    return false;
  }
  // 空正文照样记一条「带时刻的空草稿」：这是会跨实例传播的删除墓碑（见头文件）。
  for (PersistentDraft& existing : state->drafts) {
    if (existing.repositoryKey == draft.repositoryKey) {
      existing = std::move(draft);
      return true;
    }
  }
  state->drafts.push_back(std::move(draft));
  if (state->drafts.size() > kMaxPersistedDrafts) {
    SortDraftsByAge(&state->drafts);
    state->drafts.erase(state->drafts.begin(),
                        state->drafts.begin() +
                            static_cast<long long>(state->drafts.size() - kMaxPersistedDrafts));
  }
  return true;
}

bool RemovePersistentDraft(PersistentState* state, std::wstring_view repositoryKey) {
  if (state == nullptr || repositoryKey.empty()) {
    return false;
  }
  const auto found = std::find_if(state->drafts.begin(), state->drafts.end(),
                                  [&](const PersistentDraft& draft) {
                                    return draft.repositoryKey == repositoryKey;
                                  });
  if (found == state->drafts.end()) {
    return false;
  }
  state->drafts.erase(found);
  return true;
}

const PersistentDraft* FindPersistentDraft(const PersistentState& state, std::wstring_view repositoryKey) {
  if (repositoryKey.empty()) {
    return nullptr;
  }
  const auto found = std::find_if(state.drafts.begin(), state.drafts.end(),
                                  [&](const PersistentDraft& draft) {
                                    return draft.repositoryKey == repositoryKey;
                                  });
  return found == state.drafts.end() ? nullptr : &*found;
}

void TouchPersistentRecent(PersistentState* state, std::wstring repositoryRoot) {
  if (state == nullptr || repositoryRoot.empty()) {
    return;
  }
  const std::wstring key = git::CanonicalPathKey(repositoryRoot);
  // 同一 worktree 的旧条目整条换掉（显示写法以这次的为准），再把新的放最前。
  std::erase_if(state->recentRepositories, [&](const std::wstring& entry) {
    return git::CanonicalPathKey(entry) == key;
  });
  state->recentRepositories.insert(state->recentRepositories.begin(), std::move(repositoryRoot));
  if (state->recentRepositories.size() > kMaxRecentRepositories) {
    state->recentRepositories.resize(kMaxRecentRepositories);
  }
}

void ClearPersistedUserData(PersistentState* state) {
  if (state == nullptr) {
    return;
  }
  // 策略三态与版本号是「这台机器上的用户做过什么选择」，不是「用户的内容」；
  // 其余（仓库、Git 路径、布局、草稿）全部是内容记录，一律清掉。
  state->gitExecutablePath.clear();
  state->recentRepositories.clear();
  state->window = PersistentWindowGeometry{};
  state->columns = PersistentColumns{};
  state->drafts.clear();
}

std::wstring SerializePersistentState(const PersistentState& state) {
  std::wstring out;
  out += RecordLine(kMagicName, {std::to_wstring(state.formatVersion)});
  out.push_back(L'\n');
  out += RecordLine(kPolicyName,
                    {std::to_wstring(state.consentRecorded ? 1 : 0) + L"," +
                     std::to_wstring(state.persistenceEnabled ? 1 : 0) + L"," +
                     std::to_wstring(state.draftSavingEnabled ? 1 : 0)});
  out.push_back(L'\n');
  out += RecordLine(kRevisionName, {std::to_wstring(state.revision < 0 ? 0 : state.revision)});
  out.push_back(L'\n');
  if (!state.gitExecutablePath.empty()) {
    out += RecordLine(kGitName, {EscapeField(state.gitExecutablePath)});
    out.push_back(L'\n');
  }
  for (const std::wstring& recent : state.recentRepositories) {
    out += RecordLine(kRecentName, {EscapeField(recent)});
    out.push_back(L'\n');
  }
  if (state.window.valid) {
    out += RecordLine(kWindowName,
                      {std::to_wstring(state.window.x) + L"," + std::to_wstring(state.window.y) + L"," +
                       std::to_wstring(state.window.width) + L"," + std::to_wstring(state.window.height) +
                       L"," + std::to_wstring(state.window.maximized ? 1 : 0)});
    out.push_back(L'\n');
  }
  if (state.columns.valid) {
    out += RecordLine(kColumnsName,
                      {std::to_wstring(state.columns.leftPermille) + L"," +
                       std::to_wstring(state.columns.middlePermille)});
    out.push_back(L'\n');
  }
  for (const PersistentDraft& draft : state.drafts) {
    if (PersistentDraftBodyIsEmpty(draft)) {
      continue;  // 空正文不落盘（合并的删除传播发生在内存里）。
    }
    std::vector<std::wstring> fields;
    fields.push_back(EscapeField(draft.repositoryRoot));
    fields.push_back(EscapeField(draft.repositoryKey));
    fields.push_back(std::to_wstring(draft.savedAtEpoch < 0 ? 0 : draft.savedAtEpoch));
    fields.push_back(std::to_wstring(draft.timesSynced ? 1 : 0) + L"," +
                     std::to_wstring(draft.timesUserEdited ? 1 : 0));
    fields.push_back(FormatCivil(draft.authorWall));
    fields.push_back(FormatCivil(draft.committerWall));
    fields.push_back(EscapeField(draft.form.subject));
    fields.push_back(EscapeField(draft.form.description));
    fields.push_back(EscapeField(draft.form.author));
    fields.push_back(std::to_wstring(draft.form.coauthors.size()));
    for (const std::wstring& entry : draft.form.coauthors) {
      fields.push_back(EscapeField(entry));
    }
    out += RecordLine(kDraftName, fields);
    out.push_back(L'\n');
  }
  return out;
}

PersistentLoadResult ParsePersistentState(std::wstring_view text) {
  PersistentLoadResult result;
  // 全空白：当作「没有内容可读」，不是损坏。
  if (std::all_of(text.begin(), text.end(),
                  [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; })) {
    result.status = PersistentLoadStatus::empty;
    result.state = DefaultState();
    return result;
  }
  // 行尾回车：本程序写出的文件只用 '\n'，字段里的 '\r' 是转义过的。
  // 出现裸 '\r' 说明这份内容不是本程序写的（或被改坏过）——整份按损坏处理。
  if (text.find(L'\r') != std::wstring_view::npos) {
    result.status = PersistentLoadStatus::corrupt;
    result.reason = L"记录里出现裸的回车符，与写入约定不符";
    return result;
  }
  if (text.find(L'\0') != std::wstring_view::npos) {
    result.status = PersistentLoadStatus::corrupt;
    result.reason = L"记录里出现 NUL";
    return result;
  }

  std::vector<std::wstring_view> lines;
  {
    size_t start = 0;
    for (;;) {
      const size_t newline = text.find(L'\n', start);
      lines.push_back(text.substr(start, newline == std::wstring_view::npos ? std::wstring_view::npos
                                                                            : newline - start));
      if (newline == std::wstring_view::npos) {
        break;
      }
      start = newline + 1;
    }
    // 序列化总在最后一条记录后补一个 '\n'，因此拆出来的末行是空串；只允许末尾这一个空行。
    if (!lines.empty() && lines.back().empty()) {
      lines.pop_back();
    }
  }
  if (lines.empty()) {
    result.status = PersistentLoadStatus::empty;
    result.state = DefaultState();
    return result;
  }

  PersistentState state = DefaultState();
  bool hasRevision = false;
  bool hasPolicy = false;
  bool hasGit = false;

  size_t firstRecord = 0;
  int version = -1;
  {
    // 首行是版本头吗：`evernightcommit.prefs|<n>`。不是就按未分版本的老文件（v0）迁移。
    const std::vector<std::wstring_view> head = SplitPipes(lines[0]);
    if (head.size() == 2 && head[0] == std::wstring_view(kMagicName)) {
      long long parsedVersion = 0;
      if (!ParseNonNegative(head[1], &parsedVersion) || parsedVersion > 4294967296LL) {
        result.status = PersistentLoadStatus::corrupt;
        result.reason = L"版本头的版本号不成立";
        return result;
      }
      version = static_cast<int>(parsedVersion);
      firstRecord = 1;
    }
  }
  if (version < 0) {
    result.detectedVersion = 0;
  } else {
    result.detectedVersion = version;
  }
  if (version > kPersistentFormatVersion) {
    // 比本程序新的版本：字段含义没有依据，不应用也不许覆盖。
    result.status = PersistentLoadStatus::tooNew;
    result.reason = L"文件由更新版本的程序写出（版本 " + std::to_wstring(version) + L"）";
    return result;
  }
  state.formatVersion = kPersistentFormatVersion;

  for (size_t lineIndex = firstRecord; lineIndex < lines.size(); ++lineIndex) {
    const std::wstring_view line = lines[lineIndex];
    if (line.empty()) {
      result.status = PersistentLoadStatus::corrupt;
      result.reason = L"第 " + std::to_wstring(lineIndex + 1) + L" 行是空行，记录文件里没有这种东西";
      return result;
    }
    const std::vector<std::wstring_view> parts = SplitPipes(line);
    const std::wstring_view name = parts[0];
    if (!IsKnownRecordName(name)) {
      result.status = PersistentLoadStatus::corrupt;
      result.reason = L"第 " + std::to_wstring(lineIndex + 1) + L" 行是本程序不认识的记录";
      return result;
    }
    const auto corrupt = [&result, lineIndex](std::wstring_view why) {
      result.status = PersistentLoadStatus::corrupt;
      result.reason =
          L"第 " + std::to_wstring(lineIndex + 1) + L" 行不完整：" + std::wstring(why);
      return result;
    };
    std::wstring decoded;
    if (name == kPolicyName) {
      if (parts.size() != 2 || hasPolicy || !UnescapeField(parts[1], &decoded)) {
        return corrupt(L"策略记录形态不对");
      }
      // decoded 形如 "1,1,0"：asked,enabled,drafts。
      size_t cursor = 0;
      bool flags[3] = {false, false, false};
      for (int i = 0; i < 3; ++i) {
        const size_t comma = decoded.find(L',', cursor);
        const std::wstring_view piece =
            comma == std::wstring_view::npos ? std::wstring_view(decoded).substr(cursor)
                                             : std::wstring_view(decoded).substr(cursor, comma - cursor);
        if (!ParseFlag(piece, &flags[i])) {
          return corrupt(L"策略记录的三态取值不成立");
        }
        if (comma == std::wstring_view::npos) {
          if (i != 2) {
            return corrupt(L"策略记录缺字段");
          }
          break;
        }
        if (i == 2) {
          return corrupt(L"策略记录多出了认不出的字段");  // 恰好三个，尾随的逗号也算形态不成立。
        }
        cursor = comma + 1;
      }
      hasPolicy = true;
      state.consentRecorded = flags[0];
      state.persistenceEnabled = flags[1];
      state.draftSavingEnabled = flags[2];
      continue;
    }
    if (name == kRevisionName) {
      if (parts.size() != 2 || hasRevision || !UnescapeField(parts[1], &decoded)) {
        return corrupt(L"修订号形态不对");
      }
      long long revision = 0;
      if (!ParseNonNegative(decoded, &revision)) {
        return corrupt(L"修订号不是非负十进制整数");
      }
      hasRevision = true;
      state.revision = revision;
      continue;
    }
    if (name == kGitName) {
      if (parts.size() != 2 || hasGit || !UnescapeField(parts[1], &decoded) || decoded.empty()) {
        return corrupt(L"Git 路径记录形态不对");
      }
      hasGit = true;
      state.gitExecutablePath = std::move(decoded);
      continue;
    }
    if (name == kRecentName) {
      if (parts.size() != 2 || !UnescapeField(parts[1], &decoded) || decoded.empty()) {
        return corrupt(L"最近仓库记录形态不对");
      }
      const std::wstring key = git::CanonicalPathKey(decoded);
      for (const std::wstring& existing : state.recentRepositories) {
        if (git::CanonicalPathKey(existing) == key) {
          return corrupt(L"同一个仓库在最近列表里出现了两次");
        }
      }
      if (state.recentRepositories.size() >= kMaxRecentRepositories) {
        return corrupt(L"最近仓库条数超过上限");
      }
      state.recentRepositories.push_back(std::move(decoded));
      continue;
    }
    if (name == kWindowName) {
      if (parts.size() != 2 || state.window.valid) {
        return corrupt(L"窗口记录形态不对");
      }
      if (!UnescapeField(parts[1], &decoded)) {
        return corrupt(L"窗口记录转义不成立");
      }
      // "x,y,w,h,max"
      std::wstring_view rest = decoded;
      int values[4] = {0, 0, 0, 0};
      bool ok = true;
      for (int i = 0; i < 4; ++i) {
        const size_t comma = rest.find(L',');
        if (comma == std::wstring_view::npos) {
          ok = false;
          break;
        }
        ok = ok && ParseSigned(rest.substr(0, comma), &values[i]);
        rest = rest.substr(comma + 1);
      }
      bool maximized = false;
      ok = ok && ParseFlag(rest, &maximized);
      if (!ok || values[2] <= 0 || values[3] <= 0) {
        return corrupt(L"窗口记录的坐标或尺寸不成立");
      }
      state.window.valid = true;
      state.window.x = values[0];
      state.window.y = values[1];
      state.window.width = values[2];
      state.window.height = values[3];
      state.window.maximized = maximized;
      continue;
    }
    if (name == kColumnsName) {
      if (parts.size() != 2 || state.columns.valid || !UnescapeField(parts[1], &decoded)) {
        return corrupt(L"列宽记录形态不对");
      }
      std::wstring_view rest = decoded;
      const size_t comma = rest.find(L',');
      if (comma == std::wstring_view::npos) {
        return corrupt(L"列宽记录缺字段");
      }
      int left = 0;
      int middle = 0;
      if (!ParseSigned(rest.substr(0, comma), &left) || !ParseSigned(rest.substr(comma + 1), &middle)) {
        return corrupt(L"列宽记录不是整数");
      }
      if (left < 0 || middle < 0 || left > kPersistentColumnPermilleBase ||
          middle > kPersistentColumnPermilleBase ||
          left + middle >= kPersistentColumnPermilleBase) {
        return corrupt(L"列宽比例超出可用范围");
      }
      state.columns.valid = true;
      state.columns.leftPermille = left;
      state.columns.middlePermille = middle;
      continue;
    }
    // draft：固定 10 个前置字段 + 变长的合作者尾巴。
    if (parts.size() < 11) {
      return corrupt(L"草稿记录的字段不够");
    }
    PersistentDraft draft;
    if (!UnescapeField(parts[1], &draft.repositoryRoot) || draft.repositoryRoot.empty() ||
        !UnescapeField(parts[2], &draft.repositoryKey) || draft.repositoryKey.empty()) {
      return corrupt(L"草稿记录的仓库身份不成立");
    }
    if (draft.repositoryKey != git::CanonicalPathKey(draft.repositoryRoot)) {
      return corrupt(L"草稿记录的仓库键与根目录对不上");
    }
    if (FindPersistentDraft(state, draft.repositoryKey) != nullptr) {
      return corrupt(L"同一个仓库有两份草稿");
    }
    long long epoch = 0;
    if (!ParseNonNegative(parts[3], &epoch)) {
      return corrupt(L"草稿的保存时刻不是非负整数");
    }
    draft.savedAtEpoch = epoch;
    {
      // "sync,edited"
      std::wstring_view flags = parts[4];
      const size_t split = flags.find(L',');
      bool sync = false;
      bool edited = false;
      if (split == std::wstring_view::npos || !ParseFlag(flags.substr(0, split), &sync) ||
          !ParseFlag(flags.substr(split + 1), &edited)) {
        return corrupt(L"草稿的时间记号不成立");
      }
      draft.timesSynced = sync;
      draft.timesUserEdited = edited;
    }
    if (!ParseCivilTime(parts[5], &draft.authorWall) || !ParseCivilTime(parts[6], &draft.committerWall)) {
      return corrupt(L"草稿的墙钟时间不成立");
    }
    if (!UnescapeField(parts[7], &draft.form.subject) || !UnescapeField(parts[8], &draft.form.description) ||
        !UnescapeField(parts[9], &draft.form.author)) {
      return corrupt(L"草稿正文的转义不成立");
    }
    long long coauthorCount = 0;
    if (!ParseNonNegative(parts[10], &coauthorCount) ||
        coauthorCount != static_cast<long long>(parts.size() - 11) ||
        coauthorCount > static_cast<long long>(kMaxDraftContentChars)) {
      return corrupt(L"草稿的合作者条数与尾巴字段对不上");
    }
    for (long long i = 0; i < coauthorCount; ++i) {
      std::wstring entry;
      if (!UnescapeField(parts[static_cast<size_t>(11 + i)], &entry) || entry.empty()) {
        return corrupt(L"草稿里有一条合作者不成立");
      }
      draft.form.coauthors.push_back(std::move(entry));
    }
    if (PersistentDraftBodyIsEmpty(draft)) {
      return corrupt(L"草稿记录不该是空正文（空正文的删除在写入时就不落盘）");
    }
    if (PersistentDraftContentTooLarge(draft.form)) {
      return corrupt(L"草稿正文超过上限");
    }
    std::wstring refusal;
    UpsertPersistentDraft(&state, std::move(draft), &refusal);
  }

  if (version <= 0) {
    // 未分版本的老文件（没有版本头，或自报 0）：至少要有一条认得的记录才算「老配置」，
    // 否则就是坏文件。
    const bool hasAnything = hasPolicy || hasRevision || hasGit || !state.recentRepositories.empty() ||
                             state.window.valid || state.columns.valid || !state.drafts.empty();
    if (!hasAnything) {
      result.status = PersistentLoadStatus::corrupt;
      result.reason = L"没有任何本程序认得的记录";
      return result;
    }
    // v0 没有「首次说明」的概念：迁移后按「问过一次」处理会跳过说明，因此保持未询问、
    // 由界面重新展示首次说明；开关按 v0 的默认（启用保存、启用草稿）落地。
    result.status = PersistentLoadStatus::migrated;
    result.reason = L"老版本（未分版本）的记录文件已迁移到版本 " + std::to_wstring(kPersistentFormatVersion);
    state.formatVersion = kPersistentFormatVersion;
    result.state = std::move(state);
    return result;
  }

  result.status = PersistentLoadStatus::loaded;
  result.state = std::move(state);
  return result;
}

PersistentState MergeForWrite(const PersistentState& disk, const PersistentState& ours,
                              const PersistentWriteIntents& intents, PersistentMergeReport* report) {
  PersistentState merged = disk;
  merged.formatVersion = kPersistentFormatVersion;
  merged.revision = disk.revision + 1;

  if (intents.replaceAll) {
    // 「清除已存记录」：内容整份以本窗口为准（此刻本窗口就是空的），只沿用盘上的修订号继续数。
    merged.gitExecutablePath = ours.gitExecutablePath;
    merged.recentRepositories = ours.recentRepositories;
    merged.window = ours.window;
    merged.columns = ours.columns;
    merged.drafts = ours.drafts;
  } else {
    if (intents.policy) {
      merged.consentRecorded = ours.consentRecorded;
      merged.persistenceEnabled = ours.persistenceEnabled;
      merged.draftSavingEnabled = ours.draftSavingEnabled;
    }
    if (intents.gitPath || merged.gitExecutablePath.empty()) {
      // 点名要写、或盘上本来就是空的（首次保存）：用本窗口的值。
      // 「填洞」不算踩踏：盘上没有值，就没有别的窗口的数据可被覆盖。
      if (!ours.gitExecutablePath.empty()) {
        merged.gitExecutablePath = ours.gitExecutablePath;
      }
    }
    if ((intents.windowGeometry || !merged.window.valid) && ours.window.valid) {
      merged.window = ours.window;
    }
    if ((intents.columns || !merged.columns.valid) && ours.columns.valid) {
      merged.columns = ours.columns;
    }
    if (intents.recentList || merged.recentRepositories.empty()) {
      // 本窗口的顺序优先，盘上独有的续在后面（两个窗口各开各的仓库，谁都不该丢）。
      std::vector<std::wstring> recent = ours.recentRepositories;
      for (const std::wstring& entry : disk.recentRepositories) {
        const std::wstring key = git::CanonicalPathKey(entry);
        const bool seen = std::any_of(recent.begin(), recent.end(),
                                      [&](const std::wstring& oursEntry) {
                                        return git::CanonicalPathKey(oursEntry) == key;
                                      });
        if (!seen) {
          recent.push_back(entry);
        }
      }
      if (recent.size() > kMaxRecentRepositories) {
        recent.resize(kMaxRecentRepositories);
      }
      merged.recentRepositories = std::move(recent);
    }
    if (intents.drafts) {
      // 逐键合并：本窗口带来的（含空正文的删除墓碑）与盘上的比保存时刻。
      for (const PersistentDraft& ourDraft : ours.drafts) {
        const auto found = std::find_if(merged.drafts.begin(), merged.drafts.end(),
                                        [&](const PersistentDraft& draft) {
                                          return draft.repositoryKey == ourDraft.repositoryKey;
                                        });
        if (found == merged.drafts.end()) {
          merged.drafts.push_back(ourDraft);
          continue;
        }
        if (found->savedAtEpoch > ourDraft.savedAtEpoch) {
          // 盘上的更新：保留它，并把这件事报告出去（本窗口屏幕上的内容没有落盘）。
          if (report != nullptr) {
            report->newerDraftsKeptFromDisk.push_back(found->repositoryRoot);
          }
          continue;
        }
        *found = ourDraft;
      }
    }
  }

  if (!merged.draftSavingEnabled) {
    merged.drafts.clear();
  } else {
    // 删除墓碑（空正文）到此兑现为「没有这条记录」。
    std::erase_if(merged.drafts, [](const PersistentDraft& draft) {
      return PersistentDraftBodyIsEmpty(draft);
    });
    if (merged.drafts.size() > kMaxPersistedDrafts) {
      SortDraftsByAge(&merged.drafts);
      merged.drafts.erase(merged.drafts.begin(),
                          merged.drafts.begin() +
                              static_cast<long long>(merged.drafts.size() - kMaxPersistedDrafts));
    }
  }
  return merged;
}

std::wstring DescribeStoragePath(std::wstring_view directory, std::wstring_view fileName) {
  if (directory.empty()) {
    return std::wstring(L"（当前环境取不到应用数据目录）");
  }
  std::wstring path(directory);
  if (path.back() != L'\\' && path.back() != L'/') {
    path.push_back(L'\\');
  }
  path += fileName;
  return path;
}

}  // namespace gc::app
