#include "git/commit_plan.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId：身份字段进比较之前先验形态

namespace gc::git {
namespace {

// 確認文字裡最多列出多少項暫存條目、多少行提交訊息。MessageBox 不能滾動，
// 超出部分一律折成「另有幾項／幾行」，讓使用者知道範圍但不会被淹沒。
constexpr size_t kMaxPreviewItems = 15;
constexpr size_t kMaxPreviewMessageLines = 20;
constexpr size_t kMaxConflictListItems = 5;

std::wstring AsciiToWide(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (const char c : text) {
    result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return result;
}

// 能否安全交給命令窗口：路徑會進 cmd 腳本裡被引號包住，雙引號與控制字元會破坏那一行，
// 因此與 git/command_window 對參數的要求保持同一條底線。
bool IsCommandLineSafePath(std::wstring_view path, std::wstring* reason) {
  if (path.empty()) {
    if (reason != nullptr) {
      *reason = L"没有拿到提交信息文件的路径。";
    }
    return false;
  }
  for (const wchar_t c : path) {
    if (c == L'"') {
      if (reason != nullptr) {
        *reason = L"提交信息文件的路径里有双引号，命令窗口无法安全表达，因此没有执行任何命令。"
                  L"请把系统临时目录换到不含引号的位置。";
      }
      return false;
    }
    if (c < 0x20 || c == 0x7F) {
      if (reason != nullptr) {
        *reason = L"提交信息文件的路径里有控制字符，命令窗口无法安全表达，因此没有执行任何命令。";
      }
      return false;
    }
  }
  return true;
}

// 環境值只能是「不含控制字元的文字」：環境塊以 NUL 分隔，CR/LF 以外的控制字元會破坏結構，
// 而身份裡的換行更能在提交信息裡憑空多出一行。
bool IsEnvironmentValueSafe(std::wstring_view value) {
  for (const wchar_t c : value) {
    if (c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  return true;
}

std::vector<std::wstring> SplitWideLines(std::wstring_view text) {
  std::vector<std::wstring> lines;
  std::wstring current;
  for (const wchar_t c : text) {
    if (c == L'\n') {
      if (!current.empty() && current.back() == L'\r') {
        current.pop_back();
      }
      lines.push_back(current);
      current.clear();
      continue;
    }
    if (c == L'\r') {
      continue;
    }
    current.push_back(c);
  }
  lines.push_back(current);
  return lines;
}

std::wstring TrimOne(std::wstring_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
    --end;
  }
  return std::wstring(text.substr(begin, end - begin));
}

// 一行裡的狀態與路徑：路徑可能很長，確認框寬度有限，超長時中間打點。
std::wstring Ellipsis(std::wstring_view text, size_t limit) {
  if (text.size() <= limit) {
    return std::wstring(text);
  }
  const size_t head = limit / 2;
  const size_t tail = limit - head - 3;
  return std::wstring(text.substr(0, head)) + L"…" + std::wstring(text.substr(text.size() - tail));
}

std::wstring StagedItemLine(const ChangeItem& item) {
  // 狀態用 Git 原始 XY（「M」「A」「R100」），與列表同源；這裡不另立一套說法。
  const std::wstring status = item.statusCode.empty() ? std::wstring(L"?") : TrimOne(item.statusCode);
  return L"  " + status + L" " + Ellipsis(item.PathLabel(), 96);
}

bool HasUnmergedStaged(const WorkspaceModel& model) {
  const auto unmerged = [](const ChangeItem& item) { return item.kind == ChangeKind::conflicted; };
  return std::any_of(model.staged.begin(), model.staged.end(), unmerged);
}

std::wstring ConflictListText(const WorkspaceModel& model) {
  std::vector<std::wstring> paths;
  const auto collect = [&paths](const std::vector<ChangeItem>& items) {
    for (const ChangeItem& item : items) {
      if (item.kind == ChangeKind::conflicted && paths.size() < kMaxConflictListItems) {
        paths.push_back(item.PathLabel());
      }
    }
  };
  collect(model.staged);
  collect(model.unstaged);
  size_t total = 0;
  for (const ChangeItem& item : model.staged) {
    total += item.kind == ChangeKind::conflicted ? 1 : 0;
  }
  for (const ChangeItem& item : model.unstaged) {
    total += item.kind == ChangeKind::conflicted ? 1 : 0;
  }
  std::wstring text;
  for (size_t index = 0; index < paths.size(); ++index) {
    if (index > 0) {
      text += L"、";
    }
    text += paths[index];
  }
  if (total > paths.size()) {
    text += L" 等 " + std::to_wstring(total) + L" 处";
  }
  return text;
}

std::wstring HeadDisplay(const RepoDetection& detection) {
  std::wstring text = FormatBranchDisplay(detection);
  if (detection.headResolved && !detection.shortSha.empty()) {
    text += L"（最近提交 " + detection.shortSha + L"）";
  } else {
    text += L"（尚无提交，这次是仓库的第一个提交）";
  }
  return text;
}

std::wstring TimeLineText(std::wstring_view label, const CommitTimeChoice& time) {
  std::wstring text = std::wstring(label) + L"：" + (time.displayText.empty() ? L"（无法换算）" : time.displayText);
  if (!time.gitDate.empty()) {
    text += L"　交给 Git：" + AsciiToWide(time.gitDate);
  }
  return text;
}

// ---- 身份查詢的判讀輔助 ----
//
// 本模組不能复用 git/undo_commit_plan 的那套 ReadUndoQuery：那個標頭反過來依賴本模組的
// RepositoryWorkflowState，在這裡包含它會造成標頭循環。判定順序與它一致——
// 先看啟動失敗／逾時／輸出完整度這類結構性事實，再看退出碼，最後才解析輸出。
struct SingleLineAnswer {
  bool ok = false;
  bool negative = false;  // --quiet 系查詢以退出碼 1、無輸出明確回答「沒有」
  std::wstring value;     // ok 且非 negative 時的第一行非空輸出（已修剪行尾）
  std::wstring failure;   // 不成立時面向界面的原因（已含 Git 的歸類與細節）
};

std::wstring TrimmedFirstLine(const GitQueryResult& result) {
  for (const std::wstring_view line : SplitLines(result.utf16Output)) {
    const std::wstring trimmed = TrimWide(line);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return {};
}

// quietStyle：這條查詢屬於 `--quiet` 系（symbolic-ref / rev-parse --verify），
// 退出碼 1 且無輸出是 Git 的明確回答「沒有」，不是失敗。
SingleLineAnswer ReadSingleLine(const GitQueryResult& result, bool quietStyle, std::wstring_view question) {
  SingleLineAnswer answer;
  std::wstring detail;
  const RepoError error = ClassifyGitFailure(result, detail);
  if (error == RepoError::none) {
    answer.value = TrimmedFirstLine(result);
    if (answer.value.empty()) {
      answer.failure = std::wstring(question) + L"：Git 回答成功了，却没有给出任何内容";
      return answer;
    }
    answer.ok = true;
    return answer;
  }
  if (quietStyle && error == RepoError::gitFailed && result.exitCode == 1 &&
      TrimWide(result.utf16Output).empty()) {
    answer.ok = true;
    answer.negative = true;
    return answer;
  }
  answer.failure = std::wstring(question) + L"：" + BuildRepoErrorDetail(error, detail);
  return answer;
}

std::vector<std::wstring> AnsweredLines(const GitQueryResult& result) {
  std::vector<std::wstring> lines;
  for (const std::wstring_view line : SplitLines(result.utf16Output)) {
    const std::wstring trimmed = TrimWide(line);
    if (!trimmed.empty()) {
      lines.push_back(trimmed);
    }
  }
  return lines;
}

std::wstring BranchNameFromRef(std::wstring_view branchRef) {
  constexpr std::wstring_view prefix = L"refs/heads/";
  return branchRef.size() > prefix.size() && branchRef.compare(0, prefix.size(), prefix) == 0
             ? std::wstring(branchRef.substr(prefix.size()))
             : std::wstring();
}

// 物件 ID 的展示：完整 ID 是複核比對的依據，短 ID 只是給人一眼认出的。
std::wstring ObjectIdDisplay(const std::wstring& fullObjectId) {
  if (fullObjectId.empty()) {
    return L"（读不到）";
  }
  return ShortObjectId(fullObjectId) + L"（完整 ID " + fullObjectId + L"）";
}

std::wstring PathDisplay(const std::wstring& path) {
  return path.empty() ? std::wstring(L"（读不到）") : path;
}

// 分支一侧的展示：两种身份载体的字段同名，这里只取「在分支上就给完整引用，不在就说游离」。
std::wstring DetachedOrRef(const CommitPlanIdentity& identity) {
  return identity.onBranch ? identity.branchRef : std::wstring(L"不在分支上（游离 HEAD）");
}

std::wstring DetachedOrRef(const CommitIdentityFacts& facts) {
  return facts.onBranch ? facts.branchRef : std::wstring(L"不在分支上（游离 HEAD）");
}

std::wstring CommitterDisplay(const std::wstring& identityText, CommitterIdentityState state) {
  if (!identityText.empty()) {
    return identityText;
  }
  return state == CommitterIdentityState::missing ? std::wstring(L"Git 配置里凑不出完整身份")
                                                  : std::wstring(L"（读不到）");
}

// 「身份對不上」的統一措辭：確認框還沒彈出來就要拒絕時用。
std::wstring StaleFactsMessage(std::wstring_view detail) {
  std::wstring text = L"预检读回的仓库现状与界面上显示的已经对不上：";
  text += detail;
  text += L"。这两种时刻的事实拼不成一句可信的承诺，因此这里没有给出确认框，也没有执行任何命令。"
          L"请点“刷新”重读，看清现状后再重新点一次“创建提交”。";
  return text;
}

}  // namespace

std::wstring RepositoryWorkflowState::SpecialFlowText() const {
  std::vector<std::wstring> names;
  if (mergeInProgress) {
    names.push_back(L"合并（MERGE_HEAD）");
  }
  if (revertInProgress) {
    names.push_back(L"撤销（REVERT_HEAD）");
  }
  if (cherryPickInProgress) {
    names.push_back(L"拣选（CHERRY_PICK_HEAD）");
  }
  if (rebaseInProgress) {
    names.push_back(L"变基（rebase-merge/ 或 rebase-apply/）");
  }
  if (bisectInProgress) {
    names.push_back(L"二分定位（BISECT_LOG）");
  }
  std::wstring text;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0) {
      text += L"；";
    }
    text += names[index];
  }
  return text;
}

// ---- 身份查詢的参数 ----

std::vector<std::wstring> BuildCommitWorktreeArguments(std::wstring_view repositoryDirectory) {
  // 两行的顺序固定：--absolute-git-dir 在前、--show-toplevel 在后（与形态路径组同源）。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--absolute-git-dir",
                                   L"--show-toplevel"};
}

std::vector<std::wstring> BuildCommitBranchRefArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"symbolic-ref", L"--quiet", L"HEAD"};
}

std::vector<std::wstring> BuildCommitHeadObjectArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   L"HEAD"};
}

std::vector<std::wstring> BuildCommitIndexTreeArguments(std::wstring_view repositoryDirectory) {
  // write-tree 要把索引里的状态信息刷新后写回，取的是 Git 自己必需的锁，不是「可选锁」：
  // 这里加 --no-optional-locks 只会让它跳过那层可有可无的锁档案，不会让它失败。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"write-tree"};
}

CommitIdentityFacts InterpretCommitIdentity(const CommitIdentityQueries& queries) {
  CommitIdentityFacts facts;
  facts.workflow = queries.workflow;
  facts.workflowProbed = queries.workflowProbed;

  std::vector<std::wstring> failures;

  // 1) 工作区根 + 绝对 Git 目录：两行都要有，缺第二行等同于问错了地方。
  const SingleLineAnswer gitDir =
      ReadSingleLine(queries.worktree, false, L"绝对 Git 目录（rev-parse --absolute-git-dir）");
  std::wstring topLevel;
  if (gitDir.ok) {
    const std::vector<std::wstring> lines = AnsweredLines(queries.worktree);
    facts.absoluteGitDir = lines.empty() ? std::wstring() : lines.front();
    if (lines.size() >= 2) {
      topLevel = lines[1];
    } else {
      failures.push_back(L"工作区根（rev-parse --show-toplevel）：Git 只回了一行，取不到工作区根目录");
    }
  } else {
    failures.push_back(gitDir.failure);
  }
  if (!topLevel.empty()) {
    facts.repositoryDirectory = topLevel;
  }
  // 问的是哪块工作区，答的就该是哪块：这一步不成立时，后面每一条事实都不再可信。
  if (!queries.requestedRepositoryDirectory.empty() && !topLevel.empty() &&
      !PathsEqualFolded(topLevel, queries.requestedRepositoryDirectory)) {
    failures.push_back(L"预检问的是「" + queries.requestedRepositoryDirectory +
                       L"」，Git 却把工作区根回答成「" + topLevel + L"」");
  }

  // 2) 分支引用与 HEAD 完整 ID：这两条允许「明确没有」（游离 HEAD / 尚无提交），不允许「不知道」。
  const SingleLineAnswer branch =
      ReadSingleLine(queries.branchRef, true, L"当前分支引用（symbolic-ref --quiet HEAD）");
  if (!branch.ok) {
    failures.push_back(branch.failure);
  } else if (!branch.negative) {
    facts.onBranch = true;
    facts.branchRef = branch.value;
    facts.branchName = BranchNameFromRef(branch.value);
  }

  const SingleLineAnswer head =
      ReadSingleLine(queries.headObject, true, L"HEAD 的完整对象 ID（rev-parse --verify --quiet HEAD）");
  if (!head.ok) {
    failures.push_back(head.failure);
  } else if (!head.negative) {
    if (!LooksLikeFullObjectId(head.value)) {
      failures.push_back(L"HEAD 的完整对象 ID：Git 回的内容不是一个完整的对象 ID");
    } else {
      facts.headResolved = true;
      facts.headObjectId = head.value;
    }
  }

  // 3) 索引内容标识：这条没有「明确没有」这一档——要么给出树对象 ID，要么就是问不出内容。
  //    索引里有未合并条目时 Git 以退出码 128 失败并逐条报 unmerged，那种索引本来就不是可提交的内容。
  const SingleLineAnswer tree = ReadSingleLine(queries.indexTree, false, L"索引内容（git write-tree）");
  if (!tree.ok) {
    failures.push_back(tree.failure);
  } else if (!LooksLikeFullObjectId(tree.value)) {
    failures.push_back(L"索引内容（git write-tree）：Git 回的内容不是一个完整的树对象 ID");
  } else {
    facts.indexTreeResolved = true;
    facts.indexTreeOid = tree.value;
  }

  // 4) 提交者身份：两条 config 各问一句。「没设过」是明确答案，只有查询本身失败才算读不到——
  //    读不到的场合不能假定 Git 提交时会拿到什么身份，那种提交根本不该被确认。
  const ConfigValueRead name = ParseIdentityConfigQuery(queries.configName, kCommitCommitterConfigKeys[0]);
  const ConfigValueRead email = ParseIdentityConfigQuery(queries.configEmail, kCommitCommitterConfigKeys[1]);
  if (name.state == ConfigValueState::failed || email.state == ConfigValueState::failed) {
    failures.push_back(L"有效配置里的提交者身份（git config --get user.name / user.email）没能读回来");
  } else {
    const AuthorIdentityConfig config = CombineIdentityConfig(name, email);
    facts.committerConfigRead = true;
    facts.committer = config.CommitterState();
    facts.committerIdentityText = config.Identity();
  }

  if (!failures.empty()) {
    std::wstring text = L"执行前的只读预检没能问齐这份提交该绑定的事实，因此没有给出确认框，"
                        L"也没有执行任何命令：\n";
    for (size_t index = 0; index < failures.size(); ++index) {
      text += (index == 0 ? L"  · " : L"\n  · ") + failures[index];
    }
    facts.queryFailure = text;
    return facts;
  }
  facts.queryOk = true;
  return facts;
}

std::wstring DescribeCommitIdentityChange(const CommitPlanIdentity& confirmed,
                                          const CommitIdentityFacts& current) {
  if (!current.queryOk) {
    // 「读不回来」与「读回来不一样」都得作废旧方案：前者连现在是什么都不知道，
    // 拿旧方案去提交就等于对着一个没核对过的仓库下命令。
    return L"执行前复核没能读回这个仓库的现状，因此没有发出那条提交命令。\n\n" + current.queryFailure +
           L"\n\n表单里的内容一个字都没动。仓库状态正在重读，看清现状后如仍要提交请再点一次“创建提交”。";
  }

  std::vector<std::wstring> changes;
  if (!PathsEqualFolded(confirmed.repositoryDirectory, current.repositoryDirectory)) {
    changes.push_back(L"工作区根从「" + PathDisplay(confirmed.repositoryDirectory) + L"」变成「" +
                      PathDisplay(current.repositoryDirectory) + L"」");
  }
  if (!PathsEqualFolded(confirmed.absoluteGitDir, current.absoluteGitDir)) {
    changes.push_back(L"这个工作区连接的 Git 目录从「" + PathDisplay(confirmed.absoluteGitDir) +
                      L"」变成「" + PathDisplay(current.absoluteGitDir) +
                      L"」（换仓库、换工作树或 .git 被动过）");
  }
  if (confirmed.onBranch != current.onBranch || confirmed.branchRef != current.branchRef) {
    changes.push_back(L"当前分支从「" + DetachedOrRef(confirmed) + L"」变成「" + DetachedOrRef(current) +
                      L"」——就算 HEAD 还是同一份提交，这次提交要落在哪条线已经不一样了");
  }
  if (confirmed.headResolved != current.headResolved ||
      (confirmed.headResolved && current.headResolved &&
       confirmed.headObjectId != current.headObjectId)) {
    changes.push_back(L"HEAD 从「" + ObjectIdDisplay(confirmed.headObjectId) + L"」变成「" +
                      ObjectIdDisplay(current.headObjectId) + L"」");
  }
  if (!current.indexTreeResolved || confirmed.indexTreeOid != current.indexTreeOid) {
    changes.push_back(
        !current.indexTreeResolved
            ? std::wstring(L"索引现在算不出树对象了（最常见的原因是里面出现了未解决的冲突条目）")
            : std::wstring(L"索引内容从树「" + ObjectIdDisplay(confirmed.indexTreeOid) + L"」变成「" +
                           ObjectIdDisplay(current.indexTreeOid) +
                           L"」——条目数可以一点没变，但暂存的文件内容或模式已经换了"));
  }
  // 流程痕跡與 index.lock 比的是「這一回與確認時相比有沒有出現」：確認框上已經交代過的
  // index.lock 不算是變化（那種情況下 Git 自己會拒絕，界面老實轉述它的輸出）。
  if (current.workflow.HasSpecialFlowInProgress() && !confirmed.workflow.HasSpecialFlowInProgress()) {
    changes.push_back(L"这个仓库里出现了还没走完的 Git 流程：" + current.workflow.SpecialFlowText());
  }
  if (current.workflow.indexLocked && !confirmed.workflow.indexLocked) {
    changes.push_back(L"仓库里现在有了 index.lock，另一个 Git 进程正在写这份索引");
  }
  if (current.committer != CommitterIdentityState::available ||
      current.committerIdentityText != confirmed.committerIdentityText) {
    changes.push_back(L"有效配置里的提交者身份从「" + CommitterDisplay(confirmed.committerIdentityText,
                                                                       CommitterIdentityState::available) +
                      L"」变成「" +
                      CommitterDisplay(current.committerIdentityText, current.committer) + L"」");
  }
  if (changes.empty()) {
    return {};
  }

  std::wstring text = L"按下“确定”之后、发出命令之前，程序把预检那组只读查询原样重发了一遍："
                      L"读回来的结果和确认框上写的那一份已经不是同一件事了。\n";
  for (size_t index = 0; index < changes.size(); ++index) {
    text += (index == 0 ? L"  · " : L"\n  · ") + changes[index];
  }
  text += L"\n\n因此这条提交命令没有发出，刚写的提交信息文件已删除，仓库没有被改动过。";
  text += L"表单里的标题、描述、作者与合作者一个字都没动；仓库状态正在重读，"
          L"看清现状后如仍要提交请再点一次“创建提交”，对新的现状重新确认一遍。";
  return text;
}

CommitPlan BuildCommitPlan(const CommitPlanInput& input) {
  CommitPlan plan;
  plan.operationId = L"commit";
  plan.displayName = L"创建提交";
  plan.commandLabel = L"git commit -F";

  const auto block = [&](std::wstring reason) {
    CommitPlan blocked;
    blocked.blocked = true;
    blocked.blockedReason = std::move(reason);
    blocked.operationId = plan.operationId;
    blocked.displayName = plan.displayName;
    blocked.commandLabel = plan.commandLabel;
    return blocked;
  };

  // ---- 前提核對：任何一項不成立都不產生命令 ----
  if (input.detection.root.empty()) {
    return block(L"没有可用的仓库工作区根目录，无法确定这次提交属于哪个仓库。请先点“刷新”。");
  }
  if (!KindHasWorkspace(input.detection.kind)) {
    return block(L"这个仓库的形态（" + input.detection.KindLabel() +
                 L"）没有工作区，不能在这里创建提交。");
  }

  // ---- 身份预检：确认框要摆出来的那一份事实必须先问齐 ----
  if (!input.identity.queryOk) {
    return block(input.identity.queryFailure);
  }
  // 预检与界面列表是两次只读查询，中间照样可能被人改了仓库。两处答案对不上时，
  // 确认框上写的就会是「一半旧的一半新的」拼出来的承诺——那种话不能给。
  {
    std::vector<std::wstring> mismatch;
    if (!PathsEqualFolded(input.identity.repositoryDirectory, input.detection.root)) {
      mismatch.push_back(L"工作区根：" + PathDisplay(input.detection.root) + L" / " +
                         PathDisplay(input.identity.repositoryDirectory));
    }
    if (!PathsEqualFolded(input.identity.absoluteGitDir, input.detection.absoluteGitDir)) {
      mismatch.push_back(L"Git 目录：" + PathDisplay(input.detection.absoluteGitDir) + L" / " +
                         PathDisplay(input.identity.absoluteGitDir));
    }
    if (input.identity.headResolved != input.detection.headResolved) {
      mismatch.push_back(L"HEAD 能不能解析：界面读到「" +
                         std::wstring(input.detection.headResolved ? L"可以" : L"还不行（尚无提交）") +
                         L"」，预检读到「" +
                         std::wstring(input.identity.headResolved ? L"可以" : L"还不行（尚无提交）") + L"」");
    }
    if (input.identity.onBranch && !input.detection.branch.empty() &&
        input.identity.branchName != input.detection.branch) {
      mismatch.push_back(L"分支名：" + input.detection.branch + L" / " + input.identity.branchName);
    }
    if (input.identity.onBranch != !input.detection.branch.empty()) {
      mismatch.push_back(L"在不在分支上：界面读到「" +
                         std::wstring(input.detection.branch.empty() ? L"不在分支上" : L"在分支上") +
                         L"」，预检读到「" + std::wstring(input.identity.onBranch ? L"在分支上" : L"不在分支上") +
                         L"」");
    }
    if (!mismatch.empty()) {
      std::wstring detail;
      for (size_t index = 0; index < mismatch.size(); ++index) {
        detail += (index == 0 ? L"" : L"；") + mismatch[index];
      }
      return block(StaleFactsMessage(detail));
    }
  }

  std::wstring pathReason;
  if (!IsCommandLineSafePath(input.messageFilePath, &pathReason)) {
    return block(pathReason);
  }
  if (input.message.empty()) {
    return block(L"提交信息是空的：标题不能全为空白。请写下标题再提交。");
  }
  if (input.author.name.empty() || input.author.email.empty()) {
    return block(L"作者身份没有被拆解成「姓名 <邮箱>」，因此没有执行任何命令。"
                 L"请在“作者”那一栏写成这个形状，或把它清空改用仓库配置里的身份。");
  }
  if (!IsEnvironmentValueSafe(input.author.name) || !IsEnvironmentValueSafe(input.author.email)) {
    return block(L"作者身份里有控制字符，无法安全交给 Git，因此没有执行任何命令。");
  }
  if (input.authorTime.gitDate.empty() || input.committerTime.gitDate.empty()) {
    return block(L"作者时间或提交者时间没能换算成 Git 的记录形态（这个时刻落在系统时区规则能表达的"
                 L"范围之外）。请按“恢复当前时间”回到此刻，或另选一个时间。");
  }
  if (input.identity.committer != CommitterIdentityState::available) {
    const std::wstring cause = input.identity.committer == CommitterIdentityState::missing
                                   ? L"这个仓库的有效 Git 配置里凑不出提交者身份（user.name 与 user.email "
                                     L"至少有一项没设）。"
                                   : L"还没读到这个仓库的有效 Git 配置，无法确定提交者身份。";
    return block(L"提交者的身份由 Git 配置决定，本程序不替你写配置，也不在表单里输入它。" + cause +
                 L"请先在 Git 里设好 user.name 与 user.email，再点“刷新”重读。");
  }

  // 進行中的特殊流程：Git 在那種狀態下提交的含义與「普通提交」完全不同（合併提交有第二個父、
  // 變基中途提交會推進 todo 清單）。本步驟只支持「索引相對 HEAD 的一次普通提交」，
  // 因此明确拒绝，也不去碰那種流程的現場（不 abort、不 --quit）。
  // 痕跡来自预检刚问到的那个 Git 目录，不是界面早前存下的那一份。
  if (input.identity.workflow.HasSpecialFlowInProgress()) {
    return block(L"这个仓库里有 Git 流程还没走完：" + input.identity.workflow.SpecialFlowText() +
                 L"。那种状态下的提交与平常的提交不是同一件事（合并提交会带上第二个父、变基中途的提交"
                 L"会推进待办清单），本程序当前不在这类流程中创建提交，也不会替你终止它。"
                 L"请在原来的 Git 命令里把流程走完（git merge --continue / --abort、git rebase --continue"
                 L" / --abort 等），再点“刷新”。");
  }
  if (input.model.HasConflicts() || HasUnmergedStaged(input.model)) {
    return block(L"还有没解决的冲突条目：" + ConflictListText(input.model) +
                 L"。未解决的冲突不是可提交的内容：本程序不会把带冲突标记的索引直接提交，"
                 L"也不会替你解决冲突。请先处理这些条目（解决后“加入暂存区”，或用“← 移出暂存区”退回），"
                 L"再点“刷新”。");
  }
  if (input.model.staged.empty()) {
    return block(L"“已暂存的更改”里没有任何条目，因此没有可提交的内容。本程序不会代为暂存，"
                 L"也不会用 git commit -a 把工作区的改动一起收进来。请先在“未暂存的更改”里选中条目"
                 L"并点“加入暂存区 →”，再点“创建提交”。");
  }

  // ---- 命令與環境 ----
  plan.arguments = {L"commit", L"--cleanup=verbatim", L"-F", input.messageFilePath};
  plan.environmentOverrides = {
      {L"GIT_AUTHOR_NAME", input.author.name},
      {L"GIT_AUTHOR_EMAIL", input.author.email},
      {L"GIT_AUTHOR_DATE", AsciiToWide(input.authorTime.gitDate)},
      {L"GIT_COMMITTER_DATE", AsciiToWide(input.committerTime.gitDate)},
  };

  // 這份身份就是「確認框上寫的那一份」：界面要在點頭之後拿它逐條複核，
  // 啟動命令時的工作目錄與 git.exe 也只從它取，不再回讀可能已經被換掉的界面狀態。
  plan.identity.gitExecutable = input.gitExecutable;
  plan.identity.repositoryDirectory = input.identity.repositoryDirectory;
  plan.identity.absoluteGitDir = input.identity.absoluteGitDir;
  plan.identity.onBranch = input.identity.onBranch;
  plan.identity.branchRef = input.identity.branchRef;
  plan.identity.branchName = input.identity.branchName;
  plan.identity.headResolved = input.identity.headResolved;
  plan.identity.headObjectId = input.identity.headObjectId;
  plan.identity.indexTreeOid = input.identity.indexTreeOid;
  plan.identity.workflow = input.identity.workflow;
  plan.identity.author = input.author;
  plan.identity.committerIdentityText = input.identity.committerIdentityText;
  plan.identity.messageFilePath = input.messageFilePath;
  plan.identity.messageUtf8Bytes = input.messageUtf8Bytes;
  plan.identity.authorGitDate = input.authorTime.gitDate;
  plan.identity.committerGitDate = input.committerTime.gitDate;
  plan.identity.stagedItems = input.model.staged.size();

  // ---- 確認文字：把「要提交什麼」一次講完 ----
  std::wstring preview;
  // 把真正交给 Git 的参数原样列出来（命令窗口里执行并回显的也是这一条），让用户核对的是同一件事。
  std::wstring commandLine = L"  git";
  for (const std::wstring& argument : plan.arguments) {
    commandLine += L' ' + argument;
  }
  preview += L"将在命令窗口里执行（工作目录：" + input.identity.repositoryDirectory + L"）：\n";
  preview += commandLine + L"\n\n";

  preview += L"这次提交绑定的现状（点“确定”后、发出命令前会照这一份逐条复核）：\n";
  preview += L"  仓库工作区根：" + PathDisplay(input.identity.repositoryDirectory) + L"\n";
  preview += L"  Git 目录：" + PathDisplay(input.identity.absoluteGitDir) + L"\n";
  preview += L"  分支／HEAD：" + HeadDisplay(input.detection) + L"\n";
  preview += L"      完整分支引用：" + DetachedOrRef(input.identity) + L"\n";
  preview += L"      HEAD 完整对象 ID：" +
             (input.identity.headResolved ? ObjectIdDisplay(input.identity.headObjectId)
                                          : std::wstring(L"（尚无提交，这次是仓库的第一个提交）")) +
             L"\n";
  preview += L"  索引内容标识：" + ObjectIdDisplay(input.identity.indexTreeOid) + L"\n";
  preview += L"      （由 git write-tree 当场把这 " + std::to_wstring(input.model.staged.size()) +
             L" 项已暂存内容算成的树对象；索引里的路径、模式或内容有任何变化，这个 ID 就一定变。）\n\n";

  preview += L"本次提交包含已暂存的 " + std::to_wstring(input.model.staged.size()) + L" 项：\n";
  size_t listed = 0;
  for (const ChangeItem& item : input.model.staged) {
    if (listed >= kMaxPreviewItems) {
      break;
    }
    preview += StagedItemLine(item) + L"\n";
    ++listed;
  }
  if (input.model.staged.size() > listed) {
    preview += L"  ……另有 " + std::to_wstring(input.model.staged.size() - listed) + L" 项未列出。\n";
  }
  if (!input.model.unstaged.empty()) {
    preview += L"（未暂存的 " + std::to_wstring(input.model.unstaged.size()) +
               L" 项改动不会进入这次提交，仍留在工作区。）\n";
  }
  preview += L"\n";

  preview += L"提交信息（写进 " + Ellipsis(input.messageFilePath, 64) + L"，" +
             std::to_wstring(input.messageUtf8Bytes) + L" 字节）：\n";
  const std::vector<std::wstring> lines = SplitWideLines(input.message);
  size_t shown = 0;
  for (const std::wstring& line : lines) {
    if (shown >= kMaxPreviewMessageLines) {
      break;
    }
    preview += L"  " + Ellipsis(line, 110) + L"\n";
    ++shown;
  }
  if (lines.size() > shown) {
    preview += L"  ……另有 " + std::to_wstring(lines.size() - shown) + L" 行未列出。\n";
  }
  preview += L"\n";

  preview += TimeLineText(L"作者时间（GIT_AUTHOR_DATE）", input.authorTime) + L"\n";
  preview += L"作者（GIT_AUTHOR_NAME / GIT_AUTHOR_EMAIL，只覆盖这一次提交）：" + input.author.Format() + L"\n";
  preview += TimeLineText(L"提交者时间（GIT_COMMITTER_DATE）", input.committerTime);
  if (input.timesSynced) {
    preview += L"　（“时间同步修改”已勾选：提交者时间与作者时间一致）";
  }
  preview += L"\n";
  preview += L"提交者身份仍由这个仓库的有效 Git 配置决定：" +
             (input.identity.committerIdentityText.empty()
                 ? std::wstring(L"（本程序没有读到，Git 会自己按配置取值）")
                 : input.identity.committerIdentityText) +
             L"\n\n";

  preview += L"命令里没有 --no-verify：仓库的 hooks 与签名设置照常生效，需要口令时由命令窗口自己提问。\n";
  preview += L"这些时间与身份只覆盖这次的 Git 子进程，不会改动你的环境变量、也不写任何配置文件。\n";
  preview += L"点“确定”之后、发出命令之前，程序会把上面这份现状用同一组只读查询重发一遍核对"
             L"（工作区根与 Git 目录／完整分支引用／HEAD 完整对象 ID／索引内容标识／流程痕迹／提交者身份配置）。"
             L"任何一条对不上，这条命令就不会发出，表单里的字不动，需要对新现状重新确认一次。\n";
  preview += L"这条保证的边界要说清楚：核对发生在“确认到建立进程”之间，进程建立之后到 Git 真正读索引"
             L"之间还有一段窗口，那期间归 Git 自己的 index.lock 管——本程序不持有也无法持有那个锁"
             L"（自己锁着它再启动 git commit 只会必然失败）。仓库里的 prepare-commit-msg / pre-commit 之类"
             L"hooks 本来也能改变最终提交的内容（例如格式化工具再 git add），那发生在核对之后，"
             L"本程序不阻止也无法预先测出。所以这里说的是“确认的范围与发出的命令一致”，"
             L"不是事务级的隔离保证。\n";
  preview += L"另外交代一处副作用：算索引内容标识要执行一次 git write-tree，它会把这棵树写进对象库"
             L"（没有任何引用指向它，之后由 Git 的垃圾回收清理），索引里过期的文件状态信息可能被顺带刷新"
             L"——条目、内容、模式都不动，分支与 HEAD 也不动。\n";
  preview += L"确定要执行吗？取消不会打开命令窗口，也不会改动仓库。";

  if (input.captured.valid) {
    // HEAD 的比较用「短 ID + 能不能解析」两件事：短 ID 有一边拿不到时不硬比，
    // 免得把「界面当时没显示 ID」误报成「HEAD 变了」。
    const bool headComparable =
        !input.captured.shortSha.empty() && !input.detection.shortSha.empty();
    const bool headChanged =
        (headComparable && input.captured.shortSha != input.detection.shortSha) ||
        input.captured.hasHead != input.detection.headResolved;
    const bool stagedChanged = input.captured.stagedItems != input.model.staged.size();
    if (headChanged || stagedChanged) {
      std::wstring note = L"界面原先显示的仓库现状与刚刚读回的已经不同：";
      if (headChanged) {
        note += L"HEAD 从「" + (input.captured.shortSha.empty() ? std::wstring(L"尚无提交")
                                                                : input.captured.shortSha) +
                L"」变成「" + (input.detection.shortSha.empty() ? std::wstring(L"尚无提交")
                                                                 : input.detection.shortSha) +
                L"」；";
      }
      if (stagedChanged) {
        note += L"已暂存的条目从 " + std::to_wstring(input.captured.stagedItems) + L" 项变成 " +
                std::to_wstring(input.model.staged.size()) + L" 项；";
      }
      note += L"下面的确认内容以刚刚读回的为准（列表也已经就地更新）。请复核后再决定。";
      plan.stateChangeNote = note;
      preview = note + L"\n\n" + preview;
    }
  }

  plan.previewText = std::move(preview);
  std::wstring notice = L"本次提交只包含刚刚读回的 " + std::to_wstring(input.model.staged.size()) +
                        L" 项已暂存内容（索引内容标识 " + ObjectIdDisplay(input.identity.indexTreeOid) +
                        L"）；未暂存的改动仍留在工作区。";
  if (input.identity.workflow.indexLocked) {
    // 锁归 Git 自己管：这里只提醒，不去删锁、也不替谁终止。
    notice += L"（注意：仓库里已有 index.lock，另一个 Git 进程可能正在写索引，这一次提交"
              L"很可能被 Git 自己拒绝）";
  }
  plan.notice = std::move(notice);
  plan.stagedItems = input.model.staged.size();
  return plan;
}

}  // namespace gc::git
