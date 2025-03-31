#include "platform/windows/ansi_text.h"

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace gc::platform {

bool EncodeToSystemAnsi(std::wstring_view text, std::string& outBytes) {
  outBytes.clear();
  if (text.empty()) {
    return true;
  }
  const int needed = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return false;  // 含码页无法表示的字符（未提供默认字符时 WideCharToMultiByte 失败）。
  }
  std::vector<char> buffer(static_cast<size_t>(needed));
  int usedDefault = 0;
  const int written =
      ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(), static_cast<int>(text.size()),
                            buffer.data(), needed, nullptr, &usedDefault);
  if (written <= 0 || usedDefault != 0 || static_cast<size_t>(written) != buffer.size()) {
    return false;
  }
  // 往返校验必须用同一个码页（CP_ACP）解回来比对；若按 UTF-8 解码，
  // 在 GBK 一类的机器上会把合法中文误判成“不可表示”，整个操作被无理由拒绝
  // —— 本机码页是 UTF-8，这类错误在本机不会暴露，所以要写对而不是靠本机测试发现。
  const int wideNeeded = ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, buffer.data(), written,
                                               nullptr, 0);
  if (wideNeeded != static_cast<int>(text.size())) {
    return false;
  }
  std::vector<wchar_t> roundTrip(static_cast<size_t>(wideNeeded));
  if (::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, buffer.data(), written, roundTrip.data(),
                            wideNeeded) != static_cast<int>(text.size())) {
    return false;
  }
  if (std::wstring_view(roundTrip.data(), static_cast<size_t>(wideNeeded)) != text) {
    return false;
  }
  outBytes.assign(buffer.data(), static_cast<size_t>(written));
  return true;
}

}  // namespace gc::platform
