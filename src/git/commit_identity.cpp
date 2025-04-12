#include "git/commit_identity.h"

#include <algorithm>
#include <cwctype>

namespace gc::git {
namespace {

// 控制字元與行分隔字元一律拒絕：身份稍後要寫進提交與 `-F` 清單檔案，
// 換行會把一條身份變成兩行文字（進而可能被解釋成另一條 trailer）。
constexpr bool IsForbiddenCharacter(wchar_t value) noexcept {
  if (value < 0x20 || value == 0x7F) {
    return true;
  }
  if (value >= 0x80 && value <= 0x9F) {
    return true;  // C1 控制字元。
  }
  return value == 0x2028 || value == 0x2029;  // Unicode 行／段分隔符。
}

// 「像空格但不是控制字元」的一類：全形空格、不换行空格等都算空白，
// 姓名與郵箱的邊界不能靠這些字元蒙過去。
constexpr bool IsSpaceLike(wchar_t value) noexcept {
  if (value == L' ' || value == 0x00A0 || value == 0x1680 || value == 0x202F || value == 0x205F ||
      value == 0x3000) {
    return true;
  }
  return value >= 0x2000 && value <= 0x200A;
}

std::wstring TrimSpaces(std::wstring_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && IsSpaceLike(text[begin])) {
    ++begin;
  }
  while (end > begin && IsSpaceLike(text[end - 1])) {
    --end;
  }
  return std::wstring(text.substr(begin, end - begin));
}

// 把內部連續空白折成一個半形空格：只用於比較鍵，不改寫使用者實際輸入的文字。
std::wstring CollapseSpaces(std::wstring_view text) {
  std::wstring result;
  result.reserve(text.size());
  bool pendingSpace = false;
  for (const wchar_t value : text) {
    if (IsSpaceLike(value)) {
      pendingSpace = !result.empty();
      continue;
    }
    if (pendingSpace) {
      result.push_back(L' ');
      pendingSpace = false;
    }
    result.push_back(value);
  }
  return result;
}

// 只做 ASCII 大小寫摺疊：郵箱的網域名稱部分大小寫不敏感，而本模組不依賴 Win32 或 ICU，
// 因此非 ASCII 字母一律原樣比較（少見的 Unicode 信箱大小寫不同時視為兩條，宁可多留一條）。
std::wstring AsciiLower(std::wstring_view text) {
  std::wstring result(text);
  std::transform(result.begin(), result.end(), result.begin(), [](wchar_t value) {
    return (value >= L'A' && value <= L'Z') ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
  });
  return result;
}

std::wstring FormatError(std::wstring_view roleLabel, std::wstring_view reason) {
  return std::wstring(roleLabel) + L"要写成「姓名 <邮箱>」：" + std::wstring(reason);
}

}  // namespace

std::wstring GitIdentity::Format() const {
  return name + L" <" + email + L">";
}

bool ParseGitIdentity(std::wstring_view text, GitIdentity* out, std::wstring* error,
                      std::wstring_view roleLabel) {
  const auto refuse = [&](std::wstring_view reason) {
    if (error != nullptr) {
      *error = FormatError(roleLabel, reason);
    }
    return false;
  };

  for (const wchar_t value : text) {
    if (IsForbiddenCharacter(value)) {
      return refuse(L"含有换行或控制字符，提交身份必须是单行文字。");
    }
  }

  const std::wstring trimmed = TrimSpaces(text);
  if (trimmed.empty()) {
    return refuse(L"这一项还没有填写。");
  }

  const size_t open = trimmed.rfind(L'<');
  if (open == std::wstring::npos) {
    return refuse(L"没有用 < 包住邮箱。");
  }
  if (trimmed.back() != L'>') {
    // 「< 之後根本沒有 >」與「> 之後還有字」是兩種壞形態，但都落在同一個檢查裡：
    // 結尾必須就是 `>`，否則 Git 會把多餘的文字丟掉或把郵箱拆錯。
    return refuse(L"邮箱没有以 > 结尾（> 之后也不能再有别的文字）。");
  }

  GitIdentity parsed;
  parsed.name = TrimSpaces(trimmed.substr(0, open));
  parsed.email = TrimSpaces(trimmed.substr(open + 1, trimmed.size() - open - 2));

  if (parsed.name.empty()) {
    return refuse(L"缺少姓名。");
  }
  if (parsed.name.find_first_of(L"<>") != std::wstring::npos) {
    return refuse(L"姓名里不能有 < 或 >。");
  }
  if (parsed.email.empty()) {
    return refuse(L"< > 之间没有邮箱。");
  }
  for (const wchar_t value : parsed.email) {
    if (IsSpaceLike(value)) {
      return refuse(L"邮箱里不能有空白。");
    }
  }
  const size_t at = parsed.email.find(L'@');
  if (at == std::wstring::npos) {
    return refuse(L"邮箱缺少 @。");
  }
  if (at == 0 || at + 1 == parsed.email.size()) {
    return refuse(L"邮箱的 @ 不能出现在开头或结尾。");
  }

  if (out != nullptr) {
    *out = std::move(parsed);
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::wstring ValidateGitIdentity(std::wstring_view text, std::wstring_view roleLabel) {
  std::wstring error;
  GitIdentity parsed;
  if (ParseGitIdentity(text, &parsed, &error, roleLabel)) {
    return {};
  }
  return error;
}

std::wstring CanonicalIdentityKey(std::wstring_view text) {
  GitIdentity parsed;
  std::wstring error;
  if (!ParseGitIdentity(text, &parsed, &error, L"身份")) {
    // 壞形態也要有確定的比較鍵：整串摺疊空白並做 ASCII 小寫。
    return AsciiLower(CollapseSpaces(text));
  }
  // 姓名與郵箱都做 ASCII 小寫：只差大小寫或姓名內多幾個空格的兩條，幾乎必然是同一個人，
  // 判成兩個人就會在提交訊息裡寫出兩條一模一樣的 Co-authored-by。
  return AsciiLower(CollapseSpaces(parsed.name)) + L" <" + AsciiLower(parsed.email) + L">";
}

std::vector<std::wstring> DeduplicateIdentities(const std::vector<std::wstring>& entries,
                                                std::vector<std::wstring>* removed) {
  if (removed != nullptr) {
    removed->clear();
  }
  std::vector<std::wstring> kept;
  kept.reserve(entries.size());
  std::vector<std::wstring> keys;  // 與 kept 同序的比較鍵，避免逐條重複 CanonicalIdentityKey。
  for (const std::wstring& entry : entries) {
    const std::wstring key = CanonicalIdentityKey(entry);
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
      if (removed != nullptr) {
        removed->push_back(entry);
      }
      continue;
    }
    keys.push_back(key);
    kept.push_back(entry);
  }
  return kept;
}

}  // namespace gc::git
