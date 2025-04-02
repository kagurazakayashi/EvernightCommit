#include "app/list_view_memory.h"

#include <algorithm>

namespace gc::app {

std::wstring ViewKeyForItem(const git::ChangeItem& item) { return item.path; }

RestoredListView MapListViewMemory(const std::vector<git::ChangeItem>& items, const ListViewMemory& memory) {
  RestoredListView restored;
  if (memory.selectedKeys.empty()) {
    return restored;  // 沒有要保留的選擇：不要把第 0 行當成「使用者選中的那一行」。
  }

  // 同一個鍵在一個列表裡最多出現一次（porcelain v2 的同一欄不會重複報告同一個路徑），
  // 因此每個鍵只取第一處命中，異常輸出也不會把一行複制成多條選中記錄。
  for (size_t row = 0; row < items.size(); ++row) {
    const std::wstring key = ViewKeyForItem(items[row]);
    if (!key.empty() && std::find(memory.selectedKeys.begin(), memory.selectedKeys.end(), key) !=
                            memory.selectedKeys.end()) {
      restored.selectedRows.push_back(static_cast<int>(row));
    }
  }
  return restored;
}

}  // namespace gc::app
