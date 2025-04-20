#include "git/commit_plan.h"

#include <algorithm>
#include <string>
#include <vector>

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
  if (input.committer != CommitterIdentityState::available) {
    const std::wstring cause = input.committer == CommitterIdentityState::missing
                                   ? L"这个仓库的有效 Git 配置里凑不出提交者身份（user.name 与 user.email "
                                     L"至少有一项没设）。"
                                   : L"还没读到这个仓库的有效 Git 配置，无法确定提交者身份。";
    return block(L"提交者的身份由 Git 配置决定，本程序不替你写配置，也不在表单里输入它。" + cause +
                 L"请先在 Git 里设好 user.name 与 user.email，再点“刷新”重读。");
  }

  // 進行中的特殊流程：Git 在那種狀態下提交的含义與「普通提交」完全不同（合併提交有第二個父、
  // 變基中途提交會推進 todo 清單）。本步驟只支持「索引相對 HEAD 的一次普通提交」，
  // 因此明确拒绝，也不去碰那種流程的現場（不 abort、不 --quit）。
  if (input.workflow.HasSpecialFlowInProgress()) {
    return block(L"这个仓库里有 Git 流程还没走完：" + input.workflow.SpecialFlowText() +
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

  // ---- 確認文字：把「要提交什麼」一次講完 ----
  std::wstring preview;
  // 把真正交给 Git 的参数原样列出来（命令窗口里执行并回显的也是这一条），让用户核对的是同一件事。
  std::wstring commandLine = L"  git";
  for (const std::wstring& argument : plan.arguments) {
    commandLine += L' ' + argument;
  }
  preview += L"将在命令窗口里执行（工作目录：" + input.detection.root + L"）：\n";
  preview += commandLine + L"\n\n";

  preview += L"仓库：" + input.detection.root + L"\n";
  preview += L"分支／HEAD：" + HeadDisplay(input.detection) + L"\n\n";

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
             (input.committerIdentityText.empty() ? std::wstring(L"（本程序没有读到，Git 会自己按配置取值）")
                                                  : input.committerIdentityText) +
             L"\n\n";

  preview += L"命令里没有 --no-verify：仓库的 hooks 与签名设置照常生效，需要口令时由命令窗口自己提问。\n";
  preview += L"这些时间与身份只覆盖这次的 Git 子进程，不会改动你的环境变量、也不写任何配置文件。\n";
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
                        L" 项已暂存内容；未暂存的改动仍留在工作区。";
  if (input.workflow.indexLocked) {
    // 锁归 Git 自己管：这里只提醒，不去删锁、也不替谁终止。
    notice += L"（注意：仓库里已有 index.lock，另一个 Git 进程可能正在写索引，这一次提交"
              L"很可能被 Git 自己拒绝）";
  }
  plan.notice = std::move(notice);
  plan.stagedItems = input.model.staged.size();
  return plan;
}

}  // namespace gc::git
