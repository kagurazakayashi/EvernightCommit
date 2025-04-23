#include "support/tiny_test.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"

namespace {

using gc::platform::StreamCaptureState;
using gc::platform::SubprocessRunResult;
using gc::test::PrerequisiteFailure;
using gc::test::TempDirectory;

constexpr unsigned long kProcessTimeoutMs = 20000;
constexpr unsigned long kFastTimeoutMs = 4000;
constexpr unsigned long kBigDrainMs = 4000;
constexpr size_t kDefaultCap = gc::platform::kDefaultMaxCapturedOutputBytes;

std::wstring CmdPath() {
  wchar_t buffer[MAX_PATH]{};
  const UINT copied = ::GetSystemDirectoryW(buffer, MAX_PATH);
  if (copied == 0) {
    throw PrerequisiteFailure("无法取到系统目录，找不到 cmd.exe");
  }
  return std::wstring(buffer) + L"\\cmd.exe";
}

// 每个用例独占一个临时目录：批处理与数据文件只出现在本用例拥有的路径里，用完随目录删除。
class ScriptFixture {
public:
  explicit ScriptFixture(std::wstring label) {
    std::string reason;
    if (!temp_.Create(L"GcCapture" + label, reason)) {
      throw PrerequisiteFailure("创建用例临时目录失败：" + reason);
    }
    dir_ = temp_.Path();
  }

  ScriptFixture(const ScriptFixture&) = delete;
  ScriptFixture& operator=(const ScriptFixture&) = delete;

  const std::wstring& Directory() const { return dir_; }

  // TempDirectory::Path() 不带结尾分隔符，拼接必须显式加，否则文件会落在目录外面。
  std::wstring PathOf(std::wstring_view name) const { return dir_ + L"\\" + std::wstring(name); }

  // 按字节原样写数据文件（不追加任何换行）。
  void WriteData(std::wstring_view name, std::string_view bytes) {
    const std::wstring path = PathOf(name);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw PrerequisiteFailure("无法写入用例数据文件");
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
      throw PrerequisiteFailure("用例数据文件写入不完整");
    }
  }

  // 批处理一律 ASCII（第一行关掉回显，输出里就只有脚本自己写的内容）。
  void WriteScript(std::wstring_view name, std::string_view body) {
    WriteData(name, std::string("@echo off\r\n") + std::string(body));
  }

  [[nodiscard]] SubprocessRunResult Run(std::wstring_view name, size_t maxBytes = kDefaultCap,
                                        unsigned long timeoutMs = kProcessTimeoutMs,
                                        unsigned long drainMs = kBigDrainMs) const {
    return gc::platform::RunHiddenCaptured(CmdPath(), {L"/d", L"/c", PathOf(name)}, dir_, timeoutMs,
                                           nullptr, maxBytes, drainMs);
  }

private:
  TempDirectory temp_;
  std::wstring dir_;
};

std::string BytesOf(size_t count, char fill = 'x') { return std::string(count, fill); }

// 先用不设限的一次运行问出「这个脚本到底产出了多少字节」，再拿它当上限做边界用例，
// 免得把 cmd 的换行习惯硬编进断言。
size_t ProbeSize(const ScriptFixture& fixture, std::wstring_view script) {
  const SubprocessRunResult run = fixture.Run(script);
  if (!run.started || !run.exited || run.exitCode != 0 || !run.stdoutCapture.Complete()) {
    throw PrerequisiteFailure("用于探长度的脚本没有正常跑完：started=" +
                              std::to_string(run.started ? 1 : 0) +
                              " exited=" + std::to_string(run.exited ? 1 : 0) + " timedOut=" +
                              std::to_string(run.timedOut ? 1 : 0) + " exit=" +
                              std::to_string(run.exitCode) + " stdout字节=" +
                              std::to_string(run.stdoutCapture.observedBytes) + " 命令行=" +
                              gc::platform::Utf16ToUtf8(run.commandLine));
  }
  return run.stdoutCapture.observedBytes;
}

}  // namespace

// ---- 保留上限：恰好、超过、以及超限后仍在排空 ----

GC_TEST(capture_pipe_output_exactly_at_cap_is_complete) {
  ScriptFixture fixture(L"exact");
  fixture.WriteData(L"payload.bin", BytesOf(4000));
  fixture.WriteScript(L"emit.cmd", "type payload.bin\r\n");
  const size_t produced = ProbeSize(fixture, L"emit.cmd");

  const SubprocessRunResult run = fixture.Run(L"emit.cmd", produced);
  GC_CHECK(run.started);
  GC_CHECK(run.exited);
  GC_CHECK(run.exitCode == 0);
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete,
                   "恰好等于上限是完整读取，不是截断");
  GC_CHECK(run.stdoutCapture.observedBytes == produced);
  GC_CHECK(run.stdoutCapture.bytes.size() == produced);
  GC_CHECK(run.stderrCapture.state == StreamCaptureState::complete);
  GC_CHECK(run.stderrCapture.bytes.empty());
}

GC_TEST(capture_pipe_output_over_cap_is_an_explicit_failure) {
  ScriptFixture fixture(L"overcap");
  fixture.WriteData(L"payload.bin", BytesOf(4000));
  fixture.WriteScript(L"emit.cmd", "type payload.bin\r\n");
  const size_t produced = ProbeSize(fixture, L"emit.cmd");

  const SubprocessRunResult run = fixture.Run(L"emit.cmd", produced - 1);
  GC_CHECK(run.started && run.exited);
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::truncated,
                   "超过一个字节也要明确报告截断，而不是少一条就算完");
  GC_CHECK(run.stdoutCapture.bytes.size() == produced - 1);
  GC_CHECK_MESSAGE(run.stdoutCapture.observedBytes == produced,
                   "截断时照样报出总字节数，界面才能说「还差多少」");
  GC_CHECK_MESSAGE(!run.AllStreamsComplete(), "截断的输出不能被当成完整快照");
  GC_CHECK(!run.stdoutCapture.note.empty());
}

GC_TEST(capture_pipe_keeps_draining_after_the_cap) {
  // 远超上限：多出来的内容丢弃，但管道要一直读到 EOF。半途停止读取会让子进程被写端堵死；
  // 这里既验证不卡死，也验证总字节数记得住。
  ScriptFixture fixture(L"drain");
  fixture.WriteData(L"payload.bin", BytesOf(3u * 1024u * 1024u));
  fixture.WriteScript(L"emit.cmd", "type payload.bin\r\n");
  const SubprocessRunResult run = fixture.Run(L"emit.cmd", 64u * 1024u);
  GC_CHECK(run.started);
  GC_CHECK(run.exited);
  GC_CHECK(run.exitCode == 0);
  GC_CHECK(run.stdoutCapture.state == StreamCaptureState::truncated);
  GC_CHECK(run.stdoutCapture.bytes.size() == 64u * 1024u);
  GC_CHECK(run.stdoutCapture.observedBytes == 3u * 1024u * 1024u);
}

GC_TEST(capture_pipe_empty_output_is_a_complete_answer) {
  ScriptFixture fixture(L"empty");
  fixture.WriteScript(L"quiet.cmd", "rem no output on purpose\r\n");
  const SubprocessRunResult run = fixture.Run(L"quiet.cmd");
  GC_CHECK(run.started && run.exited && run.exitCode == 0);
  GC_CHECK(run.stdoutCapture.state == StreamCaptureState::complete);
  GC_CHECK(run.stderrCapture.state == StreamCaptureState::complete);
  GC_CHECK(run.stdoutCapture.bytes.empty());
}

// ---- 进程很快就退出：管道里已缓冲的输出必须先排空 ----

GC_TEST(capture_pipe_drains_buffered_bytes_after_fast_exit) {
  // 一次写出远大于管道缓冲的数据后立刻退出。旧实现进程一退出就取消读线程，
  // 缓冲区里没读完的字节会静默丢失；现在必须一字不少地读到 EOF。
  ScriptFixture fixture(L"buffered");
  fixture.WriteData(L"payload.bin", BytesOf(700u * 1024u));
  fixture.WriteScript(L"emit.cmd", "type payload.bin\r\n");
  const size_t produced = ProbeSize(fixture, L"emit.cmd");
  GC_CHECK_MESSAGE(produced > 64u * 1024u, "写入量要超过管道缓冲，才谈得上「排空」");

  const SubprocessRunResult run = fixture.Run(L"emit.cmd");
  GC_CHECK(run.exited);
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete, "进程退出后仍要读到 EOF");
  GC_CHECK(run.stdoutCapture.bytes.size() == produced);
}

// ---- 两条流同时大量输出 ----

GC_TEST(capture_pipe_handles_both_streams_at_full_rate) {
  // 父脚本往 stderr 灌一份，同时用 start /b 再起一个进程往 stdout 灌另一份：
  // 两条管道真的同时被写满。单线程轮读会在其中一条堵住时把另一条也拖死。
  ScriptFixture fixture(L"twostreams");
  fixture.WriteData(L"out.bin", BytesOf(900u * 1024u, 'x'));
  fixture.WriteData(L"err.bin", BytesOf(900u * 1024u, 'y'));
  fixture.WriteScript(L"both.cmd",
                      "start /b cmd /d /c type out.bin\r\n"
                      "type err.bin 1>&2\r\n");
  const SubprocessRunResult run = fixture.Run(L"both.cmd", 128u * 1024u);
  GC_CHECK(run.started);
  GC_CHECK(run.exited);
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::truncated, "标准输出应读满并判截断");
  GC_CHECK_MESSAGE(run.stderrCapture.state == StreamCaptureState::truncated, "标准错误应读满并判截断");
  GC_CHECK(run.stdoutCapture.bytes.size() == 128u * 1024u);
  GC_CHECK(run.stderrCapture.bytes.size() == 128u * 1024u);
  // 两条流的内容不许互相串。
  GC_CHECK(run.stdoutCapture.bytes.find('y') == std::string::npos);
  GC_CHECK(run.stderrCapture.bytes.find('x') == std::string::npos);
  GC_CHECK(run.stdoutCapture.observedBytes >= 900u * 1024u);
  GC_CHECK(run.stderrCapture.observedBytes >= 900u * 1024u);
}

// ---- 子进程的子孙仍占着写端：有界收尾，并明确「没读到结尾」 ----

GC_TEST(capture_pipe_reports_unfinished_pipe_when_descendant_holds_it) {
  ScriptFixture fixture(L"holder");
  // 占位形态用内联命令：子 cmd 约 3 秒才退出，期间持有继承来的两个写端。
  // 收尾期限只有 400 毫秒（两路各自 400 毫秒，合计不到 1 秒），因此两路都必然落在 abandoned，
  // 判定不依赖「谁先退出」的时序。它自然结束后不留残骸（末尾再等它退出，目录才删得掉）。
  fixture.WriteScript(L"main.cmd",
                      "echo marker\r\n"
                      "start /b cmd /d /c ping -n 4 127.0.0.1\r\n");
  const SubprocessRunResult run = fixture.Run(L"main.cmd", kDefaultCap, kProcessTimeoutMs, 400);
  GC_CHECK(run.started);
  GC_CHECK_MESSAGE(run.exited, "父脚本本身要能判定为已退出");
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::abandoned,
                   "写端没全关就要报「未能读到结尾」，不能当作 EOF");
  GC_CHECK_MESSAGE(run.stderrCapture.state == StreamCaptureState::abandoned, "标准错误同理");
  GC_CHECK_MESSAGE(run.stdoutCapture.bytes.rfind("marker", 0) == 0,
                   "中止前已经读到的字节要保留，不能整段丢掉");
  GC_CHECK(!run.stdoutCapture.note.empty());
  GC_CHECK(!run.AllStreamsComplete());
  // 我们是在子孙还在写的时候放弃的：等它自己退出再让夹具删目录，否则临时目录会被它占着删不掉。
  ::Sleep(4000);
}

GC_TEST(capture_pipe_reaches_eof_once_descendant_holds_it_briefly) {
  // 对照形态：子孙只活约一秒就退出，收尾期限内管道自然结束——这就该判完整，而不是中止。
  // 注意用 start /b cmd /d /c <命令> 这种内联形态；实测 `start /b "" "某个.cmd"` 即便子脚本
  // 立刻退出，写端仍会被新控制台的宿主进程长期占住，那就不是「会自然结束」的对照了。
  ScriptFixture fixture(L"shortlived");
  fixture.WriteScript(L"main.cmd",
                      "echo marker\r\n"
                      "start /b cmd /d /c ping -n 2 127.0.0.1 \r\n");
  const ULONGLONG beginMs = ::GetTickCount64();
  const SubprocessRunResult run = fixture.Run(L"main.cmd", kDefaultCap, kProcessTimeoutMs, 8000);
  const ULONGLONG elapsedMs = ::GetTickCount64() - beginMs;
  GC_CHECK(run.started && run.exited);
  GC_CHECK_MESSAGE(run.stdoutCapture.state == StreamCaptureState::complete,
                   "等到 EOF 才算完整；这里的子孙只占管道约一秒，实际状态=" +
                       gc::platform::Utf16ToUtf8(std::wstring(run.stdoutCapture.StateLabel())) +
                       " 说明=" + gc::platform::Utf16ToUtf8(run.stdoutCapture.note) +
                       " 用时毫秒=" + std::to_string(elapsedMs));
  GC_CHECK_MESSAGE(elapsedMs < 7000, "子孙退出后应当立刻等到 EOF，不该把收尾期限等满");
  GC_CHECK(run.stdoutCapture.bytes.find("marker") != std::string::npos);
}

// ---- 启动失败与超时：与「退出码」互不冒充 ----

GC_TEST(capture_pipe_launch_failure_is_not_an_exit_code) {
  ScriptFixture fixture(L"launch");
  const std::wstring missing = fixture.Directory() + L"\\no-such-program.exe";
  const SubprocessRunResult run =
      gc::platform::RunHiddenCaptured(missing, {}, fixture.Directory(), kFastTimeoutMs);
  GC_CHECK_MESSAGE(!run.started, "程序不存在时不能报「已启动」");
  GC_CHECK(run.launchError != 0);
  GC_CHECK(!run.launchErrorText.empty());
  GC_CHECK(!run.exited);
  GC_CHECK(!run.timedOut);
  GC_CHECK_MESSAGE(run.exitCode == 0, "没有进程就没有退出码，不能用某个数值冒充失败");
  GC_CHECK(run.stdoutCapture.state != StreamCaptureState::complete);
  GC_CHECK(run.stderrCapture.state != StreamCaptureState::complete);
}

GC_TEST(capture_pipe_timeout_is_reported_as_timeout) {
  ScriptFixture fixture(L"timeout");
  // cmd 把 ping 当子进程等：终止 cmd 之后 ping 还会写约 2 秒，收尾必须自己有界。
  fixture.WriteScript(L"slow.cmd", "ping -n 3 127.0.0.1\r\n");
  const ULONGLONG start = ::GetTickCount64();
  const SubprocessRunResult run = fixture.Run(L"slow.cmd", kDefaultCap, 700, 300);
  const ULONGLONG elapsed = ::GetTickCount64() - start;
  GC_CHECK(run.started);
  GC_CHECK_MESSAGE(run.timedOut, "超时要点名报出来");
  GC_CHECK(!run.exited);
  GC_CHECK_MESSAGE(run.terminated, "被本进程终止后要确认它真的结束");
  GC_CHECK(run.exitCode != 0);  // 被终止的退出码是我们给的哨兵值，不是程序自己的回答
  // 关闭过程有界：超时 700 + 终止确认 + 收尾 300，留足余量也应远小于 10 秒。
  GC_CHECK_MESSAGE(elapsed < 10000, "收尾必须在有界期限内结束");
  // 被留下的那个 ping 会自己退出；等它一下，用例目录才删得干净（不在 %TEMP% 留残骸）。
  ::Sleep(2500);
}

// ---- 并发启动：句柄继承白名单要挡住「别人的管道」 ----

GC_TEST(capture_pipe_concurrent_launches_do_not_share_pipes) {
  // 一路快退出、一路慢退出，几乎同时启动。没有继承句柄白名单时，先启动那一路的管道写端
  // 会被后启动的子进程继承，于是它要等那个无关进程退出才可能读到 EOF（这里判定为 abandoned）。
  // 加上白名单后每一路只拿到属于自己的那两个写端。重复多轮，专门盯这个竞争窗口。
  ScriptFixture fixture(L"concurrent");
  fixture.WriteScript(L"slow.cmd", "echo slow-marker\r\nping -n 3 127.0.0.1\r\n");
  fixture.WriteScript(L"quick.cmd", "echo quick-marker\r\n");

  for (int round = 0; round < 6; ++round) {
    SubprocessRunResult slowResult;
    SubprocessRunResult quickResult;
    std::thread slowThread([&]() { slowResult = fixture.Run(L"slow.cmd", kDefaultCap, kProcessTimeoutMs, 800); });
    std::thread quickThread([&]() { quickResult = fixture.Run(L"quick.cmd", kDefaultCap, kProcessTimeoutMs, 800); });
    slowThread.join();
    quickThread.join();

    GC_CHECK_MESSAGE(slowResult.started && quickResult.started, "两个子进程都要启动成功");
    GC_CHECK_MESSAGE(slowResult.stdoutCapture.state == StreamCaptureState::complete,
                     "慢的那一路不能被别人的写端拖成「读不到结尾」");
    GC_CHECK_MESSAGE(quickResult.stdoutCapture.state == StreamCaptureState::complete,
                     "快的那一路同理");
    GC_CHECK(slowResult.stdoutCapture.bytes.find("slow-marker") != std::string::npos);
    GC_CHECK(quickResult.stdoutCapture.bytes.find("quick-marker") != std::string::npos);
    // 各自的输出绝不混进对方：混了就说明句柄走错了进程。
    GC_CHECK(quickResult.stdoutCapture.bytes.find("slow-marker") == std::string::npos);
    GC_CHECK(slowResult.stdoutCapture.bytes.find("quick-marker") == std::string::npos);
  }
}

GC_TEST(capture_pipe_repeated_large_output_runs_stay_correct) {
  // 与竞争相关的另一条路径重复跑：截断判定依赖「读满上限后还能等到 EOF」。
  ScriptFixture fixture(L"repeat");
  fixture.WriteData(L"payload.bin", BytesOf(300u * 1024u));
  fixture.WriteScript(L"emit.cmd", "type payload.bin\r\n");
  for (int round = 0; round < 5; ++round) {
    const SubprocessRunResult run = fixture.Run(L"emit.cmd", 100u * 1024u);
    GC_CHECK(run.exited);
    GC_CHECK(run.stdoutCapture.state == StreamCaptureState::truncated);
    GC_CHECK(run.stdoutCapture.bytes.size() == 100u * 1024u);
    GC_CHECK_MESSAGE(run.stdoutCapture.observedBytes == 300u * 1024u, "重复运行里总字节数不能飘");
  }
}
