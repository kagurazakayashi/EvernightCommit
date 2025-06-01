#pragma once

#include <cstdint>
#include <string>

#include "git/commit_date.h"

namespace gc::platform {

// ---------- 本机时区与「墙上时间 ↔ UTC 瞬间」的能力契约（平台无关部分） ----------
//
// 为什么偏移必须由操作系统算：界面上填的值是本机墙上时间，而同一个墙上时间在夏令时前后
// 可能属于两个不同的瞬间；跨时区规则（含历史改动）只有系统的时区数据库知道。
// 换算不出偏移就凭本机当前时刻猜一个填进去，挑出来的时间会被挪几个小时。
//
// 纯历法算法在 git/commit_date；本文件只声明「问操作系统」的那几句，不含任何原生时间结构。
// Windows 实现见 src/platform/windows/local_time.cpp（SYSTEMTIME / TIME_ZONE_INFORMATION）；
// Linux 实现后续用 tzset/localtime_r 在同一份契约上另写，调用方一个字都不改。
//
// 换算不出来的那一刻一律 valid=false 并带上面向用户的原因，绝不返回一个猜出来的瞬间。

// 一个本机墙上时间换算出来的瞬间。
struct LocalInstant {
  bool valid = false;
  // 墙上时间在夏令时“回拨”的那一小时里出现两次时为 true：utcEpochSeconds/offsetMinutes
  // 是确定选中的那一个（Linux 实现固定选较早的瞬间），alternate* 是另一个。
  // 墙上时间落在夏令时“跳过”的空洞里时 valid=false（不猜一个不存在的时刻）。
  bool ambiguous = false;
  git::CivilTime wall{};         // 用户看到并挑选的墙上时间
  long long utcEpochSeconds = 0; // 这一刻真正的 UTC 秒数（交给 Git 的值用它）
  int offsetMinutes = 0;         // 这一刻实际生效的 UTC 偏移（含夏令时）
  long long alternateUtcEpochSeconds = 0;  // ambiguous 时的另一个瞬间
  int alternateOffsetMinutes = 0;          // ambiguous 时的另一个偏移
  std::wstring failureReason;    // valid 为 false 时面向用户的说明
};

// 用本机的时区规则（含那一天的夏令时状态）把墙上时间换算成 UTC 秒与该时刻的实际偏移。
// 墙上时间不成立、或换算失败时返回 valid 为 false，并给出原因；绝不返回一个猜出来的瞬间。
[[nodiscard]] LocalInstant ResolveLocalWallTime(const git::CivilTime& wall);

// 本机此刻：墙上时间 + UTC 秒 + 此刻的偏移。
[[nodiscard]] LocalInstant CurrentLocalInstant();

// 本机时区名字（如「中国标准时间」/「China Standard Time」）；取不到时返回空串。
// 与 git/commit_date 的偏移文本搭配成「本机时区：… 此刻 UTC+08:00」这样的界面说明。
[[nodiscard]] std::wstring LocalTimeZoneName();

// Unix 秒 → 本机时区的「YYYY-MM-DD HH:MM:SS」展示文本（提交历史列表的时间列用）。
// 换算走系统的时区规则（含那一天实际的夏令时状态），与 ResolveLocalWallTime 同一套依据。
// 越界或换算失败时退回裸秒数的文本：时间列宁可难看地可读，也不能留一个空格让用户以为没读到。
[[nodiscard]] std::wstring FormatLocalEpochSeconds(long long epochSeconds);

}  // namespace gc::platform
