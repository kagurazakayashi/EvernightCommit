#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

// ---------- 后台进程的统一契约 ----------
//
// 后台查询与终端执行是两条互不替代的通道：
//   * 本文件描述的是「隐藏启动、捕获输出、有界结束」的后台进程。程序路径、工作目录、
//     参数数组、环境、两条标准流、超时与取消状态都在请求/结果里说清楚，执行器不读
//     shell、不拼命令行，也不替调用方猜环境。
//   * 用户交互式 Git 仍然走命令窗口（独立终端、真实 stdin/tty、hooks、SSH、凭据助手
//     与签名程序都要能提问）。后台查询的取消策略绝不能套到那条通道上。
//
// Linux 用 posix_spawn 直接启动（不经 sh -c），每个后台进程独占一个会话/进程组；
// 超时或取消只对该进程组发送 SIGKILL，不会影响同一程序的其他任务。
// Windows 用 CreateProcessW 加显式继承句柄白名单，行为语义与这里保持一致。

// 后台任务的停止信号。worker 持有并翻牌，请求携带引用；空指针表示“无人喊停”。
// 它只作用于后台只读查询：当前子进程会被中止，后续查询不再发起。
using StopFlag = std::shared_ptr<std::atomic<bool>>;

[[nodiscard]] inline bool StopRequested(const StopFlag& flag) noexcept {
  return flag != nullptr && flag->load(std::memory_order_relaxed);
}

// 一路输出（stdout 或 stderr）最终的收尾形态。四种「没有完整读完」的原因互不相同，
// 界面与日志要能说清是哪一种，不能都写成「没有输出」。
enum class StreamCaptureState {
  complete,    // 写端全部关闭后读到 EOF，字节数没超过上限：这一路是完整的。
  truncated,   // 超过字节上限：只保留前 maxBytes 字节，其余已丢弃（仍在继续排空管道）。
  readFailed,  // 读取本身失败，或读线程根本没建起来。
  abandoned,   // 有界收尾期限内既没有新字节也没有 EOF（常见于孙进程仍持有写端）：主动中止。
  notStarted,  // 还没开始读，或这一路从未建立。
};

// 一路输出的捕获结果。bytes 只有在 state == complete 时才是这一路的完整回答；
// observedBytes 是实际出现过的字节总数（含超限丢弃的部分），用于说明「读到了多少」。
struct StreamCapture {
  StreamCaptureState state = StreamCaptureState::notStarted;
  std::string bytes;
  size_t observedBytes = 0;
  size_t maxBytes = 0;  // 本次生效的保留上限
  std::wstring note;    // 限长说明（读错误文本、中止原因等），供界面与日志

  [[nodiscard]] bool Complete() const noexcept { return state == StreamCaptureState::complete; }
  [[nodiscard]] std::wstring_view StateLabel() const noexcept;
};

// 进程结束的方式。启动失败、正常退出、被信号终止、超时和我们主动取消是五种不同事实：
//   notStarted —— 进程没有被创建（路径、权限、环境装配失败等）。
//   exited     —— wait 到正常退出（含非 0 退出码）。
//   signaled   —— 进程自己收到（或外部发给它）一个信号而结束，signal 记录信号号。
//   timedOut   —— 超过本层期限，由本层终止进程（terminated=true）。
//   canceled    —— 调用方的停止信号先到，由本层终止进程（terminated=true）。
//   unknown    —— 进程启动后既没有在期限内被确认退出，也没有被本层终止。
enum class ProcessTermination {
  notStarted,
  exited,
  signaled,
  timedOut,
  canceled,
  unknown,
};

[[nodiscard]] std::wstring_view ProcessTerminationLabel(ProcessTermination termination) noexcept;

// 一次后台进程执行的结果。字段之间不互斥地“合并报告”：
// terminated 表示本层确实发出过终止动作；reaped 表示 waitpid 成功回收；
// timedOut / canceled 只在对应终止原因下为真。界面不能拿 exitCode 0 当成“没跑过”。
struct ProcessRunResult {
  bool started = false;
  unsigned long launchError = 0;
  std::wstring launchErrorText;
  ProcessTermination termination = ProcessTermination::notStarted;
  bool exited = false;     // 进程已结束并确认（正常退出、信号或本层终止后回收）
  bool timedOut = false;
  bool canceled = false;
  bool terminated = false;  // 本层发出过终止动作
  bool reaped = false;      // 子进程已被回收；false 时不得假定进程已消失
  int exitCode = 0;
  int signal = 0;           // POSIX 信号号；Windows 恒为 0
  StreamCapture stdoutCapture;
  StreamCapture stderrCapture;
  std::wstring commandLine;  // 诊断展示用；Linux 为逐参数引用后的形式，不是执行输入

  // 两条流都完整读回，才可以说「这份输出就是进程给的全部回答」。
  [[nodiscard]] bool AllStreamsComplete() const noexcept {
    return stdoutCapture.Complete() && stderrCapture.Complete();
  }
  [[nodiscard]] bool CleanExit() const noexcept {
    return termination == ProcessTermination::exited && exitCode == 0 && AllStreamsComplete();
  }
};

// 默认保留上限：porcelain v2 一条记录约 60～90 字节，16 MiB 足以容纳二十万条上下的变化条目。
// 超过上限不再当成完整快照（调用方会明确失败），但仍继续排空管道，避免把子进程卡死。
inline constexpr size_t kDefaultMaxCapturedOutputBytes = 16u << 20;

// 进程结束后给管道的收尾时间上限。正常形态下进程一退出写端就关闭、EOF 立刻到达；
// 只有子进程的子孙还占着写端时才会耗尽它，届时按「没读到结尾」报告并清理进程组。
inline constexpr unsigned long kDefaultDrainAfterExitMilliseconds = 5000;

// 请求：程序路径与参数数组必须分开给，执行器不解释任何 shell 元字符。
// timeoutMilliseconds 为 0 表示不设超时；cancel 为空表示不响应外部停止。
struct ProcessRequest {
  std::wstring program;
  std::vector<std::wstring> arguments;
  std::wstring workingDirectory;
  unsigned long timeoutMilliseconds = 0;
  size_t maxCaptureBytesPerStream = kDefaultMaxCapturedOutputBytes;
  unsigned long drainAfterExitMilliseconds = kDefaultDrainAfterExitMilliseconds;
  StopFlag cancel;
};

}  // namespace gc::platform
