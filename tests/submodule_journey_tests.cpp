// 「父仓库 ⇄ 子模块」导航状态保管的纯逻辑测试（不起 Git、不碰窗口）：
//   * 来路栈：进入压一层、返回按 LIFO 退一层、同一条父→子不重复压（连点两次不会留下
//     一个没人认领的草稿），嵌套子模块就是多层；
//   * 返回的准入：只有当前绑定的根与栈顶记下的那个子模块是同一个（折叠斜杠/大小写）才放行，
//     一层都没有、或界面已被人手动换掉时各自有不同的说法，且都不动代管内容；
//   * 草稿代管：空内容不进代管（否则返回时会拿着一条空记录去问「要不要覆盖你现在写的」），
//     同一个仓库再收起一次以新的那份为准，Take 会取消代管而 Peek 不会；
//   * 交还判定四种场合分开：没有代管 / 代管是空的 / 界面空着可以直接交 / 两边都有内容必须先问；
//   * 交还后的说明文字：哪一份去了哪里、有没有东西被覆盖，都说清且不吞字。
#include <string>
#include <utility>

#include "app/submodule_journey.h"
#include "support/tiny_test.h"

namespace {

using gc::app::DraftSwapDecision;
using gc::app::HeldForm;
using gc::app::JourneyHop;
using gc::app::SubmoduleJourney;

constexpr std::wstring_view kParentKey = L"d:/父仓库";
constexpr std::wstring_view kParentRoot = L"D:\\父仓库";
constexpr std::wstring_view kChildKey = L"d:/父仓库/child";
constexpr std::wstring_view kChildRoot = L"D:\\父仓库\\child";

HeldForm Draft(std::wstring_view key, std::wstring_view root, std::wstring_view subject) {
  HeldForm held;
  held.repositoryKey = std::wstring(key);
  held.repositoryRoot = std::wstring(root);
  held.form.subject = std::wstring(subject);
  held.hasUserContent = !subject.empty();
  return held;
}

JourneyHop Hop() {
  JourneyHop hop;
  hop.parentRoot = kParentRoot;
  hop.parentKey = kParentKey;
  hop.submodulePath = L"child";
  hop.childRoot = kChildRoot;
  hop.childKey = kChildKey;
  return hop;
}

bool TextContains(const std::wstring& haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring::npos;
}

}  // namespace

GC_TEST(journey_starts_empty_and_tracks_depth) {
  SubmoduleJourney journey;
  GC_CHECK(journey.empty());
  GC_CHECK(journey.depth() == 0);
  GC_CHECK(journey.top() == nullptr);
  GC_CHECK(journey.DescribeHeld().empty());
  JourneyHop popped;
  GC_CHECK(!journey.PopTop(&popped));
}

GC_TEST(journey_descent_holds_the_parents_draft) {
  SubmoduleJourney journey;
  journey.Descent(Hop(), Draft(kParentKey, kParentRoot, L"父仓库里写到一半的标题"));
  GC_CHECK(!journey.empty());
  GC_CHECK(journey.depth() == 1);
  const JourneyHop* top = journey.top();
  GC_REQUIRE(top != nullptr, "top() 为空");
  GC_CHECK(top->parentKey == kParentKey && top->childKey == kChildKey);
  GC_CHECK(top->submodulePath == L"child");

  HeldForm held;
  GC_CHECK(journey.Peek(kParentKey, &held));
  GC_CHECK(held.form.subject == L"父仓库里写到一半的标题");
  // Peek 不取消代管：交还没发生之前那份内容必须还在原处。
  HeldForm again;
  GC_CHECK(journey.Peek(kParentKey, &again));
}

GC_TEST(journey_empty_draft_is_not_held) {
  SubmoduleJourney journey;
  journey.Descent(Hop(), Draft(kParentKey, kParentRoot, L""));
  HeldForm held;
  GC_CHECK(!journey.Take(kParentKey, &held));
  // 代管表是空的：返回时不会拿一条空记录去问「要不要覆盖你现在写的东西」。
  GC_CHECK(journey.DescribeHeld().empty());
  // 来路照样记下：返回父仓库、核对那三份位置靠的是它，与有没有草稿无关。
  GC_CHECK(!journey.empty());
}

GC_TEST(journey_same_hop_twice_does_not_stack_twice) {
  SubmoduleJourney journey;
  journey.Descent(Hop(), Draft(kParentKey, kParentRoot, L"第一份"));
  journey.Descent(Hop(), Draft(kParentKey, kParentRoot, L"第二份"));
  GC_CHECK_MESSAGE(journey.depth() == 1, "同一条父→子被压了两层");
  HeldForm held;
  GC_CHECK(journey.Take(kParentKey, &held));
  GC_CHECK_MESSAGE(held.form.subject == L"第二份", "该以这次收起来的为准");
}

GC_TEST(journey_nested_submodules_return_one_level_at_a_time) {
  SubmoduleJourney journey;
  JourneyHop outer = Hop();
  journey.Descent(outer, Draft(kParentKey, kParentRoot, L"外层草稿"));
  JourneyHop inner;
  inner.parentRoot = kChildRoot;
  inner.parentKey = kChildKey;
  inner.submodulePath = L"sub/deeper";
  inner.childRoot = std::wstring(kChildRoot) + L"\\sub\\deeper";
  inner.childKey = std::wstring(kChildKey) + L"/sub/deeper";
  journey.Descent(inner, Draft(kChildKey, kChildRoot, L"内层草稿"));
  GC_CHECK(journey.depth() == 2);

  // 返回先退到内层的父仓库（也就是 outer 那个子模块），栈顶随之换人。
  HeldForm innerDraft;
  GC_CHECK(journey.Take(kChildKey, &innerDraft));
  GC_CHECK(innerDraft.form.subject == L"内层草稿");
  JourneyHop popped;
  GC_CHECK(journey.PopTop(&popped));
  GC_CHECK(popped.parentKey == inner.parentKey);
  GC_CHECK(journey.depth() == 1);
  GC_CHECK(journey.top()->childKey == kChildKey);
}

GC_TEST(journey_return_check_names_the_three_cases) {
  SubmoduleJourney journey;
  GC_CHECK(journey.CheckReturn(kChildRoot) == SubmoduleJourney::ReturnCheck::nothingHeld);

  journey.Descent(Hop(), Draft(kParentKey, kParentRoot, L"有内容"));
  GC_CHECK(journey.CheckReturn(kChildRoot) == SubmoduleJourney::ReturnCheck::ok);
  // 折叠比较：斜杠方向与 ASCII 大小写由 git::PathsEqualFolded 负责（与「是不是同一个仓库」
  // 在别处用的判据同一份）。
  GC_CHECK(journey.CheckReturn(L"d:/父仓库/child") == SubmoduleJourney::ReturnCheck::ok);
  GC_CHECK(journey.CheckReturn(L"D:\\父仓库\\other") == SubmoduleJourney::ReturnCheck::notOnThatChild);
  GC_CHECK(journey.CheckReturn(L"") == SubmoduleJourney::ReturnCheck::notOnThatChild);
  // 上面这些拒绝都不动栈与代管内容：代管里那份还是用户的字。
  HeldForm still;
  GC_CHECK(journey.Peek(kParentKey, &still));
  GC_CHECK(journey.depth() == 1);
}

GC_TEST(journey_clear_all_is_explicit_only) {
  SubmoduleJourney journey;
  journey.Descent(Hop(), Draft(kParentKey, kParentRoot, L"会被明说之后才丢"));
  journey.ClearAll();
  GC_CHECK(journey.empty());
  HeldForm held;
  GC_CHECK(!journey.Take(kParentKey, &held));
}

GC_TEST(journey_hold_replaces_same_key_and_lists_roots) {
  SubmoduleJourney journey;
  journey.Hold(Draft(kParentKey, kParentRoot, L"旧的"));
  journey.Hold(Draft(kParentKey, kParentRoot, L"新的"));
  HeldForm held;
  GC_CHECK(journey.Take(kParentKey, &held));
  GC_CHECK(held.form.subject == L"新的");

  journey.Hold(Draft(kChildKey, kChildRoot, L"另一份"));
  journey.Hold(Draft(kParentKey, kParentRoot, L"又一份"));
  const std::wstring described = journey.DescribeHeld();
  GC_CHECK(TextContains(described, L"2 份"));
  GC_CHECK(TextContains(described, std::wstring(kChildRoot)));
  GC_CHECK(TextContains(described, std::wstring(kParentRoot)));
}

GC_TEST(journey_take_requires_a_key_and_removes_the_record) {
  SubmoduleJourney journey;
  journey.Hold(Draft(kParentKey, kParentRoot, L"内容"));
  HeldForm held;
  GC_CHECK(!journey.Take(L"", &held));
  GC_CHECK(journey.Take(kParentKey, &held));
  GC_CHECK(held.form.subject == L"内容");
  GC_CHECK(!journey.Take(kParentKey, &held));  // 交还过一次就不再交第二次（旧内容不该复活）
}

GC_TEST(draft_swap_decision_separates_four_cases) {
  const HeldForm empty = Draft(kParentKey, kParentRoot, L"");
  const HeldForm filled = Draft(kParentKey, kParentRoot, L"有内容");
  HeldForm nothing;
  GC_CHECK(gc::app::PlanDraftSwap(nothing, false) == DraftSwapDecision::nothingHeld);
  GC_CHECK(gc::app::PlanDraftSwap(nothing, true) == DraftSwapDecision::nothingHeld);
  GC_CHECK(gc::app::PlanDraftSwap(empty, false) == DraftSwapDecision::heldIsEmpty);
  GC_CHECK(gc::app::PlanDraftSwap(empty, true) == DraftSwapDecision::heldIsEmpty);
  GC_CHECK(gc::app::PlanDraftSwap(filled, false) == DraftSwapDecision::restoreDirectly);
  GC_CHECK(gc::app::PlanDraftSwap(filled, true) == DraftSwapDecision::askBeforeSwap);
}

GC_TEST(form_handback_wording_says_who_kept_what) {
  const HeldForm filled = Draft(kParentKey, kParentRoot, L"标题");
  const std::wstring direct =
      gc::app::DescribeFormHandback(filled, false, DraftSwapDecision::restoreDirectly);
  GC_CHECK(TextContains(direct, std::wstring(kParentRoot)));
  GC_CHECK(TextContains(direct, L"没有任何内容被覆盖"));
  GC_CHECK(TextContains(direct, L"交回"));

  const std::wstring asked =
      gc::app::DescribeFormHandback(filled, true, DraftSwapDecision::askBeforeSwap);
  GC_CHECK(TextContains(asked, L"两份都"));

  GC_CHECK(TextContains(gc::app::DescribeFormHandback(Draft(kParentKey, kParentRoot, L""), true,
                                                      DraftSwapDecision::heldIsEmpty),
                        L"表单一个字没动") ||
           TextContains(gc::app::DescribeFormHandback(Draft(kParentKey, kParentRoot, L""), true,
                                                      DraftSwapDecision::heldIsEmpty),
                        L"界面上的原样留着"));

  const std::wstring none =
      gc::app::DescribeFormHandback(HeldForm{}, true, DraftSwapDecision::nothingHeld);
  GC_CHECK(TextContains(none, L"原样留着"));
}
