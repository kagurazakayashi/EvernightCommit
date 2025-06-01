#include "platform/local_time.h"

#include <ctime>

#include <algorithm>
#include <string>
#include <vector>

#include "git/commit_date.h"
#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

std::tm ToTm(const git::CivilTime& wall, int isdst) {
  std::tm value{};
  value.tm_year = wall.year - 1900;
  value.tm_mon = wall.month - 1;
  value.tm_mday = wall.day;
  value.tm_hour = wall.hour;
  value.tm_min = wall.minute;
  value.tm_sec = wall.second;
  value.tm_isdst = isdst;
  return value;
}

bool MatchesWall(const std::tm& value, const git::CivilTime& wall) {
  return value.tm_year == wall.year - 1900 && value.tm_mon == wall.month - 1 && value.tm_mday == wall.day &&
         value.tm_hour == wall.hour && value.tm_min == wall.minute && value.tm_sec == wall.second;
}

// 用系统时区规则把一个“墙上时间”落成一个真实瞬间。mktime 对夏令时空洞会做规范化，
// 因此必须再用 localtime_r 反查：反查回来不是同一个墙上时间，就说明这个时刻不存在。
bool MakeCandidate(const git::CivilTime& wall, int isdst, long long* outEpoch) {
  std::tm local = ToTm(wall, isdst);
  const std::time_t epoch = std::mktime(&local);
  if (epoch == static_cast<std::time_t>(-1)) {
    return false;
  }
  std::tm back{};
  if (::localtime_r(&epoch, &back) == nullptr || !MatchesWall(back, wall)) {
    return false;
  }
  *outEpoch = static_cast<long long>(epoch);
  return true;
}

int OffsetMinutes(long long wallEpoch, long long utcEpoch, bool* ok) {
  const long long offsetSeconds = wallEpoch - utcEpoch;
  const long long rounding = offsetSeconds >= 0 ? 30 : -30;
  const long long minutes = (offsetSeconds + rounding) / 60;
  if (minutes < -1024 || minutes > 1024) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return static_cast<int>(minutes);
}

std::wstring FormatCivil(const std::tm& value) {
  const auto pad = [](int number, int width) {
    std::wstring text = std::to_wstring(number);
    while (static_cast<int>(text.size()) < width) {
      text.insert(text.begin(), L'0');
    }
    return text;
  };
  return pad(value.tm_year + 1900, 4) + L"-" + pad(value.tm_mon + 1, 2) + L"-" + pad(value.tm_mday, 2) + L" " +
         pad(value.tm_hour, 2) + L":" + pad(value.tm_min, 2) + L":" + pad(value.tm_sec, 2);
}

}  // namespace

LocalInstant ResolveLocalWallTime(const git::CivilTime& wall) {
  LocalInstant instant;
  instant.wall = wall;
  if (!git::CivilTimeLooksValid(wall)) {
    instant.failureReason =
        L"这个日期时间不成立（年份要在 1970 到 2099 之间，月日与时分秒也要各自不越界）。";
    return instant;
  }

  ::tzset();

  // 同一个墙上时间最多对应两个瞬间：isdst=-1 让系统选，另外显式试 0/1 以发现“回拨重复”。
  std::vector<long long> candidates;
  for (const int isdst : {-1, 0, 1}) {
    long long epoch = 0;
    if (MakeCandidate(wall, isdst, &epoch) &&
        std::find(candidates.begin(), candidates.end(), epoch) == candidates.end()) {
      candidates.push_back(epoch);
    }
  }
  if (candidates.empty()) {
    // 两种情况都会落到这里：mktime 把墙上时间推进到夏令时之后（反查不相等），
    // 或该时刻在可记录范围之外（例如负 epoch）。
    instant.failureReason =
        L"本机时区无法把这个墙上时间换算成一个真实存在过的时刻（可能落在夏令时开始的空洞，"
        L"或超出 1970 至 2099 年的可记录范围）。";
    return instant;
  }
  std::sort(candidates.begin(), candidates.end());
  const long long chosen = candidates.front();

  long long wallEpoch = 0;
  if (!git::CivilTimeToUtcEpoch(wall, &wallEpoch)) {
    instant.failureReason = L"这个墙上时间超出了可以记录的范围。";
    return instant;
  }
  if (!git::IsStorableCommitEpoch(chosen)) {
    instant.failureReason = L"换算出来的时刻超出了可以记录的范围（1970 至 2099 年之间）。";
    return instant;
  }

  bool offsetOk = false;
  const int offset = OffsetMinutes(wallEpoch, chosen, &offsetOk);
  if (!offsetOk) {
    instant.failureReason = L"本机时区给出的偏移超出现实范围，无法交给 Git。";
    return instant;
  }

  instant.utcEpochSeconds = chosen;
  instant.offsetMinutes = offset;
  instant.valid = true;
  if (candidates.size() > 1) {
    instant.ambiguous = true;
    instant.alternateUtcEpochSeconds = candidates[1];
    bool alternateOk = false;
    instant.alternateOffsetMinutes = OffsetMinutes(wallEpoch, candidates[1], &alternateOk);
    if (!alternateOk) {
      instant.ambiguous = false;
      instant.alternateUtcEpochSeconds = 0;
      instant.alternateOffsetMinutes = 0;
    }
  }
  return instant;
}

LocalInstant CurrentLocalInstant() {
  LocalInstant instant;
  ::tzset();
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  if (::localtime_r(&now, &local) == nullptr) {
    instant.failureReason = L"本机时区没能解释当前时间。";
    return instant;
  }
  instant.wall = git::CivilTime{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                local.tm_hour, local.tm_min, local.tm_sec};
  long long wallEpoch = 0;
  if (!git::CivilTimeToUtcEpoch(instant.wall, &wallEpoch)) {
    instant.failureReason = L"当前时间超出了可以记录的范围。";
    return instant;
  }
  bool offsetOk = false;
  const int offset = OffsetMinutes(wallEpoch, static_cast<long long>(now), &offsetOk);
  if (!offsetOk) {
    instant.failureReason = L"本机时区给出的偏移超出现实范围。";
    return instant;
  }
  instant.utcEpochSeconds = static_cast<long long>(now);
  instant.offsetMinutes = offset;
  instant.valid = true;
  return instant;
}

std::wstring LocalTimeZoneName() {
  ::tzset();
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  if (::localtime_r(&now, &local) == nullptr) {
    return {};
  }
  const char* name = ::tzname[local.tm_isdst > 0 ? 1 : 0];
  if (name == nullptr || name[0] == '\0') {
    return {};
  }
  std::wstring text;
  if (!TryWidePathFromBytes(name, text)) {
    return {};  // 时区名不是 UTF-8：宁可只显示偏移，也不显示替换字符。
  }
  return text;
}

std::wstring FormatLocalEpochSeconds(long long epochSeconds) {
  const std::wstring fallback = std::to_wstring(epochSeconds);
  const std::time_t value = static_cast<std::time_t>(epochSeconds);
  if (static_cast<long long>(value) != epochSeconds) {
    return fallback;
  }
  ::tzset();
  std::tm local{};
  if (::localtime_r(&value, &local) == nullptr) {
    return fallback;
  }
  return FormatCivil(local);
}

}  // namespace gc::platform
