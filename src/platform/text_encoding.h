#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace gc::platform {

// ---------- 显式文本表示（跨平台编码边界） ----------
//
// 本工程只有两种文本表示，含义固定、不随平台改变：
//   * std::string          —— 字节序列。写入文件、Git 原始 stdout/stderr、POSIX 路径与环境项
//                             一律是 UTF-8；非法 UTF-8 就是非法数据，不做“猜测码页”。
//   * std::wstring         —— 本机宽字符。Windows 是 UTF-16 单元，Linux 是 UTF-32 码位。
//                             它是界面/业务层沿用的表示，不是“跨平台 UTF-16”。
// 因此任何依赖单元数的判定（截断、长度上限）必须用下面的 Utf16UnitsOf /
// TruncateToUtf16Units，不能直接比较 std::wstring::size()——那在 Linux 上会把
// 非 BMP 字符只算一个单元。
//
// 宽松转换只用于“显示/日志”：非法字节替换成 U+FFFD。路径、文件名、Git 参数与
// 临时文件内容必须用严格版本；严格版本失败时既不写 U+FFFD，也不返回半份结果。
//
// 进程参数边界（后续执行通道使用）：Windows 直接把 std::wstring 交给 CreateProcessW；
// Linux 必须先用 TryWideToUtf8Strict 变成 UTF-8 字节再放进 argv，绝不经过 shell，
// 也绝不用替换字符顶替无法编码的参数。

// UTF-8 → 本机宽字符（宽松）。非法字节按 U+FFFD 替换。
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);

// 本机宽字符 → UTF-8（宽松）。孤立代理项/越界码位按 U+FFFD 替换。
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide);

// 严格 UTF-8 → 本机宽字符。非法序列（截断、过长编码、代理项、>U+10FFFF、CESU）
// 返回 false 且 outText 保持为空。空串成功。
[[nodiscard]] bool TryUtf8ToWideStrict(std::string_view utf8, std::wstring& outText);

// 严格本机宽字符 → UTF-8。孤立代理项（UTF-16 平台）或非法码位（UTF-32 平台）
// 返回 false 且 outBytes 保持为空。空串成功。
[[nodiscard]] bool TryWideToUtf8Strict(std::wstring_view wide, std::string& outBytes);

// UTF-8 是否完全合法（与 TryUtf8ToWideStrict 同一判据，不分配宽字符串）。
[[nodiscard]] bool IsValidUtf8(std::string_view utf8) noexcept;

// 这段文字按 Unicode 码位计有多少个 UTF-16 单元（BMP 1 个，非 BMP 2 个）。
// 非法孤立代理项按 1 个单元计：调用方应先保证输入合法。
[[nodiscard]] size_t Utf16UnitsOf(std::wstring_view wide) noexcept;

// 按 UTF-16 单元上限截断，截点一定落在码位边界。truncated 可空。
[[nodiscard]] std::wstring TruncateToUtf16Units(std::wstring_view wide, size_t limit,
                                                bool* truncated = nullptr);

// ---------- 路径字节边界 ----------
//
// POSIX 路径就是字节；本工程只接受能无损表示为 UTF-8 的文件名。Git 输出/目录枚举
// 里出现非法 UTF-8 时，调用方必须报告“该文件名无法无损显示/操作”，不能先替换成
// U+FFFD 再拿替换后的字符串去访问另一条路径。
[[nodiscard]] bool TryPathBytesFromWide(std::wstring_view widePath, std::string& outBytes);
[[nodiscard]] bool TryWidePathFromBytes(std::string_view bytes, std::wstring& outPath);

}  // namespace gc::platform
