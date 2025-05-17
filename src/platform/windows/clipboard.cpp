#include "platform/windows/clipboard.h"

#include <windows.h>

#include <cstring>

namespace gc::platform {
namespace {

// 剪贴板同一时刻只有一个所有者：被别的程序短暂占用时，重试一小段时间而不是直接放弃。
// 只有「已被占用」这类可重试错误才重试；其它错误立刻如实返回。
bool OpenClipboardBounded(HWND owner) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (::OpenClipboard(owner) != 0) {
      return true;
    }
    const unsigned long error = ::GetLastError();
    if (error != ERROR_ACCESS_DENIED) {
      return false;
    }
    ::Sleep(50);
  }
  return false;
}

}  // namespace

bool CopyTextToClipboard(void* ownerWindow, std::wstring_view text, std::wstring* failure) {
  HWND owner = static_cast<HWND>(ownerWindow);
  if (text.empty()) {
    if (failure != nullptr) {
      *failure = L"没有内容可复制。";
    }
    return false;
  }
  // CF_UNICODETEXT 要求以 NUL 结尾的 UTF-16。先独占申请一块可移动内存填好，再谈剪贴板所有权。
  const size_t byteCount = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, byteCount);
  if (memory == nullptr) {
    if (failure != nullptr) {
      *failure = L"分配剪贴板内存失败，本次没有复制。";
    }
    return false;
  }
  void* destination = ::GlobalLock(memory);
  if (destination == nullptr) {
    ::GlobalFree(memory);
    if (failure != nullptr) {
      *failure = L"锁定剪贴板内存失败，本次没有复制。";
    }
    return false;
  }
  std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
  static_cast<wchar_t*>(destination)[text.size()] = L'\0';
  ::GlobalUnlock(memory);

  if (!OpenClipboardBounded(owner)) {
    ::GlobalFree(memory);  // 打不开剪贴板：这块内存还是自己的，回收。
    if (failure != nullptr) {
      *failure = L"剪贴板正被其它程序占用，本次没有复制。";
    }
    return false;
  }
  bool copied = false;
  unsigned long failureError = 0;
  if (::EmptyClipboard() != 0) {
    // 成功后所有权移交系统：此后绝不能再 GlobalFree 这块内存。
    if (::SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
      memory = nullptr;
      copied = true;
    } else {
      failureError = ::GetLastError();
    }
  } else {
    failureError = ::GetLastError();
  }
  ::CloseClipboard();
  if (memory != nullptr) {
    ::GlobalFree(memory);  // 没移交成功，收回自己申请的那块。
  }
  if (!copied && failure != nullptr) {
    *failure = L"写入剪贴板失败（Windows 错误码 " + std::to_wstring(failureError) + L"），本次没有复制。";
  }
  return copied;
}

}  // namespace gc::platform
