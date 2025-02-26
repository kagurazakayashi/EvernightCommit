#include "platform/windows/platform_init.h"

#include <commctrl.h>
#include <objbase.h>

namespace gc::platform {

CommonControls::CommonControls() {
  INITCOMMONCONTROLSEX init{sizeof(INITCOMMONCONTROLSEX),
                            ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_DATE_CLASSES |
                                ICC_UPDOWN_CLASS};
  ::InitCommonControlsEx(&init);
}

ComApartment::ComApartment() {
  const HRESULT result =
      ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  succeeded_ = SUCCEEDED(result);
}

ComApartment::~ComApartment() {
  if (succeeded_) {
    ::CoUninitialize();
  }
}

std::wstring LastErrorText(DWORD code) {
  PWSTR buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length =
      ::FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<PWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return L"Win32 错误 " + std::to_wstring(code);
  }
  std::wstring message(buffer, length);
  ::LocalFree(buffer);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
    message.pop_back();
  }
  return message + L"（错误码 " + std::to_wstring(code) + L"）";
}

}  // namespace gc::platform
