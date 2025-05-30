#pragma once

#include <windows.h>

#include <string>

#include "git/commit_date.h"
#include "platform/local_time.h"  // LocalInstant 与那几句「问操作系统」的换算

namespace gc::platform {

// Win32 侧的本机时间结构换算：只有这一层认识 SYSTEMTIME。
// SYSTEMTIME 一律取到秒：控件给不出毫秒，提交时间也不需要毫秒。
// 墙上时间↔UTC 瞬间的换算、此刻、时区名字与展示文本都声明在 platform/local_time.h，
// 实现仍在本目录的 local_time.cpp（同一份文件里两套声明各归各位）。

// SYSTEMTIME → 墙上时间（丢掉星期与毫秒）。
[[nodiscard]] git::CivilTime CivilFromSystemTime(const SYSTEMTIME& value) noexcept;

// 墙上时间 → SYSTEMTIME（星期与毫秒由系统不需要，一律置零）。
[[nodiscard]] SYSTEMTIME SystemTimeFromCivil(const git::CivilTime& value) noexcept;

}  // namespace gc::platform
