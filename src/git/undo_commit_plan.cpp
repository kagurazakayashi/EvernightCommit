#include "git/undo_commit_plan.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/commit_history.h"

namespace gc::git {
namespace {

// 远端引用列表在确认框里最多列几条；其余折成「等 N 个」。
constexpr size_t kMaxPublishRefList = 5;

// 进 reflog 的说明：纯 ASCII、不含空格与引号，既是一条命令行参数就一个 token，
// 也让确认框里逐字拼出来的命令行没有歧义。
constexpr std::wstring_view kReflogReason = L"EvernightCommit:undo-last-commit";

// 提交对象头部里「parent <完整ID>」这一行的前缀。
constexpr std::wstring_view kParentHeader = L"parent ";

std::wstring LowerAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
  }
  return result;
}

bool HasPrefix(std::wstring_view text, std::wstring_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// 短 ID 与完整 ID 的对照：Git 的 --short 长度随仓库增长，界面摘要里的短 ID 未必正好
// 是 kShortObjectIdLength 个字符，因此「相等或一方是另一方的前缀」都算同一个提交；
// 任何一边为空则视为不可比（不参与「现状已变」的判定）。
bool ShaDisplaysMatch(std::wstring_view left, std::wstring_view right) {
  if (left.empty() || right.empty()) {
    return true;
  }
  const std::wstring a = LowerAscii(left);
  const std::wstring b = LowerAscii(right);
  if (a.size() <= b.size()) {
    return b.compare(0, a.size(), a) == 0;
  }
  return a.compare(0, b.size(), b) == 0;
}

std::wstring RefusalWith(const UndoQueryRead& read, std::wstring_view fallback) {
  if (!read.detail.empty()) {
    return std::wstring(read.detail);
  }
  return std::wstring(fallback);
}

// 列表里每个 ID 都以短 ID 展示，供拒绝说明点名（最多 4 个，多的折成「等」）。
std::wstring ListIds(const std::vector<std::wstring>& ids) {
  constexpr size_t kMaxListed = 4;
  std::wstring listed;
  for (size_t index = 0; index < std::min(ids.size(), kMaxListed); ++index) {
    if (!listed.empty()) {
      listed += L"、";
    }
    listed += ShortObjectId(ids[index]);
  }
  if (ids.size() > kMaxListed) {
    listed += L" 等 " + std::to_wstring(ids.size()) + L" 个";
  }
  return listed;
}

// rev-list --parents 的一行：「自身ID 父1 父2 …」。按空白拆 token，逐个校验完整对象 ID。
bool ParseParentsLine(std::wstring_view line, std::wstring* selfId, std::vector<std::wstring>* parents,
                      std::wstring* failure) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (const wchar_t c : line) {
    if (c == L' ' || c == L'\t') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  if (tokens.empty()) {
    if (failure != nullptr) {
      *failure = L"rev-list --parents 没有给出任何内容，无法确定父提交。";
    }
    return false;
  }
  if (!LooksLikeFullObjectId(tokens.front())) {
    if (failure != nullptr) {
      *failure = L"rev-list --parents 的自身 ID 不符合完整对象 ID 的约定。";
    }
    return false;
  }
  for (size_t index = 1; index < tokens.size(); ++index) {
    if (!LooksLikeFullObjectId(tokens[index])) {
      if (failure != nullptr) {
        *failure = L"rev-list --parents 输出里有一个父提交 ID 形态不合格，无法安全撤回。";
      }
      return false;
    }
  }
  if (selfId != nullptr) {
    *selfId = tokens.front();
  }
  if (parents != nullptr) {
    parents->assign(tokens.begin() + 1, tokens.end());
  }
  return true;
}

// 提交对象的原始文本：头部一行一个字段，第一个空行之后是正文。
// 只取头部里的 parent 行——正文里完全可能出现「parent 」开头的句子，因此空行必须划清界限；
// 字段续行以空格开头（gpgsig 就是这种形态），所以前缀一律按原始行判断，不先修剪空白。
// 顺手核对有没有 tree 行：没有就说明这份输出根本不是一个提交对象的头部，不能用。
bool ParseRecordedParents(std::wstring_view objectText, std::vector<std::wstring>* parents,
                          std::wstring* failure) {
  parents->clear();
  bool sawTree = false;
  bool headerEnded = false;
  for (std::wstring_view rawLine : SplitLines(objectText)) {
    if (rawLine.empty()) {
      headerEnded = true;
      break;
    }
    if (HasPrefix(rawLine, L"tree ")) {
      sawTree = true;
      continue;
    }
    if (HasPrefix(rawLine, kParentHeader)) {
      const std::wstring value = TrimWide(rawLine.substr(kParentHeader.size()));
      if (!LooksLikeFullObjectId(value)) {
        *failure = L"提交对象里有一条 parent 行不是完整对象 ID，无法确定父提交。";
        return false;
      }
      parents->push_back(value);
    }
  }
  if (!headerEnded) {
    *failure = L"提交对象的输出里没有空行分隔头部与正文，形态不合约定。";
    return false;
  }
  if (!sawTree) {
    *failure = L"提交对象的头部里没有 tree 行，它不是一个可采信的提交对象。";
    return false;
  }
  return true;
}

std::wstring PublishEvidenceSentence(const UndoPreflightFacts& facts) {
  switch (facts.publish) {
    case UndoPublishEvidence::contained: {
      std::wstring listed;
      for (size_t index = 0; index < std::min(facts.containingRemoteRefs.size(), kMaxPublishRefList);
           ++index) {
        if (!listed.empty()) {
          listed += L"、";
        }
        listed += facts.containingRemoteRefs[index];
      }
      if (facts.containingRemoteRefs.size() > kMaxPublishRefList) {
        listed += L" 等 " + std::to_wstring(facts.containingRemoteRefs.size()) + L" 处";
      }
      return L"发布状态：已知已发布。本地远端跟踪引用显示该提交已被 " + listed +
             L" 包含——它很可能已经推送过。其他协作者可能已经基于它工作，撤回会在你们之间造成"
             L"历史分叉。本程序只会移动本机的分支引用，不会（也无法自动）改动远端。";
    }
    case UndoPublishEvidence::notFound:
      return L"发布状态：本地信息未发现已发布。本地的远端跟踪引用（截至最近一次 fetch；本程序没有"
             L"联网核对，这些引用可能已过期）中没有任何一个包含该提交。注意：这不能证明它从未被"
             L"推送过——也可能只是还没有 fetch 下来。";
    case UndoPublishEvidence::noRemoteRefs:
      return L"发布状态：无法判断。这个仓库本地没有任何远端跟踪引用（refs/remotes 为空），"
             L"可能从未推送，也可能只是从未 fetch；本程序不联网核对，不能保证它从未 push。";
    case UndoPublishEvidence::queryFailed:
      return L"发布状态：无法判断。询问远端跟踪引用时 Git 没能给出可采信的回答（" +
             (facts.publishFailureDetail.empty() ? std::wstring(L"原因未知")
                                                 : facts.publishFailureDetail) +
             L"）。判断不了就当作有已发布的风险处理。";
    case UndoPublishEvidence::notRun:
    default:
      return L"发布状态：未能查询。";
  }
}

// 撤回目标的分支引用必须是一个完整、可用的引用名：Git 的 update-ref 按名字操作，
// 名字含糊（相对名、HEAD、带空白或控制字符）就等于把目标交给一次二次解析，
// 而「用户确认的那个分支」必须是一个不再变化的名字。
// Git 自己还会按 refname 规则再核一次（非法名以非 0 拒绝），这里只把明显不能用的挡在边界上。
bool RefnameShaped(std::wstring_view branchRef) {
  if (!HasPrefix(branchRef, L"refs/") || branchRef.size() <= std::wstring_view(L"refs/").size()) {
    return false;  // 必须是 refs/ 下面的完整引用名，且「refs/」本身不算
  }
  if (branchRef.back() == L'/') {
    return false;
  }
  if (branchRef.find(L"..") != std::wstring_view::npos ||
      branchRef.find(L"@{") != std::wstring_view::npos) {
    return false;
  }
  for (const wchar_t c : branchRef) {
    if (c <= 0x20 || c == 0x7F || c == L'"' || c == L'\'' || c == L'`' || c == L'\\') {
      return false;  // 空白、控制字符与引号类字符都不进命令行
    }
  }
  return true;
}

}  // namespace

bool IsSafeUndoTargetRef(std::wstring_view branchRef) { return RefnameShaped(branchRef); }

std::vector<std::wstring> BuildUndoSymbolicRefArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"symbolic-ref", L"--quiet", L"HEAD"};
}

std::vector<std::wstring> BuildUndoHeadCommitArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   L"HEAD"};
}

std::vector<std::wstring> BuildUndoParentsArguments(std::wstring_view repositoryDirectory,
                                                    std::wstring_view headSha) {
  if (!LooksLikeFullObjectId(headSha)) {
    return {};  // 不合格的 ID 根本不配送进 Git；调用方据此跳过这条查询。
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-list", L"--parents", L"-n", L"1",
                                   std::wstring(headSha)};
}

std::vector<std::wstring> BuildUndoHeadSummaryArguments(std::wstring_view repositoryDirectory,
                                                        std::wstring_view headSha) {
  if (!LooksLikeFullObjectId(headSha)) {
    return {};
  }
  // 与 git/commit_history 同样的防护：--no-decorate 关掉装饰、log.showSignature=false
  // 覆盖用户配置，否则 %s 前后的杂项会污染这一行展示文本。
  return std::vector<std::wstring>{L"-C",    std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"-c", L"log.showSignature=false", L"log",
                                   L"-1",    L"--no-decorate", L"--format=%s",
                                   std::wstring(headSha)};
}

std::vector<std::wstring> BuildUndoCommitObjectArguments(std::wstring_view repositoryDirectory,
                                                         std::wstring_view headSha) {
  if (!LooksLikeFullObjectId(headSha)) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"cat-file", L"commit",
                                   std::wstring(headSha)};
}

std::vector<std::wstring> BuildUndoShallowStateArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"rev-parse", L"--is-shallow-repository"};
}

std::vector<std::wstring> BuildUndoParentObjectArguments(std::wstring_view repositoryDirectory,
                                                         std::wstring_view parentSha) {
  if (!LooksLikeFullObjectId(parentSha)) {
    return {};
  }
  // --quiet：对象不在本地时以退出码 1 + 空输出作答，这是一个明确答案（历史不完整），
  // 而不是「查询失败」；类型不是提交则是另一回事，判读层按不一致处理。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"cat-file", L"-t", L"--quiet",
                                   std::wstring(parentSha)};
}

std::vector<std::wstring> BuildUndoRemoteRefsArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"for-each-ref", L"--format=%(refname)", L"refs/remotes"};
}

std::vector<std::wstring> BuildUndoRemoteContainsArguments(std::wstring_view repositoryDirectory,
                                                           std::wstring_view headSha) {
  if (!LooksLikeFullObjectId(headSha)) {
    return {};  // 不合格的 ID 根本不配送进 Git；调用方据此跳过这条查询。
  }
  return std::vector<std::wstring>{L"-C",       std::wstring(repositoryDirectory),
                                   L"--no-optional-locks", L"--no-replace-objects", L"for-each-ref",
                                   L"--contains", std::wstring(headSha), L"--format=%(refname)",
                                   L"refs/remotes"};
}

UndoQueryRead ReadUndoQuery(const GitQueryResult& result) {
  UndoQueryRead read;
  if (!result.started) {
    read.error = RepoError::gitLaunchFailed;
    read.detail = result.launchDetail.empty()
                      ? L"Git 查询进程未能启动"
                      : L"Git 查询进程未能启动：" + result.launchDetail;
    return read;
  }
  if (result.timedOut) {
    read.error = RepoError::gitTimeout;
    read.detail = L"Git 查询超时";
    return read;
  }
  if (!result.exited) {
    read.error = RepoError::gitFailed;
    read.detail = L"Git 查询未能正常结束";
    return read;
  }
  // 完整度排在退出码之前：半途断掉的输出既不能算「答完了」，也不能据它归出「没有这条配置」——
  // 退出码 1 加上读不全的标准输出，只能说「这件事不知道」，不能说「Git 说没有」。
  if (!result.outputComplete) {
    read.error = RepoError::outputIncomplete;
    read.detail =
        result.incompleteReason.empty() ? L"Git 的标准输出没有完整读回" : result.incompleteReason;
    return read;
  }
  if (result.exitCode == 0) {
    read.outcome = UndoQueryOutcome::answered;
    for (std::wstring_view line : SplitLines(result.utf16Output)) {
      const std::wstring trimmed = TrimWide(line);
      if (!trimmed.empty()) {
        read.lines.push_back(trimmed);
      }
    }
    read.firstLine = read.lines.empty() ? std::wstring() : read.lines.front();
    return read;
  }
  if (result.exitCode == 1 && TrimWide(result.utf16Output).empty()) {
    // --quiet 系查询的「正常没有」：symbolic-ref（不在分支上）、rev-parse --verify --quiet
    // （HEAD 不可解析）与 cat-file -t --quiet（对象不在本地）都以退出码 1、无输出作答。
    // 其余命令不该走到这里，判读方会按语义处理。
    read.outcome = UndoQueryOutcome::noResult;
    return read;
  }
  std::wstring detail;
  read.error = ClassifyGitFailure(result, detail);
  if (read.error == RepoError::none) {
    read.error = RepoError::gitFailed;
  }
  read.detail = detail;
  return read;
}

std::wstring UndoFirstReportedParent(const GitQueryResult& parentsQuery) {
  const UndoQueryRead read = ReadUndoQuery(parentsQuery);
  if (read.outcome != UndoQueryOutcome::answered) {
    return {};
  }
  std::wstring selfId;
  std::vector<std::wstring> parents;
  if (!ParseParentsLine(read.firstLine, &selfId, &parents, nullptr)) {
    return {};
  }
  return parents.empty() ? std::wstring() : parents.front();
}

std::wstring_view UndoPublishEvidenceLabel(UndoPublishEvidence evidence) noexcept {
  switch (evidence) {
    case UndoPublishEvidence::notRun:
      return L"未查询";
    case UndoPublishEvidence::queryFailed:
      return L"无法判断（查询失败）";
    case UndoPublishEvidence::noRemoteRefs:
      return L"无法判断（没有远端跟踪引用）";
    case UndoPublishEvidence::notFound:
      return L"本地信息未发现已发布";
    case UndoPublishEvidence::contained:
      return L"已知已发布";
  }
  return L"未查询";
}

std::wstring_view UndoTargetKindLabel(UndoTargetKind kind) noexcept {
  switch (kind) {
    case UndoTargetKind::undetermined:
      return L"未能判定";
    case UndoTargetKind::verifiedRoot:
      return L"真正根提交";
    case UndoTargetKind::singleParent:
      return L"普通提交（单父）";
    case UndoTargetKind::mergeParents:
      return L"合并提交（多父）";
    case UndoTargetKind::hiddenByShallow:
      return L"浅仓库历史边界";
    case UndoTargetKind::unreadableParent:
      return L"父对象读不到";
    case UndoTargetKind::inconsistentParents:
      return L"父关系不一致";
  }
  return L"未能判定";
}

UndoHeadFacts InterpretUndoHeadSnapshot(const GitQueryResult& symbolicRef,
                                        const GitQueryResult& headCommit) {
  UndoHeadFacts facts;
  const UndoQueryRead sym = ReadUndoQuery(symbolicRef);
  const UndoQueryRead head = ReadUndoQuery(headCommit);
  if (sym.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出当前分支：" + RefusalWith(sym, L"symbolic-ref 未成功");
    return facts;
  }
  if (head.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出 HEAD 指向哪个提交：" + RefusalWith(head, L"rev-parse 未成功");
    return facts;
  }
  facts.queryOk = true;

  if (sym.outcome == UndoQueryOutcome::answered) {
    facts.onBranch = true;
    facts.branchRef = sym.firstLine;
    constexpr std::wstring_view kHeadsPrefix = L"refs/heads/";
    if (facts.branchRef.size() > kHeadsPrefix.size() &&
        facts.branchRef.compare(0, kHeadsPrefix.size(), kHeadsPrefix) == 0) {
      facts.branchName = facts.branchRef.substr(kHeadsPrefix.size());
    }
  }
  if (head.outcome == UndoQueryOutcome::answered) {
    if (LooksLikeFullObjectId(head.firstLine)) {
      facts.headResolved = true;
      facts.headObjectId = head.firstLine;
    } else {
      facts.queryOk = false;
      facts.queryFailure = L"rev-parse 给出的 HEAD 不符合完整对象 ID 的约定，无法安全撤回。";
    }
  }
  return facts;
}

namespace {

// 浅仓库状态那条查询的判读：只认 Git 明确回答的 true/false。
// 老版本 Git 不认这个选项时是「查询失败」，判成「问不出来」——判读层不猜。
void InterpretShallowState(const UndoPreflightQueries& queries, UndoTargetEvidence* target) {
  const UndoQueryRead read = ReadUndoQuery(queries.shallowState);
  if (read.outcome != UndoQueryOutcome::answered) {
    target->shallowDetail = RefusalWith(read, L"git rev-parse --is-shallow-repository 未成功");
    return;
  }
  const std::wstring answer = LowerAscii(read.firstLine);
  if (answer == L"true") {
    target->shallowQueried = true;
    target->repositoryIsShallow = true;
    return;
  }
  if (answer == L"false") {
    target->shallowQueried = true;
    target->repositoryIsShallow = false;
    return;
  }
  target->shallowDetail = L"git 关于「是不是浅仓库」的回答不是 true/false（读到的是：" + read.firstLine +
                          L"）。";
}

// 父关系判定：rev-list 的历史视图 × 提交对象自己记录的 parent 行 × 浅仓库状态 × 目标父对象可读性。
// 只有三种结论能进入方案：verifiedRoot、singleParent、mergeParents；其余都是明确拒绝的理由。
void InterpretUndoTarget(const UndoPreflightQueries& queries, UndoHeadFacts* facts) {
  UndoTargetEvidence& target = facts->target;
  target.queried = true;
  InterpretShallowState(queries, &target);

  std::vector<std::wstring> recorded;
  std::wstring objectFailure;
  const UndoQueryRead objectRead = ReadUndoQuery(queries.commitObject);
  if (objectRead.outcome != UndoQueryOutcome::answered) {
    objectFailure = RefusalWith(objectRead, L"cat-file commit 没能给出这条提交的对象");
  } else if (!ParseRecordedParents(queries.commitObject.utf16Output, &recorded, &objectFailure)) {
    recorded.clear();
  }
  if (!objectFailure.empty()) {
    target.kind = UndoTargetKind::undetermined;
    target.failure = L"没能读出这条提交对象自己记录的父关系：" + objectFailure;
    return;
  }
  target.recordedParentIds = recorded;

  if (!facts->parentsResolved) {
    // rev-list 那一侧本来就没给出可采信的答案；父关系只能算问不出来，
    // 绝不能因为「历史视图里没有父」就把它当成根提交——那正是浅边界的表现。
    target.kind = UndoTargetKind::undetermined;
    target.failure = L"没能从历史视图里问出这条提交的父提交：" +
                     (facts->parentsFailure.empty() ? std::wstring(L"原因未知") : facts->parentsFailure);
    return;
  }

  const std::vector<std::wstring>& reported = facts->parentObjectIds;
  if (reported != recorded) {
    if (reported.empty() && !recorded.empty()) {
      target.kind = UndoTargetKind::hiddenByShallow;
      return;
    }
    target.kind = UndoTargetKind::inconsistentParents;
    return;
  }

  if (recorded.empty()) {
    // 两处都说没有父：只有仓库明确不是浅仓库时，这才是真正的根提交。
    if (!target.shallowQueried) {
      target.kind = UndoTargetKind::undetermined;
      target.failure = L"这条提交没有记录任何父关系，可 Git 没能回答这个仓库是不是浅仓库（" +
                       (target.shallowDetail.empty() ? std::wstring(L"原因未知") : target.shallowDetail) +
                       L"）。分不清「真正的第一个提交」与「浅克隆截断出来的边界」时，"
                       L"本程序不会去删除分支引用。";
      return;
    }
    if (target.repositoryIsShallow) {
      target.kind = UndoTargetKind::hiddenByShallow;
      return;
    }
    target.kind = UndoTargetKind::verifiedRoot;
    return;
  }

  // 目标父提交必须真的在本地：读不到就说明历史不完整，撤回去的是一个不存在的提交。
  const std::wstring& wanted = reported.front();
  if (!queries.parentObjectRan || queries.parentObjectQueryOid != wanted) {
    target.kind = UndoTargetKind::undetermined;
    target.failure = L"没能核对「要挪去的那个父提交」在本地读不读得到。";
    return;
  }
  const UndoQueryRead parentRead = ReadUndoQuery(queries.parentObject);
  if (parentRead.outcome == UndoQueryOutcome::noResult) {
    target.kind = UndoTargetKind::unreadableParent;
    target.unreadableParentId = wanted;
    return;
  }
  if (parentRead.outcome != UndoQueryOutcome::answered) {
    target.kind = UndoTargetKind::undetermined;
    target.failure = L"没能问出那个父提交对象在不在本地：" +
                     RefusalWith(parentRead, L"cat-file -t 未成功");
    return;
  }
  if (LowerAscii(parentRead.firstLine) != L"commit") {
    target.kind = UndoTargetKind::inconsistentParents;
    target.mismatchedParentId = wanted;
    return;
  }
  target.kind = reported.size() == 1 ? UndoTargetKind::singleParent : UndoTargetKind::mergeParents;
}

}  // namespace

UndoPreflightFacts InterpretUndoPreflight(const UndoPreflightQueries& queries) {
  UndoPreflightFacts facts;
  facts.head = InterpretUndoHeadSnapshot(queries.symbolicRef, queries.headCommit);

  if (facts.head.headResolved) {
    facts.publishQueried = queries.commitDependentRan;
    if (queries.commitDependentRan) {
      facts.head.parentsQueried = true;
      const UndoQueryRead parents = ReadUndoQuery(queries.parents);
      if (parents.outcome != UndoQueryOutcome::answered) {
        facts.head.parentsFailure = RefusalWith(parents, L"rev-list 未成功");
      } else {
        std::wstring selfId;
        std::wstring failure;
        if (ParseParentsLine(parents.firstLine, &selfId, &facts.head.parentObjectIds, &failure)) {
          facts.head.selfMatchesHead = (selfId == facts.head.headObjectId);
          facts.head.parentsResolved = facts.head.selfMatchesHead;
          if (!facts.head.selfMatchesHead) {
            facts.head.parentsFailure =
                L"rev-list 报告的提交与 rev-parse 给出的 HEAD 不是同一个对象（HEAD 正在被别的"
                L"进程改动？），本次不撤回。";
          }
        } else {
          facts.head.parentsFailure = failure;
        }
      }
      InterpretUndoTarget(queries, &facts.head);

      // 标题只用于确认文字，查不到不算致命。
      const UndoQueryRead summary = ReadUndoQuery(queries.headSummary);
      if (summary.outcome == UndoQueryOutcome::answered) {
        facts.head.headSummary = summary.firstLine;
      }

      const UndoQueryRead refs = ReadUndoQuery(queries.remoteRefs);
      const UndoQueryRead contains = ReadUndoQuery(queries.remoteContains);
      if (refs.outcome == UndoQueryOutcome::failed) {
        facts.publish = UndoPublishEvidence::queryFailed;
        facts.publishFailureDetail = RefusalWith(refs, L"远端跟踪引用列表查询未成功");
      } else if (contains.outcome == UndoQueryOutcome::failed) {
        facts.publish = UndoPublishEvidence::queryFailed;
        facts.publishFailureDetail = RefusalWith(contains, L"远端包含查询未成功");
      } else if (contains.outcome != UndoQueryOutcome::answered ||
                 refs.outcome != UndoQueryOutcome::answered) {
        // for-each-ref 就算一无所获也是退出码 0；走到这里说明回答不合约定，按查不到处理。
        facts.publish = UndoPublishEvidence::queryFailed;
        facts.publishFailureDetail = L"for-each-ref 的回答不符合约定（意外的退出码或输出）。";
      } else if (!contains.lines.empty()) {
        facts.publish = UndoPublishEvidence::contained;
        facts.containingRemoteRefs = contains.lines;
      } else if (refs.lines.empty()) {
        facts.publish = UndoPublishEvidence::noRemoteRefs;
      } else {
        facts.publish = UndoPublishEvidence::notFound;
      }
    }
  }

  facts.statusQueried = true;
  const UndoQueryRead status = ReadUndoQuery(queries.status);
  if (status.outcome != UndoQueryOutcome::answered) {
    facts.statusError = status.error;
    facts.statusDetail = RefusalWith(status, L"git status 未成功");
  } else {
    WorkspaceStatusParseResult parsed = ParseWorkspacePorcelainV2(queries.status.utf16Output);
    if (!parsed.error.empty()) {
      facts.statusError = RepoError::badOutput;
      facts.statusDetail = parsed.error;
    } else {
      facts.statusOk = true;
      facts.model = std::move(parsed.model);
    }
  }
  return facts;
}

UndoCommitPlan BuildUndoCommitPlan(const UndoCommitPlanInput& input) {
  UndoCommitPlan plan;
  plan.operationId = L"undo-commit";
  plan.displayName = L"撤回最近提交";

  const auto block = [&](std::wstring reason) {
    UndoCommitPlan blocked;
    blocked.blocked = true;
    blocked.blockedReason = std::move(reason);
    blocked.operationId = plan.operationId;
    blocked.displayName = plan.displayName;
    return blocked;
  };

  // ---- 前提核對：任何一項不成立都不產生命令 ----
  if (input.repositoryRoot.empty()) {
    return block(L"没有可用的仓库工作区根目录，无法确定这次撤回属于哪个仓库。请先点“刷新”。");
  }
  const UndoHeadFacts& head = input.facts.head;
  if (!head.queryOk) {
    return block(head.queryFailure.empty() ? std::wstring(L"没能问出 HEAD 与分支，无法撤回。")
                                           : head.queryFailure);
  }
  if (!head.headResolved) {
    return block(L"还没有任何提交，没有可撤回的提交。“撤回最近提交”只处理当前分支的最新一次提交；"
                 L"仓库（或这个分支）尚为空时，索引与工作区本来就没有“相对某次提交的改动”可言。");
  }
  if (!head.onBranch) {
    return block(L"当前处于游离 HEAD（不在任何分支上）。撤回针对的是「当前分支的最新一次提交」，"
                 L"游离状态下没有分支引用可挪，本程序不替游离 HEAD 移动引用。请先 checkout 回一个"
                 L"分支再操作。");
  }
  if (!IsSafeUndoTargetRef(head.branchRef)) {
    return block(L"当前分支的引用名不合格（读回来的是「" + head.branchRef +
                 L"」）。撤回只按完整引用名（refs/heads/…）移动用户确认的那一个分支，"
                 L"不用可变的 HEAD，也不接受形态不明的引用名。");
  }
  if (input.workflow.HasSpecialFlowInProgress()) {
    return block(L"这个仓库里有 Git 流程还没走完：" + input.workflow.SpecialFlowText() +
                 L"。那种状态下分支与 HEAD 引用正被流程当作进度记录使用，此时移动引用会把流程的现场"
                 L"搅乱；“强制继续”也不能替你绕过——Git 自己就先不接受。请在原来的 Git 命令里把流程"
                 L"走完（git merge --continue / --abort、git rebase --continue / --abort 等），"
                 L"再点“刷新”。");
  }
  if (!head.parentsQueried) {
    return block(L"没能读到这条提交的父提交信息，无法确定撤回的目标。请点“刷新”后重试。");
  }
  if (!head.parentsResolved) {
    return block(L"父提交查询没能得出可采信的结论：" +
                 (head.parentsFailure.empty() ? std::wstring(L"原因未知") : head.parentsFailure) +
                 L"。目标定不准，本程序不会执行撤回。");
  }
  if (!input.facts.statusQueried || !input.facts.statusOk) {
    return block(L"没能读到工作区/索引现状（" +
                 (input.facts.statusDetail.empty() ? std::wstring(L"原因未知")
                                                   : input.facts.statusDetail) +
                 L"），无法确认撤回会把哪些内容留在索引里。本程序不在看不懂的现状上移动引用。");
  }
  if (input.facts.model.HasConflicts()) {
    return block(L"还有没解决的冲突条目。冲突状态下的索引与「一次普通提交的产物」不是一个东西，"
                 L"本程序不在带冲突标记的索引上移动引用，也不会替你解决冲突。请先处理这些冲突"
                 L"（git add 或按你的方式解决），再点“刷新”。");
  }
  if (!input.facts.publishQueried) {
    return block(L"预检没有问过这条提交与远端跟踪引用的关系，因此无法说明它是否已经发布过。"
                 L"不确认发布状态就不打开确认框——请点“刷新”后重试。");
  }
  if (!head.target.queried) {
    return block(L"没能核对这条提交的父关系（浅仓库边界、缺失父对象都可能让目标落空），"
                 L"无法确定撤回的目标。请点“刷新”后重试。");
  }

  // ---- 撤回目标分類：只有三種結論可以進入命令構造 ----
  const UndoTargetEvidence& target = head.target;
  const bool rootUndo = target.kind == UndoTargetKind::verifiedRoot;
  const bool mergeCommit = target.kind == UndoTargetKind::mergeParents;
  std::wstring newOid;
  switch (target.kind) {
    case UndoTargetKind::verifiedRoot:
      break;  // 真正根提交：唯一允许删除引用的情形。
    case UndoTargetKind::singleParent:
    case UndoTargetKind::mergeParents:
      newOid = head.parentObjectIds.front();
      break;
    case UndoTargetKind::hiddenByShallow:
      return block(L"这条提交的父关系在两份证据之间对不上，是浅仓库历史边界的典型形态："
                   L"提交对象自己记录了父提交（" + ListIds(target.recordedParentIds) +
                   L"），而 Git 的历史视图里没有给出任何父。那份历史在本地是不完整的——把分支挪到"
                   L"仓库里根本不存在的提交上，引用会被 Git 拒绝，或者留下一个谁也检不出的状态；"
                   L"把「父关系看不见」当成「这就是第一个提交」则会直接删掉整个分支。"
                   L"要撤回这种仓库里的提交，得先补全历史（例如 git fetch --unshallow，或按你的抓取"
                   L"方式加深到这条父关系为止）——本程序不会替你联网抓取，也不会自动 unshallow、"
                   L"不会改动任何远端配置。");
    case UndoTargetKind::unreadableParent:
      return block(L"要挪去的那个父提交（" + ShortObjectId(target.unreadableParentId) +
                   L"）在本地读不到对象内容：这份历史不完整（浅克隆、部分克隆或被清理过的对象库）"
                   L"。本程序不把分支引用挪向一个读不到的提交，也不会自动联网补全。请先用你自己的方式"
                   L"补全历史（git fetch / git fetch --unshallow），再点“刷新”。");
    case UndoTargetKind::inconsistentParents: {
      const std::wstring named =
          target.mismatchedParentId.empty() ? target.unreadableParentId : target.mismatchedParentId;
      std::wstring which = named.empty() ? std::wstring()
                                         : (std::wstring(L"（") + ShortObjectId(named) + L"）");
      return block(L"这条提交的父关系在两份证据之间对不上，或那个父对象并不是提交对象" + which +
                   L"。对象库里的历史形态超出本程序能安全判读的范围，目标定不准就不执行撤回。"
                   L"查询已带 --no-replace-objects，因此 git replace 的替换对象不是这里的解释；"
                   L"请先用命令行工具看清这条提交的实际形态。");
    }
    case UndoTargetKind::undetermined:
    default:
      return block(L"没能把这条提交的父关系判成可采信的结论：" +
                   (target.failure.empty() ? std::wstring(L"原因未知") : target.failure) +
                   L"。目标定不准，本程序不会执行撤回。");
  }
  if (!rootUndo && !LooksLikeFullObjectId(newOid)) {
    return block(L"父提交 ID 的形态不合格，无法把它作为撤回目标。");
  }
  if (!LooksLikeFullObjectId(head.headObjectId)) {
    return block(L"HEAD 的完整对象 ID 形态不合格，无法核对与恢复。");
  }

  // ---- 風險分類：哪些要走「強制撤回（僅本地）」 ----
  std::vector<std::wstring> forceReasons;
  if (input.facts.publish == UndoPublishEvidence::contained) {
    forceReasons.push_back(L"该提交已被本地远端跟踪引用包含（很可能已推送过）");
  }
  if (input.facts.publish == UndoPublishEvidence::queryFailed ||
      input.facts.publish == UndoPublishEvidence::noRemoteRefs) {
    forceReasons.push_back(L"无法判断该提交是否已被远端包含（信息不足）");
  }
  if (mergeCommit) {
    forceReasons.push_back(L"这是一条合并提交（撤回会丢掉它与第一父之外的父关联）");
  }
  if (target.repositoryIsShallow) {
    forceReasons.push_back(L"这个仓库是浅仓库：更早的历史不在本地，撤回后没法在这份历史里继续往回找");
  }
  plan.requiresForce = !forceReasons.empty();

  // ---- 命令：帶預期舊值的原子引用更新（普通路徑與根提交路徑都是） ----
  plan.targetRef = head.branchRef;
  plan.expectedOldObjectId = head.headObjectId;
  plan.newObjectId = newOid;
  plan.targetKind = target.kind;
  if (rootUndo) {
    plan.arguments = {L"update-ref", L"-d", L"-m", std::wstring(kReflogReason), head.branchRef,
                     head.headObjectId};
    plan.commandLabel = L"git update-ref -d";
    plan.targetDisplay = L"回到「尚无提交」（分支引用被删除，索引与工作区不变）";
  } else {
    plan.arguments = {L"update-ref", L"--create-reflog", L"-m", std::wstring(kReflogReason),
                      head.branchRef, newOid, head.headObjectId};
    plan.commandLabel = L"git update-ref";
    plan.targetDisplay = L"父提交 " + ShortObjectId(newOid);
  }

  // ---- 確認文字 ----
  const WorkspaceModel& model = input.facts.model;
  std::wstring preview;
  std::wstring commandLine = L"  git";
  for (const std::wstring& argument : plan.arguments) {
    commandLine += L' ' + argument;
  }
  preview += L"将在命令窗口里执行（工作目录：" + input.repositoryRoot + L"）：\n";
  preview += commandLine + L"\n\n";

  preview += L"仓库：" + input.repositoryRoot + L"\n";
  preview += L"要移动的分支引用：" + head.branchRef + L"\n";
  preview += L"将撤回的提交：" + ShortObjectId(head.headObjectId) + L"（完整 ID " + head.headObjectId +
             L"）「" + (head.headSummary.empty() ? std::wstring(L"（无标题）") : head.headSummary) +
             L"」\n";
  preview += L"预期旧值核对：Git 只在上面那个引用确实还指向 " + head.headObjectId +
             L" 时才动它，值对不上就原样拒绝（非 0 退出）。这一步是原子核对，不是先读后写，"
             L"所以「确认之后分支被别人推进」不会让这次撤回落错位置。\n";
  if (rootUndo) {
    preview += L"目标是回到「尚无提交」：这条提交的对象与 Git 的历史视图都记录它没有父提交，"
               L"而且仓库明确不是浅仓库，因此它是真正的第一个提交（git reset --soft 表达不了那种状态）。\n";
    preview += L"命令用的是带预期旧值的删除形态：只删除 " + head.branchRef +
               L" 这一个引用，索引与工作区一个字节都不动。\n";
  } else {
    preview += L"撤回目标：" + plan.targetDisplay + L"（完整 ID " + newOid + L"）\n";
    preview += L"这个目标同时经过两份证据核对：Git 的历史视图与提交对象自己记录的 parent 行完全一致，"
               L"而且那个父提交对象在本地读得到。\n";
    if (mergeCommit) {
      preview += L"这是一条合并提交：以第一父提交为目标。其余父提交（及其独有的历史）不会出现在"
                 L"当前分支上，但对象仍在仓库里，相关引用与分支的 reflog 还能找回。\n";
    }
  }
  if (target.repositoryIsShallow) {
    preview += L"注意：这个仓库是浅仓库（Git 回答 --is-shallow-repository=true）。本次要挪去的父提交"
               L"已确认在本地读得到，但更早的历史不在本地。\n";
  } else if (!target.shallowQueried) {
    preview += L"注意：Git 没有回答「这个仓库是不是浅仓库」，因此本程序不会把「看不见父提交」"
               L"当成「这就是第一个提交」——真正根提交的删除路径在这种仓库里一律不开放。\n";
  }
  preview += L"关于竞争：这条命令原子核对的是引用的值；HEAD 是否还指向这个分支由执行前的同步复核保证，"
             L"复核与本程序启动 Git 之间的极短窗口无法由本程序锁住（完整的符号引用事务需要 "
             L"git update-ref --stdin，而命令窗口没有标准输入通道）。那种情况下被移动的也只还是上面"
             L"写明的这一个引用，绝不会顺手改动别的分支、索引或工作区。\n\n";

  if (!model.staged.empty() || !model.unstaged.empty()) {
    preview += L"工作区/索引现状：已暂存 " + std::to_wstring(model.staged.size()) + L" 项、未暂存 " +
               std::to_wstring(model.unstaged.size()) + L" 项。\n";
    preview += L"撤回后，原提交的改动将与这些现有改动一起保留；Git 不记录“哪一行属于哪一次提交”，"
               L"本程序无法把两堆分开。这种撤回只移动分支引用、不碰索引，因此不会产生文本合并冲突。\n";
  } else {
    preview += L"工作区与索引目前是干净的：撤回后，原提交的全部改动会原样出现在“已暂存的更改”里。\n";
  }
  preview += L"\n";

  preview += PublishEvidenceSentence(input.facts) + L"\n\n";

  preview += L"恢复线索：原提交完整 ID " + head.headObjectId + L"。\n";
  if (rootUndo) {
    preview += L"找回方式：git update-ref " + head.branchRef + L" " + head.headObjectId +
               L"（分支引用被删除后，它自己的 reflog 是否还在由 Git 决定，本程序不把它当作找回依据；"
               L"可靠的是对象仍在仓库里，按完整 ID 重建引用即可）。本程序不会自动恢复。\n";
  } else {
    preview += L"找回方式：git update-ref " + head.branchRef + L" " + head.headObjectId +
               L"（这条找回命令不再带预期旧值，由你确认时机；本次移动已带 --create-reflog 写进了该分支的 "
               L"reflog，可用 git reflog show " +
               head.branchName + L" 查回原完整 ID）。本程序不会自动恢复，也不会删除 reflog。\n";
  }
  preview += L"这只移动本机的这一个分支引用：不产生反向提交，不丢弃任何改动，不触碰远端，"
             L"更不会自动 force push。\n";
  if (plan.requiresForce) {
    std::wstring joined;
    for (size_t index = 0; index < forceReasons.size(); ++index) {
      if (index > 0) {
        joined += L"；";
      }
      joined += forceReasons[index];
    }
    preview += L"\n需要明确确认的风险：" + joined + L"。\n";
    preview += L"要继续请点击“强制撤回（仅本地）”。那个按钮只是确认你接受上述风险，"
               L"命令本身与上面展示的一条不差、不会改用更激烈的手段。";
  } else {
    preview += L"确定要执行吗？取消不会打开命令窗口，也不会改动仓库。";
  }

  plan.stateChangeNote = [&]() -> std::wstring {
    const CapturedSnapshot& captured = input.captured;
    if (!captured.valid) {
      return {};
    }
    const bool headChanged = captured.hasHead != head.headResolved ||
                             !ShaDisplaysMatch(captured.shortSha, ShortObjectId(head.headObjectId));
    const bool stagedChanged = captured.stagedItems != model.staged.size();
    if (!headChanged && !stagedChanged) {
      return {};
    }
    std::wstring note = L"界面原先显示的仓库现状与刚刚读回的已经不同：";
    if (headChanged) {
      note += L"HEAD 从「" +
              (captured.shortSha.empty() ? std::wstring(L"尚无提交") : captured.shortSha) + L"」变成「" +
              ShortObjectId(head.headObjectId) + L"」；";
    }
    if (stagedChanged) {
      note += L"已暂存的条目从 " + std::to_wstring(captured.stagedItems) + L" 项变成 " +
              std::to_wstring(model.staged.size()) + L" 项；";
    }
    note += L"下面的确认内容以刚刚读回的为准。请复核后再决定。";
    return note;
  }();
  if (!plan.stateChangeNote.empty()) {
    preview = plan.stateChangeNote + L"\n\n" + preview;
  }
  plan.previewText = std::move(preview);

  std::wstring notice = rootUndo
                            ? L"只删除分支引用 " + head.branchRef +
                                  L"（仅本地，带预期旧值 " + ShortObjectId(head.headObjectId) +
                                  L"）：索引与工作区保持原样。"
                            : L"只移动分支引用 " + head.branchRef +
                                  L"（仅本地，带预期旧值 " + ShortObjectId(head.headObjectId) +
                                  L"）：索引与工作区保持原样。";
  if (input.workflow.indexLocked) {
    notice += L"（注意：仓库里已有 index.lock，另一个 Git 进程可能正在写索引；撤回本身只动引用，"
              L"但结果以 Git 的回答为准）";
  }
  plan.notice = std::move(notice);

  plan.restoreHint = L"原提交 " + ShortObjectId(head.headObjectId) + L"（完整 ID " +
                     head.headObjectId + L"）已撤回；" + head.branchRef + L" 现在" +
                     (rootUndo ? std::wstring(L"还没有提交")
                               : (std::wstring(L"指向父提交 ") + ShortObjectId(newOid))) +
                     L"。如需找回，可执行 git update-ref " + head.branchRef + L" " +
                     head.headObjectId + L"（本程序不会自动执行）。";
  if (!rootUndo) {
    plan.restoreHint += L"该分支的 reflog（git reflog show " + head.branchName + L"）也记录了这次移动。";
  } else {
    plan.restoreHint += L"分支引用已删除，它的 reflog 不作为找回依据。";
  }
  return plan;
}

}  // namespace gc::git
