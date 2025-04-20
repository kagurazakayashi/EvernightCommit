#pragma once

#include <string_view>

namespace gc::platform {

// 辅助入口的开关字：执行器（command_window_runner）拼命令行时用，辅助进程判定模式时用，
// 两边共用同一个常量，避免其中一边改了另一边不知道。
inline constexpr std::wstring_view kCommandWindowHelperSwitch = L"--gc-console-helper";

// 命令窗口辅助入口（同一个可执行文件里的隐藏模式）。
//
// 为什么需要它：本程序要把中文用户名/中文临时目录/中文仓库路径/中文窗口标题，
// 以及任何“系统 ANSI 码页装不下”的字符原样送进命令窗口里的 Git。只要数据还要经过
// cmd.exe 的批处理字节，就必须先按某个码页编码，而码页装不下时只有两种坏结果：
// 拒绝一次合法操作，或者把用户的文件名改成别的样子。因此这条链路上不再有批处理：
// 执行器用 CreateProcessW 直接启动本程序的辅助模式，辅助进程自己分配控制台，
// 用 Unicode API 设置标题、写标记文件，并用 CreateProcessW 把 Git 的命令行按数据形态送出。
//
// 边界（这个入口不是通用命令执行器）：
//   * 只接受 “--gc-console-helper <操作目录> <随机口令>” 这一种形态，参数个数固定；
//   * 要执行什么只来自该独占目录里的 spec.txt，命令行上没有任何可执行文件或参数；
//   * 目录必须是当前用户临时目录根下、名字符合 GcOp<进程ID>x<序号> 的独占目录；
//   * 说明书里的口令必须与命令行上传来的完全一致，说明书声明的目录名必须与所在目录一致；
//   * 读回的值一律按不可信输入处理，再用与 GUI 同一套校验（git::BuildGitCommandLine）复核，
//     不合格就不执行，也绝不“修好”它（不做最佳匹配、不替换字符、不丢参数）。
//   残留风险如实说明：能以当前用户身份在本临时目录里造文件、造进程的攻击者，
//   本来就能以同一身份直接执行命令；这里的绑定防的是“用别的目录来驱动本入口”与
//   “GUI 已经不管了却还留着一条能被外部触发的执行通道”，不是同身份攻击者的隔离边界。
//
// 返回值：requested 为 true 时，本进程已经按辅助模式跑完，exitCode 是进程退出码，
// 调用方（GUI 入口或测试入口）必须直接结束，不再进入正常启动流程。
bool RunCommandWindowHelperIfRequested(int* exitCode);

}  // namespace gc::platform
