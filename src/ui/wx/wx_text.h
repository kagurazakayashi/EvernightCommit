#pragma once

#include <string>
#include <string_view>

#include <wx/string.h>

#include "platform/text_encoding.h"

namespace gc::ui::wx {

// wxWidgets 与业务层之间的文本边界：业务层一律 std::wstring（Windows UTF-16 / Linux UTF-32），
// wxString 内部是 UTF-16。这里统一经 UTF-8 中转，避免在 Linux 上按 wchar_t 尺寸直接构造。
[[nodiscard]] inline wxString FromWide(std::wstring_view text) {
  return wxString::FromUTF8(platform::WideToUtf8(text));
}

[[nodiscard]] inline std::wstring ToWide(const wxString& text) {
  return text.ToStdWstring();
}

}  // namespace gc::ui::wx
