#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "git/commit_plan.h"
#include "git/pull_plan.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「pull 预检」的请求。工作区根与绝对 Git 目录都取仓库识别的结果，不取输入框原文；
// repositoryDirectory 原样回显进结果，界面据此判别「这份预检还是不是这个仓库的」。
struct PullProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring absoluteGitDir;
  unsigned long timeoutMilliseconds = 0;
  // false：只问阶段一（分支 / HEAD / 上游 / 策略配置 / 现状）——抓取前的前提核对，
  //         以及点头之后、执行之前的那次「还是不是同一份现状」的复核都走这一档；
  // true：抓取结束之后，连本地与远端的关系、会带进哪些文件、内容冲突预演一起问回来。
  bool includeRelationship = false;
};

struct PullProbeOutcome {
  git::PullTargetFacts target;
  git::PullRelationshipFacts relationship;  // includeRelationship 时才有内容
  std::wstring repositoryDirectory;
};

// 执行依赖以回调注入，使「问哪几条、怎么判读」的编排可脱离 Win32 用真实临时仓库测试。
struct PullProbeDeps {
  git::GitQueryRunner runner;
};

// 阶段一：只读问回「在哪个分支、HEAD 是哪一份、上游是谁、策略配置怎么写的、现状如何」。
// 全程隐藏窗口子进程：不弹命令窗口、不访问远端、不写对象库。不在分支上时根本不发
// 「问上游」以及后续那几条依赖分支名的查询——那种仓库本来也没有分支级配置可问。
[[nodiscard]] git::PullTargetFacts CollectPullTarget(const PullProbeRequest& request,
                                                     const PullProbeDeps& deps);

// 阶段二：在阶段一的事实之上追问「本地与远端各有几个独有提交、共同基准、会带进哪些文件、
// 内容冲突预演」。除关系那一条外全部用完整对象 ID 提问：预检看的与实际整合的必须是同一份提交。
// 阶段一没问出可用事实时返回带 queryFailure 的结论，一条查询也不发。
[[nodiscard]] git::PullRelationshipFacts CollectPullRelationship(const git::PullTargetFacts& target,
                                                                 const PullProbeRequest& request,
                                                                 const PullProbeDeps& deps);

[[nodiscard]] PullProbeDeps MakePullProbeDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并按请求的阶段执行预检，请求携带的目录原样带进结果。
[[nodiscard]] PullProbeOutcome RunPullProbeLoad(const PullProbeRequest& request);

// 整合命令非 0 退出之后，问一句「现在究竟卡在什么现场」：Git 目录里的流程痕迹（merge/rebase
// 进行中）、索引里的未合并条目，以及分支/HEAD 现在的位置。三条查询加一次档案存在性判断，
// 都是毫秒级，直接在调用线程执行。
// 全程只读：本函数与它的调用方都不会 abort、不会 reset、不会 continue，也不会替用户选任何一边。
struct PullAftermath {
  git::RepositoryWorkflowState workflow;
  git::PullConflictState conflict;
};

[[nodiscard]] PullAftermath CapturePullAftermath(const std::wstring& exePath,
                                                 const std::wstring& repositoryDirectory,
                                                 const std::wstring& absoluteGitDir,
                                                 unsigned long timeoutMilliseconds);

// 预检的后台控制器：查询在工作线程执行，GUI 线程不冻结；
// 连续点击时旧结果按序号作废，不会把上一个仓库的事实混进确认框。
using PullProbeWorker = GitTaskWorker<PullProbeRequest, PullProbeOutcome>;

}  // namespace gc::platform
