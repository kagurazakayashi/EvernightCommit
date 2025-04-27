#include "git/fetch_plan.h"

#include <string>
#include <vector>

#include "git/push_plan.h"  // MaskPushUrlCredentials：确认框里展示 URL 前先过一次凭据掩码

namespace gc::git {
namespace {

constexpr std::wstring_view kHeadsPrefix = L"refs/heads/";
constexpr std::wstring_view kFetchSuffix = L" (fetch)";
constexpr std::wstring_view kPushSuffix = L" (push)";

bool EndsWith(std::wstring_view text, std::wstring_view suffix) noexcept {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring DisplayUrl(const FetchRemoteEntry& entry) {
  return entry.fetchUrl.empty() ? std::wstring(L"（这个远端没有记录 fetch URL）")
                                : MaskPushUrlCredentials(entry.fetchUrl);
}

// 一份「范围已经核得住」的 ready 方案：参数、命令展示、范围说明与确认正文全部取自
// git/fetch_scope 的决策——界面 fetch 按钮与 pull 第一步因此拿到的是同一份承诺。
FetchPlan ReadyFromScope(const FetchScopeDecision& scope, const FetchRemoteEntry& entry,
                         std::wstring_view sourceSentence, std::wstring_view repositoryDirectory) {
  FetchPlan plan;
  plan.state = FetchPlanState::ready;
  plan.operationId = L"fetch";
  plan.displayName = L"fetch";
  plan.remoteName = entry.name;
  plan.remoteUrl = entry.fetchUrl;
  plan.arguments = scope.arguments;
  plan.commandLabel = scope.commandLabel;
  plan.notice = L"fetch 范围：" + scope.noticeCore;
  plan.confirmationText = BuildFetchConfirmationText(scope, entry.name, DisplayUrl(entry), sourceSentence,
                                                     repositoryDirectory);
  return plan;
}

FetchPlan BlockedPlan(std::wstring reason) {
  FetchPlan plan;
  plan.state = FetchPlanState::blocked;
  plan.explanation = std::move(reason);
  plan.operationId = L"fetch";
  plan.displayName = L"fetch";
  return plan;
}

FetchPlan ChoosePlan(std::wstring reason, std::vector<FetchRemoteEntry> candidates) {
  FetchPlan plan;
  plan.state = FetchPlanState::chooseRemote;
  plan.explanation = std::move(reason);
  plan.candidates = std::move(candidates);
  plan.operationId = L"fetch";
  plan.displayName = L"fetch";
  return plan;
}

const FetchRemoteEntry* FindRemote(const std::vector<FetchRemoteEntry>& remotes,
                                   std::wstring_view name) {
  for (const FetchRemoteEntry& entry : remotes) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

std::vector<std::wstring> BuildFetchSymbolicRefArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"symbolic-ref", L"--quiet", L"HEAD"};
}

std::vector<std::wstring> BuildFetchBranchRemoteArguments(std::wstring_view repositoryDirectory,
                                                          std::wstring_view branchName) {
  if (branchName.empty()) {
    return {};  // 不在分支上就没有「分支配置的远端」可问；调用方据此跳过这条查询。
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"config", L"--get", L"branch." + std::wstring(branchName) + L".remote"};
}

std::vector<std::wstring> BuildFetchRemotesArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"remote", L"-v"};
}

FetchTargetFacts InterpretFetchTarget(const FetchTargetQueries& queries) {
  FetchTargetFacts facts;
  // 抓取范围的配置先读回来：它按远端名归组，与「目标是谁」是两件独立的事，
  // 后面无论目标是分支配置给的还是用户当场选的，这同一份事实都可用。
  facts.scope = InterpretFetchScope(queries.scope);

  const UndoQueryRead symbolic = ReadUndoQuery(queries.symbolicRef);
  if (symbolic.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出当前分支：" + symbolic.detail;
    return facts;
  }
  if (symbolic.outcome == UndoQueryOutcome::answered) {
    const std::wstring_view ref = symbolic.firstLine;
    if (ref.size() > kHeadsPrefix.size() && ref.compare(0, kHeadsPrefix.size(), kHeadsPrefix) == 0) {
      facts.onBranch = true;
      facts.branchName = std::wstring(ref.substr(kHeadsPrefix.size()));
    }
    // symbolic-ref 给出 refs/heads/ 以外的引用是合法但罕见的形态：按「没有分支配置可问」
    // 处理（branch.<name> 的键名本来就取分支名），不当成查询失败。
  }

  if (facts.onBranch && queries.branchRemoteRan) {
    const UndoQueryRead configured = ReadUndoQuery(queries.branchRemote);
    if (configured.outcome == UndoQueryOutcome::failed) {
      facts.queryFailure = L"没能问出当前分支配置的远端（branch." + facts.branchName +
                           L".remote）：" + configured.detail;
      return facts;
    }
    if (configured.outcome == UndoQueryOutcome::answered) {
      facts.configuredRemote = configured.firstLine;
    }
    // noResult：Git 明确回答「没有这个配置」，configuredRemote 留空，交给远端清单去判。
  }

  const UndoQueryRead remoteRead = ReadUndoQuery(queries.remotes);
  if (remoteRead.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出这个仓库的远端清单：" + remoteRead.detail;
    return facts;
  }
  if (remoteRead.outcome != UndoQueryOutcome::answered) {
    // `git remote -v` 成功时恒以退出码 0 作答（没有远端就是空输出）；不合约定的回答按失败处理。
    facts.queryFailure = L"远端清单的查询回答不符合约定（意外的退出码），不在这上面猜目标。";
    return facts;
  }

  // 解析 git remote -v：一行一条「名字<TAB>URL (fetch)」或「(push)」。
  // 同名远端通常 fetch/push 各一行：URL 取第一条 (fetch)；只有 (push) 时退而用之（仅作展示）。
  // 没有制表符的行按纯名字输出（git remote）处理，URL 留空。
  std::vector<FetchRemoteEntry> remotes;
  std::vector<std::wstring> pushFallback;  // 与 remotes 同序：仅 (push) 行的 URL
  bool malformed = false;
  for (const std::wstring& line : remoteRead.lines) {
    const size_t tab = line.find(L'\t');
    if (tab == std::wstring::npos) {
      if (!line.empty() && FindRemote(remotes, line) == nullptr) {
        remotes.push_back(FetchRemoteEntry{line, L""});
        pushFallback.emplace_back();
      }
      continue;
    }
    const std::wstring name = line.substr(0, tab);
    // 必须持有实际字符串：这里若用 wstring_view 指向 substr 的临时对象，
    // 临时量在语句结束即析构，view 当场悬空（曾让整份远端清单被解析成空）。
    const std::wstring rest = line.substr(tab + 1);
    std::wstring url;
    bool isFetch = false;
    bool isPush = false;
    if (EndsWith(rest, kFetchSuffix)) {
      url = TrimWide(rest.substr(0, rest.size() - kFetchSuffix.size()));
      isFetch = true;
    } else if (EndsWith(rest, kPushSuffix)) {
      url = TrimWide(rest.substr(0, rest.size() - kPushSuffix.size()));
      isPush = true;
    } else {
      malformed = true;
      continue;
    }
    bool merged = false;
    for (size_t index = 0; index < remotes.size() && !merged; ++index) {
      if (remotes[index].name != name) {
        continue;
      }
      merged = true;
      if (isFetch && remotes[index].fetchUrl.empty()) {
        remotes[index].fetchUrl = url;
      }
      if (isPush && pushFallback[index].empty() && remotes[index].fetchUrl.empty()) {
        pushFallback[index] = url;
      }
    }
    if (!merged) {
      remotes.push_back(FetchRemoteEntry{name, isFetch ? url : L""});
      pushFallback.emplace_back(isPush && !isFetch ? url : std::wstring());
    }
  }
  // 只有 push URL 的远端（罕见：fetch URL 被单独删掉）退用 push URL 展示；
  // 两者都没有的条目保留空 URL——命令仍然只传名字，行不行由 Git 自己说。
  for (size_t index = 0; index < remotes.size(); ++index) {
    if (remotes[index].fetchUrl.empty() && !pushFallback[index].empty()) {
      remotes[index].fetchUrl = pushFallback[index];
    }
  }

  facts.remoteListOk = true;
  facts.remotes = std::move(remotes);
  if (malformed) {
    facts.remoteListDetail = L"远端清单里有不符合约定的行，已按能解析的部分继续。";
  }
  facts.queryOk = true;
  return facts;
}

FetchPlan BuildFetchPlan(const FetchTargetFacts& facts, std::wstring_view repositoryDirectory) {
  if (!facts.queryOk) {
    return BlockedPlan((facts.queryFailure.empty() ? std::wstring(L"远端目标的只读查询没能完成。")
                                                   : facts.queryFailure) +
                       L"\n\n没有发出任何命令，也没有改动仓库。点“刷新”确认仓库现状后可以再试。");
  }

  const FetchRemoteEntry* configured =
      facts.configuredRemote.empty() ? nullptr : FindRemote(facts.remotes, facts.configuredRemote);

  if (configured != nullptr) {
    // 当前分支明确配置的远端，而且它确实在远端清单里：这就是「可确定的目标」。
    // 目标定了还得这个远端的抓取范围核得住，否则一样不发命令。
    const FetchScopeDecision scope = DecideFetchScope(facts.scope, configured->name);
    if (!scope.allowed) {
      return BlockedPlan(scope.refusal);
    }
    FetchPlan ready = ReadyFromScope(
        scope, *configured,
        L"当前分支明确配置的远端（branch." + facts.branchName + L".remote = " + configured->name + L"）",
        repositoryDirectory);
    ready.explanation = L"抓取目标是当前分支配置的远端「" + configured->name + L"」。";
    return ready;
  }

  if (facts.remotes.empty()) {
    std::wstring reason = L"这个仓库没有配置任何远端，fetch 需要一个抓取的对象。";
    if (!facts.configuredRemote.empty()) {
      reason += L"当前分支的配置指向远端「" + facts.configuredRemote +
                L"」，但 git remote 的清单里没有这个远端。";
    }
    reason += L"本程序不替你猜一个 origin、不创建远端、也不改动任何配置。"
              L"请先用 Git 把远端配好（git remote add …），再回来点 fetch。";
    return BlockedPlan(std::move(reason));
  }

  // 目标定不下来，但既有远端可以摆出来让人选——选谁由用户明确点头，程序不猜。
  std::wstring reason;
  if (!facts.configuredRemote.empty()) {
    reason = L"当前分支配置的远端「" + facts.configuredRemote +
             L"」不在这个仓库的既有远端里（配置与远端清单对不上）。本程序不修改你的配置，"
             L"也不替你猜它想指哪个。";
  } else if (facts.onBranch) {
    reason = L"当前分支 " + facts.branchName +
             L" 没有配置远端（branch." + facts.branchName +
             L".remote 未设置）。本程序不替你猜 origin。";
  } else {
    reason = L"现在不在任何分支上（游离 HEAD），没有分支配置可参考。本程序不替你猜 origin。";
  }
  reason += facts.remoteListDetail;
  reason += L"\n\n请在下面的既有远端里选择抓取目标：";
  return ChoosePlan(std::move(reason), facts.remotes);
}

FetchPlan ChooseFetchRemote(const FetchTargetFacts& facts, std::wstring_view remoteName,
                            std::wstring_view repositoryDirectory) {
  if (!facts.queryOk || !facts.remoteListOk) {
    return BlockedPlan(L"远端清单没能读回来，选择无从核对，没有发出任何命令。请点“刷新”后重试。");
  }
  const FetchRemoteEntry* entry = FindRemote(facts.remotes, remoteName);
  if (entry == nullptr) {
    return BlockedPlan(L"选中的远端「" + std::wstring(remoteName) +
                       L"」已经不在刚刚读回的清单里（仓库在外部被改过？）。"
                       L"没有发出任何命令，请重新点 fetch 再选。");
  }
  // 选完立刻按同一份范围策略核对：能中和的中和，核不住的映射在执行前拒绝（不改配置、不猜映射）。
  const FetchScopeDecision scope = DecideFetchScope(facts.scope, entry->name);
  if (!scope.allowed) {
    return BlockedPlan(scope.refusal);
  }
  FetchPlan ready = ReadyFromScope(
      scope, *entry, L"你在远端选择界面里刚选定的既有远端（不是猜的：分支配置里没有指定它）",
      repositoryDirectory);
  ready.explanation = L"抓取目标是你在选择界面挑的既有远端「" + entry->name + L"」。";
  return ready;
}

}  // namespace gc::git
