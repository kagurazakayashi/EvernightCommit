#pragma once

#include <string>
#include <vector>

#include "platform/process.h"

namespace gc::platform {

// Linux 后台执行器：直接 posix_spawn 程序路径与参数数组，不经过 sh -c。
// environmentEntries 是显式、完整的 "NAME=VALUE" 列表（UTF-8 字节），直接作为 envp；
// 传空列表表示只有空环境（仍会设置 argv，子进程不会继承本进程环境）。
//
// 每条记录都必须是调用方装配好的子进程环境（生产 Git 查询走
// BuildGitChildEnvironment）。本函数不会读 environ、不会 setenv，也不会改本进程环境。
//
// 子进程：
//   * stdin 接 /dev/null，后台查询永远不会停在看不见的输入上；
//   * stdout/stderr 各一条独立管道，独立线程并行排空；
//   * 通过 setsid 独占一个会话/进程组，超时或取消时只向该组发送 SIGKILL；
//   * 关闭 3 号及以上的继承描述符（glibc 2.34+ 的 addclosefrom_np），
//     并发任务之间不会互相漏管道。
//
// 返回值区分启动失败、正常退出、信号终止、超时和取消；两条流的完整/截断/读取失败/
// 放弃收尾各自记录，不合并成「没有输出」。
[[nodiscard]] ProcessRunResult RunCapturedProcess(
    const ProcessRequest& request, const std::vector<std::string>& environmentEntries);

}  // namespace gc::platform
