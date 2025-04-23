#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// 一路输出（stdout 或 stderr）最终的收尾形态。四种「没有完整读完」的原因互不相同，
// 界面与日志要能说清是哪一种，不能都写成「没有输出」。
enum class StreamCaptureState {
  complete,    // 写端全部关闭后读到 EOF，字节数没超过上限：这一路是完整的。
  truncated,   // 超过字节上限：只保留前 maxBytes 字节，其余已丢弃（仍在继续排空管道）。
  readFailed,  // ReadFile 报了非 EOF 的错，或读线程根本没建起来。
  abandoned,   // 有界收尾期限内既没有新字节也没有 EOF（常见于孙进程仍持有写端）：主动中止。
  notStarted,  // 还没开始读，或这一路从未建立。
};

// 一路输出的捕获结果。bytes 只有在 state == complete 时才是这一路的完整回答；
// observedBytes 是实际出现过的字节总数（含超限丢弃的部分），用于向用户说明「读到了多少」。
struct StreamCapture {
  StreamCaptureState state = StreamCaptureState::notStarted;
  std::string bytes;
  size_t observedBytes = 0;
  size_t maxBytes = 0;      // 本次生效的保留上限
  std::wstring note;        // 限长说明（读错误文本、中止原因等），供界面与日志
  [[nodiscard]] bool Complete() const noexcept { return state == StreamCaptureState::complete; }
  [[nodiscard]] std::wstring_view StateLabel() const noexcept;
};

// 序列化结果（成功或失败原因）与运行结果分开：失败必须给出可展示的具体原因。
struct SubprocessRunResult {
  bool started = false;      // CreateProcessW 是否成功
  unsigned long launchError = 0;
  std::wstring launchErrorText; // FormatMessageW 文本（本地化语言）
  bool exited = false;       // 进程在期限内退出（含被超时终止后的确认）
  bool timedOut = false;
  bool terminated = false;   // 超时后由本进程 TerminateProcess 结束
  unsigned long exitCode = 0;
  StreamCapture stdoutCapture;  // 标准输出：机器可读字段
  StreamCapture stderrCapture;  // 标准错误：致命信息
  std::wstring commandLine;     // 实际传给 CreateProcessW 的命令行，供诊断展示

  // 两条流都完整读回，才可以说「这份输出就是进程给的全部回答」。
  [[nodiscard]] bool AllStreamsComplete() const noexcept {
    return stdoutCapture.Complete() && stderrCapture.Complete();
  }
};

// 默认保留上限：porcelain v2 一条记录约 60～90 字节，16 MiB 足以容纳二十万条上下的变化条目。
// 超过上限不再当成完整快照（调用方会明确失败），但仍继续排空管道，避免把子进程卡死。
inline constexpr size_t kDefaultMaxCapturedOutputBytes = 16u << 20;

// 进程结束后给管道的收尾时间上限。正常形态下进程一退出写端就关闭、EOF 立刻到达，
// 这段时间一秒也不会用满；只有子进程的子孙还占着写端时才会耗尽它，届时按「没读到结尾」报告。
inline constexpr unsigned long kDefaultDrainAfterExitMilliseconds = 5000;

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
// maxCaptureBytesPerStream 是每一路各自保留的字节上限，测试用它把「恰好上限」「超过上限」
// 做成便宜的用例；生产按 kDefaultMaxCapturedOutputBytes。
// drainAfterExitMilliseconds 是进程结束后的收尾期限，测试用它把「子孙占着管道」这一路
// 做成便宜的用例；生产按 kDefaultDrainAfterExitMilliseconds。
//
// 子进程结束（或被终止）后，这里会先把管道里已缓冲的输出排空再收工，正常情况必然等到 EOF；
// 只有在超过有界收尾期限（孙进程占着写端等）时才转为 abandoned，并如实标成不完整。
// 交给子进程的继承句柄只有这一对管道写端：并发启动时不会把别的进程的管道句柄漏进新进程。
[[nodiscard]] SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                                    const std::vector<std::wstring>& arguments,
                                                    std::wstring_view workingDirectory,
                                                    unsigned long timeoutMilliseconds,
                                                    const wchar_t* environmentBlock,
                                                    size_t maxCaptureBytesPerStream,
                                                    unsigned long drainAfterExitMilliseconds);

[[nodiscard]] inline SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                                           const std::vector<std::wstring>& arguments,
                                                           std::wstring_view workingDirectory,
                                                           unsigned long timeoutMilliseconds,
                                                           const wchar_t* environmentBlock,
                                                           size_t maxCaptureBytesPerStream) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, environmentBlock,
                           maxCaptureBytesPerStream, kDefaultDrainAfterExitMilliseconds);
}

[[nodiscard]] inline SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                                           const std::vector<std::wstring>& arguments,
                                                           std::wstring_view workingDirectory,
                                                           unsigned long timeoutMilliseconds,
                                                           const wchar_t* environmentBlock) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, environmentBlock,
                           kDefaultMaxCapturedOutputBytes);
}

[[nodiscard]] inline SubprocessRunResult RunHiddenCaptured(std::wstring_view program,
                                                           const std::vector<std::wstring>& arguments,
                                                           std::wstring_view workingDirectory,
                                                           unsigned long timeoutMilliseconds) {
  return RunHiddenCaptured(program, arguments, workingDirectory, timeoutMilliseconds, nullptr);
}

}  // namespace gc::platform
