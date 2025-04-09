#include "git/diff_view.h"

#include <algorithm>

namespace gc::git {
namespace {

// 所有查看命令共用的全局選項：
//   --no-optional-locks  —— 不讓 Git 順手做需要上鎖的額外工作（與內部只讀查詢同一套）；
//   --no-replace-objects —— 不套用 replace 引用，看到的物件與工作區列表的判定基準一致；
//   -c core.quotepath=off —— 中文檔名在窗口裡直接可讀。默認取值下 Git 會把非 ASCII 路徑
//     印成 "\346\226\207\344\273\266" 這樣的八進位轉義，使用者根本認不出那是列表裡那個檔案。
//     這隻改輸出的顯示形態，不影響哪個檔案被匹配、也不影響比對結果。
constexpr std::wstring_view kGlobalPrefix[] = {L"--no-optional-locks", L"--no-replace-objects", L"-c",
                                               L"core.quotepath=off"};

// 差異一律由 Git 自己生成：不调用使用者配置的外部 diff（--no-ext-diff）
// 與 textconv（--no-textconv），後者會在「比對二進位檔」時把任意轉換命令拉進來。
constexpr std::wstring_view kNativeDiffFlags[] = {L"--no-ext-diff", L"--no-textconv"};

// 子模組只按父倉庫的視角顯示提交指標：--submodule=short 覆蓋使用者的 diff.submodule，
// 否則使用者配了 diff.submodule=diff 時，父倉庫的一次查看會递归進子模組列出內部檔案，
// 與列表「父倉庫只有一條 gitlink 記錄」的說法互相矛盾。
constexpr std::wstring_view kSubmoduleFlags[] = {L"--ignore-submodules=none", L"--submodule=short"};

// 比較子命令名：兩側都是 `git diff`，差別在是否加 --cached。
constexpr std::wstring_view kDiffSubcommand = L"diff";
constexpr std::wstring_view kCachedFlag = L"--cached";
constexpr std::wstring_view kFindRenamesFlag = L"--find-renames";
constexpr std::wstring_view kNoIndexFlag = L"--no-index";
constexpr std::wstring_view kStatFlag = L"--stat";
constexpr std::wstring_view kOptionTerminator = L"--";

void AppendAll(std::vector<std::wstring>* out, const std::wstring_view* items, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    out->push_back(std::wstring(items[index]));
  }
}

// 重命名/複製與「舊路徑非空」的條目都要帶上兩個路徑：只給新路徑時，
// Git 把暫存的重命名顯示成「新增檔案」，使用者看到的與列表上的「重命名」不一致。
void AppendItemPathspecs(ChangeSide side, const ChangeItem& item, std::vector<std::wstring>* out) {
  out->push_back(MakeLiteralPathspec(item.path));
  const bool pairWithOldPath = !item.oldPath.empty() &&
                               (side == ChangeSide::staged || item.kind == ChangeKind::renamed ||
                                item.kind == ChangeKind::copied);
  if (pairWithOldPath) {
    // 舊路徑排在前面：`rename from/to` 的閱讀順序與 diff 輸出一致。
    out->insert(out->end() - 1, MakeLiteralPathspec(item.oldPath));
  }
}

// 未跟蹤檔案的預檢裁決。回傳 false 時 filled 帶 blockedReason。
bool DecideUntrackedView(const WorktreeFileFacts& facts, DiffViewPlan* plan) {
  const std::wstring sizeText = std::to_wstring(facts.sizeBytes) + L" 字节";
  if (!facts.probed) {
    plan->blockedReason =
        L"无法读取该文件的属性，因此没有打开命令窗口。" +
        (facts.failureReason.empty() ? std::wstring() : L"\n" + facts.failureReason);
    return false;
  }
  if (!facts.exists) {
    // 未跟蹤條目在外部被刪除是常見情況（手動刪檔、別的進程清理）：
    // 這時沒有「內容」可看，也絕不能讓 Git 去開一個不存在的路徑並把報錯留給使用者猜。
    plan->blockedReason = L"文件已不在工作区（可能在别处被删除或移动），没有可显示的内容。";
    return false;
  }
  if (facts.isDirectory) {
    plan->blockedReason =
        L"这是一个目录而不是文件。git status 已按 --untracked-files=all 展开到单个文件，"
        L"出现目录条目说明该条来历异常，请点“刷新”后重试。";
    return false;
  }
  if (!facts.readable) {
    plan->blockedReason =
        L"没有读取该文件的权限（或被其他程序独占占用），因此没有打开命令窗口。" +
        (facts.failureReason.empty() ? std::wstring() : L"\n" + facts.failureReason);
    return false;
  }
  if (facts.sizeBytes > kUntrackedSummaryLimitBytes) {
    plan->blockedReason = L"文件 " + sizeText + L"，超过 " +
                          std::to_wstring(kUntrackedSummaryLimitBytes / (1024ULL * 1024ULL)) +
                          L" MB 上限，连摘要都要自行计算差异，因此没有打开命令窗口。"
                          L"预览不改动仓库；如需完整内容请在仓库目录里自行执行 git diff --no-index。";
    return false;
  }
  return true;
}

std::wstring HumanPath(const ChangeItem& item) {
  return item.PathLabel();
}

}  // namespace

std::wstring_view DiffViewKindLabel(DiffViewKind kind) noexcept {
  switch (kind) {
    case DiffViewKind::trackedDiff:
      return L"差异";
    case DiffViewKind::untrackedContent:
      return L"内容";
    case DiffViewKind::untrackedSummary:
      return L"内容摘要";
    case DiffViewKind::blocked:
      return L"未打开";
  }
  return L"未打开";
}

std::wstring MakeLiteralPathspec(std::wstring_view relativePath) {
  if (relativePath.empty()) {
    return {};
  }
  // 已是 magic pathspec（`:(...)...`）或帶 `!` 取反前綴的輸入不再加前綴：
  // 我們的條目不會產生這種形態，這裡只防「重複包裝」。
  if (relativePath.front() == L':') {
    return std::wstring(relativePath);
  }
  return std::wstring(L":(literal)") + std::wstring(relativePath);
}

bool IsWorktreeRelativePath(std::wstring_view relativePath, std::wstring* reason) {
  const auto reject = [&](std::wstring_view text) {
    if (reason != nullptr) {
      *reason = std::wstring(text);
    }
    return false;
  };
  if (relativePath.empty()) {
    return reject(L"条目路径为空");
  }
  if (relativePath.front() == L'/' || relativePath.front() == L'\\') {
    return reject(L"条目路径以分隔符开头，不是仓库相对路径");
  }
  size_t segmentStart = 0;
  for (size_t index = 0; index <= relativePath.size(); ++index) {
    const wchar_t c = index < relativePath.size() ? relativePath[index] : L'/';
    if (c != L'/') {
      if (c == L'\\') {
        return reject(L"条目路径含反斜杠（Windows 分隔符），与 git status 的输出约定不符");
      }
      if (c == L':') {
        // 盤符（C:）與 NTFS 數據流（file.txt:stream）都要拒絕：
        // 兩者都能把「工作區內的一個名字」變成「指向別處的引用」。
        return reject(L"条目路径含冒号，可能被解释为盘符或数据流");
      }
      if (c < 0x20 || c == 0x7F) {
        return reject(L"条目路径含控制字符");
      }
      if (c == L'"') {
        return reject(L"条目路径含双引号");
      }
      continue;
    }
    const std::wstring_view segment = relativePath.substr(segmentStart, index - segmentStart);
    if (segment == L"..") {
      return reject(L"条目路径含 ..，会指向仓库之外");
    }
    segmentStart = index + 1;
  }
  return true;
}

std::wstring JoinWorktreeFilePath(std::wstring_view repositoryRoot, std::wstring_view relativePath) {
  std::wstring result(repositoryRoot);
  if (!result.empty() && result.back() != L'\\') {
    result.push_back(L'\\');
  }
  result.reserve(result.size() + relativePath.size());
  for (const wchar_t c : relativePath) {
    result.push_back(c == L'/' ? L'\\' : c);
  }
  return result;
}

DiffViewPlan BuildDiffViewPlan(ChangeSide side, const ChangeItem& item, const WorktreeFileFacts& facts) {
  DiffViewPlan plan;
  std::wstring pathProblem;
  if (!IsWorktreeRelativePath(item.path, &pathProblem)) {
    plan.kind = DiffViewKind::blocked;
    plan.blockedReason =
        L"这条记录的路径不符合仓库相对路径的约定，已拒绝在命令窗口里执行任何命令。\n原因：" + pathProblem +
        L"\n条目：" + HumanPath(item);
    return plan;
  }

  if (item.kind == ChangeKind::untracked) {
    plan.displayName = L"未跟踪文件 " + item.path;
    plan.operationId = L"preview-content";
    if (!DecideUntrackedView(facts, &plan)) {
      plan.kind = DiffViewKind::blocked;
      return plan;
    }
    // --no-index 的兩條路徑是檔案系統路徑（不是 pathspec，所以不套 :(literal)），
    // 第一條固定為 /dev/null：與「沒有基準版本」的語義一致，輸出裡顯示成 `--- /dev/null`。
    std::vector<std::wstring> arguments;
    AppendAll(&arguments, kGlobalPrefix, std::size(kGlobalPrefix));
    arguments.push_back(std::wstring(kDiffSubcommand));
    AppendAll(&arguments, kNativeDiffFlags, std::size(kNativeDiffFlags));
    arguments.push_back(std::wstring(kNoIndexFlag));
    const bool summaryOnly = facts.sizeBytes > kUntrackedContentPreviewLimitBytes;
    if (summaryOnly) {
      arguments.push_back(std::wstring(kStatFlag));
      plan.kind = DiffViewKind::untrackedSummary;
      plan.displayName = L"未跟踪文件摘要 " + item.path;
      plan.operationId = L"preview-summary";
      plan.notice = L"文件 " + std::to_wstring(facts.sizeBytes) + L" 字节，超过 " +
                    std::to_wstring(kUntrackedContentPreviewLimitBytes / 1024ULL) +
                    L" KB 的内容预览上限，窗口里给出的是 --stat 摘要（行数与规模），不是全文。";
    } else {
      plan.kind = DiffViewKind::untrackedContent;
      if (facts.containsNullByte) {
        // 二進位不做文字輸出：Git 自己會寫「Binary files ... differ」，
        // 窗口裡留下的是一句話說明，而不是一屏控制字元。
        plan.notice = L"该文件按二进制对待（前 " + std::to_wstring(kBinaryProbeBytes) +
                      L" 字节内出现 NUL），Git 只会给出“Binary files differ”的说明，不会输出内容。";
      }
    }
    arguments.push_back(std::wstring(kOptionTerminator));
    arguments.push_back(std::wstring(kEmptySidePath));
    arguments.push_back(item.path);
    plan.arguments = std::move(arguments);
    return plan;
  }

  // 已跟蹤條目（含刪除、衝突、子模組）：一律走 git diff 的兩側比較，
  // 刪除項由 Git 顯示成 `-` 側的刪除差異，界面不去打開已經不存在的路徑。
  plan.kind = DiffViewKind::trackedDiff;
  plan.operationId = side == ChangeSide::staged ? L"diff-index" : L"diff-worktree";
  plan.displayName = (side == ChangeSide::staged ? L"已暂存差异 " : L"工作区差异 ") + HumanPath(item);

  std::vector<std::wstring> arguments;
  AppendAll(&arguments, kGlobalPrefix, std::size(kGlobalPrefix));
  arguments.push_back(std::wstring(kDiffSubcommand));
  if (side == ChangeSide::staged) {
    arguments.push_back(std::wstring(kCachedFlag));
    // 只有索引側才可能記着重命名；显式要求重命名配對，讓窗口輸出與列表的「重命名」標識一致。
    if (item.kind == ChangeKind::renamed || item.kind == ChangeKind::copied) {
      arguments.push_back(std::wstring(kFindRenamesFlag));
    }
  }
  AppendAll(&arguments, kNativeDiffFlags, std::size(kNativeDiffFlags));
  AppendAll(&arguments, kSubmoduleFlags, std::size(kSubmoduleFlags));
  arguments.push_back(std::wstring(kOptionTerminator));
  AppendItemPathspecs(side, item, &arguments);
  plan.arguments = std::move(arguments);

  if (item.kind == ChangeKind::submodule) {
    std::wstring internal;
    if (item.submodule.commitChanged) {
      internal += L"提交指针已移动、";
    }
    if (item.submodule.trackedChanges) {
      internal += L"内部已跟踪文件有改动、";
    }
    if (item.submodule.untrackedChanges) {
      internal += L"内部有未跟踪文件、";
    }
    if (internal.empty()) {
      internal = L"仅记录提交指针";
    } else {
      internal.pop_back();
    }
    plan.notice = L"子模块 " + item.path + L"（" + internal +
                  L"）。窗口里显示的是父仓库记录的提交指针差异（--submodule=short，"
                  L"脏状态以 -dirty 后缀标出）；子模块内部的文件属于那个仓库自己的改动，"
                  L"要进入子模块工作区另行查看与暂存。";
  } else if (item.kind == ChangeKind::conflicted) {
    plan.notice = L"该条目未合并：窗口里是 Git 的组合差异（diff --cc），"
                  L"显示的是工作区文件中的冲突标记内容，不是某一侧的干净差异。";
  } else if (item.kind == ChangeKind::deleted) {
    plan.notice = L"该文件已删除：显示的是删除差异，程序不会去打开已经不存在的路径。";
  }
  return plan;
}

std::wstring DescribeDiffViewCommand(const DiffViewPlan& plan) {
  std::wstring text = std::wstring(DiffViewKindLabel(plan.kind)) + L"：git";
  for (const std::wstring& argument : plan.arguments) {
    text += L' ';
    text += argument;
  }
  return text;
}

std::wstring DescribeDiffViewExitCode(DiffViewKind kind, long exitCode) noexcept {
  switch (kind) {
    case DiffViewKind::trackedDiff:
      // 實測：git diff 找到差異時仍返回 0（只有 --exit-code/--quiet 才改成非 0）。
      return exitCode == 0
                 ? std::wstring(
                       L"（正常完成：git diff 有差异时也是退出码 0，输出为空表示这一侧确实没有差异）")
                 : std::wstring(L"（非 0，Git 报了错误，详细输出在命令窗口里查看）");
    case DiffViewKind::untrackedContent:
    case DiffViewKind::untrackedSummary:
      if (exitCode == 0) {
        return std::wstring(L"（正常完成：文件没有内容，与空的一侧相比没有差异）");
      }
      if (exitCode == 1) {
        return std::wstring(L"（正常完成：Git 用退出码 1 表示“存在差异”，不是失败）");
      }
      return std::wstring(L"（非 0 且不是 1，Git 报了错误，详细输出在命令窗口里查看）");
    case DiffViewKind::blocked:
      break;
  }
  return {};
}

}  // namespace gc::git
