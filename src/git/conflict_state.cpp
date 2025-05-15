#include "git/conflict_state.h"

#include <algorithm>
#include <string>

namespace gc::git {
namespace {

// 按 NUL 拆原始輸出（路徑裡可能有空格、中文、&），與 pull 的未合併清單同一套拆法。
std::vector<std::wstring> SplitNulPaths(const std::wstring& text) {
  std::vector<std::wstring> parts;
  std::wstring current;
  for (const wchar_t c : text) {
    if (c == L'\0') {
      if (!current.empty()) {
        parts.push_back(current);
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  return parts;
}

// 「問不到」與「明確沒有」分開：只有 Git 明確答「沒有這條引用」時才算不在分支上。
struct RefRead {
  bool answered = false;
  bool exists = false;
  std::wstring value;
  std::wstring detail;
};

RefRead ReadRefQuery(const GitQueryResult& result, std::wstring_view queryName) {
  RefRead read;
  const UndoQueryRead parsed = ReadUndoQuery(result);
  switch (parsed.outcome) {
    case UndoQueryOutcome::answered:
      read.answered = true;
      read.exists = !parsed.firstLine.empty();
      read.value = parsed.firstLine;
      break;
    case UndoQueryOutcome::noResult:
      // --quiet 系查詢的退出碼 1 + 空輸出：Git 明確回答「沒有」。
      read.answered = true;
      read.exists = false;
      break;
    case UndoQueryOutcome::failed:
      read.answered = false;
      read.detail = parsed.detail.empty() ? std::wstring(queryName) + L" 没问出结果" : parsed.detail;
      break;
  }
  return read;
}

std::wstring ShortOrUnknown(const std::wstring& objectId, std::wstring_view missingNote) {
  if (LooksLikeFullObjectId(objectId)) {
    return ShortObjectId(objectId);
  }
  return std::wstring(missingNote);
}

// 列表的展示形態：超過上限只列前若干條並說明其餘有多少，絕不把半份清單說成全部。
std::wstring FormatPaths(const std::vector<std::wstring>& paths) {
  std::wstring text;
  const size_t shown = std::min(paths.size(), kConflictListedPathCap);
  for (size_t index = 0; index < shown; ++index) {
    text += index == 0 ? L"\n  · " : L"\n  · ";
    text += paths[index];
  }
  if (paths.size() > shown) {
    text += L"\n  · …（其余 " + std::to_wstring(paths.size() - shown) + L" 个没在这里列出）";
  }
  return text;
}

std::wstring Bullet(std::wstring head, std::wstring tail) {
  return L"  · " + std::move(head) + std::move(tail) + L"\n";
}

// head-name 只在形態像一條引用時才拿去展示：那是 Git 寫下來的引用名，不是本程序拼出來的；
// 形態不像就承認「這條問不準」，不猜。
bool RefShapeLooksLikeHeadName(std::wstring_view ref) {
  constexpr std::wstring_view kPrefix = L"refs/";
  if (ref.size() <= kPrefix.size()) {
    return false;
  }
  if (ref.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }
  return ref.find(L'\n') == std::wstring_view::npos && ref.find(L'\t') == std::wstring_view::npos;
}

// 檔案裡讀回來的步數必須是純十進位數字才拿去展示：別的形態（空白、混著別的字）一律當「問不準」。
bool IsDecimalToken(const std::wstring& token) {
  if (token.empty() || token.size() > 9) {
    return false;
  }
  return std::all_of(token.begin(), token.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

// 痕跡名单的展示形態（頓號分隔）：多種並存時要把看到了什麼全列出來，不能只報第一個。
std::wstring JoinMarkers(const std::vector<std::wstring>& names) {
  std::wstring text;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0) {
      text += L"、";
    }
    text += names[index];
  }
  return text;
}

}  // namespace

std::wstring ConflictFlowKindLabel(ConflictFlowKind kind) {
  switch (kind) {
    case ConflictFlowKind::unreadable:
      return L"没能探到这个仓库的流程痕迹（不知道有没有流程停着）";
    case ConflictFlowKind::none:
      return L"没有 Git 流程停在进行中";
    case ConflictFlowKind::merge:
      return L"合并停在中间（MERGE_HEAD）";
    case ConflictFlowKind::rebaseMergeBackend:
      return L"变基停在中间（rebase-merge\\，即默认的合并后端）";
    case ConflictFlowKind::rebaseApplyBackend:
      return L"变基停在中间（rebase-apply\\，apply 后端）";
    case ConflictFlowKind::cherryPick:
      return L"拣选停在中间（CHERRY_PICK_HEAD）";
    case ConflictFlowKind::revert:
      return L"撤销停在中间（REVERT_HEAD）";
    case ConflictFlowKind::bisect:
      return L"二分定位停在中间（BISECT_LOG）";
    case ConflictFlowKind::squashOnly:
      return L"只有 SQUASH_MSG：--squash 那种「改动已进暂存区、没有 MERGE_HEAD」的停法";
    case ConflictFlowKind::mixed:
      return L"同时看到多种互不相容的流程痕迹（状态不一致）";
    case ConflictFlowKind::ambiguousRebase:
      return L"只有 rebase-apply\\ 且形态读不回来：分不清是变基（apply 后端）还是 git am";
  }
  return L"没能探到这个仓库的流程痕迹（不知道有没有流程停着）";
}

ConflictStateFacts InterpretConflictState(const ConflictMarkerFacts& markers,
                                          const GitQueryResult& unmergedListing,
                                          const GitQueryResult& symbolicRef,
                                          const GitQueryResult& headObject) {
  ConflictStateFacts facts;
  facts.probed = markers.probed;
  facts.probeFailure = markers.probeFailure;
  facts.indexLock = markers.indexLock;
  facts.sequencerPending = markers.sequencerDir;
  facts.autostashEntry = markers.mergeAutostash || markers.rebaseAutostash;
  facts.rebaseInteractive = markers.rebaseInteractiveMark;
  facts.unreadableContents = markers.contentFailures;

  // ---- 三條只讀查詢：當前分支 / HEAD 現在在哪 / 索引裡還有誰未合併 ----
  const RefRead branch = ReadRefQuery(symbolicRef, L"git symbolic-ref HEAD");
  facts.branchQueried = branch.answered;
  if (branch.answered && branch.exists) {
    facts.branchRef = branch.value;
    facts.onBranch = true;
  }

  const RefRead head = ReadRefQuery(headObject, L"git rev-parse HEAD");
  facts.headQueried = head.answered;
  if (head.answered && head.exists && LooksLikeFullObjectId(head.value)) {
    facts.headObjectId = head.value;
  }

  const UndoQueryRead listing = ReadUndoQuery(unmergedListing);
  if (listing.outcome != UndoQueryOutcome::answered) {
    facts.unmergedReadFailure =
        listing.detail.empty() ? std::wstring(L"git diff --diff-filter=U 没成功") : listing.detail;
    facts.unmergedReadFailure = L"没能问出索引里还有哪些未合并条目：" + facts.unmergedReadFailure;
  } else {
    std::wstring recordReason;
    if (!NulRecordsAreComplete(unmergedListing.utf16Output, recordReason)) {
      // 清單殘缺就不能当成「冲突只有这些」：漏掉的那一条正是下一步要解决的文件。
      facts.unmergedReadFailure = L"未合并条目的清单不符合记录约定：" + recordReason;
    } else {
      facts.unmergedPaths = SplitNulPaths(unmergedListing.utf16Output);
      facts.unmergedReadOk = true;
    }
  }

  // ---- 痕跡判讀 ----
  const auto push = [&facts](std::wstring_view name) {
    if (std::find(facts.markersSeen.begin(), facts.markersSeen.end(), std::wstring(name)) ==
        facts.markersSeen.end()) {
      facts.markersSeen.push_back(std::wstring(name));
    }
  };
  if (markers.mergeHead) {
    push(L"MERGE_HEAD");
  }
  if (markers.cherryPickHead) {
    push(L"CHERRY_PICK_HEAD");
  }
  if (markers.revertHead) {
    push(L"REVERT_HEAD");
  }
  if (markers.bisectLog) {
    push(L"BISECT_LOG");
  }
  if (markers.rebaseMergeDir) {
    push(L"rebase-merge\\");
  }
  if (markers.rebaseApplyDir) {
    push(L"rebase-apply\\");
  }
  if (markers.squashMsg) {
    push(L"SQUASH_MSG");
  }
  if (markers.mergeMode) {
    push(L"MERGE_MODE");
  }
  if (markers.sequencerDir) {
    push(L"sequencer\\");
  }
  if (markers.mergeAutostash) {
    push(L"MERGE_AUTOSTASH");
  }
  if (markers.rebaseAutostash) {
    push(L"REBASE_AUTOSTASH");
  }
  if (markers.indexLock) {
    push(L"index.lock");
  }

  if (!markers.probed) {
    facts.kind = ConflictFlowKind::unreadable;
    if (facts.probeFailure.empty()) {
      facts.probeFailure = L"这个仓库的 Git 目录没能探到（界面绑定的仓库没有可用的绝对 Git 目录）。";
    }
    facts.continueBlockedReason = facts.probeFailure;
    facts.abortBlockedReason = facts.probeFailure;
    return facts;
  }

  const bool hasRebase = markers.rebaseMergeDir || markers.rebaseApplyDir;
  // 變基/序列進行中時，CHERRY_PICK_HEAD、REVERT_HEAD、MERGE_HEAD 是序列化器自己留下的同伴痕跡，
  // 不是「兩種流程並存在跑」。只有 rebase 目錄不存在時，這幾條才互斥，並存才算不一致。
  const int independentFlows =
      (markers.mergeHead ? 1 : 0) + (markers.cherryPickHead ? 1 : 0) +
      (markers.revertHead ? 1 : 0) + (hasRebase ? 1 : 0);

  if (markers.bisectLog && (independentFlows > 0 || hasRebase)) {
    facts.kind = ConflictFlowKind::mixed;
  } else if (markers.bisectLog) {
    facts.kind = ConflictFlowKind::bisect;
  } else if (markers.rebaseMergeDir) {
    facts.kind = ConflictFlowKind::rebaseMergeBackend;
  } else if (markers.rebaseApplyDir) {
    // apply 後端的 rebase 與 git am 共用同一個目錄名：只有 head-name 與 onto 都讀得回，
    // 才有依據說這是變基（am 不寫 head-name）。讀不回來就承認分不清，兩條命令都不給。
    const bool shapeKnown = RefShapeLooksLikeHeadName(markers.rebaseHeadName) && LooksLikeFullObjectId(markers.rebaseOnto);
    facts.kind = shapeKnown ? ConflictFlowKind::rebaseApplyBackend
                            : ConflictFlowKind::ambiguousRebase;
  } else if (independentFlows > 1) {
    facts.kind = ConflictFlowKind::mixed;
  } else if (markers.mergeHead) {
    facts.kind = ConflictFlowKind::merge;
  } else if (markers.cherryPickHead) {
    facts.kind = ConflictFlowKind::cherryPick;
  } else if (markers.revertHead) {
    facts.kind = ConflictFlowKind::revert;
  } else if (markers.squashMsg || markers.mergeMode) {
    // 沒有 MERGE_HEAD 卻還留著合併意圖檔案：這是 --squash（或 --no-commit 之外的殘留）那一類停法，
    // --continue/--abort 都以 MERGE_HEAD 存在為前提，這裡不猜該走哪一條。
    facts.kind = ConflictFlowKind::squashOnly;
  } else {
    facts.kind = ConflictFlowKind::none;
  }

  // ---- 流程自己的目標 ----
  switch (facts.kind) {
    case ConflictFlowKind::merge:
      facts.flowTarget = LooksLikeFullObjectId(markers.mergeHeadOid)
                             ? L"正在合并进来的那一份提交：" + ShortObjectId(markers.mergeHeadOid)
                             : L"正在合并进来哪一份提交：MERGE_HEAD 的内容没能原样读回，这一条问不到";
      break;
    case ConflictFlowKind::cherryPick:
      facts.flowTarget = LooksLikeFullObjectId(markers.pickHeadOid)
                             ? L"正在拣选的那一份提交：" + ShortObjectId(markers.pickHeadOid)
                             : L"正在拣选哪一份提交：CHERRY_PICK_HEAD 的内容没能原样读回，这一条问不到";
      break;
    case ConflictFlowKind::revert:
      facts.flowTarget = LooksLikeFullObjectId(markers.pickHeadOid)
                             ? L"正在撤销的那一份提交：" + ShortObjectId(markers.pickHeadOid)
                             : L"正在撤销哪一份提交：REVERT_HEAD 的内容没能原样读回，这一条问不到";
      break;
    case ConflictFlowKind::rebaseMergeBackend:
    case ConflictFlowKind::rebaseApplyBackend: {
      // 三份內容都是 Git 自己寫下的檔案值：形態不合格（引用名不像引用、步數不是純十進位）
      // 一律當「問不準」展示，不拿它拼進說明裡冒充事實。
      const bool headNameKnown = RefShapeLooksLikeHeadName(markers.rebaseHeadName);
      facts.rebaseHeadName = headNameKnown ? markers.rebaseHeadName : std::wstring();
      facts.rebaseOntoShort = ShortOrUnknown(markers.rebaseOnto, L"（onto 问不到）");
      const bool progressKnown = IsDecimalToken(markers.rebaseMsgnum) && IsDecimalToken(markers.rebaseEnd);
      facts.rebaseProgress = progressKnown
                                 ? L"第 " + markers.rebaseMsgnum + L" 步 / 共 " + markers.rebaseEnd +
                                       L" 步"
                                 : L"第几步、共几步：没能从 rebase 目录里读回合格的数值";
      facts.flowTarget =
          L"要把 " + (headNameKnown ? markers.rebaseHeadName : std::wstring(L"（head-name 问不到）")) +
          L" 变基到 " + facts.rebaseOntoShort + L"；" + facts.rebaseProgress +
          (markers.rebaseInteractiveMark ? L"（交互式变基的待办停在半路）" : L"");
      break;
    }
    default:
      break;
  }

  // ---- 兩個入口的可用性 ----
  const bool supported = facts.kind == ConflictFlowKind::merge ||
                         facts.kind == ConflictFlowKind::rebaseMergeBackend ||
                         facts.kind == ConflictFlowKind::rebaseApplyBackend ||
                         facts.kind == ConflictFlowKind::cherryPick ||
                         facts.kind == ConflictFlowKind::revert;

  if (!supported) {
    if (facts.kind == ConflictFlowKind::none) {
      facts.continueBlockedReason =
          L"这个仓库里没有 Git 流程停在进行中（既没有 MERGE_HEAD，也没有 rebase-merge\\、"
          L"rebase-apply\\、CHERRY_PICK_HEAD、REVERT_HEAD 这些痕迹）。"
          L"没有流程可继续，也没有流程可中止。";
      facts.abortBlockedReason =
          L"这个仓库里没有 Git 流程停在进行中，因此不会生成任何 --abort 命令："
          L"那种命令在没有流程的仓库里只会被 Git 当场拒绝，本程序不拿它去试。";
    } else if (facts.kind == ConflictFlowKind::bisect) {
      facts.continueBlockedReason =
          L"停在的是二分定位（BISECT_LOG）。它的收尾是 git bisect 自己那一组子命令，"
          L"--continue/--abort 不是它的恢复方式；本程序不接手这一形态，也不猜该怎么收。";
      facts.abortBlockedReason = facts.continueBlockedReason;
    } else if (facts.kind == ConflictFlowKind::squashOnly) {
      facts.continueBlockedReason =
          L"只看到 SQUASH_MSG（或 MERGE_MODE）却没有 MERGE_HEAD。--continue 与 --abort 都以 "
          L"MERGE_HEAD 存在为前提，这种停法两条都不适用：剩下的收尾要么由你提交那份暂存内容，"
          L"要么由你自己决定怎么退回它，本程序不代为决定、也不代为执行。";
      facts.abortBlockedReason = facts.continueBlockedReason;
    } else if (facts.kind == ConflictFlowKind::mixed) {
      facts.continueBlockedReason =
          L"同时看到了互不相容的多种流程痕迹（" + JoinMarkers(facts.markersSeen) +
          L"）。这种状态本程序解释不了，更不会猜一条「大概能恢复」的命令发出去："
          L"请先自己在终端里核对是哪一层留下的痕迹。";
      facts.abortBlockedReason = facts.continueBlockedReason;
    } else if (facts.kind == ConflictFlowKind::ambiguousRebase) {
      facts.continueBlockedReason =
          L"只有 rebase-apply\\ 这一层，而且里面的 head-name/onto 没能读回来。"
          L"git am 用的也是同一个目录名，所以此刻分不清这是「变基（apply 后端）」还是「git am」："
          L"本程序不猜，两条命令都不给。若是 git am，它自己的恢复方式是 git am --continue/--abort/"
          L"--show-current-patch，那不在本程序的接手范围内。";
      facts.abortBlockedReason = facts.continueBlockedReason;
    } else {
      facts.continueBlockedReason = L"现在这个现场读不出可以继续的依据。";
      facts.abortBlockedReason = L"现在这个现场读不出可以中止的依据。";
    }
    return facts;
  }

  // 支持的那幾種流程：index.lock 先擋（本程序絕不刪鎖文件），未合併清單必須讀得回來。
  if (facts.indexLock) {
    facts.continueBlockedReason =
        L"仓库里有 index.lock：另一个 Git 进程正在写这份索引（编辑器、终端里的另一条命令都算）。"
        L"这一步在这种状态下会被 Git 自己拒绝，本程序不会替你删那个锁。";
    facts.abortBlockedReason = facts.continueBlockedReason;
    return facts;
  }
  if (!facts.unmergedReadOk) {
    facts.continueBlockedReason =
        facts.unmergedReadFailure +
        L"。清单没读回来之前，本程序不能说「已经没有未合并文件」，因此不生成继续的命令。";
    facts.abortBlockedReason.clear();  // 中止不依賴未合併清單，仍然可用
  } else if (facts.HasUnmergedEntries()) {
    facts.continueBlockedReason =
        L"索引里还有 " + std::to_wstring(facts.unmergedPaths.size()) +
        L" 个未合并的文件，Git 在这种状态下自己也会拒绝 --continue。先把它们解决并暂存"
        L"（解决内容得由你自己写：本程序不会替你选 ours/theirs，也不会替你 add 任何文件），"
        L"再来点「继续该流程」。";
    facts.abortBlockedReason.clear();
  }

  facts.continueAvailable = facts.continueBlockedReason.empty();
  facts.abortAvailable = facts.abortBlockedReason.empty();
  return facts;
}

std::wstring DescribeConflictState(const ConflictStateFacts& facts) {
  std::wstring text = L"停着的是什么：";
  if (!facts.probed) {
    text += ConflictFlowKindLabel(ConflictFlowKind::unreadable) + L"\n";
    text += facts.probeFailure.empty() ? L"  · 没能探到 Git 目录里的流程痕迹。\n"
                                       : L"  · " + facts.probeFailure + L"\n";
    text += L"\n在这种「问不到」的状态下，本程序既不生成继续的命令，也不生成中止的命令。";
    return text;
  }

  text += ConflictFlowKindLabel(facts.kind) + L"\n";
  if (!facts.markersSeen.empty()) {
    text += L"  · 看到的痕迹：" + JoinMarkers(facts.markersSeen) + L"\n";
  }

  // 当前分支与 HEAD：问不到就说问不到，绝不把「没问出来」写成「没有分支」。
  if (facts.branchQueried) {
    text += L"  · 当前分支：" +
            (facts.onBranch ? facts.branchRef
                            : std::wstring(L"HEAD 没有指向任何分支引用（游离 HEAD 或尚无提交的那种状态）")) +
            L"\n";
  } else {
    text += L"  · 当前分支：没能问出来（这不是「没有分支」，是那一条查询没答上来）\n";
  }
  if (facts.headQueried) {
    text += L"  · HEAD 现在在：" +
            (facts.headObjectId.empty() ? std::wstring(L"问不到（HEAD 尚不可解析或形态不合格）")
                                        : ShortObjectId(facts.headObjectId)) +
            L"\n";
  } else {
    text += L"  · HEAD 现在在：没能问出来\n";
  }
  if (!facts.flowTarget.empty()) {
    text += L"  · " + facts.flowTarget + L"\n";
  }
  if (facts.sequencerPending) {
    text += L"  · 还有序列待办（sequencer\\）：这可能是一条序列里的某一步，"
            L"继续或中止管的都是整条序列，不止眼前这一步。\n";
  }
  if (facts.autostashEntry) {
    text += L"  · 这个流程开始时留过一份自动 stash（MERGE_AUTOSTASH/REBASE_AUTOSTASH）："
            L"中止时 Git 会按它自己的规则处理那一份，本程序不碰它。\n";
  }
  if (facts.indexLock) {
    text += L"  · 仓库里有 index.lock：另一个 Git 进程正在写这份索引，这一步在这种状态下发不出去"
            L"（本程序绝不删那个锁）。\n";
  }

  // 未合并文件
  if (!facts.unmergedReadOk) {
    text += L"未合并的文件：" + facts.unmergedReadFailure + L"\n";
  } else if (facts.unmergedPaths.empty()) {
    text += L"未合并的文件：0 个（索引里没有未合并条目）。\n";
  } else {
    text += L"未合并的文件共 " + std::to_wstring(facts.unmergedPaths.size()) + L" 个：" +
            FormatPaths(facts.unmergedPaths) + L"\n";
  }
  if (!facts.unreadableContents.empty()) {
    text += L"  · 这些痕迹档案存在、内容却没能在本轮原样读回：" + JoinMarkers(facts.unreadableContents) +
            L"（读不回来不等于没有，涉及它们的说明都会写明「问不到」）。\n";
  }

  // 继续前提
  text += L"\n继续的前提（就看上面这份实据）：\n";
  const std::wstring unmergedVerdict =
      !facts.unmergedReadOk
          ? std::wstring(L"没能读回来，因此不作答")
          : (facts.unmergedPaths.empty()
                 ? std::wstring(L"已满足（0 个）")
                 : L"不满足（还有 " + std::to_wstring(facts.unmergedPaths.size()) + L" 个）");
  text += Bullet(L"索引里没有未合并条目：", unmergedVerdict);
  text += Bullet(L"仓库里没有 index.lock：",
                 facts.indexLock ? std::wstring(L"不满足（另一个 Git 进程正在写索引）")
                                 : std::wstring(L"已满足"));
  text += Bullet(L"停着的这一种流程在本程序接手范围内：",
                 facts.continueAvailable || facts.abortAvailable
                     ? std::wstring(L"是")
                     : std::wstring(L"不是（见下面那句原因）"));
  text += L"  · 上面三条只是「这条命令能发出去」的条件。真能不能按你的意思收尾，是 Git 自己的事"
          L"（提交说明、钩子、签名、是否打开编辑器）。本程序不会替你解决任何文件内容，"
          L"也不会替你暂存或退回任何文件。\n";

  text += L"\n现在能做的：\n";
  if (facts.continueAvailable) {
    text += L"  · 「继续该流程」可以把那条 --continue 交进命令窗口（点下去之后还有确认框，"
            L"确认框会写清这一步由 Git 建立提交、钩子与签名照常生效）。\n";
  } else {
    text += L"  · 「继续该流程」现在不可用：" + facts.continueBlockedReason + L"\n";
  }
  if (facts.abortAvailable) {
    text += L"  · 「中止该流程」可以把那条 --abort 交进命令窗口（它会改动工作区，"
            L"风险在确认框里逐条写着，必须你亲手点确认）。\n";
  } else {
    text += L"  · 「中止该流程」现在不可用：" + facts.abortBlockedReason + L"\n";
  }

  text += L"\n看差异：\n";
  text += L"  · 「未暂存的更改」里带「冲突」的每一行，双击就是 Git 原生的组合差异"
          L"（diff --cc，命令窗口里看得见原文）；已暂存那一侧的同名条目显示的是 Git 的"
          L"「* Unmerged path」说明，不是某一侧的干净差异。\n";
  text += L"  · 本程序不打开任何内置合并器，也不改文件：解决内容由你在自己顺手的编辑器里写，"
          L"写完按「加入暂存区」，全部解决后再来「继续该流程」。\n";
  return text;
}

ConflictOperationPlan BuildConflictContinuePlan(const ConflictStateFacts& facts) {
  ConflictOperationPlan plan;
  if (!facts.continueAvailable) {
    plan.blockedReason = facts.continueBlockedReason.empty()
                             ? L"现在这个现场读不出可以继续的依据，因此不生成任何命令。"
                             : facts.continueBlockedReason;
    return plan;
  }

  std::wstring subcommand;
  switch (facts.kind) {
    case ConflictFlowKind::merge:
      subcommand = L"merge";
      break;
    case ConflictFlowKind::rebaseMergeBackend:
    case ConflictFlowKind::rebaseApplyBackend:
      subcommand = L"rebase";
      break;
    case ConflictFlowKind::cherryPick:
      subcommand = L"cherry-pick";
      break;
    case ConflictFlowKind::revert:
      subcommand = L"revert";
      break;
    default:
      plan.blockedReason = L"停着的这一种流程不在本程序的接手范围内。";
      return plan;
  }

  plan.arguments = {L"-c", L"submodule.recurse=false", subcommand, L"--continue"};
  plan.operationId = L"conflict-continue";
  plan.displayName = L"冲突流程 继续";
  plan.commandLabel = subcommand + L" --continue";
  plan.blocked = false;

  std::wstring text = L"接下来会在命令窗口里执行这一条（工作目录就是这个仓库的工作区根）：\n";
  text += L"  git -c submodule.recurse=false " + subcommand + L" --continue\n\n";
  text += L"这一步是「由 Git 建立提交」的那一步，不是一条只读命令：\n";
  text += L"  · Git 文档写明 merge --continue 会先确认这里真有一个中断中的合并，然后调用 git commit"
          L"（变基/拣选/撤销的 --continue 同样是把序列里剩下的那一步提交出去）。"
          L"提交的内容就是你当前暂存区里的那一份树——和「创建提交」同一套规则。\n";
  text += L"  · 没有 --no-verify：按 Git 的文档，冲突解决后提交合并时跑的是 pre-commit 与 commit-msg"
          L"（pre-merge-commit 在这种「先把冲突解决再单独提交」的场合不跑），"
          L"prepare-commit-msg 本来也不受 --no-verify 影响。任何一个钩子非 0 退出都会让这一步停下来，"
          L"Git 的报错原样留在命令窗口里，流程痕迹仍在。\n";
  text += L"  · 提交签名按你的 commit.gpgsign 配置生效（本程序没传 -S 也没传 --no-gpg-sign）。\n";
  text += L"  · 是否打开编辑器由 Git 自己决定：本程序不传 --no-edit、也不传 -m 去替它决定。"
          L"变基的合并后端按 Git 文档会在 --continue 时打开编辑器让你改提交说明"
          L"（apply 后端直接用原来的说明）。编辑器就在这个命令窗口里；取消它会让这一步以非 0 结束，"
          L"流程不会因此被清掉，你写的解决内容也不会因此丢。\n";
  if (subcommand == L"rebase") {
    text += L"  · 这一条只管「把当前停住的那一步交出去」：序列里剩下的步骤会由 Git 继续跑，"
            L"后面的每一步同样可能再停一次。\n";
  }
  text += L"\n现场（以刚读回来的为准）：\n";
  if (facts.onBranch) {
    text += L"  · 当前分支：" + facts.branchRef + L"\n";
  } else {
    text += L"  · 当前分支：HEAD 没指向分支引用（游离状态）\n";
  }
  if (!facts.headObjectId.empty()) {
    text += L"  · HEAD 现在在：" + ShortObjectId(facts.headObjectId) + L"\n";
  }
  text += L"  · " + facts.flowTarget + L"\n";
  text += L"  · 索引里的未合并条目：0 个（这正是「可以继续」的依据；"
          L"从复核到 Git 自己读索引之间那一段，归 Git 的 index.lock 管，本程序不持那把锁）\n";
  if (facts.sequencerPending) {
    text += L"  · 还有序列待办（sequencer\\）：这是整条序列的中间一步，不是最后一步\n";
  }
  text += L"\n范围与边界：\n";
  text += L"  · 只发这一条命令，只对这个仓库、这一个流程；-c submodule.recurse=false 只对这一个"
          L"子进程生效，不写你的配置文件，子模块不会被递归改动。\n";
  text += L"  · 本程序不会替你选冲突内容（没有 ours/theirs、没有 merge-file、没有 rerere 操作），"
          L"不会暂存或退回任何文件，也不会删锁。\n";
  text += L"  · 成败只看命令窗口里 Git 的退出码；非 0 时现场原样留着，本程序不做任何自动恢复"
          L"（不 reset --hard、不 clean、不 stash、不重试）。\n";
  plan.confirmationText = text;

  plan.notice = L"范围：只处理这个仓库里已经停着的这一个流程（" +
                ConflictFlowKindLabel(facts.kind) + L"）；命令是 " + plan.commandLabel +
                L"，不递归子模块、不访问远端、不替你解决任何内容。";
  return plan;
}

ConflictOperationPlan BuildConflictAbortPlan(const ConflictStateFacts& facts) {
  ConflictOperationPlan plan;
  if (!facts.abortAvailable) {
    plan.blockedReason = facts.abortBlockedReason.empty()
                             ? L"现在这个现场读不出可以中止的依据，因此不生成任何命令。"
                             : facts.abortBlockedReason;
    return plan;
  }

  std::wstring subcommand;
  switch (facts.kind) {
    case ConflictFlowKind::merge:
      subcommand = L"merge";
      break;
    case ConflictFlowKind::rebaseMergeBackend:
    case ConflictFlowKind::rebaseApplyBackend:
      subcommand = L"rebase";
      break;
    case ConflictFlowKind::cherryPick:
      subcommand = L"cherry-pick";
      break;
    case ConflictFlowKind::revert:
      subcommand = L"revert";
      break;
    default:
      plan.blockedReason = L"停着的这一种流程不在本程序的接手范围内。";
      return plan;
  }

  plan.arguments = {L"-c", L"submodule.recurse=false", subcommand, L"--abort"};
  plan.operationId = L"conflict-abort";
  plan.displayName = L"冲突流程 中止";
  plan.commandLabel = subcommand + L" --abort";
  plan.blocked = false;

  std::wstring text = L"这条命令会改动你的工作区，风险请自己核对之后再确认：\n";
  text += L"  git -c submodule.recurse=false " + subcommand + L" --abort\n\n";
  text += L"它会做什么（按 Git 自己的文档口径）：\n";
  if (subcommand == L"merge") {
    text += L"  · 终止当前的冲突解决过程，并「试图重建合并开始前的状态」；文档写明这一条在 "
            L"MERGE_HEAD 存在时等同于 git reset --merge（若还有 MERGE_AUTOSTASH，Git 改为把那份"
            L"自动 stash 应用回工作区）。\n";
    text += L"  · 你在冲突文件里已经写好的解决内容随之丢弃。文档还明确写着："
            L"合并开始时就存在、之后没有提交的工作区改动，某些情况下 Git 无法重建。\n";
  } else if (subcommand == L"rebase") {
    text += L"  · 中止变基并把 HEAD 复位到原来的分支（带着分支名开始时就是那条分支，"
            L"否则回到变基开始时 HEAD 所在的位置）。这一层读回的 head-name：" +
            (facts.rebaseHeadName.empty() ? std::wstring(L"问不到") : facts.rebaseHeadName) + L"\n";
    text += L"  · 已经重放出来的那些提交不再被那条引用指向（对象还在库里，能不能靠 reflog/ORIG_HEAD "
            L"找回由 Git 决定，本程序不把它当作依据）。文档没有逐格写明索引与工作区怎么处理，"
            L"因此这里按「会重写工作区」看待：你写好的解决内容与未提交的改动都可能丢弃。\n";
  } else {
    text += L"  · 取消整个" + (subcommand == L"revert" ? std::wstring(L"撤销") : std::wstring(L"拣选")) +
            L"序列，并回到序列开始前保存的那份状态：这条序列里已经做成的提交会随引用退回而不再被指向"
            L"（对象仍在库内），工作区与索引里未提交的改动同样可能被丢弃。\n";
  }
  text += L"\n本程序不会做的事：\n";
  text += L"  · 不会替你 stash、不会 reset --hard、不会 clean、不会删 index.lock，也不会后台"
          L"「自动恢复」任何东西；除了上面那一条命令，什么都不会发出去。\n";
  text += L"  · 如果你只想撤掉流程痕迹而保住工作区里已经写好的东西，Git 那边另有 --quit"
          L"（文档写明它留下索引与工作区不动）——但那不是你点这个按钮要做的事，本程序不代为执行。\n";
  text += L"\n现场（以刚读回来的为准）：\n";
  text += L"  · " + ConflictFlowKindLabel(facts.kind) + L"\n";
  if (facts.onBranch) {
    text += L"  · 当前分支：" + facts.branchRef + L"\n";
  } else {
    text += L"  · 当前分支：HEAD 没指向分支引用（游离状态）\n";
  }
  if (!facts.headObjectId.empty()) {
    text += L"  · HEAD 现在在：" + ShortObjectId(facts.headObjectId) + L"\n";
  }
  if (!facts.flowTarget.empty()) {
    text += L"  · " + facts.flowTarget + L"\n";
  }
  if (facts.unmergedReadOk) {
    text += L"  · 索引里的未合并条目：" + std::to_wstring(facts.unmergedPaths.size()) + L" 个";
    if (!facts.unmergedPaths.empty()) {
      text += L"（中止不需要先把它们解决，这一步丢掉的正是这些文件里已经写好的内容）";
    }
    text += L"\n";
  } else {
    text += L"  · 索引里的未合并条目：没能读回来（" + facts.unmergedReadFailure +
            L"）——这一条不影响中止能不能发出去，只影响说明的完整度。\n";
  }
  if (facts.autostashEntry) {
    text += L"  · 这个流程开始时留过一份自动 stash：Git 中止时按它自己的规则处理那一份。\n";
  }
  text += L"\n范围：只发这一条命令，只对这个仓库、这一个流程；-c submodule.recurse=false 只对这一个"
          L"子进程生效，不写你的配置文件，子模块不会被递归改动，也不访问远端。\n";
  plan.confirmationText = text;

  plan.notice = L"范围：中止这一个流程（" + ConflictFlowKindLabel(facts.kind) +
                L"）；命令是 " + plan.commandLabel +
                L"，会改动工作区与索引，本程序不做任何自动恢复。";
  return plan;
}

std::wstring DescribeConflictStateChange(const ConflictStateFacts& confirmed,
                                         const ConflictStateFacts& latest) {
  std::vector<std::wstring> changes;
  if (!latest.probed) {
    return L"点头之后、发出命令之前，程序要把确认框上写的那件事再核一遍，"
           L"但这一轮的 Git 目录探测没能完成（" +
           (latest.probeFailure.empty() ? std::wstring(L"原因没能带回来") : latest.probeFailure) +
           L"）。宁可不发这条命令，也不对着没核实过的现场执行，仓库没有被改动过。"
           L"仓库状态正在重读，看清现状后如仍要继续或中止请重新点一次。";
  }
  if (latest.kind != confirmed.kind) {
    if (latest.kind == ConflictFlowKind::none) {
      changes.push_back(L"确认框上写的那个流程现在在这个仓库里已经没有痕迹了——它很可能已经在外部终端"
                        L"或别的程序里走完或被中止了");
    } else if (latest.kind == ConflictFlowKind::unreadable) {
      changes.push_back(L"这一轮没能探出流程痕迹：现在停着的是什么，问不到");
    } else {
      changes.push_back(L"停着的流程变成了「" + ConflictFlowKindLabel(latest.kind) + L"」，"
                        L"确认框上写的不是这一种");
    }
  }
  if (latest.indexLock && !confirmed.indexLock) {
    changes.push_back(L"仓库里现在有了 index.lock，另一个 Git 进程正在写这份索引");
  }
  if (!latest.unmergedReadOk) {
    changes.push_back(L"这一轮的未合并条目没能读回来：" + latest.unmergedReadFailure);
  } else if (latest.unmergedPaths != confirmed.unmergedPaths) {
    std::wstring detail =
        L"未合并条目和确认框上写的不是同一份：现在共 " + std::to_wstring(latest.unmergedPaths.size()) +
        L" 个（确认时 " + std::to_wstring(confirmed.unmergedPaths.size()) + L" 个）";
    if (!latest.unmergedPaths.empty() && latest.unmergedPaths != confirmed.unmergedPaths) {
      detail += FormatPaths(latest.unmergedPaths);
    }
    changes.push_back(detail);
  }
  if (latest.branchQueried && confirmed.branchQueried && latest.branchRef != confirmed.branchRef) {
    changes.push_back(L"当前分支从「" +
                      (confirmed.onBranch ? confirmed.branchRef : std::wstring(L"没指向分支引用")) +
                      L"」变成「" + (latest.onBranch ? latest.branchRef : std::wstring(L"没指向分支引用")) +
                      L"」");
  }
  if (!confirmed.headObjectId.empty() && latest.headObjectId != confirmed.headObjectId) {
    changes.push_back(L"HEAD 从 " + ShortObjectId(confirmed.headObjectId) + L" 变成了 " +
                      (latest.headObjectId.empty() ? std::wstring(L"问不到的位置")
                                                   : ShortObjectId(latest.headObjectId)));
  }
  if (!confirmed.flowTarget.empty() && latest.flowTarget != confirmed.flowTarget) {
    changes.push_back(L"流程自己的目标也换了：确认时是「" + confirmed.flowTarget + L"」，现在是「" +
                      latest.flowTarget + L"」");
  }
  if (!confirmed.autostashEntry && latest.autostashEntry) {
    changes.push_back(L"现在多了一份流程开始时留下的自动 stash 痕迹");
  }
  if (changes.empty()) {
    return {};
  }

  std::wstring text = L"按下“确定”之后、发出命令之前，程序把预检那组只读查询原样重发了一遍："
                      L"读回来的结果和确认框上写的那一份已经不是同一件事了。\n";
  for (size_t index = 0; index < changes.size(); ++index) {
    text += (index == 0 ? L"  · " : L"\n  · ") + changes[index];
  }
  text += L"\n\n因此这条命令没有发出，仓库没有被本程序改动过（刚才那一份状态没被任何人替你动）。\n";
  text += L"本程序不会替你选「继续还是中止」：仓库状态正在重读，看清现状后如仍要做同一件事，"
          L"请重新点一次，对新的现状重新确认一遍。";
  return text;
}

}  // namespace gc::git
