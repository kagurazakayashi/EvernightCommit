#pragma once

#include <string>

#include "git/restore_plan.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「按记录恢复引用」的只读预检请求。工作区根与绝对 Git 目录都取识别结果，不取输入框原文；
// repositoryDirectory 原样回显进结果，界面据此判别「这份预检还属于这个仓库」。
// branchRef / moveToOid / isRootDeletion 来自一条历史记录的恢复线索；isRootDeletion 为真时
// 根本不问「要挪去的对象」（删除形态没有新值可验）。
struct RestoreProbeRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring absoluteGitDir;  // 有值才探流程痕迹（读档案存在性，非子进程）
  std::wstring branchRef;       // 要恢复的那条完整分支引用
  std::wstring moveToOid;       // 要挪回去的对象（完整 ID；删除形态为空）
  bool isRootDeletion = false;
  unsigned long timeoutMilliseconds = 0;
  StopFlag stopFlag;  // 退出收尾时逐条查询前核对，剩余查询不再发起
};

// 一次后台预检的成品：判读所需的原始查询结果 + 请求携带的目录回显。
struct RestoreProbeOutcome {
  git::RestorePreflightQueries queries;
  std::wstring repositoryDirectory;
};

// 只读地发起恢复预检（分支现在指着什么 + 要挪去的对象可达性 + 有没有流程停着）。
// 全程隐藏窗口子进程，不弹命令窗口、不写对象库、不访问远端。
[[nodiscard]] git::RestorePreflightQueries CollectRestoreFacts(const RestoreProbeRequest& request);

// 工作线程任务体：装配查询并把请求携带的目录原样带进结果。
[[nodiscard]] RestoreProbeOutcome RunRestoreProbeLoad(const RestoreProbeRequest& request);

// 恢复预检的后台控制器：查询在工作线程执行，GUI 线程不冻结；连续点击时旧结果按序号作废。
using RestoreProbeWorker = GitTaskWorker<RestoreProbeRequest, RestoreProbeOutcome>;

// 「点头之后、发命令之前」的分支现值复核也走后台：只重问 <分支>^{commit} 这一条。
[[nodiscard]] git::GitQueryResult CaptureRestoreBranchRecheck(const std::wstring& exePath,
                                                             const std::wstring& repositoryDirectory,
                                                             const std::wstring& branchRef,
                                                             unsigned long timeoutMilliseconds);
using RestoreRecheckWorker = GitTaskWorker<RestoreProbeRequest, git::GitQueryResult>;

}  // namespace gc::platform
