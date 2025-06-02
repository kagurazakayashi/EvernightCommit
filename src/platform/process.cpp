#include "platform/process.h"

namespace gc::platform {

std::wstring_view StreamCapture::StateLabel() const noexcept {
  switch (state) {
    case StreamCaptureState::complete:
      return L"完整";
    case StreamCaptureState::truncated:
      return L"超过上限被截断";
    case StreamCaptureState::readFailed:
      return L"读取失败";
    case StreamCaptureState::abandoned:
      return L"未能读到结尾";
    case StreamCaptureState::notStarted:
      return L"未读取";
  }
  return L"未读取";
}

std::wstring_view ProcessTerminationLabel(ProcessTermination termination) noexcept {
  switch (termination) {
    case ProcessTermination::notStarted:
      return L"未启动";
    case ProcessTermination::exited:
      return L"正常结束";
    case ProcessTermination::signaled:
      return L"被信号终止";
    case ProcessTermination::timedOut:
      return L"超时终止";
    case ProcessTermination::canceled:
      return L"取消终止";
    case ProcessTermination::unknown:
      return L"结束状态未知";
  }
  return L"结束状态未知";
}

}  // namespace gc::platform
