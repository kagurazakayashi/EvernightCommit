#include "git/workspace_model.h"

namespace gc::git {

EmptyStateTexts NotLoadedTexts() noexcept {
  // 显式断行：纯中文文案没有可断行的空格，窄栏内单行绘制会被裁剪。
  static constexpr EmptyStateTexts kTexts{
      L"暂无数据\r\n尚未接入 git status。",
      L"暂无数据\r\n尚未接入 git status。",
      L"暂无数据\r\n尚未接入 git log。"};

  return kTexts;
}

EmptyStateTexts LoadedButNotImplementedTexts() noexcept {
  static constexpr EmptyStateTexts kTexts{
      L"仓库已识别\r\n文件列表将在后续步骤接入 git status。",
      L"仓库已识别\r\n文件列表将在后续步骤接入 git status。",
      L"仓库已识别\r\n提交历史将在后续步骤接入 git log。"};

  return kTexts;
}

}  // namespace gc::git
