#pragma once

#include <string>

#include "git/commit_plan.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「提交身份预检」的请求。工作区根取仓库识别的结果，不取输入框原文；
// repositoryDirectory 原样回显进结果，界面据此判别「这份预检还是不是这个仓库的」。
// 绝对 Git 目录不在请求里：这一趟自己问，流程痕迹按刚问到的那个目录探测，
// 免得拿界面早前存下的旧目录探出上一块工作树的痕迹来配新的 HEAD。
struct CommitProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  unsigned long timeoutMilliseconds = 0;
};

// 一次后台预检的成品：判读结果加请求携带的目录回显。
struct CommitProbeOutcome {
  git::CommitIdentityFacts facts;
  std::wstring repositoryDirectory;
};

// 执行依赖以回调注入，使「问哪几条、怎么判读」的编排可脱离 Win32 用真实临时仓库测试。
struct CommitProbeDeps {
  git::GitQueryRunner runner;
};

// 只读地问回这一次提交要绑定的事实：工作区根与绝对 Git 目录、完整分支引用、HEAD 完整对象 ID、
// 索引内容标识（git write-tree）、有效配置里的提交者身份，以及 Git 目录里的流程痕迹。
// 同一条路径既用于确认框之前的预检，也用于点头之后、发命令之前的复核——两次问法一字不差，
// 比对才有意义。
//
// 唯一的可观察副作用是 git write-tree 本身：它把算出的树对象写进对象库（没有引用指向它，
// 之后由 Git 的垃圾回收清理），索引里过期的文件状态信息可能被顺带刷新（条目与内容一字不动）。
// 不产生提交、不移动分支或标签、不改工作区文件、不写任何配置；也不会等任何输入。
[[nodiscard]] git::CommitIdentityFacts CollectCommitPreflight(const CommitProbeRequest& request,
                                                              const CommitProbeDeps& deps);

// 用本工程的隐藏窗口子进程执行器装配 CommitProbeDeps（只在后台工作线程调用）。
[[nodiscard]] CommitProbeDeps MakeCommitProbeDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行预检，请求携带的目录原样带进结果。
[[nodiscard]] CommitProbeOutcome RunCommitProbeLoad(const CommitProbeRequest& request);

// 预检的后台控制器：查询在工作线程执行，GUI 线程不冻结；
// 连续点击时旧结果按序号作废，不会把上一个仓库的事实混进确认框或复核结论。
using CommitProbeWorker = GitTaskWorker<CommitProbeRequest, CommitProbeOutcome>;

}  // namespace gc::platform
