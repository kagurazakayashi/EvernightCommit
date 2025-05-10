# EvernightCommit 测试指南

本文是稳定、可复用的开发测试参考：怎么构建、怎么按组跑、每组授权给谁、
隔离规则与环境矩阵怎么定义。某一轮的实际结果、日志与失败现场不进本文，
按惯例记入本地开发记录（`.evernight-local/` 目录，不随仓库分发；没有该目录的
读者可忽略其具体位置，只需知道结果记录不在本仓库内）。

## 1. 测试入口

全部用例编译进单一控制台程序 `gc_tests.exe`（与正式程序共享同一条命令窗口
辅助入口链路），并注册为一个 ctest 用例 `gc_tests`：

| 入口 | 命令 | 说明 |
| --- | --- | --- |
| 构建 | `cmake --build --preset ninja-debug` | 先需要 `cmake --preset ninja-x64` 完成配置（见 `building.md`） |
| 逐用例全量 | `build\ninja-x64\bin\Debug\gc_tests.exe` | 跑全部注册用例，含会在隔离夹具里 `git commit`/`git push` 的一组 |
| 全量（CTest） | `ctest --preset ninja-debug` | 同上，经 ctest 汇总 |
| 分组运行 | `gc_tests.exe --group <组名>` | 组名见下节；这是「先说清哪组能跑、再跑」的推荐入口 |
| 列名单 | `gc_tests.exe --list [--group <组名>] [姓名片段]` | 输出 `组名<TAB>用例名`，供脚本与核对 |
| 姓名片段 | `gc_tests.exe <片段>` | 包含匹配；**命中 0 个时返回退出码 2，绝不报「通过」** |

退出码约定：`0` 全部通过；`1` 有断言/前置失败；`2` 过滤器命中 0 个（没有运行
任何东西，不算结果）；`3` 分组表漂移或未知分组（拒绝在名单与注册集不一致时运行）。

汇总行的固定形态：`注册总数 N：pure a / windows b / git-readonly c / git-mutating d / unclassified e`，
随后 `本次执行 M 个（组外未执行 N-M 个）：失败 x 个、前置失败 y 个、断言失败 z 处`。
汇报测试结果时必须带上这三类计数（执行 / 失败 / 未运行），只有「执行数=0」或
「未说明未运行数」的「全绿」不可采信。

## 2. 分组与授权边界

分组表与判定规则维护在 `tests/support/test_main.cpp` 顶部注释里；四组的含义：

| 组名 | 判定标准 | 授权 |
| --- | --- | --- |
| `pure` | 纯逻辑：不启动任何子进程，不碰真实 Git；构造桩输入、判读与参数形态 | AI 会话与开发者均可直接跑 |
| `windows` | 真实 Win32 集成：起 `cmd.exe`、建真实临时目录/句柄/联接，但逐文件核实**不启动 Git** | 同上 |
| `git-readonly` | 真实 Git 集成，但逐例核实「用例体 + 其调用到的夹具辅助函数」都不出现 `git commit`/`git push`（`init`/`add`/`config`/`fetch`/`update-ref`/`hash-object`/`merge --no-commit`/`ls-files`/`status` 等非提交形态允许） | 同上；会弹真实命令窗口的用例自动收起 |
| `git-mutating` | 夹具内部执行真实 `git commit`/`git push`（含 `RemoteRig::Prepare`） | **只由项目所有者本人运行**：本项目的开发协作约束（不随仓库分发）禁止 AI 会话执行或代发提交/推送，也不得用脚本、测试或 API 等等价手段绕过 |

判定「只读」以逐行核实为准，注释与文件名不可作数；拿不准一律留在 `git-mutating`。
新增测试文件必须登记进 `test_main.cpp` 的三张文件表之一，否则其用例落入
`unclassified`：任何分组选择都不含它、只允许全量运行，且每次运行都会打印缺口警告。
`kVerifiedReadOnlyCases` 白名单每条必须恰好命中一个注册用例，用例改名、删除或
搬移文件而不同步本表时，运行器在任何模式下启动即失败——这是防止「含提交的用例
被错误放进可自跑组」的机制，修表时要把名单改到与注册集一致，而不是删校验。

推荐的分步命令（依次执行，各自独立成账）：

```bat
build\ninja-x64\bin\Debug\gc_tests.exe --group pure
build\ninja-x64\bin\Debug\gc_tests.exe --group windows
build\ninja-x64\bin\Debug\gc_tests.exe --group git-readonly
rem 下面这条含真实 commit/push，只由用户本人执行：
build\ninja-x64\bin\Debug\gc_tests.exe --group git-mutating
```

## 3. 夹具隔离规则（跑之前先确认可信）

真实 Git 用例都在**用例自有**的临时根里动作，规则由 `tests/support/git_fixture.{h,cpp}` 强制：

- 临时根用 `CreateDirectoryW` 原子认领（撞名即换随机名重试），只有创建成功者拥有并负责删除；
  设置环境变量 `GC_TEST_KEEP_TEMP=1` 可让用例结束后保留现场并打印路径（默认一律清理）。
- 每条命令的工作目录折叠为绝对路径并断言仍位于临时根内；越界直接抛前置失败。
- 子进程环境块与用户完全隔离：`GIT_CONFIG_GLOBAL` 指向夹具自己的空档案、
  `GIT_CONFIG_NOSYSTEM` 屏蔽系统配置、`GIT_TEMPLATE_DIR` 指向空模板目录，
  因此用户的 hooks、别名、凭据助手、签名配置一律不可见；提交身份固定为测试姓名/邮箱，
  提交时间取固定基准 + 序号，结果可复现。
- 传输限制：`GIT_ALLOW_PROTOCOL=file`；远端 URL 必须通过 `IsAllowedTestRemote` 守卫——
  含 `://` 的（含 `file://` 网络共享形态）、UNC、scp 形态一律拒绝，只允许「盘符绝对路径且
  折叠后仍在临时根内」。开发仓库与用户真实仓库永远不会成为夹具的目标。
- 串行前提：`environment_block_tests.cpp` 与个别绑定用例会在**测试进程作用域**内临时改
  `HOME`/`USERPROFILE`/`GIT_*` 并以 RAII 精确复原；这些用例不得并发运行。
  当前运行器是单进程串行，若将来并行化必须先把这类用例标注隔离域。

## 4. 边界矩阵的定义

矩阵的每一格按「行为后果」验收，不接受只断言参数拼接的通过。自动化能覆盖的格
已在用例里钉住；下表列出的形态需要真实环境，属**人工/独立环境验证**，未跑过就写未跑过：

| 形态 | 自动化覆盖 | 待人工实测的判定 |
| --- | --- | --- |
| 系统 ANSI = UTF-8（Beta UTF-8 开启） | 开发机即此形态的全部命令窗口/夹具用例 | — |
| 传统代码页 936 / 西文 1252、437 | 判定链路不读系统码页（`utf`/说明书/标题用例） | 命令窗口打开且中文与 emoji 文件名显示正常；窗口内手工跑传统 GBK 程序的显示预期；状态栏与确认框文案可读 |
| 中文用户名（`%TEMP%` 天然非 ASCII） | `command_window_works_with_non_ascii_temp_directory`（进程级 TEMP 指向中文目录） | 真实中文用户名登录形态下点一次 `status` |
| 多实例（两个程序进程并排） | 同进程第二执行器实例 + 句柄持有覆盖同一判定（`directories_are_unique_and_reclaimed`、`active_operation_survives_stale_sweep`） | 启动第二个实例并确认：第一个实例进行中的操作目录不被清扫误删、窗口仍在、状态栏仍显示进行中 |
| 浅仓库 / 部分克隆 | 判读层纯逻辑全矩阵（`interpret_*` 桩组合） | 落地钉：`undo_probe_` 组的 `shallow_clone_*` 两条与 `git_env_identity_commit`（含真实提交，用户运行）；撤回预检的父对象可读性问法为 `rev-parse --verify --quiet <ID>^{commit}`（实测 Git 2.53：`cat-file -t --quiet` 是用法错误 129，不得使用） |
| 超限与截断输出 | `completeness_*`（记录约定）、`capture_pipe_*`（真实管道：等于上限、超上限、退避排空、双流、子孙持管、快速退出后仍有缓冲） | 生产默认 16 MiB 上限只用注入小上限覆盖同一分支；真实 16 MiB 端到端与「非 UTF-8 文件名整份拒绝」需在真仓库形态点一次 `status` |
| 无值/裸键配置 | `completeness_push_config_accepts_valueless_boolean_record`、`fetch_scope_*` 裸键用例 | 端到端：`push_valueless_boolean_mirror_config_is_understood_and_neutralized`（用户运行） |
| 多 URL 改写（insteadOf/pushInsteadOf、多 pushurl） | 解析与展开的纯逻辑用例 | 传输层落点：`push_reaches_every_one_of_multiple_push_urls`、`push_resolves_two_urls_rewritten_by_push_instead_of`（用户运行） |
| 确认后仓库被外部改动 | 复核比对纯逻辑（`commit_identity_change_*`、`pull_recheck`、`DescribeUndoRecheckMismatch`） | 真实作废旧方案：`commit_identity_fixture_voids_confirmed_plan_*` 三条、`executed_plan_is_rejected_*`、`push_execution_recheck_*`、`pull_execution_recheck_*`（用户运行）；或真机在确认框弹出后于外部终端切分支/改暂存 |
| 终态与退出收尾 | 观察循环用例（`settles_while_console_window_still_open`、`git_not_started`、`early_close_settles_terminated`）、`shutdown_*`、协调器结案一次 | 组策略禁用 `cmd.exe` 的机器：窗口退化为「即关」而判定不受影响；退出时恰有一个操作在途的确认框行为 |
| 更旧的 Git 版本 | 拒绝路径按「Git 原生用法错误」设计（`--rebase-merges`、`get-url --all` 等） | 未在本机之外验证；如需支持要另备旧版本环境实测 |

## 5. 结果记录与现场保留

- 一轮验证的汇总（通过/失败/受权限限制未运行/环境缺失未运行）、机器信息、
  每条命令的退出码与日志路径，记录在本地开发记录（`.evernight-local/`，不随仓库分发），不进本文。
- 失败现场：设 `GC_TEST_KEEP_TEMP=1` 后重跑该用例，夹具会打印保留的临时根路径；
  命令窗口操作目录在 `%TEMP%\GcOp<进程号>x<随机段>`，超阈值且无人持有后由下一次启动清扫。
- 汇报口径：未运行的分组必须写明「未运行及原因」（权限边界或未具备的环境），
  不得以「过滤后全绿」代替全平台通过；「241/241 通过」这类数字必须同时给出
  它对应的组与执行方式才可引用。

## 6. 明确的边界

- 本项目的验证不依赖任何远程 CI：不上传仓库内容、不发布构建产物。若将来引入 CI，
  必须另立文档说明其权限、平台矩阵与「不得访问生产远端」的边界，且远程 runner 上
  运行 `git-mutating` 组同样需要先获得用户授权。
- 任何测试都不指向真实远端与用户仓库；开发过程中若需临时验证 Git 行为，
  一律在会话自有的临时目录里进行，且不得执行 `git commit`/`git push`
  （与本项目对开发会话的一贯约束一致：提交/推送始终由项目所有者本人决定并执行）。
