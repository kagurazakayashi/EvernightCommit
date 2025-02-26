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

}  // namespace gc::git
