#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// 外部命令窗口执行器的可移植逻辑（本模块不触碰任何 Win32 API）：
//   1) 把一次“用户主动执行的 Git 操作”构造为将进入 cmd.exe 一次性批处理的命令行与脚本，
//      并在进入 cmd 的边界完成注入校验（引号、控制字符、cmd 元字符、码页可表示性）；
//   2) 读取并解释脚本写出的标记文件与结果文件；
//   3) 把“进程启动 / 进程退出 / 标记文件 / 结果文件”四路事实合并成唯一、明确的完成状态。
// 宽字符串进出；字节编码（系统 ANSI 码页）由平台层完成后传入。
//
// 完成判定绝不依赖匹配 Git 输出里的某句话：凭据只有本次操作独占目录内的文件。
// cmd 窗口是否关闭只决定“能否拿到退出码”；cmd 自身的退出码也绝不被当作 Git 的退出码。

// 操作 ID 约束：ASCII 字母/数字/-/_，可安全进消息、文件名与结果比对。
inline constexpr size_t kMaxOperationIdLength = 32;
inline constexpr size_t kMaxArguments = 64;
inline constexpr size_t kMaxDisplayCommandLength = 8000;

// 操作目录内的文件（脚本写出的观察端只读）：
//   start.txt   —— 脚本第一行创建：cmd 确在执行本脚本（区别于“cmd 启动即失败”）
//   result.txt  —— 一行十进制整数：Git 的退出码；行尾换行表示写完。
//                  操作 ID 由独占目录绑定（目录名即 ID），不写进文件。
//   run.cmd     —— 一次性脚本；由执行器在判定终态后删除。cmd 运行期间持有该文件，
//                  脚本自己删不掉，所以不用“脚本是否消失”来判断执行进度。
// Git 程序的存在性由执行器在启动 cmd 之前检查（不是文件则根本不启动，报启动失败）；
// 脚本内不做存在性判断（cmd 的 if exist 无法安全表达含空格的变量路径），
// 因此“call 的程序不存在”只剩 Windows 保留退出码 9009 这一条可观测痕迹。
inline constexpr const char* kStartMarkerFileName = "start.txt";
inline constexpr const char* kResultFileName = "result.txt";
inline constexpr const char* kScriptFileName = "run.cmd";

// 一条环境覆盖：value 有值时写入（大小写不敏感替换），无值时从环境块删除该变量。
struct EnvironmentOverride {
  std::wstring name;
  std::optional<std::wstring> value;
};

// 一次外部命令窗口操作的请求：GUI 只填语义字段并提交，不拼 shell 字符串。
// 实际执行形态：新控制台的 cmd.exe 运行一次性批处理，批处理内 call 所选 git.exe，
// 子进程工作目录即 repositoryDirectory（等价 cd），Git 原生交互留在该终端里。
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
  quoteInPath,             // 路径含双引号：cmd 引号规则无法安全表达，直接拒绝
  controlCharacterInPath,  // 路径含控制字符（含制表、换行），会截断脚本行
  illegalArgument,         // 参数含双引号或控制字符（Git 参数按语义不应需要引号）
  tooManyArguments,
  commandTooLong,
  illegalScriptDirectory,  // 临时脚本目录不是纯 ASCII：重定向文件名会被码页破坏
  nonEncodableCommand,     // 命令行含系统 ANSI 码页无法表示的字符（平台层判定）
};

[[nodiscard]] std::wstring_view CommandPlanRejectLabel(CommandPlanReject reject) noexcept;

// 构造出的计划：脚本为 ASCII 骨架 + 平台层已编码的字段。
// 界面展示用的命令行直接用 BuildGitCommandLine 返回的宽字符结果；
// 计划不回传字节形态，否则把码页字节按字符逐个提升会显示成乱码。
// 操作 ID 由独占目录名绑定（目录名就是 ID），不需要额外文件承载。
struct CommandWindowPlan {
  std::string scriptAnsi;  // cmd 批处理内容（系统 ANSI 码页字节）
};

// 第一步：校验请求并生成将进入脚本的 Git 命令行（引号区域安全性在此判定）。
[[nodiscard]] bool BuildGitCommandLine(const CommandWindowOperation& operation, std::wstring* gitLine,
                                       CommandPlanReject* reject, std::wstring* detail);

// 生成命令窗口标题：前缀 + 操作名 + 操作 ID，并剔除操作名里会改变 cmd 行解析的字符。
// 结果保证是“可写进 title 行且可被 FindWindowW 精确匹配”的形态：
//   - 带唯一操作 ID，避免多个窗口同名（否则“关闭窗口”会找错目标）；
//   - 不含引号与控制字符，因此平台层无需再退回占位标题。
[[nodiscard]] std::wstring MakeSafeConsoleTitle(std::wstring_view prefix, std::wstring_view displayName,
                                                std::wstring_view operationId);

// 第二步：把平台层按系统码页编码好的各段组装成脚本。
// programAnsi 是引号包裹的 git.exe 路径段；argumentsAnsi 是空格分隔的已校验参数引用段
// （两者都由 EncodeToSystemAnsi 产出，合起来等于 BuildGitCommandLine 的字节形态）；
// scriptDirectoryAnsi 必须纯 ASCII。
// titleWide 与 titleAnsi 是同一个标题的两种形态：安全性判定必须看宽字符形态，
// 因为多字节码页（如 GBK）的第二字节可能落在 ASCII 区间，直接检查字节会误判；
// 而写进脚本的必须是码页字节。标题不合格时退回含操作 ID 的 ASCII 占位标题，
// 绝不因为“展示文字不好”而拒绝一次合法的操作。
[[nodiscard]] bool AssembleCommandWindowScript(std::wstring_view operationId,
                                               std::string_view scriptDirectoryAnsi,
                                               std::wstring_view titleWide, std::string_view titleAnsi,
                                               std::string_view programAnsi,
                                               std::string_view argumentsAnsi, CommandWindowPlan* plan);

// 解析 result.txt 全文（首行为十进制退出码；可为负数，CRT 语义下原值写回）。
// 未写完（无换行结尾）或内容不合约定一律返回 false，由观察端继续等待。
[[nodiscard]] bool ParseCommandWindowResult(std::string_view content, long* exitCode);

// cmd 对“call 的程序不存在”返回的系统保留退出码。Git 自身恰好返回同值的概率极低，
// 判定为“Git 未能启动”后界面会同时给出退出码，由用户复核。
inline constexpr long kCommandNotFoundExitCode = 9009;

// 完成状态：六个终态 + 两个中间态，界面、事件与测试一律以它为准。
enum class CommandCompletion {
  launchFailed = 0,  // cmd.exe 未能启动（CreateProcessW 失败，附 Windows 错误码）
  launched,          // 进程已创建，脚本尚未开始执行（中间态）
  running,           // 脚本已开始执行，等待 Git 退出码（中间态）
  finished,          // 拿到 Git 退出码（exitCode==0 视为成功）
  gitNotStarted,     // 脚本执行完毕但 Git 进程从未被创建（如 git.exe 中途消失）
  terminated,        // 窗口被提前关闭：脚本已跑过但拿不到退出码，结果未知
  scriptNeverRan,    // cmd 已启动又退出，脚本连 start.txt 都没写出且脚本仍在：脚本无法执行
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
