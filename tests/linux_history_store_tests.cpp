// Linux 操作历史存储契约：步骤 11 之前的实现必须**如实报告**不具备跨会话保存，
// 不读任何文件、不写任何文件，也不把调用方的内存记录丢掉。
#include <string>

#include "app/operation_history.h"
#include "platform/operation_history_store.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::test::PosixTempDirectory;
using gc::test::RequireReadFile;
using gc::test::RequireWriteFile;

}  // namespace

GC_TEST(linux_history_store_reports_no_cross_session_persistence) {
  const gc::platform::HistoryStorageAvailability availability =
      gc::platform::DescribeHistoryStorageAvailability();
  GC_CHECK_MESSAGE(!availability.crossSessionPersistence,
                   "步骤 11 之前 Linux 不得声称具备跨会话保存");
  GC_CHECK(!availability.usable);
  GC_CHECK_MESSAGE(!availability.reason.empty(), "不可用时必须给出面向用户的原因");
  GC_CHECK(availability.location.empty());

  const gc::platform::HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths();
  GC_CHECK(!paths.valid);
  GC_CHECK(!paths.failureReason.empty());
  GC_CHECK(paths.historyFile.empty());
}

GC_TEST(linux_history_store_never_reads_or_writes_while_unavailable) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), "临时目录应创建成功");
  const std::string historyFile = temp.PathIn("history.prefs");
  RequireWriteFile(historyFile, "evernightcommit.history|1\nmeta|1,90,1000\n");

  // 即使有人给出了一个真实存在的历史文件路径，Linux 实现也不读它：
  // 「没有持久化位置」不是「读到一个空历史」。
  const gc::platform::HistoryReadResult read =
      gc::platform::ReadHistoryFile(gc::platform::Utf8ToWide(historyFile));
  GC_CHECK(read.kind == gc::platform::HistoryReadKind::absent);
  GC_CHECK(read.text.empty());
  GC_CHECK(read.detail.empty());

  const gc::platform::HistoryStorePaths paths = gc::platform::ResolveHistoryStorePaths();
  gc::app::OperationLog ours;
  ours.historyEnabled = true;
  const gc::platform::HistorySaveOutcome save =
      gc::platform::SaveOperationHistory(paths, ours, gc::app::HistoryWriteIntents{}, 1700000000);
  GC_CHECK(save.status == gc::platform::HistorySaveStatus::failed);
  GC_CHECK(save.merged.historyEnabled);  // 内存基线原样交还，不丢本会话记录
  GC_CHECK(!save.detail.empty());

  std::string after;
  RequireReadFile(historyFile, after);
  GC_CHECK_MESSAGE(after.find("evernightcommit.history|1") == 0,
                   "不可用实现不得改写或删除任何已有文件");

  std::wstring reason;
  GC_CHECK(!gc::platform::DeleteHistoryFile(paths, &reason));
  GC_CHECK(!reason.empty());

  std::wstring exportPath;
  reason.clear();
  GC_CHECK(!gc::platform::ExportOperationHistoryFile(paths, ours, 1700000000, &exportPath, &reason));
  GC_CHECK(exportPath.empty());
  GC_CHECK(!reason.empty());
}
