#pragma once

#include <windows.h>

#include <string>

namespace gc::platform {

// Common Controls v6（视觉样式、ListView、DateTimePicker 等）初始化，进程范围内一次性完成。
class CommonControls {
public:
  CommonControls();
  CommonControls(const CommonControls&) = delete;
  CommonControls& operator=(const CommonControls&) = delete;
};

// STA COM 套间：路径选择对话框需要；析构时按配对规则释放。
class ComApartment {
public:
  ComApartment();
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;
  ~ComApartment();
  [[nodiscard]] bool Succeeded() const noexcept { return succeeded_; }

private:
  bool succeeded_ = false;
};

[[nodiscard]] std::wstring LastErrorText(DWORD code);

}  // namespace gc::platform
