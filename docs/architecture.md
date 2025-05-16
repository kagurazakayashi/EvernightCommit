# EvernightCommit 代码结构

本文说明各目录与模块的职责边界，是稳定参考资料。逐操作的用户可见行为规格见
`git-behavior.md`；测试入口与分组授权见 `testing.md`；构建与环境要求见 `building.md`。

## 分层原则

- `src/git/` 是纯逻辑层：Git 数据模型、机器输出解析、参数方案构造、前提判定与
  操作范围说明文字。这一层不碰 Win32、不读文件系统、不启动子进程，全部可用桩
  输入完整测试。
- `src/app/` 承载应用状态、任务协调、仓库绑定、表单会话状态，以及与窗口无关的
  操作编排：写操作准入门（哪个流程可以开始、其余流程以何种顺序拒绝、拒绝文案
  原文）与各操作的终端结论文字都在这里。
- `src/platform/windows/` 承载进程创建、环境块装配、Unicode/路径处理、管道、
  临时文件、时钟与其他原生集成。
- `src/ui/` 承载 Win32 控件、事件、布局与用户确认。每个编排级 Git 操作
  （提交/撤回/fetch/pull/推送）有独立的操作控制器，持有自己的阶段、后台工作器、
  已确认方案与复核数据；控制器只通过窄接口 `ui/operation_host.h` 与不可变上下文
  快照接触主窗口，流程中途绝不读取主窗口的可变状态。
- `tests/` 是纯逻辑测试与明确标注的隔离 Windows/Git 集成测试。
- `resources/` 是图标、版本资源、manifest 等应用资源。

平台 I/O 不进入 Git 规划与解析代码；业务流转不进入控件布局代码。

## src/git/（纯逻辑）

| 模块 | 职责 |
| --- | --- |
| `git_locator` | 按 PATH 逐目录发现 `git.exe` 候选、去重 |
| `git_probe` | `git --version` 输出判定与版本号解析 |
| `git_environment` | 集中 Git 子进程环境策略：保留/删除/受控覆盖的分类名单、`GIT_CONFIG_KEY_<n>` 等数字后缀注入项识别、从继承环境核对被移除的重定向变量并生成只含变量名的告知文本。后台探测与命令窗口共用同一份名单 |
| `repository` | 仓库形态解析与分类（只接 UTF-16 文本） |
| `workspace_model` | 工作区条目分类、状态与路径显示、读取周期与空状态文案 |
| `workspace_status` | `status --porcelain=v2 -z` 参数构造与 NUL 记录解析（XY 两侧拆分、重命名双路径、未合并归侧、子模块 S 栏位） |
| `diff_view` | 差异视图的数据准备 |
| `commit_history` | `git log` 五字段 NUL 参数构造与分组解析、对象 ID 校验（不硬编码 SHA-1 长度）、合并提交识别、上限截断说明、双击详情的 `git show` 方案 |
| `command_window` | 外部命令窗口计划：命令行引号区域校验、操作说明书序列化与解析、结果解析、完成状态判定 |
| `staging_plan` | 暂存/取消暂存方案：选中条目→字面 pathspec 清单与参数两种形态、Git 能力判定、未合并与子模块的确认文案 |
| `commit_identity` | 「姓名 <邮箱>」解析/校验/同一位判定/列表去重 |
| `commit_message` | 表单→标题+描述+`Co-authored-by` trailer 合成、trailer 段重复条目处理、校验结论文案 |
| `commit_date` | 墙上时间↔UTC 秒的历法算法、`@<秒> <±hhmm>` 形态与 1970—2099 边界 |
| `commit_plan` | 创建提交方案：拒绝原因或 `commit --cleanup=verbatim -F <文件>` + 四项环境覆盖 + 确认文字；另含「这次提交绑定的那组事实」的参数构造与判读（`InterpretCommitIdentity`）、预检与界面现状对不上时的拒绝、确认之后逐处比对的作废说明（`DescribeCommitIdentityChange`） |
| `author_config` | `git config --null --get` 的参数与回答归类、作者默认值与提交者可得性判定 |
| `undo_commit_plan` | 撤回方案：预检参数与各组回答判读、浅边界与真正根提交的区分、远端包含三态、拒绝/强制确认/普通确认分类、两种带预期旧值的 `update-ref` 命令形态、确认文字与恢复线索 |
| `fetch_plan` | fetch 目标判读：三条只读查询的参数构造、`git remote -v` 解析、目标判定（配置命中即 ready／否则列候选／一个也没有或查询失败即 blocked 且不猜 origin） |
| `fetch_scope` | 抓取范围策略：`git config --null --get-regexp` 的构造与判读、四个中和项与点名单个远端合成的命令形态、`remote.<远端>.fetch` 映射逐条验证（越界即拒绝）、确认正文与范围说明——界面 `fetch` 按钮与 `pull` 第一步共用这同一份 |
| `pull_plan` | pull 两阶段判读与方案：阶段一/阶段二查询的构造与判读、前提拒绝、四种关系、按 Git 原生解析矩阵把策略与快进意愿翻译成显式命令行参数、风险清单、执行前复核 `DescribePullChange`、未合并清单解析 |
| `push_plan` | push 判读与方案：预检查询构造与判读（含 `config --list --null` 三种记录形态与三态布尔判读、`remote get-url --push --all` 逐条展开发布地址）、发布远端优先序裁定、命令形态与条件中和、前提拒绝、风险清单、执行前复核 `DescribePushChange`、推送后逐目标核实与四种结论、URL 内嵌凭据掩码 |
| `first_push_plan` | 首次推送（分支还没有上游）判读与方案：向导可用性裁决、候选远端与逐远端发布地址的判读（与 `push_plan` 共用 `ResolvePushUrls`／`InspectPushScopeConfig`／形态判定）、目标分支名交给 `check-ref-format` 的裁定、逐发布地址 `ls-remote` 的「没有／问不到」三态聚合、可选的非快进关系查询、把「推送」与「两条 `git config` 上游写入」分成各自有结果的方案、执行前复核 `DescribeFirstPushChange` |
| `conflict_state` | 冲突与暂停流程判读：Git 目录痕迹（存在性 + 限长内容）判出停着的到底是哪一种流程（变基目录优先，`CHERRY_PICK_HEAD` 等同伴痕迹不算并存；认不出/不一致/`BISECT_LOG`/只有 `SQUASH_MSG` 一律拒绝且不猜恢复方式），未合并清单与分支/HEAD 的三态判读（与 `pull_plan` 共用那条 `diff --diff-filter=U` 查询），「继续前提」与「中止风险」两套确认文字、`-c submodule.recurse=false <子命令> --continue/--abort` 的命令形态（不传 `--no-verify`/`--no-edit`/`-m`），以及确认前后两份现场的执行前复核 `DescribeConflictStateChange` |
| `submodule_navigation` | 子模块导航判读：`ls-files -s -z -- :(literal)<路径>` / `rev-parse --verify --quiet HEAD:<路径>` / 子模块自己 HEAD 三条只读查询的参数构造，gitlink 记录（`160000` + 完整对象 ID）判读，「索引 / 父提交 / 子模块 HEAD」三份位置合成六种结论，进入前的身份裁决（目录形态、父仓库是否同一个、未初始化与独立仓库分开拒绝），返回后的提示措辞与「不 add、不提交、不联网」声明 |

## src/app/

| 模块 | 职责 |
| --- | --- |
| `app_state` | 界面读写状态（含 Git 验证结果与仓库识别结果），不直接访问 Git；每个写操作各有独立接通开关 |
| `task_coordinator` | 仓库身份版本、刷新合并槽、外部操作单槽、成败以 Git 退出码为准、状态栏文案拼接 |
| `list_view_memory` | 按条目身份记忆/映射多选（更改列表按仓库相对路径，提交历史按完整对象 ID） |
| `commit_form_session` | 区分「默认值填的」与「用户写的」、换仓库时要不要问保留/放弃、提交成功后只清正文栏位 |
| `operation_gate` | 写操作准入门（六个被编排流程，冲突处理是第六个：它等全部五个，五个不因它额外被拦——提交/撤回/pull 本来就在方案层按流程痕迹拒绝，命令窗口一占槽位都会互斥；导航则一律等它）：哪个编排流程可以开始、其余流程按何种顺序拒绝、拒绝文案原文；另含仓库导航（进入子模块/返回父仓库）的准入 `DescribeNavigationRefusal`——它不写任何东西，但会整个换掉绑定的仓库，因此在途流程一律先结束 |
| `submodule_journey` | 父仓库⇄子模块导航的状态保管：来路栈（嵌套逐层退、同一条父→子不重复压）、按工作区根代管的表单草稿（空内容不存、交还即除号）、返回前的身份核对、`PlanDraftSwap` 四种交还场合与交还说明文字 |
| `persistent_state` | 可选持久化的数据模型与版本化文本格式（最近仓库/Git 路径/窗口布局/按 worktree 身份分的提交草稿）：严格转义与整份判读、超限与损坏/更高版本判定、未分版本老文件的迁移、多实例合并（盘上更新的草稿保留并报告、写入意图门控、删除墓碑传播、关闭草稿即清空、replaceAll）。纯逻辑，不碰文件系统与时钟 |
| `operation_conclusions` | 各操作的终端结论文字 |

## src/platform/windows/

进程与环境：`subprocess`（隐藏窗口子进程，stdout/stderr 各一线程捕获，四种收尾形态，
继承句柄只给属于该操作的那一对管道写端）、`environment_block`（覆盖/删除变量、
同名项大小写合并、盘符联动项原样保留、环境读取失败与真空环境分两种答复；
集中策略装配入口 `BuildGitChildEnvironment`）、`git_query_result`（唯一一处把子进程
捕获转成 `GitQueryResult` 的地方，生产后台查询统一走 `RunGitBackgroundQuery` /
`RunGitCaptured`，被移除的重定向变量以名字记在 `environmentNotice`）、
`platform_init`、`raii`、`win_path`（文件/目录/PATH 辅助与 Git 目录流程痕迹只读探测）。

编码与本机：`utf_text`（读入用宽松、说明书写出用严格，非法码元与 CESU 三字节代理项
一律失败，不用替代字符冒充原值）、`local_time`（墙上时间→UTC 瞬间与该刻实际生效
偏移，含夏令时；Unix 秒→本机时区展示文本）、`locale_text`。

持久化：`persistent_store`（当前用户应用数据目录 `%APPDATA%\EvernightCommit` 的解析、
限长且严格 UTF-8 判读的读取、`state.prefs.lock` 独占句柄租约写的保存锁、损坏原件改名保留、
临时文件独占创建 + 刷新 + 原子替换发布；更高版本文件拒绝读写；窗口几何恢复前的显示器可达性
验证。数据与合并语义全在 `app/persistent_state`，这一层只搬字节）。

命令窗口：`command_window_helper`（辅助入口：自己开控制台、按操作目录里的说明书执行
Git、写标记与结果文件，再把窗口交给 `cmd /k`；绝不接受调用方给的可执行文件或参数）、
`command_window_runner`（`CreateProcessW` 启动、观察线程、按操作 ID 绑定完成事件、
`Shutdown` 不杀 Git）。

写操作文件：`pathspec_file`（UTF-8 + NUL 分隔清单、独占创建、Git 退出后回收）、
`commit_message_file`（正文写成 UTF-8 临时档案交给 `git commit -F`，空正文拒绝写出）。

预检编排（执行依赖全部回调注入，均提供按序号作废旧结果的后台控制器别名）：
`git_toolchain`、`repo_detect`、`workspace_status`、`author_config`、`commit_probe`
（确认前预检与确认后复核走同一份查询路径）、`undo_probe`、`fetch_probe`、
`pull_probe`（含整合失败后的只读取证）、
`conflict_probe`（流程痕迹的存在性与限长内容读取 + 未合并条目/分支/HEAD 三条只读查询，
预检与执行前复核、失败后的现场读取共用同一个函数，全程不写任何东西）、`push_probe`（预检 + 逐发布 URL 的核实链）、`submodule_probe`（进入前的目录形态 + 识别 + 父索引 gitlink，返回后的三份位置；全程只读）。

界面辅助：`path_picker`、`remote_choice_dialog`（模态单选，也复用为 pull 的
合并/变基选择）、`identity_prompt`（合作者输入框，校验不过不关闭）、
`git_task_worker` / `git_verify_worker`（通用后台任务与核实控制器）。

## src/ui/

`main_window`（窗口与消息循环）、`repo_bar`、`changes_pane`、`action_bar`、
`commit_form`、`controls`、`layout`、`ui_metrics`、`splitter`、`resource_ids`、
`commands`，以及七个操作控制器：`commit_flow`、`undo_flow`、`fetch_flow`、
`pull_flow`、`push_flow`、`conflict_flow`（冲突与暂停流程的三个入口：一次只读现场读取按
阶段分流成「展示」「方案确认」与「执行前复核」，`--abort` 走风险确认框，命令没做成时再读回
现场补完结论；「没有痕迹不生成 `--abort`」「还有未合并条目不生成 `--continue`」都在
`git/conflict_state` 里裁好，控制器只按结论行动）、
`submodule_flow`（子模块导航：探测阶段、来路与代管的交还、返回后的指针核对）。
`action_bar` 右下角另有记录开关区（`保存记录`/`保存草稿` 复选框与 `清除已存记录`），
主窗口据 `persistent_state` + `persistent_store` 做首次说明、防抖保存、恢复与关窗前 flush；
它不属于六个被编排的 Git 操作，不占用命令窗口槽位。控制器通过 `operation_host` 窄接口与主窗口交互；导航用的换绑定与表单收交分别是 `NavigateRepository` 与 `ClearFormForNavigation` / `ApplyHeldFormForNavigation`，仍走既有的识别→绑定→重读链路，不另开第二套。

## tests/

`support/` 提供轻量断言框架与测试入口（无外部依赖、不联网、可按用例名片段或
分组运行），以及 `git_fixture`（RAII 临时目录 + 隔离环境块 + 真实提交辅助 +
测试远端守卫 + `RemoteRig` 三方形态）——仅测试链接，生产流程不调用。
`*_tests.cpp` 为纯逻辑测试，`*_fixture_tests.cpp` 为真实 Git/子进程集成测试。
分组口径、授权边界与夹具隔离规则见 `testing.md`，此处不再逐文件罗列用例清单。

## resources/ 与构建

`resources/app.manifest`：Common Controls v6、Per-Monitor V2 DPI 感知、`asInvoker`。
`resources/app.rc.in` 与 `src/ui/resource_ids.h` 共用同一份资源 ID 定义。
`CMakeLists.txt` / `CMakePresets.json` 见 `building.md`。

## 编码与进程约定

源码 UTF-8（`/utf-8`）；Win32 一律使用 Unicode/W API，控件与路径使用 UTF-16，
跨边界只经 `platform::Utf8ToUtf16` / `Utf16ToUtf8`（读入宽松、写出严格，失败就是
失败，不做替代改写）；系统 ANSI 码页不用于任何数据编码。`HWND`、`HFONT` 由 RAII
持有；窗口过程内部捕获所有异常，异常不越过系统回调。Git 机器输出走 stdout、
致命信息走 stderr，两条流分别捕获后再解析。
