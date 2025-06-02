#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "platform/process.h"

namespace gc::platform {

// 纯逻辑：按 MSVC CRT / CommandLineToArgvW 的解析规则把程序路径与参数数组拼成命令行字符串。
// 只处理程序参数转义，不经过 cmd，因此不存在 shell 元字符二次解释的问题。
[[nodiscard]] std::wstring BuildCommandLine(std::wstring_view program,
                                            const std::vector<std::wstring>& arguments);

// 把单个字符串（程序路径或一个参数）按同一套程序参数规则编码为“带引号的一段”：
// 一律加引号，内部引号转义，反斜杠按规则加倍。供需要“逐段构造命令行”的调用方复用
// （外部命令窗口执行器要按段校验 cmd 解析后的引号区域，见 git/command_window.h）。
[[nodiscard]] std::wstring QuoteArgument(std::wstring_view value);

// 同步执行一个隐藏窗口的程序并捕获输出。会阻塞调用线程直到退出、超时或被停止信号中止，
// 因此只允许在后台工作线程调用，不允许在 GUI 线程调用。
// workingDirectory 为空时沿用父进程当前目录。
// environmentBlock 为 NULL 结尾的 Unicode 环境块（GetEnvironmentStringsW 同格式），
// 传 nullptr 时子进程继承本进程环境。生产 Git 查询走 platform::RunGitCaptured
// （集中环境策略装配环境块），不直接调用本函数裸继承环境。
// stdout 与 stderr 分别捕获：Git 的机器输出走 stdout，致命信息走 stderr，
// 合并读取会因两条流的写入时机不同而打乱按行取字段的顺序。
// timeoutMilliseconds 为 0 表示不设超时；cancel 只中止本次后台进程，不给命令窗口使用。
// maxCaptureBytesPerStream 是每一路各自保留的字节上限，测试用它把「恰好上限」「超过上限」
// 做成便宜的用例；生产按 kDefaultMaxCapturedOutputBytes。
// drainAfterExitMilliseconds 是进程结束后的收尾期限，测试用它把「子孙占着管道」这一路
// 做成便宜的用例；生产按 kDefaultDrainAfterExitMilliseconds。
[[nodiscard]] ProcessRunResult RunHiddenCaptured(std::wstring_view program,
                                                 const std::vector<std::wstring>& arguments,
                                                 std::wstring_view workingDirectory,
                                                 unsigned long timeoutMilliseconds,
                                                 const wchar_t* environmentBlock,
                                                 size_t maxCaptureBytesPerStream,
                                                 unsigned long drainAfterExitMilliseconds,
                                                 StopFlag cancel = {});

[[nodiscard]] inline ProcessRunResult RunHiddenCaptured(std::wstring_view program,
                                                        const std::vector<std::wstring>& arguments,
                                                        std::wstring_view workingDirectory,
                                                        unsigned long timeoutMilliseconds,
                                                        const wchar_t* environmentBlock,
                                                        size_t maxCaptureBytesPerStream) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, environmentBlock,
                           maxCaptureBytesPerStream, kDefaultDrainAfterExitMilliseconds);
}

[[nodiscard]] inline ProcessRunResult RunHiddenCaptured(std::wstring_view program,
                                                        const std::vector<std::wstring>& arguments,
                                                        std::wstring_view workingDirectory,
                                                        unsigned long timeoutMilliseconds,
                                                        const wchar_t* environmentBlock) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, environmentBlock,
                           kDefaultMaxCapturedOutputBytes);
}

[[nodiscard]] inline ProcessRunResult RunHiddenCaptured(std::wstring_view program,
                                                        const std::vector<std::wstring>& arguments,
                                                        std::wstring_view workingDirectory,
                                                        unsigned long timeoutMilliseconds) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, nullptr);
}

}  // namespace gc::platform
