#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// 序列化结果（成功或失败原因）与运行结果分开：失败必须给出可展示的具体原因。
struct SubprocessRunResult {
  bool started = false;      // CreateProcessW 是否成功
  unsigned long launchError = 0;
  std::wstring launchErrorText; // FormatMessageW 文本（本地化语言）
  bool exited = false;       // 进程在期限内退出（含被超时终止后的确认）
  bool timedOut = false;
  unsigned long exitCode = 0;
  std::string utf8Stdout;    // 子进程标准输出的原始字节
  std::string utf8Stderr;    // 子进程标准错误的原始字节
  std::string utf8Output;    // stdout 后接 stderr 的合并字节，供只关心“有没有回话”的探测使用
  std::wstring commandLine;  // 实际传给 CreateProcessW 的命令行，供诊断展示
};

// 纯逻辑：按 MSVC CRT / CommandLineToArgvW 的解析规则把程序路径与参数数组拼成命令行字符串。
// 只处理程序参数转义，不经过 cmd，因此不存在 shell 元字符二次解释的问题。
[[nodiscard]] std::wstring BuildCommandLine(std::wstring_view program,
                                            const std::vector<std::wstring>& arguments);

// 把单个字符串（程序路径或一个参数）按同一套程序参数规则编码为“带引号的一段”：
// 一律加引号，内部引号转义，反斜杠按规则加倍。供需要“逐段构造命令行”的调用方复用
// （外部命令窗口执行器要按段校验 cmd 解析后的引号区域，见 git/command_window.h）。
[[nodiscard]] std::wstring QuoteArgument(std::wstring_view value);

// 同步执行一个隐藏窗口的程序并捕获输出。会阻塞调用线程直到退出或超时（超时后终止进程），
// 因此只允许在后台工作线程调用，不允许在 GUI 线程调用。
// workingDirectory 为空时沿用父进程当前目录。
// environmentBlock 为 NULL 结尾的 Unicode 环境块（GetEnvironmentStringsW 同格式），
// 传 nullptr 时子进程继承本进程环境。仅测试夹具需要传入隔离环境块；
// 生产代码一律使用下面的四参重载，不指定环境块。
// stdout 与 stderr 分别捕获：Git 的机器输出走 stdout，致命信息走 stderr，
// 合并读取会因两条流的写入时机不同而打乱按行取字段的顺序。
[[nodiscard]] SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                                    const std::vector<std::wstring>& arguments,
                                                    std::wstring_view workingDirectory,
                                                    unsigned long timeoutMilliseconds,
                                                    const wchar_t* environmentBlock);

[[nodiscard]] inline SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                                           const std::vector<std::wstring>& arguments,
                                                           std::wstring_view workingDirectory,
                                                           unsigned long timeoutMilliseconds) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, nullptr);
}

}  // namespace gc::platform
