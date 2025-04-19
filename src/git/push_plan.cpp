#include "git/push_plan.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "git/commit_history.h"  // LooksLikeFullObjectId / ShortObjectId

namespace gc::git {
namespace {

constexpr std::wstring_view kHeadsPrefix = L"refs/heads/";
constexpr std::wstring_view kRefsPrefix = L"refs/";

// 配置鍵的段名與變量名在 `git config --list` 的輸出裡已被 Git 小寫化（subsection 原樣保留），
// 因此查表一律用小寫形态；给人看的文案另外用使用者书写的驼峰形态。
constexpr std::wstring_view kKeyBranchPushRemote = L".pushremote";
constexpr std::wstring_view kKeyBranchRemote = L".remote";
constexpr std::wstring_view kKeyRemotePrefix = L"remote.";
constexpr std::wstring_view kKeyUrl = L".url";
constexpr std::wstring_view kKeyPushUrl = L".pushurl";

bool StartsWith(std::wstring_view text, std::wstring_view prefix) noexcept {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// 与命令窗口执行器（git/command_window）同一套边界判定：双引号与控制字符进不了脚本行。
// 这里提前拒绝是为了给出「哪一段、为什么」的说明，而不是让一次合法点击停在执行器的通用拒绝上。
bool HasIllegalCharacter(std::wstring_view text) {
  if (text.empty()) {
    return true;
  }
  for (const wchar_t c : text) {
    if (c == L'"' || c == L':' || c < 0x20 || c == 0x7F) {
      return true;
    }
  }
  return false;
}

bool IsUsableLocalBranchRef(std::wstring_view ref) {
  return ref.size() > kHeadsPrefix.size() && StartsWith(ref, kHeadsPrefix) &&
         !HasIllegalCharacter(ref);
}

// 远端那一侧只要求 `refs/` 开头：上游配置把分支映射到 refs/heads/ 以外的引用是 Git 允许的形态，
// 本程序照它推，不「纠正」成 refs/heads/。但不是 refs/ 的东西（裸名字、`HEAD`、以 `-` 开头的写法）
// 一律拒绝——那种场合猜一次就是替用户决定推到哪。
bool IsUsableRemoteRef(std::wstring_view ref) {
  return ref.size() > kRefsPrefix.size() && StartsWith(ref, kRefsPrefix) && !HasIllegalCharacter(ref);
}

bool IsUsableRemoteName(std::wstring_view name) {
  return !name.empty() && name.front() != L'-' && !HasIllegalCharacter(name);
}

std::wstring LowerAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
  }
  return result;
}

// Git 认的布尔取值（大小写不敏感）。认不出来的取值返回 nullopt：不猜它想说什么。
std::optional<bool> ParseConfigBool(std::wstring_view value) {
  const std::wstring text = LowerAscii(value);
  if (text == L"true" || text == L"yes" || text == L"1" || text == L"on") {
    return true;
  }
  if (text == L"false" || text == L"no" || text == L"0" || text == L"off") {
    return false;
  }
  return std::nullopt;
}

std::vector<std::wstring> SplitNulList(const std::wstring& text) {
  std::vector<std::wstring> items;
  std::wstring current;
  for (const wchar_t c : text) {
    if (c == L'\0') {
      if (!current.empty()) {
        items.push_back(current);
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    items.push_back(current);
  }
  return items;
}

std::wstring RefusalWith(const UndoQueryRead& read, std::wstring_view fallback) {
  return read.detail.empty() ? std::wstring(fallback) : read.detail;
}

// `remote.<名字>.<變數>` 的查表鍵。
std::wstring RemoteKey(std::wstring_view remoteName, std::wstring_view variable) {
  return std::wstring(kKeyRemotePrefix) + std::wstring(remoteName) + L"." + std::wstring(variable);
}

std::wstring BranchKey(std::wstring_view branchName, std::wstring_view variable) {
  return L"branch." + std::wstring(branchName) + L"." + std::wstring(variable);
}

// 「这条设置到底会不会生效」：设成了明确的假值就算没设，其余（包括 Git 不认的取值）都按会生效对待，
// 因为那种取值本来就会让整条命令失败，中和它正是把这条路还给用户。
bool ConfigTakesEffect(const PushConfigListing& config, std::wstring_view key) {
  if (!config.HasKey(key)) {
    return false;
  }
  return ParseConfigBool(config.Value(key)).value_or(true);
}

std::wstring DescribeRemoteSet(std::wstring_view url) {
  return url.empty() ? std::wstring(L"（没能问出发布 URL）") : MaskPushUrlCredentials(url);
}

std::wstring UpstreamSentence(const PushPreflightFacts& facts) {
  std::wstring text = L"上游跟踪引用：" + facts.upstreamTrackingRef;
  if (facts.trackingResolved) {
    text += L"（本地记的位置是 " + ShortObjectId(facts.trackingObjectId) +
            L"；这只是上一次 fetch 读回来的，不代表远端现在停在哪）\n";
  } else if (facts.trackingRan) {
    text += L"（本地还没有这个引用：上一次抓取没把它带回来，也可能远端那边还没有这条分支）\n";
  } else {
    text += L"（位置未知）\n";
  }
  return text;
}

std::wstring RelationshipSentence(const PushPreflightFacts& facts) {
  if (!facts.relationship.known) {
    return L"本地领先/落后：没能问出（" +
           (facts.relationship.detail.empty() ? std::wstring(L"原因未知") : facts.relationship.detail) +
           L"）。\n";
  }
  std::wstring text = L"本地领先 " + std::to_wstring(facts.relationship.ahead) + L" 个提交、落后 " +
                      std::to_wstring(facts.relationship.behind) + L" 个提交";
  text += facts.pushTargetIsFetchTarget
              ? std::wstring(L"（依据：" + facts.upstreamTrackingRef + L"，也就是本次的发布目标）\n")
              : std::wstring(L"（依据：" + facts.upstreamTrackingRef +
                             L"，那是抓取的那一侧；本次的发布目标另有去处，这个数字只能当参考）\n");
  return text;
}

std::wstring PushTargetSentence(const PushPreflightFacts& facts) {
  std::wstring text = L" · 目标远端：" + facts.pushRemoteName + L"（来历：" + facts.pushRemoteSource +
                      L"）\n";
  text += L" · 发布 URL：" + FormatPushUrlList(facts.pushUrls) + L"\n";
  if (!facts.pushUrlNote.empty()) {
    text += L"　" + facts.pushUrlNote + L"\n";
  }
  return text;
}

std::wstring NeutralizeSentence(const PushPreflightFacts& facts) {
  std::wstring text;
  if (facts.mirrorConfigured) {
    text += L" · 仓库里有 remote." + facts.pushRemoteName +
            L".mirror：实测它会让「带 refspec 的 push」整条命令失败，因此这条命令临时按 "
            L"-c remote." + facts.pushRemoteName +
            L".mirror=false 发出（只对这个子进程生效，不写你的配置文件）。\n";
  }
  if (facts.tagOptConfigured) {
    // 命令里写作 `tagopt`（小写）：Git 的配置键名大小写不敏感，这里跟命令保持一字不差，
    // 免得人在窗口里找不到界面说的那个写法。
    text += L" · 仓库里有 remote." + facts.pushRemoteName +
            L".tagOpt：为免它把标签一起带上，这条命令临时用 -c remote." + facts.pushRemoteName +
            L".tagopt= 把它置空。\n";
  }
  if (facts.followTagsConfigured) {
    text += L" · 仓库里有 push.followTags：这条命令临时用 -c push.followTags=false 关掉它。\n";
  }
  if (facts.extraPushRefspecsConfigured) {
    text += L" · 仓库里有 remote." + facts.pushRemoteName +
            L".push（额外的推送 refspec）：实测带显式 refspec 时它不会被附上，"
            L"本程序仍然只推命令行上那一条。\n";
  }
  return text;
}

std::wstring QuotedArguments(const std::vector<std::wstring>& arguments) {
  std::wstring text = L"git";
  for (const std::wstring& argument : arguments) {
    text += L' ' + argument;
  }
  return text;
}

}  // namespace

// ---- 生效配置清单 ----

std::wstring PushConfigListing::Value(std::wstring_view key) const {
  std::wstring found;
  for (const auto& entry : entries) {
    if (entry.first == key) {
      found = entry.second;  // 後出現的覆蓋先出現的：與 `git config --get` 一致。
    }
  }
  return found;
}

std::vector<std::wstring> PushConfigListing::Values(std::wstring_view key) const {
  std::vector<std::wstring> found;
  for (const auto& entry : entries) {
    if (entry.first == key) {
      found.push_back(entry.second);
    }
  }
  return found;
}

bool PushConfigListing::HasKey(std::wstring_view key) const {
  for (const auto& entry : entries) {
    if (entry.first == key) {
      return true;
    }
  }
  return false;
}

std::vector<std::wstring> PushConfigListing::RemoteValues(std::wstring_view remoteName,
                                                          std::wstring_view variable) const {
  return Values(RemoteKey(remoteName, variable));
}

std::vector<std::wstring> PushConfigListing::RemoteNames() const {
  std::vector<std::wstring> names;
  for (const auto& entry : entries) {
    const std::wstring& key = entry.first;
    if (!StartsWith(key, kKeyRemotePrefix)) {
      continue;
    }
    const std::wstring_view rest = std::wstring_view(key).substr(kKeyRemotePrefix.size());
    bool urlish = false;
    std::wstring_view name;
    if (rest.size() > kKeyUrl.size() && rest.substr(rest.size() - kKeyUrl.size()) == kKeyUrl) {
      urlish = true;
      name = rest.substr(0, rest.size() - kKeyUrl.size());
    } else if (rest.size() > kKeyPushUrl.size() &&
               rest.substr(rest.size() - kKeyPushUrl.size()) == kKeyPushUrl) {
      urlish = true;
      name = rest.substr(0, rest.size() - kKeyPushUrl.size());
    }
    if (!urlish || name.empty()) {
      continue;
    }
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(std::wstring(name));
    }
  }
  return names;
}

PushConfigListing ParsePushConfigListing(const GitQueryResult& listing) {
  PushConfigListing result;
  const UndoQueryRead read = ReadUndoQuery(listing);
  if (read.outcome != UndoQueryOutcome::answered) {
    result.readFailure = RefusalWith(read, L"git config --list 未成功");
    return result;
  }
  // --null 的记录形态（本机 Git 2.53 实测，od -c 逐字节看过）：`key<换行>value<NUL>`。
  // 键里不会有换行，值里可能有（多行值），所以只按**第一个**换行切；切出来的键若还含 `=`，
  // 说明这个版本用的是 `key=value<NUL>` 那种写法，退回按第一个 `=` 切。
  // 两种都认不是「为不可能的场合兜底」：这是 Git 自己跨版本的输出形态，认不出来的记录才是真认不出。
  for (const std::wstring& entry : SplitNulList(listing.utf16Output)) {
    const size_t newline = entry.find(L'\n');
    const size_t equals = entry.find(L'=');
    size_t separator = std::wstring::npos;
    if (newline != std::wstring::npos && (equals == std::wstring::npos || newline < equals)) {
      separator = newline;
    } else if (equals != std::wstring::npos) {
      separator = equals;
    }
    if (separator == std::wstring::npos) {
      result.entries.clear();
      result.readFailure =
          L"git config --list 的回答里有一条既不含换行也不含 `=`，这份配置清单没法照原样采信（那一条是：" +
          entry + L"）。本程序不在看不清实际发布地点的前提下推送。";
      return result;
    }
    result.entries.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
  }
  result.readOk = true;  // 一条配置也没有也是明确答案（全新隔离仓库就是这个形态）。
  return result;
}

std::wstring MaskPushUrlCredentials(std::wstring_view url) {
  const size_t schemeEnd = url.find(L"://");
  if (schemeEnd == std::wstring_view::npos) {
    return std::wstring(url);  // 本机路径、相对路径一类：本来就没有 userinfo 的位置。
  }
  // authority 段到第一个 `/`、`\` 或 `?` 为止（`#` 不算：它属于 fragment，也不会出现在远端 URL 里）。
  size_t authorityEnd = url.size();
  for (size_t index = schemeEnd + 3; index < url.size(); ++index) {
    const wchar_t c = url[index];
    if (c == L'/' || c == L'\\' || c == L'?') {
      authorityEnd = index;
      break;
    }
  }
  const size_t at = url.find_last_of(L'@', authorityEnd - 1);
  if (at == std::wstring_view::npos || at < schemeEnd + 3) {
    return std::wstring(url);
  }
  return std::wstring(url.substr(0, schemeEnd + 3)) + L"***@" +
         std::wstring(url.substr(at + 1));
}

std::wstring FormatPushUrlList(const std::vector<std::wstring>& urls) {
  if (urls.empty()) {
    return std::wstring(L"（没能问出发布 URL）");
  }
  if (urls.size() == 1) {
    return DescribeRemoteSet(urls.front());
  }
  std::wstring text = L"共 " + std::to_wstring(urls.size()) + L" 个（Git 会推给每一个）：";
  for (size_t index = 0; index < urls.size(); ++index) {
    text += L"\n   " + std::to_wstring(index + 1) + L") " + MaskPushUrlCredentials(urls[index]);
  }
  return text;
}

// ---- 查询参数 ----

std::vector<std::wstring> BuildPushConfigListingArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"config", L"--list", L"--null"};
}

std::vector<std::wstring> BuildPushRemoteUrlArguments(std::wstring_view repositoryDirectory,
                                                      std::wstring_view remoteName) {
  if (!IsUsableRemoteName(remoteName)) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"remote", L"get-url", L"--push", std::wstring(remoteName)};
}

std::vector<std::wstring> BuildPushCommandArguments(std::wstring_view remoteName,
                                                   std::wstring_view localBranchRef,
                                                   std::wstring_view remoteBranchRef,
                                                   bool neutralizeMirror, bool neutralizeTagOpt) {
  if (!IsUsableRemoteName(remoteName) || !IsUsableLocalBranchRef(localBranchRef) ||
      !IsUsableRemoteRef(remoteBranchRef)) {
    return {};
  }
  // -c 是 git 的全局选项，必须排在子命令之前；--recurse-submodules=no 是 push 自己的选项。
  std::vector<std::wstring> arguments;
  if (neutralizeMirror) {
    arguments.push_back(L"-c");
    arguments.push_back(RemoteKey(remoteName, L"mirror") + L"=false");
  }
  if (neutralizeTagOpt) {
    arguments.push_back(L"-c");
    arguments.push_back(RemoteKey(remoteName, L"tagopt") + L"=");
  }
  arguments.push_back(L"-c");
  arguments.push_back(L"push.followTags=false");
  arguments.push_back(L"push");
  arguments.push_back(L"--recurse-submodules=no");
  arguments.push_back(std::wstring(remoteName));
  // 完整两侧的显式 refspec：不写 `main`、不写 `HEAD`，也不留任何「由 push.default 决定」的余地。
  arguments.push_back(std::wstring(localBranchRef) + L":" + std::wstring(remoteBranchRef));
  return arguments;
}

std::vector<std::wstring> BuildPushRemoteProbeArguments(std::wstring_view repositoryDirectory,
                                                        std::wstring_view pushUrl,
                                                        std::wstring_view remoteBranchRef) {
  if (repositoryDirectory.empty() || pushUrl.empty() || !IsUsableRemoteRef(remoteBranchRef)) {
    return {};
  }
  // `--` 之后才是仓库地址：以 `-` 开头的 URL 会被 Git 当成选项，隔开之后它只剩位置这一种含义。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"ls-remote", L"--", std::wstring(pushUrl),
                                   std::wstring(remoteBranchRef)};
}

// ---- 发布远端的裁决 ----

PushRemoteChoice ResolvePushRemote(const PushConfigListing& config, std::wstring_view branchName,
                                   std::wstring_view upstreamRemote) {
  if (!config.readOk) {
    return {std::wstring(), L"生效配置没能读回来：" + config.readFailure};
  }
  // 注意：这里必须持有实际的 std::wstring 再取视图——把 string_view 绑到函数返回的临时串上，
  // 临时量在语句结束就析构，视图当场悬空（fetch_plan 曾因同样的写法把整份远端清单读成空）。
  const std::wstring branchKey = BranchKey(branchName, L"pushremote");
  if (!branchName.empty() && config.HasKey(branchKey)) {
    const std::wstring value = config.Value(branchKey);
    if (value.empty()) {
      return {std::wstring(), L"branch." + std::wstring(branchName) +
                                  L".pushRemote 被设成了空值（Git 在这种情况下也没有可推的远端）"};
    }
    return {value, L"branch." + std::wstring(branchName) + L".pushRemote = " + value};
  }
  if (config.HasKey(L"remote.pushdefault")) {
    const std::wstring value = config.Value(L"remote.pushdefault");
    if (value.empty()) {
      return {std::wstring(), L"remote.pushDefault 被设成了空值"};
    }
    return {value, L"remote.pushDefault = " + value};
  }
  if (!upstreamRemote.empty()) {
    return {std::wstring(upstreamRemote),
            branchName.empty()
                ? std::wstring(L"上游所在的远端（branch.<分支>.remote）")
                : std::wstring(L"没有单独的推送远端配置，用上游所在的远端 branch.") +
                      std::wstring(branchName) + L".remote = " + std::wstring(upstreamRemote)};
  }
  return {std::wstring(), L"既没有 branch.<分支>.pushRemote / remote.pushDefault，也没有上游所在远端"};
}

PushRemoteChoice ResolvePushRemoteFromQueries(const GitQueryResult& configListing,
                                              std::wstring_view branchName,
                                              std::wstring_view upstreamRemote) {
  return ResolvePushRemote(ParsePushConfigListing(configListing), branchName, upstreamRemote);
}

// ---- 预检判读 ----

PushPreflightFacts InterpretPushPreflight(const PushPreflightQueries& queries) {
  PushPreflightFacts facts;
  facts.workflow = queries.workflow;
  facts.workflowProbed = queries.workflowProbed;

  const UndoQueryRead symbolic = ReadUndoQuery(queries.symbolicRef);
  if (symbolic.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出当前分支：" + RefusalWith(symbolic, L"symbolic-ref 未成功");
    return facts;
  }
  if (symbolic.outcome == UndoQueryOutcome::answered) {
    facts.branchRef = symbolic.firstLine;
    facts.onBranch = PullBranchNameFromRef(facts.branchRef, &facts.branchName);
    if (!facts.onBranch) {
      facts.branchRef.clear();  // refs/heads/ 以外的引用：没有「这个分支的上游」可谈。
    }
  }

  const UndoQueryRead head = ReadUndoQuery(queries.headObject);
  facts.headQueried = head.outcome != UndoQueryOutcome::failed;
  if (head.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出 HEAD 指向哪个提交：" + RefusalWith(head, L"rev-parse 未成功");
    return facts;
  }
  if (head.outcome == UndoQueryOutcome::answered) {
    if (!LooksLikeFullObjectId(head.firstLine)) {
      facts.queryFailure = L"rev-parse 给出的 HEAD 不符合完整对象 ID 的约定，不在这上面做判断。";
      return facts;
    }
    facts.headResolved = true;
    facts.headObjectId = head.firstLine;
  }
  // noResult：这个分支还没有任何提交——是明确答案，交给方案层拒绝（那种场合连「要推哪一份」都没有）。

  if (facts.onBranch && queries.upstreamRan) {
    facts.upstreamRan = true;
    const UndoQueryRead upstream = ReadUndoQuery(queries.upstream);
    if (upstream.outcome == UndoQueryOutcome::failed) {
      facts.queryFailure = L"没能问出分支 " + facts.branchName + L" 的上游配置：" +
                           RefusalWith(upstream, L"for-each-ref 未成功");
      return facts;
    }
    if (upstream.outcome == UndoQueryOutcome::answered && !upstream.lines.empty()) {
      const PullUpstreamInfo info = ParsePullUpstreamLine(upstream.lines.front());
      facts.upstreamRemote = info.remote;
      facts.upstreamTrackingRef = info.trackingRef;
      facts.upstreamRemoteRef = info.remoteBranch;
      facts.upstreamConfigured = info.configured;
    }
  }

  if (facts.upstreamConfigured && queries.trackingRan) {
    facts.trackingRan = true;
    const UndoQueryRead tracking = ReadUndoQuery(queries.trackingObject);
    if (tracking.outcome == UndoQueryOutcome::failed) {
      facts.queryFailure = L"没能问出 " + facts.upstreamTrackingRef + L" 停在哪个提交：" +
                           RefusalWith(tracking, L"rev-parse 未成功");
      return facts;
    }
    if (tracking.outcome == UndoQueryOutcome::answered) {
      if (!LooksLikeFullObjectId(tracking.firstLine)) {
        facts.queryFailure = L"rev-parse 给出的 " + facts.upstreamTrackingRef +
                             L" 不符合完整对象 ID 的约定，不在这上面做判断。";
        return facts;
      }
      facts.trackingResolved = true;
      facts.trackingObjectId = tracking.firstLine;
    } else {
      facts.trackingDetail = L"本地还没有这个引用。";
    }
  }

  facts.config = ParsePushConfigListing(queries.configListing);
  facts.configOk = facts.config.readOk;
  if (!facts.config.readOk) {
    // 发布目标的依据全在这份清单里（推送远端优先序、pushurl、mirror/tagOpt 等）。
    // 看不见它就不能确定「这一条命令到底会把东西送到哪」——直接拒绝，不带着猜上线。
    facts.queryFailure = L"没能问出这个仓库的生效配置：" + facts.config.readFailure;
    return facts;
  }

  const PushRemoteChoice choice =
      ResolvePushRemote(facts.config, facts.branchName, facts.upstreamRemote);
  facts.pushRemoteName = choice.remoteName;
  facts.pushRemoteSource = choice.source;
  facts.pushDefault = facts.config.Value(L"push.default");
  facts.extraPushRefspecsConfigured = !facts.config.RemoteValues(facts.pushRemoteName, L"push").empty();
  facts.mirrorConfigured = ConfigTakesEffect(facts.config, RemoteKey(facts.pushRemoteName, L"mirror"));
  facts.tagOptConfigured = !facts.config.RemoteValues(facts.pushRemoteName, L"tagopt").empty();
  facts.followTagsConfigured = ConfigTakesEffect(facts.config, L"push.followtags");

  if (IsUsableRemoteName(facts.pushRemoteName)) {
    facts.pushRemoteExists = !facts.config.RemoteValues(facts.pushRemoteName, L"url").empty() ||
                             !facts.config.RemoteValues(facts.pushRemoteName, L"pushurl").empty();
    const std::vector<std::wstring> pushUrls =
        facts.config.RemoteValues(facts.pushRemoteName, L"pushurl");
    facts.rawPushUrls =
        pushUrls.empty() ? facts.config.RemoteValues(facts.pushRemoteName, L"url") : pushUrls;
    // get-url 的回答是 Git 自己算的「实际会去哪里」：pushurl 优先于 url，而且 url.*.insteadOf /
    // url.*.pushInsteadOf 的改写已经叠在里面。多个 pushurl 时它只回答第一条，其余按配置原样列出。
    std::wstring effective;
    if (queries.remoteUrlRan) {
      const UndoQueryRead urlRead = ReadUndoQuery(queries.remoteUrl);
      if (urlRead.outcome == UndoQueryOutcome::answered) {
        effective = urlRead.firstLine;
      } else if (urlRead.outcome == UndoQueryOutcome::failed) {
        facts.pushUrlFailure = RefusalWith(urlRead, L"git remote get-url 未成功");
      }
    }
    if (!effective.empty()) {
      // 「改写」只在其一目标时说得准：拿 get-url 的回答与配置里那一条比。
      facts.pushUrlRewritten = !facts.rawPushUrls.empty() && effective != facts.rawPushUrls.front();
      facts.pushUrls =
          facts.rawPushUrls.size() > 1 ? facts.rawPushUrls : std::vector<std::wstring>{effective};
    } else {
      facts.pushUrls = facts.rawPushUrls;
    }
    if (facts.rawPushUrls.size() > 1) {
      facts.pushUrlNote =
          L"这个远端配了 " + std::to_wstring(facts.rawPushUrls.size()) +
          L" 个 push URL：实测 `git push <远端>` 会推给每一个。`git remote get-url --push` 只能"
          L"回答第一个的改写结果，因此上面按配置原样列出全部；如果其中有的配了 url.*.insteadOf，"
          L"Git 推送时仍会逐条改写——那一处本程序核不出来，推送后会逐个目标去问实际位置。";
    } else if (facts.pushUrlRewritten && !facts.rawPushUrls.empty()) {
      facts.pushUrlNote = L"这个地址是 Git 按 url.*.insteadOf / pushInsteadOf 改写之后的结果"
                          L"（配置里写的是 " + MaskPushUrlCredentials(facts.rawPushUrls.front()) + L"）。";
    }
    const std::vector<std::wstring> configuredPushUrls =
        facts.config.RemoteValues(facts.pushRemoteName, L"pushurl");
    facts.pushTargetIsFetchTarget =
        facts.pushRemoteName == facts.upstreamRemote && configuredPushUrls.empty() &&
        !facts.pushUrlRewritten;
  }

  if (queries.aheadBehindRan) {
    const UndoQueryRead count = ReadUndoQuery(queries.aheadBehind);
    if (count.outcome != UndoQueryOutcome::answered) {
      facts.relationship.detail = RefusalWith(count, L"rev-list --left-right --count 未成功");
    } else {
      const std::wstring_view line =
          count.lines.empty() ? std::wstring_view() : std::wstring_view(count.lines.front());
      if (ParseAheadBehindCount(line, &facts.relationship.ahead, &facts.relationship.behind)) {
        facts.relationship.known = true;
      } else {
        facts.relationship.detail =
            L"rev-list --left-right --count 的回答不是「左 右」两个非负整数（那一行是：" +
            std::wstring(line) + L"）";
      }
    }
  }

  facts.queryOk = true;
  return facts;
}

// ---- 方案 ----

namespace {

// 前提核对：返回空串表示可以给出命令。
std::wstring PushPrerequisiteRefusal(const PushPreflightFacts& facts,
                                     std::wstring_view repositoryDirectory) {
  if (repositoryDirectory.empty()) {
    return L"没有可用的仓库工作区根目录，无法确定这次推送属于哪个仓库。请先点“刷新”。";
  }
  if (!facts.queryOk) {
    return facts.queryFailure.empty() ? std::wstring(L"推送前的只读查询没能完成，无法确定分支与发布目标。")
                                      : facts.queryFailure;
  }
  if (!facts.onBranch) {
    return L"当前处于游离 HEAD（不在任何分支上）。push 要做的是「把这个分支送到它的上游」，"
           L"游离状态下既没有分支、也没有分支级的上游配置可依据——随便挑一个分支去推就是替你决定"
           L"仓库的历史该怎么长。本程序不这么做。请先切回一个分支，点“刷新”后再来。";
  }
  if (!IsUsableLocalBranchRef(facts.branchRef)) {
    return L"当前分支的引用名形态不合格（不是 refs/heads/ 开头，或含命令窗口无法安全表达的字符）：" +
           facts.branchRef + L"。没有构造任何命令。";
  }
  if (!facts.headQueried) {
    return L"没能问出 HEAD 指向哪个提交，本程序不在问不出目标的前提下动远端。";
  }
  if (!facts.headResolved) {
    return L"分支 " + facts.branchName +
           L" 还没有任何提交（HEAD 还不可解析），这一次没有可推送的东西。"
           L"请先创建提交（或把已有的东西提交上去），再点“刷新”。";
  }
  if (!facts.upstreamRan) {
    return L"没能完成上游配置的查询，无法确定这次推送的目标。请点“刷新”后重试。";
  }
  if (!facts.upstreamConfigured) {
    if (!facts.upstreamRemote.empty()) {
      return L"当前分支的上游配置不完整：远端是「" + facts.upstreamRemote + L"」，可 branch." +
             facts.branchName + L".merge 没有对应的分支记录。没有可确定的远端分支，本程序不猜一个同名"
             L"分支推上去，也不改你的配置。";
    }
    return L"分支 " + facts.branchName +
           L" 没有设置上游（branch." + facts.branchName + L".remote / .merge 未配置）。"
           L"没有上游就不知道“该推给哪个远端的哪条分支”，本程序不猜 origin/" + facts.branchName +
           L"，也不替你 git push --set-upstream、git branch --set-upstream-to——那些都会写你的配置文件。"
           L"请先用 Git 把上游设好（例如 git push -u origin " + facts.branchName +
           L"，或 git branch --set-upstream-to=origin/" + facts.branchName + L"），再回来点“推送”。";
  }
  if (!IsUsableRemoteRef(facts.upstreamRemoteRef)) {
    return L"上游配置给出的远端引用形态不合格（不是 refs/ 开头，或含 `:`、引号等无法安全进入命令的字符）：" +
           (facts.upstreamRemoteRef.empty() ? std::wstring(L"（空）") : facts.upstreamRemoteRef) +
           L"。本程序不把它“改写成一个看起来对的”，也没有发出任何命令。";
  }
  if (!facts.configOk) {
    return L"这个仓库的生效配置没能读回来，发布目标无从确定。";
  }
  if (facts.pushRemoteName.empty()) {
    return L"没能定出这次要推给哪个远端（依据：" +
           (facts.pushRemoteSource.empty() ? std::wstring(L"没有可依据的配置") : facts.pushRemoteSource) +
           L"）。本程序不替你挑一个远端推上去。";
  }
  if (!IsUsableRemoteName(facts.pushRemoteName)) {
    return L"定出的推送远端名字形态不合格（为空、以 - 开头，或含命令窗口无法安全表达的字符）：" +
           facts.pushRemoteName + L"。没有构造任何命令。";
  }
  if (!facts.pushRemoteExists) {
    std::wstring text = L"这次要推给远端「" + facts.pushRemoteName + L"」（依据：" + facts.pushRemoteSource +
                        L"），可这个仓库的远端清单里没有它——它没有 url，Git 也无从知道该送到哪里。"
                        L"本程序不猜另一个远端顶替，也不会替你 git remote add、不改任何配置。";
    const std::vector<std::wstring> names = facts.config.RemoteNames();
    text += names.empty()
                ? std::wstring(L"这个仓库目前一个远端也没有。")
                : std::wstring(L"仓库里既有的远端是：") + [&names] {
                    std::wstring joined;
                    for (const std::wstring& name : names) {
                      if (!joined.empty()) {
                        joined += L"、";
                      }
                      joined += name;
                    }
                    return joined;
                  }() + L"。";
    return text;
  }
  if (facts.pushUrls.empty()) {
    return L"远端「" + facts.pushRemoteName + L"」在清单里，却问不出了一个可用的发布 URL（" +
           (facts.pushUrlFailure.empty() ? std::wstring(L"git remote get-url --push 没有给出地址")
                                         : facts.pushUrlFailure) +
           L"）。看不见要去哪里就不推：本程序不在猜出来的地址上动远端。";
  }
  return std::wstring();
}

}  // namespace

PushPlan BuildPushPlan(const PushPreflightFacts& facts, std::wstring_view repositoryDirectory) {
  const auto block = [](std::wstring reason) {
    PushPlan blocked;
    blocked.state = PushPlanState::blocked;
    blocked.explanation = std::move(reason);
    blocked.operationId = L"push";
    blocked.displayName = L"推送";
    return blocked;
  };

  const std::wstring refusal = PushPrerequisiteRefusal(facts, repositoryDirectory);
  if (!refusal.empty()) {
    return block(refusal + L"\n\n没有发出任何命令，也没有接触远端、没有改动本地与远端的任何东西。");
  }

  const std::vector<std::wstring> arguments =
      BuildPushCommandArguments(facts.pushRemoteName, facts.branchRef, facts.upstreamRemoteRef,
                               facts.mirrorConfigured, facts.tagOptConfigured);
  if (arguments.empty()) {
    return block(L"推送命令的参数没有通过构造期的形态检查（引用名或远端名里有 Git 命令行无法安全"
                 L"表达的写法）。没有发出任何命令。");
  }

  PushPlan plan;
  plan.state = PushPlanState::ready;
  plan.operationId = L"push";
  plan.displayName = L"推送";
  plan.branchName = facts.branchName;
  plan.localBranchRef = facts.branchRef;
  plan.remoteBranchRef = facts.upstreamRemoteRef;
  plan.remoteName = facts.pushRemoteName;
  plan.remoteNameSource = facts.pushRemoteSource;
  plan.pushUrls = facts.pushUrls;
  plan.remoteUrlDisplay = FormatPushUrlList(facts.pushUrls);
  plan.pushedObjectId = facts.headObjectId;
  plan.arguments = arguments;
  plan.commandLabel = QuotedArguments(arguments);

  // ---- 风险：要用户明确点头的几条 ----
  std::vector<std::wstring> risks;
  if (!facts.pushTargetIsFetchTarget) {
    std::wstring text = L"本次的发布目标与上游所在的抓取目标不是同一个地方：";
    if (facts.pushRemoteName != facts.upstreamRemote) {
      text += L"上游在远端「" + facts.upstreamRemote + L"」，而按 " + facts.pushRemoteSource +
              L" 这次要推给远端「" + facts.pushRemoteName + L"」";
    } else {
      text += L"远端「" + facts.pushRemoteName + L"」配了独立的 push URL，或地址被 url.*.insteadOf "
               L"改写过：抓回来的那份历史与这次要送过去的地方可能不是同一个仓库";
    }
    text += L"。界面顶部的“上游”以及下面那句领先/落后，说的都是抓取的那一侧，不能拿来当成推送的结果。"
            L"本程序把两边都写在这里，推送之后还会向**发布目标**逐条核实（见结果说明）。";
    risks.push_back(std::move(text));
  }
  if (facts.pushUrls.size() > 1) {
    risks.push_back(L"这个远端配了 " + std::to_wstring(facts.pushUrls.size()) +
                    L" 个 push URL，实测 Git 会推给每一个：这一次点“推送”等于同时推 " +
                    std::to_wstring(facts.pushUrls.size()) + L" 个地方。上面已经把每一个地址都列出来了。");
  }
  if (facts.relationship.known && facts.relationship.behind > 0) {
    risks.push_back(L"本地落后 " + std::to_wstring(facts.relationship.behind) +
                    L" 个提交：按 " + facts.upstreamTrackingRef +
                    L"（上一次 fetch 读回的位置）看，那边有本地没有的提交，这次推送会被 Git 以"
                    L" non-fast-forward 拒绝。本程序不带 --force、不带 --force-with-lease，"
                    L"也不会先替你 pull——那种“为了让它成功而更激烈”的做法会把别人（或别的机器）"
                    L"的提交从这条分支上抹掉。被拒绝时先 fetch/pull 把远端的东西整合进来，再推。");
  }
  if (facts.relationship.known && facts.relationship.ahead == 0 && facts.relationship.behind == 0) {
    risks.push_back(L"按 " + facts.upstreamTrackingRef +
                    L" 看，本地与它记录的位置是同一份提交：这次推送多半只会得到「Everything "
                    L"up-to-date」。仍然把命令发出去，是为了向远端问个准话（本地这份了解可能早已过期）。");
  }
  if (!facts.trackingResolved) {
    risks.push_back(L"本地没有 " + facts.upstreamTrackingRef +
                    L" 这个引用：远端那边也许还没有这条分支，这次推送会在对端**新建**它；"
                    L"也可能只是你还没抓取。本程序不替你分辨，也不替你抓取。");
  }
  if (facts.mirrorConfigured) {
    risks.push_back(L"仓库里有 remote." + facts.pushRemoteName +
                    L".mirror：不中和它的话，带 refspec 的 push 会被 Git 直接拒绝（实测 128）。"
                    L"这次只对这个子进程临时置为 false，配置文件一个字不写。");
  }
  if (facts.tagOptConfigured || facts.followTagsConfigured) {
    risks.push_back(L"仓库里有会让推送顺带带上标签的设置（remote." + facts.pushRemoteName +
                    L".tagOpt / push.followTags）：实测带显式 refspec 时它们并不生效，"
                    L"本程序仍然为这一个子进程把它们关掉——绝不多推一个标签。");
  }
  plan.requiresForce = !risks.empty();
  plan.risks = std::move(risks);

  // ---- 不要求点头、但必须交代的事 ----
  std::vector<std::wstring> notes;
  const std::wstring pushDefaultDisplay =
      facts.pushDefault.empty() ? std::wstring(L"没设，Git 的默认是 simple") : facts.pushDefault;
  notes.push_back(L"命令里写的是完整两侧的 refspec " + plan.localBranchRef + L":" +
                  plan.remoteBranchRef + L"：推的就是这一个引用。push.default（现在是 " +
                  pushDefaultDisplay +
                  L"）与 remote.<远端>.push 都不参与，也不会顺带推标签或所有分支。");
  if (facts.extraPushRefspecsConfigured) {
    notes.push_back(L"remote." + facts.pushRemoteName +
                    L".push 配了额外的推送 refspec，本程序不中和它也不使用它：实测显式 refspec 之下"
                    L"它不会被附上（测试里钉着这一条）。");
  }
  if (!facts.pushUrlNote.empty()) {
    notes.push_back(facts.pushUrlNote);
  }
  if (facts.workflowProbed && facts.workflow.HasSpecialFlowInProgress()) {
    notes.push_back(L"顺带说明：这个仓库里还有一次 Git 流程没走完（" +
                    facts.workflow.SpecialFlowText() +
                    L"）。推送只送已有的提交，不会把那种现场搅进去，也不碰索引与工作区。");
  }
  plan.notes = std::move(notes);

  plan.notice =
      L"推送范围：只把 " + plan.localBranchRef + L" 送到远端「" + plan.remoteName + L"」的 " +
      plan.remoteBranchRef + L"（发布目标：" + plan.remoteUrlDisplay +
      L"）。不带 --force / --force-with-lease / --mirror / --all / --tags，不递归子模块，"
      L"不推标签，不动本地分支、索引与工作区，也不写任何配置文件。";

  std::wstring text;
  text += L"这次推送的三样东西：\n";
  text += L" · 源分支：" + plan.localBranchRef + L"（现在在 " + ShortObjectId(plan.pushedObjectId) +
          L"，完整 ID " + plan.pushedObjectId + L"）\n";
  text += L" · 目标远端：" + plan.remoteName + L"（来历：" + plan.remoteNameSource + L"）\n";
  text += L" · 目标分支：" + plan.remoteBranchRef + L"\n";
  text += PushTargetSentence(facts);
  text += L" · " + UpstreamSentence(facts);
  text += L"\n将在命令窗口里执行：" + plan.commandLabel + L"\n";
  if (!repositoryDirectory.empty()) {
    text += L"（工作目录：" + std::wstring(repositoryDirectory) + L"）\n";
  }
  text += L"\n" + RelationshipSentence(facts);
  text += NeutralizeSentence(facts);
  if (!facts.pushDefault.empty()) {
    text += L" · push.default = " + facts.pushDefault +
            L"：本次带了完整 refspec，它不参与（实测：设为 nothing 时显式 refspec 照旧生效）。\n";
  }
  text += L"\n命令里没有 --force，也没有 --force-with-lease / --mirror / --all / --tags：Git 认为"
          L"这次不是快进时就会把它拒绝，本程序不会为了让它“成功”而更激烈。\n";
  text += L"没有 --no-verify：pre-push hook 与签名设置照常生效；需要口令时由命令窗口里 Git 自己提问，"
          L"沿用你已有的认证方式（本程序不经手、也不显示任何凭据）。\n";
  if (!plan.risks.empty()) {
    text += L"\n风险提示（本程序不认为这些可以忽略）：\n";
    for (size_t index = 0; index < plan.risks.size(); ++index) {
      text += L" " + std::to_wstring(index + 1) + L") " + plan.risks[index] + L"\n";
    }
  } else {
    text += L"\n预检没有发现问题：上游明确、发布目标与抓取目标是同一个地方、本地只领先不落后。\n";
  }
  if (!plan.notes.empty()) {
    text += L"\n还要交代的：\n";
    for (const std::wstring& note : plan.notes) {
      text += L" · " + note + L"\n";
    }
  }
  text += L"\n预检不是保证：点头之后到 Git 真正跑完之间，远端可能又被别人推进（那正是 non-fast-forward"
          L" 的来源）、hooks 可能拒绝、认证可能失败。失败时命令窗口里留着 Git 的真实输出，"
          L"本程序不重试、不减弱、不撤销。\n";
  text += L"命令窗口报告结束后，本程序还会向**上面列出的发布目标**发一次只读 ls-remote，"
          L"核对那条引用到底停在哪——推送成功与否以那一份实况为准，不是以本地某个引用看起来一致为准。"
          L"取消这一步：不打开命令窗口、不接触远端、不改动仓库。";
  plan.confirmationText = std::move(text);
  plan.explanation = L"推送目标：" + plan.localBranchRef + L" → 远端「" + plan.remoteName + L"」的 " +
                     plan.remoteBranchRef + L"（" + plan.remoteUrlDisplay + L"）";
  return plan;
}

// ---- 执行前复核 ----

namespace {

std::wstring DescribeObjectOr(std::wstring_view objectId, std::wstring_view fallback) {
  return objectId.empty() ? std::wstring(fallback) : ShortObjectId(objectId);
}

}  // namespace

std::wstring DescribePushChange(const PushPreflightFacts& preflight, const PushPreflightFacts& latest) {
  if (!latest.queryOk) {
    return L"执行前的复核没能完成：" +
           (latest.queryFailure.empty() ? std::wstring(L"原因未知") : latest.queryFailure) +
           L"。看不清要推的是哪一份提交就不推，本次没有发出任何命令。";
  }
  if (!latest.configOk) {
    return L"执行前的复核没能问回这个仓库的生效配置，发布目标无从确认。本次没有发出任何命令。";
  }

  std::vector<std::wstring> changes;
  if (latest.branchRef != preflight.branchRef) {
    changes.push_back(L"当前分支从「" +
                      (preflight.branchRef.empty() ? std::wstring(L"不在分支上") : preflight.branchRef) +
                      L"」变成了「" +
                      (latest.branchRef.empty() ? std::wstring(L"不在分支上") : latest.branchRef) + L"」");
  }
  if (latest.headObjectId != preflight.headObjectId) {
    changes.push_back(L"要推送的提交从 " +
                      DescribeObjectOr(preflight.headObjectId, L"（读不到）") + L" 变成了 " +
                      DescribeObjectOr(latest.headObjectId, L"（读不到）") +
                      L"（点头之后本地又提交过、或分支被外部挪过）");
  }
  if (latest.upstreamTrackingRef != preflight.upstreamTrackingRef ||
      latest.upstreamRemoteRef != preflight.upstreamRemoteRef ||
      latest.upstreamRemote != preflight.upstreamRemote) {
    changes.push_back(L"上游配置也变了：原来是「" + preflight.upstreamRemote + L" / " +
                      preflight.upstreamTrackingRef + L" / " + preflight.upstreamRemoteRef +
                      L"」，现在是「" + latest.upstreamRemote + L" / " + latest.upstreamTrackingRef +
                      L" / " + latest.upstreamRemoteRef + L"」");
  }
  if (latest.pushRemoteName != preflight.pushRemoteName) {
    changes.push_back(L"发布远端从「" +
                      (preflight.pushRemoteName.empty() ? std::wstring(L"定不出来")
                                                        : preflight.pushRemoteName) +
                      L"」变成了「" +
                      (latest.pushRemoteName.empty() ? std::wstring(L"定不出来") : latest.pushRemoteName) +
                      L"」");
  }
  if (latest.pushUrls != preflight.pushUrls) {
    changes.push_back(L"发布 URL 也不再是刚刚那一批：\n  预检时：" +
                      FormatPushUrlList(preflight.pushUrls) + L"\n  现在：" +
                      FormatPushUrlList(latest.pushUrls));
  }
  // 会被中和的设置变了也要重看：命令的形态（多不带哪几句 -c）取决于它此刻在不在。
  if (latest.mirrorConfigured != preflight.mirrorConfigured ||
      latest.tagOptConfigured != preflight.tagOptConfigured ||
      latest.followTagsConfigured != preflight.followTagsConfigured ||
      latest.extraPushRefspecsConfigured != preflight.extraPushRefspecsConfigured) {
    changes.push_back(L"仓库里影响推送范围的配置与预检时不一样了（mirror / tagOpt / push.followTags / "
                      L"remote.<远端>.push 至少有一项变了）");
  }
  if (latest.trackingObjectId != preflight.trackingObjectId) {
    changes.push_back(L"远端跟踪引用 " + preflight.upstreamTrackingRef + L" 的位置从 " +
                      DescribeObjectOr(preflight.trackingObjectId, L"（本地没有这个引用）") + L" 变成了 " +
                      DescribeObjectOr(latest.trackingObjectId, L"（本地没有这个引用）") +
                      L"（预检之后又有过抓取，或引用被外部改动过）");
  }
  if (changes.empty()) {
    return std::wstring();
  }
  std::wstring text = L"点头之后、执行之前，仓库与配置又变了：\n";
  for (const std::wstring& change : changes) {
    text += L" · " + change + L"\n";
  }
  text += L"本次没有发出任何命令：预检得出的那份方案已经不属于此刻的这个仓库，照着它执行就是"
          L"拿旧结论推新东西。工作区与索引的改动不在这次的比对范围里（推送本来就不碰它们）。"
          L"仓库状态正在重读，看清现状后如仍要推送请再点一次。";
  return text;
}

// ---- 推送之后：向发布目标核对 ----

PushTargetCheck InterpretPushTargetCheck(std::wstring_view url, const GitQueryResult& lsRemoteResult,
                                         std::wstring_view remoteBranchRef) {
  PushTargetCheck check;
  check.url = std::wstring(url);
  check.queried = lsRemoteResult.started;
  if (!check.queried) {
    check.failure = L"核对进程未能启动";
    return check;
  }
  const UndoQueryRead read = ReadUndoQuery(lsRemoteResult);
  if (read.outcome != UndoQueryOutcome::answered) {
    check.failure = RefusalWith(read, L"git ls-remote 未成功");
    return check;
  }
  check.ok = true;
  // 每行「<对象 ID><TAB><引用名>」。ls-remote 的模式匹配是宽松的（可能顺带回别的相关引用），
  // 因此只认引用名完全等于目标的那一行，并且取 Git 给出的第一行。
  for (const std::wstring& line : read.lines) {
    const size_t tab = line.find(L'\t');
    if (tab == std::wstring::npos) {
      continue;
    }
    if (TrimWide(std::wstring_view(line).substr(tab + 1)) != remoteBranchRef) {
      continue;
    }
    const std::wstring objectId = TrimWide(std::wstring_view(line).substr(0, tab));
    if (!LooksLikeFullObjectId(objectId)) {
      check.failure = L"对端给出的对象 ID 形态不合格：" + objectId;
      return check;
    }
    check.refPresent = true;
    check.remoteObjectId = objectId;
    return check;
  }
  return check;  // 问成功了，但对端没有那条引用。
}

std::wstring_view PushVerificationVerdictLabel(PushVerificationVerdict verdict) noexcept {
  switch (verdict) {
    case PushVerificationVerdict::nothingChecked:
      return L"未能核实";
    case PushVerificationVerdict::confirmed:
      return L"已核实";
    case PushVerificationVerdict::mismatched:
      return L"与预期不符";
    case PushVerificationVerdict::mixed:
      return L"部分核实";
  }
  return L"未能核实";
}

PushVerificationReport ComposePushVerification(const std::vector<PushTargetCheck>& checks,
                                               std::wstring_view expectedObjectId,
                                               std::wstring_view remoteBranchRef,
                                               bool commandSucceeded,
                                               std::wstring_view commandConclusion) {
  PushVerificationReport report;
  size_t confirmed = 0;
  size_t mismatched = 0;
  size_t unanswered = 0;
  const std::wstring expected = LooksLikeFullObjectId(expectedObjectId)
                                    ? std::wstring(expectedObjectId)
                                    : std::wstring();

  for (const PushTargetCheck& check : checks) {
    std::wstring line = L"· 发布目标 " + MaskPushUrlCredentials(check.url) + L" 的 " +
                        std::wstring(remoteBranchRef.empty() ? std::wstring(L"（目标引用未记录）")
                                                             : remoteBranchRef) +
                        L"：";
    if (!check.queried) {
      ++unanswered;
      line += L"没能问（" + (check.failure.empty() ? std::wstring(L"核对没有执行") : check.failure) +
              L"）";
    } else if (!check.ok) {
      ++unanswered;
      line += L"没能问到（" + (check.failure.empty() ? std::wstring(L"原因未知") : check.failure) + L"）";
    } else if (!check.refPresent) {
      ++mismatched;
      line += L"对端没有这条引用";
    } else if (!expected.empty() && check.remoteObjectId == expected) {
      ++confirmed;
      line += L"现在在 " + ShortObjectId(check.remoteObjectId) +
              L"，正是这次推出去的那一份（已核实）";
    } else {
      ++mismatched;
      line += L"现在在 " + ShortObjectId(check.remoteObjectId) + L"，不是这次推出去的那一份" +
              (expected.empty() ? std::wstring() : std::wstring(L"（" + ShortObjectId(expected) + L"）"));
    }
    report.lines.push_back(std::move(line));
  }

  if (checks.empty()) {
    report.verdict = PushVerificationVerdict::nothingChecked;
    report.headline =
        L"没有向发布目标发出核对" +
        (commandSucceeded
             ? std::wstring(L"：这次推送的结论只能以命令窗口里 Git 的输出为准，本程序不替它背书。")
             : std::wstring(L"。") + std::wstring(commandConclusion));
    return report;
  }
  if (confirmed > 0 && mismatched > 0) {
    report.verdict = PushVerificationVerdict::mismatched;
  } else if (confirmed > 0 && unanswered > 0) {
    report.verdict = PushVerificationVerdict::mixed;
  } else if (confirmed > 0) {
    report.verdict = PushVerificationVerdict::confirmed;
  } else if (mismatched > 0) {
    report.verdict = PushVerificationVerdict::mismatched;
  } else {
    report.verdict = PushVerificationVerdict::nothingChecked;
  }

  const std::wstring_view verdictLabel = PushVerificationVerdictLabel(report.verdict);
  switch (report.verdict) {
    case PushVerificationVerdict::confirmed:
      report.headline = commandSucceeded
                            ? std::wstring(L"推送结果（") + std::wstring(verdictLabel) +
                                  L"）：命令窗口报告成功，" + std::to_wstring(confirmed) +
                                  L" 个发布目标上的那条引用也都已经停在这一次推出去的那一份提交上。"
                            : std::wstring(L"推送结果（") + std::wstring(verdictLabel) +
                                  L"）：命令窗口那头的结论是「" + std::wstring(commandConclusion) +
                                  L"」，但向发布目标核对到的实况是那条引用已经在这一份提交上——"
                                  L"以实况为准。";
      break;
    case PushVerificationVerdict::mismatched:
      report.headline =
          commandSucceeded
              ? std::wstring(L"推送结果（") + std::wstring(verdictLabel) +
                    L"）：命令窗口里 Git 报告成功（退出码 0），但按下面的核对，"
                    L"至少一个发布目标上那条引用并不是这次推出去的那一份——本程序不把它算成推送成功。"
              : std::wstring(L"推送结果（") + std::wstring(verdictLabel) + L"）：命令窗口那头的结论是「" +
                    std::wstring(commandConclusion) + L"」，向发布目标核对的实况如下。";
      break;
    case PushVerificationVerdict::mixed:
      report.headline = std::wstring(L"推送结果（") + std::wstring(verdictLabel) +
                        L"）：" + std::to_wstring(confirmed) +
                        L" 个发布目标已核上，另有 " + std::to_wstring(unanswered) +
                        L" 个没能问到（认证、网络或对端设置都可能是原因）。"
                        L"命令窗口那头的结论是「" +
                        std::wstring(commandConclusion) + L"」。逐目标见下面每一行。";
      break;
    case PushVerificationVerdict::nothingChecked:
    default:
      report.headline = std::wstring(L"推送结果（") + std::wstring(verdictLabel) +
                        L"）：向发布目标的核对一次也没成功，因此这条引用到底送没送到，"
                        L"只能以命令窗口里 Git 的真实输出为准（那头的结论是「" +
                        std::wstring(commandConclusion) + L"」）。本程序不拿「窗口已打开」"
                        L"或本地跟踪引用的位置当作推送成功。";
      break;
  }
  return report;
}

}  // namespace gc::git
