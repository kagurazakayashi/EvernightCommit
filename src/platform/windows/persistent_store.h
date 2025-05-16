#pragma once

#include <string>
#include <string_view>

#include "app/persistent_state.h"

namespace gc::platform {

// 用户应用数据目录与状态文件的落点、限长读取、独占写锁与原子发布（Windows 层）。
//
// 数据目录取当前用户的漫游应用数据目录（%APPDATA%）下的 EvernightCommit：
// 那是 Windows 给「这个用户的这个应用」的稳定位置，与项目工作区、Git 配置、
// 开发进度文件都无关；取不到时明确失败，绝不悄悄退回到程序目录或工作区。
// 测试必须传入自己的受控临时目录（baseDirectoryOverride），不得读写真实用户配置。

inline constexpr std::wstring_view kPersistentDirectoryName = L"EvernightCommit";
inline constexpr std::wstring_view kPersistentStateFileName = L"state.prefs";
inline constexpr std::wstring_view kPersistentLockFileName = L"state.prefs.lock";
// 状态文件的大小上限：超限的文件不做「读一半」的判读，整份按损坏处理。
inline constexpr unsigned long long kPersistentFileMaxBytes = 4ull * 1024 * 1024;

struct PersistentStorePaths {
  bool valid = false;
  std::wstring directory;      // <APPDATA>\EvernightCommit
  std::wstring stateFile;      // <directory>\state.prefs
  std::wstring lockFile;       // <directory>\state.prefs.lock
  std::wstring failureReason;  // valid 为 false 时的具体原因（面向界面，不含内容）
};

// baseDirectoryOverride 为空时按当前用户的 %APPDATA% 解析；测试传入自己的临时目录根。
// 本函数只解析与校验（基目录存在、不是再分析点），不创建任何东西。
[[nodiscard]] PersistentStorePaths ResolvePersistentStorePaths(std::wstring_view baseDirectoryOverride = {});

enum class PersistentReadKind {
  absent,        // 文件不存在：首次使用的正常形态
  loaded,        // 完整读回且是合法 UTF-8（内容成立与否交给 ParsePersistentState 判）
  oversized,     // 超过大小上限：整份不可信
  unreadable,    // 打开/读取失败（权限、被占用……），原因在 detail
  invalidUtf8,   // 字节不是合法 UTF-8：整份不可信
};

struct PersistentReadResult {
  PersistentReadKind kind = PersistentReadKind::absent;
  std::wstring text;     // loaded 时才有内容；其余一律为空
  std::wstring detail;   // 非 loaded/absent 时的具体原因（只报元数据）
};

[[nodiscard]] PersistentReadResult ReadPersistentStateFile(const std::wstring& stateFile);

enum class PersistentSaveStatus {
  saved,             // 已合并并原子发布
  recoveredCorrupt,  // 读到的文件不成立：原件已改名保留后重新写出
  busy,              // 另一个实例正持锁写入：本次没有写任何东西，内存内容原样留着
  refusedTooNew,     // 盘上文件由更高版本写出：不读不写
  failed,            // 目录、编码、写入、发布任一环节失败；原因在 detail
};

struct PersistentSaveOutcome {
  PersistentSaveStatus status = PersistentSaveStatus::failed;
  app::PersistentState merged;                    // saved/recoveredCorrupt 时为实际落盘的那份（调用方以此更新基线）
  app::PersistentMergeReport report;              // 草稿合并中「盘上更新所以保留盘上」的仓库列表
  std::wstring detail;                       // 面向界面的具体说明（只含元数据与原因）
};

// 一次「拿锁 → 重读 → 合并 → 原子写 → 放锁」的完整保存。nowEpoch 由调用方从系统时钟取
// （纯逻辑不碰时钟，时钟注入点是这一层）。任何一步失败都不会留下半个文件，
// 也不会静默覆盖另一个实例的新草稿。
[[nodiscard]] PersistentSaveOutcome SavePersistentState(const PersistentStorePaths& paths,
                                                        const app::PersistentState& ours,
                                                        const app::PersistentWriteIntents& intents,
                                                        long long nowEpoch);

// 「清除已存记录」里删掉数据文件的动作（策略记录仍会随后被保存，因此删除的语义由
// intents.replaceAll 的保存完成；本函数只负责把状态文件从盘上带走）。
[[nodiscard]] bool DeletePersistentStateFile(const PersistentStorePaths& paths, std::wstring* reason);

// 恢复窗口位置前的可见性验证：给出的矩形（物理像素，GetWindowRect 形态）必须与某台
// 当前接入的显示器有实际交集，否则不采用——显示器拔掉、DPI 变了、屏幕挪了，
// 拿上次的坐标恢复会把窗口扔到看不见的位置，那比回退到默认位置更糟。
[[nodiscard]] bool IsWindowRectReachable(int x, int y, int width, int height);

}  // namespace gc::platform
