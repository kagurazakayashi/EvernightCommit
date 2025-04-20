#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// 外部命令窗口执行器的可移植逻辑（本模块不触碰任何 Win32 API）：
//   1) 把一次“用户主动执行的 Git 操作”校验并序列化成“操作说明书”（spec），
//      在边界完成注入校验（引号、控制字符、shell 元字符、数量与长度上限）；
//   2) 由命令窗口辅助进程把说明书原样读回，并再次用同一套校验复核后才执行；
//   3) 读取并解释辅助进程写出的标记文件与结果文件，把“进程启动 / 进程退出 /
//      标记文件 / 结果文件”四路事实合成唯一、明确的完成状态。
//
// 编码边界（为什么这里不再有系统 ANSI 码页）：
//   * 说明书与 Git 的参数一律走 UTF-16 内存形态，由平台层用 Unicode API 传递：
//     CreateProcessW 的命令行、工作目录与窗口标题都不经过任何码页往返。
//   * 说明书落盘时按 UTF-8（严格编码，本模块产出宽字符文本，编码由平台层完成）；
//     UTF-8 是双方自己约定的格式，与系统 ANSI 码页、控制台码页都无关，
//     因此“中文用户名 / 中文临时目录 / 中文仓库路径 / 中文标题”都不再需要本机码页配合。
//   * 命令窗口的控制台输入/输出码页由辅助进程显式设为 UTF-8，与 Git 自身的 UTF-8 输出对齐；
//     系统 ANSI 代码页（GetACP）在本条链路上不再被使用，也没有合法参数可选的转换点。
//   * 本模块只做“值”的校验与序列化：绝不做最佳匹配、问号替换或丢字符。
//
// 完成判定绝不依赖匹配 Git 输出里的某句话：凭据只有本次操作独占目录内的文件。
// 命令窗口是否关闭只决定“能否拿到退出码”；辅助进程自身的退出码也绝不被当作 Git 的退出码。

// 操作 ID 约束：ASCII 字母/数字/-/_，可安全进消息、文件名与结果比对。
inline constexpr size_t kMaxOperationIdLength = 32;
inline constexpr size_t kMaxArguments = 64;
inline constexpr size_t kMaxDisplayCommandLength = 8000;
// 操作 ID 与说明书里的随机口令（nonce）的长度上限：只用于诊断展示与越界防护。
inline constexpr size_t kMaxNonceLength = 64;

// 操作目录名前缀：GcOp<进程ID>x<序号>，纯 ASCII，用于清扫残留目录与核对目录归属。
inline constexpr std::wstring_view kOperationDirectoryPrefix = L"GcOp";

// 操作目录内的文件（辅助进程写出，观察端只读）：
//   start.txt   —— 辅助进程启动后第一件事：命令窗口确实已在执行本操作（区别于“进程根本没起来”）
//   result.txt  —— 一行十进制整数：Git 的退出码；行尾换行表示写完。
//                  操作 ID 由独占目录绑定（目录名即 ID），不写进文件。
//   spec.txt    —— 操作说明书；由执行器写入，辅助进程读回后按它执行，用完由本程序删除。
// Git 程序的存在性由执行器在启动命令窗口之前检查（不是文件则根本不启动，报启动失败）；
// 辅助进程再查一次，专防“执行期间 git.exe 被移动或删除”，此时用保留码 9009 上报。
inline constexpr const char* kStartMarkerFileName = "start.txt";
inline constexpr const char* kResultFileName = "result.txt";
inline constexpr const char* kSpecFileName = "spec.txt";

// 一条环境覆盖：value 有值时写入（大小写不敏感替换），无值时从环境块删除该变量。
struct EnvironmentOverride {
  std::wstring name;
  std::optional<std::wstring> value;
};

// 一次外部命令窗口操作的请求：GUI 只填语义字段并提交，不拼 shell 字符串。
// 实际执行形态：新控制台的命令窗口辅助进程（本程序自己的隐藏入口）按说明书用 CreateProcessW
// 直接启动所选 git.exe，子进程工作目录即 repositoryDirectory（等价 cd），
// Git 原生交互留在该终端里；命令跑完后辅助进程在同一控制台里交给 cmd /k 保留窗口。
struct CommandWindowOperation {
  std::wstring operationId;  // 见 kMaxOperationIdLength 约束
  std::wstring displayName;  // 操作名称（进入窗口标题与输出回显），如 "status"
  std::wstring gitExecutable;               // 已验证的 git.exe 绝对路径
  std::wstring repositoryDirectory;         // 子进程工作目录（绝对路径）
  std::vector<std::wstring> arguments;      // Git 参数列表，一项一个元素
  std::vector<EnvironmentOverride> environmentOverrides;  // 继承环境之上的受控覆盖
};

// 请求在边界上被拒绝的原因；界面要能展示具体是哪一段、为什么。
enum class CommandPlanReject {
  none = 0,
  emptyOperationId,
  illegalOperationId,
  emptyExecutable,
  emptyWorkingDirectory,
  quoteInPath,             // 路径含双引号：命令行引用形态无法安全表达，直接拒绝
  controlCharacterInPath,  // 路径含控制字符（含制表、换行），会截断说明书行
  illegalArgument,         // 参数含双引号或控制字符（Git 参数按语义不应需要引号）
  tooManyArguments,
  commandTooLong,
  illegalNonce,              // 随机口令缺失或含非 ASCII 字母数字：无法与被启动的辅助进程绑定
  illegalOperationDirectory,  // 操作目录名不是执行器生成的形态：说明书与目录不是同一件事
  illegalWindowTitle,        // 窗口标题含控制字符或制表符，会破坏说明书的行形态
};

[[nodiscard]] std::wstring_view CommandPlanRejectLabel(CommandPlanReject reject) noexcept;

// 第一步：校验请求并生成将展示/将执行的 Git 命令行（引号区域安全性在此判定）。
[[nodiscard]] bool BuildGitCommandLine(const CommandWindowOperation& operation, std::wstring* gitLine,
                                       CommandPlanReject* reject, std::wstring* detail);

// 生成命令窗口标题：前缀 + 操作名 + 操作 ID，并剔除操作名里会改变 shell 行解析的字符。
// 结果保证是“可原样写进说明书、并可被 FindWindowW 精确匹配”的形态：
//   - 带唯一操作 ID，避免多个窗口同名（否则“关闭窗口”会找错目标）；
//   - 不含引号、控制字符与制表符，因此不会被说明书的行格式截断。
// 标题现在由辅助进程用 SetConsoleTitleW 直接设置，中文标题在任何代码页的机器上都能显示，
// 也不再因为“展示文字不好编码”而拒绝一次合法的操作。
[[nodiscard]] std::wstring MakeSafeConsoleTitle(std::wstring_view prefix, std::wstring_view displayName,
                                                std::wstring_view operationId);

// 第二步：把校验过的操作序列化成说明书文本（UTF-16；落盘编码由平台层严格转成 UTF-8）。
// 行格式固定为“字段名<TAB>值”，每行一个字段：值里不可能含制表符与控制字符
// （第一步的校验已经把这类输入拒绝在边界上），因此拆分无歧义，参数只按数据传递。
// directoryToken 是操作独占目录的目录名（GcOp<进程ID>x<序号>）：说明书与目录互相绑定，
// 辅助进程核对两者一致才肯执行。nonce 是本次操作的随机口令：执行器用 Unicode 命令行把它
// 交给辅助进程，辅助进程要求说明书里的 nonce 与之相符，防止有人用别的目录来驱动这个入口。
[[nodiscard]] bool BuildCommandWindowSpecText(const CommandWindowOperation& operation,
                                              std::wstring_view directoryToken, std::wstring_view title,
                                              std::wstring_view nonce, std::wstring* specText,
                                              CommandPlanReject* reject, std::wstring* detail);

// 说明书读回后的结构化形态（全部字段来自文本，未经任何码页转换）。
struct CommandWindowSpec {
  std::wstring directoryToken;    // 必须等于操作目录名
  std::wstring operationId;
  std::wstring nonce;
  std::wstring title;
  std::wstring gitExecutable;
  std::wstring workingDirectory;  // Git 的工作目录（绝对路径；辅助进程核实后按它执行）
  std::vector<std::wstring> arguments;
};

// 解析说明书：任一行不合格式、字段缺失或重复、版本不符都返回 false 并给出原因。
// 入参是已经由平台层严格解码好的 UTF-16 文本（本模块不做任何码页转换）。
// 只负责格式与字段收集，值的安全校验由调用方接着用 BuildGitCommandLine 复核
// （辅助进程就是这么做的：说明书的内容一律按不可信输入处理）。
[[nodiscard]] bool ParseCommandWindowSpecText(std::wstring_view specText, CommandWindowSpec* outSpec,
                                              std::wstring* failureReason);

// 操作目录名（不含父目录）是否是执行器会生成的形态：GcOp 开头、纯 ASCII 字母数字、
// 只允许一个分隔 x、长度受限。辅助进程用它核对“说明书与所在目录是同一件事”。
[[nodiscard]] bool IsSafeOperationDirectoryName(std::wstring_view directoryName);

// 随机口令是否是执行器会生成的形态：纯 ASCII 字母数字，长度 8..kMaxNonceLength。
[[nodiscard]] bool IsSafeNonce(std::wstring_view nonce);

// 解析 result.txt 全文（首行为十进制退出码；可为负数，CRT 语义下原值写回）。
// 未写完（无换行结尾）或内容不合约定一律返回 false，由观察端继续等待。
[[nodiscard]] bool ParseCommandWindowResult(std::string_view content, long* exitCode);

// “要调用的程序不存在”的保留码：辅助进程 CreateProcessW(git.exe) 失败时用它在 result.txt
// 上报同一形态。其余退出码一律视为 Git 自己的回答。
inline constexpr long kCommandNotFoundExitCode = 9009;

// 完成状态：六个终态 + 两个中间态，界面、事件与测试一律以它为准。
enum class CommandCompletion {
  launchFailed = 0,  // 命令窗口辅助进程未能启动（CreateProcessW 失败，附 Windows 错误码）
  launched,          // 进程已创建，辅助进程尚未写出开始标记（中间态）
  running,           // 辅助进程已接管命令窗口，等待 Git 退出码（中间态）
  finished,          // 拿到 Git 退出码（exitCode==0 视为成功）
  gitNotStarted,     // 辅助进程跑完了但 Git 进程从未被创建（如 git.exe 中途消失）
  terminated,        // 窗口被提前关闭：Git 已开跑但拿不到退出码，结果未知
  helperNeverStarted,  // 辅助进程已退出却连 start.txt 都没有：从未真正开始执行
  stillUnknown,      // 进程存活超过观察期限仍无结果：结果未知，不谎报也不永远“执行中”
};

[[nodiscard]] std::wstring_view CommandCompletionLabel(CommandCompletion completion) noexcept;

// 观察一次操作目录所需的全部事实（由平台层收集后交给纯逻辑判定）。
struct CommandWindowObservation {
  bool createProcessSucceeded = false;
  bool processExited = false;
  bool startMarkerSeen = false;       // start.txt 存在
  bool resultParsed = false;          // result.txt 已完整且解析成功
  long exitCode = 0;                  // resultParsed 时有效
};

// 观察器读文件用的接口：返回 nullopt 表示“不存在或暂时读不到”。
using CommandWindowFileReader = std::function<std::optional<std::string>(std::string_view fileName)>;

// 用注入的读文件回调收集事实（平台层绑定真实目录读取，测试绑定桩）。
[[nodiscard]] CommandWindowObservation ObserveCommandWindow(
    const CommandWindowFileReader& readFile, bool createProcessSucceeded, bool processExited);

// 把事实合并为终态或中间态；outExitCode 仅在 finished 时写入。
[[nodiscard]] CommandCompletion DecideCommandCompletion(const CommandWindowObservation& facts,
                                                        long* outExitCode) noexcept;

}  // namespace gc::git
