#include "git/submodule_navigation.h"

#include <string>
#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId：位置一律按完整对象 ID 比，不按短 ID 猜
#include "git/diff_view.h"       // IsWorktreeRelativePath / MakeLiteralPathspec：路径约定只有一份
#include "git/pull_plan.h"       // 子模块自己 HEAD 的那条查询与 pull/push 预检同一形态

namespace gc::git {
namespace {

constexpr std::wstring_view kGitlinkMode = L"160000";

bool HasPrefix(std::wstring_view text, std::wstring_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// NUL 分隔的记录逐条取出（结尾 NUL 已由 NulRecordsAreComplete 核过，这里只管拆）。
std::vector<std::wstring> SplitNulRecords(std::wstring_view text) {
  std::vector<std::wstring> records;
  std::wstring current;
  for (const wchar_t c : text) {
    if (c == L'\0') {
      if (!current.empty()) {
        records.push_back(current);
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  return records;
}

// `<mode> <oid> <stage>\t<路径>`：三段空格分隔 + 一个制表符 + 路径原文。
// 形态不符就是「这份回答不能采认」，不是「没有记录」——半条记录看起来也像一条记录。
bool ParseIndexRecord(std::wstring_view record, std::wstring* mode, std::wstring* objectId,
                      std::wstring* path) {
  const size_t tab = record.find(L'\t');
  if (tab == std::wstring_view::npos) {
    return false;
  }
  const std::wstring_view head = record.substr(0, tab);
  const size_t first = head.find(L' ');
  const size_t second = first == std::wstring_view::npos ? std::wstring_view::npos
                                                         : head.find(L' ', first + 1);
  if (first == std::wstring_view::npos || second == std::wstring_view::npos) {
    return false;
  }
  *mode = std::wstring(head.substr(0, first));
  *objectId = std::wstring(head.substr(first + 1, second - first - 1));
  *path = std::wstring(record.substr(tab + 1));
  return !mode->empty() && !path->empty();
}

// 短 ID 展示：位置一律以完整对象 ID 比对，这里只是把话说短。空就是「没问出来」，不写 0。
std::wstring ShortOrEmpty(const std::wstring& objectId) {
  return objectId.empty() ? std::wstring(L"（没问出来）") : ShortObjectId(objectId);
}

std::wstring PresenceProblem(const SubmoduleEntryFacts& facts) {
  switch (facts.presence) {
    case SubmodulePathPresence::missing:
      if (facts.index.isGitlink) {
        return L"父索引里这条子模块记录还在（" + facts.relativePath + L"，记着 " +
               ShortOrEmpty(facts.index.objectId) +
               L"），可那个目录已经不在了——被移走、删掉，或在别的机器上没检出过。"
               L"本程序不去找回它，也不会替你 clone 或 git submodule update --init（那要联网、"
               L"会改动工作区，不属于「进入子模块」这一步）。请先由你自己把那份工作区放回去，"
               L"再点一次「刷新」。";
      }
      return L"这个路径既不在文件系统上，也不在父仓库索引里作为 gitlink 存在：" +
             facts.relativePath + L"。界面看到的这一条与仓库里的事实已经不是一件事了，"
             L"所以没有进入任何仓库。请点“刷新”看清现状。";
    case SubmodulePathPresence::existsNotDirectory:
      return L"「" + facts.relativePath +
             L"」这个名字上现在是一个文件（或别的非目录对象），不是子模块工作区目录。"
             L"本程序不把它当仓库，也不改名、不删除任何东西。";
    case SubmodulePathPresence::unknown:
    default:
      return L"没能确认那个目录在不在（只读探测没有做成）。看不清落点就不换绑定的仓库："
             L"点“刷新”后重试。";
  }
}

std::wstring ChildIdentityProblem(const SubmoduleEntryFacts& facts) {
  const RepoDetection& child = facts.child;
  const std::wstring shownRoot =
      child.root.empty() ? std::wstring(L"（Git 没有答出工作区根目录）") : child.root;
  // Git 沿目录往上找仓库：子模块自己没有 .git 时会答成「父仓库本身」。这一条必须单独说清，
  // 否则「未初始化」会被显示成一句含糊的「这不是仓库」，而用户下一步该做的完全不同。
  if (!child.root.empty() && PathsEqualFolded(child.root, facts.parentRoot)) {
    return L"这个目录自己不是仓库：Git 从它往上找到的工作区根就是当前这个父仓库（" +
           facts.parentRoot + L"）。这正是子模块**还没有初始化**的样子——父索引里只有那条 gitlink，"
           L"目录里没有它自己的 .git。"
           L"初始化会联网并改动工作区，本程序不在点「进入子模块」时替你做：需要的话请自己在终端里跑 "
           L"git submodule update --init -- " +
           facts.relativePath + L"（或你指定的别的形态），再回来点“刷新”。";
  }
  switch (child.kind) {
    case RepoKind::notRepository:
      return L"这个目录存在，但 Git 明确回答它不在任何仓库里：" + facts.resolvedDirectory +
             L"。父索引里那条 gitlink 记的是 " + ShortOrEmpty(facts.index.objectId) +
             L"，而目录里没有仓库身份——也就是子模块还没有初始化（或被换成了一个普通目录）。"
             L"初始化会联网并改动工作区，本程序不在导航里替你做。";
    case RepoKind::bare:
    case RepoKind::insideGitDir:
      return L"这个目录的形态不能当工作区用：Git 说它是「" + std::wstring(RepoKindLabel(child.kind)) +
             L"」（" + shownRoot + L"）。没有可操作的工作区就没有可绑定的仓库，本程序不进入。";
    case RepoKind::plainWorktree:
    case RepoKind::linkedWorktree:
      return L"这个目录是一个仓库的工作区，但 Git 说它**不是由父仓库登记的子模块**"
             L"（--show-superproject-working-tree 没有答出父仓库）。当前父仓库是 " + facts.parentRoot +
             L"，而这个目录是 " + shownRoot +
             L"。它可能是被人另行 clone 的独立仓库、或原来那个子模块已被换掉——"
             L"来历对不上就不进去，本程序不「看着像就算」。";
    case RepoKind::submodule:
      return L"Git 说这个目录是子模块，可它的父仓库是 " +
             (child.superprojectTree.empty() ? std::wstring(L"（没答出来）") : child.superprojectTree) +
             L"，而界面现在绑定的是 " + facts.parentRoot +
             L"。两个不是同一个父仓库（目录被移动过、或这是另一份克隆里的同名子模块）。"
             L"本程序不换绑一个来历不同的仓库。";
    case RepoKind::detached:
    case RepoKind::noCommits:
    case RepoKind::unknown:
    case RepoKind::failed:
    default:
      return L"这个目录的仓库身份没能问出来：" +
             (child.message.empty() ? std::wstring(L"仓库识别没有给出可采认的回答") : child.message) +
             L"。看不清它是谁、属于哪个父仓库，就不换绑定的仓库。";
  }
}

}  // namespace

// ---- 只读查询的参数 ----

std::vector<std::wstring> BuildSubmoduleIndexPointerArguments(std::wstring_view repositoryRoot,
                                                              std::wstring_view relativePath) {
  if (repositoryRoot.empty()) {
    return {};
  }
  std::wstring problem;
  if (!IsWorktreeRelativePath(relativePath, &problem)) {
    return {};  // 路径不合仓库相对路径的约定：宁可不问，也不带着一个会被解释成别的东西的路径去问。
  }
  // -z 是必须的：不带它时 Git 会把含空格或非 ASCII 的路径按 C 引号转写，那种回答不是原样路径。
  // -- 之后才放 pathspec：路径本身可能以 - 开头，隔开之后它只剩位置这一种含义。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryRoot), L"--no-optional-locks",
                                   L"ls-files", L"-s", L"-z", L"--", MakeLiteralPathspec(relativePath)};
}

std::vector<std::wstring> BuildSubmoduleHeadPointerArguments(std::wstring_view repositoryRoot,
                                                             std::wstring_view relativePath) {
  if (repositoryRoot.empty()) {
    return {};
  }
  std::wstring problem;
  if (!IsWorktreeRelativePath(relativePath, &problem)) {
    return {};
  }
  // HEAD:<相对路径> 由 Git 自己解析；对 gitlink 而言它答的就是父提交记着的那一份提交 ID。
  // --quiet：没有这个提交（仓库还没有提交）或这个路径不在 HEAD 树里时，退出码 1 + 空输出，
  // 那是明确答案而不是错误。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryRoot), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   std::wstring(L"HEAD:") + std::wstring(relativePath)};
}

std::vector<std::wstring> BuildSubmoduleOwnHeadArguments(std::wstring_view submoduleDirectory) {
  // 与 pull/push 预检问 HEAD 的那一条同一个实现，只是工作目录换成子模块自己的目录。
  if (submoduleDirectory.empty()) {
    return {};
  }
  return BuildPullHeadObjectArguments(submoduleDirectory);
}

// ---- 父索引记录的判读 ----

GitlinkIndexEntry ParseGitlinkIndexEntry(const GitQueryResult& listing,
                                        std::wstring_view expectedRelativePath) {
  GitlinkIndexEntry entry;
  const UndoQueryRead read = ReadUndoQuery(listing);
  if (read.outcome == UndoQueryOutcome::noResult) {
    // ls-files 对「没有这个路径」的答案是退出码 0 + 空输出；退出码 1 加空输出在这个命令上
    // 不是「正常没有」，而是问不成（例如 pathspec 语法被拒）。因此这里按问不成处理。
    entry.readFailure = L"git ls-files 没有正常回答这条查询（退出码 1）。";
    return entry;
  }
  if (read.outcome != UndoQueryOutcome::answered) {
    entry.readFailure = read.detail.empty() ? std::wstring(L"git ls-files -s 未成功") : read.detail;
    return entry;
  }
  std::wstring completenessReason;
  if (!listing.utf16Output.empty() && !NulRecordsAreComplete(listing.utf16Output, completenessReason)) {
    // 半条记录看起来也像一条记录：半个对象 ID 会被当成「父索引记的就是这一份」。
    entry.readFailure = L"父索引记录不符合 NUL 分隔的约定：" + completenessReason;
    return entry;
  }
  entry.readOk = true;
  if (listing.utf16Output.empty()) {
    return entry;  // 空输出是明确答案：这个路径在索引里没有记录。
  }
  for (const std::wstring& record : SplitNulRecords(listing.utf16Output)) {
    std::wstring mode;
    std::wstring objectId;
    std::wstring path;
    if (!ParseIndexRecord(record, &mode, &objectId, &path)) {
      entry.readOk = false;
      entry.readFailure = L"父索引里有一条记录的形态不像 `<mode> <对象 ID> <阶段>\\t<路径>`，"
                          L"本程序不猜它想说什么。";
      entry.recordFound = false;
      return entry;
    }
    if (path != std::wstring(expectedRelativePath)) {
      // 字面 pathspec 之下 Git 只该答这一条；答出别的路径说明这份回答不是我们要问的那一条，
      // 采认它就是把另一个路径的位置当成这个子模块的。
      continue;
    }
    entry.recordFound = true;
    entry.mode = mode;
    entry.recordedPath = path;
    if (mode != kGitlinkMode) {
      entry.isGitlink = false;
      return entry;
    }
    if (!LooksLikeFullObjectId(objectId)) {
      entry.readOk = false;
      entry.isGitlink = false;
      entry.recordFound = false;
      entry.readFailure =
          L"父索引里那条 160000 记录给出的不是完整对象 ID（那一条是：" + mode + L" " + objectId +
          L"），本程序不在看不清位置的前提下判断该暂存什么。";
      return entry;
    }
    entry.isGitlink = true;
    entry.objectId = objectId;
    return entry;
  }
  return entry;  // 问成功、回答里没有我们要问的那一条：索引里没有这个路径的记录。
}

// ---- 三份位置的合成 ----

std::wstring_view SubmodulePointerStateLabel(SubmodulePointerState state) noexcept {
  switch (state) {
    case SubmodulePointerState::consistentClean:
      return L"一致（父仓库这一条显示干净）";
    case SubmodulePointerState::pointerStaged:
      return L"指针已暂存（等父仓库创建提交）";
    case SubmodulePointerState::pointerUnstaged:
      return L"指针未暂存（子模块已经走到别的位置）";
    case SubmodulePointerState::submoduleHeadUnknown:
      return L"问不出子模块现在在哪一份提交";
    case SubmodulePointerState::notGitlinkInIndex:
      return L"这个路径在父索引里不是 gitlink 记录";
    case SubmodulePointerState::unknown:
    default:
      return L"位置没能问全";
  }
}

namespace {

// 一条 `rev-parse --verify --quiet` 回答里的对象 ID；noResult 是「明确没有」，不是错误。
bool ReadObjectId(const GitQueryResult& result, bool* queried, std::wstring* objectId,
                  std::wstring* failure) {
  const UndoQueryRead read = ReadUndoQuery(result);
  if (read.outcome == UndoQueryOutcome::noResult) {
    *queried = true;
    return true;  // 明确没有：对象 ID 留空，调用方按「没有这一份」说话。
  }
  if (read.outcome != UndoQueryOutcome::answered) {
    *queried = false;
    if (failure != nullptr && !read.detail.empty()) {
      *failure = read.detail;
    }
    return false;
  }
  *queried = true;
  if (!LooksLikeFullObjectId(read.firstLine)) {
    if (failure != nullptr) {
      *failure = L"Git 答出的不是完整对象 ID（那一行是：" + read.firstLine + L"），不拿它算差异。";
    }
    return false;
  }
  *objectId = read.firstLine;
  return true;
}

}  // namespace

SubmodulePointerFacts InterpretSubmodulePointer(const SubmodulePointerQueries& queries) {
  SubmodulePointerFacts facts;
  facts.index = ParseGitlinkIndexEntry(queries.indexListing, queries.relativePath);
  if (!facts.index.readOk) {
    facts.state = SubmodulePointerState::unknown;
    facts.detail = facts.index.readFailure;
    return facts;
  }
  if (!facts.index.recordFound) {
    facts.state = SubmodulePointerState::notGitlinkInIndex;
    facts.detail = L"父仓库的索引里没有 " + queries.relativePath +
                   L" 这条记录（它可能已被移出暂存区、改名或删掉）。"
                   L"没有记录就无从谈「要暂存哪一份指针」，本程序不猜。";
    return facts;
  }
  if (!facts.index.isGitlink) {
    facts.state = SubmodulePointerState::notGitlinkInIndex;
    facts.detail = L"父仓库索引里 " + queries.relativePath + L" 那条记录的 mode 是 " +
                   facts.index.mode + L"，不是 160000 gitlink。界面此刻把它当子模块，"
                   L"而索引说的已经不是同一件事了：请先点“刷新”看清现状。";
    return facts;
  }

  std::wstring failure;
  bool headQueried = false;
  if (!ReadObjectId(queries.headPointer, &headQueried, &facts.headObjectId, &failure)) {
    facts.state = SubmodulePointerState::unknown;
    facts.detail = failure;
    return facts;
  }
  facts.headQueried = headQueried;

  bool submoduleQueried = false;
  if (!ReadObjectId(queries.submoduleHead, &submoduleQueried, &facts.submoduleHeadId, &failure)) {
    facts.state = SubmodulePointerState::unknown;
    facts.detail = failure;
    return facts;
  }
  facts.submoduleHeadQueried = submoduleQueried;
  if (!submoduleQueried || facts.submoduleHeadId.empty()) {
    facts.state = SubmodulePointerState::submoduleHeadUnknown;
    facts.detail = facts.submoduleHeadId.empty()
                       ? std::wstring(L"子模块那边问不出 HEAD 是哪一份提交（还没有提交、"
                                       L"引用坏了，或那已经不是本程序识别过的那个工作区）。"
                                       L"本程序不猜它的位置，也不说父仓库这一条「干净」。")
                       : std::wstring(L"子模块那边的 HEAD 查询没能完成，位置不知道。");
    return facts;
  }

  if (facts.submoduleHeadId != facts.index.objectId) {
    facts.state = SubmodulePointerState::pointerUnstaged;
    return facts;
  }
  // 索引与子模块一致：父提交记的是不是同一份决定「干净」还是「已暂存待提交」。
  // 父提交里根本没有这条记录（新加的 gitlink）也算已暂存。
  if (facts.headObjectId.empty() || facts.headObjectId == facts.index.objectId) {
    facts.state = facts.headObjectId.empty() && !headQueried
                      ? SubmodulePointerState::unknown
                      : SubmodulePointerState::consistentClean;
    if (facts.headObjectId.empty()) {
      facts.state = SubmodulePointerState::pointerStaged;
      facts.detail = L"父仓库的 HEAD 树里还没有这条 gitlink 记录（新加的子模块，或还没有任何提交）。";
    }
    return facts;
  }
  facts.state = SubmodulePointerState::pointerStaged;
  return facts;
}

std::wstring ComposeSubmodulePointerReport(const SubmodulePointerFacts& facts,
                                          std::wstring_view submodulePath) {
  std::wstring text = L"位置核对（" + std::wstring(submodulePath) + L"）：" +
                      std::wstring(SubmodulePointerStateLabel(facts.state)) + L"\n";
  text += L" · 父索引记的那一份：" + ShortOrEmpty(facts.index.objectId) +
          (facts.index.isGitlink ? std::wstring() : std::wstring(L"（这条不是 gitlink 记录）")) +
          L"\n";
  text += L" · 父提交记的那一份：" + ShortOrEmpty(facts.headObjectId) + L"\n";
  text += L" · 子模块现在在：" + ShortOrEmpty(facts.submoduleHeadId) + L"\n";
  if (!facts.detail.empty()) {
    text += L" · " + facts.detail + L"\n";
  }

  switch (facts.state) {
    case SubmodulePointerState::pointerUnstaged:
      text += L"\n下一步该由你做：在父仓库的“未暂存的更改”里选中这一条，点“加入暂存区”，"
              L"再在父仓库创建提交。本程序不会替你在导航里 add／commit／push，"
              L"也不会递归处理别的子模块。\n";
      break;
    case SubmodulePointerState::pointerStaged:
      text += L"\n指针已经在父仓库索引里了：下一步是在父仓库“创建提交”。"
              L"本程序不会替你提交，也不会自动推送。\n";
      break;
    case SubmodulePointerState::consistentClean:
      text += L"\n父仓库这一条现在显示干净：索引、父提交与子模块的 HEAD 是同一份。\n";
      break;
    case SubmodulePointerState::submoduleHeadUnknown:
    case SubmodulePointerState::notGitlinkInIndex:
    case SubmodulePointerState::unknown:
    default:
      text += L"\n位置没问全之前，本程序不说「该暂存」也不说「已经干净」。点“刷新”后再看一次。\n";
      break;
  }

  text += L"子模块内部有没有已跟踪改动或未跟踪文件，看的是父仓库列表里这一条自己的状态；"
          L"要处理它们得再点“进入子模块”，在里面的列表里暂存与提交——那些归子模块自己的仓库，"
          L"父仓库这一头只有指针。\n";
  if (facts.state == SubmodulePointerState::consistentClean) {
    text += L"提醒：父仓库显示干净，只说明这三处记的是同一份提交，"
            L"**不证明**子模块那次提交已经推到任何远端、更不证明别人已经收到。"
            L"要确认发布状态得在子模块里向它的远端核实。\n";
  }
  text += L"本次导航本身：没有改动父仓库索引、没有产生提交、没有访问远端。";
  return text;
}

// ---- 进入子模块的裁决 ----

SubmoduleEntryPlan BuildSubmoduleEntryPlan(const SubmoduleEntryFacts& facts) {
  SubmoduleEntryPlan plan;
  const auto refuse = [&plan, &facts](std::wstring reason) {
    plan.decision = SubmoduleEntryDecision::refuse;
    plan.reason = std::move(reason);
    if (!facts.relativePath.empty()) {
      plan.reason += L"\n\n条目：" + facts.relativePath;
    }
    return plan;
  };

  if (!facts.itemIsSubmodule) {
    return refuse(L"选中的这一条不是子模块条目（父仓库对它是按文件或其他记录处理的）。"
                  L"“进入子模块”只对状态里标成「子模块」的那一条有效，本程序没有换绑任何仓库。");
  }
  if (facts.relativePath.empty()) {
    return refuse(L"这条子模块记录没有可用的仓库相对路径，本程序不猜要进哪个目录。");
  }
  if (!facts.pathShapeOk) {
    return refuse(L"这条记录的路径不符合仓库相对路径的约定，已拒绝进入任何仓库。\n原因：" +
                  facts.pathShapeProblem);
  }
  if (facts.resolvedDirectory.empty()) {
    return refuse(L"没能把「" + facts.relativePath +
                  L"」安全拼成当前仓库之内的目录路径（父仓库根目录此刻也不可用）。"
                  L"本程序不去猜一个落点，也没有换绑任何仓库。");
  }
  // 先问父索引：它是「这条 gitlink 到底还在不在索引里」的唯一依据。
  if (!facts.index.readOk) {
    return refuse(L"没能问清父仓库索引里那条 gitlink：" +
                  (facts.index.readFailure.empty() ? std::wstring(L"查询没有正常回答")
                                                  : facts.index.readFailure) +
                  L"。看不清这条记录的来历就不进目录。");
  }
  if (!facts.index.recordFound) {
    return refuse(L"父仓库的索引里已经没有 " + facts.relativePath +
                  L" 这条记录了：界面显示的那一条与索引的事实不是同一件事（多半刚被移出暂存区、"
                  L"改名或删掉）。本程序没有进入任何仓库，请点“刷新”后再决定。");
  }
  if (!facts.index.isGitlink) {
    return refuse(L"父仓库索引里 " + facts.relativePath + L" 那条记录的 mode 是 " +
                  facts.index.mode + L"，不是 160000 gitlink。它已经不是「一个子模块」了，"
                  L"本程序不把它当子模块进入。");
  }
  if (facts.presence != SubmodulePathPresence::existsDirectory) {
    return refuse(PresenceProblem(facts));
  }
  if (!facts.childProbed) {
    return refuse(L"没能对那个目录跑一次仓库识别（只读查询没有做成）。"
                  L"没有 Git 的回答就没法确认它是谁的子模块，本程序不换绑仓库。");
  }
  if (!facts.child.submodule || facts.child.kind != RepoKind::submodule) {
    return refuse(ChildIdentityProblem(facts));
  }
  if (facts.child.superprojectTree.empty() ||
      !PathsEqualFolded(facts.child.superprojectTree, facts.parentRoot)) {
    return refuse(ChildIdentityProblem(facts));
  }

  plan.decision = SubmoduleEntryDecision::enter;
  plan.targetDirectory = facts.resolvedDirectory;
  std::wstring reason = L"要进入的子模块：" + facts.relativePath + L"（" + facts.resolvedDirectory +
                        L"）\n父仓库：" + facts.parentRoot +
                        L"\n父索引那条 gitlink 记的是：" + ShortOrEmpty(facts.index.objectId) +
                        L"\n识别结果（全部来自 Git 的只读回答）：" +
                        (facts.child.message.empty() ? std::wstring(RepoKindLabel(facts.child.kind))
                                                     : facts.child.message);
  reason += L"\n\n这一步做的事只有两件：把界面绑定的仓库换成这个子模块的工作区，"
            L"以及把父仓库这一份未提交的表单草稿连同「从哪儿来」记下来（返回时原样交回）。"
            L"\n不做的事：不改父仓库索引、不 add、不 commit、不 push、不联网，"
            L"也不递归处理任何子模块。";
  plan.reason = std::move(reason);

  std::wstring disclosure;
  if (facts.flags.commitChanged) {
    disclosure += L" · 父仓库看到这条指针**已经变了**（子模块 HEAD 与父索引记的不是同一份）。\n";
  } else {
    disclosure += L" · 父仓库看到这条指针此刻与子模块 HEAD 一致；返回时还会再核一次。\n";
  }
  if (facts.flags.trackedChanges) {
    disclosure += L" · 子模块内部有已跟踪文件的改动：那些要在子模块自己的列表里暂存与提交，"
                  L"父仓库那一头只会记指针。\n";
  }
  if (facts.flags.untrackedChanges) {
    disclosure += L" · 子模块内部还有未跟踪文件：它们同样属于子模块自己的仓库，"
                  L"父仓库既看不到也不会动它们。\n";
  }
  if (!facts.child.branch.empty()) {
    disclosure += L" · 子模块当前分支：" + facts.child.branch + L"（游离或尚无提交时以父仓库识别的回答为准）。\n";
  }
  disclosure += L" · 提交子模块内部内容与提交父仓库的指针是**两个不同的操作**，"
                L"在两个不同的仓库里做；父仓库显示干净不证明子模块那次提交已经发布。";
  plan.disclosure = std::move(disclosure);
  return plan;
}

// ---- 返回父仓库的准入 ----

bool IsJourneyChildRoot(std::wstring_view recordedChildRoot, std::wstring_view currentRoot) {
  if (recordedChildRoot.empty() || currentRoot.empty()) {
    return false;  // 任何一边不知道，就不能断言「还在刚才那个子模块里」。
  }
  return PathsEqualFolded(recordedChildRoot, currentRoot);
}

}  // namespace gc::git
