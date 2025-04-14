#include "app/list_view_memory.h"

#include <algorithm>

namespace gc::app {
namespace {

// 按鍵恢復選中的公共實現：鍵在一個列表裡最多出現一次（同一個路徑/同一條提交不會重複報告），
// 因此每個鍵只取第一處命中，異常輸出也不會把一行複制成多條選中記錄。
template <typename Items>
RestoredListView MapKeysToRows(const Items& items, const std::vector<std::wstring>& selectedKeys) {
  RestoredListView restored;
  if (selectedKeys.empty()) {
    return restored;  // 沒有要保留的選擇：不要把第 0 行當成「使用者選中的那一行」。
  }
  for (size_t row = 0; row < items.size(); ++row) {
    const std::wstring key = ViewKeyForItem(items[row]);
    if (!key.empty() && std::find(selectedKeys.begin(), selectedKeys.end(), key) != selectedKeys.end()) {
      restored.selectedRows.push_back(static_cast<int>(row));
    }
  }
  return restored;
}

}  // namespace

std::wstring ViewKeyForItem(const git::ChangeItem& item) { return item.path; }

std::wstring ViewKeyForItem(const git::CommitItem& item) { return item.objectId; }

RestoredListView MapListViewMemory(const std::vector<git::ChangeItem>& items,
                                   const ListViewMemory& memory) {
  return MapKeysToRows(items, memory.selectedKeys);
}

RestoredListView MapListViewMemory(const std::vector<git::CommitItem>& items,
                                   const ListViewMemory& memory) {
  return MapKeysToRows(items, memory.selectedKeys);
}

}  // namespace gc::app
