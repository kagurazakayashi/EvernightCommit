#pragma once

#include <windows.h>

#include <string>

#include "git/commit_date.h"

namespace gc::platform {

// 本机时区与「墙上时间 ↔ UTC 瞬间」的换算（Win32 侧；纯算法部分在 git/commit_date）。
//
// 为什么偏移必须由操作系统算：日期时间控件里的值是本机墙上时间，而同一个墙上时间
// 在夏令时前后可能属于两个不同的瞬间；跨时区规则（含历史改动）只有系统的时区数据库知道。
// 换算不出偏移就凭本机当前时刻猜一个填进去，挑出来的时间会被挪几个小时。
//
// SYSTEMTIME 一律取到秒：控件给不出毫秒，提交时间也不需要毫秒。

// SYSTEMTIME → 墙上时间（丢掉星期与毫秒）。
[[nodiscard]] git::CivilTime CivilFromSystemTime(const SYSTEMTIME& value) noexcept;

// 墙上时间 → SYSTEMTIME（星期与毫秒由系统不需要，一律置零）。
[[nodiscard]] SYSTEMTIME SystemTimeFromCivil(const git::CivilTime& value) noexcept;

// 一个本机墙上时间换算出来的瞬间。
struct LocalInstant {
  bool valid = false;
  git::CivilTime wall{};      // 用户看到并挑选的墙上时间
  long long utcEpochSeconds = 0;  // 这一刻真正的 UTC 秒数（交给 Git 的值用它）
  int offsetMinutes = 0;      // 这一刻实际生效的 UTC 偏移（含夏令时）
  std::wstring failureReason;  // valid 为 false 时面向用户的说明
};

// 用本机的时区规则（含那一天的夏令时状态）把墙上时间换算成 UTC 秒与该时刻的实际偏移。
// 墙上时间不成立、或换算失败时返回 valid 为 false，并给出原因；绝不返回一个猜出来的瞬间。
//
// 实测边界（本机 Windows 11 + Git 2.53）：
//   * 夏令时「跳过」的那一小时（例如凌晨 02:30 在开始夏令时那天不存在）与
//     「重复」的那一小时（结束时 02:30 出现两次），系统各自给出一个确定的映射；
//     本函數照实回报系统给的那个偏移，界面显示的偏移与交给 Git 的值因此永远一致。
//   * 偏移按整分钟给出（现实时区都是 15/30/45 分钟的整数倍）。
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
