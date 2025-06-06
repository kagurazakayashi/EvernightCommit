#include "platform/operation_history_store.h"

#include <string>

// Linux 的操作历史持久化按计划在后续步骤（步骤 11）接入 XDG 数据目录与同款的
// 独占锁/原子发布；在那之前这一层**如实拒绝**所有落盘动作。界面仍然可以在内存里
// 记录并展示本会话的操作，但必须同时说明「关闭界面即丢失」——不能把内存记录说成
// 已经保存，也不能退到项目目录或任何别的位置去写。

namespace gc::platform {
namespace {

// 所有接口共用的同一句解释：换个说法会让界面在不同入口给出不一致的承诺。
constexpr std::wstring_view kNoPersistenceReason =
    L"Linux 的操作历史跨会话保存尚未接入（计划在后续步骤完成）。"
    L"本次运行只在内存里记录并展示操作历史，关闭界面即丢失；"
    L"本程序不会把这份记录写到项目目录、仓库或任何别的位置。";

}  // namespace

HistoryStorageAvailability DescribeHistoryStorageAvailability() {
  HistoryStorageAvailability availability;
  availability.crossSessionPersistence = false;
  availability.usable = false;
  availability.reason = std::wstring(kNoPersistenceReason);
  return availability;
}

HistoryStorePaths ResolveHistoryStorePaths(std::wstring_view baseDirectoryOverride) {
  static_cast<void>(baseDirectoryOverride);
  HistoryStorePaths paths;
  paths.failureReason = std::wstring(kNoPersistenceReason);
  return paths;
}

HistoryReadResult ReadHistoryFile(const std::wstring& historyFile) {
  static_cast<void>(historyFile);
  HistoryReadResult result;
  // 没有落盘位置就没有文件可读；按「不存在」回答，绝不编造一份空历史冒充读成功。
  result.kind = HistoryReadKind::absent;
  return result;
}

HistorySaveOutcome SaveOperationHistory(const HistoryStorePaths& paths,
                                        const app::OperationLog& ours,
                                        const app::HistoryWriteIntents& intents,
                                        long long nowEpoch) {
  static_cast<void>(paths);
  static_cast<void>(intents);
  static_cast<void>(nowEpoch);
  HistorySaveOutcome outcome;
  outcome.status = HistorySaveStatus::failed;
  outcome.merged = ours;  // 内存基线保持调用方给出的那一份，不丢本会话记录。
  outcome.detail = std::wstring(kNoPersistenceReason);
  return outcome;
}

bool DeleteHistoryFile(const HistoryStorePaths& paths, std::wstring* reason) {
  static_cast<void>(paths);
  if (reason != nullptr) {
    *reason = std::wstring(kNoPersistenceReason);
  }
  return false;
}

bool ExportOperationHistoryFile(const HistoryStorePaths& paths, const app::OperationLog& log,
                                long long nowEpoch, std::wstring* outPath, std::wstring* reason) {
  static_cast<void>(paths);
  static_cast<void>(log);
  static_cast<void>(nowEpoch);
  if (outPath != nullptr) {
    outPath->clear();
  }
  if (reason != nullptr) {
    *reason = std::wstring(kNoPersistenceReason);
  }
  return false;
}

}  // namespace gc::platform
