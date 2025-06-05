#include "app/list_rebuild.h"

#include <algorithm>

namespace gc::app {

KeyedRebuildPlan PlanKeyedRebuild(const std::vector<std::wstring>& oldKeys,
                                  const std::vector<std::wstring>& newKeys) {
  KeyedRebuildPlan plan;
  const size_t sharedLimit = std::min(oldKeys.size(), newKeys.size());
  while (plan.commonPrefix < sharedLimit &&
         oldKeys[plan.commonPrefix] == newKeys[plan.commonPrefix]) {
    ++plan.commonPrefix;
  }
  // 尾部公共段只在「不与开头段重叠」的范围内比较：两个列表整体相同时，
  // prefix 已吃掉全部，suffix 必须为 0，否则会把同一批行既算保留又算公共结尾。
  const size_t remainingOld = oldKeys.size() - plan.commonPrefix;
  const size_t remainingNew = newKeys.size() - plan.commonPrefix;
  const size_t suffixLimit = std::min(remainingOld, remainingNew);
  while (plan.commonSuffix < suffixLimit &&
         oldKeys[oldKeys.size() - 1 - plan.commonSuffix] ==
             newKeys[newKeys.size() - 1 - plan.commonSuffix]) {
    ++plan.commonSuffix;
  }
  plan.removedRows = oldKeys.size() - plan.commonPrefix - plan.commonSuffix;
  plan.insertedRows = newKeys.size() - plan.commonPrefix - plan.commonSuffix;
  return plan;
}

}  // namespace gc::app
