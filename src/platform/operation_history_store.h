#pragma once

#include <string>
#include <string_view>

#include "app/operation_history.h"

namespace gc::platform {

// 操作历史文件的落点、限长读取、独占写锁与原子发布（平台契约）。
//
// 数据语义全在 app/operation_history；本层只搬 UTF-8 字节，不做「一条记录算不算完成」
// 这类判定。Windows 实现落在当前用户漫游应用数据目录（%APPDATA%\EvernightCommit），
// 复用持久化偏好同一套「拿独占句柄写锁 → 重读 → 合并 → 临时文件全量写 → flush →
// 原子改名发布」与「损坏原件改名保留、更高版本拒读写」的判据；Linux 在步骤 11
// 接入跨会话保存之前**明确报告不可用**——界面据此只保留本会话内存记录，
// 绝不假装已经落盘。
//
// 数据目录一律来自当前用户的漫游应用数据目录（经由持久化偏好的同一处解析），
// 取不到时明确失败，绝不退到项目目录、Git 配置或任何别处。测试必须传入自己的
// 受控临时目录（baseDirectoryOverride），不得读写真实用户配置。

inline constexpr std::wstring_view kHistoryFileName = L"history.prefs";
inline constexpr std::wstring_view kHistoryLockFileName = L"history.prefs.lock";
// 历史文件的大小上限：超限的文件不做「读一半」的判读，整份按损坏处理。
// 历史会随时间累积条目，比偏好文件给更大的额度。
inline constexpr unsigned long long kHistoryFileMaxBytes = 16ull * 1024 * 1024;

// 本平台有没有「跨会话保存」这项能力，以及这一次运行到底能不能读写。
//   * crossSessionPersistence —— 平台实现是否已经具备落盘能力（Linux 在步骤 11 前为 false）；
//   * usable —— 这一次运行真的能读写（目录解析失败、盘上版本过高时为 false）；
//   * location —— 可展示的位置（不可用时可以为空）；
//   * reason —— 不可用或本平台尚不支持时的完整说明，界面必须原样显示给用户。
// 即使 crossSessionPersistence 为 false，界面仍可以在内存里记录并展示本会话的操作，
// 但必须同时说明「关闭界面即丢失」，不得写成「已保存」。
struct HistoryStorageAvailability {
  bool crossSessionPersistence = false;
  bool usable = false;
  std::wstring location;
  std::wstring reason;
};

[[nodiscard]] HistoryStorageAvailability DescribeHistoryStorageAvailability();

struct HistoryStorePaths {
  bool valid = false;
  std::wstring directory;      // <APPDATA>\EvernightCommit（与偏好同目录）
  std::wstring historyFile;    // <directory>\history.prefs
  std::wstring lockFile;       // <directory>\history.prefs.lock
  std::wstring failureReason;  // valid 为 false 时的具体原因（面向界面，不含内容）
};

// baseDirectoryOverride 为空时按当前用户的 %APPDATA% 解析（经由持久化偏好的同一处目录解析）；
// 测试传入自己的临时目录根。本函数只解析与校验，不创建任何东西。
// 不支持跨会话保存的平台返回 valid=false，并在 failureReason 里说清原因。
[[nodiscard]] HistoryStorePaths ResolveHistoryStorePaths(std::wstring_view baseDirectoryOverride = {});

enum class HistoryReadKind {
  absent,        // 文件不存在：首次使用的正常形态
  loaded,        // 完整读回且是合法 UTF-8（内容成立与否交给 ParseOperationLog 判）
  oversized,     // 超过大小上限：整份不可信
  unreadable,    // 打开/读取失败（权限、被占用……），原因在 detail
  invalidUtf8,   // 字节不是合法 UTF-8：整份不可信
};

struct HistoryReadResult {
  HistoryReadKind kind = HistoryReadKind::absent;
  std::wstring text;     // loaded 时才有内容；其余一律为空
  std::wstring detail;   // 非 loaded/absent 时的具体原因（只报元数据）
};

[[nodiscard]] HistoryReadResult ReadHistoryFile(const std::wstring& historyFile);

enum class HistorySaveStatus {
  saved,             // 已合并并原子发布
  recoveredCorrupt,  // 读到的文件不成立：原件已改名保留后重新写出
  busy,              // 另一个实例正持锁写入：本次没有写任何东西，内存内容原样留着
  refusedTooNew,     // 盘上文件由更高版本写出：不读不写
  failed,            // 目录、编码、写入、发布任一环节失败；原因在 detail
};

struct HistorySaveOutcome {
  HistorySaveStatus status = HistorySaveStatus::failed;
  app::OperationLog merged;                    // saved/recoveredCorrupt 时为实际落盘的那份（调用方以此更新基线）
  app::HistoryMergeReport report;              // 合并中「盘上终态更强所以保留盘上」的记录 ID 列表
  std::wstring detail;                         // 面向界面的具体说明（只含元数据与原因）
};

// 一次「拿锁 → 重读 → 合并 → 原子写 → 放锁」的完整保存。nowEpoch 由调用方从系统时钟取。
// 任何一步失败都不会留下半个文件，也不会把「半条记录」当成一次已完成的操作。
[[nodiscard]] HistorySaveOutcome SaveOperationHistory(const HistoryStorePaths& paths,
                                                     const app::OperationLog& ours,
                                                     const app::HistoryWriteIntents& intents,
                                                     long long nowEpoch);

// 「清除操作历史」里删掉数据文件的动作（随后的策略保存会重新写出空历史；本函数只负责把
// 文件从盘上带走，删除幂等）。
[[nodiscard]] bool DeleteHistoryFile(const HistoryStorePaths& paths, std::wstring* reason);

// 把当前历史脱敏导出到同目录下一个新文件（history.export-<epoch>.txt）并返回其路径。
// 导出走与存储同一份序列化文本，落账时已逐字段掩码与限长，因此导出天然同样脱敏——
// 不含完整文件内容、提交正文、凭据 URL、令牌、环境或原始输出。导出是一次性快照，
// 不参与写锁/合并（它不是那条会被反复读写的状态文件）。
[[nodiscard]] bool ExportOperationHistoryFile(const HistoryStorePaths& paths,
                                             const app::OperationLog& log, long long nowEpoch,
                                             std::wstring* outPath, std::wstring* reason);

}  // namespace gc::platform
