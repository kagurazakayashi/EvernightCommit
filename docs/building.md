# EvernightCommit 构建与环境参考

本文是稳定、可复用的构建/开发参考资料；面向普通用户的使用说明见仓库根目录 `README.md`，
测试入口与分组授权见 `testing.md`，代码结构见 `architecture.md`，逐操作行为规格见 `git-behavior.md`。
某轮构建与测试的实际结果不写入本文（那是本地开发记录的内容，不随仓库分发）。

## 1. 环境与工具链

按官方资料确认的最低要求，以及本机实测版本：

| 组件 | 最低要求 | 本机实测 |
| --- | --- | --- |
| 编译器 | MSVC 工具集 14.28（Visual Studio 2019 16.11）起提供 `/std:c++20` | 14.51.36231（cl 19.51） |
| Windows SDK | Windows 10 SDK（`GetDpiForWindow`、`AdjustWindowRectExForDpi` 需 10.0.14393+；本工程按 `_WIN32_WINNT=0x0A00` 编译） | 10.0.26100.0 |
| CMake | 3.28（`CMakePresets` schema 8；C++20 支持自 3.12 的 `cxx_std_20` 起即有）。`vs2026-x64` 预设另需 **4.2 及以上**：“Visual Studio 18 2026” 生成器自 CMake 4.2 提供 | VS 2026 自带 4.2.3-msvc3；MSYS2 4.3.1（无 Visual Studio 生成器） |
| 生成器 | Visual Studio 生成器，或 Ninja（需自带 `cl`/`rc` 的开发者环境） | VS 2026 + Ninja 1.13 |
| 目标平台 | Windows x64（仅支持 x64，非 Windows 会在配置阶段直接报错） | — |

> 注意：MSYS2 自带的 `cmake` 不提供 Visual Studio 生成器，且其 `link.exe` 会遮蔽 MSVC 链接器。
> 请使用 Visual Studio 安装目录下的 CMake，或独立的 CMake 发行版。

x64 与 SDK 的判定不是只看指针宽度：CMake 在 MSVC 下核对编译器自报的目标架构
（`CMAKE_CXX_COMPILER_ARCHITECTURE_ID`），架构未知时显式失败而不静默放行；
VS 生成器会校验所选 Windows SDK 不低于 10.0.14393。非 MSVC 编译器会给出未经
验证的警告——本工程按 MSVC + Windows SDK 设计。

## 2. 构建命令与产物

在项目根目录执行（PowerShell 或命令提示符）：

```powershell
# 使用 Visual Studio 生成器（无需开发者命令提示符）
cmake --preset vs2026-x64
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug
.\build\vs2026-x64\bin\Debug\EvernightCommit.exe
```

只有 Visual Studio 2022 时改用 `vs2022-x64` / `vs2022-debug` / `vs2022-release` 预设即可。

使用 Ninja（必须先进入 “Developer Command Prompt for VS”，使 `cl.exe`、`rc.exe` 在 PATH 中）：

```bat
cmake --preset ninja-x64
cmake --build --preset ninja-debug
ctest --preset ninja-debug
```

其他常用开关：

- `-DBUILD_TESTING=OFF`：不生成测试目标；
- `-DGC_WARNINGS_AS_ERRORS=ON`：把 `/W4` 警告视为错误（用于要求零警告的严格构建；
  本工程自身没有远程 CI，此选项不隐含任何 CI 集成）；
- `-DCMAKE_MSVC_RUNTIME_LIBRARY=<值>`：显式选择 VC 运行库（如免部署分发用静态运行库；
  引号写法与部署含义见下节）。

产物位置：`build/<预设名>/bin/<配置>/EvernightCommit.exe`（测试程序 `gc_tests.exe` 同目录）。
构建目录与 IDE 本地文件均已在 `.gitignore` 中排除。

测试的分组入口、授权边界（含真实 `git commit`/`git push` 的一组只由项目维护者运行）、
夹具隔离规则与环境矩阵见 `testing.md`。

## 3. 运行库部署要求

未显式指定时，CMake 提供默认值：动态链接 VC 运行库（Debug `/MDd`、Release `/MD`）。
该默认值**只在用户没有指定时生效**——配置时传入 `-DCMAKE_MSVC_RUNTIME_LIBRARY=<值>`
即整体采用该值，并统一作用于核心库、平台库、UI、主程序与测试目标（本工程不做混合
CRT 链接，混用会在链接期直接失败）。

两种选择的实际依赖（本机 Release 产物 `dumpbin /dependents` 实测，基线见文末说明）：

| 运行库 | 配置值（生成器表达式，Debug 与非 Debug 各自映射） | Release 产物的 VC 运行库导入 | 分发方式 |
| --- | --- | --- | --- |
| 动态（默认） | `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` | `VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`、`MSVCP140.dll`、`api-ms-win-crt-*.dll` | 在未装 Visual Studio 的机器上，随程序部署 **Microsoft Visual C++ Redistributable (x64)**（本机对应安装包：`VC\Redist\MSVC\<版本>\vc_redist.x64.exe`） |
| 静态 | `MultiThreaded$<$<CONFIG:Debug>:Debug>` | 不导入上述 VC/UCRT DLL，仅导入 `KERNEL32`、`USER32`、`GDI32`、`COMCTL32`、`SHELL32`、`ole32` 等系统组件 | 就 VC 运行库而言免部署，exe 可单文件拷走 |

命令行示例。生成器表达式含 `<` 与 `>`，它们在 cmd 和 PowerShell 里都是重定向符，
**必须整体加引号**，否则命令会被截断或直接报语法错误：

```bat
:: cmd：引号使 < > 不触发重定向（Ninja 需先进入 Developer Command Prompt / vcvars64.bat）
cmake -S . -B build\static -G "Ninja Multi-Config" -D "CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>"
```

```powershell
# PowerShell：用单引号最稳妥（$ 与 <> 都按字面传入；--preset 后可追加 -D 覆盖缓存项）
cmake --preset vs2026-x64 '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
```

两点必须说清：

- **“静态运行库”不等于“无系统依赖”**：程序仍需要 x64 版 Windows 10 及以上
  （Common Controls v6、每显示器 DPI 与长路径行为均由系统与随程序的 manifest 声明
  决定），需要外部可用的 **Git 命令行工具**（本程序调用 `git.exe`，不内置）；在更老的
  Windows 上即便静态 CRT 也要先确认系统已具备 UCRT API Set 补丁。
- **Debug 配置只供开发机自用**：Debug 运行库（`/MDd`，或静态选项下的 `/MTd`）依赖
  `ucrtbased.dll`、`VCRUNTIME140D.dll` 等调试专用 DLL，不能随可再发行包分发给普通用户。

分发验证状态：本机（装有 Visual Studio 的开发机）只完成了依赖导入表核验；**在未安装
运行库的干净 Windows 机器上的实际部署运行未验证**，上表“分发方式”是依据依赖清单的
推断，不等同于已在干净机器上跑通。

## 4. 系统区域、代码页与用户名

本程序的数据链路不使用系统 ANSI 代码页（原因与做法见 `git-behavior.md` 命令窗口一节），
因此不要求用户为了运行它去改用户名、把仓库搬到 ASCII 路径，或开启/关闭 Windows 的
“Beta: 使用 Unicode UTF-8 提供全球语言支持”。

验收矩阵与**实际验证状态**（不要把未实测的格子当作已全绿）：

| 环境形态 | 本机是否已实测 | 说明 |
| --- | --- | --- |
| 系统 ANSI 代码页 = UTF-8（开启 Beta UTF-8 的 Windows 11） | 已实测通过 | 开发机即此形态；中文临时目录、中文仓库路径、含 `%`/`&`/中文/emoji 的文件名、中文窗口标题均可用 |
| 简体中文传统代码页（ACP/OEM 936） | **未实测，待人工验证** | 判定链路已经不看系统代码页，但需要在真的 936 机器上确认显示与交互；Git 输出为 UTF-8，控制台码页由命令窗口显式设为 UTF-8 |
| 西文代码页（1252/437） | **未实测，待人工验证** | 同上；这一形态下旧实现会因“ANSI 装不下中文”直接拒绝操作，现在不再有该判定 |
| 中文用户名导致 `%TEMP%` 含非 ASCII | 已实测通过（自动化用例把本进程临时目录指向中文目录） | 操作目录与说明书、标记、结果文件全部走 Unicode API |
| 仓库路径含中文、空格、`&` | 已实测通过 | 见 `git-behavior.md` 命令窗口一节与 `testing.md` 的用例分组 |
| 窗口标题为中文 | 已实测通过 | 标题由命令窗口辅助进程用 `SetConsoleTitleW` 设置，不再按码页编码 |

在上述未实测的机器上如发现命令窗口打不开、窗口里中文显示成乱码或“关闭窗口”按钮
找不到窗口，请在反馈时附上状态栏原文与操作目录里 `spec.txt` 的内容（路径见状态栏
提示），以便定位是哪一个环节。

## 5. 实测基线说明

本工程多轮开发中的“实测”结论来自同一台 Windows 11 x64 开发机、UTF-8 代码页形态，
不同时期 PATH 上的 Git 分别为 **2.56.0.windows.1**（早期暂存/提交/撤回相关核验）与
**2.53.0.windows.3**（近期 fetch/pull/push、环境策略与全部分组回归核验）。
文档中标注“实测”的行为以这两个版本为准；更旧的 Git 版本未在实机上验证，
程序对版本差异的处理（如 pathspec 清单退回、`restore` 是否存在）按
`git-behavior.md` 各节声明的退回路径执行。
