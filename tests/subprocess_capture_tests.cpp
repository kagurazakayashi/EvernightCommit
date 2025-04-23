#include "support/tiny_test.h"

#include <string>
#include <vector>

#include "platform/windows/git_query_result.h"
#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::platform::StreamCapture;
using gc::platform::StreamCaptureState;
using gc::platform::SubprocessRunResult;

// 造一条「平台层已经收工」的捕获结果，只填判定要看的字段。
StreamCapture Capture(StreamCaptureState state, std::string bytes, std::wstring note = {},
                      size_t maxBytes = 1024, size_t observed = 0) {
  StreamCapture capture;
  capture.state = state;
  capture.bytes = std::move(bytes);
  capture.note = std::move(note);
  capture.maxBytes = maxBytes;
  capture.observedBytes = observed == 0 ? capture.bytes.size() : observed;
  return capture;
}

SubprocessRunResult Finished(StreamCapture stdoutCapture, StreamCapture stderrCapture) {
  SubprocessRunResult run;
  run.started = true;
  run.exited = true;
  run.stdoutCapture = std::move(stdoutCapture);
  run.stderrCapture = std::move(stderrCapture);
  return run;
}

std::string Utf8(std::wstring_view text) { return gc::platform::Utf16ToUtf8(text); }

}  // namespace

// ---- 五种收尾形态各自有独立的名字，界面才说得清「为什么读不全」 ----

GC_TEST(capture_states_are_named_apart) {
  struct Expected {
    StreamCaptureState state;
    const wchar_t* mustContain;
  };
  const Expected expected[] = {
      {StreamCaptureState::complete, L"完整"},
      {StreamCaptureState::truncated, L"截断"},
      {StreamCaptureState::readFailed, L"读取失败"},
      {StreamCaptureState::abandoned, L"未能读到结尾"},
      {StreamCaptureState::notStarted, L"未读取"},
  };
  for (const Expected& item : expected) {
    StreamCapture capture;
    capture.state = item.state;
    const std::wstring_view label = capture.StateLabel();
    GC_CHECK_MESSAGE(label.find(item.mustContain) != std::wstring_view::npos,
                     "状态标签要能区分：" + Utf8(label));
  }
  // 「完整」与「未读取」不能共用一个说法：前者是答复，后者是没答案。
  StreamCapture complete;
  complete.state = StreamCaptureState::complete;
  StreamCapture none;
  none.state = StreamCaptureState::notStarted;
  GC_CHECK(complete.StateLabel() != none.StateLabel());
  GC_CHECK(complete.Complete() && !none.Complete());
}

GC_TEST(capture_all_streams_complete_needs_both_streams) {
  SubprocessRunResult run = Finished(Capture(StreamCaptureState::complete, "out"),
                                     Capture(StreamCaptureState::complete, ""));
  GC_CHECK(run.AllStreamsComplete());
  run.stderrCapture = Capture(StreamCaptureState::truncated, "err", L"超过上限", 4, 9);
  GC_CHECK_MESSAGE(!run.AllStreamsComplete(), "任意一路没读全就不能说输出完整");
}

// ---- 平台层 → git 层的转换器：完整度与原因都在这里落地 ----

GC_TEST(capture_bridge_accepts_complete_stdout) {
  const SubprocessRunResult run = Finished(Capture(StreamCaptureState::complete, "git version 2.53.0"),
                                           Capture(StreamCaptureState::complete, ""));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK(result.started);
  GC_CHECK(result.exited);
  GC_CHECK(result.outputComplete);
  GC_CHECK(result.errorComplete);
  GC_CHECK(result.incompleteReason.empty());
  GC_CHECK(result.utf16Output == L"git version 2.53.0");
}

GC_TEST(capture_bridge_marks_truncated_stdout_incomplete) {
  const SubprocessRunResult run =
      Finished(Capture(StreamCaptureState::truncated, "1 .M N... 100644 100644 100", L"超过上限", 30, 5000),
               Capture(StreamCaptureState::complete, ""));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK_MESSAGE(!result.outputComplete, "超过上限的输出必须判为不完整");
  GC_CHECK(result.incompleteReason.find(L"标准输出") != std::wstring::npos);
  GC_CHECK(result.incompleteReason.find(L"超过上限") != std::wstring::npos);
}

GC_TEST(capture_bridge_marks_abandoned_stdout_incomplete) {
  const SubprocessRunResult run =
      Finished(Capture(StreamCaptureState::abandoned, "refs/heads/ma", L"管道没有结束"),
               Capture(StreamCaptureState::complete, ""));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK_MESSAGE(!result.outputComplete, "没等到 EOF 的输出不能当完整答复");
  GC_CHECK(result.incompleteReason.find(L"没到结尾") != std::wstring::npos ||
           result.incompleteReason.find(L"未能读到结尾") != std::wstring::npos);
}

GC_TEST(capture_bridge_marks_read_failure_incomplete) {
  const SubprocessRunResult run =
      Finished(Capture(StreamCaptureState::readFailed, "", L"读取管道失败：数据错误"),
               Capture(StreamCaptureState::complete, ""));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK_MESSAGE(!result.outputComplete, "读取失败不是「Git 没有输出」");
  GC_CHECK(result.incompleteReason.find(L"读取管道失败") != std::wstring::npos);
}

GC_TEST(capture_bridge_refuses_invalid_utf8_tail) {
  // 三个字节起步的多字节字符只剩两个字节（记录被切断）：宽松解码会造出一个替代字符，
  // 那就等于用另一个路径去做判断，必须判为不完整。
  std::string broken = "name \xE4\xB8";  // 「上」的前两字节
  const SubprocessRunResult run = Finished(Capture(StreamCaptureState::complete, broken),
                                           Capture(StreamCaptureState::complete, ""));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK_MESSAGE(!result.outputComplete, "残缺的 UTF-8 序列不得解码成另一个值");
  GC_CHECK(result.incompleteReason.find(L"UTF-8") != std::wstring::npos);
}

GC_TEST(capture_bridge_multibyte_content_is_decoded_whole) {
  // 多字节字符在读取时跨块无所谓：字节先攒完整，再整段解码，因此结果与分块方式无关。
  const std::wstring expected = L"上.txt 中 文件 emoji 🌙 end";
  std::string bytes = gc::platform::Utf16ToUtf8(expected);
  const SubprocessRunResult run = Finished(Capture(StreamCaptureState::complete, bytes),
                                           Capture(StreamCaptureState::complete, ""));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK(result.outputComplete);
  GC_CHECK_MESSAGE(result.utf16Output == expected, "解码结果要原样还原多字节内容");
}

GC_TEST(capture_bridge_records_stderr_incompleteness_separately) {
  // 标准错误只影响诊断文字：它读不全时标准输出照样可用，但必须记录在案。
  const SubprocessRunResult run = Finished(Capture(StreamCaptureState::complete, "ok"),
                                           Capture(StreamCaptureState::abandoned, "fatal: ind", L"超时"));
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK(result.outputComplete);
  GC_CHECK_MESSAGE(!result.errorComplete, "标准错误的收尾形态要单独带出");
  GC_CHECK(result.incompleteReason.find(L"标准错误") != std::wstring::npos);
  GC_CHECK(result.utf16Error == L"fatal: ind");
}

GC_TEST(capture_bridge_carries_launch_failure_text) {
  gc::platform::SubprocessRunResult run;
  run.started = false;
  run.launchError = 2;
  run.launchErrorText = L"系统找不到指定的文件。";
  run.stdoutCapture = Capture(StreamCaptureState::notStarted, "");
  run.stderrCapture = Capture(StreamCaptureState::notStarted, "");
  const gc::git::GitQueryResult result = gc::platform::MakeGitQueryResult(run);
  GC_CHECK_MESSAGE(!result.started, "启动失败要照原样传播");
  GC_CHECK(result.launchDetail == L"系统找不到指定的文件。");
}
