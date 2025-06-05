// 有键列表的行差量计划：头尾公共段保留、中段删旧插新，全程不清空整列。
// 覆盖「整体相同不产生任何删插」「条目换位落在中段」「前缀相同后缀相同而中间不同」
// 「旧列表清空成空」「新列表全新」这几类边界。
#include <string>
#include <vector>

#include "app/list_rebuild.h"
#include "support/tiny_test.h"

namespace {

using gc::app::KeyedRebuildPlan;

std::vector<std::wstring> Keys(std::initializer_list<const wchar_t*> values) {
  std::vector<std::wstring> keys;
  for (const wchar_t* value : values) {
    keys.emplace_back(value);
  }
  return keys;
}

void ExpectPlan(const KeyedRebuildPlan& plan, size_t prefix, size_t suffix, size_t removed,
                size_t inserted, const char* label) {
  GC_CHECK_MESSAGE(plan.commonPrefix == prefix, std::string(label) + " prefix");
  GC_CHECK_MESSAGE(plan.commonSuffix == suffix, std::string(label) + " suffix");
  GC_CHECK_MESSAGE(plan.removedRows == removed, std::string(label) + " removed");
  GC_CHECK_MESSAGE(plan.insertedRows == inserted, std::string(label) + " inserted");
}

GC_TEST(list_rebuild_identical) {
  ExpectPlan(gc::app::PlanKeyedRebuild(Keys({L"a", L"b", L"c"}), Keys({L"a", L"b", L"c"})), 3, 0, 0,
             0, "IDENTICAL");
}

GC_TEST(list_rebuild_empty_old) {
  ExpectPlan(gc::app::PlanKeyedRebuild(Keys({}), Keys({L"a", L"b"})), 0, 0, 0, 2, "EMPTY_OLD");
}

GC_TEST(list_rebuild_empty_new) {
  ExpectPlan(gc::app::PlanKeyedRebuild(Keys({L"a", L"b"}), Keys({})), 0, 0, 2, 0, "EMPTY_NEW");
}

GC_TEST(list_rebuild_replaced_middle) {
  // 只有中间一行换了：前后公共段保留，中段一删一插。
  ExpectPlan(gc::app::PlanKeyedRebuild(Keys({L"a", L"b", L"c", L"d"}),
                                       Keys({L"a", L"x", L"c", L"d"})),
             1, 2, 1, 1, "MIDDLE");
}

GC_TEST(list_rebuild_moved_item) {
  // 条目换位（b 挪到 a 之前）：公共前缀 0、公共后缀 0，整体当作中段替换。
  ExpectPlan(gc::app::PlanKeyedRebuild(Keys({L"a", L"b"}), Keys({L"b", L"a"})), 0, 0, 2, 2,
             "MOVED");
}

GC_TEST(list_rebuild_prefix_suffix_disjoint) {
  // 尾部公共段不许越过开头段：两列表完全相同但长度不同时，suffix 只比剩余部分。
  ExpectPlan(gc::app::PlanKeyedRebuild(Keys({L"a", L"b", L"c"}), Keys({L"a", L"b"})), 2, 0, 1, 0,
             "NO_OVERLAP");
}

GC_TEST(list_rebuild_total_inconsistency_keeps_plan_consistent) {
  // 计划本身自洽：prefix + inserted + suffix 必须等于新行数，
  // prefix + (旧行 - removed) + inserted 的算术在任何删插顺序下都指向同一份新列表。
  const auto check = [](const std::vector<std::wstring>& oldKeys,
                        const std::vector<std::wstring>& newKeys) {
    const KeyedRebuildPlan plan = gc::app::PlanKeyedRebuild(oldKeys, newKeys);
    GC_CHECK_MESSAGE(plan.commonPrefix + plan.insertedRows + plan.commonSuffix == newKeys.size(),
                     "NEW_TOTAL");
    GC_CHECK_MESSAGE(plan.commonPrefix + plan.removedRows + plan.commonSuffix == oldKeys.size(),
                     "OLD_TOTAL");
  };
  check(Keys({L"甲", L"乙", L"丙"}), Keys({L"甲", L"丁", L"丙"}));
  check(Keys({L"a", L"b", L"c", L"d", L"e"}), Keys({L"a", L"c", L"e"}));
  check(Keys({L"a", L"c", L"e"}), Keys({L"a", L"b", L"c", L"d", L"e"}));
}

}  // namespace
