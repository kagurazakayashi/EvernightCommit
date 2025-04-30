#include "app/operation_gate.h"

#include <cstddef>

namespace gc::app {
namespace {

// 各操作在拒绝说明里的称呼：作为被拦的一方出现在句尾时，沿用界面上既有的写法
// （fetch/pull 带一个空格是英文词与中文之间的既有排版，撤回/推送用中文名）。
constexpr std::wstring_view kTailFor[] = {
    /*commit*/ L"提交",
    /*undo  */ L"撤回",
    /*fetch */ L" fetch",
    /*pull  */ L" pull",
    /*push  */ L"推送",
};

// 「自己已经在走」的说明：各操作原本就有自己的措辞，逐字保留。
std::wstring SelfBusyNote(GitFlow requested) {
  switch (requested) {
    case GitFlow::commit:
      return L"已经有一次“创建提交”正在核对仓库现状（或正在做执行前复核），请等它的确认框出现，"
             L"或先取消那一次。";
    case GitFlow::undo:
      return L"已经有一次撤回预检在跑，请等确认框出现，或先取消那一次。";
    case GitFlow::fetch:
      return L"已经有一次 fetch 目标预检在跑，请等它的界面出现，再点不会排队。";
    case GitFlow::pull:
      return L"已经有一次 pull 在走流程（预检、获取或整合），请等它结束或先取消那一步。";
    case GitFlow::push:
      return L"已经有一次推送在走流程（预检、复核或核实），请等它结束或先取消那一步。";
  }
  return {};
}

// 「别的操作正占路」的说明前半句：点名占路的一方与它此刻在做的阶段。
std::wstring_view BlockerPhrase(GitFlow blocker) {
  switch (blocker) {
    case GitFlow::commit:
      return L"「创建提交」正在核对仓库现状（或正在做执行前复核），请先等那一步结束，再来";
    case GitFlow::undo:
      return L"「撤回最近提交」的预检还在跑，请先等它的确认框出现，再来";
    case GitFlow::fetch:
      return L"「fetch」的目标预检还在跑，请先等它的界面出现，再来";
    case GitFlow::pull:
      return L"「pull」还在走它的预检、获取或整合，请先让那一步结束，再来";
    case GitFlow::push:
      return L"「推送」还在走它的预检、复核或核实，请先让那一步结束，再来";
  }
  return {};
}

bool Active(const GitFlowActivity& activity, GitFlow flow) {
  switch (flow) {
    case GitFlow::commit:
      return activity.commit;
    case GitFlow::undo:
      return activity.undo;
    case GitFlow::fetch:
      return activity.fetch;
    case GitFlow::pull:
      return activity.pull;
    case GitFlow::push:
      return activity.push;
  }
  return false;
}

// 每个入口原本要拦的占路者各不相同（见函数头的说明），检查次序也逐字沿用既有界面：
// 这张表改变任何一格，都会改变用户看到的拒绝理由，必须先想清楚再动。
struct BlockRule {
  GitFlow blocker;
};

const BlockRule* RulesFor(GitFlow requested, size_t* count) {
  // commit：只等自己——它的确认框与复核都是只读查询，不与其他流程抢索引之外的东西；
  // 真要发命令时另有命令窗口单槽裁决兜底。
  // undo：等 commit。
  static constexpr BlockRule kUndo[] = {{GitFlow::commit}};
  // fetch：等 push、undo、commit；不等 pull（pull 自己的抓取那一步占命令窗口槽位，
  // 与这里的只读预检不同层）。
  static constexpr BlockRule kFetch[] = {{GitFlow::push}, {GitFlow::undo}, {GitFlow::commit}};
  static constexpr BlockRule kPull[] = {{GitFlow::push}, {GitFlow::fetch}, {GitFlow::undo},
                                        {GitFlow::commit}};
  static constexpr BlockRule kPush[] = {{GitFlow::pull}, {GitFlow::fetch}, {GitFlow::undo},
                                        {GitFlow::commit}};
  switch (requested) {
    case GitFlow::commit:
      *count = 0;
      return nullptr;
    case GitFlow::undo:
      *count = std::size(kUndo);
      return kUndo;
    case GitFlow::fetch:
      *count = std::size(kFetch);
      return kFetch;
    case GitFlow::pull:
      *count = std::size(kPull);
      return kPull;
    case GitFlow::push:
      *count = std::size(kPush);
      return kPush;
  }
  *count = 0;
  return nullptr;
}

}  // namespace

std::wstring DescribeWritePrerequisiteRefusal(const WritePrerequisites& prerequisites,
                                              std::wstring_view actionLabel) {
  if (!prerequisites.gitUsable || !prerequisites.repoUsable) {
    return L"Git 或仓库当前不可用，无法" + std::wstring(actionLabel) + L"。请先确认路径并点“刷新”。";
  }
  if (prerequisites.commandWindowBusy) {
    // 命令窗口操作是单槽的：并发两次写操作会互相抢 index.lock，
    // 也会让“哪个窗口对应哪一次改动”变得含糊。
    return L"已有一个命令窗口操作在进行，请等它结束后再" + std::wstring(actionLabel) + L"。";
  }
  if (!prerequisites.workspaceStillBound) {
    // 列表里的路径是“那一个工作区”的相对路径：根目录与协调器身份一旦不一致，
    // 这些路径就可能属于上一个仓库，绝不能拿去对新仓库执行写操作。
    return L"仓库工作区已改变，请先点“刷新”再" + std::wstring(actionLabel) + L"。";
  }
  return {};
}

std::wstring AdmitWriteFlow(GitFlow requested, const GitFlowActivity& activity,
                            const WritePrerequisites& prerequisites, std::wstring_view actionLabel) {
  const std::wstring prerequisiteRefusal = DescribeWritePrerequisiteRefusal(prerequisites, actionLabel);
  if (!prerequisiteRefusal.empty()) {
    return prerequisiteRefusal;
  }
  if (Active(activity, requested)) {
    return SelfBusyNote(requested);
  }
  size_t count = 0;
  const BlockRule* rules = RulesFor(requested, &count);
  for (size_t index = 0; index < count; ++index) {
    if (Active(activity, rules[index].blocker)) {
      return std::wstring(BlockerPhrase(rules[index].blocker)) + std::wstring(kTailFor[static_cast<int>(requested)]) +
             L"。";
    }
  }
  return {};
}

}  // namespace gc::app
