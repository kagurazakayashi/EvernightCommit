#include "support/tiny_test.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/command_window.h"
#include "git/git_environment.h"

namespace {

using gc::git::EnvironmentOverride;
using gc::git::GitEnvironmentPlan;
using gc::git::GitRunPurpose;

std::wstring ToUpper(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'a' && c <= L'z') {
      c = static_cast<wchar_t>(c - 32);
    }
  }
  return result;
}

// 找名字对应的最后一条覆盖（合并按序应用，最后一条才决定最终效果）。
const EnvironmentOverride* FindLastOverride(const std::vector<EnvironmentOverride>& overrides,
                                            std::wstring_view name) {
  const std::wstring wanted = ToUpper(name);
  const EnvironmentOverride* found = nullptr;
  for (const EnvironmentOverride& entry : overrides) {
    if (ToUpper(entry.name) == wanted) {
      found = &entry;
    }
  }
  return found;
}

// 名字第一次以「删除」形态出现的下标；没有则返回 npos。
size_t FirstRemovalIndex(const std::vector<EnvironmentOverride>& overrides, std::wstring_view name) {
  const std::wstring wanted = ToUpper(name);
  for (size_t index = 0; index < overrides.size(); ++index) {
    if (ToUpper(overrides[index].name) == wanted && !overrides[index].value.has_value()) {
      return index;
    }
  }
  return std::wstring::npos;
}

size_t LastWriteIndex(const std::vector<EnvironmentOverride>& overrides, std::wstring_view name) {
  const std::wstring wanted = ToUpper(name);
  size_t found = std::wstring::npos;
  for (size_t index = 0; index < overrides.size(); ++index) {
    if (ToUpper(overrides[index].name) == wanted && overrides[index].value.has_value()) {
      found = index;
    }
  }
  return found;
}

bool Removes(const GitEnvironmentPlan& plan, std::wstring_view name) {
  const EnvironmentOverride* entry = FindLastOverride(plan.overrides, name);
  return entry != nullptr && !entry->value.has_value();
}

// 「重定向 + 身份」全名单：验收要求逐项覆盖这些变量的继承场景。
const std::wstring& AllPolicyNames() {
  static const std::wstring joined = [] {
    GitEnvironmentPlan plan = gc::git::MakeGitEnvironmentPlan(GitRunPurpose::backgroundProbe, {});
    std::wstring text;
    for (const std::wstring& name : plan.redirectNames) {
      if (!text.empty()) {
        text += L"|";
      }
      text += name;
    }
    return text;
  }();
  return joined;
}

}  // namespace

GC_TEST(git_environment_probe_plan_removes_all_redirects) {
  const GitEnvironmentPlan plan = gc::git::MakeGitEnvironmentPlan(GitRunPurpose::backgroundProbe, {});
  // 仓库定位
  GC_CHECK(Removes(plan, L"GIT_DIR"));
  GC_CHECK(Removes(plan, L"GIT_WORK_TREE"));
  GC_CHECK(Removes(plan, L"GIT_NAMESPACE"));
  // 索引与对象库
  GC_CHECK(Removes(plan, L"GIT_INDEX_FILE"));
  GC_CHECK(Removes(plan, L"GIT_OBJECT_DIRECTORY"));
  GC_CHECK(Removes(plan, L"GIT_ALTERNATE_OBJECT_DIRECTORIES"));
  // 配置注入与重定向
  GC_CHECK(Removes(plan, L"GIT_CONFIG_PARAMETERS"));
  GC_CHECK(Removes(plan, L"GIT_CONFIG_COUNT"));
  GC_CHECK(Removes(plan, L"GIT_CONFIG_GLOBAL"));
  GC_CHECK(Removes(plan, L"GIT_CONFIG_SYSTEM"));
  GC_CHECK(Removes(plan, L"GIT_CONFIG"));
  // 身份与时间
  GC_CHECK(Removes(plan, L"GIT_AUTHOR_NAME"));
  GC_CHECK(Removes(plan, L"GIT_AUTHOR_EMAIL"));
  GC_CHECK(Removes(plan, L"GIT_AUTHOR_DATE"));
  GC_CHECK(Removes(plan, L"GIT_COMMITTER_NAME"));
  GC_CHECK(Removes(plan, L"GIT_COMMITTER_EMAIL"));
  GC_CHECK(Removes(plan, L"GIT_COMMITTER_DATE"));
  // 后台探测不许删凭据助手：那是用户自己的认证方式，看得见、也不挂在隐形输入上。
  GC_CHECK(FindLastOverride(plan.overrides, L"GIT_ASKPASS") == nullptr);
  GC_CHECK(FindLastOverride(plan.overrides, L"SSH_ASKPASS") == nullptr);
  // 后台探测绝不能停在看不见的问题上。
  const EnvironmentOverride* prompt = FindLastOverride(plan.overrides, L"GIT_TERMINAL_PROMPT");
  GC_CHECK(prompt != nullptr && prompt->value.has_value() && *prompt->value == L"0");
}

GC_TEST(git_environment_window_plan_keeps_in_window_interaction) {
  const GitEnvironmentPlan plan = gc::git::MakeGitEnvironmentPlan(GitRunPurpose::commandWindow, {});
  // 重定向与身份同样全删：预检与执行语义一致的前提是同一份名单。
  GC_CHECK(Removes(plan, L"GIT_DIR"));
  GC_CHECK(Removes(plan, L"GIT_INDEX_FILE"));
  GC_CHECK(Removes(plan, L"GIT_AUTHOR_NAME"));
  GC_CHECK(Removes(plan, L"GIT_COMMITTER_DATE"));
  // 交互契约：提问留在窗口、分页器不许扣住退出码、askpass 弹窗形态被排除。
  const EnvironmentOverride* prompt = FindLastOverride(plan.overrides, L"GIT_TERMINAL_PROMPT");
  GC_CHECK(prompt != nullptr && prompt->value.has_value() && *prompt->value == L"1");
  const EnvironmentOverride* pager = FindLastOverride(plan.overrides, L"GIT_PAGER");
  GC_CHECK(pager != nullptr && pager->value.has_value() && *pager->value == L"cat");
  GC_CHECK(Removes(plan, L"GIT_ASKPASS"));
  GC_CHECK(Removes(plan, L"SSH_ASKPASS"));
}

GC_TEST(git_environment_operation_overrides_apply_last) {
  // 表单的作者覆盖与「操作自己注入配置」都必须赢过策略的删除：顺序即优先级。
  const std::vector<EnvironmentOverride> operation = {
      EnvironmentOverride{L"GIT_AUTHOR_NAME", std::wstring(L"表单作者")},
      EnvironmentOverride{L"GIT_CONFIG_COUNT", std::wstring(L"1")},
  };
  const GitEnvironmentPlan plan =
      gc::git::MakeGitEnvironmentPlan(GitRunPurpose::commandWindow, operation);

  const size_t removal = FirstRemovalIndex(plan.overrides, L"GIT_AUTHOR_NAME");
  const size_t write = LastWriteIndex(plan.overrides, L"GIT_AUTHOR_NAME");
  GC_CHECK(removal != std::wstring::npos);
  GC_CHECK(write != std::wstring::npos);
  GC_CHECK_MESSAGE(removal < write, "操作覆盖必须排在策略删除之后");

  const size_t countRemoval = FirstRemovalIndex(plan.overrides, L"GIT_CONFIG_COUNT");
  const size_t countWrite = LastWriteIndex(plan.overrides, L"GIT_CONFIG_COUNT");
  GC_CHECK(countRemoval != std::wstring::npos);
  GC_CHECK_MESSAGE(countRemoval < countWrite, "操作注入的配置必须排在策略删除之后");

  // 策略注入（GIT_TERMINAL_PROMPT=1）也在操作覆盖之前：请求里同名项说了算。
  const std::vector<EnvironmentOverride> withPrompt = {
      EnvironmentOverride{L"GIT_TERMINAL_PROMPT", std::wstring(L"0")},
  };
  const GitEnvironmentPlan overridden =
      gc::git::MakeGitEnvironmentPlan(GitRunPurpose::commandWindow, withPrompt);
  const EnvironmentOverride* last = FindLastOverride(overridden.overrides, L"GIT_TERMINAL_PROMPT");
  GC_CHECK(last != nullptr && last->value.has_value() && *last->value == L"0");
}

GC_TEST(git_environment_redirect_names_cover_deletions) {
  // redirectNames（告知候选）必须囊括全部「重定向类」删除，但不含交互类。
  const GitEnvironmentPlan plan = gc::git::MakeGitEnvironmentPlan(GitRunPurpose::commandWindow, {});
  const std::wstring listed = AllPolicyNames();
  for (std::wstring_view name : {L"GIT_DIR", L"GIT_WORK_TREE", L"GIT_INDEX_FILE", L"GIT_CONFIG_COUNT",
                                 L"GIT_CONFIG_GLOBAL", L"GIT_AUTHOR_NAME", L"GIT_COMMITTER_EMAIL",
                                 L"GIT_NAMESPACE", L"GIT_OBJECT_DIRECTORY"}) {
    GC_CHECK_MESSAGE(listed.find(name) != std::wstring::npos, "告知名单缺少一项重定向变量");
  }
  GC_CHECK(listed.find(L"GIT_ASKPASS") == std::wstring::npos);
  GC_CHECK(listed.find(L"SSH_ASKPASS") == std::wstring::npos);
  GC_CHECK(listed.find(L"GIT_PAGER") == std::wstring::npos);
}

GC_TEST(git_environment_numbered_config_name_matching) {
  GC_CHECK(gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG_KEY_0"));
  GC_CHECK(gc::git::IsNumberedConfigInjectionName(L"git_config_value_12"));
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG_KEY_"));
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG_KEY_A"));
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG_KEY_-1"));
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG_COUNT"));
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG"));
  // 用户自定义的同前缀名字不该被误删。
  GC_CHECK(!gc::git::IsNumberedConfigInjectionName(L"GIT_CONFIG_KEY_NAME"));
}

GC_TEST(git_environment_finds_inherited_redirects_case_insensitively) {
  const std::vector<std::wstring> base = {
      L"PATH=C:\\bin", L"Git_Dir=C:\\other\\repo\\.git", L"=C:=C:\\Temp",
      L"git_author_name=继承劫持", L"GIT_INDEX_FILE="};
  GitEnvironmentPlan plan = gc::git::MakeGitEnvironmentPlan(GitRunPurpose::backgroundProbe, {});
  const std::vector<std::wstring> found =
      gc::git::FindInheritedRedirects(base, plan.redirectNames);
  GC_CHECK(std::find(found.begin(), found.end(), L"GIT_DIR") != found.end());
  GC_CHECK(std::find(found.begin(), found.end(), L"GIT_AUTHOR_NAME") != found.end());
  GC_CHECK(std::find(found.begin(), found.end(), L"GIT_INDEX_FILE") != found.end());
  GC_CHECK(std::find(found.begin(), found.end(), L"PATH") == found.end());
  // 返回的必须是名单里的规范写法，不能是继承项原文（原文带着值）。
  for (const std::wstring& name : found) {
    GC_CHECK(name.find(L'=') == std::wstring::npos);
    GC_CHECK(name != L"Git_Dir" && name != L"git_author_name");
  }
  // 没有重定向的干净环境必须一无所获。
  GC_CHECK(gc::git::FindInheritedRedirects({L"PATH=C:\\bin"}, plan.redirectNames).empty());
}

GC_TEST(git_environment_notice_lists_names_never_values) {
  GC_CHECK(gc::git::BuildRedirectNoticeText({}).empty());
  const std::wstring notice =
      gc::git::BuildRedirectNoticeText({L"GIT_DIR", L"GIT_INDEX_FILE", L"GIT_AUTHOR_NAME"});
  GC_CHECK(notice.find(L"GIT_DIR") != std::wstring::npos);
  GC_CHECK(notice.find(L"GIT_INDEX_FILE") != std::wstring::npos);
  GC_CHECK(notice.find(L"GIT_AUTHOR_NAME") != std::wstring::npos);
  GC_CHECK(notice.find(L"C:\\") == std::wstring::npos);  // 告知文本里没有值的容身之处

  std::vector<std::wstring> many;
  for (int index = 0; index < 20; ++index) {
    many.push_back(L"GIT_DIR_" + std::to_wstring(index));
  }
  const std::wstring folded = gc::git::BuildRedirectNoticeText(many);
  GC_CHECK(folded.find(L"等") != std::wstring::npos);
  GC_CHECK(folded.size() < 400);  // 告知必须限长，不能变成环境清单
}
