#include "app/operation_history.h"

#include <string>
#include <vector>

#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

namespace h = gc::app;

constexpr wchar_t kFullOidA = L'a';
std::wstring FullOid(char filler) {
  return std::wstring(40, static_cast<wchar_t>(filler));
}

// 一条身份自洽的记录：key 由根目录算出，对象 ID 用合法十六进制。
h::OperationRecord MakeRecord(std::wstring_view id) {
  h::OperationRecord record;
  record.id = std::wstring(id);
  record.startedEpoch = 1700000000;
  record.terminalEpoch = 1700000010;
  record.flow = h::HistoryFlow::undo;
  record.workTreeRoot = L"C:\\repo\\proj";
  record.repositoryKey = gc::git::CanonicalPathKey(record.workTreeRoot);
  record.operationLabel = L"撤回最近提交";
  record.sourceRef = L"refs/heads/main";
  record.sourceObjectId = FullOid('b');
  record.restoreKind = h::HistoryRestoreKind::refMove;
  record.restoreBranchRef = L"refs/heads/main";
  record.restoreUndoToObjectId = FullOid('c');
  record.restoreExpectedCurrentId = FullOid('b');
  record.terminal = h::HistoryTerminal::inProgress;
  return record;
}

// ---- SanitizeHistoryText ----

h::OperationLog FreshLog() {
  h::OperationLog log;
  log.historyEnabled = true;
  return log;
}

}  // namespace

GC_TEST(operation_history_id_safety) {
  GC_CHECK(h::IsSafeHistoryId(L"op-2026_1001-a1"));
  GC_CHECK(h::IsSafeHistoryId(std::wstring(h::kMaxHistoryIdLength, L'x')));
  GC_CHECK_MESSAGE(!h::IsSafeHistoryId(L""), "空 ID 非法");
  GC_CHECK_MESSAGE(!h::IsSafeHistoryId(std::wstring(h::kMaxHistoryIdLength + 1, L'x')), "超长 ID 非法");
  GC_CHECK_MESSAGE(!h::IsSafeHistoryId(L"has space"), "含空格非法");
  GC_CHECK_MESSAGE(!h::IsSafeHistoryId(L"has|bar"), "含竖线非法");
  GC_CHECK_MESSAGE(!h::IsSafeHistoryId(L"中文"), "非 ASCII 非法");
}

GC_TEST(operation_history_sanitize_masks_and_strips) {
  // 凭据 URL 掩码：任何 scheme://user:pass@ 形态都要被收掉。
  const std::wstring masked = h::SanitizeHistoryText(L"push https://user:token@example.com/x.git failed");
  GC_CHECK_MESSAGE(masked.find(L"token") == std::wstring::npos, "掩码后不得残留口令");
  GC_CHECK_MESSAGE(masked.find(L"***") != std::wstring::npos, "掩码应留下 *** 标记");
  // 控制字符剔除（含制表/换行/NUL）：一行一条记录，摘要不许带换行。
  const std::wstring stripped = h::SanitizeHistoryText(std::wstring(L"a\tb\r\nc\0d", 8));
  GC_CHECK_MESSAGE(stripped == L"abcd", "控制字符必须被剔除");
  // 限长。
  const std::wstring longText(h::kMaxHistoryFieldChars + 100, L'z');
  GC_CHECK_MESSAGE(h::SanitizeHistoryText(longText).size() == h::kMaxHistoryFieldChars, "超上限须截断");
}

GC_TEST(operation_history_append_rejects_bad_identity) {
  h::OperationLog log = FreshLog();
  h::OperationRecord bad = MakeRecord(L"bad id");  // 含空格
  std::wstring refusal;
  GC_CHECK_MESSAGE(!h::AppendHistoryRecord(&log, bad, &refusal), "非法 ID 应拒绝");
  GC_CHECK_MESSAGE(log.records.empty(), "被拒绝的记录不得进入内存");

  h::OperationRecord mismatched = MakeRecord(L"ok1");
  mismatched.repositoryKey = L"totally-different-key";
  GC_CHECK_MESSAGE(!h::AppendHistoryRecord(&log, mismatched, &refusal), "根目录与规范键不匹配应拒绝");

  h::OperationRecord badOid = MakeRecord(L"ok2");
  badOid.sourceObjectId = L"not-hex!";
  GC_CHECK_MESSAGE(!h::AppendHistoryRecord(&log, badOid, &refusal), "非法对象 ID 应拒绝");
}

GC_TEST(operation_history_round_trip_full_record) {
  h::OperationLog log = FreshLog();
  h::OperationRecord record = MakeRecord(L"op-full-1");
  record.terminal = h::HistoryTerminal::succeeded;
  record.exitCodeKnown = true;
  record.exitCode = 0;
  record.completionLabel = L"正常结束";
  record.outcomeNote = L"分支已挪回父提交";
  record.flow = h::HistoryFlow::push;
  record.targetRef = L"refs/heads/main";
  record.targetObjectId = FullOid('d');
  record.remoteName = L"origin";
  h::HistoryTargetCheck check;
  check.maskedUrl = L"https://***@example.com/x.git";
  check.queried = true;
  check.ok = true;
  check.refPresent = true;
  check.remoteObjectId = FullOid('d');
  record.targetChecks.push_back(check);
  h::HistoryReview review;
  review.epoch = 1700000099;
  review.text = L"事后 ls-remote 复核：对端仍是那一份";
  record.reviews.push_back(review);
  std::wstring refusal;
  GC_REQUIRE(h::AppendHistoryRecord(&log, record, &refusal), "记录一条完整操作");

  const std::wstring text = h::SerializeOperationLog(log);
  const h::HistoryLoadResult parsed = h::ParseOperationLog(text);
  GC_CHECK_MESSAGE(parsed.status == h::HistoryLoadStatus::loaded, "完整记录应能读回");
  GC_REQUIRE_MESSAGE(parsed.log.records.size() == 1, std::string("round trip produced one record"));
  if (parsed.log.records.size() == 1) {
    const h::OperationRecord& back = parsed.log.records[0];
    GC_CHECK(back.id == L"op-full-1");
    GC_CHECK(back.terminal == h::HistoryTerminal::succeeded);
    GC_CHECK(back.exitCodeKnown && back.exitCode == 0);
    GC_CHECK(back.targetObjectId == FullOid('d'));
    GC_CHECK(back.restoreKind == h::HistoryRestoreKind::refMove);
    GC_CHECK(back.restoreUndoToObjectId == FullOid('c'));
    GC_CHECK_MESSAGE(back.targetChecks.size() == 1, "逐目标核实应挂回主记录");
    if (!back.targetChecks.empty()) {
      GC_CHECK(back.targetChecks[0].remoteObjectId == FullOid('d'));
      GC_CHECK(back.targetChecks[0].queried && back.targetChecks[0].ok);
    }
    GC_CHECK_MESSAGE(back.reviews.size() == 1, "复核应挂回主记录");
    if (!back.reviews.empty()) {
      GC_CHECK(back.reviews[0].epoch == 1700000099);
    }
  }
}

GC_TEST(operation_history_empty_and_toonew) {
  GC_CHECK(h::ParseOperationLog(L"").status == h::HistoryLoadStatus::empty);
  GC_CHECK(h::ParseOperationLog(L"   \n\t\r\n").status == h::HistoryLoadStatus::empty);
  const std::wstring future = L"evernightcommit.history|99\nmeta|1,90,1000\n";
  const h::HistoryLoadResult tooNew = h::ParseOperationLog(future);
  GC_CHECK_MESSAGE(tooNew.status == h::HistoryLoadStatus::tooNew, "更高版本须判 tooNew");
  GC_CHECK(tooNew.detectedVersion == 99);
}

GC_TEST(operation_history_corruption_rejects_whole_file) {
  const std::wstring valid = [] {
    h::OperationLog log = FreshLog();
    h::OperationRecord record = MakeRecord(L"op-c");
    record.terminal = h::HistoryTerminal::succeeded;
    std::wstring refusal;
    h::AppendHistoryRecord(&log, record, &refusal);
    return h::SerializeOperationLog(log);
  }();
  // 缺版本头。
  GC_CHECK(h::ParseOperationLog(L"meta|1,90,1000\n").status == h::HistoryLoadStatus::corrupt);
  // 裸回车。
  std::wstring withCr = valid;
  withCr[5] = L'\r';
  GC_CHECK(h::ParseOperationLog(withCr).status == h::HistoryLoadStatus::corrupt);
  // 未知记录名。
  GC_CHECK(h::ParseOperationLog(valid + L"bogus|x\n").status == h::HistoryLoadStatus::corrupt);
  // 附属行没有主记录。
  GC_CHECK(h::ParseOperationLog(valid + L"check|ghost|url|1,1,0|oid|f\n")
               .status == h::HistoryLoadStatus::corrupt);
  // record 字段数量不对（砍掉尾巴一列）。
  {
    std::vector<std::wstring> lines;
    size_t start = 0;
    for (;;) {
      const size_t nl = valid.find(L'\n', start);
      lines.push_back(valid.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
      if (nl == std::wstring::npos) {
        break;
      }
      start = nl + 1;
    }
    for (std::wstring& line : lines) {
      if (line.rfind(L"record|", 0) == 0) {
        const size_t lastBar = line.find_last_of(L'|');
        line = line.substr(0, lastBar);  // 少一列
      }
    }
    std::wstring truncated;
    for (const std::wstring& line : lines) {
      truncated += line;
      truncated.push_back(L'\n');
    }
    GC_CHECK_MESSAGE(h::ParseOperationLog(truncated).status == h::HistoryLoadStatus::corrupt,
                     "record 少字段必须整份拒绝");
  }
  // 同一 ID 出现两次。
  {
    h::OperationLog log = FreshLog();
    std::wstring refusal;
    h::AppendHistoryRecord(&log, MakeRecord(L"dup"), &refusal);
    std::wstring serialized = h::SerializeOperationLog(log);
    // 手工复制 record 行造成重复。
    const size_t recStart = serialized.find(L"record|");
    const size_t recEnd = serialized.find(L'\n', recStart);
    serialized += serialized.substr(recStart, recEnd - recStart);
    serialized.push_back(L'\n');
    GC_CHECK(h::ParseOperationLog(serialized).status == h::HistoryLoadStatus::corrupt);
  }
}

GC_TEST(operation_history_dedup_upgrades_terminal) {
  h::OperationLog log = FreshLog();
  std::wstring refusal;
  h::OperationRecord first = MakeRecord(L"op-up");
  first.terminal = h::HistoryTerminal::inProgress;
  GC_REQUIRE(h::AppendHistoryRecord(&log, first, &refusal), "先记一条已启动未见结果");

  h::OperationRecord second = MakeRecord(L"op-up");
  second.terminal = h::HistoryTerminal::succeeded;
  second.exitCodeKnown = true;
  second.outcomeNote = L"命令窗口里 Git 返回 0";
  GC_REQUIRE(h::AppendHistoryRecord(&log, second, &refusal), "同 ID 再次登记");
  GC_CHECK(log.records.size() == 1);
  GC_CHECK(log.records[0].terminal == h::HistoryTerminal::succeeded);
  GC_CHECK(log.records[0].outcomeNote == L"命令窗口里 Git 返回 0");

  // 更弱的一次观察不得把已确定的终态降级回去。
  h::OperationRecord third = MakeRecord(L"op-up");
  third.terminal = h::HistoryTerminal::inProgress;
  GC_REQUIRE(h::AppendHistoryRecord(&log, third, &refusal), "第三次登记");
  GC_CHECK_MESSAGE(log.records[0].terminal == h::HistoryTerminal::succeeded,
                   "已确定终态不得被 inProgress 覆盖");
}

GC_TEST(operation_history_append_never_downgrades_then_upgrades) {
  // unknown（结果未知）可被之后确定的一次观察取代：这正是「未知后重新核实」的落点。
  h::OperationLog log = FreshLog();
  std::wstring refusal;
  h::OperationRecord unknown = MakeRecord(L"op-unk");
  unknown.terminal = h::HistoryTerminal::unknown;
  h::AppendHistoryRecord(&log, unknown, &refusal);
  h::OperationRecord settled = MakeRecord(L"op-unk");
  settled.terminal = h::HistoryTerminal::failed;
  settled.exitCodeKnown = true;
  settled.exitCode = 128;
  h::AppendHistoryRecord(&log, settled, &refusal);
  GC_CHECK(log.records[0].terminal == h::HistoryTerminal::failed);
  GC_CHECK(log.records[0].exitCode == 128);
}

GC_TEST(operation_history_append_review) {
  h::OperationLog log = FreshLog();
  std::wstring refusal;
  h::AppendHistoryRecord(&log, MakeRecord(L"op-rev"), &refusal);
  GC_CHECK(h::AppendHistoryReview(&log, L"op-rev", 100, L"第一次复核"));
  GC_CHECK(h::AppendHistoryReview(&log, L"op-rev", 200, L"第二次复核"));
  // 重复登记同一次复核不制造两条。
  GC_CHECK(h::AppendHistoryReview(&log, L"op-rev", 100, L"第一次复核"));
  GC_CHECK(log.records[0].reviews.size() == 2);
  GC_CHECK_MESSAGE(!h::AppendHistoryReview(&log, L"ghost", 1, L"x"), "找不到主记录应返回 false");

  // 复核尾巴超限丢最早的。
  for (long long i = 1; i <= 40; ++i) {
    h::AppendHistoryReview(&log, L"op-rev", 1000 + i, L"r" + std::to_wstring(i));
  }
  GC_CHECK(log.records[0].reviews.size() == h::kMaxHistoryReviewsPerRecord);
  GC_CHECK_MESSAGE(log.records[0].reviews.front().epoch > 1000, "最早的复核应被淘汰");
}

GC_TEST(operation_history_capacity_trims_oldest) {
  h::OperationLog log = FreshLog();
  log.maxRecords = 3;
  std::wstring refusal;
  for (int i = 0; i < 5; ++i) {
    h::OperationRecord record = MakeRecord(L"op" + std::to_wstring(i));
    record.startedEpoch = 1000 + i;
    h::AppendHistoryRecord(&log, record, &refusal);
  }
  GC_CHECK(log.records.size() == 3);
  GC_CHECK(log.records.front().id == L"op2");
  GC_CHECK(log.records.back().id == L"op4");
}

GC_TEST(operation_history_retention_by_age) {
  h::OperationLog log = FreshLog();
  log.retentionDays = 10;
  std::wstring refusal;
  const long long now = 1000 + 100LL * 24 * 3600;  // 远在保留期之后
  h::OperationRecord old = MakeRecord(L"old");
  old.startedEpoch = 1000;                                     // 远超 10 天：淘汰
  h::OperationRecord recent = MakeRecord(L"recent");
  recent.startedEpoch = now - 9LL * 24 * 3600;                // 相对此刻 9 天前：保留
  h::OperationRecord unknownTime = MakeRecord(L"epoch0");
  unknownTime.startedEpoch = 0;  // 时刻未知：宁可多留也不当过期丢
  h::AppendHistoryRecord(&log, old, &refusal);
  h::AppendHistoryRecord(&log, recent, &refusal);
  h::AppendHistoryRecord(&log, unknownTime, &refusal);
  h::ApplyHistoryRetention(&log, now);
  GC_CHECK_MESSAGE(log.records.size() == 2, "超期的整条带走，未知时刻与近期保留");
  for (const h::OperationRecord& record : log.records) {
    GC_CHECK_MESSAGE(record.id != L"old", "超期记录必须淘汰");
  }
  // retentionDays=0 时不按时间淘汰。
  log.retentionDays = 0;
  log.records.clear();
  h::AppendHistoryRecord(&log, old, &refusal);
  h::ApplyHistoryRetention(&log, now);
  GC_CHECK(log.records.size() == 1);
}

GC_TEST(operation_history_merge_union_and_stronger_kept) {
  // 盘上已确定，本窗口这条更弱（重启后又看到 inProgress）：保留盘上并报告。
  h::OperationLog disk = FreshLog();
  h::OperationLog ours = FreshLog();
  std::wstring refusal;
  h::OperationRecord settled = MakeRecord(L"m1");
  settled.terminal = h::HistoryTerminal::succeeded;
  h::AppendHistoryRecord(&disk, settled, &refusal);
  h::OperationRecord weaker = MakeRecord(L"m1");
  weaker.terminal = h::HistoryTerminal::inProgress;
  h::AppendHistoryRecord(&ours, weaker, &refusal);
  h::OperationRecord fresh = MakeRecord(L"m2");
  fresh.startedEpoch = 2000;
  h::AppendHistoryRecord(&ours, fresh, &refusal);

  h::HistoryWriteIntents intents;
  intents.append = true;
  h::HistoryMergeReport report;
  const h::OperationLog merged = h::MergeHistoryForWrite(disk, ours, intents, 3000, &report);
  GC_CHECK(merged.records.size() == 2);
  GC_CHECK_MESSAGE(report.keptFromDiskStrongerTerminal.size() == 1 &&
                       report.keptFromDiskStrongerTerminal[0] == L"m1",
                   "盘上更强的终态必须报告为保留");
  for (const h::OperationRecord& record : merged.records) {
    if (record.id == L"m1") {
      GC_CHECK(record.terminal == h::HistoryTerminal::succeeded);
    }
  }
}

GC_TEST(operation_history_merge_policy_and_sort) {
  h::OperationLog disk = FreshLog();
  disk.historyEnabled = true;
  std::wstring refusal;
  h::OperationRecord newerOnDisk = MakeRecord(L"s2");
  newerOnDisk.startedEpoch = 5000;
  h::AppendHistoryRecord(&disk, newerOnDisk, &refusal);
  h::OperationLog ours = FreshLog();
  ours.historyEnabled = false;
  ours.retentionDays = 7;
  ours.maxRecords = 50;
  h::OperationRecord olderFromUs = MakeRecord(L"s1");
  olderFromUs.startedEpoch = 1000;
  h::AppendHistoryRecord(&ours, olderFromUs, &refusal);

  h::HistoryWriteIntents intents;
  intents.policy = true;
  intents.append = true;
  const h::OperationLog merged = h::MergeHistoryForWrite(disk, ours, intents, 0, nullptr);
  GC_CHECK_MESSAGE(!merged.historyEnabled, "策略取本窗口");
  GC_CHECK(merged.retentionDays == 7);
  GC_CHECK(merged.maxRecords == 50);
  GC_CHECK(merged.records.size() == 2);
  GC_CHECK_MESSAGE(merged.records.front().id == L"s1", "合并后按启动时刻升序");
}

GC_TEST(operation_history_merge_replace_all_clears) {
  h::OperationLog disk = FreshLog();
  std::wstring refusal;
  h::AppendHistoryRecord(&disk, MakeRecord(L"x1"), &refusal);
  h::AppendHistoryRecord(&disk, MakeRecord(L"x2"), &refusal);
  h::OperationLog ours = disk;
  ours.records.clear();
  ours.historyEnabled = true;
  h::HistoryWriteIntents intents;
  intents.replaceAll = true;
  const h::OperationLog merged = h::MergeHistoryForWrite(disk, ours, intents, 0, nullptr);
  GC_CHECK(merged.records.empty());
}

GC_TEST(operation_history_labels_nonempty) {
  GC_CHECK(!h::HistoryFlowLabel(h::HistoryFlow::undo).empty());
  GC_CHECK(!h::HistoryTerminalLabel(h::HistoryTerminal::unknown).empty());
  GC_CHECK(!h::HistoryRestoreKindLabel(h::HistoryRestoreKind::manualRemote).empty());
  // 未知/越界也要有兜底文本，不能返回空串让界面显示空白。
  GC_CHECK(!h::HistoryFlowLabel(static_cast<h::HistoryFlow>(99)).empty());
}

GC_TEST(operation_history_compose_masks_and_separates_terminal) {
  h::HistoryCapture capture;
  capture.record = true;
  capture.flow = h::HistoryFlow::push;
  capture.operationLabel = L"推送";
  capture.sourceRef = L"refs/heads/main";
  capture.sourceObjectId = FullOid('b');
  capture.targetObjectId = FullOid('d');
  capture.remoteName = L"origin";
  capture.publishUrls = {L"https://user:secret@remote.example.com/x.git", L"P:\\local\\bare.git"};
  capture.restoreKind = h::HistoryRestoreKind::manualRemote;
  capture.restoreNote = L"已推送：本地回退不会让远端也回退";

  h::HistoryTerminalInfo t;
  t.outcome = h::HistoryOutcome::unknown;  // 窗口提前关闭：结果未知，不能算成功也不能算失败
  t.startedEpoch = 1000;
  t.terminalEpoch = 2000;
  t.exitCodeKnown = false;
  t.completionLabel = L"窗口提前关闭";
  t.conclusion = L"git push https://u:p@host failed";

  const h::OperationRecord rec = h::ComposeHistoryRecord(L"op-compose-1", L"P:\\repo\\proj", capture, t);
  GC_CHECK(rec.flow == h::HistoryFlow::push);
  GC_CHECK(rec.terminal == h::HistoryTerminal::unknown);
  GC_CHECK(rec.repositoryKey == gc::git::CanonicalPathKey(L"P:\\repo\\proj"));
  GC_CHECK(rec.sourceObjectId == FullOid('b'));
  GC_CHECK(rec.targetObjectId == FullOid('d'));
  GC_CHECK_MESSAGE(rec.targetChecks.size() == 2, "每个发布 URL 建一条未核实的逐目标占位");
  if (rec.targetChecks.size() == 2) {
    GC_CHECK(rec.targetChecks[0].maskedUrl.find(L"secret") == std::wstring::npos);
    GC_CHECK(rec.targetChecks[0].maskedUrl.find(L"***") != std::wstring::npos);
    GC_CHECK_MESSAGE(!rec.targetChecks[0].queried, "命令窗口那刻还没核实");
  }
  // 结论文本同样脱敏：URL 内嵌凭据被掩掉。
  GC_CHECK_MESSAGE(rec.outcomeNote.find(L"u:p") == std::wstring::npos, "结论里不得残留凭据");
  GC_CHECK(rec.restoreKind == h::HistoryRestoreKind::manualRemote);
  // 落账后能往返。
  h::OperationLog log = FreshLog();
  std::wstring refusal;
  GC_REQUIRE(h::AppendHistoryRecord(&log, rec, &refusal), "装配记录可落账");
  const h::HistoryLoadResult back = h::ParseOperationLog(h::SerializeOperationLog(log));
  GC_CHECK(back.status == h::HistoryLoadStatus::loaded);
}

GC_TEST(operation_history_compose_undo_refmove_clues) {
  h::HistoryCapture capture;
  capture.record = true;
  capture.flow = h::HistoryFlow::undo;
  capture.operationLabel = L"撤回最近提交";
  capture.sourceRef = L"refs/heads/main";
  capture.restoreKind = h::HistoryRestoreKind::refMove;
  capture.restoreBranchRef = L"refs/heads/main";
  capture.restoreExpectedCurrentId = FullOid('b');  // 撤回后分支应指（父提交）
  capture.restoreUndoToObjectId = FullOid('c');     // 恢复要挪回的原提交
  capture.restoreIsRoot = false;

  h::HistoryTerminalInfo t;
  t.outcome = h::HistoryOutcome::succeeded;
  t.startedEpoch = 5;
  t.terminalEpoch = 9;
  t.exitCodeKnown = true;
  t.exitCode = 0;
  const h::OperationRecord rec = h::ComposeHistoryRecord(L"op-compose-2", L"P:\\repo", capture, t);
  GC_CHECK(rec.terminal == h::HistoryTerminal::succeeded);
  GC_CHECK(rec.exitCodeKnown && rec.exitCode == 0);
  GC_CHECK(rec.restoreKind == h::HistoryRestoreKind::refMove);
  GC_CHECK(rec.restoreExpectedCurrentId == FullOid('b'));
  GC_CHECK(rec.restoreUndoToObjectId == FullOid('c'));
}
