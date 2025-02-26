#include "platform/windows/locale_text.h"

namespace gc::platform {

std::wstring LocalTimeZoneLabel() {
  DYNAMIC_TIME_ZONE_INFORMATION info{};
  const LONG state = ::GetDynamicTimeZoneInformation(&info);
  // 无夏令时的时区返回 TIME_ZONE_ID_UNKNOWN，此时 StandardBias 仍有效。
  const bool daylight = (state == TIME_ZONE_ID_DAYLIGHT);
  const LONG biasMinutes = info.Bias + (daylight ? info.DaylightBias : info.StandardBias);
  const LONG offsetMinutes = -biasMinutes;
  const LONG absolute = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;

  std::wstring offset = L"UTC";
  offset += (offsetMinutes < 0 ? L'-' : L'+');
  const LONG hours = absolute / 60;
  const LONG minutes = absolute % 60;
  if (hours < 10) {
    offset += L'0';
  }
  offset += std::to_wstring(hours) + L':';
  if (minutes < 10) {
    offset += L'0';
  }
  offset += std::to_wstring(minutes);

  std::wstring label = L"本机时区：";
  const wchar_t* name = daylight ? info.DaylightName : info.StandardName;
  if (name[0] != L'\0') {
    label += name;
    label += L' ';
  }
  label += offset;
  return label;
}

SYSTEMTIME CurrentLocalTime() noexcept {
  SYSTEMTIME now{};
  ::GetLocalTime(&now);
  return now;
}

}  // namespace gc::platform
