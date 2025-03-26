#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "git/repository.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次识别任务：参数已在本机边界完成绝对化，工作线程只负责执行只读 Git 查询。
struct RepoDetectRequest {
  std::wstring exePath;
  std::wstring directory;  // 用户选择的目录（绝对路径；Git 回答的工作区根写进结果）
  unsigned long timeoutMilliseconds = 0;
};

// 仓库识别的执行依赖，全部以回调注入，使流程编排可脱离 Win32 单元测试：
//   runner          —— 以参数数组执行一次只读 Git 查询（不弹命令窗口，不经过 cmd）
//   absolutize       —— 把相对路径按 givenDirectory 展开为绝对路径（Git 可能返回相对 common-dir）
//   slashify         —— 把 Git 输出的斜杠路径统一成本地路径写法
//   directoryExists、gitExeIsFile —— 输入可用性检查
struct RepoDetectDeps {
  git::GitQueryRunner runner;
  std::function<std::wstring(const std::wstring&, std::wstring_view)> absolutize;
  std::function<std::wstring(std::wstring_view)> slashify;
  std::function<bool(const std::wstring&)> directoryExists;
  std::function<bool(const std::wstring&)> gitExeIsFile;
};

// 识别“用户选择的目录”属于哪种 Git 仓库：普通工作区、链接工作树、子模块、裸仓库、
// .git 内部、尚无提交、游离 HEAD 或非仓库。全程只读：不 init、不改配置、不 fetch、
// 不自动修复 safe.directory，也不改变本进程的工作目录。
[[nodiscard]] git::RepoDetection DetectRepository(const RepoDetectRequest& request,
                                                  const RepoDetectDeps& deps);

// 用本工程的隐藏窗口子进程执行器装配 RepoDetectDeps；每条查询都有独立超时。
[[nodiscard]] RepoDetectDeps MakeRepoDetectDeps(unsigned long timeoutMilliseconds);

// 工作线程任务体：装配依赖并执行识别（GitQueryRunner 在这里绑定子进程执行器）。
[[nodiscard]] git::RepoDetection RunRepositoryDetection(const RepoDetectRequest& request);

// 仓库识别的后台控制器：识别在工作线程执行，GUI 线程不冻结；
// 连续切换仓库时旧结果按序号作废，不会覆盖新选择。
using RepoDetectWorker = GitTaskWorker<RepoDetectRequest, git::RepoDetection>;

}  // namespace gc::platform
