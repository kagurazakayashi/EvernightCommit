#include "git/commit_changes.h"

#include <algorithm>

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId
#include "git/diff_view.h"       // MakeLiteralPathspec / IsWorktreeRelativePath
#include "git/repository.h"      // NulRecordsAreComplete

namespace gc::git {
namespace {

// 与 porcelain v2 解析同一套切分：记录以 NUL 结束，Git 每条记录的结尾 NUL 会多出一个空片段，
// 它不是一条记录，也不参与字段分组。
std::vector<std::wstring_view> SplitNulTokens(std::wstring_view text) {
  std::vector<std::wstring_view> tokens;
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t nul = text.find(L'\0', cursor);
    const size_t end = (nul == std::wstring_view::npos) ? text.size() : nul;
    tokens.push_back(text.substr(cursor, end - cursor));
    if (nul == std::wstring_view::npos) {
      break;
    }
    cursor = nul + 1;
  }
  if (!tokens.empty() && tokens.back().empty()) {
    tokens.pop_back();
  }
  return tokens;
}

// 片段的可读摘要：只用于说明「哪一条读不懂」，取前 40 个字符且不显示控制字符。
[[nodiscard]] std::wstring RecordSample(std::wstring_view record) {
  const size_t limit = std::min<size_t>(record.size(), 40);
  return std::wstring(record.substr(0, limit));
}

// name-status 的状态字母 → 条目类别。只认 Git 会为一条提交给出的字母：
// 未合并（U）不会出现在某个提交的树里，遇到其它字母说明输出形态变了，按整份作废处理。
bool KindForNameStatusLetter(wchar_t letter, ChangeKind* kind) {
  switch (letter) {
    case L'A':
      *kind = ChangeKind::added;
      return true;
    case L'M':
      *kind = ChangeKind::modified;
      return true;
    case L'D':
      *kind = ChangeKind::deleted;
      return true;
    case L'T':
      *kind = ChangeKind::typeChange;
      return true;
    case L'R':
      *kind = ChangeKind::renamed;
      return true;
    case L'C':
      *kind = ChangeKind::copied;
      return true;
    default:
      return false;
  }
}

bool IsRenameOrCopyLetter(wchar_t letter) noexcept { return letter == L'R' || letter == L'C'; }

}  // namespace

std::vector<std::wstring> BuildCommitChangesArguments(std::wstring_view repositoryDirectory,
                                                      std::wstring_view objectId) {
  return std::vector<std::wstring>{
      L"-C",
      std::wstring(repositoryDirectory),
      L"--no-optional-locks",
      L"--no-replace-objects",
      L"-c",
      L"core.quotepath=off",
      L"-c",
      L"log.showSignature=false",
      L"show",
      // --format= 关掉提交头与差异正文；--name-status 只列文件名与状态。
      L"--format=",
      L"--name-status",
      L"--find-renames",
      // 合并提交相对第一个父提交给出变化；对普通提交无副作用（见头文件说明）。
      L"--first-parent",
      L"--no-ext-diff",
      L"--no-textconv",
      L"--ignore-submodules=none",
      L"-z",
      std::wstring(objectId)};
}

CommitChangesParseResult ParseCommitChanges(std::wstring_view nulSeparatedOutput) {
  CommitChangesParseResult result;
  // 记录边界约定排在一切解析之前：缺结尾 NUL 的最后一条可能是半个文件名。
  std::wstring reason;
  if (!NulRecordsAreComplete(nulSeparatedOutput, reason)) {
    result.error = L"提交文件变化输出的最后一条记录不完整：" + reason;
    return result;
  }

  const std::vector<std::wstring_view> tokens = SplitNulTokens(nulSeparatedOutput);
  size_t index = 0;
  while (index < tokens.size()) {
    const std::wstring_view status = tokens[index];
    if (status.empty()) {
      result.error = L"提交文件变化输出中出现空的状态字段";
      result.changes.clear();
      return result;
    }
    ChangeKind kind = ChangeKind::unknown;
    if (!KindForNameStatusLetter(status[0], &kind)) {
      result.error = L"提交文件变化记录的状态字母不合约定：" + RecordSample(status);
      result.changes.clear();
      return result;
    }

    ChangeItem item;
    item.kind = kind;
    item.statusCode = std::wstring(status);
    if (IsRenameOrCopyLetter(status[0])) {
      // 实测形态：`R100\0old\0new\0`——相似度与字母同段，旧、新各占一条路径字段。
      if (index + 2 >= tokens.size()) {
        result.error = L"重命名/复制记录缺少旧路径或新路径：" + RecordSample(status);
        result.changes.clear();
        return result;
      }
      const std::wstring_view oldPath = tokens[index + 1];
      const std::wstring_view newPath = tokens[index + 2];
      if (oldPath.empty() || newPath.empty()) {
        result.error = L"重命名/复制记录的路径为空：" + RecordSample(status);
        result.changes.clear();
        return result;
      }
      item.oldPath = std::wstring(oldPath);
      item.path = std::wstring(newPath);
      item.similarity = std::wstring(status);
      index += 3;
    } else {
      if (index + 1 >= tokens.size()) {
        result.error = L"文件变化记录缺少路径：" + RecordSample(status);
        result.changes.clear();
        return result;
      }
      const std::wstring_view path = tokens[index + 1];
      if (path.empty()) {
        result.error = L"文件变化记录的路径为空：" + RecordSample(status);
        result.changes.clear();
        return result;
      }
      item.path = std::wstring(path);
      index += 2;
    }
    result.changes.push_back(std::move(item));
  }
  return result;
}

CommitFileDiffPlan BuildCommitFileDiffPlan(const CommitItem& commit, const ChangeItem& change) {
  CommitFileDiffPlan plan;
  plan.operationId = L"commit-file-show";
  if (!LooksLikeFullObjectId(commit.objectId)) {
    plan.refusalReason =
        L"这条提交记录的对象 ID 不符合约定（只允许小写十六进制的完整 ID），"
        L"为避免向 git show 交出不可信的参数，本次没有打开命令窗口，也没有对仓库做任何改动。\n"
        L"请点“刷新”重新读取提交历史。";
    return plan;
  }

  // 路径必须逐条复核：那是在工作区内的仓库相对路径，界面到执行器之间隔着好几层，
  // 最后关头仍要确认送出去的不是空串、绝对路径、盘符、NTFS 数据流或含 `..` 的逃逸路径。
  std::wstring pathReason;
  if (!IsWorktreeRelativePath(change.path, &pathReason)) {
    plan.refusalReason = L"这条文件记录的路径不符合约定（" + pathReason +
                         L"），为避免把不可信路径交给 git show，本次没有打开命令窗口，"
                         L"也没有对仓库做任何改动。请点“刷新”后重试。";
    return plan;
  }
  const bool hasOldPath = !change.oldPath.empty();
  if (hasOldPath) {
    std::wstring oldReason;
    if (!IsWorktreeRelativePath(change.oldPath, &oldReason)) {
      plan.refusalReason = L"这条重命名/复制记录的来源路径不符合约定（" + oldReason +
                           L"），为避免把不可信路径交给 git show，本次没有打开命令窗口，"
                           L"也没有对仓库做任何改动。请点“刷新”后重试。";
      return plan;
    }
  }

  plan.allowed = true;
  plan.displayName = L"查看提交文件 " + ShortObjectId(commit.objectId);
  // 与「查看提交详情」同一套规则：不调用外部 diff 与 textconv，子模块只给提交指针。
  // 对象 ID 放在 `--` 之前（它是修订，不是路径），文件名清单放在 `--` 之后；
  // pathspec 用 :(literal) 前缀关闭通配，含 [ ] 的合法档名因此不会连带到别的文件。
  plan.arguments = {L"--no-optional-locks",
                    L"--no-replace-objects",
                    L"-c",
                    L"core.quotepath=off",
                    L"-c",
                    L"log.showSignature=false",
                    L"show",
                    L"--format=fuller",
                    L"--first-parent",
                    L"--no-ext-diff",
                    L"--no-textconv",
                    L"--ignore-submodules=none",
                    L"--submodule=short",
                    commit.objectId,
                    L"--",
                    MakeLiteralPathspec(change.path)};
  if (hasOldPath) {
    // 重命名/复制要同时限定旧、新两条路径，Git 才会把它显示成 rename from/to 而不是「新增文件」。
    plan.arguments.push_back(MakeLiteralPathspec(change.oldPath));
  }
  if (commit.isMergeCommit) {
    plan.notice = L"这是合并提交里的一条文件变化：显示的是相对第一个父提交的差异。";
  }
  return plan;
}

}  // namespace gc::git
