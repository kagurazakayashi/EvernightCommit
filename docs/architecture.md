# EvernightCommit 代码结构

本文说明各目录与模块的职责边界，是稳定参考资料。逐操作的用户可见行为规格见
`git-behavior.md`；测试入口与分组授权见 `testing.md`；构建与环境要求见 `building.md`。

## 分层原则

- `src/git/` 是纯逻辑层：Git 数据模型、机器输出解析、参数方案构造、前提判定与
  操作范围说明文字。这一层不碰 Win32、不读文件系统、不启动子进程，全部可用桩
  输入完整测试。
- `src/app/` 承载应用状态、任务协调、仓库绑定、表单会话状态，以及与窗口无关的
  操作编排：写操作准入门（哪个流程可以开始、其余流程以何种顺序拒绝、拒绝文案
  原文）与各操作的终端结论文字都在这里；操作历史的记录数据模型与按记录恢复的
  准入判定也在这里（这一层不碰文件系统与时钟）。
- `src/platform/`（只有头文件）是**平台无关的能力契约**：进程之外的系统能力——
  后台任务与任务代次、完成通知的投递目标、本机时区换算、程序自绘对话框的内容——
  在这里只声明数据与语义，不含任何原生句柄。目录里的 `git_task_worker.h` 是唯一的
  例外：代次作废与工作线程这套规则与操作系统无关，实现也在这一层。
- `src/platform/windows/` 承载进程创建、环境块装配、Unicode/路径处理、管道、
  临时文件、时钟与其他原生集成，并**实现**上面那份契约里由系统负责的部分。
- `src/ui/` 分成两半：九个操作控制器与 `operation_host.h` 那条窄接口属于共用界面逻辑，
  只依赖 `app/`、`git/` 与 `src/platform/` 契约；窗口过程、控件、布局、消息号表是
  **Win32 过渡实现**，将来由统一的 wxWidgets 界面取代（见「过渡期的 Win32 界面」）。
- `tests/` 是纯逻辑测试与明确标注的隔离 Windows/Git 集成测试。
- `resources/` 是图标、版本资源、manifest 等应用资源。

平台 I/O 不进入 Git 规划与解析代码；业务流转不进入控件布局代码。

## 依赖方向（本轮拆分后的实际状态）

```
src/git/ ──►（无依赖，纯 std）
src/app/ ──► src/git/
src/platform/*.h（契约） ──► src/git/、src/app/（只取数据模型）
src/platform/windows/ ──► 契约 + src/git/ + src/app/     （Win32 实现，含 subprocess/utf_text/win_path 等）
src/ui/*flow*、operation_host.h（控制器与窄接口） ──► 契约 + src/app/ + src/git/
src/ui/ 其余（main_window、控件、布局、commands.h） ──► 上面全部 + <windows.h>
```

三条可核对的硬边界（`docs/building.md` 给了 Linux 侧的一条命令化检查）：

1. `src/git/`、`src/app/`、`src/platform/*.h` 里没有任何 `#include <windows.h>`，
   也没有对 `platform/windows/` 头文件的包含（注释里指向实现位置不算）。
2. `src/ui/operation_host.h` 不出现 `HWND`/`HFONT`/`WM_APP`/对话框模板 ID；
   原生句柄只在 `src/ui/main_window.*` 与 `src/platform/windows/` 内部出现，
   `src/platform/windows/notification.cpp` 是全工程唯一把 `NotificationTarget`
   解释成 `HWND` 的地方。
3. 共用的 git/app 层不反向依赖界面：`gc_core` 不链接 `gc_platform`、`gc_ui`。

## src/platform/（平台无关契约，只有头文件）

| 文件 | 提供什么 | Windows 实现落在哪 |
| --- | --- | --- |
| `notification.h` | `NotificationTarget`（不透明投递目标）、`CompletionMessageId`（完成通道号）、`PostCompletionNotification()` | `windows/notification.cpp`（`PostMessageW`，wParam 带序号） |
| `git_task_worker.h` | `StopFlag`/`StopRequested` 与 `GitTaskWorker<请求,结果>`：工作线程执行、按提交序号作废旧结果、`BeginStop`+`Join` 两阶段收尾 | 全部就在这份头文件里（std 线程与互斥），只有通知投递走 `notification.h` |
| `dialogs.h` | `ChoiceItem`、`SingleChoiceRequest`、`TextPromptRequest`、`DialogLayoutHints`：单选列表框与单行输入框要问什么（取消由调用方拿到空答复表示，不设结果结构体），以及 DIP 行高与内容宽度 | `windows/identity_prompt.*`、`windows/remote_choice_dialog.*`（模板、字体、DPI 像素换算），由 `ui/main_window.*` 的 `ToRemoteChoiceSpec`/`ToIdentityPromptSpec` 翻译 |
| `local_time.h` | `LocalInstant`、`ResolveLocalWallTime`、`CurrentLocalInstant`、`LocalTimeZoneName`、`FormatLocalEpochSeconds` | `windows/local_time.cpp`（`SYSTEMTIME`/`TIME_ZONE_INFORMATION`）；`windows/local_time.h` 另留两个 `SYSTEMTIME` 换算，只给 Win32 控件用 |

契约里的 `PostCompletionNotification` 与那几句时间换算是**声明在共用层、实现在平台库**的
自由函数：一个构建里只有一份实现，链接期就能发现漏实现，比虚表接口更省一层间接。
后续要收敛但本轮**没有**动的能力（仍在 `platform/windows/` 里被界面直接调用）：
子进程执行器、环境块装配、路径与 Git 目录探测、临时文件写出、剪贴板、
`persistent_store`/`operation_history_store` 的读锁与原子发布、目录与可执行文件选择框。
它们目前只被 Win32 过渡界面和平台库内部消费，等 wxWidgets 界面落地时再按同一模式提出来，
现在先套一层抽象只会多出没有第二个实现来验证的空壳。

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
| `commit_detail` | 选中那一条提交的完整信息：`git log -n 1` 七字段（完整 ID、作者姓名与邮箱、两个时间、标题、描述）NUL 参数构造与整份作废式解析、答非所问（对象 ID 与发起时不是同一枚）时拒绝、描述超上限时截断且不切断代理对、面板与剪贴板共用那份 `标签：值` 排版文本与 CRLF 统一 |
| `command_window` | 外部命令窗口计划：命令行引号区域校验、操作说明书序列化与解析、结果解析、完成状态判定 |
| `staging_plan` | 暂存/取消暂存方案：选中条目→字面 pathspec 清单与参数两种形态、Git 能力判定、未合并与子模块的确认文案 |
| `commit_identity` | 「姓名 <邮箱>」解析/校验/同一位判定/列表去重 |
| `commit_message` | 表单→标题+描述+`Co-authored-by` trailer 合成、trailer 段重复条目处理、校验结论文案 |
| `commit_date` | 墙上时间↔UTC 秒的历法算法、`@<秒> <±hhmm>` 形态与 1970—2099 边界 |
| `commit_plan` | 创建提交方案：拒绝原因或 `commit --cleanup=verbatim -F <文件>` + 四项环境覆盖 + 确认文字；另含「这次提交绑定的那组事实」的参数构造与判读（`InterpretCommitIdentity`）、预检与界面现状对不上时的拒绝、确认之后逐处比对的作废说明（`DescribeCommitIdentityChange`） |
| `author_config` | `git config --null --get` 的参数与回答归类、作者默认值与提交者可得性判定 |
| `undo_commit_plan` | 撤回方案：预检参数与各组回答判读、浅边界与真正根提交的区分、远端包含三态、目标引用完整性（三问：Git 的 `check-ref-format` 认不认这个名字、这条分支引用本身是不是符号引用、有没有被别的 `git worktree` 检出，外加认出本工作树自己检出的是哪条分支——任一问不到即不放行）、拒绝/强制确认/普通确认分类、两种带预期旧值且 `--no-deref` 的 `update-ref` 命令形态、确认文字与恢复线索 |
| `fetch_plan` | fetch 目标判读：三条只读查询的参数构造、`git remote -v` 解析、目标判定（配置命中即 ready／否则列候选／一个也没有或查询失败即 blocked 且不猜 origin） |
| `fetch_scope` | 抓取范围策略：`git config --null --get-regexp` 的构造与判读、四个中和项与点名单个远端合成的命令形态、`remote.<远端>.fetch` 映射逐条验证（越界即拒绝）、确认正文与范围说明——界面 `fetch` 按钮与 `pull` 第一步共用这同一份 |
| `pull_plan` | pull 两阶段判读与方案：阶段一/阶段二查询的构造与判读、前提拒绝、四种关系、按 Git 原生解析矩阵把策略与快进意愿翻译成显式命令行参数、风险清单、执行前复核 `DescribePullChange`、未合并清单解析 |
| `push_plan` | push 判读与方案：预检查询构造与判读（含 `config --list --null` 三种记录形态与三态布尔判读、`remote get-url --push --all` 逐条展开发布地址）、发布远端优先序裁定、命令形态与条件中和、前提拒绝、风险清单、执行前复核 `DescribePushChange`、推送后逐目标核实与四种结论、URL 内嵌凭据掩码；面向展示与**入库/导出**的掩码分开的两份（`MaskStoredPushUrl`／`MaskStoredPushUrlInText`：除 userinfo 外还丢弃 query 与 `#` 片段，scp 形态的 `user@host:path` 只掩用户名，本地/UNC/相对路径与邮箱、分支名不动） |
| `first_push_plan` | 首次推送（分支还没有上游）判读与方案：向导可用性裁决、候选远端与逐远端发布地址的判读（与 `push_plan` 共用 `ResolvePushUrls`／`InspectPushScopeConfig`／形态判定）、目标分支名交给 `check-ref-format` 的裁定、逐发布地址 `ls-remote` 的「没有／问不到」三态聚合、可选的非快进关系查询、把「推送」与「两条 `git config` 上游写入」分成各自有结果的方案、执行前复核 `DescribeFirstPushChange`，以及**写配置之前**那一次前提复核 `DescribeUpstreamWriteStaleness`（分支是否还在、上游是否仍没配、要不要覆盖已有值） |
| `restore_plan` | 按操作历史恢复的判读与方案：把记录里的恢复线索与刚读回的仓库实况合成「能不能安全恢复」的裁决（引用现值／目标对象本地可读性——`<ID>^{commit}` 只回答能不能解析，不是「从分支可达」／有没有流程停着／引用名的 Git 权威裁定、形态与占用，并认出本工作树自己检出的那条分支以便讲清「相对 HEAD 的差异会变」），两种带预期旧值且 `--no-deref` 的 `update-ref` 命令形态、不可行与读不到的分类拒绝文字、确认后的现场复核 `DescribeRestoreRecheckMismatch`，以及三种命令表示的唯一构造点 `DescribeRestoreCopy`（数据数组／预览／按目标 shell 真实规则编排的可粘贴行，表达不了时只给逐段字段清单） |
| `shell_text` | 一条命令三种表示里「展示」与「可粘贴」两档的唯一构造点：把一份已审查的参数数组渲染成逐段预览（`[参数]`，各流程确认框都取自它，不再有任何一处自己按空格拼接）、渲染成给人复制的命令文本（cmd 与 PowerShell 两种目标的引号规则；无法可靠表达的字符——`"` `%` `!`、控制字符、以反斜杠收尾的需引号参数——一律拒绝而不是替换），并给出逐段字段清单；`DescribeDataForDisplay` 把被拒数据里的控制字符按 `\xNN` 显示，供拒绝原因与清单使用（只影响阅读形态，数据数组始终原值） |
| `conflict_state` | 冲突与暂停流程判读：Git 目录痕迹（存在性 + 限长内容）判出停着的到底是哪一种流程（变基目录优先，`CHERRY_PICK_HEAD` 等同伴痕迹不算并存；认不出/不一致/`BISECT_LOG`/只有 `SQUASH_MSG` 一律拒绝且不猜恢复方式），未合并清单与分支/HEAD 的三态判读（与 `pull_plan` 共用那条 `diff --diff-filter=U` 查询），「继续前提」与「中止风险」两套确认文字、`-c submodule.recurse=false <子命令> --continue/--abort` 的命令形态（不传 `--no-verify`/`--no-edit`/`-m`），以及确认前后两份现场的执行前复核 `DescribeConflictStateChange` |
| `submodule_navigation` | 子模块导航判读：`ls-files -s -z -- :(literal)<路径>` / `rev-parse --verify --quiet HEAD:<路径>` / 子模块自己 HEAD 三条只读查询的参数构造，gitlink 记录（`160000` + 完整对象 ID）判读，「索引 / 父提交 / 子模块 HEAD」三份位置合成六种结论，进入前的身份裁决（目录形态、父仓库是否同一个、未初始化与独立仓库分开拒绝），返回后的提示措辞与「不 add、不提交、不联网」声明 |

## src/app/

| 模块 | 职责 |
| --- | --- |
| `app_state` | 界面读写状态（含 Git 验证结果与仓库识别结果），不直接访问 Git；每个写操作各有独立接通开关 |
| `task_coordinator` | 仓库身份版本、刷新合并槽、外部操作单槽、成败以 Git 退出码为准、状态栏文案拼接；另提供 `RepositoryBinding`（一次编排流程开始时快照下的 Git 路径/工作区根/绝对 Git 目录 + 代号），供控制器在复核与命令终态回来时判断「界面还是不是当初那个仓库」 |
| `list_view_memory` | 按条目身份记忆/映射多选（更改列表按仓库相对路径，提交历史按完整对象 ID） |
| `commit_form_session` | 区分「默认值填的」与「用户写的」、换仓库时要不要问保留/放弃、提交成功后只清正文栏位 |
| `operation_gate` | 写操作准入门（七个被编排流程：提交、撤回、fetch、pull、推送、冲突处理、按记录恢复。每个流程自己已在走时的拒绝说法、以及它要等哪些在途流程，都是逐字钉住的原文；这套互斥刻意不对称——冲突处理与按记录恢复等全部其它流程，其它六个不因它们额外被拦（提交/撤回/pull 本来就在方案层按流程痕迹拒绝，命令窗口一占槽位都会互斥）；创建提交的只读核对不拦任何人；fetch 与 pull 的只读预检可以并存；导航则一律等全部七个）：哪个编排流程可以开始、其余流程按何种顺序拒绝、拒绝文案原文；另含仓库导航（进入子模块/返回父仓库）的准入 `DescribeNavigationRefusal`——它不写任何东西，但会整个换掉绑定的仓库，因此在途流程一律先结束 |
| `operation_history` | 可选的操作历史数据模型与版本化文本格式：记录字段（唯一 ID、时间、工作区身份、操作种类、确认过的源/目标引用与完整对象 ID、启动与终态、逐目标核实结果、引用级恢复线索）、整份严格判读与损坏/更高版本判定、保留期与条数裁剪、脱敏（发布地址、凭据形态）、多实例按记录 ID 追加合并（更强的终态保留并报告）。在途记录（已启动未见结果）与终态升级（inProgress < unknown < settled）在这里裁定，`DescribeHistoryCaptureRefusal` 也是这里对界面入口的边界说明。纯逻辑，不碰文件系统与时钟 |
| `decimal_text` | 有界十进制解析：无符号/带符号两条路径都在乘加之前做上限检查（`value > (upper - digit) / 10`），超限即拒绝而不是回绕；持久化与历史文件里所有非负整数（版本号、保留天数、条数、草稿长度）都经这里，调用方各自给出自己的上界 |
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
验证。数据与合并语义全在 `app/persistent_state`，这一层只搬字节）、
`operation_history_store`（同一目录下的另一份档案 `history.prefs` 与它自己的 `history.prefs.lock`
租约、16 MiB 整份大小上限、同样的限长严格 UTF-8 判读、损坏改名保留、原子发布；
测试一律传入自己的受控临时目录。语义全在 `app/operation_history`）、
`clipboard`（把已审查的参数数组或逐段字段清单以文本形态交给剪贴板，不做任何改写）。

命令窗口：`command_window_helper`（辅助入口：自己开控制台、按操作目录里的说明书执行
Git、写标记与结果文件，再把窗口交给 `cmd /k`；绝不接受调用方给的可执行文件或参数）、
`command_window_runner`（`CreateProcessW` 启动、观察线程、按操作 ID 绑定完成事件、
`Shutdown` 不杀 Git）。

写操作文件：`pathspec_file`（UTF-8 + NUL 分隔清单、独占创建、Git 退出后回收）、
`commit_message_file`（正文写成 UTF-8 临时档案交给 `git commit -F`，空正文拒绝写出）。

预检编排（执行依赖全部回调注入，均提供按序号作废旧结果的后台控制器别名）：
`git_toolchain`、`repo_detect`、`workspace_status`、`author_config`、`commit_changes`
（选中一条提交 → 它相对第一个父提交改了哪些文件，`git show --format= --name-status -z --first-parent`）、
`commit_detail`（选中一条提交 → 底部「提交详情」带要的那一条完整信息，一次 `git log -n 1` 七字段只读查询；
时间与「最近提交」那一列共用 `local_time` 的本机换算，落地时带回 `git::FormatCommitDetailText` 排好的可复制文本）、
`commit_probe`
（确认前预检与确认后复核走同一份查询路径）、`undo_probe`（含引用形态那两条：分支名自身的
`symbolic-ref --quiet` 与 `worktree list --porcelain`）、`fetch_probe`、
`pull_probe`（含整合失败后的只读取证）、
`conflict_probe`（流程痕迹的存在性与限长内容读取 + 未合并条目/分支/HEAD 三条只读查询，
预检与执行前复核、失败后的现场读取共用同一个函数，全程不写任何东西）、`push_probe`（预检 + 逐发布 URL 的核实链）、`submodule_probe`（进入前的目录形态 + 识别 + 父索引 gitlink，返回后的三份位置；全程只读）、
`restore_probe`（按记录恢复的那一组只读事实：分支现在指着什么、要挪回去的对象在本地读不读得到、引用形态与
占用、有没有流程停着；确认后复核复用同一份收集函数，输出 `git::RestoreRecheckScene`）。
`win_path` 在这里额外提供 `ProbeRepositoryWorkflowStateStrict`：每个痕迹三态（ absent / present /
unreadable），Git 目录读不动或任一痕迹读不回来时整轮 `readable=false` 并带上原因，绝不把
「看不见」当成「没有流程」。

界面辅助：`path_picker`、`remote_choice_dialog`（模态单选，也复用为 pull 的
合并/变基选择与操作历史菜单；它收的是 `platform::RemoteChoiceSpec`——含 `HFONT` 与像素布局，
只有 `ui/main_window.*` 会从共用契约 `platform::SingleChoiceRequest` 翻译过来）、
`identity_prompt`（合作者输入框，校验不过不关闭，同一套翻译规矩）、`notification.cpp`
（`PostCompletionNotification` 的 Win32 实现）、`clipboard`、`local_time.h` 里那两个
`SYSTEMTIME` 换算。`git_verify_worker` 是 Git --version 验证的后台别名，
`platform::git_task_worker` 的代次机制在共用层，这里只剩 `GitExeVerification` 这个结果类型。

## src/ui/

本目录两类文件要分开读：九个操作控制器加 `operation_host.h` 那条窄接口是随统一界面保留的
共用部分，其余（`main_window`、控件、布局、度量、消息号表）是 Win32 过渡实现，
逐文件名单见下面的「过渡期的 Win32 界面」。

`main_window`（窗口与消息循环）、`repo_bar`、`changes_pane`（一个选项卡控件两页：`变化信息`
是未暂存/箭头列/已暂存三栏，`提交历史` 是最近提交 | 分隔条 | 文件变化两栏，同一页底部再压一条横跨
整页的 `提交详情` 带——只读多行编辑框 + 标题行右端的 `复制详情` 按钮；页内容仍是主窗口上的普通子控件，
所以列表句柄、选中行与工具提示都不跨页搬家）、`action_bar`、
`commit_form`、`controls`、`layout`、`ui_metrics`、`splitter`、`resource_ids`、
`commands`，以及八个操作控制器：`commit_flow`、`undo_flow`、`fetch_flow`、
`pull_flow`、`push_flow`、`conflict_flow`（冲突与暂停流程的三个入口：一次只读现场读取按
阶段分流成「展示」「方案确认」与「执行前复核」，`--abort` 走风险确认框，命令没做成时再读回
现场补完结论；「没有痕迹不生成 `--abort`」「还有未合并条目不生成 `--continue`」都在
`git/conflict_state` 里裁好，控制器只按结论行动）、
`submodule_flow`（子模块导航：探测阶段、来路与代管的交还、返回后的指针核对）、
`restore_flow`（按记录恢复：开始时快照 `app::RepositoryBinding`，后台预检 → 确认 → 用绑定值
重问同一组事实 → 命令窗口；命令终态只按拿到的退出码说话，结果未知就说不确定，绝不自动重发，
界面换了仓库就把剩下的步骤停下）。
`push_flow` 另持有一台 `upstream_write_flow`：首次推送那条 push 报告成功之后，逐条把已审查的
`git config` 参数数组交进命令窗口（一条一个退出码），空参数表在交给执行器之前就按接线缺陷拒绝；
某条失败或界面换仓库时，把「推送那一步已成的部分」与「配置写到第几条」分开说，不自动重发也不自动回退。
`action_bar` 是两行：上一行摆 Git 操作按钮与状态说明，下一行从 `保存记录` 起是记录开关区
（`保存记录`/`保存草稿` 复选框与 `清除已存记录`，与操作历史那组之间留一个更宽的间隔），
主窗口据 `persistent_state` + `persistent_store` 做首次说明、防抖保存、恢复与关窗前 flush；
操作历史走同样那条收尾：启动当场落那条在途行、终态用同一个 ID 改写，关闭时把还没结案的那一条
补成「结果未知」再由那次保存落盘——只写记录，不中断仍在命令窗口里跑的 Git；
它不属于七个被编排的 Git 操作，不占用命令窗口槽位。控制器通过 `operation_host` 窄接口与主窗口交互；导航用的换绑定与表单收交分别是 `NavigateRepository` 与 `ClearFormForNavigation` / `ApplyHeldFormForNavigation`，仍走既有的识别→绑定→重读链路，不另开第二套。

## 过渡期的 Win32 界面

统一界面这一步还没开始，当前的 Windows 界面就是**过渡实现**，本轮把它和随迁移保留的部分
在目录与构建目标上分开列出（`CMakeLists.txt` 的 `gc_ui` 按两类分组注释）：

| 归类 | 文件 | 迁移时的处置 |
| --- | --- | --- |
| 随统一界面保留（共用） | `ui/operation_host.h`、`ui/commit_flow.*`、`undo_flow.*`、`fetch_flow.*`、`pull_flow.*`、`push_flow.*`、`conflict_flow.*`、`restore_flow.*`、`submodule_flow.*`、`upstream_write_flow.*` | 继续消费 `app/` 状态与 `src/platform/` 契约；需要处理的是下面两条遗留项 |
| Win32 过渡实现（将被 wxWidgets 取代） | `ui/main_window.*`、`controls.*`、`layout.*`、`ui_metrics.*`、`splitter.*`、`repo_bar.*`、`action_bar.*`、`commit_form.*`、`changes_pane.*`、`commands.h`、`resource_ids.h`、`src/main.cpp` | 窗口过程、控件、DPI 度量与消息号表都在这一档；`commands.h`/`resource_ids.h` 顶部有同样的标记 |

`tests/support/fake_operation_host.h` 实现的是共用那条窄接口，因此控制器的接线测试
在未来界面上原样可用。

## 已知待办差异（本轮刻意没做完的部分）

按影响大小排列，都不改变当前 Windows 行为，只是迁移到 wxWidgets/Linux 之前必须先处理的口子：

1. **字符串编码边界**：`git/`、`app/` 与窄接口普遍使用 `std::wstring`，在 Windows 上是
   UTF-16，在 Linux 上 `wchar_t` 是 32 位——同一段代码在两侧并不等价，代理对长度、
   `commit_detail` 的截断上限那类判定尤其明显。本轮不一次性改字符类型，
   先按 AGENTS.md 的要求把边界写清楚：Linux 落地时必须换成显式的 UTF-8 或 UTF-16 单元类型，
   转换失败一律拒绝，不许用替代字符冒充原值。
2. **完成通道还是 Win32 号**：控制器提交后台任务时传的是 `ui/commands.h` 里的 `WM_APP+n`
   （`CompletionMessageId` 已能原样承载）。迁移时要换成与消息循环无关的通道枚举，
   由各平台界面把通道映射到 wx 事件；`CommandWindowRunner` 那条 `kCompletionMessage`
   与命令窗口观察线程也一并跟着走。
3. **控制器仍直呼 `platform/windows/*_probe.h`**：这些头文件本轮已经不含任何 Windows 类型，
   请求/结果/依赖注入都是纯数据，`CollectXxx` 那套编排本来就能脱离 Win32 测试；
   留在 windows 目录的原因是 `RunXxxLoad` 里绑定了本平台的子进程执行器。
   下一步应把「请求 + 结果 + 依赖」提到 `src/platform/`，只把执行器装配留在各平台。
   同样情况见 `commit_message_file`、`pathspec_file`、`win_path`（`undo_flow.cpp` 用它读流程痕迹）。
4. **主窗口直接使唤平台设施**：`main_window.h` 里持久化存储、目录选择框、剪贴板、
   Git 工具链验证都是 Win32 实现的直接调用。这些接口目前只被过渡界面消费，
   等 wx 界面出现时再按 `dialogs.h` 那样提出契约，不在此刻凭空造抽象。
5. **构建与交付**：根 `CMakeLists.txt` 仍然只放行 Windows 构建（非 Win32 直接在配置阶段
   报错，报错文本已说明共用层可以单独做编译核对）。Linux 的 wxWidgets 依赖获取、
   链接方式与运行库部署要求都还没验证，不宣称支持。
6. **进程与终端**：命令窗口执行器依赖 `cmd /k` 与 `CreateProcessW`；Linux 侧要换成
   用户自己的终端模拟器，同时保留「用户触发的操作能看见真实命令与输出」这条产品行为。
   交互式 Git 的终端、hooks、SSH、凭据助手与编辑器环境沿用现有分类名单，
   PATH 分隔符（`;` 对 `:`）与盘符联动项那套规则需要按平台分叉。

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
