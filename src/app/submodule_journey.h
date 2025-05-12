#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_message.h"  // CommitFormData（表单内容本身的数据类型）
#include "git/commit_date.h"     // CivilTime：两个时间控件的墙上时间

namespace gc::app {

// 「父仓库 ⇄ 子模块」导航的状态保管（纯逻辑，不碰 HWND，因此可整条测试）。
//
// 为什么需要它：这个程序的界面一次只绑一个仓库，而「进入子模块」就是把绑定整个换掉。
// 换绑定的既有链路（识别→绑定→清空旧列表→重读→作者默认值重查）本身是对的，但它对表单
// 只做一件事：问一句「保留还是放弃」。用在导航上会出两种坏结果——
//   * 父仓库那份没提交的草稿被留在屏上，成了子模块表单的内容（串仓库）；
//   * 或者被当成「放弃」丢掉（误清用户写的字）。
// 所以导航要自己把「来路」与「当时界面上那份草稿」都收起来，回到那个仓库时原样交还，
// 并且交还之前先核对「现在这个仓库是不是当初那个」——键就是工作区根（与表单会话同一判据，
// 换 Git 程序不算换仓库）。任何一步都不静默删内容：只有用户明确选择放弃才丢。
//
// 嵌套子模块天然就是再多压一层：返回是逐层返回，每层各管自己那一份草稿。

// 一份被代管的表单内容（连同它属于哪个仓库）。
struct HeldForm {
  std::wstring repositoryKey;  // CanonicalPathKey(工作区根)：交还时先核对回到的是不是同一个仓库
  std::wstring repositoryRoot;
  git::CommitFormData form;
  git::CivilTime authorWall{};
  git::CivilTime committerWall{};
  bool timesSynced = false;
  bool timesUserEdited = false;
  bool hasUserContent = false;  // 收起时界面上到底有没有用户写的东西

  [[nodiscard]] bool HasAnythingToReturn() const noexcept { return hasUserContent; }
};

// 一级来路：从哪个父仓库的哪条 gitlink 进到哪个子模块。草稿一律存在代管表里（按仓库键），
// 这里只记身份——同一份内容存两处就会出现「交还了旧的那一份、还留着新的那一份」。
struct JourneyHop {
  std::wstring parentRoot;
  std::wstring parentKey;
  std::wstring submodulePath;  // 父仓库里那条记录的相对路径（返回后核指针就问这一条）
  std::wstring childRoot;      // 进入时 Git 自己答出的子模块工作区根
  std::wstring childKey;
};

// 交还草稿之前，界面上可能已经写着别的东西（用户在子模块里打了一半）。
// 判定只给结论，取舍由界面问用户——程序无权在「保留」与「丢掉」之间替人决定。
enum class DraftSwapDecision {
  nothingHeld = 0,  // 代管里没有这一份：什么都不动
  heldIsEmpty,      // 代管的那份本来就是空的：交还没有意义，界面上的原样留着
  restoreDirectly,  // 界面上没有用户内容：直接交还
  askBeforeSwap,    // 界面上有用户内容：必须先问，两个都不能悄悄覆盖另一个
};

[[nodiscard]] DraftSwapDecision PlanDraftSwap(const HeldForm& held, bool screenHasUserContent);

class SubmoduleJourney {
public:
  // 进入一层：压进来路，并把父仓库那份草稿收进代管。空内容不进代管表——
  // 否则返回时会对着一个空代管记录去问「要不要覆盖你现在写的东西」，那是白问。
  void Descent(JourneyHop hop, HeldForm parentDraft);

  [[nodiscard]] bool empty() const noexcept { return hops_.empty(); }
  [[nodiscard]] size_t depth() const noexcept { return hops_.size(); }
  [[nodiscard]] const JourneyHop* top() const noexcept {
    return hops_.empty() ? nullptr : &hops_.back();
  }

  enum class ReturnCheck {
    ok = 0,          // 当前绑定的就是栈顶记下的那个子模块：可以返回
    nothingHeld,     // 一层都没有：没有「父仓库」可回
    notOnThatChild,  // 界面已经不在记下的那个子模块里了（被人手动换了仓库或目录挪过）
  };
  // 返回的准入。判据只有 Git 答过的那两个根，不猜：把父仓库草稿交回一个不相干的仓库，
  // 比不返回更糟。
  [[nodiscard]] ReturnCheck CheckReturn(std::wstring_view currentRoot) const;
  // 弹出栈顶（只在 CheckReturn() 返回 ok 之后调用；其余场合调用它拿到的也是 false）。
  [[nodiscard]] bool PopTop(JourneyHop* out);

  // ---- 草稿代管 ----
  void Hold(HeldForm held);
  // 取出并取消代管（交还成功之后不留旧副本，免得下次再交还一次过期内容）。
  [[nodiscard]] bool Take(std::wstring_view repositoryKey, HeldForm* out);
  [[nodiscard]] bool Peek(std::wstring_view repositoryKey, HeldForm* out) const;
  // 只有一层都不剩、而且每一句都当面说清了之后才整体作废：本类的任何导航步骤都不调用它。
  void ClearAll();

  // 代管里现在有哪几个仓库的草稿（写进状态栏用，不含表单正文，避免把用户写的长文摊在屏上）。
  [[nodiscard]] std::wstring DescribeHeld() const;

private:
  std::vector<JourneyHop> hops_;
  std::vector<HeldForm> held_;
};

// 返回父仓库之后要摆给用户的那句总结：导航做了什么、现在三份位置是什么关系、下一步由谁做。
// 这里只组装与「代管/交还」有关的两句；位置那份由 git/submodule_navigation 的判读给。
[[nodiscard]] std::wstring DescribeFormHandback(const HeldForm& returned,
                                                bool screenHadUserContent,
                                                DraftSwapDecision decision);

}  // namespace gc::app
