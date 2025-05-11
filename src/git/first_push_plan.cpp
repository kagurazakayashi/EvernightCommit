#include "git/first_push_plan.h"

#include <string>
#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId
#include "git/pull_plan.h"       // 復用符號引用／HEAD／上游／關係查詢的形態與判讀

namespace gc::git {
namespace {

constexpr std::wstring_view kHeadsPrefix = L"refs/heads/";

bool StartsWithHeads(std::wstring_view text) {
  return text.size() >= kHeadsPrefix.size() && text.compare(0, kHeadsPrefix.size(), kHeadsPrefix) == 0;
}

// 展示用的命令行（與 push_plan 裡同一種寫法：一項一個空格隔開，不加任何 shell 引用）。
std::wstring CommandDisplay(const std::vector<std::wstring>& arguments) {
  std::wstring text = L"git";
  for (const std::wstring& argument : arguments) {
    text += L' ' + argument;
  }
  return text;
}

std::wstring ShortOr(const std::wstring& objectId, std::wstring_view fallback) {
  return objectId.empty() ? std::wstring(fallback) : ShortObjectId(objectId);
}

// 候選遠端清單拒絕時的說明：把 Git 答出的遠端名點出來，一個都不猜。
std::wstring DescribeCandidateRefusal(const FirstPushCandidateFacts& candidates) {
  if (!candidates.configOk) {
    return L"这个仓库的生效配置没能读回来：" + candidates.configFailure +
           L"。远端清单全靠这份配置，看不见它就没有可摆出来的目标——本程序不猜一个远端名，"
           L"也不拿配置里的原样地址顶替。";
  }
  if (candidates.candidates.empty()) {
    return L"这个仓库里一个配好地址的远端也没有（生效配置里找不到任何 remote.<名字>.url 或 "
           L"remote.<名字>.pushurl）。首次推送同样得有「送到哪里」的答案，没有远端就没有目标，"
           L"本程序不替你 git remote add，也不猜一个地址。";
  }
  std::wstring text = L"仓库里的远端没有一个能确定发布地址：\n";
  for (const FirstPushRemoteCandidate& candidate : candidates.candidates) {
    text += L" · 「" + candidate.name + L"」：" + candidate.failure + L"\n";
  }
  text += L"看不见要寄到哪里就不寄：本程序不拿未经解析的原样地址继续发布。";
  return text;
}

std::wstring PresenceDetail(const FirstPushFacts& facts) {
  if (!facts.presenceKnown) {
    std::wstring text = L"对端现状：没能问出来。";
    for (const PushTargetCheck& check : facts.presence) {
      text += L"\n   " + MaskPushUrlCredentials(check.url) + L"：" +
              (check.failure.empty() ? std::wstring(check.queried ? L"问成功了但结果无法采认"
                                                                  : L"这条询问没有发出去")
                                     : check.failure);
    }
    return text;
  }
  if (facts.presenceDisagrees) {
    std::wstring text = L"对端现状：各个发布目标答的位置不一样（多个发布地址本来可能不是同一个仓库），"
                        L"本程序不把它们合并成一个位置：";
    for (const PushTargetCheck& check : facts.presence) {
      text += L"\n   " + MaskPushUrlCredentials(check.url) + L"：" +
              (check.refPresent ? ShortOr(check.remoteObjectId, L"（位置没读回来）")
                                : std::wstring(L"没有这条引用"));
    }
    return text;
  }
  if (!facts.remoteRefExists) {
    return L"对端现状：每个发布目标都答了——那边现在**没有** " + facts.targetBranchRef +
           L" 这条引用。这次推送会在对端新建它。";
  }
  return L"对端现状：那条引用**已经存在**，现在停在 " + ShortOr(facts.remoteObjectId, L"（没读回来）") +
         L"（完整 ID " + facts.remoteObjectId + L"）。也就是说这一次不是「新建分支」。";
}

std::wstring RelationshipDetail(const FirstPushFacts& facts) {
  if (!facts.presenceKnown || !facts.remoteRefExists || facts.presenceDisagrees) {
    return std::wstring();  // 没有一致的对端位置可比，谈快进与否没有意义。
  }
  if (facts.remoteObjectId == facts.headObjectId) {
    return L" · 对端那条引用已经停在你要推的这份提交上：这次多半只会得到「Everything up-to-date」。";
  }
  if (!facts.relationshipKnown) {
    return L" · 快进判断：没能问出来（" +
           (facts.relationshipDetail.empty() ? std::wstring(L"原因未知") : facts.relationshipDetail) +
           L"）。对端那份提交的对象本地读不到时就是这样，本程序不猜它是不是这次那份提交的祖先。";
  }
  if (facts.remoteOnly > 0) {
    return L" · 快进判断：对端有 " + std::to_wstring(facts.remoteOnly) +
           L" 个提交是这次要推的那份里**没有**的——这次推送会被 Git 以 non-fast-forward 拒绝。"
           L"本程序不带 --force，也不会先替你抓取或整合。";
  }
  return L" · 快进判断：这次要推的那份包含对端那条引用的全部内容（本地另有 " +
         std::to_wstring(facts.localOnly) + L" 个提交要送过去），推送是快进。";
}

}  // namespace

// ---- 要不要走向导 ----

bool CanOfferFirstPush(const PushPreflightFacts& facts) {
  if (!facts.queryOk || !facts.configOk) {
    return false;  // 连现状都没读回来，摆不出任何可信的目标。
  }
  if (!facts.onBranch || !IsUsableLocalBranchRef(facts.branchRef)) {
    return false;  // 游离 HEAD 或引用名形态不合格：没有「这条分支」可推。
  }
  if (!facts.headResolved || !LooksLikeFullObjectId(facts.headObjectId)) {
    return false;  // 分支还没有任何提交：连「推哪一份」都答不出来。
  }
  // 上游查询必须**问过且明确回答没有**。问不成（Git 报错）不在这里放行——那种场合
  // 连「到底有没有上游」都不知道，该走的是普通推送的拒绝说明。
  if (!facts.upstreamRan || facts.upstreamConfigured || !facts.upstreamRemote.empty()) {
    return false;
  }
  return !facts.config.RemoteNames().empty();
}

// ---- 候选远端 ----

std::wstring FirstPushRemoteCandidate::urlsDisplay() const {
  if (!publishUrls.empty()) {
    return FormatPushUrlList(publishUrls);
  }
  if (!failure.empty()) {
    return L"（不能选：" + failure + L"）";
  }
  return std::wstring(L"（没能问出发布 URL）");
}

FirstPushCandidateFacts InterpretFirstPushCandidates(
    const GitQueryResult& configListing, const std::vector<GitQueryResult>& remoteUrlQueries) {
  FirstPushCandidateFacts result;
  const PushConfigListing config = ParsePushConfigListing(configListing);
  result.configOk = config.readOk;
  if (!result.configOk) {
    result.configFailure = config.readFailure;
    return result;
  }
  const std::vector<std::wstring> names = config.RemoteNames();
  result.candidates.reserve(names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    FirstPushRemoteCandidate candidate;
    candidate.name = names[index];
    if (!IsUsableRemoteName(candidate.name)) {
      candidate.failure = L"这个远端名的形态不合格（为空、以 - 开头，或含引号/冒号/控制字符），"
                          L"连问一句「它实际去哪里」都发不出去";
    } else if (index >= remoteUrlQueries.size()) {
      candidate.failure = L"这个远端的发布地址查询没有发出去（前面的查询已经把这一轮问不完），"
                          L"本程序不拿别的远端的地址充数";
    } else {
      const PushUrlResolution resolution =
          ResolvePushUrls(remoteUrlQueries[index], true, config, candidate.name);
      candidate.publishUrls = resolution.urls;
      if (candidate.publishUrls.empty()) {
        candidate.failure = resolution.failure.empty() ? std::wstring(L"Git 没答出发布地址")
                                                      : resolution.failure;
      }
    }
    result.candidates.push_back(std::move(candidate));
  }
  return result;
}

std::wstring DescribeFirstPushCandidateRefusal(const FirstPushCandidateFacts& candidates) {
  return DescribeCandidateRefusal(candidates);
}

// ---- 目标分支名的形态底线 ----

std::wstring DescribeFirstPushBranchNameProblem(std::wstring_view branchName) {
  if (branchName.empty()) {
    return std::wstring(L"分支名不能是空的。");
  }
  if (branchName.front() == L' ' || branchName.back() == L' ') {
    return std::wstring(L"分支名首尾不能是空格。");
  }
  if (branchName.find(L' ') != std::wstring_view::npos) {
    return std::wstring(L"分支名里不能有空格。");
  }
  if (!IsUsableRemoteRef(std::wstring(kHeadsPrefix) + std::wstring(branchName))) {
    return std::wstring(L"分支名里有命令窗口无法安全表达的写法（引号、冒号或控制字符）。");
  }
  return std::wstring();
}

std::wstring BuildFirstPushTargetBranchRef(std::wstring_view branchName) {
  if (branchName.empty()) {
    return std::wstring();  // 空的分支名拼不出「refs/heads/」之后还有内容的引用。
  }
  const std::wstring ref = std::wstring(kHeadsPrefix) + std::wstring(branchName);
  return IsUsableRemoteRef(ref) ? ref : std::wstring();
}

// ---- 查询参数 ----

std::vector<std::wstring> BuildFirstPushRefFormatArguments(std::wstring_view repositoryDirectory,
                                                           std::wstring_view targetBranchRef) {
  if (repositoryDirectory.empty() || !IsUsableRemoteRef(targetBranchRef)) {
    return {};
  }
  // 一律交 refs/heads/… 的完整形态：check-ref-format 不认 `--`（实测带裸名字 `-oops` 连 `--`
  // 挡不住，它先以 129 用法错误死在自己的参数解析上），完整引用名以 refs/ 开头就不可能被当成选项。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"check-ref-format", std::wstring(targetBranchRef)};
}

std::vector<std::wstring> BuildFirstPushConfigPathArguments(std::wstring_view repositoryDirectory) {
  if (repositoryDirectory.empty()) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"rev-parse", L"--git-path", L"config"};
}

std::vector<std::wstring> BuildFirstPushRelationshipArguments(std::wstring_view repositoryDirectory,
                                                              std::wstring_view remoteObjectId,
                                                              std::wstring_view localObjectId) {
  // 形态与 pull 的关系查询同一份实现（两个完整对象 ID、`--no-replace-objects` 也带上）。
  // 这里的左右读法相反：写在前面的那个是对端的位置，所以「左=只有对端有」「右=只有本地有」。
  return BuildPullAheadBehindArguments(std::wstring(repositoryDirectory), remoteObjectId,
                                       localObjectId);
}

// ---- 上游写入的兩條命令 ----

std::vector<UpstreamWriteStep> BuildUpstreamWriteSteps(std::wstring_view branchName,
                                                      std::wstring_view remoteName,
                                                      std::wstring_view targetBranchRef) {
  if (branchName.empty() || !IsUsableRemoteName(remoteName) || !IsUsableRemoteRef(targetBranchRef)) {
    return {};
  }
  const std::wstring remoteKey = L"branch." + std::wstring(branchName) + L".remote";
  const std::wstring mergeKey = L"branch." + std::wstring(branchName) + L".merge";
  // 配置鍵是會原樣交給 `git config` 的資料：鍵名裡有引號、冒號或控制字符就沒人說得清
  // 「寫的到底是哪一把鍵」，這種場合整步拒絕，不修一個看起來能用的鍵名。
  if (!IsUsableRemoteName(remoteKey) || !IsUsableRemoteName(mergeKey)) {
    return {};
  }

  UpstreamWriteStep remoteStep;
  remoteStep.key = remoteKey;
  remoteStep.value = std::wstring(remoteName);
  remoteStep.purpose = L"记下这条分支跟踪哪个远端";
  remoteStep.arguments = {L"config", remoteStep.key, remoteStep.value};
  remoteStep.operationId = L"push-upstream-remote";
  remoteStep.displayName = L"设置上游（远端）";
  remoteStep.commandLabel = CommandDisplay(remoteStep.arguments);
  remoteStep.conclusion = L"已写入 " + remoteKey + L" = " + std::wstring(remoteName) + L"。";

  UpstreamWriteStep mergeStep;
  mergeStep.key = mergeKey;
  mergeStep.value = std::wstring(targetBranchRef);
  mergeStep.purpose = L"记下这条分支对应远端那边的哪个引用";
  mergeStep.arguments = {L"config", mergeStep.key, mergeStep.value};
  mergeStep.operationId = L"push-upstream-merge";
  mergeStep.displayName = L"设置上游（远端引用）";
  mergeStep.commandLabel = CommandDisplay(mergeStep.arguments);
  mergeStep.conclusion = L"已写入 " + mergeKey + L" = " + std::wstring(targetBranchRef) + L"。";

  return {remoteStep, mergeStep};
}

// ---- 對端現狀的聚合 ----

FirstPushPresenceSummary SummarizeFirstPushPresence(const std::vector<PushTargetCheck>& checks) {
  FirstPushPresenceSummary summary;
  if (checks.empty()) {
    summary.detail = L"一个发布目标都没问过（连一个展开出来的地址都没有时就是这样）";
    return summary;
  }
  size_t unanswered = 0;
  std::wstring firstFailure;
  for (const PushTargetCheck& check : checks) {
    if (!check.queried || !check.ok) {
      ++unanswered;
      if (firstFailure.empty()) {
        firstFailure = check.failure.empty()
                           ? std::wstring(check.queried ? L"询问未能采认" : L"询问没有发出去")
                           : check.failure;
      }
      continue;
    }
    if (!check.refPresent) {
      continue;
    }
    summary.exists = true;
    if (summary.remoteObjectId.empty()) {
      summary.remoteObjectId = check.remoteObjectId;
    } else if (summary.remoteObjectId != check.remoteObjectId) {
      summary.disagrees = true;
    }
  }
  summary.known = unanswered == 0;
  if (!summary.known) {
    summary.detail = L"有 " + std::to_wstring(unanswered) + L" 个发布目标没能问到（" + firstFailure +
                     L"）——这与「对端没有这条分支」是两件事，本程序不把问不到写成没有";
  }
  return summary;
}

// ---- 預檢判讀 ----

FirstPushFacts InterpretFirstPushProbe(const FirstPushProbeQueries& queries) {
  FirstPushFacts facts;
  facts.chosenRemoteName = queries.chosenRemoteName;
  facts.targetBranchRef = queries.targetBranchRef;

  const UndoQueryRead symbolic = ReadUndoQuery(queries.symbolicRef);
  if (symbolic.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出当前分支：符号引用查询未成功。";
    return facts;
  }
  if (symbolic.outcome == UndoQueryOutcome::answered) {
    facts.branchRef = symbolic.firstLine;
    facts.onBranch = PullBranchNameFromRef(facts.branchRef, &facts.branchName);
    if (!facts.onBranch) {
      facts.branchRef.clear();
    }
  }

  const UndoQueryRead head = ReadUndoQuery(queries.headObject);
  if (head.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出 HEAD 指向哪个提交，本程序不在问不出目标的前提下动远端。";
    return facts;
  }
  if (head.outcome == UndoQueryOutcome::answered) {
    if (!LooksLikeFullObjectId(head.firstLine)) {
      facts.queryFailure = L"rev-parse 给出的 HEAD 不符合完整对象 ID 的约定，不在这上面做判断。";
      return facts;
    }
    facts.headResolved = true;
    facts.headObjectId = head.firstLine;
  }

  if (queries.upstreamRan) {
    facts.upstreamRan = true;
    const UndoQueryRead upstream = ReadUndoQuery(queries.upstream);
    if (upstream.outcome == UndoQueryOutcome::failed) {
      facts.queryFailure = L"没能问出分支的上游配置，无法确认这次是不是「还没有上游」的那种推送。";
      return facts;
    }
    if (upstream.outcome == UndoQueryOutcome::answered && !upstream.lines.empty()) {
      const PullUpstreamInfo info = ParsePullUpstreamLine(upstream.lines.front());
      facts.upstreamConfigured = info.configured;
      facts.upstreamRemote = info.remote;
      facts.upstreamTrackingRef = info.trackingRef;
    }
  }

  facts.config = ParsePushConfigListing(queries.configListing);
  facts.configOk = facts.config.readOk;
  if (!facts.configOk) {
    facts.queryFailure = L"没能问回这个仓库的生效配置：" + facts.config.readFailure +
                         L"。远端清单与发布地址都由这份配置定，看不见它就没有可确认的目标。";
    return facts;
  }

  facts.chosenRemoteNameUsable = IsUsableRemoteName(facts.chosenRemoteName);
  if (facts.chosenRemoteNameUsable) {
    facts.chosenRemoteExists = facts.config.HasRemote(facts.chosenRemoteName);
    facts.rawPublishUrls = RawConfiguredPushUrls(facts.config, facts.chosenRemoteName);
    const PushUrlResolution resolution =
        ResolvePushUrls(queries.chosenRemoteUrl, queries.chosenRemoteUrlRan, facts.config,
                        facts.chosenRemoteName);
    facts.publishUrls = resolution.urls;
    facts.publishUrlFailure = resolution.failure;
    facts.publishUrlRewritten = resolution.rewritten;
    facts.publishUrlNote =
        DescribePushUrlNote(facts.publishUrls, facts.publishUrlRewritten, facts.rawPublishUrls);
    facts.scopeEffects = InspectPushScopeConfig(facts.config, facts.chosenRemoteName);
  } else {
    facts.publishUrlFailure = L"选定的远端名字形态不合格（为空、以 - 开头，或含引号/冒号/控制字符）"
                              L"：连问一句「它实际去哪里」都发不出去。";
  }

  facts.refFormatRan = queries.refFormatRan;
  if (facts.refFormatRan) {
    const UndoQueryRead format = ReadUndoQuery(queries.refFormat);
    if (format.outcome == UndoQueryOutcome::answered) {
      facts.refFormatAccepted = true;
      facts.refFormatDetail = L"Git 认定这个引用名可以用（check-ref-format 退出码 0）。";
    } else if (format.outcome == UndoQueryOutcome::noResult) {
      facts.refFormatAccepted = false;
      facts.refFormatDetail = L"Git 明确回答这个引用名不能用（check-ref-format 退出码 1）：" +
                              facts.targetBranchRef + L"。本程序不改写它、不加前缀凑一个，也不发出任何命令。";
    } else {
      facts.refFormatAccepted = false;
      facts.refFormatDetail = L"引用名的裁定没能问出来（check-ref-format 没有正常回答）。";
    }
  } else {
    facts.refFormatDetail = L"这条查询没有发出去（目标引用的形态先不合格）。";
  }

  // 逐發布目標的在/不在/問不到。條數對不上時一律按「沒問全」處理：少問的那個地址
  // 也是這次要推去的地方，不能當它不存在。
  if (queries.presenceQueries.size() != facts.publishUrls.size()) {
    facts.presence.reserve(facts.publishUrls.size());
    for (const std::wstring& url : facts.publishUrls) {
      PushTargetCheck unknown;
      unknown.url = url;
      unknown.queried = false;
      unknown.failure = L"这条询问没有发出去（与展开出的发布地址条数对不上）";
      facts.presence.push_back(std::move(unknown));
    }
  } else {
    facts.presence.reserve(queries.presenceQueries.size());
    for (size_t index = 0; index < queries.presenceQueries.size(); ++index) {
      facts.presence.push_back(InterpretPushTargetCheck(facts.publishUrls[index],
                                                         queries.presenceQueries[index],
                                                         facts.targetBranchRef));
    }
  }
  const FirstPushPresenceSummary presence = SummarizeFirstPushPresence(facts.presence);
  facts.presenceKnown = presence.known;
  facts.remoteRefExists = presence.exists;
  facts.presenceDisagrees = presence.disagrees;
  facts.remoteObjectId = presence.remoteObjectId;

  if (queries.relationshipRan) {
    const UndoQueryRead count = ReadUndoQuery(queries.relationship);
    if (count.outcome != UndoQueryOutcome::answered) {
      facts.relationshipDetail = L"rev-list --left-right --count 没能正常回答（对端那份提交的对象"
                                L"本地读不到时，Git 会在这里报错）";
    } else {
      const std::wstring_view line =
          count.lines.empty() ? std::wstring_view() : std::wstring_view(count.lines.front());
      // 這裡的左右與 pull 相反：命令寫的是 `<對端那份>...<這次要推的那份>`。
      long long onlyRemote = 0;
      long long onlyLocal = 0;
      if (ParseAheadBehindCount(line, &onlyRemote, &onlyLocal)) {
        facts.relationshipKnown = true;
        facts.remoteOnly = onlyRemote;
        facts.localOnly = onlyLocal;
      } else {
        facts.relationshipDetail = L"rev-list --left-right --count 的回答不是「左 右」两个非负整数"
                                   L"（那一行是：" + std::wstring(line) + L"）";
      }
    }
  }

  if (queries.configPathRan) {
    const UndoQueryRead path = ReadUndoQuery(queries.configPath);
    if (path.outcome == UndoQueryOutcome::answered && !path.firstLine.empty()) {
      facts.configFilePath = path.firstLine;
    } else {
      facts.configFilePathFailure =
          L"配置文件的落点没能问出来（git rev-parse --git-path config 没有正常回答）。";
    }
  } else {
    facts.configFilePathFailure = L"配置文件的落点没有去问。";
  }

  facts.queryOk = true;
  return facts;
}

// ---- 方案 ----

namespace {

std::wstring FirstPushPrerequisiteRefusal(const FirstPushFacts& facts,
                                         std::wstring_view repositoryDirectory) {
  if (repositoryDirectory.empty()) {
    return L"没有可用的仓库工作区根目录，无法确定这次首次推送属于哪个仓库。请先点“刷新”。";
  }
  if (!facts.queryOk) {
    return facts.queryFailure.empty() ? std::wstring(L"首次推送的只读查询没能完成，无法确定分支与目标。")
                                      : facts.queryFailure;
  }
  if (!facts.onBranch) {
    return L"当前处于游离 HEAD（不在任何分支上）。首次推送做的是「把这条分支送到你选的地方」，"
           L"游离状态下没有分支可送。请先切回一个分支，点“刷新”后再来。";
  }
  if (!IsUsableLocalBranchRef(facts.branchRef)) {
    return L"当前分支的引用名形态不合格（不是 refs/heads/ 开头，或含命令窗口无法安全表达的字符）：" +
           facts.branchRef + L"。没有构造任何命令。";
  }
  if (!facts.headResolved || !LooksLikeFullObjectId(facts.headObjectId)) {
    return L"分支 " + facts.branchName +
           L" 还没有任何提交（HEAD 还不可解析），这次没有可推送的东西。"
           L"请先创建提交，再点“刷新”后重来。";
  }
  if (!facts.upstreamRan) {
    return L"没能确认「这条分支到底有没有上游」，因此也无从判断该不该走首次推送。请点“刷新”后重试。";
  }
  if (facts.upstreamConfigured) {
    return L"这条分支已经有上游了（远端「" + facts.upstreamRemote + L"」的 " +
           facts.upstreamTrackingRef + L"）。那种场合该走的是普通推送：目标由仓库配置说清楚，"
           L"不该在这里再挑一次。请重新点“推送”。";
  }
  if (!facts.configOk) {
    return L"这个仓库的生效配置没能读回来，远端与发布目标都无从确定。";
  }
  if (!facts.chosenRemoteNameUsable) {
    return L"你选的远端名字形态不合格：" +
           (facts.chosenRemoteName.empty() ? std::wstring(L"（空）") : facts.chosenRemoteName) +
           L"。没有构造任何命令。";
  }
  if (!facts.chosenRemoteExists) {
    return L"你选的远端「" + facts.chosenRemoteName +
           L"」在这个仓库的生效配置里已经没有地址了（既没有 url 也没有 pushurl）。"
           L"本程序不猜别的远端顶替，也不会替你 git remote add 或改任何配置。";
  }
  if (facts.publishUrls.empty()) {
    return L"远端「" + facts.chosenRemoteName + L"」在清单里，但 Git 没能答出这次实际会去的发布地址（" +
           (facts.publishUrlFailure.empty() ? std::wstring(L"git remote get-url --push --all 没有给出地址")
                                            : facts.publishUrlFailure) +
           L"）。看不见要去哪里就不推：本程序不会拿配置里未经解析的原样地址顶替它继续发布。";
  }
  if (!facts.refFormatRan || !facts.refFormatAccepted) {
    return L"目标分支名的裁定没有通过：" + facts.refFormatDetail +
           L"\n\n没有发出任何命令，也没有接触远端、没有改动仓库、没有写任何配置。";
  }
  if (!IsUsableRemoteRef(facts.targetBranchRef)) {
    return L"目标引用的形态不合格（不是 refs/ 开头，或含 `:`、引号等无法安全进入命令的写法）：" +
           (facts.targetBranchRef.empty() ? std::wstring(L"（空）") : facts.targetBranchRef) +
           L"。本程序不把它改写成一个看起来对的，也没有发出任何命令。";
  }
  return std::wstring();
}

}  // namespace

FirstPushPlan BuildFirstPushPlan(const FirstPushPlanInput& input) {
  const FirstPushFacts& facts = input.facts;
  const auto block = [&input](std::wstring reason) {
    FirstPushPlan blocked;
    blocked.state = FirstPushPlanState::blocked;
    blocked.explanation = std::move(reason);
    blocked.operationId = L"push";
    blocked.displayName = L"推送";
    blocked.setUpstreamRequested = input.setUpstreamRequested;
    return blocked;
  };

  const std::wstring refusal = FirstPushPrerequisiteRefusal(facts, input.repositoryDirectory);
  if (!refusal.empty()) {
    return block(refusal);
  }

  const std::vector<std::wstring> arguments =
      BuildPushCommandArguments(facts.chosenRemoteName, facts.headObjectId, facts.targetBranchRef,
                               facts.scopeEffects.mirrorConfigured, facts.scopeEffects.tagOptConfigured);
  if (arguments.empty()) {
    return block(L"推送命令的参数没有通过构造期的形态检查（远端名、要钉住的完整提交 ID 或目标引用"
                 L"里有 Git 命令行无法安全表达的写法）。没有发出任何命令。");
  }

  FirstPushPlan plan;
  plan.state = FirstPushPlanState::ready;
  plan.operationId = L"push";
  plan.displayName = L"推送";
  plan.branchName = facts.branchName;
  plan.localBranchRef = facts.branchRef;
  plan.pushedObjectId = facts.headObjectId;
  plan.remoteName = facts.chosenRemoteName;
  plan.targetBranchRef = facts.targetBranchRef;
  plan.pushUrls = facts.publishUrls;
  plan.remoteUrlDisplay = FormatPushUrlList(facts.publishUrls);
  plan.arguments = arguments;
  plan.commandLabel = CommandDisplay(arguments);
  plan.setUpstreamRequested = input.setUpstreamRequested;
  plan.upstreamTargetFile = facts.configFilePath;
  if (plan.setUpstreamRequested) {
    plan.upstreamSteps =
        BuildUpstreamWriteSteps(facts.branchName, facts.chosenRemoteName, facts.targetBranchRef);
    if (plan.upstreamSteps.empty()) {
      return block(L"上游配置的两条写入命令没能通过形态检查（分支名或它对应的配置键里有命令窗口"
                   L"无法安全表达的写法）。本次没有构造任何命令——包括那条推送：既然答应了"
                   L"「推送并设置上游」，就不能只做成一半。请改成「只推送」再来一次，"
                   L"或先把分支名换成一个规规矩矩的写法。");
    }
  }

  // ---- 风险：要用户明确点头的几条 ----
  std::vector<std::wstring> risks;
  if (!facts.presenceKnown) {
    std::wstring asked;
    for (const PushTargetCheck& check : facts.presence) {
      if (check.queried && check.ok) {
        continue;
      }
      if (!asked.empty()) {
        asked += L"；";
      }
      asked += MaskPushUrlCredentials(check.url) + L"：" +
               (check.failure.empty()
                    ? std::wstring(check.queried ? L"询问的结果没能采认" : L"询问没有发出去")
                    : check.failure);
    }
    if (asked.empty()) {
      asked = L"逐目标的询问结果对不上发布地址的条数";
    }
    risks.push_back(L"没能问清对端到底有没有 " + facts.targetBranchRef + L" 这条引用（" + asked +
                    L"）。这与「对端没有这条分支」是两件事：认证、权限、网络都落在「问不到」这一类里。"
                    L"本程序不因为问不到就断定对端是空的，也不据此拒绝一次本来合法的推送——"
                    L"命令里没有 --force，对端若已有那条引用且不是快进，Git 自己会拒绝。");
  } else if (facts.remoteRefExists) {
    std::wstring text = L"对端已经有 " + facts.targetBranchRef + L" 这条引用，这次不是在对端新建分支。";
    if (facts.presenceDisagrees) {
      text += L"而且各发布目标答出的位置不一样，本程序不把它们合并成一个位置来看。";
    }
    text += L"「首次推送」不是覆盖它的理由：命令里没有 --force、没有 --force-with-lease，"
            L"本程序也不会先替你抓取或整合。对端若有本地这份提交里没有的东西，这次就会被 Git "
            L"自己拒绝（快进与否见上面那句判断，本程序不猜）；那种场合请先 fetch/pull 把远端的"
            L"东西整合进来再推。";
    risks.push_back(std::move(text));
  }
  if (facts.publishUrls.size() > 1) {
    risks.push_back(L"这个远端配了 " + std::to_wstring(facts.publishUrls.size()) +
                    L" 个发布地址，实测 Git 会推给每一个：这一次点「推送」等于同时推 " +
                    std::to_wstring(facts.publishUrls.size()) +
                    L" 个地方（上面已逐条列出）。那条命令的退出码是各目标合计的，"
                    L"整体成功不代表每个都送到位。");
  }
  if (facts.publishUrlRewritten) {
    risks.push_back(L"发布地址与配置里写的原样地址不一样：那是 Git 按 url.*.insteadOf / pushInsteadOf "
                    L"改写之后的结果。上面展示的是展开后的实际地址。");
  }
  if (facts.scopeEffects.mirrorConfigured) {
    risks.push_back(L"仓库里有 remote." + facts.chosenRemoteName +
                    L".mirror：不中和它的话，带 refspec 的 push 会被 Git 直接拒绝（实测 128）。"
                    L"这次只对这个子进程临时置为 false，配置文件一个字不写。");
  }
  if (facts.scopeEffects.tagOptConfigured || facts.scopeEffects.followTagsConfigured) {
    risks.push_back(L"仓库里有会让推送顺带带上标签的设置（remote." + facts.chosenRemoteName +
                    L".tagOpt / push.followTags）：实测带显式 refspec 时它们并不生效，"
                    L"本程序仍然为这一个子进程把它们关掉——绝不多推一个标签。");
  }
  if (plan.setUpstreamRequested) {
    risks.push_back(L"这次除了推送，还要往这个仓库的本地配置里写两条分支设置（下面第 2 步逐条列出）。"
                    L"推送没成功时不会执行这一步；两步都只影响「" + facts.branchName +
                    L"」这一条分支的上游记录，不动任何别的配置。");
  }
  plan.requiresForce = !risks.empty();
  plan.risks = std::move(risks);

  // ---- 不要求点头、但必须交代的事 ----
  std::vector<std::wstring> notes;
  notes.push_back(L"这一步的预检里已经访问过远端了：向每一个发布地址问那句「有没有这条引用」用的是 "
                  L"只读 ls-remote（不改任何一边，需要认证时按你自己的凭据方式处理）。"
                  L"它问不到时的措辞是「没能问出来」，不会写成「对端没有」。");
  const std::wstring pushDefaultDisplay = facts.scopeEffects.pushDefault.empty()
                                              ? std::wstring(L"没设，Git 的默认是 simple")
                                              : facts.scopeEffects.pushDefault;
  notes.push_back(L"命令里写的是完整两侧的显式 refspec " + plan.pushedObjectId + L":" +
                  plan.targetBranchRef + L"：源侧钉死在你确认的这份提交上（不写分支名），"
                  L"目标侧就是这一个引用。push.default（现在是 " + pushDefaultDisplay +
                  L"）与 remote.<远端>.push 都不参与，也不会顺带推标签或所有分支。");
  if (!facts.publishUrlNote.empty()) {
    notes.push_back(facts.publishUrlNote);
  }
  if (!facts.relationshipKnown && facts.presenceKnown && facts.remoteRefExists &&
      facts.remoteObjectId != facts.headObjectId && !facts.presenceDisagrees) {
    notes.push_back(L"快进与否没能判断出来时，本程序只把「命令不带 --force」这一条交给 Git。");
  }
  plan.notes = std::move(notes);

  plan.notice =
      L"首次推送范围：只把 " + plan.localBranchRef + L" 现在这份提交（完整 ID " + plan.pushedObjectId +
      L"，命令源侧写死它）送到远端「" + plan.remoteName + L"」的 " + plan.targetBranchRef +
      L"（发布目标：" + plan.remoteUrlDisplay + L"）。不带 --force / --force-with-lease / --mirror / "
      L"--all / --tags，不递归子模块，不推标签，不动本地分支、索引与工作区。" +
      (plan.setUpstreamRequested ? std::wstring(L"上游那两条 git config 只在推送成功之后才发，"
                                                L"各自的成败逐条报告，不自动重试。")
                                 : std::wstring(L"本次不写任何配置文件。"));

  std::wstring text;
  text += L"这次首次推送要钉死的几样东西：\n";
  text += L" · 源分支：" + plan.localBranchRef + L"（现在在 " + ShortObjectId(plan.pushedObjectId) +
          L"，完整 ID " + plan.pushedObjectId + L"）\n";
  text += L" · 你选的发布远端：" + plan.remoteName + L"（这个仓库的远端清单里有它）\n";
  text += L" · 发布 URL（`git remote get-url --push --all` 由 Git 展开的实际地址，逐条列出）：" +
          plan.remoteUrlDisplay + L"\n";
  text += L" · 你选的目标分支：" + plan.targetBranchRef + L"（这个名字的可用性由 Git 的 check-ref-format " +
          facts.refFormatDetail + L"）\n";
  text += L" · " + PresenceDetail(facts) + L"\n";
  const std::wstring relationship = RelationshipDetail(facts);
  if (!relationship.empty()) {
    text += relationship + L"\n";
  }
  if (facts.upstreamConfigured) {
    text += L" · 上游：已经被设好了（" + facts.upstreamRemote + L"）\n";
  } else {
    text += L" · 上游：branch." + plan.branchName +
            L".remote / .merge 现在都没有配置——这正是走这个向导的原因。\n";
  }

  text += L"\n这件事分成两步，各自有结果，绝不合并成一句话：\n";
  text += L"第 1 步（推送）：在命令窗口里执行 " + plan.commandLabel + L"\n";
  if (!input.repositoryDirectory.empty()) {
    text += L"（工作目录：" + std::wstring(input.repositoryDirectory) + L"）\n";
  }
  text += L"  命令里没有 --force，也没有 --force-with-lease / --mirror / --all / --tags：Git 认为"
          L"这次不是快进时就会把它拒绝，本程序不会为了让它「成功」而更激烈。\n";
  text += L"  没有 --no-verify：pre-push hook 与签名设置照常生效；需要口令时由命令窗口里 Git 自己提问，"
          L"沿用你已有的认证方式（本程序不经手、也不显示任何凭据）。\n";
  // 会被这一条命令就地中和的设置：与普通推送同一份措辞、同一批判定（两处各写一套就会出现
  // 「一个入口说了、另一个入口漏说」的中和项）。
  text += DescribePushNeutralization(plan.remoteName, facts.scopeEffects);
  text += L"  命令窗口报告结束后，本程序还会向上面列出的发布目标逐个发只读 ls-remote，核对那条引用"
          L"到底停在哪：推送成功与否以那份实况为准。命令报了成功却没核上时，结论写成"
          L"「已推送但未核实」或「与预期不符」，不自动重推。\n";
  text += L"第 2 步（设置本地上游）：";
  if (plan.setUpstreamRequested) {
    text += L"推送那一步报告成功后，再在命令窗口里**分别**执行下面 " +
            std::to_wstring(plan.upstreamSteps.size()) + L" 条命令，每条只管一把配置键：\n";
    for (size_t index = 0; index < plan.upstreamSteps.size(); ++index) {
      const UpstreamWriteStep& step = plan.upstreamSteps[index];
      text += L"  " + std::to_wstring(index + 1) + L") " + step.commandLabel + L"\n";
      text += L"     写入 " + step.key + L" = " + step.value + L"（" + step.purpose + L"）\n";
    }
    text += L"  适用的分支只有 " + plan.branchName + L"（键名里带着它），别的分支一个字都不动。\n";
    text += L"  配置文件落点：" +
            (facts.configFilePath.empty()
                 ? (facts.configFilePathFailure.empty() ? std::wstring(L"（没去问）")
                                                         : facts.configFilePathFailure)
                 : std::wstring(L"由 `git rev-parse --git-path config` 当场问回：" + facts.configFilePath)) +
            L"\n";
    text += L"  为什么不用 git push -u：-u 会把「远端收到没有」与「本地配置写成没有」揉进同一条命令，"
            L"配置没写成只在 Git 的输出里留一句话，本程序就没办法把两个结果分开报告。这里不发 -u，"
            L"也不为了用 -u 把命令的源侧退回成会变的分支名。\n";
    text += L"  两条各自报告：第一条失败时不再发第二条（那种场合上游配置只写了一半，"
            L"照原样留着并如实告诉你写到哪一步）；都成功才算设置完成。任何一步失败都不自动重发。\n";
  } else {
    text += L"这次**不做**（你选了「只推送，不改本地配置」）。这条分支推送之后仍然没有上游，"
            L"pull 与普通推送会继续拒绝它，那是你的选择，本程序不替你改主意。\n";
  }

  if (!plan.risks.empty()) {
    text += L"\n风险提示（本程序不认为这些可以忽略）：\n";
    for (size_t index = 0; index < plan.risks.size(); ++index) {
      text += L" " + std::to_wstring(index + 1) + L") " + plan.risks[index] + L"\n";
    }
  } else {
    text += L"\n预检没有发现问题：这条分支确实还没有上游，对端也还没有那条引用，"
            L"这次会在发布目标上新建它。\n";
  }
  if (!plan.notes.empty()) {
    text += L"\n还要交代的：\n";
    for (const std::wstring& note : plan.notes) {
      text += L" · " + note + L"\n";
    }
  }
  text += L"\n预检不是保证。点头之后、执行之前，本程序会把上面这套只读查询**原样重发一遍**"
          L"（分支、这份提交、「上游还是没有」、选定远端在不在清单里、展开后的发布 URL、"
          L"引用名的 Git 裁定、对端那条引用的位置、会被中和的设置），对不上就作废、不发命令。"
          L"对上了也仍有一个挡不住的窗口：复核通过到 Git 自己启动并读取配置之间，外部进程仍可能"
          L"改动 remote 指向，远端也可能又被别人推进——本程序不持有（也不该持有）Git 的锁。"
          L"源侧因为写死了完整提交 ID，改动「送哪一份」已经不在那个窗口里，剩下的只有「送去哪里」，"
          L"而那由推送后的逐目标核实兜底。\n";
  text += L"取消这一步：不打开命令窗口、不接触远端、不写任何配置。";
  plan.confirmationText = std::move(text);
  plan.explanation = L"首次推送：" + plan.localBranchRef + L" → 远端「" + plan.remoteName + L"」的 " +
                     plan.targetBranchRef + L"（" + plan.remoteUrlDisplay + L"）";
  return plan;
}

// ---- 點頭之後、啟動命令之前的執行前復核 ----

std::wstring DescribeFirstPushChange(const FirstPushFacts& preflight, const FirstPushFacts& latest) {
  if (!latest.queryOk) {
    return L"执行前的复核没能完成：" +
           (latest.queryFailure.empty() ? std::wstring(L"原因未知") : latest.queryFailure) +
           L"。看不清要推的是哪一份提交、送到哪里，就不发命令：本次没有打开命令窗口，"
           L"没有接触远端，也没有写任何配置。";
  }
  if (!latest.configOk) {
    return L"执行前的复核没能问回这个仓库的生效配置，远端与发布目标无从确认。本次没有发出任何命令。";
  }

  std::vector<std::wstring> changes;
  if (latest.branchRef != preflight.branchRef) {
    changes.push_back(L"当前分支从「" +
                      (preflight.branchRef.empty() ? std::wstring(L"不在分支上") : preflight.branchRef) +
                      L"」变成了「" +
                      (latest.branchRef.empty() ? std::wstring(L"不在分支上") : latest.branchRef) + L"」");
  }
  if (latest.headObjectId != preflight.headObjectId) {
    changes.push_back(L"要推送的提交从 " + ShortOr(preflight.headObjectId, L"（读不到）") + L" 变成了 " +
                      ShortOr(latest.headObjectId, L"（读不到）") +
                      L"（点头之后本地又提交过、或分支被外部挪过）");
  }
  if (latest.upstreamConfigured) {
    changes.push_back(L"这条分支的上游已经被设好了（远端「" + latest.upstreamRemote + L"」的 " +
                      latest.upstreamTrackingRef +
                      L"）：那种场合该走普通推送，目标由仓库配置说清楚，"
                      L"不该拿这份「自己挑的目标」去执行。");
  }
  if (latest.chosenRemoteName != preflight.chosenRemoteName) {
    changes.push_back(L"选定的远端名字与预检时对不上（预检是「" + preflight.chosenRemoteName +
                      L"」，现在是「" + latest.chosenRemoteName + L"」）");
  }
  if (!latest.chosenRemoteExists && preflight.chosenRemoteExists) {
    changes.push_back(L"远端「" + latest.chosenRemoteName +
                      L"」在生效配置里已经没有地址了（既没有 url 也没有 pushurl）");
  }
  if (latest.publishUrls != preflight.publishUrls) {
    changes.push_back(L"发布 URL 也不再是刚刚那一批：\n  预检时：" + FormatPushUrlList(preflight.publishUrls) +
                      L"\n  现在：" + FormatPushUrlList(latest.publishUrls));
  }
  if (latest.targetBranchRef != preflight.targetBranchRef) {
    changes.push_back(L"目标引用的形态与预检时对不上（预检是 " + preflight.targetBranchRef + L"，现在是 " +
                      latest.targetBranchRef + L"）");
  }
  if (latest.refFormatAccepted != preflight.refFormatAccepted) {
    changes.push_back(L"Git 对那个引用名的裁定也变了（现在：" + latest.refFormatDetail + L"）");
  }
  if (latest.remoteObjectId != preflight.remoteObjectId ||
      latest.presenceKnown != preflight.presenceKnown ||
      latest.remoteRefExists != preflight.remoteRefExists) {
    changes.push_back(L"对端那条引用的现状与预检时不一样了：预检「" +
                      (preflight.presenceKnown
                           ? (preflight.remoteRefExists
                                  ? L"已有，停在 " + ShortOr(preflight.remoteObjectId, L"（没读回来）")
                                  : L"还没有")
                           : L"问不到") +
                      L"」，现在「" +
                      (latest.presenceKnown
                           ? (latest.remoteRefExists ? L"已有，停在 " +
                                                        ShortOr(latest.remoteObjectId, L"（没读回来）")
                                                      : L"还没有")
                           : L"问不到") +
                      L"」");
  }
  if (latest.scopeEffects.mirrorConfigured != preflight.scopeEffects.mirrorConfigured ||
      latest.scopeEffects.tagOptConfigured != preflight.scopeEffects.tagOptConfigured ||
      latest.scopeEffects.followTagsConfigured != preflight.scopeEffects.followTagsConfigured ||
      latest.scopeEffects.extraPushRefspecsConfigured !=
          preflight.scopeEffects.extraPushRefspecsConfigured) {
    changes.push_back(L"仓库里影响推送范围的配置与预检时不一样了（mirror / tagOpt / push.followTags / "
                      L"remote.<远端>.push 至少有一项变了）");
  }
  if (latest.relationshipKnown != preflight.relationshipKnown ||
      latest.remoteOnly != preflight.remoteOnly) {
    changes.push_back(L"本地与对端的领先/落后关系也变了（现在：" +
                      (latest.relationshipKnown
                           ? std::to_wstring(latest.remoteOnly) + L" 个只有对端有、" +
                                 std::to_wstring(latest.localOnly) + L" 个只有本地有"
                           : std::wstring(L"没能问出来")) +
                      L"）");
  }
  if (changes.empty()) {
    return std::wstring();
  }
  std::wstring text = L"点头之后、执行之前，仓库、远端与对端又变了：\n";
  for (const std::wstring& change : changes) {
    text += L" · " + change + L"\n";
  }
  text += L"本次没有发出任何命令，也没有写任何配置：预检得出的那份方案已经不属于此刻的这个仓库，"
          L"照着它执行就是拿旧结论推新东西。工作区与索引的改动不在这次的比对范围里"
          L"（推送本来就不碰它们）。仓库状态正在重读，看清现状后如仍要推送请再点一次。";
  return text;
}

}  // namespace gc::git
