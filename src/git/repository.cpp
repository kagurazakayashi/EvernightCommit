#include "git/repository.h"

#include <cctype>

namespace gc::git {

std::wstring TrimWide(std::wstring_view text) {
  size_t begin = 0;
  size_t end = text.size();
  const auto isBlank = [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
  };
  while (begin < end && isBlank(text[begin])) {
    ++begin;
  }
  while (end > begin && isBlank(text[end - 1])) {
    --end;
  }
  return std::wstring(text.substr(begin, end - begin));
}

namespace {

constexpr std::wstring_view kBranchRefPrefix = L"refs/heads/";
constexpr std::wstring_view kNoUpstreamText = L"未设置上游";
constexpr size_t kDetailSampleChars = 200;

wchar_t Fold(wchar_t c) noexcept {
  if (c >= L'A' && c <= L'Z') {
    return static_cast<wchar_t>(c - L'A' + L'a');
  }
  return c;
}

bool EqualsFolded(std::wstring_view left, std::wstring_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t index = 0; index < left.size(); ++index) {
    if (Fold(left[index]) != Fold(right[index])) {
      return false;
    }
  }
  return true;
}

bool ContainsFolded(std::wstring_view haystack, std::wstring_view needle) {
  if (needle.empty()) {
    return true;
  }
  if (haystack.size() < needle.size()) {
    return false;
  }
  for (size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
    if (EqualsFolded(haystack.substr(start, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

bool IsTrueFlag(std::wstring_view value) noexcept {
  return EqualsFolded(TrimWide(value), L"true");
}

bool IsBooleanFlag(std::wstring_view value) noexcept {
  const std::wstring trimmed = TrimWide(value);
  return EqualsFolded(trimmed, L"true") || EqualsFolded(trimmed, L"false");
}

std::wstring FirstLine(std::wstring_view text) {
  const size_t newline = text.find(L'\n');
  return TrimWide(newline == std::wstring_view::npos ? text : text.substr(0, newline));
}

std::wstring TrimmedSample(std::wstring_view text) {
  std::wstring trimmed = TrimWide(text);
  if (trimmed.size() > kDetailSampleChars) {
    trimmed.resize(kDetailSampleChars);
  }
  return trimmed;
}

}  // namespace

std::vector<std::wstring> SplitLines(std::wstring_view text) {
  std::vector<std::wstring> lines;
  if (text.empty()) {
    return lines;
  }
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t newline = text.find(L'\n', cursor);
    const size_t length = (newline == std::wstring_view::npos) ? text.size() - cursor : newline - cursor;
    std::wstring line;
    line.reserve(length);
    for (size_t index = 0; index < length; ++index) {
      if (text[cursor + index] != L'\r') {
        line.push_back(text[cursor + index]);
      }
    }
    lines.push_back(std::move(line));
    if (newline == std::wstring_view::npos) {
      break;
    }
    cursor = newline + 1;
  }
  // 结尾换行会多出一个空行，它不是字段。
  if (!lines.empty() && lines.back().empty() && !text.empty() && text.back() == L'\n') {
    lines.pop_back();
  }
  return lines;
}

bool ParseShapeFlags(const std::vector<std::wstring>& lines, RepoShape* out) {
  if (out == nullptr) {
    return false;
  }
  out->bare = false;
  out->insideWorkTree = false;
  out->insideGitDir = false;
  if (lines.size() < kShapeFlagCount) {
    return false;
  }
  // 三个标志必须是 true/false，否则说明取回来的不是形态查询的机器输出。
  if (!IsBooleanFlag(lines[kFlagBare]) || !IsBooleanFlag(lines[kFlagInsideWorkTree]) ||
      !IsBooleanFlag(lines[kFlagInsideGitDir])) {
    return false;
  }
  RepoShape shape;
  shape.bare = IsTrueFlag(lines[kFlagBare]);
  shape.insideWorkTree = IsTrueFlag(lines[kFlagInsideWorkTree]);
  shape.insideGitDir = IsTrueFlag(lines[kFlagInsideGitDir]);
  *out = std::move(shape);
  return true;
}

bool ParseShapePaths(const std::vector<std::wstring>& lines, RepoShape* out) {
  if (out == nullptr) {
    return false;
  }
  // 先清空再由成功路径写入，避免复用同一个 RepoShape 时留下上一次的字段。
  out->absoluteGitDir.clear();
  out->commonDir.clear();
  out->topLevel.clear();
  if (lines.size() < kPathCommonDir + 1) {
    return false;
  }
  // 裸仓库与 .git 内部只给出前两项后就以“must be run in a work tree”中止，topLevel 留空。
  out->absoluteGitDir = TrimWide(lines[kPathAbsoluteGitDir]);
  out->commonDir = TrimWide(lines[kPathCommonDir]);
  if (lines.size() > kPathTopLevel) {
    out->topLevel = TrimWide(lines[kPathTopLevel]);
  }
  return !out->absoluteGitDir.empty();
}

RepoHead ParseHeadOutput(std::wstring_view symbolicRef, std::wstring_view shortSha) {
  RepoHead head;
  const std::wstring ref = FirstLine(symbolicRef);
  const std::wstring sha = FirstLine(shortSha);
  head.hasCommits = !sha.empty();
  head.unborn = !head.hasCommits;

  if (ref.size() > kBranchRefPrefix.size() && ref.rfind(kBranchRefPrefix, 0) == 0) {
    head.onBranch = true;
    head.branchName = ref.substr(kBranchRefPrefix.size());
    return head;
  }
  head.shortSha = sha;
  return head;
}

RepoUpstream ParseUpstreamOutput(std::wstring_view line) {
  RepoUpstream upstream;
  const size_t tab = line.find(L'\t');
  const std::wstring remote = TrimWide(line.substr(0, tab));
  const std::wstring refName = (tab == std::wstring_view::npos) ? std::wstring() : TrimWide(line.substr(tab + 1));
  // 未设置上游时两个字段都是空值（Git 输出空行或只有制表符）。
  if (remote.empty() || refName.empty()) {
    return upstream;
  }
  std::wstring branchName = refName;
  if (branchName.size() > kBranchRefPrefix.size() && branchName.rfind(kBranchRefPrefix, 0) == 0) {
    branchName = branchName.substr(kBranchRefPrefix.size());
  }
  upstream.present = true;
  upstream.name = remote + L"/" + branchName;
  return upstream;
}

RepoError ClassifyGitFailure(const GitQueryResult& result, std::wstring& detail) {
  detail.clear();
  if (!result.started) {
    // 启动失败的具体原因（Win32 说明文字，平台层已限长）要一并带出，否则界面只能看到「无法启动」。
    detail = L"Git 程序未能启动";
    if (!result.launchDetail.empty()) {
      detail += L"：" + result.launchDetail;
    }
    return RepoError::gitLaunchFailed;
  }
  if (result.timedOut || !result.exited) {
    detail = L"Git 未在限定时间内返回";
    return RepoError::gitTimeout;
  }
  // 完整度排在退出码之前：半份 stdout 既不能当成功答复，也不能拿来归类错误文案。
  if (!result.outputComplete) {
    detail = result.incompleteReason.empty() ? L"Git 的标准输出没有完整读回" : result.incompleteReason;
    return RepoError::outputIncomplete;
  }
  if (result.exitCode == 0) {
    return RepoError::none;
  }

  // Git 的致命信息走 stderr、机器输出走 stdout，归类时两条都要看。
  std::wstring haystack = result.utf16Error;
  haystack += L'\n';
  haystack += result.utf16Output;
  if (ContainsFolded(haystack, L"not a git repository")) {
    detail = L"Git 报告该位置不是仓库，也不在任何仓库之内";
    return RepoError::notRepository;
  }
  if (ContainsFolded(haystack, L"dubious ownership")) {
    // 只转述原因：safe.directory 属于用户配置，本程序不代为修改。
    detail = L"仓库目录的所有者与当前用户不一致，需要由你自行确认归属后配置 safe.directory，"
             L"本程序不会改动你的 Git 配置";
    return RepoError::dubiousOwnership;
  }
  if (ContainsFolded(haystack, L"index.lock") &&
      (ContainsFolded(haystack, L"file exists") || ContainsFolded(haystack, L"already exists"))) {
    // 锁文件是 Git 自己的并发保护：另一个 Git 进程（可能是用户在外部终端里跑的，
    // 也可能是本程序的命令窗口操作）正持有它。这里只转述并等它结束，绝不删除锁文件——
    // 擅自删除正在被使用的 index.lock 会破坏别人尚未写完的索引。
    detail = L"仓库的锁文件（.git/index.lock）已存在，说明另一个 Git 进程正在改动这个仓库";
    return RepoError::indexLocked;
  }
  if (ContainsFolded(haystack, L"permission denied") || ContainsFolded(haystack, L"access is denied") ||
      ContainsFolded(haystack, L"unable to access")) {
    detail = L"Git 无法读取该目录";
    return RepoError::accessDenied;
  }
  if (ContainsFolded(haystack, L"cannot change to")) {
    detail = L"Git 无法进入该目录";
    return RepoError::inputNotDirectory;
  }
  detail = L"退出码 " + std::to_wstring(static_cast<unsigned long long>(result.exitCode));
  // 诊断片段优先取 stderr：致命信息本来就写在那里。
  std::wstring sample = TrimmedSample(result.utf16Error);
  if (sample.empty()) {
    sample = TrimmedSample(result.utf16Output);
  }
  if (!sample.empty()) {
    detail += L"：" + sample;
  }
  return RepoError::gitFailed;
}

bool NulRecordsAreComplete(std::wstring_view output, std::wstring& reason) {
  reason.clear();
  if (output.empty()) {
    return true;  // 空输出是 Git 的明确答复「一条也没有」（干净工作区、无匹配条目）。
  }
  if (output.back() == L'\0') {
    return true;
  }
  // 不带残缺内容的片段：那可能是半个路径或半条配置值，抄进界面就等于把可疑内容摊开。
  reason = L"最后一条记录缺少结束用的空字节（这份输出在记录中间就断了，共 " +
           std::to_wstring(output.size()) + L" 个字符）";
  return false;
}

std::wstring CanonicalPathKey(std::wstring_view path) {
  std::wstring key;
  key.reserve(path.size());
  for (const wchar_t c : path) {
    key.push_back(c == L'/' ? L'\\' : Fold(c));
  }
  // 保留盘符根 "c:\"：折叠成 "c:" 会让根目录与其下的相对段看起来同名。
  while (key.size() > 3 && key.back() == L'\\') {
    key.pop_back();
  }
  return key;
}

bool PathsEqualFolded(std::wstring_view left, std::wstring_view right) {
  return EqualsFolded(CanonicalPathKey(left), CanonicalPathKey(right));
}

bool PathIsWithin(std::wstring_view child, std::wstring_view parent) {
  const std::wstring childKey = CanonicalPathKey(child);
  const std::wstring parentKey = CanonicalPathKey(parent);
  if (childKey.empty() || parentKey.empty()) {
    return false;
  }
  if (childKey == parentKey) {
    return true;
  }
  if (childKey.size() <= parentKey.size()) {
    return false;
  }
  if (!EqualsFolded(std::wstring_view(childKey).substr(0, parentKey.size()), parentKey)) {
    return false;
  }
  // 必须以分隔符接续，避免 "d:\repo-backup" 被当成 "d:\repo" 的子目录。
  return childKey[parentKey.size()] == L'\\';
}

bool KindHasWorkspace(RepoKind kind) noexcept {
  switch (kind) {
    case RepoKind::plainWorktree:
    case RepoKind::linkedWorktree:
    case RepoKind::submodule:
    case RepoKind::detached:
    case RepoKind::noCommits:
      return true;
    default:
      return false;
  }
}

std::wstring RepoDetection::KindLabel() const {
  // 来历（链接工作树/子模块）与分支状态（游离/尚无提交）可以叠加，两者都要说清楚。
  std::wstring label(RepoKindLabel(kind));
  if (kind == RepoKind::detached || kind == RepoKind::noCommits) {
    if (submodule) {
      return label + L"（子模块）";
    }
    if (linked) {
      return label + L"（链接工作树）";
    }
    return label;
  }
  if (kind == RepoKind::submodule && linked) {
    return L"链接工作树（子模块仓库）";
  }
  return label;
}

std::wstring_view RepoKindLabel(RepoKind kind) noexcept {
  switch (kind) {
    case RepoKind::plainWorktree:
      return L"普通工作区";
    case RepoKind::linkedWorktree:
      return L"链接工作树";
    case RepoKind::submodule:
      return L"子模块仓库";
    case RepoKind::bare:
      return L"裸仓库";
    case RepoKind::insideGitDir:
      return L"位于 .git 目录内部";
    case RepoKind::notRepository:
      return L"不是 Git 仓库";
    case RepoKind::detached:
      return L"游离 HEAD";
    case RepoKind::noCommits:
      return L"尚无提交";
    case RepoKind::failed:
    case RepoKind::unknown:
      return L"未能识别";
  }
  return L"未能识别";
}

std::wstring_view RepoErrorLabel(RepoError error) noexcept {
  switch (error) {
    case RepoError::none:
      return L"无";
    case RepoError::inputEmpty:
      return L"未填写仓库路径";
    case RepoError::inputNotDirectory:
      return L"仓库路径不存在或不是目录";
    case RepoError::gitUnavailable:
      return L"Git 程序不可用";
    case RepoError::notRepository:
      return L"该位置不是 Git 仓库";
    case RepoError::dubiousOwnership:
      return L"被 Git 安全检查拦截";
    case RepoError::accessDenied:
      return L"权限不足";
    case RepoError::indexLocked:
      return L"仓库正被其他 Git 操作占用";
    case RepoError::gitTimeout:
      return L"Git 查询超时";
    case RepoError::gitLaunchFailed:
      return L"无法启动 Git";
    case RepoError::gitFailed:
      return L"Git 查询失败";
    case RepoError::badOutput:
      return L"Git 输出无法解析";
    case RepoError::outputIncomplete:
      return L"Git 输出读取不完整";
  }
  return L"识别失败";
}

std::wstring BuildRepoErrorDetail(RepoError error, std::wstring_view detail) {
  std::wstring text(RepoErrorLabel(error));
  if (!detail.empty()) {
    text += L"（" + std::wstring(detail) + L"）";
  }
  switch (error) {
    case RepoError::inputEmpty:
      text += L"。请在“本地仓库”中输入目录，或用“浏览…”选择。";
      break;
    case RepoError::inputNotDirectory:
      text += L"。本程序不会在所选目录里自动执行 git init，请确认路径拼写。";
      break;
    case RepoError::gitUnavailable:
      text += L"。请先在“Git 程序”里选择并验证有效的 git.exe。";
      break;
    case RepoError::notRepository:
      text += L"。请选择仓库目录本身或其子目录。";
      break;
    case RepoError::accessDenied:
      text += L"。请检查目录权限或占用情况后重试。";
      break;
    case RepoError::indexLocked:
      text += L"。请等待那个 Git 进程结束后再点“刷新”；本程序不会替你删除 Git 的锁文件。";
      break;
    case RepoError::badOutput:
      text += L"。请确认 Git 版本较新（建议 2.30 以上）后重试。";
      break;
    case RepoError::outputIncomplete:
      text += L"。请点“刷新”重新读取；本程序不会按残缺的输出判断仓库、配置或文件清单，"
              L"也不会把读不全当成「没有文件要处理」。";
      break;
    default:
      text += L"。";
      break;
  }
  return text;
}

std::wstring BuildRepoSummary(const RepoDetection& detection, std::wstring_view inputDirectory) {
  std::wstring text = L"已识别仓库：" + detection.KindLabel();
  // 裸仓库与 .git 内部没有工作区，只报 Git 目录，避免界面出现一个不能用的“工作区根目录”。
  if (!detection.root.empty()) {
    text += L"；工作区根目录：" + detection.root;
    if (!PathsEqualFolded(detection.root, inputDirectory)) {
      text += L"（已从所选子目录上溯）";
    }
  } else if (!detection.absoluteGitDir.empty()) {
    text += L"；Git 目录：" + detection.absoluteGitDir;
  }
  switch (detection.kind) {
    case RepoKind::bare:
      text += L"。裸仓库没有工作区，不能在这里暂存或创建提交；请打开它的工作区或链接工作树。";
      break;
    case RepoKind::insideGitDir:
      text += L"。该位置在 .git 目录内部，请改选仓库工作区目录。";
      break;
    case RepoKind::submodule:
      text += L"。这是子模块自身的仓库";
      if (!detection.superprojectTree.empty()) {
        text += L"，父仓库工作区：" + detection.superprojectTree;
      }
      text += L"；在这里提交只作用于子模块，父仓库会把它显示为子模块变化。";
      break;
    case RepoKind::linkedWorktree:
      text += L"。链接工作树与普通工作区一样可以暂存与提交。";
      break;
    case RepoKind::noCommits:
      text += L"。仓库还没有任何提交，提交历史与撤回提交要等首次提交后才可用。";
      break;
    case RepoKind::detached:
      text += L"。当前是游离 HEAD，撤回提交等分支操作没有可依附的分支，请谨慎。";
      break;
    default:
      break;
  }
  return text;
}

std::wstring FormatBranchDisplay(const RepoDetection& detection) {
  if (!KindHasWorkspace(detection.kind)) {
    return {};
  }
  if (detection.kind == RepoKind::noCommits) {
    return detection.branch.empty() ? L"尚无提交" : L"尚无提交（分支 " + detection.branch + L"）";
  }
  if (detection.kind == RepoKind::detached) {
    return detection.shortSha.empty() ? L"游离 HEAD" : L"游离 HEAD（" + detection.shortSha + L"）";
  }
  return detection.branch;
}

std::wstring FormatUpstreamDisplay(const RepoDetection& detection) {
  if (!KindHasWorkspace(detection.kind)) {
    return {};
  }
  if (detection.kind == RepoKind::detached) {
    return L"—（游离 HEAD 无分支上游）";
  }
  if (!detection.upstream.empty()) {
    return detection.upstream;
  }
  if (detection.kind == RepoKind::noCommits) {
    return std::wstring(kNoUpstreamText) + L"（尚无提交）";
  }
  return std::wstring(kNoUpstreamText);
}

}  // namespace gc::git
