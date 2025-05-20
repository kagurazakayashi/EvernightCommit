// 上游写入那一段的控制器接线测试（windows 组：链接 UI 层，不起子进程、不碰文件系统、不启动 Git）。
// 钉的是 R1 的那条链：已审查的参数数组必须**原样**到达宿主收到的操作对象，仓库根必须是这次推送
// 绑定的那一个（不是界面此刻的），两条命令按顺序发、第一条失败时第二条一次都不发。
// 纯计划测试（first_push_plan_tests.cpp）只断言 BuildUpstreamWriteSteps 的返回值，看不见这段接线。
#include <string>
#include <vector>

#include "git/first_push_plan.h"
#include "git/repository.h"
#include "support/fake_operation_host.h"
#include "support/tiny_test.h"
#include "ui/upstream_write_flow.h"

namespace {

using gc::git::BuildUpstreamWriteSteps;
using gc::git::CommandWindowOperation;
using gc::git::RepoDetection;
using gc::git::UpstreamWriteStep;
using gc::test::FakeOperationHost;
using gc::ui::OperationContext;
using gc::ui::UpstreamWriteFlow;

constexpr wchar_t kBoundRoot[] = L"P:\\推送绑定的\\repo";
constexpr wchar_t kBoundExe[] = L"C:\\Program Files\\Git\\bin\\git.exe";

// 界面此刻的仓库与 Git 程序**故意**与绑定值不同：接线若从界面重猜，断言就会失败。
OperationContext MakeContext(std::wstring currentRoot) {
  OperationContext ctx;
  ctx.notifyWindow = nullptr;
  ctx.gitExecutable = L"C:\\other\\git.exe";
  ctx.detection = RepoDetection{};
  ctx.detection.root = std::move(currentRoot);
  ctx.repoUsable = true;
  return ctx;
}

UpstreamWriteFlow::Plan MakePlan(std::vector<UpstreamWriteStep> steps) {
  UpstreamWriteFlow::Plan plan;
  plan.steps = std::move(steps);
  plan.branchName = L"main";
  plan.gitExecutable = kBoundExe;
  plan.repositoryDirectory = kBoundRoot;
  plan.pushConclusion = L"退出码 0";
  return plan;
}

std::vector<UpstreamWriteStep> NormalSteps() {
  return BuildUpstreamWriteSteps(L"main", L"origin", L"refs/heads/main");
}

bool ArgumentsEqual(const std::vector<std::wstring>& actual,
                    const std::vector<std::wstring>& expected) {
  return actual == expected;
}

}  // namespace

GC_TEST(upstream_write_flow_delivers_reviewed_arguments_to_host) {
  FakeOperationHost host;
  UpstreamWriteFlow flow;
  const std::vector<UpstreamWriteStep> steps = NormalSteps();
  GC_REQUIRE(steps.size() == 2, "两条上游步骤应生成成功");
  // 界面此刻在另一个仓库：绑定值必须压过它。
  OperationContext ctx = MakeContext(L"P:\\界面切到的\\other");

  GC_CHECK(flow.Begin(host, ctx, MakePlan(steps)), "第一条应当场发出");
  GC_REQUIRE(host.launches.size() == 1, "Begin 之后恰好发出一条");
  const CommandWindowOperation first = host.launches[0].operation;  // 取副本：后面还会往同一个 vector 里追加
  GC_CHECK(ArgumentsEqual(first.arguments, steps[0].arguments));
  GC_CHECK(first.operationId == steps[0].operationId);
  GC_CHECK(first.displayName == steps[0].displayName);
  GC_CHECK(first.gitExecutable == std::wstring(kBoundExe));
  GC_CHECK(first.repositoryDirectory == std::wstring(kBoundRoot));
  GC_CHECK(host.launches[0].options.upstreamWriteStep == 1);
  GC_CHECK(host.launches[0].options.scopeNotice.find(steps[0].key) != std::wstring::npos);
  GC_CHECK(host.launches[0].options.scopeNotice.find(steps[0].value) != std::wstring::npos);
  // 上游写入不是另一次「推送」，不重复落一条操作历史，也不借用推送的核实判定。
  GC_CHECK(!host.launches[0].options.history.record);
  GC_CHECK(!host.launches[0].options.pushOperation);

  // 第一条成功 → 才发第二条；参数、ID、仓库、序号都要是第二条自己的。
  ctx.detection.root = std::wstring(kBoundRoot);  // 回到绑定的那个仓库
  GC_CHECK(flow.OnStepSettled(host, ctx, 1, true, L"退出码 0"), "第二条发出后仍在等终态");
  GC_REQUIRE(host.launches.size() == 2, "第一条成功后才发第二条");
  const CommandWindowOperation second = host.launches[1].operation;
  GC_CHECK(ArgumentsEqual(second.arguments, steps[1].arguments));
  GC_CHECK(second.operationId == steps[1].operationId);
  GC_CHECK(second.repositoryDirectory == std::wstring(kBoundRoot));
  GC_CHECK(second.gitExecutable == std::wstring(kBoundExe));
  GC_CHECK(host.launches[1].options.upstreamWriteStep == 2);
  GC_CHECK(host.launches[1].options.scopeNotice.find(steps[1].key) != std::wstring::npos);
  // 顺序：第一把键在前、第二把在后（Git 自己设上游也是这个顺序）。
  GC_CHECK(ArgumentsEqual(first.arguments, std::vector<std::wstring>{L"config", L"branch.main.remote",
                                                                    L"origin"}));
  GC_CHECK(ArgumentsEqual(second.arguments,
                          std::vector<std::wstring>{L"config", L"branch.main.merge",
                                                    L"refs/heads/main"}));

  // 第二条成功 → 总结把「推送」与「两条配置」分开列，且这一段不再活跃。
  GC_CHECK(!flow.OnStepSettled(host, ctx, 2, true, L"退出码 0"), "两条都写成后应结案");
  GC_CHECK(!flow.Active());
  const std::wstring summary = gc::test::LastStatus(host);
  GC_CHECK(summary.find(L"推送那一步的结论") != std::wstring::npos);
  GC_CHECK(summary.find(L"都已写成") != std::wstring::npos);
  GC_CHECK(summary.find(steps[0].conclusion) != std::wstring::npos);
  GC_CHECK(summary.find(steps[1].conclusion) != std::wstring::npos);
  GC_REQUIRE(host.launches.size() == 2, "两条就是两条，不多发");
}

GC_TEST(upstream_write_flow_first_failure_never_launches_second) {
  FakeOperationHost host;
  UpstreamWriteFlow flow;
  const std::vector<UpstreamWriteStep> steps = NormalSteps();
  GC_REQUIRE(steps.size() == 2, "两条上游步骤应生成成功");
  OperationContext ctx = MakeContext(std::wstring(kBoundRoot));

  GC_CHECK(flow.Begin(host, ctx, MakePlan(steps)));
  GC_CHECK(!flow.OnStepSettled(host, ctx, 1, false, L"退出码 1"), "第一条失败即结案");
  GC_REQUIRE(host.launches.size() == 1, "第一条失败后第二条一次都不发");
  GC_CHECK(!flow.Active());
  GC_REQUIRE(host.warnings.size() == 1, "失败要当面说，不能只挤在状态栏");
  const std::wstring text = host.warnings[0].second;
  GC_CHECK(text.find(L"第 1 条没有写成") != std::wstring::npos);
  GC_CHECK(text.find(L"剩下的 1 条没有发出") != std::wstring::npos);
  // 已成的那部分不能因为后一步失败被说成「什么都没做」。
  GC_CHECK(text.find(L"推送那一步") != std::wstring::npos);
  GC_CHECK(text.find(L"退出码 0") != std::wstring::npos);
}

GC_TEST(upstream_write_flow_stops_when_repository_switched) {
  FakeOperationHost host;
  UpstreamWriteFlow flow;
  OperationContext ctx = MakeContext(std::wstring(kBoundRoot));
  GC_CHECK(flow.Begin(host, ctx, MakePlan(NormalSteps())));

  // 确认之后界面切到了另一个仓库：剩下的配置命令绝不能发给那个仓库。
  ctx.detection.root = L"P:\\已经切走的\\other-repo";
  GC_CHECK(!flow.OnStepSettled(host, ctx, 1, true, L"退出码 0"), "换仓库后应结案");
  GC_REQUIRE(host.launches.size() == 1, "换仓库后不再发第二条");
  const std::wstring status = gc::test::LastStatus(host);
  GC_CHECK(status.find(L"界面上的仓库已经换掉") != std::wstring::npos);
  GC_CHECK(status.find(L"不自动回退") != std::wstring::npos);
}

GC_TEST(upstream_write_flow_refuses_argument_less_step) {
  FakeOperationHost host;
  UpstreamWriteFlow flow;
  std::vector<UpstreamWriteStep> steps = NormalSteps();
  GC_REQUIRE(steps.size() == 2, "两条上游步骤应生成成功");
  steps[0].arguments.clear();  // 模拟「方案生成了参数，装配时漏传」的那一类接线缺陷
  OperationContext ctx = MakeContext(std::wstring(kBoundRoot));

  GC_CHECK(!flow.Begin(host, ctx, MakePlan(steps)), "参数表为空时一条都不发");
  GC_CHECK(host.launches.empty(), "绝不能把裸 git 发出去");
  const std::wstring status = gc::test::LastStatus(host);
  GC_CHECK(status.find(L"参数表是空的") != std::wstring::npos);
  GC_CHECK(!flow.Active());
}

GC_TEST(upstream_write_flow_launch_failure_settles_without_retry) {
  FakeOperationHost host;
  host.launchResults = {false};
  UpstreamWriteFlow flow;
  OperationContext ctx = MakeContext(std::wstring(kBoundRoot));

  GC_CHECK(!flow.Begin(host, ctx, MakePlan(NormalSteps())), "第一步就没启动成功");
  GC_REQUIRE(host.launches.size() == 1, "启动失败不重试：只尝试过一次");
  GC_CHECK(!flow.Active());
  const std::wstring status = gc::test::LastStatus(host);
  GC_CHECK(status.find(L"没能启动") != std::wstring::npos);
  GC_CHECK(status.find(L"不自动") == std::wstring::npos || status.find(L"没有改动") != std::wstring::npos);

  // 第二段也没法靠终态往下推：迟到的终态被拒。
  GC_CHECK(!flow.OnStepSettled(host, ctx, 1, true, L"退出码 0"));
}

GC_TEST(upstream_write_flow_ignores_out_of_order_and_late_terminals) {
  FakeOperationHost host;
  UpstreamWriteFlow flow;
  OperationContext ctx = MakeContext(std::wstring(kBoundRoot));
  GC_CHECK(flow.Begin(host, ctx, MakePlan(NormalSteps())));

  // 等的是第 1 条：第 2 条的终态（上一轮残留或界面重复投递）不推进、也不重发。
  GC_CHECK(!flow.OnStepSettled(host, ctx, 2, true, L"退出码 0"));
  GC_REQUIRE(host.launches.size() == 1, "错序终态不得触发第二条");
  // 第 1 条真正的终态：成功 → 发第 2 条（仍在等），此时错序的那份终态已经不会再重复推进。
  GC_CHECK(flow.OnStepSettled(host, ctx, 1, true, L"退出码 0"), "第一条成功后应接着等第二条");
  GC_REQUIRE(host.launches.size() == 2, "错序终态没有抢先发第二条，这次才发");
  GC_CHECK(!flow.OnStepSettled(host, ctx, 1, true, L"退出码 0"), "重复投递的第 1 条终态被丢弃");
  GC_REQUIRE(host.launches.size() == 2, "重复终态不得再多发一条");
  GC_CHECK(!flow.OnStepSettled(host, ctx, 2, true, L"退出码 0"), "第 2 条成功后整段结案");
  GC_CHECK(!flow.Active());
  GC_REQUIRE(host.launches.size() == 2, "结案之后不再发任何命令");
}
