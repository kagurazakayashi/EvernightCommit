![EvernightCommit 图标](resources/EvernightCommit.ico)

# EvernightCommit（Git 提交工具，Windows 原生 C++）

项目名 **EvernightCommit**；窗口标题沿用需求文档规定的“Git 提交工具”。
面向 Windows 的 Git 提交图形界面工具，原生 Win32 + Common Controls，不依赖任何跨平台 GUI 框架。

**当前实现范围：Git 可执行文件的发现、选择与验证（开发计划步骤 2）。**
程序启动即按 PATH 自动定位 `git.exe` 并在后台验证；“Git 程序”行可下拉选择候选、手动输入或浏览选择。
但**尚未接入任何 Git 操作功能**：所有依赖 Git 的按钮仍处于“功能未实现”的禁用状态，
列表显示空状态说明，不会伪造数据或成功提示。

## 1. 环境与工具链

按官方资料确认的最低要求，以及本机实测版本：

| 组件 | 最低要求 | 本机实测 |
| --- | --- | --- |
| 编译器 | MSVC 工具集 14.28（Visual Studio 2019 16.11）起提供 `/std:c++20` | 14.51.36231（cl 19.51） |
| Windows SDK | Windows 10 SDK（`GetDpiForWindow`、`AdjustWindowRectExForDpi` 需 10.0.14393+；本工程按 `_WIN32_WINNT=0x0A00` 编译） | 10.0.26100.0 |
| CMake | 3.28（`CMakePresets` schema 8；C++20 支持自 3.12 的 `cxx_std_20` 起即有） | 4.4.2 / VS 自带 4.3.1 |
| 生成器 | Visual Studio 生成器，或 Ninja（需自带 `cl`/`rc` 的开发者环境） | VS 2026 + Ninja 1.13 |
| 目标平台 | Windows x64（仅支持 x64，非 Windows 会在配置阶段直接报错） | — |

> 注意：MSYS2 自带的 `cmake` 不提供 Visual Studio 生成器，且其 `link.exe` 会遮蔽 MSVC 链接器。
> 请使用 Visual Studio 安装目录下的 CMake，或独立的 CMake 发行版。

### 运行库部署要求

默认动态链接 VC 运行库（Debug `/MDd`、Release `/MD`）。因此：

- 开发机上（已装 Visual Studio）直接运行 `EvernightCommit.exe` 即可；
- 在未安装运行库的机器上分发时，需要随程序部署 **Microsoft Visual C++ Redistributable (x64)**，
  本机对应安装包为 `VC\Redist\MSVC\<版本>\vc_redist.x64.exe`；
- 若希望免部署单文件，可改用静态运行库：配置时加上
  `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`。

## 2. 构建与运行

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
- `-DGC_WARNINGS_AS_ERRORS=ON`：把 `/W4` 警告视为错误（CI 用）。

产物位置：`build/<预设名>/bin/<配置>/EvernightCommit.exe`。构建目录与 IDE 本地文件均已在
`.gitignore` 中排除。

## 3. 界面说明

窗口标题“Git 提交工具”，初始尺寸 1100×780（按 DPI 缩放），可自由缩放，最小尺寸由内容决定
（当前约 752×505 @100% 缩放）。

- **顶部**：`本地仓库` 路径输入 + `浏览…` 按钮（已可用，仅记录路径）；
  `Git 程序` 为可编辑下拉框：启动时自动列出按 PATH 发现的 `git.exe` 候选（完整路径、去重），
  也可直接键入路径或用 `浏览…` 选择文件，改动后在后台自动运行 `git --version` 验证，
  结果（版本号或具体失败原因）显示在“任务状态”里（详见第 3.1 节）；
  右侧为仓库级操作 `fetch` / `pull` / `status`（禁用，功能未接入）。
- **第二行**：当前分支、上游、任务状态占位说明，以及程序版本信息。
- **中部三栏**：`未暂存的更改`、`已暂存的更改`、`最近提交`，两个可拖动分隔条调整三栏宽度；
  中间独立一列放 `加入暂存区 →` / `← 移出暂存区`（禁用，不与仓库级操作混放）。
  列表预留“状态 / 相对路径”等列，支持多选。
- **提交表单**：标题、多行描述、作者 `姓名 <邮箱>`、合作者列表（`添加`/`删除` 禁用）、
  作者时间与提交者时间（各由“日期 + 时分秒”两个控件组成，可键盘直接输入，随附本机时区显示）、
  `时间同步修改` 选项。
- **底部**：`刷新`、`创建提交`、`撤回最近提交`、`推送`（全部禁用），以及固定说明文字。
- 每个未实现按钮都带工具提示，写明“尚未实现”及对应的原生 Git 命令。

### 3.1 Git 前置条件与路径选择

运行本程序需要一个 Windows 版 Git（推荐 [Git for Windows](https://git-scm.com/download/win)）；
程序本身不携带也不安装 Git。`Git 程序` 的确定流程：

1. **自动发现**：启动时按当前进程的 Windows PATH 逐目录查找 `git.exe`（效果对应 `where git`
   的搜索意图），不扫描磁盘、不读注册表；多个候选按 PATH 顺序去重（大小写不敏感）后进入下拉列表。
   发现结果自动选中第一个候选并立即验证。
2. **手动选择**：直接在组合框键入路径（裸名 `git` 也会按 PATH 解析），或用 `浏览…` 选择 `git.exe`。
   有效路径统一规范化为绝对路径；后续所有 Git 调用都使用该确定的程序路径，不再依赖 shell 再次搜索。
3. **后台验证**：对工作线程直接以参数数组执行 `<git.exe> --version`（`CreateProcessW` +
   `CREATE_NO_WINDOW`，不弹命令窗口、不经 cmd 拼接），限时 3 秒，捕获退出码、输出与启动错误；
   确认路径是文件且程序确实报告 Git 版本后保存版本号供后续步骤使用。
4. **失败与恢复**：找不到 Git、文件不存在、无法启动、超时或输出不是 Git 版本时，“任务状态”显示
   具体原因；依赖 Git 的功能保持禁用。改正路径后自动重新验证并恢复，无需重启程序。
   连续输入有 0.5 秒防抖，较慢完成的旧验证结果会被序号丢弃，不会覆盖新选择。

## 4. 代码结构

```
src/
  main.cpp                 入口：wWinMain、消息循环（IsDialogMessageW 提供 Tab 焦点）
  app/                     应用状态（AppState）：界面读写状态（含 Git 验证结果），不直接访问 Git
  git/                     Git 纯逻辑：PATH 候选发现（git_locator）、--version 结果判定（git_probe）、
                           工作区数据模型与空状态文案（本步骤不执行仓库命令）
  ui/                      Win32 界面：主窗口、面板、控件（含候选组合框）、DPI 度量、
                           纯几何布局、分隔条
  platform/windows/        UTF-8↔UTF-16 边界转换、RAII 句柄、COM/Common Controls、路径选择对话框、
                           时区文本、文件/PATH 环境辅助（win_path）、隐藏窗口子进程执行器
                           （subprocess）、Git 发现与验证服务（git_toolchain）、后台验证控制器
                           （git_verify_worker，PostMessage 回 UI 线程）
tests/
  support/                 轻量断言框架与测试入口（无外部依赖，不联网）
  *_tests.cpp              布局几何、DPI 度量、编码转换、应用状态、候选发现、版本判定、
                           命令行转义等纯逻辑测试
resources/app.manifest     Common Controls v6、Per-Monitor V2 DPI 感知、asInvoker
```

约定：源码 UTF-8（`/utf-8`），Win32 一律使用 Unicode/W API，控件与路径使用 UTF-16，
跨边界只经 `platform::Utf8ToUtf16` / `Utf16ToUtf8`；`HWND`、`HFONT` 由 RAII 持有；
窗口过程内部捕获所有异常，异常不越过系统回调。

## 5. 尚未实现（后续步骤）

仓库识别与分支摘要、工作区状态解析、测试仓库夹具、外部命令窗口执行器、
暂存/取消暂存、创建提交、提交历史、撤回提交、fetch/pull/push、
配置持久化、自动更新与安装器。

（Git 可执行文件的发现、选择与验证已在步骤 2 完成。）
