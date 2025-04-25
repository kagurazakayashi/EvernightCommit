#pragma once

#include <string>
#include <utility>

#include "git/undo_commit_plan.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「撤回前预检」的请求。工作区根取识别结果，不取输入框原文；
// repositoryDirectory 原样回显进结果，界面据此判别「这份预检还是不是这个仓库的」。
struct UndoProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  unsigned long timeoutMilliseconds = 0;
};

// 一次后台预检的成品：判读结果加请求携带的目录回显。
struct UndoProbeOutcome {
  git::UndoPreflightFacts facts;
  std::wstring repositoryDirectory;
};

// 执行依赖以回调注入，使「问哪几条、怎么判读」的编排可脱离 Win32 用真实临时仓库测试。
struct UndoProbeDeps {
  git::GitQueryRunner runner;
};

// 只读地发起一组撤回预检查询（分支 / HEAD 完整 ID / 父提交 / 提交对象自己的 parent 行 /
// 是否浅仓库 / 目标父对象可读性 / 标题 / 远端跟踪引用 / 工作区状态），
// 并把回答交给 git/undo_commit_plan 判读。全程隐藏窗口子进程，不弹命令窗口、不写对象库、
// 不访问远端；HEAD 不可解析时根本不发依赖它的那几条查询（那种仓库本来也没有可撤回的提交），
// 历史视图里没有父提交时也不发「父对象可读性」那条查询（没有目标可问）。
[[nodiscard]] git::UndoPreflightFacts CollectUndoPreflight(const UndoProbeRequest& request,
                                                           const UndoProbeDeps& deps);

// 用本工程的隐藏窗口子进程执行器装配 UndoProbeDeps。
[[nodiscard]] UndoProbeDeps MakeUndoProbeDeps(unsigned long timeoutMilliseconds);

// 用户在确认框点头之后、启动命令窗口之前，界面要同步复核 HEAD 与分支——只发这两条
// 极轻的只读查询，直接在调用线程执行（本地毫秒级；超时/启动失败如实记进 queryOk=false，
// 由调用方按「复核不过就放弃执行」处理）。返回的 facts 只填 symbolic-ref/rev-parse 相关字段。
[[nodiscard]] git::UndoHeadFacts CaptureUndoHeadSnapshot(const std::wstring& exePath,
                                                         const std::wstring& repositoryDirectory,
                                                         unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行预检，请求携带的目录原样带进结果。
[[nodiscard]] UndoProbeOutcome RunUndoProbeLoad(const UndoProbeRequest& request);

// 预检的后台控制器：查询在工作线程执行，GUI 线程不冻结；
// 连续点击时旧结果按序号作废，不会把上一个仓库的事实混进确认框。
using UndoProbeWorker = GitTaskWorker<UndoProbeRequest, UndoProbeOutcome>;

}  // namespace gc::platform
