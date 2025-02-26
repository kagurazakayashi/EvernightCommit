#pragma once

#include <windows.h>

#include <string>

namespace gc::platform {

// 形如 “本机时区：中国标准时间 (UTC+08:00)”，使用当前用户的时区名与标准/夏令时偏差。
[[nodiscard]] std::wstring LocalTimeZoneLabel();

// 本机当前时间，供日期时间控件取初值。
[[nodiscard]] SYSTEMTIME CurrentLocalTime() noexcept;

}  // namespace gc::platform
