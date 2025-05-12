#include "app/submodule_journey.h"

#include <algorithm>
#include <string>

#include "git/repository.h"

namespace gc::app {
namespace {

// 代管列表按仓库键查找（键已经过 CanonicalPathKey 折叠，这里只做等值比较，不再猜）。
std::vector<HeldForm>::iterator FindHeld(std::vector<HeldForm>& held, std::wstring_view key) {
  return std::find_if(held.begin(), held.end(),
                      [key](const HeldForm& item) { return item.repositoryKey == key; });
}

std::vector<HeldForm>::const_iterator FindHeld(const std::vector<HeldForm>& held,
                                                std::wstring_view key) {
  return std::find_if(held.begin(), held.end(),
                      [key](const HeldForm& item) { return item.repositoryKey == key; });
}

}  // namespace

DraftSwapDecision PlanDraftSwap(const HeldForm& held, bool screenHasUserContent) {
  if (held.repositoryKey.empty()) {
    return DraftSwapDecision::nothingHeld;
  }
  if (!held.HasAnythingToReturn()) {
    // 当时界面上就没有用户内容，也没有要交还的说明文字：交还没有意义，
    // 更不能因为「代管里有一条记录」就去动现在屏幕上的东西。
    return DraftSwapDecision::heldIsEmpty;
  }
  return screenHasUserContent ? DraftSwapDecision::askBeforeSwap
                              : DraftSwapDecision::restoreDirectly;
}

void SubmoduleJourney::Descent(JourneyHop hop, HeldForm parentDraft) {
  // 同一条父→子不重复压：连点两次「进入」若压出两条来路，返回就只退一层、
  // 界面上会留下一个再也没人认领的草稿。
  const auto duplicate = std::find_if(hops_.begin(), hops_.end(),
                                      [&hop](const JourneyHop& item) {
                                        return item.parentKey == hop.parentKey &&
                                               item.childKey == hop.childKey;
                                      });
  if (duplicate != hops_.end()) {
    // 来路已经在栈上：只把这次收起来的草稿更新过去（内容新的那一份才算数），不再压一层。
    if (parentDraft.HasAnythingToReturn()) {
      Hold(std::move(parentDraft));
    }
    return;
  }
  if (parentDraft.HasAnythingToReturn()) {
    Hold(std::move(parentDraft));
  }
  hops_.push_back(std::move(hop));
}

SubmoduleJourney::ReturnCheck SubmoduleJourney::CheckReturn(
    std::wstring_view currentRoot) const {
  if (hops_.empty()) {
    return ReturnCheck::nothingHeld;
  }
  const JourneyHop& top = hops_.back();
  if (top.childRoot.empty() || currentRoot.empty()) {
    return ReturnCheck::notOnThatChild;  // 任何一边不知道，就不能断言「还在刚才那个子模块里」。
  }
  return git::PathsEqualFolded(top.childRoot, currentRoot) ? ReturnCheck::ok
                                                           : ReturnCheck::notOnThatChild;
}

bool SubmoduleJourney::PopTop(JourneyHop* out) {
  if (out == nullptr || hops_.empty()) {
    return false;
  }
  *out = hops_.back();
  hops_.pop_back();
  return true;
}

void SubmoduleJourney::Hold(HeldForm held) {
  if (held.repositoryKey.empty()) {
    return;  // 不知道属于哪个仓库的内容不能进代管：交还时无法核对落点。
  }
  const auto existing = FindHeld(held_, held.repositoryKey);
  if (existing != held_.end()) {
    *existing = std::move(held);  // 同一个小仓库再收起一次：新的那一份才算数。
    return;
  }
  held_.push_back(std::move(held));
}

bool SubmoduleJourney::Take(std::wstring_view repositoryKey, HeldForm* out) {
  if (out == nullptr || repositoryKey.empty()) {
    return false;
  }
  const auto found = FindHeld(held_, repositoryKey);
  if (found == held_.end()) {
    return false;
  }
  *out = std::move(*found);
  held_.erase(found);
  return true;
}

bool SubmoduleJourney::Peek(std::wstring_view repositoryKey, HeldForm* out) const {
  if (out == nullptr || repositoryKey.empty()) {
    return false;
  }
  const auto found = FindHeld(held_, repositoryKey);
  if (found == held_.end()) {
    return false;
  }
  *out = *found;
  return true;
}

void SubmoduleJourney::ClearAll() {
  hops_.clear();
  held_.clear();
}

std::wstring SubmoduleJourney::DescribeHeld() const {
  if (held_.empty()) {
    return std::wstring();
  }
  std::wstring text = L"目前代管着 " + std::to_wstring(held_.size()) + L" 份表单草稿（按仓库）：";
  for (const HeldForm& held : held_) {
    text += L"\n · " + held.repositoryRoot;
  }
  return text;
}

std::wstring DescribeFormHandback(const HeldForm& returned, bool screenHadUserContent,
                                 DraftSwapDecision decision) {
  switch (decision) {
    case DraftSwapDecision::restoreDirectly:
      return L"表单已交回离开那个仓库时收起来的内容（" + returned.repositoryRoot +
             L"）；界面上原本没有你写的东西，所以没有任何内容被覆盖。时间也回到当时那一份" +
             (returned.timesUserEdited ? L"（那时你已经改过时间）" : L"（当时还没改过时间）") +
             L"。";
    case DraftSwapDecision::askBeforeSwap:
      return L"界面上已经有你新写的东西，代管里也有一份：" + returned.repositoryRoot +
             L"。两份都可能是你要的，程序不替你取舍——按刚才那个框的选择执行。";
    case DraftSwapDecision::heldIsEmpty:
      return L"离开这个仓库时代管的是空内容（当时界面上没有你写的东西），因此表单一个字没动。";
    case DraftSwapDecision::nothingHeld:
    default:
      return screenHadUserContent ? L"没有代管记录：表单里的内容原样留着，程序没有动它。"
                                  : L"没有代管记录，表单也是空的：程序没有动任何东西。";
  }
}

}  // namespace gc::app
