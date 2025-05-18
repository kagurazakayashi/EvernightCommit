#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gc::app {

// 「操作历史」的数据模型与版本化文本格式（纯逻辑层）。
//
// 目的：为用户查看「这台机器上这个程序到底对仓库做过什么」和判断「怎么恢复」提供一份本地记录，
// 绝不自动替用户撤销任何东西。它是上一节「提交草稿与常用配置持久化」的姊妹功能，但刻意是
// *另一份文件*（history.prefs 而不是 state.prefs）：偏好是整体读写、按意图合并的快照，
// 历史是只追加、按条保留、有容量与保留期限的事件流，两种生命周期混在一个文件里只会让
// 合并规则互相踩踏，因此各走各的文件与各自的写锁。
//
// 为什么放在 app 层而不是平台层：这份数据的*语义*——一条记录算不算「已完成操作」、
// 复核能不能改写已落的终态、谁的记录更新、超限丢哪一条、哪些字段绝不落盘——
// 都必须能脱离文件系统与 Win32 逐条测试；文件读写、编码边界与原子发布在
// platform/windows/operation_history_store（那一侧只搬运 UTF-8 字节，不做任何判定）。
//
// 记录内容的边界（与 README 的用户说明一致）——只记「做过什么」的元数据，不记「内容本身」：
//   * 操作类型、时间、仓库工作区身份（绝对根 + CanonicalPathKey）、已确认的源/目标引用与
//     完整对象 ID、逐目标的发布核实结果、以及供人判断恢复方式的引用线索；
//   * 一律不记：完整文件内容、提交正文（标题/描述/合作者正文都不落这里）、凭据或含凭据的 URL
//     （发布地址一律先经 git::MaskPushUrlCredentialsInText 掩码）、进程环境、Git 的原始输出。
//   * 每个自由文本字段都过 SanitizeHistoryText 做「掩码 + 去控制字符 + 限长」，
//     导出走同一份序列化文本，因此导出天然与存储同样脱敏。
// 日志/状态文字只报元数据（键、条数、原因），绝不复述被排除的内容。

// 当前写入的版本号。读到更高的版本号时本版本必须拒绝读写（见 HistoryLoadStatus::tooNew）；
// 与 prefs 同一判据：本版本内出现未知记录名即视为损坏，不做「未知键宽容」。
inline constexpr int kHistoryFormatVersion = 1;

// 容量与保留：二者取先满足者淘汰最旧的记录，防止文件无限膨胀。
// 条数上限：一次编排操作最多产生一条 record + 若干 check/review 附属行。
inline constexpr size_t kMaxHistoryRecords = 1000;
// 保留天数：0 表示不按时间淘汰（只受条数约束）。超期的记录在每次合并/裁剪时整条带走。
inline constexpr int kDefaultHistoryRetentionDays = 90;
inline constexpr int kMaxHistoryRetentionDays = 3650;  // 上限十年，防止写入一个荒谬的保留期
// 单条附属证据（check / review）的上限：一条记录不该挂着无界的复核尾巴。
inline constexpr size_t kMaxHistoryChecksPerRecord = 16;
inline constexpr size_t kMaxHistoryReviewsPerRecord = 32;
// 每个自由文本字段的字符上限（UTF-16 码元）：超限即截断（截断前已掩码），
// 因为这里存的是元数据摘要而非用户原文，截断不丢「要恢复哪一条」这种关键事实。
inline constexpr size_t kMaxHistoryFieldChars = 512;
// 记录 ID 的长度上限（ASCII）：与命令窗口 operationId 同一量级的克制。
inline constexpr size_t kMaxHistoryIdLength = 64;

// 操作类型。六个被编排的写流程各有其恢复语义，冲突流程的「继续 / 中止」也分开记
// （中止会重写工作区与索引，与继续是两个方向）；首次推送与普通推送共享核实链，
// 但「写了上游配置」这件事只属于首次推送，因此单列。
enum class HistoryFlow {
  unknown = 0,
  commit,           // 创建提交
  undo,             // 撤回最近提交
  fetch,            // 抓取（只动远端跟踪引用 / FETCH_HEAD / 对象库）
  pull,             // 拉取（获取 + 整合，整合会改 HEAD / 索引 / 工作区）
  push,             // 推送（有上游，向发布目标送一条引用）
  firstPush,        // 首次推送 + 写上游配置
  conflictContinue, // 冲突流程「继续该流程」（会让 Git 造提交）
  conflictAbort,    // 冲突流程「中止该流程」（重写工作区与索引）
  restore,          // 按记录做的引用恢复（只移动一个本地分支引用；追加在末尾，旧文件不受影响）
};

[[nodiscard]] std::wstring_view HistoryFlowLabel(HistoryFlow flow) noexcept;

// 一条记录的终态。这里刻意把「结果未知 / 取消 / 启动失败 / Git 进程从未创建」
// 与「成功 / 失败」分开——AGENTS 的长期判据是这几种各有措辞、绝不合并。
//   * inProgress —— 已在命令窗口启动、但本程序尚未确认到终态就断了（进程被强杀、断电）。
//     读取端绝不能把 inProgress 当成「这台机器已经完成了那次操作」；它是「不确定发生了什么」，
//     恢复入口据此一律拒绝「照记录执行」，只做解释与重新核实。
//   * unknown —— 拿到了观察端的「结果未知」结论（窗口被提前关闭、超过观察期限）：
//     操作可能已经生效，也可能没有，无法判定。
enum class HistoryTerminal {
  inProgress = 0,  // 已启动、尚未结案（崩溃残留；不等于完成）
  succeeded,       // 命令按退出码策略成功
  failed,          // 命令按退出码策略失败
  canceled,        // 用户在确认框之前取消（没启动、没改动仓库）
  launchFailed,    // 命令窗口 / 执行器启动失败（没跑任何 Git）
  gitNotStarted,   // 辅助进程报告 Git 进程从未被创建
  unknown,         // 结果未知：可能生效也可能没有，无法判定
};

[[nodiscard]] std::wstring_view HistoryTerminalLabel(HistoryTerminal terminal) noexcept;

// 这条记录是否已经有「确定的那一版结论」：inProgress 还没定，其余（含 unknown）都已定。
// 界面在补记「结果未知」之前先问这一句，绝不把已经拿到的终态降级。
[[nodiscard]] bool TerminalHasSettled(HistoryTerminal terminal) noexcept;

// 恢复线索的类别——决定「恢复入口」对这个记录能走哪条路，也决定它绝不走哪条路。
//   * none —— 这次操作不产生「把某个本地引用挪回去」的可审查恢复形态：
//     fetch 不改 HEAD/分支/索引/工作区；pull 的整合与冲突流程会重写工作区/索引、涉及多提交重放，
//     「挪一个引用回去」并不能安全还原，因此只解释、给线索，不自动生成恢复命令。
//   * refMove —— 本地单引用的原子可逆（创建提交、撤回提交）：恢复形态就是一条带「预期旧值」的
//     git update-ref，把目标分支从「操作产生/当前应指的那个对象」挪回「操作前它指的那个对象」，
//     索引与工作区一个字节都不动。这与「撤回最近提交」同一条命令族，复用同一套原子保护。
//   * manualRemote —— 已推送到远端的历史（推送、首次推送）：本地能挪引用，但远端不受本地恢复影响。
//     恢复入口只解释协作影响、给出精确命令供用户自行复制执行，绝不自动 force push。
enum class HistoryRestoreKind {
  none = 0,     // 不产生可审查的引用级恢复
  refMove,      // 可用一条带预期旧值的 update-ref 恢复（仅本地引用）
  manualRemote, // 涉及远端：只解释与复制命令，不自动执行
};

[[nodiscard]] std::wstring_view HistoryRestoreKindLabel(HistoryRestoreKind kind) noexcept;

// 逐目标的发布核实结果（推送 / 首次推送才有；其它流程这个列表为空）。
// 每个布尔都是「明确答案」而不是「没查到就当没有」：queried=false 表示这个目标根本没问成
// （认证、权限、网络、或核实这一步没跑），与 queried=true 但 refPresent=false 是两回事。
struct HistoryTargetCheck {
  std::wstring maskedUrl;         // 核对对象（写入前已掩码，绝不含凭据）
  bool queried = false;           // 这个目标到底问过没有
  bool ok = false;                // 问成功了（ls-remote 退出码 0）
  bool refPresent = false;        // 那条引用在对端存在
  std::wstring remoteObjectId;    // refPresent 且 ok 时：它在对端停着的完整对象 ID
  std::wstring failure;           // 没问成时的具体原因（脱敏、限长）
};

// 后续复核追加的证据：一次「重新核实」不改写已落的终态，只往这条记录后面接一条带时刻的说明。
// 这是「未知结果必须保留为未知，后续复核只能追加新证据」这条要求在数据结构上的落点。
struct HistoryReview {
  long long epoch = 0;            // 复核发生的 UTC 秒；0 = 取不到时刻
  std::wstring text;              // 复核结论（脱敏、限长）
};

// 一条操作记录。字段分三段：身份与时间、确认的引用与对象 ID、终态与恢复线索。
struct OperationRecord {
  std::wstring id;                // 唯一 ID（ASCII，见 kMaxHistoryIdLength；本层不生成，只做合法性与去重）

  long long startedEpoch = 0;     // 启动时刻 UTC 秒；0 = 未知
  long long terminalEpoch = 0;    // 终态时刻 UTC 秒；0 = 尚未有终态（配合 terminal）

  HistoryFlow flow = HistoryFlow::unknown;
  std::wstring workTreeRoot;      // Git 答出的绝对工作区根（展示与重新识别的落点）
  std::wstring repositoryKey;     // CanonicalPathKey(workTreeRoot)：判定「同一份 worktree」的键
  std::wstring operationLabel;    // 操作展示名（如「撤回最近提交」），进标题与列表

  // 已确认的源/目标引用与完整对象 ID。全部取自用户在确认框上点头的那份方案，不是事后猜。
  std::wstring sourceRef;         // 例如本地分支 refs/heads/main，或撤回目标的分支引用
  std::wstring sourceObjectId;    // 该引用在确认那一刻的完整对象 ID（可为空——如「分支尚不存在」）
  std::wstring targetRef;         // 例如推送的远端引用 refs/heads/main
  std::wstring targetObjectId;    // 目标侧的完整对象 ID（如推送要送出去的那一份）
  std::wstring remoteName;        // 发布远端名（推送 / 首次推送）

  HistoryTerminal terminal = HistoryTerminal::inProgress;
  bool exitCodeKnown = false;     // 只有真的拿到 Git 退出码时才为 true（unknown/inProgress 常为 false）
  long long exitCode = 0;         // exitCodeKnown 时有效
  std::wstring completionLabel;   // git::CommandCompletionLabel 的文本（终态来源的可读标注）
  std::wstring outcomeNote;       // 一句话终态结论（脱敏、限长）

  std::vector<HistoryTargetCheck> targetChecks;  // 逐目标核实（推送类）
  std::vector<HistoryReview> reviews;            // 后续追加的复核证据

  // 引用级恢复线索（refMove 才有意义；其它 kind 也允许留 note，但不给自动恢复命令）。
  HistoryRestoreKind restoreKind = HistoryRestoreKind::none;
  std::wstring restoreBranchRef;          // 要挪回去的那条本地分支引用（refs/heads/…）
  std::wstring restoreUndoToObjectId;     // 恢复要把分支挪去的那个对象（操作前的值）；root 删除情形留空
  std::wstring restoreExpectedCurrentId;  // 恢复命令的原子「预期旧值」= 操作产生、当前应指的那个提交完整 ID
  bool restoreIsRoot = false;             // 恢复到「该分支本不该存在」：撤回首个提交、或提交前分支尚不存在
  std::wstring restoreNote;               // 面向人的一句话线索（脱敏、限长；manualRemote 用它说明协作影响）
};

// 整份操作日志（一次会话要写的全部 + 从盘上合并进来的历史）。
struct OperationLog {
  int formatVersion = kHistoryFormatVersion;
  // 「记录历史」的独立开关。默认关闭——这是可选新增功能，未经用户勾选前一个字节都不写；
  // 关闭时既不读也不写除本条策略以外的任何内容。它与 prefs 的「保存记录」开关各自独立。
  bool historyEnabled = false;
  int retentionDays = kDefaultHistoryRetentionDays;  // 0 = 不按时间淘汰
  size_t maxRecords = kMaxHistoryRecords;
  std::vector<OperationRecord> records;  // 时间顺序：旧 → 新
};

// ---- 取值形态判定与写入（纯函数，全部可测）----

// 一个记录 ID 是否是本程序会写出的形态：纯 ASCII 字母数字/下划线/连字符，长度 1..上限。
// 与命令窗口的 operationId 校验同一取向：来历不明的字符串绝不进记录。
[[nodiscard]] bool IsSafeHistoryId(std::wstring_view id);

// 自由文本脱敏：先掩码任何 scheme://…@… 形态的凭据 URL，再剔除控制字符（含制表、换行），
// 最后按 kMaxHistoryFieldChars 截断。落盘与展示前都要过这一道。
[[nodiscard]] std::wstring SanitizeHistoryText(std::wstring_view text);


// 记录一条操作：先校验 ID 与仓库键形态，再逐字段脱敏与限长，然后
//   * 同 ID 已存在 —— 用「更确定的终态」升级它（inProgress 可被任何终态取代；
//     已是确定终态的记录不被更「弱」的信息覆盖），复核证据 append 进原记录，
//     这是「崩溃后重启、同一操作再被观察到终态」与「重新核实追加」两条路径的共同落点。
//   * 没有同 ID —— 追加到末尾，维持时间顺序。
// 超 maxRecords 时从最旧开始丢。返回 false = 记录不成立（ID 非法等），*refusal 给原因，内存不改。
bool AppendHistoryRecord(OperationLog* log, OperationRecord record, std::wstring* refusal);

// 在一条已存在的记录上追加一次复核证据（不改终态、不改确认信息）。找不到该 ID 返回 false。
// 复核尾巴超 kMaxHistoryReviewsPerRecord 时丢最早的（保留最近证据）。
bool AppendHistoryReview(OperationLog* log, std::wstring_view id, long long epoch,
                         std::wstring_view text);

// 按保留期限 + 条数上限裁剪（nowEpoch 由调用方从系统时钟取，纯逻辑不碰时钟）。
// retentionDays<=0 时不按时间淘汰。整条记录连同它的 check/review 一起走（按 startedEpoch 判新旧）。
void ApplyHistoryRetention(OperationLog* log, long long nowEpoch);

// 给界面显示用的一句话存储位置：<目录>\history.prefs（不含正文，只有元数据）。
[[nodiscard]] std::wstring DescribeHistoryStoragePath(std::wstring_view directory,
                                                     std::wstring_view fileName);

// ---- 版本化文本格式 ----
//
// 每行一条记录：名字与字段以 '|' 分隔；字段内的 '\'、'|'、换行、回车一律转义，
// 出现 NUL 即整份拒绝。首行固定为 `evernightcommit.history|<版本>`；没有首行的文件
// 无法识别为本程序的历史，按损坏处理（历史没有「未分版本的老文件」这种迁移来源）。
//
//   meta|<enabled0/1>,<retentionDays>,<maxRecords>
//   record|<id>|<startedEpoch>,<terminalEpoch>|<flow>,<terminal>,<exitKnown0/1>,<exitCode>|
//          <root>|<key>|<label>|<srcRef>|<srcOid>|<tgtRef>|<tgtOid>|<remote>|
//          <completion>|<outcomeNote>|<restoreKind>,<restoreIsRoot0/1>|
//          <restoreBranch>|<restoreUndoTo>|<restoreExpectedCurrent>|<restoreNote>
//   check |<id>|<maskedUrl>|<queried0/1>,<ok0/1>,<refPresent0/1>|<remoteOid>|<failure>
//   review|<id>|<epoch>|<text>
//
// record 是主干，check / review 用 id 关联到它；任何一条不成立、或 check/review 指向
// 没有主记录的 id，都是*整份*不成立（HistoryLoadStatus::corrupt）——调用方按「损坏恢复」处理，
// 绝不拿半份历史去覆盖、也绝不把「半条」当成一次已完成的操作。同 ID 出现两次同样是损坏。

[[nodiscard]] std::wstring SerializeOperationLog(const OperationLog& log);

enum class HistoryLoadStatus {
  empty,    // 没有内容可读（文件不存在或全空白）：按默认（关闭、空历史）开始
  loaded,   // 当前版本，完整读回
  corrupt,  // 语义残缺：调用方必须备份原件后重新开始，绝不覆盖着写
  tooNew,   // 版本比本程序高：不应用、不覆盖，等用户处理
};

struct HistoryLoadResult {
  HistoryLoadStatus status = HistoryLoadStatus::empty;
  OperationLog log;
  std::wstring reason;           // 面向用户的具体说明（不含正文）
  long long detectedVersion = 0; // 文件自报的版本（tooNew 的措辞要用它；按解析上界收下，不截断）
};

[[nodiscard]] HistoryLoadResult ParseOperationLog(std::wstring_view text);

// ---- 多实例合并 ----
//
// 「另一个窗口正往同一份历史里追加」是常态。写入前重新读盘，然后按这一份规则合并：
//   * 开关与容量/保留策略（historyEnabled / retentionDays / maxRecords）：本窗口的选择优先——
//     那是用户刚刚点的；
//   * 记录：按 ID 并集。同 ID 上，本窗口这条的终态比盘上那条「更确定」时以本窗口为准，
//     否则保留盘上的（另一窗口先看到了终态，不能被这里的一次 inProgress 覆盖）；
//     复核证据两边并起来按 epoch 去重后保留；
//   * 「清除历史」（replaceAll=true）不做记录合并：本窗口说「全部删掉」就是全部删掉，
//     只沿用盘上的策略字段继续可比。
// 合并后统一套用容量 + 保留裁剪。
struct HistoryWriteIntents {
  bool policy = false;      // 本窗口改过开关 / 保留 / 容量
  bool append = false;      // 本窗口这一会话产生了新记录或新复核
  bool replaceAll = false;  // 「清除历史」的一次性形态
};

struct HistoryMergeReport {
  // 盘上已有一个更确定的终态，本窗口这条较「弱」的因此没有落下去的记录 ID。
  // 「重新核实追加」与「崩溃重启又观察到 inProgress」都会走到这里，界面据此说明
  // 「这条历史以先记录到终态的那份为准」。
  std::vector<std::wstring> keptFromDiskStrongerTerminal;
};

// 把本窗口这一份与盘上那一份合并成要写出去的历史；nowEpoch 由调用方从系统时钟取
// （用于合并后的保留期限裁剪），纯逻辑不碰时钟。合并后已套用容量 + 保留裁剪。
[[nodiscard]] OperationLog MergeHistoryForWrite(const OperationLog& disk, const OperationLog& ours,
                                                 const HistoryWriteIntents& intents, long long nowEpoch,
                                                 HistoryMergeReport* report);

// ---- 从「已确认的方案 + 命令窗口终态」装配一条记录 ----
//
// 界面各操作在「用户已确认、启动命令窗口」的那一刻手上还握着那份结构化方案（分支引用、
// 完整对象 ID、远端与发布 URL、恢复线索），但方案在命令窗口跑完前就被控制器释放了；
// 因此把这些「已确认的事实」在启动点拷成一份 HistoryCapture 随操作带走，终态时与命令结果
// 装配成一条记录。装配是纯函数：URL 一律在这里掩码、自由文本一律在这里脱敏与限长，
// 界面不需要、也不应该自己再脱敏一遍（两处各脱敏就会漏）。

// 命令窗口终态的分类（界面从 OperationOutcome + CommandCompletion 映射进来，各自分立）。
enum class HistoryOutcome {
  inProgress,     // 已启动、还没拿到任何结果（界面关掉或进程被强杀时历史里就停在这一档）
  succeeded,      // finished + 退出码策略判成功
  failed,         // finished + 退出码策略判失败
  unknown,        // 窗口提前关闭 / 超过观察期限：结果未知
  launchFailed,   // 命令窗口 / 执行器启动失败
  gitNotStarted,  // 辅助进程报告 Git 进程从未被创建
};

// 界面启动一次写操作时拷带走的「已确认事实」。record=false 表示这一类操作不落历史
// （查看类、status、被用户取消的操作）。
struct HistoryCapture {
  bool record = false;
  HistoryFlow flow = HistoryFlow::unknown;
  std::wstring operationLabel;    // displayName，如「撤回最近提交」
  std::wstring workTreeRoot;      // 这次操作实际绑定的工作区根（装配记录时的仓库身份来源）

  std::wstring sourceRef;         // 本地分支引用（refs/heads/…）
  std::wstring sourceObjectId;    // 确认那一刻的完整对象 ID（可为空：分支尚不存在）
  std::wstring targetRef;         // 远端引用（推送类）
  std::wstring targetObjectId;    // 要送出去的那一份（推送类）
  std::wstring remoteName;        // 发布远端名（推送类）
  std::vector<std::wstring> publishUrls;  // 发布 URL（装配时逐条掩码）

  HistoryRestoreKind restoreKind = HistoryRestoreKind::none;
  std::wstring restoreBranchRef;
  std::wstring restoreUndoToObjectId;
  std::wstring restoreExpectedCurrentId;
  bool restoreIsRoot = false;
  std::wstring restoreNote;       // 文字线索 / 协作影响说明（装配时脱敏）
};

// 命令窗口的终态信息（时刻由界面从系统时钟取，纯逻辑不碰时钟）。
struct HistoryTerminalInfo {
  HistoryOutcome outcome = HistoryOutcome::unknown;
  long long startedEpoch = 0;
  long long terminalEpoch = 0;
  bool exitCodeKnown = false;
  long long exitCode = 0;
  std::wstring completionLabel;   // git::CommandCompletionLabel 的文本
  std::wstring conclusion;        // 面向界面的结论文本（装配时脱敏）
};

// 把一份 capture + 一份终态装配成一条记录。装配点：终态已定，terminal 由 outcome 映射；
// 「结果未知」映射成 unknown 而非成功/失败。URL 掩码、文本脱敏、对象 ID 校验都在这一层。
// 记录不合法（ID/键/对象 ID 不成立）时返回的 record 会带 blocked 语义：AppendHistoryRecord
// 启动一次「要落历史」的写操作之前，在公共边界上核对这份捕获里结构性必需的字段。
// 返回空串 = 可以启动（或这次压根不落历史，无需判定）。
//
// 为什么要这一道：历史上出过「控制器把 record 置了 true 却漏填仓库工作区根」，结果
// AppendHistoryRecord 因为规范键为空而拒绝整条记录——操作真的执行了，历史里却一个字都没有，
// 用户回头找不到任何线索。漏字段是接线缺陷，就该在接线处当场报出来，而不是让它在写盘时
// 悄悄失败（那会被读成「这次没做过任何操作」）。
//   * 历史开关没开：什么都不会写，因此绝不因为字段缺失拦住用户的操作；
//   * record=false：这次操作本来就不落历史，同样放行；
//   * record=true 而归属信息缺失：拒绝启动，并把话说清是哪一项。
[[nodiscard]] std::wstring DescribeHistoryCaptureRefusal(const HistoryCapture& capture,
                                                        bool historyEnabled);

// 会拒收并给原因，调用方据状态栏处理，绝不落一条坏记录。
[[nodiscard]] OperationRecord ComposeHistoryRecord(std::wstring id, std::wstring workTreeRoot,
                                                   const HistoryCapture& capture,
                                                   const HistoryTerminalInfo& terminal);

}  // namespace gc::app
