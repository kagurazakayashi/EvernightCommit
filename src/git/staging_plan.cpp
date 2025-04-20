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
constexpr std::wstring_view kRestoreSubcommand = L"restore";
constexpr std::wstring_view kResetSubcommand = L"reset";
constexpr std::wstring_view kOptionTerminator = L"--";
constexpr std::wstring_view kPathspecFileOption = L"--pathspec-from-file=";
constexpr std::wstring_view kPathspecNulOption = L"--pathspec-file-nul";
constexpr std::wstring_view kOperationId = L"stage-add";
constexpr std::wstring_view kUnstageOperationId = L"stage-unstage";

// inline 形態的自設餘量：執行器另有「64 個參數」與「8000 字元行長」兩道硬性檢查，
// 這裡先收緊，使「放不下」在構造階段就换来一句明确說明，而不是等執行器拒收後才報錯。
inline constexpr size_t kMaxInlinePathspecEntries = 48;
// 每条路径至少还要一圈引号，程序段（git.exe 绝对路径）按 260 字元预留。
inline constexpr size_t kInlineProgramSegmentReserve = 260;
inline constexpr size_t kMaxInlineCommandChars = 6000;

// 估算 inline 形態的命令行长度（与实际引号形态同量级，用于提前拒绝过长的一条）。
size_t EstimateInlineCommandLength(const std::vector<std::wstring>& entries, std::wstring_view subcommand) {
  size_t total = kInlineProgramSegmentReserve;
  for (const std::wstring_view prefix : kGlobalPrefix) {
    total += prefix.size() + 3;
  }
  total += subcommand.size() + kOptionTerminator.size() + 3;
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

// 哪些已暂存条目可以由「只写索引的取消暂存」处理。
// untracked 与 conflicted 都不属于索引相对 HEAD 的变化：前者根本不在索引里，
// 后者是未合并记录（本程序只把它放在未暂存一侧），出现在这里即说明模型与显示不一致。
bool CanUnstageByIndex(ChangeKind kind) noexcept {
  switch (kind) {
    case ChangeKind::added:
    case ChangeKind::modified:
    case ChangeKind::deleted:
    case ChangeKind::typeChange:
    case ChangeKind::renamed:
    case ChangeKind::copied:
    case ChangeKind::submodule:
      return true;
    case ChangeKind::unknown:
    case ChangeKind::untracked:
    case ChangeKind::conflicted:
      return false;
  }
  return false;
}

// XY 两栏里出现 U 即未合并记录。取消暂存在这种路径上不是「退回 HEAD」那么简单：
// 实测它会把三份 stage 记录收拢成 HEAD 的那一份，等于把冲突现场抹掉一半，
// 因此绝不执行，也不改用别的命令「顺手」处理。
bool IsUnmergedStatusCode(std::wstring_view statusCode) noexcept {
  return statusCode.find(L'U') != std::wstring_view::npos;
}

// 「内容只存在於索引裡」的條目：HEAD 沒有這個路徑（X 為 A/R/C），工作區又已經沒有那個檔案（Y 為 D）。
// 此時索引是唯一的一份，把它退回 HEAD 等於什麼都不剩 —— 必須先由使用者點頭。
bool IsIndexOnlyContent(const ChangeItem& item) {
  if (item.statusCode.size() < 2 || IsUnmergedStatusCode(item.statusCode)) {
    return false;
  }
  const wchar_t headSide = item.statusCode[0];
  const bool missingInWorktree = item.statusCode[1] == L'D';
  const bool absentFromHead = headSide == L'A' || headSide == L'R' || headSide == L'C';
  return missingInWorktree && absentFromHead;
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

// 「本次只……另有 K 项未选中」的範圍說明，兩個方向共用（重命名的來源路徑要如實報出）。
[[nodiscard]] std::wstring BuildScopeText(std::wstring_view verb, size_t selectedItems,
                                          size_t pathspecCount, size_t totalItems) {
  const size_t unselected = totalItems > selectedItems ? totalItems - selectedItems : 0;
  return std::wstring(verb) + std::to_wstring(selectedItems) + L" 项" +
         (pathspecCount != selectedItems
              ? L"（含重命名的来源路径，共 " + std::to_wstring(pathspecCount) + L" 条路径）"
              : std::wstring()) +
         (unselected == 0 ? std::wstring()
                          : L"，另有 " + std::to_wstring(unselected) + L" 项未选中、保持原状");
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

// 取消暂存时的子模块说明：父仓库这一侧退回去的只有 gitlink 指针，
// 而且因为子模块自己检出的提交没变，这条指针改动会重新落回未暂存那一侧（实测）。
[[nodiscard]] std::wstring BuildUnstageSubmoduleNotice(const std::vector<ChangeItem>& selected) {
  std::vector<std::wstring> lines;
  for (const ChangeItem& item : selected) {
    if (item.kind != ChangeKind::submodule) {
      continue;
    }
    std::wstring now = L"索引里记录的提交指针与最近一次提交的不一致（本次就是把它退回上一次记录的值）";
    if (item.submodule.trackedChanges) {
      now += L"；内部已跟踪文件有未提交改动";
    }
    if (item.submodule.untrackedChanges) {
      now += L"；内部有未跟踪文件";
    }
    lines.push_back(L"　子模块 " + item.path + L"：" + now + L"。\n"
                    L"　　父仓库对子模块只有一条 gitlink 记录，取消暂存只把这条指针退回最近一次提交里的值，"
                    L"不会进入 " + item.path +
                        L" 改动子模块内部的任何文件（本机实测：命令跑完后子模块工作区分毫未动）。"
                    L"由于子模块当前检出的提交并没有因此改变，父仓库随后仍会把这条指针改动显示在“未暂存的更改”里；"
                    L"要让指针真正定下来，必须进入那个仓库自己暂存并提交。");
  }
  if (lines.empty()) {
    return {};
  }
  return JoinWith(lines, L"\n");
}

// 取消暂存前必须点头的另一类条目：内容只存在于索引里（HEAD 没有这个路径，工作区也已没有这个文件）。
// 这类暂存记录是那份字节唯一的容身之处，退回 HEAD 就是把它删掉。
[[nodiscard]] std::wstring BuildIndexOnlyContentNotice(const std::vector<ChangeItem>& selected) {
  std::vector<std::wstring> paths;
  for (const ChangeItem& item : selected) {
    if (IsIndexOnlyContent(item)) {
      paths.push_back(item.path);
    }
  }
  if (paths.empty()) {
    return {};
  }
  return L"以下 " + std::to_wstring(paths.size()) +
         L" 个条目在工作区里已经没有对应的文件，而它们的内容只存在于索引中"
         L"（最近一次提交里没有这个路径，磁盘上也没有这个文件）：\n" +
         JoinWith(paths, L"\n") +
         L"\n把它们移出暂存区会删掉索引里的这条记录，那份内容随之没有任何副本可以找回"
         L"（本程序不会替你把它写到磁盘上，也不会用任何会丢弃改动的命令代替）。"
         L"若想保留，请先取消，把文件放回磁盘再决定怎么处理。";
}

// 选中条目的路径收集与校验：两个方向共用同一套准入规则，任何一条不合格都整份拒绝 ——
// 半份清单会让用户以为「选中的都处理了」，而实际只做了一半。
// 收集到的每条都带上 `:(literal)` 前缀（清单形态与参数形态共用同一份元素），
// 去重按原始路径做，重命名条目把来源路径一并给出（实测只给新路径会留下半套索引）。
struct PathspecCollection {
  bool accepted = false;
  std::wstring refusalReason;
  std::vector<std::wstring> entries;  // 已带 :(literal) 前缀，可直接进清单文件或参数数组
};

PathspecCollection CollectSelectedPathspecs(const std::vector<ChangeItem>& selected, bool stagedSide) {
  PathspecCollection collection;
  std::set<std::wstring> seen;
  const std::wstring_view sideLabel = stagedSide ? L"已暂存" : L"未暂存";
  for (const ChangeItem& item : selected) {
    const bool admissible = stagedSide ? CanUnstageByIndex(item.kind) : CanStageByAdd(item.kind);
    if (!admissible) {
      collection.refusalReason = L"选中条目的状态（" + item.StatusLabel() + L"）不属于“" +
                                 std::wstring(sideLabel) + L"的变化”，无法确定该用哪条命令处理。\n条目：" +
                                 item.PathLabel() + L"\n本次没有执行任何命令，请点“刷新”后重新选择。";
      return collection;
    }
    // 未合并记录绝不允许进入取消暂存：实测它会把三份 stage 记录收拢成 HEAD 那一份，
    // 冲突现场被抹掉一半，而这不是「解决冲突」。
    if (stagedSide && IsUnmergedStatusCode(item.statusCode)) {
      collection.refusalReason =
          L"选中条目 " + item.PathLabel() +
          L" 在索引里处于未合并（冲突）状态。取消暂存不是解决冲突：它会把这些冲突记录"
          L"收拢成最近一次提交里的那一份，工作区里的冲突标记却仍然留着（本机实测）。\n"
          L"本次没有执行任何命令。请先按解决冲突的正常流程处理（在工作区把内容改好后用“加入暂存区”"
          L"记下解决结果，或明确地中止合并），再点“刷新”。";
      return collection;
    }
    std::wstring problem;
    if (!IsWorktreeRelativePath(item.path, &problem)) {
      collection.refusalReason = L"这条记录的路径不符合仓库相对路径的约定，已拒绝执行任何命令。\n原因：" +
                                 problem + L"\n条目：" + item.PathLabel();
      return collection;
    }
    // 重命名/复制条目：旧路径的删除与新路径的添加是同一次变化，必须成对交给同一条命令。
    if (!item.oldPath.empty()) {
      if (item.kind != ChangeKind::renamed && item.kind != ChangeKind::copied) {
        collection.refusalReason =
            L"选中条目带有来源路径，但状态不是重命名/复制，无法确定该如何处理。\n条目：" + item.PathLabel() +
            L"\n本次没有执行任何命令，请点“刷新”后重新选择。";
        return collection;
      }
      if (!IsWorktreeRelativePath(item.oldPath, &problem)) {
        collection.refusalReason = L"这条重命名记录的来源路径不符合仓库相对路径的约定，已拒绝执行任何命令。\n原因：" +
                                   problem + L"\n条目：" + item.PathLabel();
        return collection;
      }
      // 旧路径排在前面：与 diff 输出里 `rename from/to` 的阅读顺序一致。
      if (seen.insert(item.oldPath).second) {
        collection.entries.push_back(MakeLiteralPathspec(item.oldPath));
      }
    }
    if (seen.insert(item.path).second) {
      collection.entries.push_back(MakeLiteralPathspec(item.path));
    }
  }
  collection.accepted = !collection.entries.empty();
  if (!collection.accepted && collection.refusalReason.empty()) {
    collection.refusalReason = L"选中的条目里没有可处理的路径，本次没有执行任何命令。请点“刷新”后重新选择。";
  }
  return collection;
}

// 两种传递形态的组装：清单临时文件（路径不进命令行）或字面 pathspec 参数（旧 Git）。
// 形状参数只描述「这条命令长什么样」，范围语义（子命令自己的选项）由调用方给定，
// 因此两个方向共用同一套条数/行长余量与同一句「超了就拒绝，绝不扩大范围」的策略。
struct DeliveryShape {
  std::wstring_view subcommand;              // add / restore / reset
  std::wstring_view commandLabel;            // 给用户看的完整命令名（git add / git restore --staged / git reset -q）
  std::vector<std::wstring> subcommandFlags;  // 子命令自己的选项：--staged、-q
  std::wstring scopeText;                     // 「本次只……另有 K 项未选中」
  std::wstring pathspecFileTail;              // 清单形态的补充说明
  std::wstring inlineTail;                    // 参数形态的补充说明
  std::wstring tooManyTail;                   // 超出承载能力时的拒绝说明
};

void ApplyDelivery(StagingPlan& plan, const std::vector<std::wstring>& entries,
                   const StagingPlanOptions& options, const DeliveryShape& shape) {
  std::vector<std::wstring> arguments;
  for (const std::wstring_view prefix : kGlobalPrefix) {
    arguments.push_back(std::wstring(prefix));
  }
  arguments.push_back(std::wstring(shape.subcommand));
  for (const std::wstring& flag : shape.subcommandFlags) {
    arguments.push_back(flag);
  }

  if (options.pathspecFileSupported) {
    // 路径清单走临时文件：命令行里只有档名，条目再多也不受 cmd 行长度限制。
    // 字面语义由每条路径自带的 `:(literal)` 前缀保证 —— 实测 NUL 分隔只关闭引号转义，
    // 不关闭 glob 魔法，光靠 -z 会让 brk[x].txt 连带命中 brkx.txt。
    plan.delivery = StagingDelivery::pathspecFile;
    plan.arguments = std::move(arguments);
    plan.notice = shape.scopeText + shape.pathspecFileTail;
    return;
  }
  if (entries.size() <= kMaxInlinePathspecEntries &&
      EstimateInlineCommandLength(entries, shape.subcommand) <= kMaxInlineCommandChars) {
    // 兼容路径：旧 Git 没有清单接口，仍按字面 pathspec 逐条给出，范围与清单形态完全一致。
    plan.delivery = StagingDelivery::inlinePathspec;
    arguments.push_back(std::wstring(kOptionTerminator));
    for (const std::wstring& entry : entries) {
      arguments.push_back(entry);
    }
    plan.arguments = std::move(arguments);
    plan.notice = shape.scopeText + shape.inlineTail;
    return;
  }
  plan.delivery = StagingDelivery::blocked;
  plan.arguments.clear();
  plan.blockedReason =
      L"选中的条目共 " + std::to_wstring(entries.size()) +
      L" 条路径，超出当前 Git 版本可用的命令行承载能力"
      L"（本程序对直接传参的限制是 " +
      std::to_wstring(kMaxInlinePathspecEntries) + L" 条、约 " +
      std::to_wstring(kMaxInlineCommandChars) + L" 字符）。\n" + shape.tooManyTail;
}

}  // namespace

namespace {

// 从 `git --version` 的回报文本里读出众主次版本号：形如 "2.56.0.windows.1"、"2.25.0"，
// 也容忍 "git version 2.30.1" 这种带前缀的原文。读不出来（没有数字、只有一个数字、
// 位数离谱）一律回 false —— 调用方据此退回「两种版本都合法」的命令形态，绝不猜。
bool ParseMajorMinor(std::wstring_view versionText, int* major, int* minor) {
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
  if (!readNumber(major)) {
    return false;
  }
  if (cursor >= versionText.size() || versionText[cursor] != L'.') {
    return false;  // 只解析出一个数字（例如 "2"）不足以判定主次版本。
  }
  ++cursor;
  return readNumber(minor);
}

}  // namespace

bool SupportsPathspecFileDelivery(std::wstring_view versionText) {
  int major = 0;
  int minor = 0;
  if (!ParseMajorMinor(versionText, &major, &minor)) {
    return false;
  }
  return major > 2 || (major == 2 && minor >= 25);
}

bool SupportsRestoreCommand(std::wstring_view versionText) {
  int major = 0;
  int minor = 0;
  if (!ParseMajorMinor(versionText, &major, &minor)) {
    return false;
  }
  return major > 2 || (major == 2 && minor >= 23);
}

bool AppendPathspecFileOptions(std::vector<std::wstring>* arguments, std::wstring_view pathspecFilePath) {
  if (arguments == nullptr || pathspecFilePath.empty()) {
    return false;
  }
  // 与执行器的边界校验同源：清单路径最终要作为参数原样交给 Git。
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

  const PathspecCollection collection = CollectSelectedPathspecs(selected, /*stagedSide=*/false);
  if (!collection.accepted) {
    plan.blockedReason = collection.refusalReason;
    return plan;
  }

  plan.pathspecEntries = collection.entries;
  plan.pathspecCount = collection.entries.size();
  plan.operationId = std::wstring(kOperationId);
  plan.displayName = L"加入暂存区 " + std::to_wstring(selected.size()) + L" 项";
  plan.selectionSummary = BuildSelectionSummary(selected);

  DeliveryShape shape;
  shape.subcommand = kAddSubcommand;
  shape.commandLabel = L"git add";
  shape.scopeText = BuildScopeText(L"本次只暂存选中的 ", selected.size(), plan.pathspecCount,
                                   options.totalUnstagedItems);
  shape.pathspecFileTail =
      L"。清单通过 NUL 分隔的临时文件传给 git add，每条路径都带 :(literal) 字面标记，"
      L"通配符、取反与前导减号都不会扩大范围；"
      L"命令窗口里可以看到实际执行的命令与 Git 的完整输出。";
  shape.inlineTail =
      L"。当前 Git 版本不支持路径清单文件接口（需 2.25 及以上），已把这 " +
      std::to_wstring(collection.entries.size()) +
      L" 条路径按字面 pathspec 直接写进命令；路径过多时本程序会拒绝执行，"
      L"不会改用 git add . 或暂存全部。";
  shape.tooManyTail =
      L"本次没有执行任何命令：不会退化成 git add . 或暂存全部，也不会自作主张分批。"
      L"请减少一次选择的条目数，或把 Git 升级到 2.25 以上以启用路径清单文件接口。";
  ApplyDelivery(plan, collection.entries, options, shape);
  plan.commandLabel = shape.commandLabel;
  if (plan.delivery == StagingDelivery::blocked) {
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

StagingPlan BuildStagingUnstagePlan(const std::vector<ChangeItem>& selected,
                                    const StagingPlanOptions& options) {
  StagingPlan plan;
  plan.selectedItems = selected.size();

  // 「取消全部暂存」在本程序里没有对应按钮：不带路径的 git reset 会连 HEAD 一起动，
  // 带 `.` 的 git restore --staged 会把整个索引退回上次提交。两者都不做，
  // 因此没有选中就原样返回，一句解释都不必让用户去猜。
  if (selected.empty()) {
    plan.blockedReason =
        L"没有选中任何条目。这个按钮只处理选中的已暂存变化，绝不会代为取消整个仓库的暂存"
        L"（不会执行不带路径的 git reset，也不会执行 git restore --staged . 或 git checkout）。"
        L"请在“已暂存的更改”里点选一行或多行后再点击。";
    return plan;
  }

  const PathspecCollection collection = CollectSelectedPathspecs(selected, /*stagedSide=*/true);
  if (!collection.accepted) {
    plan.blockedReason = collection.refusalReason;
    return plan;
  }

  plan.pathspecEntries = collection.entries;
  plan.pathspecCount = collection.entries.size();
  plan.operationId = std::wstring(kUnstageOperationId);
  plan.displayName = L"移出暂存区 " + std::to_wstring(selected.size()) + L" 项";
  plan.selectionSummary = BuildSelectionSummary(selected);

  // 两条命令都只写索引，选哪一条由两个条件决定：
  //   * 没有 HEAD 就没有「退回上一次提交」这回事：git restore --staged 会当场失败
  //     （实测 fatal: could not resolve 'HEAD'，退出码 128，索引未动）。
  //   * Git 太旧时 restore 这个子命令压根不存在（2.23 起才有），发出去只会得到
  //     "git: 'restore' is not a git command"。
  // 剩下的场合一律用带路径的 git reset —— 实测效果与 restore --staged 一致：
  // 所选路径的索引内容退回 HEAD（无提交时即从索引移除），磁盘文件原样保留，
  // 未选中路径的索引内容不受影响。
  const bool useRestore = options.repositoryHasHead && options.restoreCommandSupported;
  DeliveryShape shape;
  shape.scopeText = BuildScopeText(L"本次只移出暂存区选中的 ", selected.size(), plan.pathspecCount,
                                   options.totalStagedItems);
  const std::wstring sharedLiteralNote =
      L"清单通过 NUL 分隔的临时文件传给 Git，每条路径都带 :(literal) 字面标记，"
      L"通配符、取反与前导减号都不会扩大范围；"
      L"命令窗口里可以看到实际执行的命令与 Git 的完整输出。";
  if (useRestore) {
    shape.subcommand = kRestoreSubcommand;
    shape.commandLabel = L"git restore --staged";
    shape.subcommandFlags = {L"--staged"};
    shape.pathspecFileTail =
        L"。命令是只写索引的 git restore --staged：把所选路径的索引内容退回最近一次提交的版本，"
        L"绝不改写工作区里的任何文件，也不会新写出或删除磁盘上的文件。" + sharedLiteralNote;
    shape.inlineTail =
        L"。当前 Git 版本不支持路径清单文件接口（需 2.25 及以上），已把这 " +
        std::to_wstring(collection.entries.size()) +
        L" 条路径按字面 pathspec 直接写进命令；路径过多时本程序会拒绝执行，"
        L"不会退化成不带路径的 git restore --staged 或取消全部暂存。";
    shape.tooManyTail =
        L"本次没有执行任何命令：不会退化成不带路径的 git restore --staged 或取消全部暂存，"
        L"也不会自作主张分批。请减少一次选择的条目数，或把 Git 升级到 2.25 以上以启用路径清单文件接口。";
  } else {
    shape.subcommand = kResetSubcommand;
    shape.commandLabel = L"git reset -q";
    shape.subcommandFlags = {L"-q"};
    // 换成 git reset 的理由有两种，都必须说清楚：仓库根本没有可退回的 HEAD，
    // 或者这个 Git 太旧、连 restore 子命令都没有（2.23 起才有）。
    const std::wstring resetReason =
        options.repositoryHasHead
            ? L"当前 Git 版本没有 git restore 子命令（2.23 起才有）"
            : L"本仓库还没有任何提交，没有可退回的 HEAD 版本，git restore --staged 会直接失败"
              L"（本机实测）";
    shape.pathspecFileTail =
        L"。" + resetReason + L"，因此改用 git reset -q -- <所选路径> 这种只写索引的形态："
        L"它只把这些路径的索引条目退回最近一次提交的版本；最近一次提交里没有这个路径时"
        L"（仓库还没有任何提交就是这样），那条记录就从索引里移除，"
        L"磁盘上的文件原样保留、回到未跟踪状态。它不移动任何引用，也不丢弃任何未提交的改动。" +
        sharedLiteralNote +
        L"（注意：只有不带路径的 git reset 才会改动 HEAD，本程序绝不使用那种形态，"
        L"也绝不用 --hard、git checkout -- 或 git clean 这类会丢弃工作区的命令。）";
    shape.inlineTail =
        L"。当前 Git 版本不支持路径清单文件接口（需 2.25 及以上），已把这 " +
        std::to_wstring(collection.entries.size()) +
        L" 条路径按字面 pathspec 直接写进命令；路径过多时本程序会拒绝执行，"
        L"不会退化成不带路径的 git reset（那种形态才会改动 HEAD，本程序绝不使用）。";
    shape.tooManyTail =
        L"本次没有执行任何命令：不会退化成不带路径的 git reset 或取消全部暂存，也不会自作主张分批。"
        L"请减少一次选择的条目数，或把 Git 升级到 2.25 以上以启用路径清单文件接口。";
  }
  ApplyDelivery(plan, collection.entries, options, shape);
  plan.commandLabel = shape.commandLabel;
  if (plan.delivery == StagingDelivery::blocked) {
    return plan;
  }

  std::vector<std::wstring> confirmParts;
  const std::wstring indexOnlyNotice = BuildIndexOnlyContentNotice(selected);
  if (!indexOnlyNotice.empty()) {
    confirmParts.push_back(indexOnlyNotice);
  }
  const std::wstring submoduleNotice = BuildUnstageSubmoduleNotice(selected);
  if (!submoduleNotice.empty()) {
    confirmParts.push_back(submoduleNotice);
  }
  if (!confirmParts.empty()) {
    plan.confirmationText =
        JoinWith(confirmParts, L"\n\n") +
        L"\n\n是否仍在命令窗口里执行这条 " + std::wstring(shape.commandLabel) +
            L"？（取消不会改动仓库。）";
  }
  return plan;
}

}  // namespace gc::git
