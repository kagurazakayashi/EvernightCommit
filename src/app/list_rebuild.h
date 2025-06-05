#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gc::app {

// 有键列表（更改/历史/文件变化）的视图更新计划：按「头尾公共段 + 中段差异」拆行，
// 让界面只删多余行、只补少的行，其余就地改文本——全程不清空整列。
//
// 这条约定来自 app/list_view_memory.h 的同一条事实：清空重建会把报表视图的滚动位置
// 退回顶部且没有任何消息能救回来；就地增删则让控件视口原地不动。选中项本来就不靠行号
// （按条目身份经 MapListViewMemory 恢复），因此这里只回答「行怎么动」。
struct KeyedRebuildPlan {
  size_t commonPrefix = 0;  // 键逐一行相同的开头行数
  size_t commonSuffix = 0;  // 键逐一行相同的结尾行数（与开头段不重叠）
  size_t removedRows = 0;   // 从旧列表删除的行数（位置 = commonPrefix）
  size_t insertedRows = 0;  // 在 commonPrefix 处插入的行数

  // 界面执行顺序：先删 removedRows 行，再插入 insertedRows 行，然后逐行刷新文本。
  // 更新后的总行数 = commonPrefix + insertedRows + commonSuffix = newKeys.size()。
};

// oldKeys / newKeys 为「上一次落地的键序列」与「这次要落地的键序列」
// （更改列表按相对路径、历史列表按完整对象 ID——键的选取见 list_view_memory.h）。
[[nodiscard]] KeyedRebuildPlan PlanKeyedRebuild(const std::vector<std::wstring>& oldKeys,
                                                const std::vector<std::wstring>& newKeys);

}  // namespace gc::app
