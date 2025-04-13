#pragma once

#include <windows.h>

namespace gc::platform {

// 本机当前时间，供日期时间控件取初值。
// 时区名与「某个墙上时间实际生效的 UTC 偏移」见 platform/windows/local_time。
[[nodiscard]] SYSTEMTIME CurrentLocalTime() noexcept;

}  // namespace gc::platform
