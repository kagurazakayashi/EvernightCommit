#pragma once

#include <string>
#include <vector>

#include "git/commit_detail.h"
#include "git/repository.h"  // GitQueryRunner
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「选中提交的完整信息」读取任务的请求：与文件变化那份读取同一套身份字段——
// 仓库目录取识别结果的工作区根，不取输入框原文；objectId 为列表项里保存的完整对象 ID。
struct CommitDetailRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  std::wstring objectId;
  unsigned long timeoutMilliseconds = 0;
  unsigned long long readSerial = 0;
  unsigned long long bindingGeneration = 0;
  StopFlag stopFlag;  // 由 worker 挂上：退出收尾时不再发起这条查询。
};

// 一次后台读取的成品：详情文本与拼好的可复制内容，加上请求携带的身份回显与成败归类。
struct CommitDetailOutcome {
  git::RepoError error = git::RepoError::none;  // 非 none 时 detail 为空壳，message 为失败原因
  std::wstring message;                          // 面向界面的完整说明（成功摘要或失败原因）
  git::CommitDetail detail;
  std::wstring text;  // 面板显示与「复制」按钮共用的那份文本（git::FormatCommitDetailText 的产物）
  // 身份回显：读的是哪条提交、属于哪一次读取、哪个仓库身份。缺一项都没法判定「这份结果还收不收」。
  std::wstring objectId;
  unsigned long long readSerial = 0;
  unsigned long long bindingGeneration = 0;
};

// 执行依赖以回调注入，使「怎么问、怎么解析」的分类逻辑可脱离 Win32 用桩输出测试。
struct CommitDetailDeps {
  git::GitQueryRunner runner;
};

// 只读地取回并解析某条提交的完整信息：一次 git log -n 1 --format=<七字段>。
// 是隐藏窗口的只读查询，不写对象库、不访问远端、不改动仓库。
[[nodiscard]] CommitDetailOutcome LoadCommitDetail(const CommitDetailRequest& request,
                                                   const CommitDetailDeps& deps);

// 用本工程的隐藏窗口子进程执行器装配 CommitDetailDeps。
[[nodiscard]] CommitDetailDeps MakeCommitDetailDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行读取，把请求携带的读取序号与身份版本原样带回结果。
[[nodiscard]] CommitDetailOutcome RunCommitDetailLoad(const CommitDetailRequest& request);

// 提交详情读取的后台控制器：读取在工作线程执行，GUI 线程不冻结；
// 连续切换提交时旧结果按序号作废，不会把上一条提交的详情显示到新选中项下面。
using CommitDetailWorker = GitTaskWorker<CommitDetailRequest, CommitDetailOutcome>;

}  // namespace gc::platform
