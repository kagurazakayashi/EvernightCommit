#include "git/staging_plan.h"

#include <algorithm>
#include <set>

#include "git/diff_view.h"

namespace gc::git {
namespace {

// 与内部只读查询同一组全局选项：
//   --no-optional-locks  —— 不让 Git 顺手做需要上锁的额外工作；
//   --no-replace-objects —— 不套用 replace 引用；
//   -c core.quotepath=off —— 只影响 Git 在窗口里回显路径的显示形态（中文档名不变成八进制），
//     不改变哪一个文件被匹配，也不影响索引内容。
constexpr std::wstring_view kGlobalPrefix[] = {L"--no-optional-locks", L"--no-replace-objects", L"-c",
                                               L"core.quotepath=off"};

constexpr std::wstring_view kAddSubcommand = L"add";
constexpr std::wstring_view kOptionTerminator = L"--";
constexpr std::wstring_view kPathspecFileOption = L"--pathspec-from-file=";
constexpr std::wstring_view kPathspecNulOption = L"--pathspec-file-nul";
constexpr std::wstring_view kOperationId = L"stage-add";

// inline 形態的自設餘量：執行器另有「64 個參數」與「8000 字元行長」兩道硬性檢查，
// 這裡先收緊，使「放不下」在構造階段就换来一句明确說明，而不是等執行器拒收後才報錯。
inline constexpr size_t kMaxInlinePathspecEntries = 48;
// 每条路径至少还要一圈引号，程序段（git.exe 绝对路径）按 260 字元预留。
inline constexpr size_t kInlineProgramSegmentReserve = 260;
inline constexpr size_t kMaxInlineCommandChars = 6000;

// 估算 inline 形態的命令行长度（与实际引号形态同量级，用于提前拒绝过长的一条）。
size_t EstimateInlineCommandLength(const std::vector<std::wstring>& entries) {
  size_t total = kInlineProgramSegmentReserve;
  for (const std::wstring_view prefix : kGlobalPrefix) {
    total += prefix.size() + 3;
  }
  total += kAddSubcommand.size() + kOptionTerminator.size() + 3;
  for (const std::wstring& entry : entries) {
    // 条目本身已含 `:(literal)` 前缀，外面再绕一圈引号。
    total += entry.size() + 2;
  }
  return total;
}

// 哪些未暂存条目可以由一次 `git add` 处理。
// added 不在内：新增只发生在索引侧，未暂存列表里出现它说明模型与显示已不一致，
// 与其猜一条命令，不如整份拒绝并让用户刷新。
bool CanStageByAdd(ChangeKind kind) noexcept {
  switch (kind) {
    case ChangeKind::modified:
    case ChangeKind::deleted:
    case ChangeKind::typeChange:
    case ChangeKind::untracked:
    case ChangeKind::conflicted:
    case ChangeKind::submodule:
    case ChangeKind::renamed:
    case ChangeKind::copied:
      return true;
    case ChangeKind::unknown:
    case ChangeKind::added:
      return false;
  }
  return false;
}

[[nodiscard]] std::wstring JoinWith(const std::vector<std::wstring>& parts, std::wstring_view separator) {
  std::wstring result;
  for (size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      result += separator;
    }
    result += parts[index];
  }
  return result;
}

// 状态栏摘要：最多列若干条，剩下的用「等 N 项」收尾，避免把一行状态撑成不可读。
[[nodiscard]] std::wstring BuildSelectionSummary(const std::vector<ChangeItem>& selected) {
  constexpr size_t kShownInSummary = 6;
  std::vector<std::wstring> names;
  for (const ChangeItem& item : selected) {
    names.push_back(item.PathLabel());
  }
  if (names.size() <= kShownInSummary) {
    return JoinWith(names, L"、");
  }
  names.resize(kShownInSummary);
  return JoinWith(names, L"、") + L" 等 " + std::to_wstring(selected.size()) + L" 项";
}

// 子模块条目的范围说明：父仓库这一次 add 到底会写进索引什么，必须当场讲清楚。
[[nodiscard]] std::wstring BuildSubmoduleNotice(const std::vector<ChangeItem>& selected) {
  std::vector<std::wstring> lines;
  for (const ChangeItem& item : selected) {
    if (item.kind != ChangeKind::submodule) {
      continue;
    }
    std::wstring internal;
    if (item.submodule.commitChanged) {
      internal = L"提交指针已移动（父仓库这次会记录子模块当前的提交 ID）";
    } else if (item.submodule.trackedChanges || item.submodule.untrackedChanges) {
      internal = L"提交指针未移动";
    } else {
      internal = L"没有可见变化";
    }
    if (item.submodule.trackedChanges) {
      internal += L"；内部已跟踪文件有未提交改动";
    }
    if (item.submodule.untrackedChanges) {
      internal += L"；内部有未跟踪文件";
    }
    lines.push_back(L"　子模块 " + item.path + L"：" + internal + L"。\n"
                    L"　　父仓库对子模块只有一条 gitlink 记录，git add 不会进入子模块暂存它内部的任何文件；"
                    L"要保存内部改动，必须进入 " + item.path + L" 那个仓库自己暂存并提交，"
                    L"父仓库随后才能记录下那个新的提交指针。");
  }
  if (lines.empty()) {
    return {};
  }
  return JoinWith(lines, L"\n");
}

// 未合并（冲突）条目的确认文案。程序不判断冲突是否真的解决 —— 只有用户自己知道，
// 所以把 Git 的实际语义（当前内容进索引、记为已解决）原样说出来，由用户决定继续与否。
[[nodiscard]] std::wstring BuildConflictNotice(const std::vector<ChangeItem>& selected) {
  std::vector<std::wstring> paths;
  for (const ChangeItem& item : selected) {
    if (item.kind == ChangeKind::conflicted) {
      paths.push_back(item.path);
    }
  }
  if (paths.empty()) {
    return {};
  }
  return L"以下 " + std::to_wstring(paths.size()) + L" 个条目当前处于未合并（冲突）状态：\n" +
         JoinWith(paths, L"\n") +
         L"\ngit add 会把它们**工作区里的当前内容**写进索引并记为已解决。"
         L"本程序没有检查冲突是否真的解决完 —— 如果文件里还留着 <<<<<<< 之类的冲突标记，"
         L"那些标记也会被一起暂存（已在本机 Git 实测确认）。";
}

}  // namespace

bool SupportsPathspecFileDelivery(std::wstring_view versionText) {
  // 形如 "2.56.0.windows.1"、"2.25.0"，也容忍 "git version 2.30.1" 这种带前缀的原文。
  size_t cursor = 0;
  const auto readNumber = [&](int* out) -> bool {
    size_t start = cursor;
    while (start < versionText.size() && versionText[start] >= L'0' && versionText[start] <= L'9') {
      ++start;
    }
    if (start == cursor) {
      return false;
    }
    long long value = 0;
    for (size_t index = cursor; index < start; ++index) {
      value = value * 10 + (versionText[index] - L'0');
      if (value > 1000000) {
        return false;  // 版本号位数异常：视为解析失败，不猜测。
      }
    }
    *out = static_cast<int>(value);
    cursor = start;
    return true;
  };

  while (cursor < versionText.size() &&
         !(versionText[cursor] >= L'0' && versionText[cursor] <= L'9')) {
    ++cursor;
  }
  int major = 0;
  int minor = 0;
  if (!readNumber(&major)) {
    return false;
  }
  if (cursor >= versionText.size() || versionText[cursor] != L'.') {
    return false;  // 只解析出一个数字（例如 "2"）不足以判定主次版本。
  }
  ++cursor;
  if (!readNumber(&minor)) {
    return false;
  }
  return major > 2 || (major == 2 && minor >= 25);
}

bool AppendPathspecFileOptions(std::vector<std::wstring>* arguments, std::wstring_view pathspecFilePath) {
  if (arguments == nullptr || pathspecFilePath.empty()) {
    return false;
  }
  // 与执行器的边界校验同源：清单路径最终要进 cmd 脚本的 call 行。
  // 这两项检查本模块自己先做一次，界面才能在「写得出文件但送不出去」时给出准确原因。
  if (pathspecFilePath.find(L'"') != std::wstring_view::npos) {
    return false;
  }
  for (const wchar_t c : pathspecFilePath) {
    if (c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  arguments->push_back(std::wstring(kPathspecFileOption) + std::wstring(pathspecFilePath));
  arguments->push_back(std::wstring(kPathspecNulOption));
  return true;
}

StagingPlan BuildStagingAddPlan(const std::vector<ChangeItem>& selected, const StagingPlanOptions& options) {
  StagingPlan plan;
  plan.selectedItems = selected.size();

  if (selected.empty()) {
    plan.blockedReason =
        L"没有选中任何条目。这个按钮只处理选中的变化，绝不会代为暂存整个仓库"
        L"（不会执行 git add . 或 git add -A）。请在“未暂存的更改”里点选一行或多行后再点击。";
    return plan;
  }

  // 路径收集：先全部校验、全部去重，任何一条不合格都不发出命令 —— 半份清单会让用户
  // 以为「选中的都进了索引」，而实际只进了一半。
  // 收集到的每条都已带上 `:(literal)` 前缀：清单形态与参数形态共用同一份元素，
  // 去重按原始路径做（重命名的旧路径可能与另一条目同路径，前缀不影响去重结果）。
  std::vector<std::wstring> entries;
  std::set<std::wstring> seen;
  for (const ChangeItem& item : selected) {
    if (!CanStageByAdd(item.kind)) {
      plan.blockedReason = L"选中条目的状态（" + item.StatusLabel() +
                           L"）不属于“未暂存的变化”，无法确定该用哪条命令处理。\n条目：" +
                           item.PathLabel() + L"\n本次没有执行任何命令，请点“刷新”后重新选择。";
      return plan;
    }
    std::wstring problem;
    if (!IsWorktreeRelativePath(item.path, &problem)) {
      plan.blockedReason = L"这条记录的路径不符合仓库相对路径的约定，已拒绝执行任何命令。\n原因：" +
                           problem + L"\n条目：" + item.PathLabel();
      return plan;
    }
    // 重命名/复制条目：旧路径的删除与新路径的添加是同一次变化，必须成对交给同一条命令。
    if (!item.oldPath.empty()) {
      if (item.kind != ChangeKind::renamed && item.kind != ChangeKind::copied) {
        plan.blockedReason = L"选中条目带有来源路径，但状态不是重命名/复制，无法确定该如何处理。\n条目：" +
                             item.PathLabel() + L"\n本次没有执行任何命令，请点“刷新”后重新选择。";
        return plan;
      }
      if (!IsWorktreeRelativePath(item.oldPath, &problem)) {
        plan.blockedReason = L"这条重命名记录的来源路径不符合仓库相对路径的约定，已拒绝执行任何命令。\n原因：" +
                             problem + L"\n条目：" + item.PathLabel();
        return plan;
      }
      // 旧路径排在前面：与 diff 输出里 `rename from/to` 的阅读顺序一致。
      if (seen.insert(item.oldPath).second) {
        entries.push_back(MakeLiteralPathspec(item.oldPath));
      }
    }
    if (seen.insert(item.path).second) {
      entries.push_back(MakeLiteralPathspec(item.path));
    }
  }

  if (entries.empty()) {
    plan.blockedReason = L"选中的条目里没有可暂存的路径，本次没有执行任何命令。请点“刷新”后重新选择。";
    return plan;
  }

  plan.pathspecEntries = entries;
  plan.pathspecCount = entries.size();
  plan.operationId = std::wstring(kOperationId);
  plan.displayName = L"加入暂存区 " + std::to_wstring(selected.size()) + L" 项";
  plan.selectionSummary = BuildSelectionSummary(selected);

  const size_t unselected = options.totalUnstagedItems > selected.size()
                                ? options.totalUnstagedItems - selected.size()
                                : 0;
  std::wstring scope = L"本次只暂存选中的 " + std::to_wstring(selected.size()) + L" 项" +
                       (plan.pathspecCount != selected.size()
                            ? L"（含重命名的来源路径，共 " + std::to_wstring(plan.pathspecCount) + L" 条路径）"
                            : std::wstring()) +
                       (unselected == 0 ? std::wstring()
                                        : L"，另有 " + std::to_wstring(unselected) + L" 项未选中、保持原状");

  std::vector<std::wstring> arguments;
  for (const std::wstring_view prefix : kGlobalPrefix) {
    arguments.push_back(std::wstring(prefix));
  }
  arguments.push_back(std::wstring(kAddSubcommand));

  if (options.pathspecFileSupported) {
    // 路径清单走临时文件：命令行里只有档名，条目再多也不受 cmd 行长度限制。
    // 字面语义由每条路径自带的 `:(literal)` 前缀保证 —— 实测 NUL 分隔只关闭引号转义，
    // 不关闭 glob 魔法，光靠 -z 会让 brk[x].txt 连带命中 brkx.txt。
    plan.delivery = StagingDelivery::pathspecFile;
    plan.arguments = std::move(arguments);
    plan.notice = std::move(scope) +
                  L"。清单通过 NUL 分隔的临时文件传给 git add，每条路径都带 :(literal) 字面标记，"
                  L"通配符、取反与前导减号都不会扩大范围；"
                  L"命令窗口里可以看到实际执行的命令与 Git 的完整输出。";
  } else if (entries.size() <= kMaxInlinePathspecEntries &&
             EstimateInlineCommandLength(entries) <= kMaxInlineCommandChars) {
    // 兼容路径：旧 Git 没有清单接口，仍按字面 pathspec 逐条给出，范围与清单形态完全一致。
    plan.delivery = StagingDelivery::inlinePathspec;
    arguments.push_back(std::wstring(kOptionTerminator));
    for (const std::wstring& entry : entries) {
      arguments.push_back(entry);
    }
    plan.arguments = std::move(arguments);
    plan.notice = std::move(scope) +
                  L"。当前 Git 版本不支持路径清单文件接口（需 2.25 及以上），"
                  L"已把这 " + std::to_wstring(entries.size()) +
                  L" 条路径按字面 pathspec 直接写进命令；路径过多时本程序会拒绝执行，"
                  L"不会改用 git add . 或暂存全部。";
  } else {
    plan.delivery = StagingDelivery::blocked;
    plan.blockedReason =
        L"选中的条目共 " + std::to_wstring(entries.size()) +
        L" 条路径，超出当前 Git 版本可用的命令行承载能力"
        L"（本程序对直接传参的限制是 " +
        std::to_wstring(kMaxInlinePathspecEntries) + L" 条、约 " +
        std::to_wstring(kMaxInlineCommandChars) + L" 字符）。\n"
        L"本次没有执行任何命令：不会退化成 git add . 或暂存全部，也不会自作主张分批。"
        L"请减少一次选择的条目数，或把 Git 升级到 2.25 以上以启用路径清单文件接口。";
    return plan;
  }

  const std::wstring conflictNotice = BuildConflictNotice(selected);
  const std::wstring submoduleNotice = BuildSubmoduleNotice(selected);
  std::vector<std::wstring> confirmParts;
  if (!conflictNotice.empty()) {
    confirmParts.push_back(conflictNotice);
  }
  if (!submoduleNotice.empty()) {
    confirmParts.push_back(submoduleNotice);
  }
  if (!confirmParts.empty()) {
    plan.confirmationText = JoinWith(confirmParts, L"\n\n") +
                            L"\n\n是否仍在命令窗口里执行这条 git add？（取消不会改动仓库。）";
  }
  return plan;
}

}  // namespace gc::git
