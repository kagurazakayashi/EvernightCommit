#include "git/git_environment.h"

#include <algorithm>

namespace gc::git {
namespace {

// 覆盖名单集中在这里，头文件的分类说明逐条对应。改名单先改说明：
// 每一条"为什么删/为什么注入"都必须是能向用户解释的理由，不是随手加的保险。
constexpr std::wstring_view kRepositoryRedirects[] = {
    L"GIT_DIR", L"GIT_WORK_TREE", L"GIT_NAMESPACE"};

constexpr std::wstring_view kIndexObjectRedirects[] = {
    L"GIT_INDEX_FILE", L"GIT_OBJECT_DIRECTORY", L"GIT_ALTERNATE_OBJECT_DIRECTORIES"};

// GIT_CONFIG（Git 1.x 时代的全局配置别名）一并删除：留着它等于留着一道绕过删除的侧门。
constexpr std::wstring_view kConfigInjectionRedirects[] = {
    L"GIT_CONFIG_PARAMETERS", L"GIT_CONFIG_COUNT",   L"GIT_CONFIG_GLOBAL",
    L"GIT_CONFIG_SYSTEM",     L"GIT_CONFIG"};

constexpr std::wstring_view kIdentityOverrides[] = {
    L"GIT_AUTHOR_NAME",     L"GIT_AUTHOR_EMAIL",     L"GIT_AUTHOR_DATE",
    L"GIT_COMMITTER_NAME",  L"GIT_COMMITTER_EMAIL",  L"GIT_COMMITTER_DATE"};

// 命令窗口的交互契约：提问留在窗口里、分页器不许扣住退出码。
constexpr std::wstring_view kCommandWindowAskPassRemovals[] = {L"GIT_ASKPASS", L"SSH_ASKPASS"};

std::wstring ToUpperAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'a' && c <= L'z') {
      c = static_cast<wchar_t>(c - 32);
    }
  }
  return result;
}

void AppendRemovals(std::vector<EnvironmentOverride>& overrides,
                    const std::wstring_view* names, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    overrides.push_back(EnvironmentOverride{std::wstring(names[index]), std::nullopt});
  }
}

}  // namespace

GitEnvironmentPlan MakeGitEnvironmentPlan(GitRunPurpose purpose,
                                          const std::vector<EnvironmentOverride>& operationOverrides) {
  GitEnvironmentPlan plan;
  // 顺序：先删重定向与身份，再做形态注入，最后原样落下操作自己的覆盖。
  // MergeEnvironmentEntries 按序应用、后面的赢，因此这个顺序就是优先级。
  AppendRemovals(plan.overrides, kRepositoryRedirects, std::size(kRepositoryRedirects));
  AppendRemovals(plan.overrides, kIndexObjectRedirects, std::size(kIndexObjectRedirects));
  AppendRemovals(plan.overrides, kConfigInjectionRedirects, std::size(kConfigInjectionRedirects));
  AppendRemovals(plan.overrides, kIdentityOverrides, std::size(kIdentityOverrides));
  if (purpose == GitRunPurpose::commandWindow) {
    AppendRemovals(plan.overrides, kCommandWindowAskPassRemovals, std::size(kCommandWindowAskPassRemovals));
    plan.overrides.push_back(
        EnvironmentOverride{L"GIT_TERMINAL_PROMPT", std::wstring(L"1")});
    plan.overrides.push_back(EnvironmentOverride{L"GIT_PAGER", std::wstring(L"cat")});
  } else {
    // 后台探测的 stdin 是空的：任何"等终端输入"的提问都等于把查询挂死到超时。
    // 凭据助手（askpass/GCM）保留——那是用户自己的认证方式，看得见、也不挂在隐形输入上。
    plan.overrides.push_back(
        EnvironmentOverride{L"GIT_TERMINAL_PROMPT", std::wstring(L"0")});
  }
  plan.overrides.insert(plan.overrides.end(), operationOverrides.begin(), operationOverrides.end());

  for (const std::wstring_view name : kRepositoryRedirects) {
    plan.redirectNames.emplace_back(name);
  }
  for (const std::wstring_view name : kIndexObjectRedirects) {
    plan.redirectNames.emplace_back(name);
  }
  for (const std::wstring_view name : kConfigInjectionRedirects) {
    plan.redirectNames.emplace_back(name);
  }
  for (const std::wstring_view name : kIdentityOverrides) {
    plan.redirectNames.emplace_back(name);
  }
  return plan;
}

bool IsNumberedConfigInjectionName(std::wstring_view name) {
  static constexpr std::wstring_view kPrefixes[] = {L"GIT_CONFIG_KEY_", L"GIT_CONFIG_VALUE_"};
  const std::wstring upper = ToUpperAscii(name);
  for (const std::wstring_view prefix : kPrefixes) {
    if (upper.size() <= prefix.size() || upper.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    // 前缀之后必须全是不含符号与前导空白的十进制数字，否则不算这一族的名字
    //（比如用户真的有一个叫 GIT_CONFIG_KEY_NAME 的自定义变量时不该被误删）。
    bool digitsOnly = true;
    for (size_t index = prefix.size(); index < upper.size(); ++index) {
      if (upper[index] < L'0' || upper[index] > L'9') {
        digitsOnly = false;
        break;
      }
    }
    if (digitsOnly) {
      return true;
    }
  }
  return false;
}

std::vector<std::wstring> FindInheritedRedirects(const std::vector<std::wstring>& baseEntries,
                                                 const std::vector<std::wstring>& redirectNames) {
  std::vector<std::wstring> found;
  for (const std::wstring& wanted : redirectNames) {
    const std::wstring wantedUpper = ToUpperAscii(wanted);
    for (const std::wstring& entry : baseEntries) {
      // 盘符联动项（“=C:=…”）与没有 '=' 的畸形项都不参与匹配。
      if (entry.empty() || entry.front() == L'=') {
        continue;
      }
      const size_t equals = entry.find(L'=');
      if (equals == std::wstring::npos) {
        continue;
      }
      if (ToUpperAscii(std::wstring_view(entry).substr(0, equals)) == wantedUpper) {
        if (std::find(found.begin(), found.end(), wanted) == found.end()) {
          found.push_back(wanted);
        }
        break;
      }
    }
  }
  return found;
}

std::wstring BuildRedirectNoticeText(const std::vector<std::wstring>& foundNames) {
  if (foundNames.empty()) {
    return {};
  }
  // 只列名字，且限数：告知的对象是"哪些变量被移除了"，不是环境快照。
  constexpr size_t kListedNameLimit = 8;
  std::wstring text = L"已移除继承环境里的 Git 重定向变量：";
  for (size_t index = 0; index < foundNames.size() && index < kListedNameLimit; ++index) {
    if (index != 0) {
      text += L"、";
    }
    text += foundNames[index];
  }
  if (foundNames.size() > kListedNameLimit) {
    text += L" 等";
  }
  text += L"（只显示名字，不显示值；本次执行仍绑定界面所选仓库与其索引）。";
  return text;
}

}  // namespace gc::git
