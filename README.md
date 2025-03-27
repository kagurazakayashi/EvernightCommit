![EvernightCommit 图标](resources/EvernightCommit.ico)

# EvernightCommit（Git 提交工具，Windows 原生 C++）

项目名 **EvernightCommit**；窗口标题沿用需求文档规定的“Git 提交工具”。
面向 Windows 的 Git 提交图形界面工具，原生 Win32 + Common Controls，不依赖任何跨平台 GUI 框架。

**当前实现范围：Git 可执行文件的发现、选择与验证（步骤 2），以及仓库路径选择、仓库识别与当前分支摘要（步骤 3）。**
程序启动即按 PATH 自动定位 `git.exe` 并在后台验证；“Git 程序”行可下拉选择候选、手动输入或浏览选择。
“本地仓库”行初值为程序启动时的工作目录（绝对路径），可键入或浏览选择，识别在后台只读执行，
并区分普通工作区、链接工作树、子模块、裸仓库、`.git` 内部、尚无提交、游离 HEAD 与非仓库目录。
但**尚未接入任何 Git 变更操作**：所有依赖 Git 的按钮仍处于“功能未实现”的禁用状态，
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

### 2.1 自动化测试夹具与隔离规则

测试入口不变：`ctest --preset <预设名>`（或 `build/ninja-x64/bin/Debug/gc_tests.exe` 直接看逐用例输出）。
`-DBUILD_TESTING=OFF` 时测试目标与夹具完全不参与构建；测试运行期间不下载任何依赖。

纯逻辑测试用桩输出即可；涉及真实 Git 的用例由 `tests/support/git_fixture.*` 提供临时仓库夹具，
每条用例独立拥有：

- **临时根目录**：在系统临时目录下以 `CreateDirectoryW` 原子冲突重试创建唯一目录，
  只有亲手创建成功的实例才认领所有权；析构（含用例异常栈展开）递归清理，先清只读位、
  短重试应对杀软/索引器占用，析构不抛异常，删不净时打印残留路径。
  设置环境变量 `GC_TEST_KEEP_TEMP=1` 可保留现场（默认一律清理），保留时打印目录位置。
- **配置隔离**：子进程环境块剔除 `GIT_*`、`HOME`、`USERPROFILE`、语言变量后注入
  `GIT_CONFIG_GLOBAL`（指向临时空文件）、`GIT_CONFIG_NOSYSTEM=1`、空 `GIT_TEMPLATE_DIR`，
  用户的 `.gitconfig`、系统配置、模板与 hooks、凭据助手对测试仓库不可见；测试绝不写用户配置。
- **身份与时间确定**：固定测试身份 `Evernight Test <test@example.invalid>`，
  提交时间取固定基准 + 序号（与本机时钟无关），默认分支显式写死 `main`。
- **禁止网络**：`GIT_ALLOW_PROTOCOL=file` 只放行本地 file 传输，网络 URL 在发起连接前即被 Git 拒绝。
- **路径守卫**：所有 Git 调用显式绑定工作目录，且必须位于本次临时根内（折叠 `..` 后判定），
  越界直接拒绝；夹具永远不会在开发仓库或用户真实仓库执行 `init`/`add`/`commit`。
- **前置条件失败与断言失败分开**：Git 不可用、临时目录创建失败、夹具命令意外非 0 退出等
  以“前置失败”中止用例并输出命令行、退出码与 stderr 诊断，但同样计入失败、返回非 0，
  绝不静默跳过集成测试。

允许例外（对应 AGENTS.md 权限边界）：夹具仅在自己创建并认领所有权的临时目录中执行
`git init`、`git add`、`git commit` 等真实 Git 操作；产生的提交是真实本地对象，
测试远端只允许是同一临时根内的本地仓库。集成测试覆盖：空仓库、初始提交、第二次提交父子链、
未暂存/已暂存/未跟踪状态、中文路径内容回读、配置隔离、协议封禁，以及用真实仓库驱动的
仓库识别边界（普通工作区、子目录上溯、尚无提交、非仓库、裸仓库、`.git` 内部、游离 HEAD、
链接工作树、子模块）。

## 3. 界面说明

窗口标题“Git 提交工具”，初始尺寸 1100×780（按 DPI 缩放），可自由缩放，最小尺寸由内容决定
（当前约 752×505 @100% 缩放）。

- **顶部**：`本地仓库` 路径输入 + `浏览…` 按钮（已可用：键入或浏览选择目录，停顿 0.5 秒后在后台识别；
  相对路径按启动工作目录展开为绝对路径，选中仓库的子目录时识别结果会上溯到真正的工作区根并在“任务状态”里说明）；
  `Git 程序` 为可编辑下拉框：启动时自动列出按 PATH 发现的 `git.exe` 候选（完整路径、去重），
  也可直接键入路径或用 `浏览…` 选择文件，改动后在后台自动运行 `git --version` 验证，
  结果（版本号或具体失败原因）显示在“任务状态”里（详见第 3.1 节）；
  右侧为仓库级操作 `fetch` / `pull` / `status`（禁用，功能未接入）。
- **第二行**：仓库类型、当前分支、上游、任务状态说明，以及程序版本信息。
  仓库类型取 Git 自己的回答（普通工作区/链接工作树/子模块仓库/裸仓库/位于 .git 目录内部/不是 Git 仓库/未能识别）；
  分支列在无提交时显示“尚无提交（分支 X）”，游离 HEAD 时显示“游离 HEAD（短提交 ID）”；
  上游列在没有跟踪分支时显示“未设置上游”，这不算识别失败（详见第 3.2 节）。
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

### 3.2 仓库路径选择与识别

“本地仓库”的确定流程（对应开发计划步骤 3）：

1. **初值与输入**：初值是程序启动时的工作目录，取绝对路径后填入输入框；不修改进程的工作目录。
   支持直接键入（0.5 秒防抖）与 `浏览…` 选择目录；带空格、中文的路径按 UTF-16 原样处理。
2. **只问 Git，不看 `.git`**：识别一律以 Git 的回答为准，不用“目录里有没有 `.git`”推断，
   因为链接工作树与子模块的 `.git` 是文件，裸仓库根本没有 `.git` 子目录。
   查询全部是只读的 `rev-parse` / `symbolic-ref` / `for-each-ref`，以参数数组在隐藏窗口子进程中执行，
   每条查询都显式 `-C <仓库目录>` 并同时把子进程工作目录绑定为该目录，
   因此切换仓库不会影响正在运行的其他查询，也不依赖 shell 二次搜索。
3. **形态区分**：普通工作区、链接工作树（`--git-common-dir` 与 `--absolute-git-dir` 不同）、
   子模块仓库（`--show-superproject-working-tree` 有值）、裸仓库、`.git` 目录内部、非仓库目录。
   分支维度另外区分“尚无提交”（HEAD 不可解析但有分支名）与“游离 HEAD”（给出短提交 ID）。
   裸仓库与 `.git` 内部**没有可用工作区**：识别本身算成功，但不会进入面向工作区的提交流程，
   “任务状态”会说明原因；链接工作树与子模块则照常可用，只是额外说明来历。
   “来历”（链接工作树/子模块）与“分支状态”（游离/尚无提交）可能同时成立，标签会把两者一起讲清楚。
   上游为空时显示“未设置上游”，不会让仓库整体加载失败。
4. **错误恢复与连续切换**：识别在工作线程执行，GUI 线程不冻结；每次提交带递增序号，
   较慢完成的旧结果作废。路径不存在、不是目录、被 Git 的 `safe.directory` 安全检查拦截、
   权限不足、超时或输出不合预期，都会在“任务状态”里给出具体原因，改正后自动恢复。
5. **边界**：绝不在用户选中的目录执行 `git init`，不修改仓库或全局配置，
   不自动修复 `safe.directory` 与目录所有权，不执行 `fetch`，也不把真实仓库路径写进测试。
   识别用到的 Git 查询都是本地只读操作，不访问远端。

依赖 Git 的按钮要同时满足三个条件才会启用：功能已接通（后续步骤逐项打开）、
Git 程序验证可用、仓库已识别为可用工作区。步骤 3 结束后三者中仍有第一项为假，因此按钮保持禁用。

## 4. 代码结构

```
src/
  main.cpp                 入口：wWinMain、消息循环（IsDialogMessageW 提供 Tab 焦点）
  app/                     应用状态（AppState）：界面读写状态（含 Git 验证结果与仓库识别结果），不直接访问 Git
  git/                     Git 纯逻辑：PATH 候选发现（git_locator）、--version 结果判定（git_probe）、
                           仓库形态解析与分类（repository：只接 UTF-16 文本，判定与错误归类可桩测）、
                           工作区数据模型与空状态文案
  ui/                      Win32 界面：主窗口、面板、控件（含候选组合框）、DPI 度量、
                           纯几何布局、分隔条
  platform/windows/        UTF-8↔UTF-16 边界转换、RAII 句柄、COM/Common Controls、路径选择对话框、
                           时区文本、文件/目录/PATH 环境辅助（win_path）、隐藏窗口子进程执行器
                           （subprocess，stdout 与 stderr 分开捕获）、Git 发现与验证服务（git_toolchain）、
                           仓库识别编排（repo_detect，执行依赖全部回调注入）、
                           通用后台任务控制器（git_task_worker，序号丢弃 + PostMessage 回 UI 线程）
tests/
  support/                 轻量断言框架与测试入口（无外部依赖，不联网）；
                           git_fixture（RAII 临时目录 + 隔离环境块 + 真实提交辅助，仅测试链接，
                           生产流程不调用）
  *_tests.cpp              布局几何、DPI 度量、编码转换、应用状态、候选发现、版本判定、
                           命令行转义、仓库形态识别（桩化 Git 输出）等纯逻辑测试；
                           git_fixture_tests / repo_detect_fixture_tests 用真实临时仓库
                           驱动夹具与生产识别逻辑（见 2.1 的隔离规则）
resources/app.manifest     Common Controls v6、Per-Monitor V2 DPI 感知、asInvoker
```

约定：源码 UTF-8（`/utf-8`），Win32 一律使用 Unicode/W API，控件与路径使用 UTF-16，
跨边界只经 `platform::Utf8ToUtf16` / `Utf16ToUtf8`；`HWND`、`HFONT` 由 RAII 持有；
窗口过程内部捕获所有异常，异常不越过系统回调；
Git 机器输出走 stdout、致命信息走 stderr，两条流分别捕获后再解析，避免合并读取打乱按行取字段。

## 5. 尚未实现（后续步骤）

工作区状态解析（未暂存/已暂存文件列表与 diff 查看）、外部命令窗口执行器、
暂存/取消暂存、创建提交、提交历史、撤回提交、fetch/pull/push、
配置持久化、自动更新与安装器。

（Git 可执行文件的发现、选择与验证已在步骤 2 完成；仓库路径选择、仓库识别与当前分支摘要已在步骤 3 完成；
可重复运行的临时 Git 仓库测试夹具已在步骤 4 完成，见 2.1。）
