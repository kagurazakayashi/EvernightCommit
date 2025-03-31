#pragma once

#include <string>
#include <string_view>

namespace gc::platform {

// 把 UTF-16 文本编码为“系统 ANSI 码页”（GetACP）字节。
// cmd.exe 解析批处理脚本与继承来的命令行时使用系统 ANSI 码页，因此进入一次性脚本的
// 路径与参数必须先按该码页编码；不可表示的字符（无最佳匹配）一律返回 false，
// 由调用方拒绝启动并给出可读原因，绝不降级成 '?' 写入脚本。
// 空串编码结果为空串且返回 true。
[[nodiscard]] bool EncodeToSystemAnsi(std::wstring_view text, std::string& outBytes);

}  // namespace gc::platform
