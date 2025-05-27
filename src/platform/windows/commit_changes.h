#pragma once

#include <string>
#include <vector>

#include "git/commit_changes.h"
#include "git/repository.h"  // GitQueryRunner
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「某条提交改了哪些文件」读取任务的请求：仓库目录取识别结果的工作区根，
// 不取输入框原文；objectId 为列表项里保存的完整对象 ID。
struct CommitChangesRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring objectId;
  unsigned long timeoutMilliseconds = 0;
  // 读取凭证的回显字段：界面把本次读取的序号与仓库身份版本填在这里，任务体原样带回。
  // 换仓库、换提交后迟迟完成的旧读取就是靠它被判为过期。
  unsigned long long readSerial = 0;
  unsigned long long bindingGeneration = 0;
  StopFlag stopFlag;  // 由 worker 挂上：退出收尾时不再发起这条查询。
};

// 一次后台读取的成品：文件变化清单加上请求携带的身份回显，以及成败归类。
struct CommitChangesOutcome {
  git::RepoError error = git::RepoError::none;  // 非 none 时 changes 为空，message 为失败原因
  std::wstring message;                          // 面向界面的完整说明（成功摘要或失败原因）
  std::vector<git::ChangeItem> changes;
  // 身份回显：读的是哪条提交、属于哪一次读取、哪个仓库身份。
  // 界面只看得到这里带回的值，因此缺一项都没法判定「这份结果还收不收」。
  std::wstring objectId;
  unsigned long long readSerial = 0;
  unsigned long long bindingGeneration = 0;
};

// 执行依赖以回调注入，使「怎么问、怎么解析」的分类逻辑可脱离 Win32 用桩输出测试。
struct CommitChangesDeps {
  git::GitQueryRunner runner;
};

// 只读地取回并解析某条提交相对其第一个父提交的文件变化：一次 git show --name-status。
// 是隐藏窗口的只读查询，不写对象库、不访问远端、不改动仓库。
[[nodiscard]] CommitChangesOutcome LoadCommitChanges(const CommitChangesRequest& request,
                                                     const CommitChangesDeps& deps);

// 用本工程的隐藏窗口子进程执行器装配 CommitChangesDeps。
[[nodiscard]] CommitChangesDeps MakeCommitChangesDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行读取，把请求携带的读取序号与身份版本原样带回结果。
[[nodiscard]] CommitChangesOutcome RunCommitChangesLoad(const CommitChangesRequest& request);

// 提交文件变化读取的后台控制器：读取在工作线程执行，GUI 线程不冻结；
// 连续切换提交时旧结果按序号作废，不会把上一条提交的清单混进新提交。
using CommitChangesWorker = GitTaskWorker<CommitChangesRequest, CommitChangesOutcome>;

}  // namespace gc::platform
