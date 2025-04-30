// app/operation_gate 的纯逻辑测试：五个被编排操作的准入裁决。
// 覆盖：共同前提三条的先后次序与文案、每个操作「自己已在走」的拒绝、
// 既有互斥矩阵（刻意不对称的部分逐格钉住：创建提交不拦人、fetch 不等 pull、
// pull 要等 fetch 等）、以及一切空闲时的放行。
// 这里钉住的是「界面此前逐条写在各入口里的拒绝说明」——改动任何一句都必须先想清楚
// 那句长期承诺有没有变，不能顺手润色。
#include <string>

#include "app/operation_gate.h"
#include "support/tiny_test.h"

namespace {

using gc::app::AdmitWriteFlow;
using gc::app::DescribeWritePrerequisiteRefusal;
using gc::app::GitFlow;
using gc::app::GitFlowActivity;
using gc::app::WritePrerequisites;

// 一切就绪的前提。
WritePrerequisites Ready() {
  WritePrerequisites prereq;
  prereq.gitUsable = true;
  prereq.repoUsable = true;
  prereq.commandWindowBusy = false;
  prereq.workspaceStillBound = true;
  return prereq;
}

GitFlowActivity NoneActive() {
  return GitFlowActivity{};
}

GC_TEST(operation_gate_admits_every_flow_when_idle) {
  const GitFlowActivity idle = NoneActive();
  for (const GitFlow flow : {GitFlow::commit, GitFlow::undo, GitFlow::fetch, GitFlow::pull,
                             GitFlow::push}) {
    GC_CHECK_MESSAGE(AdmitWriteFlow(flow, idle, Ready(), L"操作").empty(),
                     "空闲且前提齐备时必须放行");
  }
}

GC_TEST(operation_gate_prerequisites_come_first_and_in_order) {
  GitFlowActivity idle = NoneActive();

  WritePrerequisites noGit = Ready();
  noGit.gitUsable = false;
  noGit.commandWindowBusy = true;  // 同时占用时，先报「Git 或仓库不可用」。
  GC_CHECK_MESSAGE(AdmitWriteFlow(GitFlow::push, idle, noGit, L"推送") ==
                       L"Git 或仓库当前不可用，无法推送。请先确认路径并点“刷新”。",
                   "前提第一条必须压过命令窗口占用");

  WritePrerequisites busy = Ready();
  busy.commandWindowBusy = true;
  busy.workspaceStillBound = false;  // 同时不一致时，先报命令窗口占用。
  GC_CHECK_MESSAGE(AdmitWriteFlow(GitFlow::fetch, idle, busy, L"fetch") ==
                       L"已有一个命令窗口操作在进行，请等它结束后再fetch。",
                   "命令窗口单槽：与既有文案逐字一致");

  WritePrerequisites moved = Ready();
  moved.workspaceStillBound = false;
  GC_CHECK_MESSAGE(AdmitWriteFlow(GitFlow::undo, idle, moved, L"撤回最近提交") ==
                       L"仓库工作区已改变，请先点“刷新”再撤回最近提交。",
                   "工作区与协调器身份不一致时必须拒绝");

  // 前提判定与流程无关：暂存/查看类用的就是这一层。
  GC_CHECK(DescribeWritePrerequisiteRefusal(Ready(), L"暂存").empty());
}

GC_TEST(operation_gate_self_busy_notes_are_verbatim) {
  GitFlowActivity busyCommit = NoneActive();
  busyCommit.commit = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::commit, busyCommit, Ready(), L"创建提交") ==
           L"已经有一次“创建提交”正在核对仓库现状（或正在做执行前复核），请等它的确认框出现，"
           L"或先取消那一次。");

  GitFlowActivity busyUndo = NoneActive();
  busyUndo.undo = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::undo, busyUndo, Ready(), L"撤回最近提交") ==
           L"已经有一次撤回预检在跑，请等确认框出现，或先取消那一次。");

  GitFlowActivity busyFetch = NoneActive();
  busyFetch.fetch = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::fetch, busyFetch, Ready(), L"fetch") ==
           L"已经有一次 fetch 目标预检在跑，请等它的界面出现，再点不会排队。");

  GitFlowActivity busyPull = NoneActive();
  busyPull.pull = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::pull, busyPull, Ready(), L"pull") ==
           L"已经有一次 pull 在走流程（预检、获取或整合），请等它结束或先取消那一步。");

  GitFlowActivity busyPush = NoneActive();
  busyPush.push = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::push, busyPush, Ready(), L"推送") ==
           L"已经有一次推送在走流程（预检、复核或核实），请等它结束或先取消那一步。");
}

GC_TEST(operation_gate_cross_refusals_match_the_existing_matrix) {
  // 撤回：只等创建提交。
  GitFlowActivity commitBusy = NoneActive();
  commitBusy.commit = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::undo, commitBusy, Ready(), L"撤回最近提交") ==
           L"「创建提交」正在核对仓库现状（或正在做执行前复核），请先等那一步结束，再来撤回。");

  // fetch：等 push、undo、commit；不等 pull（pull 的命令步另有单槽裁决，只读预检不同层）。
  GitFlowActivity pushBusy = NoneActive();
  pushBusy.push = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::fetch, pushBusy, Ready(), L"fetch") ==
           L"「推送」还在走它的预检、复核或核实，请先让那一步结束，再来 fetch。");
  GitFlowActivity undoBusy = NoneActive();
  undoBusy.undo = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::fetch, undoBusy, Ready(), L"fetch") ==
           L"「撤回最近提交」的预检还在跑，请先等它的确认框出现，再来 fetch。");
  GitFlowActivity pullBusy = NoneActive();
  pullBusy.pull = true;
  GC_CHECK_MESSAGE(AdmitWriteFlow(GitFlow::fetch, pullBusy, Ready(), L"fetch").empty(),
                   "fetch 不等 pull：这是既有界面的不对称，改动需要明确决定");

  // pull：等 push、fetch、undo、commit。
  GC_CHECK(AdmitWriteFlow(GitFlow::pull, pushBusy, Ready(), L"pull") ==
           L"「推送」还在走它的预检、复核或核实，请先让那一步结束，再来 pull。");
  GitFlowActivity fetchBusy = NoneActive();
  fetchBusy.fetch = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::pull, fetchBusy, Ready(), L"pull") ==
           L"「fetch」的目标预检还在跑，请先等它的界面出现，再来 pull。");
  GC_CHECK(AdmitWriteFlow(GitFlow::pull, undoBusy, Ready(), L"pull") ==
           L"「撤回最近提交」的预检还在跑，请先等它的确认框出现，再来 pull。");

  // push：等 pull、fetch、undo、commit。
  GC_CHECK(AdmitWriteFlow(GitFlow::push, pullBusy, Ready(), L"推送") ==
           L"「pull」还在走它的预检、获取或整合，请先让那一步结束，再来推送。");
  GC_CHECK(AdmitWriteFlow(GitFlow::push, fetchBusy, Ready(), L"推送") ==
           L"「fetch」的目标预检还在跑，请先等它的界面出现，再来推送。");

  // 创建提交不拦在任何其它流程之后（既有行为：它的只读核对不与别人抢索引之外的东西，
  // 真要发命令时另有命令窗口单槽兜底）。
  GitFlowActivity everythingElse = NoneActive();
  everythingElse.undo = true;
  everythingElse.fetch = true;
  everythingElse.pull = true;
  everythingElse.push = true;
  GC_CHECK_MESSAGE(AdmitWriteFlow(GitFlow::commit, everythingElse, Ready(), L"创建提交").empty(),
                   "创建提交的准入只看自己：这是既有行为，不得顺手收紧");

  // 多个占路者同时在等时，报第一个——次序也钉住（与既有界面的检查顺序一致）。
  GitFlowActivity several = NoneActive();
  several.commit = true;
  several.push = true;
  several.undo = true;
  GC_CHECK(AdmitWriteFlow(GitFlow::fetch, several, Ready(), L"fetch") ==
           L"「推送」还在走它的预检、复核或核实，请先让那一步结束，再来 fetch。");
}

}  // namespace
