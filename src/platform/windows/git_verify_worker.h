#pragma once

#include "platform/windows/git_task_worker.h"
#include "platform/windows/git_toolchain.h"

namespace gc::platform {

// Git --version 验证的后台控制器（步骤 2）：复用 GitTaskWorker 的序号丢弃机制，
// 验证在工作线程执行，GUI 线程不等待；旧结果不会覆盖用户之后的选择。
using GitVerifyWorker = GitTaskWorker<std::wstring, GitExeVerification>;

}  // namespace gc::platform
