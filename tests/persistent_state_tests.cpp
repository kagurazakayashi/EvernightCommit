// 「保存最近仓库 / 选定 Git / 窗口布局 / 提交草稿」的纯逻辑测试：
// 全部在内存里构造字节与结构，不起进程、不碰文件系统（文件读写与锁的行为在
// persistent_store_file_tests.cpp 的 windows 组验证）。覆盖：
//   * 版本化文本格式的往返（含 '|'、反斜杠、换行、中文路径与正文的逐字段转义）；
//   * 损坏判定的整份性：半条草稿、未知记录、裸回车、坏转义、重复键、坏墙钟——
//     一律整份不成立，绝不拿「读到的那几条」当快照；
//   * 更高版本文件一律 tooNew（不应用、不覆盖），未分版本老文件走 migrate；
//   * MRU 去重（按折叠键）、置顶与上限；草稿表的上限与淘汰取向；
//   * 空正文的两条路：Upsert 直接删除，RecordSnapshot 留删除墓碑，且墓碑永不落盘；
//   * 超限草稿拒绝保存且不改内存里已有的那一份；
//   * 多实例合并：盘上更新的草稿必须保留并报告（最后一个写入者不得悄悄覆盖另一个
//     窗口的新草稿）、两个 worktree 互不覆盖、写入意图门控、MRU 交叉合并、
//     关闭草稿保存即清空、replaceAll 只留策略。
#include <string>
#include <string_view>
#include <vector>

#include "app/persistent_state.h"
#include "git/repository.h"
#include "support/record_text.h"
#include "support/tiny_test.h"

namespace {

using namespace gc::app;
using gc::git::CivilTime;
using gc::git::CommitFormData;

PersistentDraft MakeDraft(std::wstring_view root, std::wstring_view subject, long long epoch) {
  PersistentDraft draft;
  draft.repositoryRoot = root;
  draft.repositoryKey = gc::git::CanonicalPathKey(root);
  draft.form.subject = subject;
  draft.savedAtEpoch = epoch;
  draft.timesUserEdited = true;
  return draft;
}

PersistentState SampleState() {
  PersistentState state;
  state.consentRecorded = true;
  state.persistenceEnabled = true;
  state.draftSavingEnabled = true;
  state.gitExecutablePath = L"C:\\Program Files\\Git\\bin\\git.exe";
  state.recentRepositories = {L"D:\\work\\alpha", L"D:\\work\\beta"};
  state.window.valid = true;
  state.window.x = -8;
  state.window.y = 0;
  state.window.width = 1280;
  state.window.height = 800;
  state.window.maximized = true;
  state.columns.valid = true;
  state.columns.leftPermille = 300;
  state.columns.middlePermille = 400;
  PersistentDraft draft = MakeDraft(L"D:\\work\\alpha", L"修复 a|b 的换行\\问题", 1727000000);
  draft.form.description = L"第一行\n第二行\r\n带 | 竖线 与 \\ 反斜杠";
  draft.form.author = L"张三 <z@example.com>";
  draft.form.coauthors = {L"李四 <l@example.com>", L"W<|>e \"quote\" <q@e.c>"};
  draft.authorWall = CivilTime{2024, 2, 29, 23, 59, 58};
  draft.committerWall = CivilTime{2024, 12, 31, 0, 0, 1};
  draft.timesSynced = false;
  state.drafts.push_back(draft);
  return state;
}

bool StatesEqual(const PersistentState& left, const PersistentState& right) {
  return SerializePersistentState(left) == SerializePersistentState(right);
}

}  // namespace

GC_TEST(persistent_round_trip_preserves_every_record_kind) {
  const PersistentState original = SampleState();
  const std::wstring text = SerializePersistentState(original);
  const PersistentLoadResult parsed = ParsePersistentState(text);
  GC_CHECK(parsed.status == PersistentLoadStatus::loaded);
  GC_CHECK(parsed.state.consentRecorded);
  GC_CHECK(parsed.state.gitExecutablePath == original.gitExecutablePath);
  GC_CHECK(parsed.state.recentRepositories == original.recentRepositories);
  GC_CHECK(parsed.state.window.valid && parsed.state.window.x == -8 && parsed.state.window.maximized);
  GC_CHECK(parsed.state.columns.leftPermille == 300 && parsed.state.columns.middlePermille == 400);
  GC_CHECK(parsed.state.drafts.size() == 1);
  if (parsed.state.drafts.size() == 1) {
    const PersistentDraft& draft = parsed.state.drafts[0];
    // 转义必须逐字符还原：竖线、反斜杠、LF、CRLF、引号与中文都不许变形。
    GC_CHECK(draft.form.subject == original.drafts[0].form.subject);
    GC_CHECK(draft.form.description == original.drafts[0].form.description);
    GC_CHECK(draft.form.author == original.drafts[0].form.author);
    GC_CHECK(draft.form.coauthors == original.drafts[0].form.coauthors);
    GC_CHECK(draft.savedAtEpoch == 1727000000);
    GC_CHECK(draft.timesSynced == false && draft.timesUserEdited == true);
    GC_CHECK(draft.authorWall.year == 2024 && draft.authorWall.month == 2 && draft.authorWall.day == 29);
    GC_CHECK(draft.committerWall.second == 1);
    GC_CHECK(draft.repositoryKey == gc::git::CanonicalPathKey(draft.repositoryRoot));
  }
  GC_CHECK(StatesEqual(original, parsed.state));
}

GC_TEST(persistent_empty_and_default_reads) {
  const PersistentLoadResult blank = ParsePersistentState(L"");
  GC_CHECK(blank.status == PersistentLoadStatus::empty);
  GC_CHECK(blank.state.consentRecorded == false);
  GC_CHECK(blank.state.draftSavingEnabled == true);
  const PersistentLoadResult whitespace = ParsePersistentState(L"  \n\t\n");
  GC_CHECK(whitespace.status == PersistentLoadStatus::empty);
}

GC_TEST(persistent_corrupt_forms_reject_the_whole_file) {
  // 半条草稿（在草稿行中间掐断）：整份不成立，不是「少一条记录」。
  const std::wstring good = SerializePersistentState(SampleState());
  const size_t draftStart = good.find(L"draft|");
  GC_CHECK(draftStart != std::wstring::npos);
  const std::wstring truncated = good.substr(0, draftStart + 8);
  const PersistentLoadResult half = ParsePersistentState(truncated);
  GC_CHECK(half.status == PersistentLoadStatus::corrupt);

  // 未知记录名 / 裸回车 / 坏转义序列 / 重复的 git 键 / 两份同键草稿：全部整份拒绝。
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\ngizmo|1\n").status ==
           PersistentLoadStatus::corrupt);
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\r\ngit|x\n").status ==
           PersistentLoadStatus::corrupt);
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\ngit|bad\\qescape\n").status ==
           PersistentLoadStatus::corrupt);
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\ngit|a\ngit|b\n").status ==
           PersistentLoadStatus::corrupt);
  const std::wstring twoSame =
      L"evernightcommit.prefs|1\ndraft|X:Y|x:y|5|1,1|2024,1,2,3,4,5|2024,1,2,3,4,5|s|||0\n"
      L"draft|X:Y|x:y|6|1,1|2024,1,2,3,4,5|2024,1,2,3,4,5|t|||0\n";
  GC_CHECK(ParsePersistentState(twoSame).status == PersistentLoadStatus::corrupt);
  // 坏墙钟（2023 年没有 2 月 29 日）：不猜、不夹取，整份拒绝。
  const std::wstring badCivil =
      L"evernightcommit.prefs|1\ndraft|X:Y|x:y|5|1,1|2023,2,29,3,4,5|2024,1,2,3,4,5|s|||0\n";
  GC_CHECK(ParsePersistentState(badCivil).status == PersistentLoadStatus::corrupt);
  // 中间空行与「键与 CanonicalPathKey(root) 对不上」都按损坏处理。
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\n\n\ngit|x\n").status ==
           PersistentLoadStatus::corrupt);
  const std::wstring badKey =
      L"evernightcommit.prefs|1\ndraft|X:Y|WRONG|5|1,1|2024,1,2,3,4,5|2024,1,2,3,4,5|s|||0\n";
  GC_CHECK(ParsePersistentState(badKey).status == PersistentLoadStatus::corrupt);
  // 尾随的多余字段同样整份拒绝：不「读前几个、丢后面的」。
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\npersistence|1,1,1,1\n").status ==
           PersistentLoadStatus::corrupt);
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\nwindow|1,2,300,400,0,9\n").status ==
           PersistentLoadStatus::corrupt);
}

GC_TEST(persistent_columns_and_window_forms_are_bounded) {
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\ncolumns|600,500\n").status ==
           PersistentLoadStatus::corrupt);  // 左+中 >= 1000，剩下的列宽没了
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\ncolumns|300,400\n").status ==
           PersistentLoadStatus::loaded);
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\nwindow|1,2,0,400,0\n").status ==
           PersistentLoadStatus::corrupt);  // 宽度为 0 的窗口不是几何
  GC_CHECK(ParsePersistentState(L"evernightcommit.prefs|1\nwindow|1,2,3\n").status ==
           PersistentLoadStatus::corrupt);
}

GC_TEST(persistent_too_new_version_is_refused_not_applied) {
  const std::wstring text = L"evernightcommit.prefs|99\ngit|C:\\future\\git.exe\n";
  const PersistentLoadResult parsed = ParsePersistentState(text);
  GC_CHECK(parsed.status == PersistentLoadStatus::tooNew);
  GC_CHECK(parsed.detectedVersion == 99);
  GC_CHECK(parsed.state.gitExecutablePath.empty());  // 认不出的版本不应用任何字段
}

GC_TEST(persistent_legacy_file_without_header_migrates) {
  // 未分版本的老文件：字段照收，缺的部分用当前默认；状态是 migrated，重写后带版本号。
  // 反斜杠在文件文本里成对出现（转义规则），这里的双份 `\\` 就是那一条约定。
  const std::wstring legacy = L"git|C:\\\\old\\\\git.exe\nrecent|D:\\\\old\\\\repo\nrevision|3\n";
  const PersistentLoadResult parsed = ParsePersistentState(legacy);
  GC_CHECK(parsed.status == PersistentLoadStatus::migrated);
  GC_CHECK(parsed.detectedVersion == 0);
  GC_CHECK(parsed.state.gitExecutablePath == std::wstring(L"C:\\old\\git.exe"));
  GC_CHECK(parsed.state.recentRepositories.size() == 1);
  GC_CHECK(parsed.state.revision == 3);
  // v0 没有「首次说明」的概念：迁移后仍保持未询问，让升级后的第一次启动重新解释一遍。
  GC_CHECK(parsed.state.consentRecorded == false);
  GC_CHECK(parsed.state.draftSavingEnabled == true);
  const std::wstring rewritten = SerializePersistentState(parsed.state);
  GC_CHECK(rewritten.rfind(L"evernightcommit.prefs|1\n", 0) == 0);
  // 认不出任何一条的老内容按损坏处理，而不是「迁移出一张全空的表」。
  GC_CHECK(ParsePersistentState(L"hello world\n").status == PersistentLoadStatus::corrupt);
}

GC_TEST(persistent_recent_list_dedupes_by_folded_key_and_caps) {
  PersistentState state;
  TouchPersistentRecent(&state, L"D:\\Work\\Alpha");
  TouchPersistentRecent(&state, L"d:\\work\\alpha");  // 大小写不同的同一 worktree
  GC_CHECK(state.recentRepositories.size() == 1);
  GC_CHECK(state.recentRepositories[0] == std::wstring(L"d:\\work\\alpha"));  // 最新写法置顶
  for (int i = 0; i < 12; ++i) {
    TouchPersistentRecent(&state, L"R:\\repo" + std::to_wstring(i));
  }
  GC_CHECK(state.recentRepositories.size() == kMaxRecentRepositories);
  GC_CHECK(state.recentRepositories.front() == std::wstring(L"R:\\repo11"));
}

GC_TEST(persistent_draft_empty_body_paths_differ_between_upsert_and_snapshot) {
  PersistentState state;
  std::wstring refusal;
  GC_CHECK(UpsertPersistentDraft(&state, MakeDraft(L"D:\\a", L"标题", 10), &refusal));
  GC_CHECK(state.drafts.size() == 1);
  // Upsert：正文为空＝直接删除这条记录。
  GC_CHECK(UpsertPersistentDraft(&state, MakeDraft(L"D:\\a", L"", 20), &refusal));
  GC_CHECK(state.drafts.empty());

  GC_CHECK(UpsertPersistentDraft(&state, MakeDraft(L"D:\\a", L"标题", 10), &refusal));
  // RecordSnapshot：正文为空＝留下带时刻的空墓碑，等合并时传播删除。
  PersistentDraft tombstone = MakeDraft(L"D:\\a", L"", 30);
  tombstone.form.description.clear();
  GC_CHECK(RecordPersistentDraftSnapshot(&state, tombstone, &refusal));
  GC_CHECK(state.drafts.size() == 1);
  GC_CHECK(PersistentDraftBodyIsEmpty(state.drafts[0]));
  // 墓碑永远不会被序列化出去。
  GC_CHECK(SerializePersistentState(state).find(L"draft|") == std::wstring::npos);
}

GC_TEST(persistent_draft_oversize_is_refused_without_mutation) {
  PersistentState state;
  std::wstring refusal;
  GC_CHECK(UpsertPersistentDraft(&state, MakeDraft(L"D:\\a", L"旧标题", 10), &refusal));
  PersistentDraft huge = MakeDraft(L"D:\\a", std::wstring(kMaxDraftContentChars + 1, L'H'), 20);
  huge.form.subject = std::wstring(kMaxDraftContentChars + 1, L'H');
  GC_CHECK(!UpsertPersistentDraft(&state, huge, &refusal));
  GC_CHECK(!refusal.empty());
  // 拒绝保存时，内存里已有的那份草稿一个字都没变。
  GC_CHECK(state.drafts.size() == 1 && state.drafts[0].form.subject == std::wstring(L"旧标题"));
  GC_CHECK(!RecordPersistentDraftSnapshot(&state, huge, &refusal));
}

GC_TEST(persistent_draft_cap_evicts_oldest) {
  PersistentState state;
  std::wstring refusal;
  for (size_t i = 0; i <= kMaxPersistedDrafts; ++i) {
    GC_CHECK(UpsertPersistentDraft(&state, MakeDraft(L"S:\\r" + std::to_wstring(i), L"标题",
                                                     static_cast<long long>(100 + i)),
                                   &refusal));
  }
  GC_CHECK(state.drafts.size() == kMaxPersistedDrafts);
  GC_CHECK(FindPersistentDraft(state, gc::git::CanonicalPathKey(L"S:\\r0")) == nullptr);
  GC_CHECK(FindPersistentDraft(state, gc::git::CanonicalPathKey(L"S:\\r30")) != nullptr);
}

GC_TEST(persistent_merge_keeps_newer_disk_draft_and_reports_it) {
  // 「最后一个写入者不得悄悄覆盖另一个窗口的新草稿」的主体判定。
  PersistentState disk;
  disk.drafts.push_back(MakeDraft(L"D:\\shared", L"另一个窗口更新过的草稿", 200));
  PersistentState ours;
  ours.drafts.push_back(MakeDraft(L"D:\\shared", L"本窗口较早的一份", 100));
  PersistentWriteIntents intents;
  intents.drafts = true;
  PersistentMergeReport report;
  const PersistentState merged = MergeForWrite(disk, ours, intents, &report);
  GC_CHECK(merged.drafts.size() == 1);
  GC_CHECK(merged.drafts[0].form.subject == std::wstring(L"另一个窗口更新过的草稿"));
  GC_CHECK(report.newerDraftsKeptFromDisk.size() == 1);
  GC_CHECK(report.newerDraftsKeptFromDisk[0] == std::wstring(L"D:\\shared"));

  // 本窗口更新时以本窗口为准，不报告。
  PersistentState oursNewer;
  oursNewer.drafts.push_back(MakeDraft(L"D:\\shared", L"本窗口最新的屏幕内容", 300));
  PersistentMergeReport report2;
  const PersistentState merged2 = MergeForWrite(disk, oursNewer, intents, &report2);
  GC_CHECK(merged2.drafts[0].form.subject == std::wstring(L"本窗口最新的屏幕内容"));
  GC_CHECK(report2.newerDraftsKeptFromDisk.empty());
}

GC_TEST(persistent_merge_two_worktrees_never_collide) {
  // 同一项目的两个 worktree：键不同，两边各自的新草稿都要活着，互不覆盖。
  PersistentState disk;
  disk.drafts.push_back(MakeDraft(L"D:\\proj\\main", L"主 worktree 的草稿", 50));
  PersistentState ours;
  ours.drafts.push_back(MakeDraft(L"D:\\proj\\feature", L"链接 worktree 的草稿", 60));
  PersistentWriteIntents intents;
  intents.drafts = true;
  const PersistentState merged = MergeForWrite(disk, ours, intents, nullptr);
  GC_CHECK(merged.drafts.size() == 2);
  GC_CHECK(FindPersistentDraft(merged, gc::git::CanonicalPathKey(L"D:\\proj\\main")) != nullptr);
  GC_CHECK(FindPersistentDraft(merged, gc::git::CanonicalPathKey(L"D:\\proj\\feature")) != nullptr);
}

GC_TEST(persistent_merge_write_intents_gate_scalar_records) {
  PersistentState disk;
  disk.gitExecutablePath = L"E:\\other\\git.exe";
  disk.window.valid = true;
  disk.window.width = 1000;
  disk.columns.valid = true;
  disk.columns.leftPermille = 200;
  PersistentState ours;
  ours.gitExecutablePath = L"E:\\mine\\git.exe";
  ours.window.valid = true;
  ours.window.width = 1500;
  ours.columns.valid = true;
  ours.columns.leftPermille = 500;
  ours.columns.middlePermille = 200;

  // 没点名的记录沿用盘上的：本窗口一次「只改了草稿」的保存不能顺手把别的窗口的布局抹掉。
  PersistentWriteIntents draftsOnly;
  draftsOnly.drafts = true;
  const PersistentState kept = MergeForWrite(disk, ours, draftsOnly, nullptr);
  GC_CHECK(kept.gitExecutablePath == std::wstring(L"E:\\other\\git.exe"));
  GC_CHECK(kept.window.width == 1000);
  GC_CHECK(kept.columns.leftPermille == 200);

  PersistentWriteIntents all;
  all.gitPath = true;
  all.windowGeometry = true;
  all.columns = true;
  const PersistentState taken = MergeForWrite(disk, ours, all, nullptr);
  GC_CHECK(taken.gitExecutablePath == std::wstring(L"E:\\mine\\git.exe"));
  GC_CHECK(taken.window.width == 1500);
  GC_CHECK(taken.columns.leftPermille == 500);
}

GC_TEST(persistent_merge_interleaves_recent_lists_ours_first) {
  PersistentState disk;
  disk.recentRepositories = {L"D:\\disk\\one", L"D:\\shared\\two"};
  PersistentState ours;
  ours.recentRepositories = {L"D:\\shared\\two", L"D:\\ours\\zero"};
  PersistentWriteIntents intents;
  intents.recentList = true;
  const PersistentState merged = MergeForWrite(disk, ours, intents, nullptr);
  GC_CHECK(merged.recentRepositories.size() == 3);
  GC_CHECK(merged.recentRepositories[0] == std::wstring(L"D:\\shared\\two"));
  GC_CHECK(merged.recentRepositories[1] == std::wstring(L"D:\\ours\\zero"));
  // 折叠键相同的条目算同一条：不能因为盘上写法大小写不同就多出一条。
  PersistentState diskFolded;
  diskFolded.recentRepositories = {L"d:\\SHARED\\two"};
  const PersistentState folded = MergeForWrite(diskFolded, ours, intents, nullptr);
  GC_CHECK(folded.recentRepositories.size() == 2);
}

GC_TEST(persistent_merge_delete_tombstone_rules) {
  PersistentState disk;
  disk.drafts.push_back(MakeDraft(L"D:\\done", L"已经提交掉的旧草稿", 100));
  PersistentState ours;
  ours.drafts.push_back(MakeDraft(L"D:\\done", L"", 150));  // 提交成功后的清空（墓碑）

  PersistentWriteIntents intents;
  intents.drafts = true;
  PersistentMergeReport report;
  const PersistentState merged = MergeForWrite(disk, ours, intents, &report);
  GC_CHECK(merged.drafts.empty());  // 删除传播了
  GC_CHECK(report.newerDraftsKeptFromDisk.empty());

  // 但盘上那份如果是*更新*的一次编辑（另一个窗口正在写），删除不得盖过它。
  PersistentState diskNewer;
  diskNewer.drafts.push_back(MakeDraft(L"D:\\done", L"另一窗口刚刚写的新草稿", 400));
  PersistentMergeReport report2;
  const PersistentState kept = MergeForWrite(diskNewer, ours, intents, &report2);
  GC_CHECK(kept.drafts.size() == 1);
  GC_CHECK(kept.drafts[0].form.subject == std::wstring(L"另一窗口刚刚写的新草稿"));
  GC_CHECK(report2.newerDraftsKeptFromDisk.size() == 1);
}

GC_TEST(persistent_merge_draft_saving_off_clears_disk_drafts) {
  PersistentState disk;
  disk.draftSavingEnabled = true;
  disk.drafts.push_back(MakeDraft(L"D:\\a", L"内容", 100));
  PersistentState ours = disk;
  ours.draftSavingEnabled = false;  // 用户刚点了「不保存草稿」
  ours.drafts.clear();
  PersistentWriteIntents intents;
  intents.policy = true;
  const PersistentState merged = MergeForWrite(disk, ours, intents, nullptr);
  GC_CHECK(merged.draftSavingEnabled == false);
  GC_CHECK(merged.drafts.empty());  // 关闭草稿时盘上的草稿也必须被清掉，不留残余
}

GC_TEST(persistent_merge_replace_all_keeps_only_policy) {
  PersistentState disk;
  disk.consentRecorded = true;
  disk.gitExecutablePath = L"E:\\x\\git.exe";
  disk.recentRepositories = {L"D:\\a"};
  disk.window.valid = true;
  disk.window.width = 100;
  disk.drafts.push_back(MakeDraft(L"D:\\a", L"标题", 100));
  PersistentState ours = disk;
  ClearPersistedUserData(&ours);
  GC_CHECK(ours.gitExecutablePath.empty() && ours.recentRepositories.empty() && ours.drafts.empty() &&
           ours.window.valid == false);
  PersistentWriteIntents intents;
  intents.replaceAll = true;
  intents.policy = true;
  const PersistentState merged = MergeForWrite(disk, ours, intents, nullptr);
  GC_CHECK(merged.gitExecutablePath.empty());
  GC_CHECK(merged.recentRepositories.empty());
  GC_CHECK(merged.drafts.empty());
  GC_CHECK(merged.window.valid == false);
  GC_CHECK(merged.consentRecorded == true);      // 「问过一次」不是用户内容，保留
  GC_CHECK(merged.draftSavingEnabled == true);
  GC_CHECK(merged.revision == disk.revision + 1);
}

GC_TEST(persistent_merge_revision_counts_up_from_disk) {
  PersistentState disk;
  disk.revision = 41;
  PersistentState ours;
  ours.revision = 7;  // 本窗口手里的旧修订号：合并只认盘上的，统一 +1。
  const PersistentState merged = MergeForWrite(disk, ours, PersistentWriteIntents{}, nullptr);
  GC_CHECK(merged.revision == 42);
}

GC_TEST(persistent_describe_storage_path_is_metadata_only) {
  GC_CHECK(DescribeStoragePath(L"C:\\Users\\me\\AppData\\Roaming", L"state.prefs") ==
           std::wstring(L"C:\\Users\\me\\AppData\\Roaming\\state.prefs"));
  GC_CHECK(DescribeStoragePath(L"C:\\Users\\me\\AppData\\Roaming\\", L"state.prefs") ==
           std::wstring(L"C:\\Users\\me\\AppData\\Roaming\\state.prefs"));
  GC_CHECK(DescribeStoragePath(L"", L"state.prefs").find(L"取不到") != std::wstring::npos);
}

// ---- R6：数字字段带上下界解析（走公开的 ParsePersistentState）----

namespace {

using gc::test::BadField;
using gc::test::ReplaceField;

bool AllPrefsCorrupt(const std::wstring& valid,
                     std::wstring_view prefix,
                     size_t fieldIndex,
                     const std::vector<BadField>& cases,
                     const char* what) {
  bool allRejected = true;
  for (const BadField& bad : cases) {
    const PersistentLoadResult result = ParsePersistentState(ReplaceField(valid, prefix, fieldIndex, bad.value));
    if (result.status != PersistentLoadStatus::corrupt) {
      allRejected = false;
      std::string note(what);
      note += " 没拦住：";
      note += bad.label;
      GC_CHECK_MESSAGE(false, note);
    }
  }
  return allRejected;
}

}  // namespace

GC_TEST(persistent_numeric_fields_reject_overflow_without_wrapping) {
  const std::wstring valid = SerializePersistentState(SampleState());
  GC_CHECK(ParsePersistentState(valid).status == PersistentLoadStatus::loaded);

  // 版本号：满量程仍判 tooNew（不应用、不覆盖、不截断），越界判损坏。
  const PersistentLoadResult hugeVersion =
      ParsePersistentState(ReplaceField(valid, L"evernightcommit.prefs", 1, L"9223372036854775807"));
  GC_CHECK_MESSAGE(hugeVersion.status == PersistentLoadStatus::tooNew, "超大自报版本仍须判 tooNew");
  GC_CHECK(hugeVersion.detectedVersion == 9223372036854775807LL);
  GC_CHECK(AllPrefsCorrupt(valid, L"evernightcommit.prefs", 1,
                           {{"19 个 9", L"9999999999999999999"},
                            {"上界 +1", L"9223372036854775808"},
                            {"负号", L"-1"}},
                           "版本号"));

  // 修订号：非负路径；19 个 9 曾在这一档上有符号溢出并返回 true。
  GC_CHECK(ParsePersistentState(ReplaceField(valid, L"revision", 1, L"0")).state.revision == 0);
  GC_CHECK(ParsePersistentState(ReplaceField(valid, L"revision", 1, L"9223372036854775807"))
               .state.revision == 9223372036854775807LL);
  GC_CHECK(AllPrefsCorrupt(valid, L"revision", 1,
                           {{"19 个 9", L"9999999999999999999"},
                            {"超长", L"999999999999999999999999999999999999999999999999"},
                            {"负号", L"-5"},
                            {"空白", L" 5"},
                            {"尾随空白", L"5 "},
                            {"非数字", L"5a"},
                            {"科学计数", L"1e3"}},
                           "修订号"));

  // 窗口坐标：int 上界在缩窄之前判定（此前的写法先算再比，越界值会先回绕）。
  GC_CHECK(ParsePersistentState(ReplaceField(valid, L"window", 1, L"-8,0,1280,800,1")).state.window.valid);
  GC_CHECK(ParsePersistentState(ReplaceField(valid, L"window", 1, L"0,0,2147483647,800,0"))
               .state.window.width == 2147483647);
  GC_CHECK(AllPrefsCorrupt(valid, L"window", 1,
                           {{"宽度越 int", L"0,0,2147483648,800,0"},
                            {"宽度 19 个 9", L"0,0,9999999999999999999,800,0"},
                            {"坐标越界", L"99999999999,0,1280,800,0"},
                            {"尺寸非正", L"0,0,0,800,0"}},
                           "窗口记录"));

  // 列宽千分比：业务区间仍由记录本身裁定，溢出输入整份拒绝。
  GC_CHECK(AllPrefsCorrupt(valid, L"columns", 1,
                           {{"溢出", L"9999999999999999999,400"},
                            {"越 int", L"2147483648,400"},
                            {"超出千分比", L"300,700"}},
                           "列宽记录"));

  // 草稿保存时刻与合作者条数。
  GC_CHECK(AllPrefsCorrupt(valid, L"draft", 3,
                           {{"19 个 9", L"9999999999999999999"},
                            {"负号", L"-1"}},
                           "草稿时刻"));
  GC_CHECK(AllPrefsCorrupt(valid, L"draft", 10,
                           {{"溢出", L"9999999999999999999"},
                            {"与尾巴字段数不符", L"5"}},
                           "合作者条数"));

  // 墙钟六个整数同样按 int 上下界判定，不接受溢出后的回绕值。
  GC_CHECK(AllPrefsCorrupt(valid, L"draft", 5,
                           {{"年份溢出", L"9999999999999999999,2,29,23,59,58"},
                            {"年份越 int", L"2147483648,2,29,23,59,58"}},
                           "墙钟时间"));
}
