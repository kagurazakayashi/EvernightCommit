#pragma once

#include <string>
#include <vector>

#include "git/push_plan.h"
#include "git/repository.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「推送预检」的请求。工作区根与绝对 Git 目录都取仓库识别的结果，不取输入框原文；
// repositoryDirectory 原样回显进结果，界面据此判别「这份预检还是不是这个仓库的」。
struct PushProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring absoluteGitDir;
  unsigned long timeoutMilliseconds = 0;
};

struct PushProbeOutcome {
  git::PushPreflightFacts facts;
  std::wstring repositoryDirectory;
};

// 执行依赖以回调注入，使「问哪几条、怎么判读」的编排可脱离 Win32 用真实临时仓库测试。
struct PushProbeDeps {
  git::GitQueryRunner runner;
};

// 只读问回「在哪个分支、要推哪一份提交、上游是谁、这次实际推给哪个远端的哪个 URL、
// 本地相对上一次抓取领先/落后几个」。全程隐藏窗口子进程：不弹命令窗口、不访问远端、不写对象库。
//
// 发问顺序是有依赖的，不是可以并成一堆的清单：
//   分支名（symbolic-ref）→ 才有分支级的上游可问（for-each-ref）→ 才有跟踪引用可问位置；
//   生效配置（config --list --null）→ 才定得出「实际推给哪个远端」→ 才问得出那个远端的发布 URL。
// 前一步没成立时后面根本没有问题可问，那些查询因此一条也不发（判读层按 *Ran 如实区分
// 「问了且 Git 答没有」与「压根没问」）。
[[nodiscard]] git::PushPreflightFacts CollectPushPreflight(const PushProbeRequest& request,
                                                           const PushProbeDeps& deps);

[[nodiscard]] PushProbeDeps MakePushProbeDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖跑一次预检，请求携带的目录原样带进结果。
[[nodiscard]] PushProbeOutcome RunPushProbeLoad(const PushProbeRequest& request);

// ---- 推送之后：向发布目标核对那条引用的实际位置 ----

// 一次核对请求。pushUrls 就是确认框上列出的那一批发布目标（不是上游所在的抓取远端）：
// 只有向它们逐个问过，才有资格说「送到没送到」。
// commandConclusion 是命令窗口那一头已经给出的结论文字，参与最终措辞。
struct PushVerifyRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring remoteBranchRef;
  std::wstring expectedObjectId;
  std::vector<std::wstring> pushUrls;
  bool pushCommandSucceeded = false;
  std::wstring commandConclusion;
  unsigned long timeoutMilliseconds = 0;
};

struct PushVerifyOutcome {
  git::PushVerificationReport report;
  // 命令窗口那一头的结论（0 才算成功），原样回显：界面据此决定要不要另开一个说明框
  // ——「Git 说成功了但我没核实上」必须当面讲清楚，而「本来就失败了、又没核上」只需写进状态栏。
  bool pushCommandSucceeded = false;
  std::wstring repositoryDirectory;
};

// 逐个发布目标发一条只读 `git ls-remote -- <URL> <引用>`，把结果判读合成一份核实报告。
// 每个目标各自成一条：一个目标问不到（认证、网络、对端设置）不影响另外几个照实记录。
[[nodiscard]] PushVerifyOutcome RunPushVerifyLoad(const PushVerifyRequest& request);

// 预检与核实的后台控制器。两者各占一个 worker：核实发生在命令窗口操作之后，
// 与预检的迟到结果作废判定互不干扰。
using PushProbeWorker = GitTaskWorker<PushProbeRequest, PushProbeOutcome>;
using PushVerifyWorker = GitTaskWorker<PushVerifyRequest, PushVerifyOutcome>;

}  // namespace gc::platform
