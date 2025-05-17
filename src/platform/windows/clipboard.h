#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// 把一段 UTF-16 文本放进系统剪贴板（CF_UNICODETEXT）。
//
// 用途边界：这是「用户只想把恢复命令复制走、自己去执行」那条路的落点——恢复入口把精确命令
// 连同目标与风险显示给用户，用户点「复制命令」时走这里；本程序绝不因为复制成功就代为执行。
//
// ownerWindow 提供剪贴板归属（可为空指针）。剪贴板被别的程序临时占用时有界重试；
// 任何一步失败都返回 false 并给出具体原因，绝不谎报「已复制」。
[[nodiscard]] bool CopyTextToClipboard(void* ownerWindow, std::wstring_view text, std::wstring* failure);

}  // namespace gc::platform
