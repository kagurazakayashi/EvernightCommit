#pragma once

#include <string>
#include <vector>

#include "git/conflict_state.h"
#include "git/repository.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「冲突与暂停流程」现场读取的请求。工作区根与绝对 Git 目录都取仓库识别的结果，
// 不取输入框原文；repositoryDirectory 原样回显进结果，界面据此判别「这份读取还是不是这个仓库的」。
struct ConflictProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring absoluteGitDir;
  unsigned long timeoutMilliseconds = 0;
  StopFlag stopFlag;  // 由 worker 挂上：退出收尾时剩余查询不再发起。
};

struct ConflictProbeOutcome {
  git::ConflictStateFacts state;
  std::wstring repositoryDirectory;
};

// 执行依赖以回调注入，使「读哪些档案、问哪几条、怎么判读」可脱离 Win32 用真实临时仓库测试。
struct ConflictProbeDeps {
  git::GitQueryRunner runner;
  // 痕迹读取（存在性 + 限长内容）：真实实现读绝对 Git 目录，测试可注入桩以覆盖「读不回来」的形态。
  std::function<git::ConflictMarkerFacts(const std::wstring& absoluteGitDir)> markers;
};

// 现场读取：Git 目录里的流程痕迹（档案/目录存在性 + 少量限长内容），加上三条只读查询
// （索引里还有哪些未合并条目、当前分支、HEAD 现在在哪）。
// 全程不写任何东西：不 add、不 restore、不 --continue、不 --abort、不 reset、不删锁文件。
// 预检与「点头之后、发命令之前」的复核共用本函数，因此两比对得上（AGENTS 的同源要求）。
[[nodiscard]] git::ConflictStateFacts CollectConflictState(const ConflictProbeRequest& request,
                                                           const ConflictProbeDeps& deps);

// 用本工程的隐藏窗口子进程执行器与真实目录读取装配依赖。
[[nodiscard]] ConflictProbeDeps MakeConflictProbeDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行现场读取，请求携带的目录原样带进结果。
[[nodiscard]] ConflictProbeOutcome RunConflictProbeLoad(const ConflictProbeRequest& request);

// 后台控制器：读取在工作线程执行，GUI 线程不冻结；连续点击时旧结果按序号作废。
using ConflictProbeWorker = GitTaskWorker<ConflictProbeRequest, ConflictProbeOutcome>;

// 真实目录读取那一份（供 deps.markers 注入与夹具直接调用）。
// 「存在但读不回来」与「不存在」分得很开：前者记进 contentFailures，后者才是明确没有。
// 任何一条存在性判断被系统拒绝（权限等）时，probed 落为 false 并带上原因——
// 「看不见」在这里绝不当成「没有流程停着」。
[[nodiscard]] git::ConflictMarkerFacts ProbeConflictMarkers(std::wstring_view absoluteGitDir);

}  // namespace gc::platform
