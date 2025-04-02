#pragma once

#include <string>
#include <vector>

#include "git/workspace_model.h"

namespace gc::app {

// 刷新是「就地增删改行」，而行號在更新後可能對應到完全不同的條目
// （某个檔案被暫存後就從左側移到右側，排序也隨 Git 的輸出而變）。
// 因此使用者的選擇按「條目身份」記憶，更新後再映射回行號；
// 已經消失的條目自然不再被選中，不會憑空選中別的檔案。
//
// 滾動位置不需要記憶：就地更新不清空整列，控件自己的視口保持不動。
// 這不是優化——報表視圖不響應 LVM_SCROLL，整列清空重建以後
// 沒有任何消息能夠把視口放回原来的位置。

// 列表條目的記憶鍵：以相對於倉庫的路徑為準（重命名/複製條目用其新路徑），
// 與狀態欄文字、顯示用的「舊 → 新」文本無關。
[[nodiscard]] std::wstring ViewKeyForItem(const git::ChangeItem& item);

// 一個列表在刷新前的視圖狀態。
struct ListViewMemory {
  std::vector<std::wstring> selectedKeys;  // 仍被選中的條目鍵（多選）
};

// 映射回刷新後的列表：只給出应当处于选中状态的行号。
struct RestoredListView {
  std::vector<int> selectedRows;
};

[[nodiscard]] RestoredListView MapListViewMemory(const std::vector<git::ChangeItem>& items,
                                                 const ListViewMemory& memory);

}  // namespace gc::app
