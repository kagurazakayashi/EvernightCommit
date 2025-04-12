#include "git/author_config.h"

#include <algorithm>

namespace gc::git {
namespace {

// `--null` 的輸出恰好是「<值>\0」：值本身不可能含 NUL，NUL 之後也不該再有字節。
// 形态不對就整份拒絕，不採用「看起来像」的一半——否則一個多輸出的 NUL 段會被当成身份的一部分。
bool TakeNulTerminatedValue(std::wstring_view output, std::wstring* value) {
  const size_t nul = output.find(L'\0');
  if (nul == std::wstring_view::npos) {
    return false;
  }
  if (nul + 1 != output.size()) {
    return false;  // NUL 之後還有別的東西：不是預期的單段輸出。
  }
  *value = std::wstring(output.substr(0, nul));
  return true;
}

std::wstring FirstNonEmptyLine(std::wstring_view text) {
  size_t cursor = 0;
  while (cursor < text.size()) {
    size_t end = text.find_first_of(L"\r\n", cursor);
    if (end == std::wstring_view::npos) {
      end = text.size();
    }
    std::wstring line = TrimWide(text.substr(cursor, end - cursor));
    if (!line.empty()) {
      return line;
    }
    cursor = (end == text.size()) ? text.size() : end + 1;
  }
  return {};
}

// Git 讀取設定檔值時不修剪頭尾空白，但把值當作身份姓名／郵箱使用時會自己修剪
// （實測：user.name = "  Spa ced  " 時 `git config --get` 回原值，而 `git log --format=%an` 回
// "Spa ced"）。界面顯示的是「實際會寫進提交的身份」，因此這裡同样修剪外圍空白。
std::wstring IdentityPart(std::wstring_view raw) {
  return TrimWide(raw);
}

}  // namespace

std::vector<std::wstring> BuildIdentityConfigArguments(std::wstring_view repositoryDirectory,
                                                      std::wstring_view key) {
  return std::vector<std::wstring>{
      L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks", L"config", L"--null", L"--get",
      std::wstring(key)};
}

ConfigValueRead ParseIdentityConfigQuery(const GitQueryResult& result, std::wstring_view key) {
  ConfigValueRead read;

  // 判定順序很重要：先問「程序有沒有真的跑起來並返回」，這些情形下的退出碼沒有意義。
  if (!result.started) {
    read.state = ConfigValueState::failed;
    read.error = RepoError::gitLaunchFailed;
    read.detail = L"Git 程序未能启动";
    return read;
  }
  if (result.timedOut || !result.exited) {
    read.state = ConfigValueState::failed;
    read.error = RepoError::gitTimeout;
    read.detail = L"Git 未在限定时间内返回";
    return read;
  }
  if (result.exitCode == 1) {
    // 這不是失敗：Git 用退出碼 1 表示「這個 key 沒有設過」。
    read.state = ConfigValueState::absent;
    return read;
  }
  if (result.exitCode == 0) {
    std::wstring value;
    if (!TakeNulTerminatedValue(result.utf16Output, &value)) {
      read.state = ConfigValueState::failed;
      read.error = RepoError::badOutput;
      read.detail = L"Git 对 " + std::wstring(key) + L" 的回答不符合约定（缺少结束用的空字节）";
      return read;
    }
    read.state = ConfigValueState::present;
    read.value = std::move(value);
    return read;
  }

  // 其餘非 0 退出碼交給仓库識別那套歸類：設定檔語法、目錄權限、safe.directory 等都有現成原因。
  std::wstring detail;
  const RepoError failure = ClassifyGitFailure(result, detail);
  read.state = ConfigValueState::failed;
  read.error = failure == RepoError::none ? RepoError::gitFailed : failure;
  read.detail = detail.empty() ? FirstNonEmptyLine(result.utf16Error) : detail;
  if (read.detail.empty()) {
    read.detail = L"退出码 " + std::to_wstring(static_cast<unsigned long long>(result.exitCode));
  }
  read.detail += L"（" + std::wstring(key) + L"）";
  return read;
}

CommitterIdentityState AuthorIdentityConfig::CommitterState() const noexcept {
  if (ReadFailed()) {
    return CommitterIdentityState::unknown;
  }
  const bool hasName = name.state == ConfigValueState::present && !IdentityPart(name.value).empty();
  const bool hasEmail = email.state == ConfigValueState::present && !IdentityPart(email.value).empty();
  return (hasName && hasEmail) ? CommitterIdentityState::available : CommitterIdentityState::missing;
}

std::wstring AuthorIdentityConfig::Identity() const {
  if (CommitterState() != CommitterIdentityState::available) {
    return {};
  }
  GitIdentity identity;
  identity.name = IdentityPart(name.value);
  identity.email = IdentityPart(email.value);
  // 配置裡的值本身可能不是「姓名 <邮箱>」能接受的形態（例如郵箱沒有 @），
  // 這時仍照原樣給出預設文字：界面會把它當成「使用者已輸入的內容」並在校驗時指出問題。
  return identity.Format();
}

AuthorIdentityConfig CombineIdentityConfig(const ConfigValueRead& name, const ConfigValueRead& email) {
  AuthorIdentityConfig config;
  config.name = name;
  config.email = email;
  return config;
}

std::wstring BuildIdentityConfigNotice(const AuthorIdentityConfig& config) {
  if (config.ReadFailed()) {
    const std::wstring& detail =
        config.name.state == ConfigValueState::failed ? config.name.detail : config.email.detail;
    return L"读取作者默认身份失败：" + detail +
           L"。作者输入框保持原样，可以手工填写；本程序不会改动你的 Git 配置。";
  }

  const bool hasName = config.name.state == ConfigValueState::present &&
                       !IdentityPart(config.name.value).empty();
  const bool hasEmail = config.email.state == ConfigValueState::present &&
                        !IdentityPart(config.email.value).empty();
  if (hasName && hasEmail) {
    return {};
  }
  if (!hasName && !hasEmail) {
    return L"这个仓库的有效 Git 配置里没有身份（user.name 与 user.email 都没设置）。"
           L"请在“作者”里填写「姓名 <邮箱>」；本程序不会替你改动 Git 配置。";
  }
  if (!hasName) {
    return L"这个仓库的有效 Git 配置里 user.name 是空的，无法凑成完整身份。"
           L"请在“作者”里补全「姓名 <邮箱>」；本程序不会替你改动 Git 配置。";
  }
  return L"这个仓库的有效 Git 配置里 user.email 是空的，无法凑成完整身份。"
         L"请在“作者”里补全「姓名 <邮箱>」；本程序不会替你改动 Git 配置。";
}

}  // namespace gc::git
