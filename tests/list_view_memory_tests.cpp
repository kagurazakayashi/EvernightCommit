// 刷新时列表选择状态的保留逻辑：按条目身份（相对路径）记忆，就地更新后再映射回行号。
// 覆盖「同一份变化被排到别的行」「条目已经消失」「没有记忆时不该顺手选中第一行」。
#include <string>
#include <vector>

#include "app/list_view_memory.h"
#include "git/workspace_model.h"
#include "support/tiny_test.h"

namespace {

using gc::app::ListViewMemory;
using gc::app::RestoredListView;
using gc::git::ChangeItem;
using gc::git::ChangeKind;

ChangeItem Item(ChangeKind kind, const std::wstring& path) {
  ChangeItem item;
  item.kind = kind;
  item.path = path;
  return item;
}

std::vector<std::wstring> Paths(const std::vector<int>& rows, const std::vector<ChangeItem>& items) {
  std::vector<std::wstring> paths;
  for (const int row : rows) {
    paths.push_back(items[static_cast<size_t>(row)].path);
  }
  return paths;
}

bool Contains(const std::vector<std::wstring>& values, std::wstring_view wanted) {
  for (const std::wstring& value : values) {
    if (value == wanted) {
      return true;
    }
  }
  return false;
}

}  // namespace

GC_TEST(list_view_memory_maps_selection_to_new_rows) {
  // 刷新前：第 0、1、2 行；用户选中 a.txt 与 c.txt。
  const std::vector<ChangeItem> before{Item(ChangeKind::modified, L"a.txt"), Item(ChangeKind::modified, L"b.txt"),
                                       Item(ChangeKind::untracked, L"c.txt")};
  ListViewMemory memory;
  memory.selectedKeys = {before[0].path, before[2].path};

  // 刷新后：Git 的顺序变了（c.txt 之前多了一个新条目，a.txt 被排到后面），
  // 行号不能再被当成同一个条目。
  const std::vector<ChangeItem> after{Item(ChangeKind::untracked, L"c.txt"), Item(ChangeKind::modified, L"a.txt"),
                                      Item(ChangeKind::modified, L"b.txt"), Item(ChangeKind::deleted, L"d.txt")};
  const RestoredListView restored = gc::app::MapListViewMemory(after, memory);
  GC_CHECK(restored.selectedRows.size() == 2);
  const std::vector<std::wstring> selected = Paths(restored.selectedRows, after);
  GC_CHECK_MESSAGE(Contains(selected, L"a.txt") && Contains(selected, L"c.txt"), "选中的还是原来那两个文件");
}

GC_TEST(list_view_memory_drops_vanished_items_only) {
  ListViewMemory memory;
  memory.selectedKeys = {L"gone.txt", L"still.here/deep.txt", L"also.gone.txt"};

  const std::vector<ChangeItem> after{Item(ChangeKind::modified, L"first.txt"),
                                      Item(ChangeKind::modified, L"still.here/deep.txt")};
  const RestoredListView restored = gc::app::MapListViewMemory(after, memory);
  // 被提交掉、被外部删掉的条目不再出现在列表里，也不会凭空把选中状态挪到别的行。
  GC_CHECK(restored.selectedRows.size() == 1);
  GC_CHECK(Paths(restored.selectedRows, after)[0] == L"still.here/deep.txt");
}

GC_TEST(list_view_memory_moves_when_one_row_is_removed_above) {
  // 真实场景：上方某个文件被暂存以后离开了未暂存列表，下面的行整体上移一行。
  std::vector<ChangeItem> before;
  for (int index = 1; index <= 5; ++index) {
    before.push_back(Item(ChangeKind::modified, L"file0" + std::to_wstring(index) + L".txt"));
  }
  ListViewMemory memory;
  memory.selectedKeys = {before[4].path};

  std::vector<ChangeItem> after{before[0], before[2], before[3], before[4]};
  const RestoredListView restored = gc::app::MapListViewMemory(after, memory);
  GC_CHECK(restored.selectedRows.size() == 1);
  GC_CHECK(restored.selectedRows[0] == 3);
}

GC_TEST(list_view_memory_keeps_non_ascii_and_space_paths_verbatim) {
  ListViewMemory memory;
  memory.selectedKeys = {L"文档 目录/改 名 后.txt", L"a&b^c.txt"};
  const std::vector<ChangeItem> after{Item(ChangeKind::renamed, L"a&b^c.txt"),
                                      Item(ChangeKind::modified, L"文档 目录/改 名 后.txt")};
  const RestoredListView restored = gc::app::MapListViewMemory(after, memory);
  GC_CHECK(restored.selectedRows.size() == 2);
  const std::vector<std::wstring> selected = Paths(restored.selectedRows, after);
  GC_CHECK(Contains(selected, L"a&b^c.txt"));
  GC_CHECK(Contains(selected, L"文档 目录/改 名 后.txt"));
}

GC_TEST(list_view_memory_without_state_selects_nothing) {
  const std::vector<ChangeItem> after{Item(ChangeKind::modified, L"a.txt")};
  const RestoredListView restored = gc::app::MapListViewMemory(after, ListViewMemory{});
  // 没有记忆时绝不“顺手”选中第一行：那是凭空替用户做的选择。
  GC_CHECK(restored.selectedRows.empty());
}

GC_TEST(list_view_key_is_the_repository_relative_path) {
  // 重命名条目按“新路径”记忆：显示文本是「旧 → 新」，但操作与身份都用原始路径字段。
  ChangeItem renamed;
  renamed.kind = ChangeKind::renamed;
  renamed.path = L"nested/kept.txt";
  renamed.oldPath = L"nested/keep.txt";
  GC_CHECK(gc::app::ViewKeyForItem(renamed) == L"nested/kept.txt");
}
