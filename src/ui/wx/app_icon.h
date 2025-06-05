#pragma once

#include <wx/icon.h>

namespace gc::ui::wx {

// 应用图标：从内嵌的 base64 PNG（app_icon_data.h）解码，Windows 与 Linux 共用同一份数据。
// 解码失败返回无效图标——调用方按「没有图标」处理即可，不影响任何业务功能。
[[nodiscard]] wxIcon MakeAppIcon();

}  // namespace gc::ui::wx
