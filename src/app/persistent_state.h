#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_date.h"
#include "git/commit_message.h"

namespace gc::app {

// 「保存最近仓库 / 选定 Git / 窗口布局 / 提交草稿」的数据模型与版本化文本格式（纯逻辑层）。
//
// 为什么放在 app 层而不是平台层：这份数据的*语义*——哪些记录互相覆盖、谁的草稿更新、
// 旧版本怎么迁移、超限怎么处理——都必须能脱离文件系统逐条测试；文件读写、编码边界与
// 原子发布在 platform/windows/persistent_store（那一侧只搬运 UTF-8 字节，不做任何判定）。
//
// 保存内容的边界（与 README 的用户说明一致）：
//   * 最近仓库的工作区根目录（绝对路径）、选定并验证过的 git.exe 路径；
//   * 窗口位置尺寸、是否最大化，以及两条分隔条的列宽比例；
//   * 每个「规范化工作区身份」（CanonicalPathKey）一份未提交的提交表单草稿。
// 一律不保存：口令、令牌、凭据内嵌 URL、完整进程环境。草稿正文是用户自己写的文字，
// 可能存在私人内容，因此存储位置要能在界面上问出来（DescribeStoragePaths），删除走
// 「清除已存记录」或直接删除那个文件。日志/状态文字只报元数据（键、条数、原因），
// 绝不复述草稿正文。

// 当前写入的版本号。读到更高的版本号时本版本必须拒绝读写（见 LoadStatus::tooNew），
// 读到更低的已知版本走 migrate 路径；新增字段一律升版本，不做「未知键宽容」——
// 与本项目「残缺的输出不是事实」的一贯判据一致：v1 内出现未知记录即视为损坏。
inline constexpr int kPersistentFormatVersion = 1;

// 记录条数上限：超限按「最旧的一条被丢弃」处理，防止文件无限膨胀。
inline constexpr size_t kMaxRecentRepositories = 10;
inline constexpr size_t kMaxPersistedDrafts = 30;
// 单份草稿的正文字符上限（UTF-16 码元计数：标题+描述+作者+全部合作者）。
// 超限是*拒绝保存*而不是截断保存：悄悄剪掉用户写的字比不存更糟。
inline constexpr size_t kMaxDraftContentChars = 32768;
// 列宽比例以千分比存储（避免文本层出现小数点与区域化写法）。
inline constexpr int kPersistentColumnPermilleBase = 1000;

// 窗口位置与尺寸：屏幕物理像素（GetWindowRect 形态）。恢复时的可见性验证在平台层做，
// 这里只管取值形态；maximized 记录的是关闭时窗口是否处于最大化。
struct PersistentWindowGeometry {
  bool valid = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool maximized = false;
};

// 「未暂存 / 已暂存」两条分隔条的列宽比例，存千分比整数（0..1000）。
// 是否真的能落地由 ui/layout 的 RatioFromMouseX 规则复核，这里只保证取值不越界。
struct PersistentColumns {
  bool valid = false;
  int leftPermille = 340;
  int middlePermille = 340;
};

// 一份按仓库身份保存的提交表单草稿（与子模块导航的 HeldForm 同一类内容，
// 但那是进程内的临时代管，这一份要跨进程、跨启动活着）。
struct PersistentDraft {
  std::wstring repositoryRoot;      // Git 答出的绝对工作区根（展示与重新识别的落点）
  std::wstring repositoryKey;       // CanonicalPathKey(repositoryRoot)：判定「同一份 worktree」的键
  git::CommitFormData form;         // 标题 / 描述 / 作者 / 合作者
  git::CivilTime authorWall{};
  git::CivilTime committerWall{};
  bool timesSynced = true;
  // 「这两个时间是用户亲手挑的」记号。恢复时没有这个记号就必须回到此刻，
  // 否则上一次的启动时间会被当成这次提交的时间意图。
  bool timesUserEdited = false;
  long long savedAtEpoch = 0;       // UTC 秒；0 = 取不到时刻（合并时按「不更新」处理）
};

// 一次会话要写的整份状态。
struct PersistentState {
  int formatVersion = kPersistentFormatVersion;
  long long revision = 0;           // 每次成功写出 +1（诊断与合并参考；草稿新旧以 savedAtEpoch 为准）
  // 「首次说明已经展示过、用户做过选择」。为 false 时程序不写任何记录，下次启动重新问。
  bool consentRecorded = false;
  // 总开关。为 false 时程序既不读也不写除本条策略以外的任何内容。
  bool persistenceEnabled = true;
  // 草稿单独开关：为 false 时不保存、不恢复草稿（其余记录照常）。
  bool draftSavingEnabled = true;
  std::wstring gitExecutablePath;              // 验证通过后才写入
  std::vector<std::wstring> recentRepositories; // MRU，最近优先
  PersistentWindowGeometry window;
  PersistentColumns columns;
  std::vector<PersistentDraft> drafts;
};

// ---- 取值形态判定（纯函数，全部可测）----

[[nodiscard]] bool PersistentDraftBodyIsEmpty(const PersistentDraft& draft) noexcept;

// 草稿正文是否超出上限。超限的草稿不保存、也不截断。
[[nodiscard]] bool PersistentDraftContentTooLarge(const git::CommitFormData& form) noexcept;

// 按当前屏幕上的表单给「这个仓库」记一份草稿：正文为空即删除该仓库的记录
// （作者栏不算正文——那是跨提交复用的身份，不该把一张只剩作者的表单独成草稿）。
// 超限时不改内存里已有的那份草稿，返回拒绝原因。
bool UpsertPersistentDraft(PersistentState* state, PersistentDraft draft, std::wstring* refusal);

// 会话内的记录入口（与 Upsert 的区别只在空正文的处理）：正文为空时*保留一条空草稿*作为
// 删除墓碑——多实例合并要靠它的 savedAtEpoch 与盘上的记录比新旧；直接从内存里抹掉这个键的话，
// 「这个窗口清空了表单」就传不到文件里，下一次合并反而会把旧草稿复活。
// 空草稿永远不会被序列化落盘（MergeForWrite 合并完统一剔除）。
bool RecordPersistentDraftSnapshot(PersistentState* state, PersistentDraft draft, std::wstring* refusal);

bool RemovePersistentDraft(PersistentState* state, std::wstring_view repositoryKey);
[[nodiscard]] const PersistentDraft* FindPersistentDraft(const PersistentState& state,
                                                         std::wstring_view repositoryKey);

// 最近仓库：同一键的旧条目被挪到最前（保留显示用的路径写法），超出上限丢最旧。
void TouchPersistentRecent(PersistentState* state, std::wstring repositoryRoot);

// 「清除已存记录」：保留策略三态（asked/enabled/drafts），删掉全部用户内容记录，
// 并让版本号与修订号继续可比。
void ClearPersistedUserData(PersistentState* state);

// ---- 版本化文本格式 ----
//
// 每行一条记录：名字与字段以 '|' 分隔；字段内的 '\'、'|'、换行、回车一律转义，
// 出现 NUL 即整份拒绝（宽字符串里本不该有它，出现说明内存里的数据已经不是我们写的那份）。
// 首行固定为 `evernightcommit.prefs|<版本>`；没有首行的老文件按版本 0 迁移：
// 只认得下面这些记录名，认不出的整份按损坏处理。
//
//   persistence|<asked>,<enabled>,<drafts>     策略三态（0/1）
//   revision|<非负整数>
//   git|<转义后的绝对路径>
//   recent|<转义后的绝对路径>                   可多条，顺序即 MRU 顺序
//   window|<x>,<y>,<w>,<h>,<max0/1>
//   columns|<左千分比>,<中千分比>
//   draft|<root>|<key>|<epoch>|<sync>,<edited>|<作者墙钟 y,m,d,H,M,S>|<提交者墙钟>|<标题>|<描述>|<作者>|<合作者条数>|<合作者>…
//
// 墙钟六个整数由 git::CivilTimeLooksValid 裁定；草稿键必须等于 CanonicalPathKey(root)
// （折叠规则变了就是数据变了，宁可拒绝也不猜）。任何一条记录不成立都是*整份*不成立
// （LoadStatus::corrupt），调用方按「损坏恢复」处理，绝不拿半份快照去覆盖。

[[nodiscard]] std::wstring SerializePersistentState(const PersistentState& state);

enum class PersistentLoadStatus {
  empty,    // 没有内容可读（文件不存在或全空白）：按默认状态开始
  loaded,   // 当前版本，完整读回
  migrated, // 老版本文件，已迁移到当前版本（调用方应尽快写回带版本号的形态）
  corrupt,  // 语义残缺：调用方必须备份原件后重新开始，绝不覆盖着写
  tooNew,   // 版本比本程序高：不应用、不覆盖，等用户处理
};

struct PersistentLoadResult {
  PersistentLoadStatus status = PersistentLoadStatus::empty;
  PersistentState state;
  std::wstring reason;      // 面向用户的具体说明（不含正文）
  int detectedVersion = 0;  // 文件自报的版本（migrated/tooNew 的措辞要用它）
};

[[nodiscard]] PersistentLoadResult ParsePersistentState(std::wstring_view text);

// ---- 多实例合并 ----
//
// 「另一个窗口正开着同一个文件」是常态而不是意外。写入前重新读盘，然后按这一份规则合并：
//   * 策略三态（consent/enabled/drafts）：本窗口的选择优先——那是用户刚刚点的开关；
//   * git / window / columns：本窗口*这一会话里改过*（intents 点名）才覆盖；
//     盘上本来就是空的按「填洞」处理（没有别人的数据可被踩踏）；其余沿用盘上的
//     （另一个窗口拖动过分隔条，不能被这里的一次无关保存抹掉）；
//   * 最近仓库：本窗口的顺序优先，盘上独有的条目按盘上顺序续在后面；
//   * 草稿：同一键上，盘上的 savedAtEpoch 严格更新时保留盘上的并明确报告——
//     这就是「最后一个写入者不得悄悄覆盖另一个窗口的新草稿」那一条；
//     本窗口清空正文记为带新时刻的空草稿，合并后统一剔除，因此「清空表单」是一次
//     会传播的删除，而不是「这条记录没了」的复活。
// replaceAll=true 的清除动作不做草稿合并：本窗口说「全部删掉」就是全部删掉。

struct PersistentWriteIntents {
  bool gitPath = false;
  bool windowGeometry = false;
  bool columns = false;
  bool recentList = false;
  bool drafts = false;
  bool policy = false;
  // 「清除已存记录」的一次性形态：除策略三态外整份以本窗口为准。
  bool replaceAll = false;
};

struct PersistentMergeReport {
  // 盘上草稿更新而被保留下来的仓库根（本窗口的屏幕内容因此没有落到盘上，必须说清）。
  std::vector<std::wstring> newerDraftsKeptFromDisk;
};

PersistentState MergeForWrite(const PersistentState& disk, const PersistentState& ours,
                              const PersistentWriteIntents& intents, PersistentMergeReport* report);

// 给界面显示用的存储位置一句话：<目录>\state.prefs（不含正文，只有元数据）。
[[nodiscard]] std::wstring DescribeStoragePath(std::wstring_view directory, std::wstring_view fileName);

}  // namespace gc::app
