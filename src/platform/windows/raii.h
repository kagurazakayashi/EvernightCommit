#pragma once

#include <windows.h>

#include <utility>

namespace gc::platform {

// 顶层窗口句柄的持有者。析构时销毁窗口；窗口过程处理 WM_DESTROY 时用 Disown() 交还所有权，
// 因为此时窗口已在销毁过程中，绝不能再次 DestroyWindow。
class UniqueWindow {
public:
  UniqueWindow() = default;
  explicit UniqueWindow(HWND hwnd) noexcept : hwnd_(hwnd) {}
  UniqueWindow(const UniqueWindow&) = delete;
  UniqueWindow& operator=(const UniqueWindow&) = delete;

  UniqueWindow(UniqueWindow&& other) noexcept : hwnd_(std::exchange(other.hwnd_, nullptr)) {}
  UniqueWindow& operator=(UniqueWindow&& other) noexcept {
    if (this != &other) {
      Reset();
      hwnd_ = std::exchange(other.hwnd_, nullptr);
    }
    return *this;
  }

  ~UniqueWindow() { Reset(); }

  void Reset(HWND hwnd = nullptr) noexcept {
    if (hwnd_ != nullptr) {
      ::DestroyWindow(hwnd_);
    }
    hwnd_ = hwnd;
  }

  // 交还所有权（用于 WM_DESTROY：窗口由系统销毁，不再由本类负责）。
  void Disown() noexcept { hwnd_ = nullptr; }

  [[nodiscard]] HWND get() const noexcept { return hwnd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return hwnd_ != nullptr; }

private:
  HWND hwnd_ = nullptr;
};

// 内核对象句柄（进程/线程/管道）的持有者；INVALID_HANDLE_VALUE 与 nullptr 都不认领。
class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  ~UniqueHandle() { Reset(); }

  void Reset(HANDLE handle = nullptr) noexcept {
    if (Valid()) {
      ::CloseHandle(handle_);
    }
    handle_ = handle;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
  [[nodiscard]] explicit operator bool() const noexcept { return Valid(); }

  // 把所有权交给系统（例如交给 WaitForSingleObject 后由调用方 CloseHandle）。
  HANDLE Release() noexcept { return std::exchange(handle_, nullptr); }

private:
  HANDLE handle_ = nullptr;
};

// GDI 字体句柄的持有者。
class OwnedFont {
public:
  OwnedFont() = default;
  explicit OwnedFont(HFONT font) noexcept : font_(font) {}
  OwnedFont(const OwnedFont&) = delete;
  OwnedFont& operator=(const OwnedFont&) = delete;

  OwnedFont(OwnedFont&& other) noexcept : font_(std::exchange(other.font_, nullptr)) {}
  OwnedFont& operator=(OwnedFont&& other) noexcept {
    if (this != &other) {
      Reset();
      font_ = std::exchange(other.font_, nullptr);
    }
    return *this;
  }

  ~OwnedFont() { Reset(); }

  void Reset(HFONT font = nullptr) noexcept {
    if (font_ != nullptr) {
      ::DeleteObject(font_);
    }
    font_ = font;
  }

  [[nodiscard]] HFONT get() const noexcept { return font_; }

private:
  HFONT font_ = nullptr;
};

}  // namespace gc::platform
