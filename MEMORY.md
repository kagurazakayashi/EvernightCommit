# EvernightCommit 会话交接（MEMORY.md）

本文件按根目录 `AGENTS.md` 的要求保存**阶段进度**，不复制整份实施计划；长期规则在 `AGENTS.md`，
稳定参考在 `README.md` 与 `docs/`。逐轮的构建/测试现场记录在本地开发记录
`.evernight-local/PROGRESS.md`（不随仓库分发）。

---

## 2026-10-16 会话：拆分共用业务逻辑与平台专用实现

### 当前阶段

架构拆分完成，Windows 程序行为不变（但本轮**未能在 Windows 上重新构建**，见「未验证」）。
Linux GUI 尚未开始，本轮明确不要求。前置事实：`src/git/`、`src/app/` 在本轮之前就已经是
不含 Windows 头的纯逻辑层，本轮补的是**界面窄接口**与**平台能力契约**这两段。

### 本轮已完成

1. **新增平台无关契约层 `src/platform/*.h`**（只有声明与模板，构建目标是新的 INTERFACE 库
   `gc_platform_contract`）：
   - `notification.h`：`NotificationTarget`（= `void*`，Win32 的 `HWND__*` 与 wx 的
     `wxEvtHandler*` 都能隐式转进来，反向必须显式 cast）、`CompletionMessageId`（= `uint32_t`）、
     `PostCompletionNotification()`（声明在共用层，实现在平台库）。
   - `git_task_worker.h`：**从 `src/platform/windows/` 整份搬上来**。`StopFlag`/`StopRequested` 与
     `GitTaskWorker<请求,结果>`（工作线程执行 + 按提交序号作废旧结果 + `BeginStop`/`Join`
     两阶段收尾）都是与操作系统无关的代次逻辑；原来唯一的 Win32 调用
     `::PostMessageW(window, message, serial, 0)` 换成 `PostCompletionNotification(...)`。
     原文件已删除，14 处 include 改为 `platform/git_task_worker.h`。
   - `dialogs.h`：`ChoiceItem` / `SingleChoiceRequest` / `SingleChoiceResult` /
     `TextPromptRequest` / `TextPromptResult` / `DialogLayoutHints`（DIP 行高与内容宽度），
     取代原先直接暴露在窄接口里的 `platform::RemoteChoiceSpec`、`IdentityPromptSpec`
     （那两个结构体仍留在 `platform/windows/`，含 `HFONT` 与像素布局）。
   - `local_time.h`：`LocalInstant`、`ResolveLocalWallTime`、`CurrentLocalInstant`、
     `LocalTimeZoneName`、`FormatLocalEpochSeconds`。`windows/local_time.h` 只留
     `CivilFromSystemTime`/`SystemTimeFromCivil` 这两个 `SYSTEMTIME` 换算（只给 Win32 控件用）。
2. **`src/ui/operation_host.h` 去掉 `<windows.h>`**：`OperationContext::notifyWindow`（HWND）
   改为 `notify`（`platform::NotificationTarget`）；
   `PromptRemoteChoice(RemoteChoiceSpec, RemoteChoiceLayoutHints)` 改为
   `PromptSingleChoice(platform::SingleChoiceRequest)`；
   `PromptForText(IdentityPromptSpec, TextInputLayoutHints)` 改为
   `PromptForText(platform::TextPromptRequest)`。取消 / 拒绝 / 超时 / 结果未知这些语义
   一个字没动，只换类型形态。
3. **Win32 侧按新接口接回**（没有第二套业务流程）：
   - `src/platform/windows/notification.cpp` 新增：`PostMessageW` 的实现，全工程唯一把
     `NotificationTarget` 解释成 `HWND` 的地方。
   - `src/ui/main_window.{h,cpp}` 新增两个私有 const 翻译函数 `ToRemoteChoiceSpec` /
     `ToIdentityPromptSpec`（填 `metrics_.Font()` 与 DPI 像素），
     `MakeCoauthorPrompt` 改为返回 `platform::TextPromptRequest`，
     `ShowOperationHistory` 的两个选择框与保留期限输入框改用中立请求 +
     `PromptSingleChoice`/`PromptForText`。
   - `fetch_flow.cpp`、`pull_flow.cpp`、`push_flow.cpp` 的对话框构造点改成中立类型；
     所有 `ctx.notifyWindow` 调用点改名 `ctx.notify`；这些文件不再 include
     `platform/windows/{identity_prompt,remote_choice_dialog}.h`。
   - `repo_detect.h` 删掉多余的 `<windows.h>`（该头没有原生类型）。
4. **过渡实现被明确标出**：`ui/commands.h`、`ui/main_window.h` 顶部加了标记注释；
   `CMakeLists.txt` 的 `gc_ui` 源列表按「九个操作控制器（随统一界面保留）/
   Win32 过渡实现（待 wxWidgets 取代）」两组分开；非 Win32 的配置期报错文本改为说明
   共用层可单独核对。
5. **边界核对固化成代码**：新增 `tests/portable_boundary_check.cpp`（不参与 Windows 构建、
   不在 `tests/CMakeLists.txt` 里），一次性 include 契约层 + 九个控制器头 +
   `ui/operation_host.h`，并用 `static_assert` 钉住 `NotificationTarget` 形态与
   worker 的代次 API；一条 `g++ -std=c++20 -fsyntax-only -Isrc tests/portable_boundary_check.cpp`
   就能回答「拆分有没有被改回去」。命令记录在 `docs/building.md` 第 5 节。

### 关键接口位置（下一会话直接读这几处）

| 想改什么 | 去哪儿 |
| --- | --- |
| 后台任务代次 / 完成通知怎么投 | `src/platform/git_task_worker.h`、`src/platform/notification.h`、`src/platform/windows/notification.cpp` |
| 单选框与单行输入框要问什么 | `src/platform/dialogs.h`（契约）、`src/ui/main_window.cpp` 的 `ToRemoteChoiceSpec`/`ToIdentityPromptSpec`（Win32 翻译）、`src/platform/windows/{identity_prompt,remote_choice_dialog}.cpp`（画出来） |
| 控制器与界面之间还能传递什么 | `src/ui/operation_host.h` |
| 本机时间 | `src/platform/local_time.h` + `src/platform/windows/local_time.cpp` |
| WM_APP 号 / 超时与防抖取值 | `src/ui/commands.h`（过渡实现专用） |
| wxWidgets 迁移要处理的口子 | `docs/architecture.md`「已知待办差异」六条 |

### 本轮实际运行过的验证（Linux x86_64，GCC 14，无 Windows SDK）

- `for f in src/git/*.cpp src/app/*.cpp; do g++ -std=c++20 -fsyntax-only -Isrc "$f"; done`
  → **36 个文件全部通过，0 失败**（共用业务逻辑层不含 Windows 头）。
- `g++ -std=c++20 -fsyntax-only -Isrc tests/portable_boundary_check.cpp` → **通过**
  （契约层四份 + 九个控制器头 + 窄接口，共 14 份头文件在 Linux 上可编译）。
- 纯逻辑用例**实际运行**：用 `/tmp` 下自写的极简运行器（自己实现 `tiny_test` 的
  `Report`/`Registrar`/`RunAll`）链接 24 个 `*_tests.cpp` + 36 个核心 `.cpp`，
  → **404 个用例，失败 0 个**。清单：`app_state commit_date commit_form_session commit_identity
  commit_message commit_plan conflict_state decimal_text fetch_plan fetch_scope first_push_plan
  git_environment git_locator git_probe operation_conclusions operation_gate operation_history
  persistent_state pull_plan push_plan shell_text staging_plan submodule_journey
  undo_commit_plan`。
  这**不是** Windows 回归，也不含 `windows`/`git-readonly`/`git-mutating` 任何分组；
  `repository_tests`、`workspace_status_tests` 因链接 `platform/windows` 实现而未纳入，
  `operation_history_store_file_tests`、`persistent_store_file_tests` 直接需要 `<windows.h>`。
- 顺带发现的既有可移植性缺口（**本轮没改**）：`task_coordinator_tests.cpp:393`、
  `restore_plan_tests.cpp:262` 用 `GC_CHECK` 传了两个实参（MSVC 只给 C4002 警告，GCC 直接报错）；
  `submodule_navigation_tests.cpp:89` 用了 `std::find` 却没 include `<algorithm>`。
- 三条 grep 边界核对（`docs/building.md` 第 5 节）：`<windows.h>` 与
  `platform/windows` 头在 `src/git`、`src/app`、`src/platform/*.h`、`src/ui/operation_host.h`
  中**零命中**；`HWND/HFONT/WM_APP` 等词只出现在注释里。
- **静态复查**（本机无法编译 Win32，改用 fresh-eyes 逐处核对整份 diff）：查出并修掉
  `tests/upstream_write_flow_tests.cpp:30` 仍写 `ctx.notifyWindow`（该文件在
  `tests/CMakeLists.txt` 里参与 Windows 构建，漏改会让 MSVC 直接编译失败）；
  删掉 `main_window.h` 里重复的 `remote_choice_dialog.h` include；
  删掉 `dialogs.h` 里没有任何实现使用的 `SingleChoiceResult`/`TextPromptResult`
  （窄接口用 `std::optional` 表达「取消」，不需要再一份结果结构体）。
  复查同时确认：`main_window.cpp` 六处 `Request(window, kXxxCompleted, …)` 的 `window` 是
  `HWND` 形参，可以隐式进 `void*`；`ui_metrics.h` 的取值函数全是 const，
  两个翻译函数按值取 `okText` 算按钮宽度与改动前逐像素等价。

### 未验证 / 明确没做

- **Windows 侧没有重新构建，也没有跑任何分组回归**：本机是 Linux，MSVC 与 Windows SDK 都不可用。
  `CMakeLists.txt` 本轮也改过（新增 INTERFACE 库、加一个 `.cpp`、`gc_ui` 源列表分组），
  同样只经过逐条比对磁盘文件名，**没跑过 `cmake` 配置**——首次 Windows 构建如果报
  「源文件不存在」或链接不到 `PostCompletionNotification`，先查这两处。
  Windows 上请项目所有者本人执行（仓库根目录，PowerShell）：
  `cmake --preset vs2026-x64` → `cmake --build --preset vs2026-debug` →
  `ctest --preset vs2026-debug`（或 `.\build.ps1`）。
  需要重点看的是本轮改过的 Win32 界面：合作者添加/修改输入框、fetch 选远端、
  pull 选整合方式、首次推送三步、操作历史菜单与保留期限输入框（弹窗文案、DPI 缩放、
  按钮宽度按 `okText` 计算这几处行为应与改动前完全一致）。
- 未做真实推送：本轮所有验证都不涉及 `git push`；含 push 的 `git-mutating` 分组未运行，
  按 `AGENTS.md` 只能由用户本人执行。
- 未提交、未推送。工作区改动清单见 `git status`（34 个已跟踪文件修改 + 6 个新文件；
  `src/platform/windows/git_task_worker.h` 已删除）。
  建议提交标题：**拆分共用业务逻辑与平台专用实现，界面窄接口不再包含 Windows 头**。

### 下一会话入口（按 `docs/architecture.md`「已知待办差异」的顺序）

1. 把九个控制器不再需要的 `ui/commands.h` 依赖换成平台无关的完成通道枚举
   （控制器目前仍传 `WM_APP+n` 给 worker），并让 `CommandWindowRunner` 的完成通知跟着走。
2. 把 `platform/windows/*_probe.h` 的「请求 + 结果 + 依赖注入」提到 `src/platform/`，
   只把子进程执行器装配留在各平台；同批考虑 `commit_message_file`、`pathspec_file`、`win_path`。
3. 字符串编码收口（`std::wstring` 的 UTF-16 语义 → 显式编码类型），这一步做之前
   Linux 上跑绿的纯逻辑用例不能当作行为等价的证据。
4. 之后才轮到 wxWidgets 界面本身与 `src/platform/linux/`。
   本轮**刻意没有**给 `persistent_store`、`clipboard`、`path_picker`、子进程执行器预先套抽象——
   目前没有第二个实现能验证它们。
