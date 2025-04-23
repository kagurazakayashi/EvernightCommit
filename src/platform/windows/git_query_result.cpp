#include "platform/windows/git_query_result.h"

#include <utility>

#include "platform/windows/utf_text.h"

namespace gc::platform {
namespace {

// 一路输出没读全时的说明：状态标签 + 具体原因的限长摘要（不含输出原文）。
std::wstring StreamReason(std::wstring_view streamName, const StreamCapture& capture) {
  std::wstring text = std::wstring(streamName) + std::wstring(capture.StateLabel());
  if (!capture.note.empty()) {
    text += L"：" + capture.note;
  }
  return text;
}

void AppendReason(std::wstring& reason, std::wstring addition) {
  if (addition.empty()) {
    return;
  }
  if (!reason.empty()) {
    reason += L"；";
  }
  reason += addition;
}

}  // namespace

std::wstring DescribeIncompleteStreams(const SubprocessRunResult& run) {
  std::wstring reason;
  if (!run.stdoutCapture.Complete()) {
    AppendReason(reason, StreamReason(L"标准输出", run.stdoutCapture));
  }
  if (!run.stderrCapture.Complete()) {
    AppendReason(reason, StreamReason(L"标准错误", run.stderrCapture));
  }
  return reason;
}

git::GitQueryResult MakeGitQueryResult(const SubprocessRunResult& run) {
  git::GitQueryResult result;
  result.started = run.started;
  result.timedOut = run.timedOut;
  result.exited = run.exited;
  result.exitCode = static_cast<int>(run.exitCode);
  result.launchDetail = run.launchErrorText;
  // GitQueryResult 的默认值是「完整」（手工桩结果就是给定那份事实），
  // 真实查询反过来：先按没读全处理，只有确认过才翻成完整，免得漏设一位就当可信输出用。
  result.outputComplete = false;
  result.errorComplete = false;

  // 标准错误只是诊断材料：宽松解码，让 Git 的英文错误文案照常可读。
  result.errorComplete = run.stderrCapture.Complete();
  result.utf16Error = Utf8ToUtf16(run.stderrCapture.bytes);

  AppendReason(result.incompleteReason, DescribeIncompleteStreams(run));
  if (run.stdoutCapture.Complete()) {
    std::wstring decoded;
    if (TryUtf8ToUtf16Strict(run.stdoutCapture.bytes, decoded)) {
      result.outputComplete = true;
      result.utf16Output = std::move(decoded);
    } else {
      // 残缺的字节序列解码后会变成另一个值：宁可这份答复整体不用。
      AppendReason(result.incompleteReason,
                   L"标准输出不是有效的 UTF-8 文本（可能含被截断的多字节字符，或该仓库的文件名"
                   L"本来就不是 UTF-8 编码）");
    }
  }
  if (result.incompleteReason.empty() && !result.outputComplete) {
    result.incompleteReason = L"标准输出没有完整读回";
  }
  return result;
}

}  // namespace gc::platform
