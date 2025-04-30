#pragma once

#include <string>

#include "git/fetch_plan.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「fetch 目标只读预检」的请求。工作区根取识别结果，不取输入框原文；
// repositoryDirectory 原样回显进结果，界面据此判别「这份预检还是不是这个仓库的」。
struct FetchProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  unsigned long timeoutMilliseconds = 0;
  StopFlag stopFlag;  // 由 worker 挂上：退出收尾时剩余查询不再发起。
};

// 一次后台预检的成品：判读结果加请求携带的目录回显。
struct FetchProbeOutcome {
  git::FetchTargetFacts facts;
  std::wstring repositoryDirectory;
};

// 执行依赖以回调注入，使「问哪几条、怎么判读」的编排可脱离 Win32 用真实临时仓库测试。
struct FetchProbeDeps {
  git::GitQueryRunner runner;
};

// 只读地发起一组目标查询（当前分支 / 分支配置的远端 / 远端清单），外加 git/fetch_scope
// 那两条「影响抓取范围」的配置查询，并把回答交给 git/fetch_plan 判读。全程隐藏窗口子进程，
// 不弹命令窗口、不写对象库、不访问远端；不在分支上时根本不发「分支配置的远端」那条查询
// （那种仓库本来也没有分支名可拼键）。
[[nodiscard]] git::FetchTargetFacts CollectFetchTarget(const FetchProbeRequest& request,
                                                       const FetchProbeDeps& deps);

// 用本工程的隐藏窗口子进程执行器装配 FetchProbeDeps。
[[nodiscard]] FetchProbeDeps MakeFetchProbeDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行预检，请求携带的目录原样带进结果。
[[nodiscard]] FetchProbeOutcome RunFetchProbeLoad(const FetchProbeRequest& request);

// 预检的后台控制器：查询在工作线程执行，GUI 线程不冻结；
// 连续点击时旧结果按序号作废，不会把上一个仓库的远端清单混进选择界面。
using FetchProbeWorker = GitTaskWorker<FetchProbeRequest, FetchProbeOutcome>;

}  // namespace gc::platform
