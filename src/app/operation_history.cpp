#include "app/operation_history.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "git/commit_history.h"  // LooksLikeFullObjectId：对象 ID 字段落盘/读回前先按可信形态裁定
#include "git/push_plan.h"       // MaskPushUrlCredentialsInText：任何 URL 落盘/展示前先过这一道
#include "git/repository.h"      // CanonicalPathKey

namespace gc::app {
namespace {

constexpr std::wstring_view kMagicName = L"evernightcommit.history";
constexpr std::wstring_view kMetaName = L"meta";
constexpr std::wstring_view kRecordName = L"record";
constexpr std::wstring_view kCheckName = L"check";
constexpr std::wstring_view kReviewName = L"review";

// ---- 字段转义（与 persistent_state 同一套约定：反斜杠最先转义）----

std::wstring EscapeField(std::wstring_view value) {
  std::wstring out;
  out.reserve(value.size() + 8);
  for (const wchar_t c : value) {
    switch (c) {
      case L'\\':
        out += L"\\\\";
        break;
      case L'|':
        out += L"\\v";
        break;
      case L'\n':
        out += L"\\n";
        break;
      case L'\r':
        out += L"\\r";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

bool UnescapeField(std::wstring_view value, std::wstring* out) {
  std::wstring decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const wchar_t c = value[i];
    if (c == L'\0') {
      return false;
    }
    if (c != L'\\') {
      decoded.push_back(c);
      continue;
    }
    if (i + 1 >= value.size()) {
      return false;
    }
    const wchar_t next = value[++i];
    switch (next) {
      case L'\\':
        decoded.push_back(L'\\');
        break;
      case L'v':
        decoded.push_back(L'|');
        break;
      case L'n':
        decoded.push_back(L'\n');
        break;
      case L'r':
        decoded.push_back(L'\r');
        break;
      default:
        return false;
    }
  }
  *out = std::move(decoded);
  return true;
}

std::vector<std::wstring_view> SplitPipes(std::wstring_view line) {
  std::vector<std::wstring_view> parts;
  size_t start = 0;
  for (;;) {
    const size_t bar = line.find(L'|', start);
    if (bar == std::wstring_view::npos) {
      parts.push_back(line.substr(start));
      break;
    }
    parts.push_back(line.substr(start, bar - start));
    start = bar + 1;
  }
  return parts;
}

bool ParseNonNegative(std::wstring_view text, long long* out) {
  if (text.empty() || text.size() > 19) {
    return false;
  }
  long long value = 0;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') {
      return false;
    }
    value = value * 10 + static_cast<long long>(c - L'0');
  }
  *out = value;
  return true;
}

bool ParseSigned(std::wstring_view text, long long* out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  size_t index = 0;
  bool negative = false;
  if (text[0] == L'-') {
    negative = true;
    index = 1;
  }
  if (index >= text.size()) {
    return false;
  }
  long long value = 0;
  for (; index < text.size(); ++index) {
    const wchar_t c = text[index];
    if (c < L'0' || c > L'9') {
      return false;
    }
    // 退出码量级不该接近 64 位边界；到了就是坏数据，拒绝而不是回绕。
    if (value > (9223372036854775807LL - 9) / 10) {
      return false;
    }
    value = value * 10 + static_cast<long long>(c - L'0');
  }
  *out = negative ? -value : value;
  return true;
}

bool ParseFlag(std::wstring_view text, bool* out) {
  if (text == L"0") {
    *out = false;
    return true;
  }
  if (text == L"1") {
    *out = true;
    return true;
  }
  return false;
}

// 三态 flag 串（0/1，逗号分隔，恰好三个）：逐目标的 queried,ok,refPresent。
bool ParseFlags3(std::wstring_view text, bool* a, bool* b, bool* c) {
  const size_t first = text.find(L',');
  if (first == std::wstring_view::npos) {
    return false;
  }
  const size_t second = text.find(L',', first + 1);
  if (second == std::wstring_view::npos || text.find(L',', second + 1) != std::wstring_view::npos) {
    return false;
  }
  return ParseFlag(text.substr(0, first), a) &&
         ParseFlag(text.substr(first + 1, second - first - 1), b) &&
         ParseFlag(text.substr(second + 1), c);
}

// ---- 枚举取值范围 ----

bool FlowInRange(long long v) { return v >= 0 && v <= static_cast<long long>(HistoryFlow::conflictAbort); }
bool TerminalInRange(long long v) {
  return v >= 0 && v <= static_cast<long long>(HistoryTerminal::unknown);
}
bool RestoreKindInRange(long long v) {
  return v >= 0 && v <= static_cast<long long>(HistoryRestoreKind::manualRemote);
}

// 一个「可选的对象 ID」字段：空串合法（表示「这个位置本就没有对象」，如分支尚不存在），
// 非空必须是 Git 的可信完整对象 ID，否则这份记录已经不是本程序写的那份。
bool OptionalObjectIdOk(std::wstring_view id) { return id.empty() || git::LooksLikeFullObjectId(id); }

// 终态的「确定度」：越小说明关于「这次到底做成了没有」知道得越少。
// 同一 ID 上，只有更确定的终态才允许替换已记录的终态（inProgress 可被任何已知结果取代；
// 一旦记录到确定结果，就绝不被更弱的一次观察悄悄降级——AGENTS「未知分立、复核只追加」的落点）。
int TerminalRank(HistoryTerminal terminal) noexcept {
  switch (terminal) {
    case HistoryTerminal::inProgress:
      return 0;
    case HistoryTerminal::unknown:
      return 1;
    case HistoryTerminal::canceled:
    case HistoryTerminal::launchFailed:
    case HistoryTerminal::gitNotStarted:
    case HistoryTerminal::failed:
    case HistoryTerminal::succeeded:
      return 2;
  }
  return 0;
}

// 把 incoming 的结构化事实并入 existing：已有值优先（确认信息只写一次），空位用 incoming 填洞；
// 终态仅在 incoming 更确定时整体升级。返回 true 表示这次并入了新信息。
bool AbsorbIntoExisting(OperationRecord& existing, OperationRecord&& incoming) {
  bool changed = false;
  const auto fill = [&](std::wstring& slot, std::wstring&& value) {
    if (slot.empty() && !value.empty()) {
      slot = std::move(value);
      changed = true;
    }
  };
  fill(existing.workTreeRoot, std::move(incoming.workTreeRoot));
  fill(existing.repositoryKey, std::move(incoming.repositoryKey));
  fill(existing.operationLabel, std::move(incoming.operationLabel));
  fill(existing.sourceRef, std::move(incoming.sourceRef));
  fill(existing.sourceObjectId, std::move(incoming.sourceObjectId));
  fill(existing.targetRef, std::move(incoming.targetRef));
  fill(existing.targetObjectId, std::move(incoming.targetObjectId));
  fill(existing.remoteName, std::move(incoming.remoteName));
  fill(existing.restoreBranchRef, std::move(incoming.restoreBranchRef));
  fill(existing.restoreUndoToObjectId, std::move(incoming.restoreUndoToObjectId));
  fill(existing.restoreExpectedCurrentId, std::move(incoming.restoreExpectedCurrentId));
  fill(existing.restoreNote, std::move(incoming.restoreNote));
  if (existing.flow == HistoryFlow::unknown && incoming.flow != HistoryFlow::unknown) {
    existing.flow = incoming.flow;
    changed = true;
  }
  if (existing.restoreKind == HistoryRestoreKind::none &&
      incoming.restoreKind != HistoryRestoreKind::none) {
    existing.restoreKind = incoming.restoreKind;
    existing.restoreIsRoot = incoming.restoreIsRoot;
    changed = true;
  }
  if (existing.startedEpoch == 0 && incoming.startedEpoch != 0) {
    existing.startedEpoch = incoming.startedEpoch;
    changed = true;
  }
  // 逐目标核实：existing 还没核实过而 incoming 带了结果，接受它（核实发生在命令之后）。
  if (existing.targetChecks.empty() && !incoming.targetChecks.empty()) {
    existing.targetChecks = std::move(incoming.targetChecks);
    changed = true;
  }
  // 复核尾巴并集：按 epoch+text 去重，保留最近若干条。
  for (HistoryReview& review : incoming.reviews) {
    const bool dup = std::any_of(existing.reviews.begin(), existing.reviews.end(),
                                 [&](const HistoryReview& e) {
                                   return e.epoch == review.epoch && e.text == review.text;
                                 });
    if (!dup) {
      existing.reviews.push_back(std::move(review));
      changed = true;
    }
  }
  if (existing.reviews.size() > kMaxHistoryReviewsPerRecord) {
    existing.reviews.erase(existing.reviews.begin(),
                           existing.reviews.begin() + static_cast<long long>(
                                                           existing.reviews.size() - kMaxHistoryReviewsPerRecord));
  }
  // 终态升级：只有 incoming 更确定时才替换终态相关字段。
  if (TerminalRank(incoming.terminal) > TerminalRank(existing.terminal)) {
    existing.terminal = incoming.terminal;
    existing.terminalEpoch = incoming.terminalEpoch;
    existing.exitCodeKnown = incoming.exitCodeKnown;
    existing.exitCode = incoming.exitCode;
    existing.completionLabel = std::move(incoming.completionLabel);
    if (!incoming.outcomeNote.empty()) {
      existing.outcomeNote = std::move(incoming.outcomeNote);
    }
    changed = true;
  } else if (existing.terminalEpoch == 0 && incoming.terminalEpoch != 0 &&
             existing.terminal == incoming.terminal) {
    existing.terminalEpoch = incoming.terminalEpoch;  // 同一终态，补上时刻
    changed = true;
  }
  return changed;
}

void TrimToMaxRecords(OperationLog& log) {
  if (log.records.size() > log.maxRecords && log.maxRecords > 0) {
    log.records.erase(log.records.begin(),
                      log.records.begin() +
                          static_cast<long long>(log.records.size() - log.maxRecords));
  }
}

}  // namespace

std::wstring_view HistoryFlowLabel(HistoryFlow flow) noexcept {
  switch (flow) {
    case HistoryFlow::unknown:
      return L"未知操作";
    case HistoryFlow::commit:
      return L"创建提交";
    case HistoryFlow::undo:
      return L"撤回最近提交";
    case HistoryFlow::fetch:
      return L"抓取";
    case HistoryFlow::pull:
      return L"拉取";
    case HistoryFlow::push:
      return L"推送";
    case HistoryFlow::firstPush:
      return L"首次推送";
    case HistoryFlow::conflictContinue:
      return L"冲突流程继续";
    case HistoryFlow::conflictAbort:
      return L"冲突流程中止";
  }
  return L"未知操作";
}

std::wstring_view HistoryTerminalLabel(HistoryTerminal terminal) noexcept {
  switch (terminal) {
    case HistoryTerminal::inProgress:
      return L"已启动，未见结果";
    case HistoryTerminal::succeeded:
      return L"成功";
    case HistoryTerminal::failed:
      return L"失败";
    case HistoryTerminal::canceled:
      return L"已取消（未启动、未改动仓库）";
    case HistoryTerminal::launchFailed:
      return L"启动失败（未运行任何 Git 命令）";
    case HistoryTerminal::gitNotStarted:
      return L"Git 进程未创建";
    case HistoryTerminal::unknown:
      return L"结果未知";
  }
  return L"结果未知";
}

std::wstring_view HistoryRestoreKindLabel(HistoryRestoreKind kind) noexcept {
  switch (kind) {
    case HistoryRestoreKind::none:
      return L"不产生引用级恢复";
    case HistoryRestoreKind::refMove:
      return L"可用本地引用回退（需重新核实并确认）";
    case HistoryRestoreKind::manualRemote:
      return L"涉及远端历史：仅说明与复制命令，不自动执行";
  }
  return L"不产生引用级恢复";
}

bool IsSafeHistoryId(std::wstring_view id) {
  if (id.empty() || id.size() > kMaxHistoryIdLength) {
    return false;
  }
  for (const wchar_t c : id) {
    const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                    c == L'_' || c == L'-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::wstring SanitizeHistoryText(std::wstring_view text) {
  // 1) 剔除控制字符（含 NUL/CR/LF/TAB 与其它 <0x20 及 0x7F）：一行一条记录，摘要文本不需要换行，
  //    控制字符会破坏记录行形态，也不该被 URL 掩码逻辑绕过。
  std::wstring stripped;
  stripped.reserve(text.size());
  for (const wchar_t c : text) {
    if (c < 0x20 || c == 0x7F) {
      continue;
    }
    stripped.push_back(c);
  }
  // 2) 掩码任何 scheme://…@… 形态里内嵌的凭据（与推送链路同一份实现，避免两处口径漂移）。
  std::wstring masked = git::MaskPushUrlCredentialsInText(stripped);
  // 3) 限长：截断只影响一条元数据摘要的可读尾巴，绝不影响「恢复哪一条」这类结构化字段。
  if (masked.size() > kMaxHistoryFieldChars) {
    masked.resize(kMaxHistoryFieldChars);
  }
  return masked;
}

bool AppendHistoryRecord(OperationLog* log, OperationRecord record, std::wstring* refusal) {
  if (log == nullptr) {
    return false;
  }
  if (!IsSafeHistoryId(record.id)) {
    if (refusal != nullptr) {
      *refusal = L"这条历史没有记录：操作 ID 不是本程序会写出的形态（只允许 ASCII 字母数字与 -_）。";
    }
    return false;
  }
  if (record.repositoryKey.empty() ||
      (!record.workTreeRoot.empty() && git::CanonicalPathKey(record.workTreeRoot) != record.repositoryKey)) {
    if (refusal != nullptr) {
      *refusal = L"这条历史没有记录：仓库工作区身份与规范键对不上。";
    }
    return false;
  }
  for (const std::wstring* oid : {&record.sourceObjectId, &record.targetObjectId,
                                  &record.restoreUndoToObjectId, &record.restoreExpectedCurrentId}) {
    if (!OptionalObjectIdOk(*oid)) {
      if (refusal != nullptr) {
        *refusal = L"这条历史没有记录：某个对象 ID 不是完整的十六进制对象 ID。";
      }
      return false;
    }
  }
  for (const HistoryTargetCheck& check : record.targetChecks) {
    if (!OptionalObjectIdOk(check.remoteObjectId)) {
      if (refusal != nullptr) {
        *refusal = L"这条历史没有记录：某个发布目标的对象 ID 不成立。";
      }
      return false;
    }
  }
  // 落盘前统一脱敏与限长的自由文本字段；对象 ID 与规范键保持原样（已单独校验）。
  const auto clean = [](std::wstring& field) { field = SanitizeHistoryText(field); };
  clean(record.workTreeRoot);
  clean(record.operationLabel);
  clean(record.sourceRef);
  clean(record.targetRef);
  clean(record.remoteName);
  clean(record.completionLabel);
  clean(record.outcomeNote);
  clean(record.restoreBranchRef);
  clean(record.restoreNote);
  for (HistoryTargetCheck& check : record.targetChecks) {
    clean(check.maskedUrl);
    clean(check.failure);
  }
  for (HistoryReview& review : record.reviews) {
    review.text = SanitizeHistoryText(review.text);
  }
  if (record.targetChecks.size() > kMaxHistoryChecksPerRecord) {
    record.targetChecks.resize(kMaxHistoryChecksPerRecord);
  }
  if (record.reviews.size() > kMaxHistoryReviewsPerRecord) {
    record.reviews.erase(record.reviews.begin(),
                         record.reviews.begin() + static_cast<long long>(
                                                       record.reviews.size() - kMaxHistoryReviewsPerRecord));
  }

  for (OperationRecord& existing : log->records) {
    if (existing.id != record.id) {
      continue;
    }
    AbsorbIntoExisting(existing, std::move(record));
    return true;
  }
  log->records.push_back(std::move(record));
  TrimToMaxRecords(*log);
  return true;
}

bool AppendHistoryReview(OperationLog* log, std::wstring_view id, long long epoch, std::wstring_view text) {
  if (log == nullptr || id.empty()) {
    return false;
  }
  for (OperationRecord& record : log->records) {
    if (record.id != id) {
      continue;
    }
    HistoryReview review;
    review.epoch = epoch < 0 ? 0 : epoch;
    review.text = SanitizeHistoryText(text);
    const bool dup = std::any_of(record.reviews.begin(), record.reviews.end(),
                                 [&](const HistoryReview& e) {
                                   return e.epoch == review.epoch && e.text == review.text;
                                 });
    if (dup) {
      return true;  // 同一次复核重复登记：视作已存在，不制造两条。
    }
    record.reviews.push_back(std::move(review));
    if (record.reviews.size() > kMaxHistoryReviewsPerRecord) {
      record.reviews.erase(record.reviews.begin(),
                           record.reviews.begin() + static_cast<long long>(
                                                         record.reviews.size() - kMaxHistoryReviewsPerRecord));
    }
    return true;
  }
  return false;
}

void ApplyHistoryRetention(OperationLog* log, long long nowEpoch) {
  if (log == nullptr) {
    return;
  }
  constexpr long long kSecondsPerDay = 24LL * 60 * 60;  // 由时间单位换算表达阈值，不手乘 tick
  if (log->retentionDays > 0 && nowEpoch > 0) {
    const long long cutoff = nowEpoch - static_cast<long long>(log->retentionDays) * kSecondsPerDay;
    std::erase_if(log->records, [&](const OperationRecord& record) {
      // 时刻未知（0）的保留：宁可多留一条也别把「说不清什么时候」的记录当成过期丢掉。
      return record.startedEpoch != 0 && record.startedEpoch < cutoff;
    });
  }
  TrimToMaxRecords(*log);
}

std::wstring DescribeHistoryStoragePath(std::wstring_view directory, std::wstring_view fileName) {
  if (directory.empty()) {
    return std::wstring(L"（当前环境取不到应用数据目录）");
  }
  std::wstring path(directory);
  if (path.back() != L'\\' && path.back() != L'/') {
    path.push_back(L'\\');
  }
  path += fileName;
  return path;
}

std::wstring SerializeOperationLog(const OperationLog& log) {
  const auto flag = [](bool b) { return std::to_wstring(b ? 1 : 0); };
  std::wstring out;
  out += kMagicName;
  out += L'|';
  out += std::to_wstring(log.formatVersion);
  out.push_back(L'\n');
  out += kMetaName;
  out += L'|';
  out += flag(log.historyEnabled) + L"," + std::to_wstring(log.retentionDays < 0 ? 0 : log.retentionDays) +
         L"," + std::to_wstring(log.maxRecords);
  out.push_back(L'\n');
  for (const OperationRecord& record : log.records) {
    std::wstring line(kRecordName);
    line += L'|' + EscapeField(record.id);
    line += L'|' + std::to_wstring(record.startedEpoch < 0 ? 0 : record.startedEpoch) + L"," +
            std::to_wstring(record.terminalEpoch < 0 ? 0 : record.terminalEpoch);
    line += L'|' + std::to_wstring(static_cast<long long>(record.flow)) + L"," +
            std::to_wstring(static_cast<long long>(record.terminal)) + L"," + flag(record.exitCodeKnown) +
            L"," + std::to_wstring(record.exitCode);
    line += L'|' + EscapeField(record.workTreeRoot);
    line += L'|' + EscapeField(record.repositoryKey);
    line += L'|' + EscapeField(record.operationLabel);
    line += L'|' + EscapeField(record.sourceRef);
    line += L'|' + EscapeField(record.sourceObjectId);
    line += L'|' + EscapeField(record.targetRef);
    line += L'|' + EscapeField(record.targetObjectId);
    line += L'|' + EscapeField(record.remoteName);
    line += L'|' + EscapeField(record.completionLabel);
    line += L'|' + EscapeField(record.outcomeNote);
    line += L'|' + std::to_wstring(static_cast<long long>(record.restoreKind)) + L"," +
            flag(record.restoreIsRoot);
    line += L'|' + EscapeField(record.restoreBranchRef);
    line += L'|' + EscapeField(record.restoreUndoToObjectId);
    line += L'|' + EscapeField(record.restoreExpectedCurrentId);
    line += L'|' + EscapeField(record.restoreNote);
    out += line;
    out.push_back(L'\n');
    for (const HistoryTargetCheck& check : record.targetChecks) {
      std::wstring cline(kCheckName);
      cline += L'|' + EscapeField(record.id);
      cline += L'|' + EscapeField(check.maskedUrl);
      cline += L'|' + flag(check.queried) + L"," + flag(check.ok) + L"," + flag(check.refPresent);
      cline += L'|' + EscapeField(check.remoteObjectId);
      cline += L'|' + EscapeField(check.failure);
      out += cline;
      out.push_back(L'\n');
    }
    for (const HistoryReview& review : record.reviews) {
      std::wstring rline(kReviewName);
      rline += L'|' + EscapeField(record.id);
      rline += L'|' + std::to_wstring(review.epoch < 0 ? 0 : review.epoch);
      rline += L'|' + EscapeField(review.text);
      out += rline;
      out.push_back(L'\n');
    }
  }
  return out;
}

HistoryLoadResult ParseOperationLog(std::wstring_view text) {
  HistoryLoadResult result;
  const auto allBlank = std::all_of(text.begin(), text.end(), [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r';
  });
  if (allBlank) {
    result.status = HistoryLoadStatus::empty;
    result.log = OperationLog{};
    return result;
  }
  if (text.find(L'\r') != std::wstring_view::npos) {
    result.status = HistoryLoadStatus::corrupt;
    result.reason = L"记录里出现裸的回车符，与写入约定不符";
    return result;
  }
  if (text.find(L'\0') != std::wstring_view::npos) {
    result.status = HistoryLoadStatus::corrupt;
    result.reason = L"记录里出现 NUL";
    return result;
  }

  std::vector<std::wstring_view> lines;
  {
    size_t start = 0;
    for (;;) {
      const size_t newline = text.find(L'\n', start);
      lines.push_back(text.substr(start, newline == std::wstring_view::npos ? std::wstring_view::npos
                                                                            : newline - start));
      if (newline == std::wstring_view::npos) {
        break;
      }
      start = newline + 1;
    }
    if (!lines.empty() && lines.back().empty()) {
      lines.pop_back();  // 序列化总在末条记录后补 '\n'，允许末尾这一个空行。
    }
  }
  if (lines.empty()) {
    result.status = HistoryLoadStatus::empty;
    result.log = OperationLog{};
    return result;
  }

  const auto corrupt = [&result](size_t lineIndex, std::wstring_view why) {
    result.status = HistoryLoadStatus::corrupt;
    result.reason = L"第 " + std::to_wstring(lineIndex + 1) + L" 行不成立：" + std::wstring(why);
    return result;
  };

  // 首行必须是版本头。历史没有「未分版本老文件」这种来源：认不出版本头就是坏文件。
  {
    const std::vector<std::wstring_view> head = SplitPipes(lines[0]);
    if (head.size() != 2 || head[0] != kMagicName) {
      return corrupt(0, L"缺少 evernightcommit.history 版本头");
    }
    long long version = 0;
    if (!ParseNonNegative(head[1], &version) || version > 4294967296LL) {
      return corrupt(0, L"版本号不成立");
    }
    result.detectedVersion = static_cast<int>(version);
    if (version > kHistoryFormatVersion) {
      result.status = HistoryLoadStatus::tooNew;
      result.reason = L"文件由更新版本的程序写出（版本 " + std::to_wstring(version) + L"）";
      return result;
    }
    if (version != kHistoryFormatVersion) {
      return corrupt(0, L"版本号与本程序要写的版本不一致");
    }
  }

  OperationLog log;
  log.formatVersion = kHistoryFormatVersion;
  bool hasMeta = false;
  std::unordered_map<std::wstring, size_t> indexById;

  for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex) {
    const std::wstring_view line = lines[lineIndex];
    if (line.empty()) {
      return corrupt(lineIndex, L"记录文件里没有空行这种东西");
    }
    const std::vector<std::wstring_view> parts = SplitPipes(line);
    const std::wstring_view name = parts[0];
    std::wstring decoded;

    if (name == kMetaName) {
      if (parts.size() != 2 || hasMeta || !UnescapeField(parts[1], &decoded)) {
        return corrupt(lineIndex, L"策略记录形态不对");
      }
      // "enabled,retentionDays,maxRecords"
      size_t cursor = 0;
      const size_t c1 = decoded.find(L',', cursor);
      if (c1 == std::wstring_view::npos) {
        return corrupt(lineIndex, L"策略记录缺字段");
      }
      bool enabled = false;
      if (!ParseFlag(std::wstring_view(decoded).substr(cursor, c1 - cursor), &enabled)) {
        return corrupt(lineIndex, L"历史开关取值不成立");
      }
      cursor = c1 + 1;
      const size_t c2 = decoded.find(L',', cursor);
      if (c2 == std::wstring_view::npos) {
        return corrupt(lineIndex, L"策略记录缺保留字段");
      }
      long long retention = 0;
      if (!ParseNonNegative(std::wstring_view(decoded).substr(cursor, c2 - cursor), &retention) ||
          retention > kMaxHistoryRetentionDays) {
        return corrupt(lineIndex, L"保留天数不成立或超出上限");
      }
      long long maxRecords = 0;
      if (!ParseNonNegative(std::wstring_view(decoded).substr(c2 + 1), &maxRecords) || maxRecords < 1 ||
          maxRecords > static_cast<long long>(kMaxHistoryRecords)) {
        return corrupt(lineIndex, L"条数上限不成立或超出上限");
      }
      hasMeta = true;
      log.historyEnabled = enabled;
      log.retentionDays = static_cast<int>(retention);
      log.maxRecords = static_cast<size_t>(maxRecords);
      continue;
    }

    if (name == kRecordName) {
      if (parts.size() != 19) {
        return corrupt(lineIndex, L"操作记录字段数量不对");
      }
      OperationRecord record;
      if (!UnescapeField(parts[1], &record.id) || !IsSafeHistoryId(record.id)) {
        return corrupt(lineIndex, L"操作 ID 不成立");
      }
      if (indexById.find(record.id) != indexById.end()) {
        return corrupt(lineIndex, L"同一个操作 ID 出现了两次");
      }
      // startedEpoch,terminalEpoch
      {
        const std::wstring_view stamps = parts[2];
        const size_t comma = stamps.find(L',');
        if (comma == std::wstring_view::npos || stamps.find(L',', comma + 1) != std::wstring_view::npos ||
            !ParseNonNegative(stamps.substr(0, comma), &record.startedEpoch) ||
            !ParseNonNegative(stamps.substr(comma + 1), &record.terminalEpoch)) {
          return corrupt(lineIndex, L"时间戳不成立");
        }
      }
      // flow,terminal,exitKnown,exitCode
      {
        std::vector<std::wstring_view> segs;
        size_t cursor = 0;
        const std::wstring_view s = parts[3];
        for (;;) {
          const size_t comma = s.find(L',', cursor);
          segs.push_back(comma == std::wstring_view::npos ? s.substr(cursor) : s.substr(cursor, comma - cursor));
          if (comma == std::wstring_view::npos) {
            break;
          }
          cursor = comma + 1;
        }
        if (segs.size() != 4) {
          return corrupt(lineIndex, L"状态段字段数不对");
        }
        long long flowValue = -1;
        long long terminalValue = -1;
        if (!ParseNonNegative(segs[0], &flowValue) || !FlowInRange(flowValue) ||
            !ParseNonNegative(segs[1], &terminalValue) || !TerminalInRange(terminalValue) ||
            !ParseFlag(segs[2], &record.exitCodeKnown) || !ParseSigned(segs[3], &record.exitCode)) {
          return corrupt(lineIndex, L"操作类型或终态取值不成立");
        }
        record.flow = static_cast<HistoryFlow>(flowValue);
        record.terminal = static_cast<HistoryTerminal>(terminalValue);
      }
      if (!UnescapeField(parts[4], &record.workTreeRoot) ||
          !UnescapeField(parts[5], &record.repositoryKey) ||
          !UnescapeField(parts[6], &record.operationLabel) ||
          !UnescapeField(parts[7], &record.sourceRef) ||
          !UnescapeField(parts[8], &record.sourceObjectId) ||
          !UnescapeField(parts[9], &record.targetRef) ||
          !UnescapeField(parts[10], &record.targetObjectId) ||
          !UnescapeField(parts[11], &record.remoteName) ||
          !UnescapeField(parts[12], &record.completionLabel) ||
          !UnescapeField(parts[13], &record.outcomeNote)) {
        return corrupt(lineIndex, L"记录里有不成立的转义");
      }
      if (record.repositoryKey.empty() ||
          (!record.workTreeRoot.empty() &&
           git::CanonicalPathKey(record.workTreeRoot) != record.repositoryKey)) {
        return corrupt(lineIndex, L"仓库工作区身份与规范键对不上");
      }
      if (!OptionalObjectIdOk(record.sourceObjectId) || !OptionalObjectIdOk(record.targetObjectId)) {
        return corrupt(lineIndex, L"某个对象 ID 不是完整的十六进制对象 ID");
      }
      // restoreKind,restoreIsRoot
      {
        long long restoreValue = -1;
        bool isRoot = false;
        std::wstring_view s = parts[14];
        const size_t comma = s.find(L',');
        if (comma == std::wstring_view::npos) {
          return corrupt(lineIndex, L"恢复线索形态不对");
        }
        if (!ParseNonNegative(s.substr(0, comma), &restoreValue) || !RestoreKindInRange(restoreValue) ||
            !ParseFlag(s.substr(comma + 1), &isRoot) || s.find(L',', comma + 1) != std::wstring_view::npos) {
          return corrupt(lineIndex, L"恢复线索取值不成立");
        }
        record.restoreKind = static_cast<HistoryRestoreKind>(restoreValue);
        record.restoreIsRoot = isRoot;
      }
      if (!UnescapeField(parts[15], &record.restoreBranchRef) ||
          !UnescapeField(parts[16], &record.restoreUndoToObjectId) ||
          !UnescapeField(parts[17], &record.restoreExpectedCurrentId) ||
          !UnescapeField(parts[18], &record.restoreNote)) {
        return corrupt(lineIndex, L"恢复线索里有不成立的转义");
      }
      if (!OptionalObjectIdOk(record.restoreUndoToObjectId) ||
          !OptionalObjectIdOk(record.restoreExpectedCurrentId)) {
        return corrupt(lineIndex, L"恢复线索里的对象 ID 不成立");
      }
      indexById[record.id] = log.records.size();
      log.records.push_back(std::move(record));
      continue;
    }

    if (name == kCheckName || name == kReviewName) {
      continue;  // 附属行在第二趟挂到主记录（主记录按序列化总在前面，但损坏文件可能乱序，故两趟）。
    }

    return corrupt(lineIndex, L"本程序不认识这条记录");
  }

  // 第二趟：把 check / review 挂到主记录（主记录按序列化总在前面；损坏文件也要求成对，故分两趟）。
  for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex) {
    const std::vector<std::wstring_view> parts = SplitPipes(lines[lineIndex]);
    if (parts.empty()) {
      continue;
    }
    const std::wstring_view name = parts[0];
    if (name != kCheckName && name != kReviewName) {
      continue;
    }
    std::wstring id;
    if (parts.size() < 2 || !UnescapeField(parts[1], &id)) {
      return corrupt(lineIndex, L"附属行缺主记录 ID");
    }
    const auto found = indexById.find(id);
    if (found == indexById.end()) {
      return corrupt(lineIndex, L"附属行指向一条不存在的主记录（半条历史，绝不采信）");
    }
    OperationRecord& record = log.records[found->second];
    if (name == kCheckName) {
      if (parts.size() != 6) {
        return corrupt(lineIndex, L"逐目标记录字段数量不对");
      }
      HistoryTargetCheck check;
      if (!UnescapeField(parts[2], &check.maskedUrl)) {
        return corrupt(lineIndex, L"逐目标记录里有不成立的转义");
      }
      if (!ParseFlags3(parts[3], &check.queried, &check.ok, &check.refPresent)) {
        return corrupt(lineIndex, L"逐目标三态取值不成立");
      }
      if (!UnescapeField(parts[4], &check.remoteObjectId) || !UnescapeField(parts[5], &check.failure)) {
        return corrupt(lineIndex, L"逐目标记录里有不成立的转义");
      }
      if (!OptionalObjectIdOk(check.remoteObjectId)) {
        return corrupt(lineIndex, L"逐目标对象 ID 不成立");
      }
      if (record.targetChecks.size() >= kMaxHistoryChecksPerRecord) {
        return corrupt(lineIndex, L"一条记录挂了过多逐目标核实");
      }
      record.targetChecks.push_back(std::move(check));
    } else {
      if (parts.size() != 4) {
        return corrupt(lineIndex, L"复核记录字段数量不对");
      }
      HistoryReview review;
      if (!ParseNonNegative(parts[2], &review.epoch)) {
        return corrupt(lineIndex, L"复核时刻不是非负整数");
      }
      if (!UnescapeField(parts[3], &review.text)) {
        return corrupt(lineIndex, L"复核记录里有不成立的转义");
      }
      if (record.reviews.size() >= kMaxHistoryReviewsPerRecord) {
        return corrupt(lineIndex, L"一条记录挂了过多复核");
      }
      record.reviews.push_back(std::move(review));
    }
  }

  result.status = HistoryLoadStatus::loaded;
  result.log = std::move(log);
  return result;
}

OperationLog MergeHistoryForWrite(const OperationLog& disk, const OperationLog& ours,
                                  const HistoryWriteIntents& intents, long long nowEpoch,
                                  HistoryMergeReport* report) {
  if (intents.replaceAll) {
    // 「清除历史」：记录整份以本窗口为准（此刻本窗口就是空的），策略也取本窗口（用户刚点的）。
    OperationLog cleared = ours;
    cleared.formatVersion = kHistoryFormatVersion;
    cleared.records.clear();
    return cleared;
  }
  OperationLog merged = disk;
  merged.formatVersion = kHistoryFormatVersion;
  if (intents.policy) {
    merged.historyEnabled = ours.historyEnabled;
    merged.retentionDays = ours.retentionDays;
    merged.maxRecords = ours.maxRecords;
  }
  if (intents.append) {
    std::unordered_map<std::wstring, size_t> indexById;
    indexById.reserve(merged.records.size() + ours.records.size());
    for (size_t i = 0; i < merged.records.size(); ++i) {
      indexById[merged.records[i].id] = i;
    }
    for (OperationRecord incoming : ours.records) {
      const auto found = indexById.find(incoming.id);
      if (found == indexById.end()) {
        indexById[incoming.id] = merged.records.size();
        merged.records.push_back(std::move(incoming));
        continue;
      }
      OperationRecord& existing = merged.records[found->second];
      // 盘上已确定、本窗口这条更弱（例如重启后又观察到一次 inProgress）：保留盘上那份并报告。
      const bool diskAlreadySettled = TerminalRank(existing.terminal) > TerminalRank(incoming.terminal);
      if (diskAlreadySettled && report != nullptr) {
        report->keptFromDiskStrongerTerminal.push_back(existing.id);
      }
      AbsorbIntoExisting(existing, std::move(incoming));
    }
    // 时间顺序：按启动时刻稳定排序（同刻保持既有相对顺序，未知时刻排在前面以免被当成最新）。
    std::stable_sort(merged.records.begin(), merged.records.end(),
                     [](const OperationRecord& left, const OperationRecord& right) {
                       return left.startedEpoch < right.startedEpoch;
                     });
  }
  ApplyHistoryRetention(&merged, nowEpoch);
  return merged;
}

namespace {
HistoryTerminal MapOutcome(HistoryOutcome outcome) {
  switch (outcome) {
    case HistoryOutcome::succeeded:
      return HistoryTerminal::succeeded;
    case HistoryOutcome::failed:
      return HistoryTerminal::failed;
    case HistoryOutcome::unknown:
      return HistoryTerminal::unknown;
    case HistoryOutcome::launchFailed:
      return HistoryTerminal::launchFailed;
    case HistoryOutcome::gitNotStarted:
      return HistoryTerminal::gitNotStarted;
  }
  return HistoryTerminal::unknown;
}
}  // namespace

OperationRecord ComposeHistoryRecord(std::wstring id, std::wstring workTreeRoot,
                                     const HistoryCapture& capture,
                                     const HistoryTerminalInfo& terminal) {
  OperationRecord record;
  record.id = std::move(id);
  // 键用原始工作区根算，展示根用脱敏后的；正常路径下脱敏不改动它，两者自然一致。
  record.repositoryKey = git::CanonicalPathKey(workTreeRoot);
  record.workTreeRoot = SanitizeHistoryText(workTreeRoot);
  record.flow = capture.flow;
  record.operationLabel = SanitizeHistoryText(capture.operationLabel);

  record.sourceRef = SanitizeHistoryText(capture.sourceRef);
  record.sourceObjectId = capture.sourceObjectId;  // 来自已确认方案，交给 Append 校验形态
  record.targetRef = SanitizeHistoryText(capture.targetRef);
  record.targetObjectId = capture.targetObjectId;
  record.remoteName = SanitizeHistoryText(capture.remoteName);

  // 发布 URL 一律掩码后作为「逐目标」占位落账；命令窗口那刻还没核实，queried 一律 false，
  // 真正的送达/未核实/不符由推送控制器在核实回来后补进这条记录。
  for (const std::wstring& url : capture.publishUrls) {
    HistoryTargetCheck check;
    check.maskedUrl = SanitizeHistoryText(url);
    check.queried = false;
    record.targetChecks.push_back(std::move(check));
  }

  record.terminal = MapOutcome(terminal.outcome);
  record.startedEpoch = terminal.startedEpoch < 0 ? 0 : terminal.startedEpoch;
  // inProgress 之外的都是「有终态」：终态时刻照带；unknown 也算已观察到的一次终态结论。
  record.terminalEpoch = terminal.terminalEpoch < 0 ? 0 : terminal.terminalEpoch;
  record.exitCodeKnown = terminal.exitCodeKnown;
  record.exitCode = terminal.exitCode;
  record.completionLabel = SanitizeHistoryText(terminal.completionLabel);
  record.outcomeNote = SanitizeHistoryText(terminal.conclusion);

  record.restoreKind = capture.restoreKind;
  record.restoreBranchRef = SanitizeHistoryText(capture.restoreBranchRef);
  record.restoreUndoToObjectId = capture.restoreUndoToObjectId;
  record.restoreExpectedCurrentId = capture.restoreExpectedCurrentId;
  record.restoreIsRoot = capture.restoreIsRoot;
  record.restoreNote = SanitizeHistoryText(capture.restoreNote);
  return record;
}

}  // namespace gc::app
