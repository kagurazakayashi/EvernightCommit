#include "git/pull_plan.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "git/commit_history.h"
#include "git/workspace_status.h"

namespace gc::git {
namespace {

// 風險條目里一個文件清單最多列幾條；其餘折成「等 N 个文件」，確認框才不會被撐爆。
constexpr size_t kMaxRiskFileList = 8;

std::wstring LowerAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
  }
  return result;
}

// Git 的 git_parse_maybe_bool：空串与 NULL（裸键）都是 0=假；布尔家族大小写不敏感；
// 其余返回不认。pull.rebase / pull.ff / merge.ff 的原生解析全部以它为第一关，
// 「设成空值」与「没设」的含义差别就出在这里——空值是明确的「假」，不是没表态。
std::optional<bool> NativeMaybeBool(std::wstring_view value) {
  if (value.empty()) {
    return false;
  }
  const std::wstring text = LowerAscii(value);
  if (text == L"true" || text == L"yes" || text == L"1" || text == L"on") {
    return true;
  }
  if (text == L"false" || text == L"no" || text == L"0" || text == L"off") {
    return false;
  }
  return std::nullopt;
}

// --name-only -z 的输出：NUL 分隔的路径清单（路径原样，不做引号转义）。
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

// 把文件清单排成一行展示文字（超上限的折成「等 N 个文件」）。
std::wstring FormatFileList(const std::vector<std::wstring>& paths) {
  std::wstring text;
  const size_t shown = std::min(paths.size(), kMaxRiskFileList);
  for (size_t index = 0; index < shown; ++index) {
    if (!text.empty()) {
      text += L"、";
    }
    text += paths[index];
  }
  if (paths.size() > shown) {
    text += L" 等 " + std::to_wstring(paths.size()) + L" 个文件";
  }
  return text;
}

// 本地未提交的改动（含未跟踪文件）与「这次要整合进来的路径」的重叠。
// 两侧都是仓库相对路径（porcelain 与 --name-only 都不带前导 ./、正斜杠分隔），按原样比较。
struct WorktreeOverlap {
  std::vector<std::wstring> dirtyTracked;  // 已跟踪文件有未提交改动，又要被这次整合改写
  std::vector<std::wstring> untracked;     // 未跟踪文件与远端带来的同名路径撞上
};

WorktreeOverlap CollectIncomingOverlap(const WorkspaceModel& model,
                                       const std::vector<std::wstring>& incomingPaths) {
  WorktreeOverlap overlap;
  if (incomingPaths.empty()) {
    return overlap;
  }
  const std::unordered_set<std::wstring> incoming(incomingPaths.begin(), incomingPaths.end());
  std::unordered_set<std::wstring> seen;
  const auto consider = [&](const ChangeItem& item, bool untrackedItem) {
    const auto matches = [&](std::wstring_view path) {
      if (path.empty() || incoming.find(std::wstring(path)) == incoming.end()) {
        return;
      }
      if (seen.insert(std::wstring(path)).second) {
        (untrackedItem ? overlap.untracked : overlap.dirtyTracked).push_back(std::wstring(path));
      }
    };
    matches(item.path);
    matches(item.oldPath);  // 重命名/复制：旧路径同样占着工作区里的那个名字
  };
  for (const ChangeItem& item : model.unstaged) {
    consider(item, item.kind == ChangeKind::untracked);
  }
  for (const ChangeItem& item : model.staged) {
    consider(item, false);
  }
  return overlap;
}

std::wstring RefusalWith(const UndoQueryRead& read, std::wstring_view fallback) {
  return read.detail.empty() ? std::wstring(fallback) : read.detail;
}

// 按制表符拆三栏（缺栏返回空串）。
std::vector<std::wstring> SplitTabs(std::wstring_view line, size_t expected) {
  std::vector<std::wstring> fields;
  std::wstring current;
  for (const wchar_t c : line) {
    if (c == L'\t' && fields.size() + 1 < expected) {
      fields.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  fields.push_back(current);
  while (fields.size() < expected) {
    fields.emplace_back();
  }
  for (std::wstring& field : fields) {
    field = TrimWide(field);
  }
  return fields;
}

// 两条 rebase 类配置（branch.<分支名>.rebase、pull.rebase）按原生规矩的判读结果。
// 语义依据是 Git 2.53 的 rebase.c:rebase_parse_value() 与 pull.c:config_get_rebase()，
// 本机 Git 2.53.0.windows.3 实测核对（见 tests 与本文件头注释）：
//   * 布尔家族先判（大小写不敏感）：真→变基；假、空值、裸键→合并；
//   * 布尔不认时只认**严格小写**的 merges/m（原生翻成 --rebase-merges）与
//     interactive/i（原生执行 git rebase -i）；"MERGES"、"I" 在这一步都是无效值；
//   * preserve/p 原生提示「已并入 merges」之后仍按无效值 die；
//   * 其余一律无效——原生 git pull 在接触远端之前就 fatal（实测），所以这里同样拒绝，
//     绝不降级成「当作没设」或「普通变基」。
enum class PullRebaseConfigValue {
  absent = 0,    // Git 明确回答没设（退出码 1）
  merge,         // 布尔假家族 / 空值 / 裸键：原生按「合并」处理
  rebase,        // 布尔真家族
  rebaseMerges,  // merges / m：变基并保留本地合并结构
  interactive,   // interactive / i：交互式变基（本程序不执行）
  invalid,       // 其余取值：原生 pull 当场拒绝
};

PullRebaseConfigValue ParseRebaseConfigValue(std::wstring_view value) {
  if (const std::optional<bool> boolean = NativeMaybeBool(value)) {
    return *boolean ? PullRebaseConfigValue::rebase : PullRebaseConfigValue::merge;
  }
  if (value == L"merges" || value == L"m") {
    return PullRebaseConfigValue::rebaseMerges;
  }
  if (value == L"interactive" || value == L"i") {
    return PullRebaseConfigValue::interactive;
  }
  return PullRebaseConfigValue::invalid;
}

// 配置取值的展示形态：空值要说明它在原生布尔语义里就是「假」，不能显示成看起来没内容。
std::wstring ConfigValueDisplay(std::wstring_view raw) {
  if (raw.empty()) {
    return L"（空值：原生布尔语义按「假」处理）";
  }
  return std::wstring(raw);
}

std::wstring RebaseConfigMeaning(PullRebaseConfigValue value) {
  switch (value) {
    case PullRebaseConfigValue::merge:
      return L"原生按「合并」处理";
    case PullRebaseConfigValue::rebase:
      return L"原生按变基处理（线性重放）";
    case PullRebaseConfigValue::rebaseMerges:
      return L"原生按变基处理并保留合并结构（--rebase-merges），本程序照此写进命令行";
    case PullRebaseConfigValue::interactive:
      return L"原生要执行交互式变基（git rebase -i，todo 清单由编辑器当场决定）";
    case PullRebaseConfigValue::invalid:
      return L"原生 git pull 会在解析配置时就拒绝（invalid value），本程序同样拒绝整合";
    case PullRebaseConfigValue::absent:
      break;
  }
  return L"没有这条配置";
}

// 定策略的那一条配置。优先级照原生：branch.<分支名>.rebase **存在**就单独说了算
// ——哪怕它是无效值（实测：branch 无效而 pull=true 时，fatal 点名的是 branch 键），
// 不存在才轮到 pull.rebase；两条都没有才是「配置没表态」。
struct PullStrategyConfig {
  PullRebaseConfigValue value = PullRebaseConfigValue::absent;
  std::wstring key;     // 定这条结论的配置项原文（branch.main.rebase / pull.rebase）
  std::wstring raw;     // Git 回答的原始取值（absent 时为空）
  std::wstring source;  // 展示句：哪条配置、什么值、原生怎么理解
};

std::wstring BranchRebaseKey(std::wstring_view branchName) {
  return L"branch." + (branchName.empty() ? std::wstring(L"〈分支名〉") : std::wstring(branchName)) +
         L".rebase";
}

PullStrategyConfig ResolveStrategyConfig(const PullTargetFacts& target) {
  PullStrategyConfig cfg;
  const auto fill = [&cfg](std::wstring key, std::wstring raw) {
    cfg.key = std::move(key);
    cfg.raw = std::move(raw);
    cfg.value = ParseRebaseConfigValue(cfg.raw);
    cfg.source = cfg.key + L" = " + ConfigValueDisplay(cfg.raw) + L"（" +
                 RebaseConfigMeaning(cfg.value) + L"）";
  };
  if (target.configBranchRebasePresent) {
    fill(BranchRebaseKey(target.branchName), target.configBranchRebase);
    return cfg;
  }
  if (target.configPullRebasePresent) {
    fill(L"pull.rebase", target.configPullRebase);
    return cfg;
  }
  cfg.source = L"branch.〈分支〉.rebase 与 pull.rebase 都没设（原生 git pull 在这种情况下按合并走，"
               L"分叉时还会先警告再拒绝，要用户当场定）";
  return cfg;
}

// pull.ff / merge.ff 按原生规矩的判读结果。依据 pull.c:config_get_ff() 与
// merge.c 对 merge.ff 的处理（本机实测核对）：
//   * pull.ff：布尔真→--ff（等于默认）；布尔假/空值→--no-ff；严格小写 only→--ff-only；
//     其余值原生 pull 在联网前就 die；
//   * merge.ff：同一套布尔+only，但**看不懂的取值被原生 merge 明确忽略**（源码注释
//     "do not barf on values from future versions"），按默认处理——与 pull.ff 的
//     「die」是不同的原生行为，判读必须分开；
//   * pull.ff 存在时说了算；它不参与的地方（变基路线根本不会执行 merge）merge.ff 也无从生效。
enum class PullFfConfigValue {
  absent = 0,  // 没有这条配置（或 merge.ff 设了个被原生忽略的值——行为等同没设）
  allow,       // 布尔真家族：能快进就快进（默认）
  never,       // 布尔假家族 / 空值：绝不快进，可快进也要产生合并提交
  only,        // only：只在可快进时整合，否则拒绝
  invalid,     // pull.ff 的其余取值：原生 pull 当场拒绝
};

// mergeFf 为 true 时按 merge.ff 的「忽略不懂值」规矩，false 时按 pull.ff 的「die」规矩。
std::pair<PullFfConfigValue, bool> ParseFfConfigValue(std::wstring_view value, bool mergeFf) {
  if (const std::optional<bool> boolean = NativeMaybeBool(value)) {
    return {*boolean ? PullFfConfigValue::allow : PullFfConfigValue::never, true};
  }
  if (value == L"only") {
    return {PullFfConfigValue::only, true};
  }
  return {mergeFf ? PullFfConfigValue::absent : PullFfConfigValue::invalid, false};
}

std::wstring FfConfigMeaning(PullFfConfigValue value, bool mergeFf, bool understood) {
  if (!understood && mergeFf) {
    return L"原生 merge 会忽略看不懂的取值，按默认（能快进就快进）处理";
  }
  switch (value) {
    case PullFfConfigValue::allow:
      return L"原生按「能快进就快进」处理（与默认一致）";
    case PullFfConfigValue::never:
      return L"原生按「绝不快进」处理：可快进也要产生合并提交";
    case PullFfConfigValue::only:
      return L"原生按「只在可快进时整合」处理，分叉时直接拒绝";
    case PullFfConfigValue::invalid:
      return L"原生 git pull 会在解析配置时就拒绝（invalid value），本程序同样拒绝整合";
    case PullFfConfigValue::absent:
      break;
  }
  return L"没有这条配置";
}

struct PullFfConfig {
  PullFfConfigValue value = PullFfConfigValue::absent;
  bool fromPullFf = false;  // 只有出自 pull.ff 的 only 才参与原生「ff-only 优先于策略」规则
  std::wstring key;
  std::wstring raw;
  std::wstring source;
};

PullFfConfig ResolveFfConfig(const PullTargetFacts& target) {
  PullFfConfig cfg;
  const auto fill = [&cfg](std::wstring key, std::wstring raw, bool pullFf) {
    cfg.key = std::move(key);
    cfg.raw = std::move(raw);
    cfg.fromPullFf = pullFf;
    const auto [value, understood] = ParseFfConfigValue(cfg.raw, !pullFf);
    cfg.value = value;
    cfg.source = cfg.key + L" = " + ConfigValueDisplay(cfg.raw) + L"（" +
                 FfConfigMeaning(cfg.value, !pullFf, understood) + L"）";
  };
  if (target.configPullFfPresent) {
    fill(L"pull.ff", target.configPullFf, true);
    return cfg;
  }
  if (target.configMergeFfPresent) {
    fill(L"merge.ff", target.configMergeFf, false);
    return cfg;
  }
  cfg.source = L"pull.ff / merge.ff 都没设（默认：能快进就快进）";
  return cfg;
}

// 两个阶段共用的前提核对。返回空串表示前提成立。
std::wstring PullPrerequisiteRefusal(const PullTargetFacts& target,
                                     std::wstring_view repositoryDirectory) {
  if (repositoryDirectory.empty()) {
    return L"没有可用的仓库工作区根目录，无法确定这次 pull 属于哪个仓库。请先点“刷新”。";
  }
  if (!target.queryOk) {
    return target.queryFailure.empty() ? std::wstring(L"pull 前的只读查询没能完成，无法确定分支与上游。")
                                       : target.queryFailure;
  }
  if (!target.onBranch) {
    return L"当前处于游离 HEAD（不在任何分支上）。pull 要做的可是「把这个分支的上游整合进来」，"
           L"游离状态下没有分支、也没有分支级配置可依据。本程序不替游离 HEAD 猜一个分支，也不替你"
           L"checkout。请先切回一个分支，再点“刷新”后重来。";
  }
  if (!target.headQueried) {
    return L"没能问出 HEAD 指向哪个提交，本程序不在问不出目标的前提下动仓库。";
  }
  if (!target.headResolved) {
    return L"这个分支还没有任何提交（HEAD 还不可解析）。本程序的 pull 要核对「本地与远端各有几个独有"
           L"提交」「整合会不会盖掉你还没提交的东西」，那种仓库里问不出可靠答案。"
           L"要用远端内容建立第一条提交，请在自己的 Git 命令里完成（本程序不代跑）。";
  }
  if (!target.upstreamRan) {
    return L"没能完成上游配置的查询，无法确定这次 pull 的对象。请点“刷新”后重试。";
  }
  if (!target.upstreamConfigured) {
    if (!target.upstreamRemote.empty()) {
      return L"当前分支的上游配置不完整：远端是「" + target.upstreamRemote +
             L"」，可 branch." + target.branchName +
             L".merge 没有对应的分支记录。没有可确定的整合对象，本程序不猜哪一条远端分支。";
    }
    return L"当前分支 " + target.branchName + L" 没有设置上游分支（branch." + target.branchName +
           L".remote / .merge 未配置）。没有上游就不知道“从哪个远端整合哪一条分支”，本程序不猜 "
           L"origin/main，也不替你 git branch --set-upstream-to、不改任何配置。"
           L"请先用 Git 把上游设好（例如 git branch --set-upstream-to=origin/" + target.branchName +
           L"），再回来点 pull。";
  }
  if (target.workflow.HasSpecialFlowInProgress()) {
    return L"这个仓库里有 Git 流程还没走完：" + target.workflow.SpecialFlowText() +
           L"。那种状态下索引、HEAD 与合并/变基的进度记录正被那套流程使用，再叠一次 pull 会把现场"
           L"搅成一团乱麻。请先把原来的流程走完（git merge --continue / --abort、"
           L"git rebase --continue / --abort 等），再点“刷新”。这种阻止不是本程序的保守策略，"
           L"Git 自己就先不接受，加任何参数都绕不过去。";
  }
  if (!target.statusRan || !target.statusOk) {
    return L"没能读到工作区/索引现状（" +
           (target.statusDetail.empty() ? std::wstring(L"原因未知") : target.statusDetail) +
           L"）。看不见现状就谈不上判断“这次整合会不会盖掉你还没提交的东西”，"
           L"本程序不在看不懂的现状上动索引与工作区。请点“刷新”后重试。";
  }
  if (target.model.HasConflicts()) {
    return L"还有没解决的冲突条目（在“未暂存的更改”里）。冲突状态下的索引与 HEAD 不是一次普通提交的"
           L"产物，再整合一次远端只会让冲突叠冲突。本程序不替你解决冲突，也不会 abort/reset 掉现有现场。"
           L"请先处理这些冲突，再点“刷新”。";
  }
  return std::wstring();
}

std::wstring TrackingRefSentence(const PullTargetFacts& target) {
  std::wstring text = L"远端跟踪引用：" + target.upstreamTrackingRef;
  if (target.trackingResolved) {
    text += L"（现在停在 " + ShortObjectId(target.trackingObjectId) +
            L"；这个位置只是上一次 fetch 读回来的，不代表远端的最新状态）\n";
  } else if (target.trackingObjectRan) {
    text += L"（本地还没有这个引用，要靠在命令窗口里那次 fetch 带回来）\n";
  } else {
    text += L"（位置未知）\n";
  }
  return text;
}

std::wstring ConfigSentence(const PullTargetFacts& target) {
  return L"策略配置：" + ResolveStrategyConfig(target).source + L"\n" +
         L"快进配置：" + ResolveFfConfig(target).source + L"\n";
}

std::wstring RemoteBranchDisplay(const PullTargetFacts& target) {
  return target.upstreamRemoteBranch.empty() ? std::wstring(L"（远端那边的分支名未记录）")
                                             : target.upstreamRemoteBranch;
}

}  // namespace

// ---- 查询参数 ----

std::vector<std::wstring> BuildPullSymbolicRefArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"symbolic-ref", L"--quiet", L"HEAD"};
}

std::vector<std::wstring> BuildPullHeadObjectArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   L"HEAD"};
}

std::vector<std::wstring> BuildPullUpstreamArguments(std::wstring_view repositoryDirectory,
                                                     std::wstring_view branchName) {
  if (branchName.empty()) {
    return {};  // 不在分支上就没有分支记录可问上游；调用方据此跳过这条查询。
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"for-each-ref",
                                   L"--format=%(upstream:remotename)\t%(upstream)\t%(upstream:remoteref)",
                                   L"refs/heads/" + std::wstring(branchName)};
}

std::vector<std::wstring> BuildPullTrackingObjectArguments(std::wstring_view repositoryDirectory,
                                                           std::wstring_view trackingRef) {
  if (trackingRef.empty()) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-parse", L"--verify", L"--quiet",
                                   std::wstring(trackingRef)};
}

std::vector<std::wstring> BuildPullConfigArguments(std::wstring_view repositoryDirectory,
                                                   std::wstring_view key) {
  if (key.empty()) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"config", L"--get", std::wstring(key)};
}

std::vector<std::wstring> BuildPullMergeEquivalenceArguments(std::wstring_view repositoryDirectory) {
  // 记录约定与 git/fetch_scope 的两条 --null --get-regexp 完全一致（键<换行>值<NUL>，
  // 无命中=退出码 1+空输出）。只取键名做判据，取值不留在事实里（driver 命令行可能含路径等噪声）。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"config", L"--null", L"--get-regexp",
                                   L"^(merge\\..*\\.driver|pull\\.(twohead|octopus))$"};
}

std::vector<std::wstring> BuildPullAheadBehindArguments(std::wstring_view repositoryDirectory,
                                                        std::wstring_view headObjectId,
                                                        std::wstring_view trackingObjectId) {
  if (!LooksLikeFullObjectId(headObjectId) || !LooksLikeFullObjectId(trackingObjectId)) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-list", L"--left-right", L"--count",
                                   std::wstring(headObjectId) + L"..." + std::wstring(trackingObjectId)};
}

std::vector<std::wstring> BuildPullMergeBaseArguments(std::wstring_view repositoryDirectory,
                                                      std::wstring_view headObjectId,
                                                      std::wstring_view trackingObjectId) {
  if (!LooksLikeFullObjectId(headObjectId) || !LooksLikeFullObjectId(trackingObjectId)) {
    return {};
  }
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"merge-base", std::wstring(headObjectId),
                                   std::wstring(trackingObjectId)};
}

std::vector<std::wstring> BuildPullIncomingArguments(std::wstring_view repositoryDirectory,
                                                     std::wstring_view baseObjectId,
                                                     std::wstring_view targetObjectId) {
  if (!LooksLikeFullObjectId(baseObjectId) || !LooksLikeFullObjectId(targetObjectId)) {
    return {};
  }
  // -z：路径以 NUL 结尾、不做引号转义，含空格/中文/&/% 的路径能与 porcelain 里的写法原样相比。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"diff", L"--name-only", L"-z",
                                   std::wstring(baseObjectId), std::wstring(targetObjectId)};
}

std::vector<std::wstring> BuildPullMergeTreeArguments(std::wstring_view repositoryDirectory,
                                                      std::wstring_view headObjectId,
                                                      std::wstring_view trackingObjectId) {
  if (!LooksLikeFullObjectId(headObjectId) || !LooksLikeFullObjectId(trackingObjectId)) {
    return {};
  }
  // 三方合并预演：不改真实工作区与索引，但**会向对象库写入结果对象**（不可达，由 gc 回收）——
  // 所以它不是「完全不写仓库」。--name-only 让冲突清单一行一个路径。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"merge-tree", L"--write-tree",
                                   L"--name-only", std::wstring(headObjectId),
                                   std::wstring(trackingObjectId)};
}

std::vector<std::wstring> BuildPullLocalMergeCountArguments(std::wstring_view repositoryDirectory,
                                                            std::wstring_view baseObjectId,
                                                            std::wstring_view headObjectId) {
  if (!LooksLikeFullObjectId(baseObjectId) || !LooksLikeFullObjectId(headObjectId)) {
    return {};
  }
  // 「本地独有的提交里有几个合并提交」：普通变基会把这些合并压平，确认框要报出实数。
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"rev-list", L"--count", L"--merges",
                                   std::wstring(baseObjectId) + L".." + std::wstring(headObjectId)};
}

std::vector<std::wstring> BuildPullConflictListingArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"--no-replace-objects", L"diff", L"--name-only", L"-z",
                                   L"--diff-filter=U"};
}

bool PullBranchNameFromRef(std::wstring_view symbolicRefOutput, std::wstring* branchName) {
  constexpr std::wstring_view kPrefix = L"refs/heads/";
  const std::wstring ref = TrimWide(symbolicRefOutput);
  if (ref.size() <= kPrefix.size() || ref.compare(0, kPrefix.size(), kPrefix) != 0) {
    return false;
  }
  *branchName = std::wstring(ref.substr(kPrefix.size()));
  return true;
}

PullUpstreamInfo ParsePullUpstreamLine(std::wstring_view line) {
  PullUpstreamInfo info;
  const std::vector<std::wstring> fields = SplitTabs(line, 3);
  info.remote = fields[0];
  info.trackingRef = fields[1];
  info.remoteBranch = fields[2];
  // 三栏里最要紧的是「本地远端跟踪引用的全名」：没有它就连要整合什么都说不清。
  info.configured = !info.trackingRef.empty();
  return info;
}

// ---- 阶段一判读 ----

PullTargetFacts InterpretPullTarget(const PullTargetQueries& queries) {
  PullTargetFacts facts;
  // 抓取范围的配置先读回来：阶段一那条 fetch 的参数与承诺由 git/fetch_scope 按这份事实生成，
  // 与界面 fetch 按钮用的是同一个判读函数（两个入口不可能各说一套范围）。
  facts.scope = InterpretFetchScope(queries.scope);

  const UndoQueryRead symbolic = ReadUndoQuery(queries.symbolicRef);
  if (symbolic.outcome == UndoQueryOutcome::failed) {
    facts.queryFailure = L"没能问出当前分支：" + RefusalWith(symbolic, L"symbolic-ref 未成功");
    return facts;
  }
  if (symbolic.outcome == UndoQueryOutcome::answered) {
    facts.branchRef = symbolic.firstLine;
    facts.onBranch = PullBranchNameFromRef(facts.branchRef, &facts.branchName);
    if (!facts.onBranch) {
      // symbolic-ref 给出 refs/heads/ 以外的引用是合法但罕见的形态：没有分支名就拼不出
      // branch.〈名〉.rebase 这类键，也就没有「分支的上游」可问，按不在分支上处理。
      facts.branchRef.clear();
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
  // noResult：这个分支还没有提交。是明确答案，交给方案层拒绝，不算查询失败。

  if (facts.onBranch) {
    facts.upstreamRan = queries.upstreamRan;
    if (queries.upstreamRan) {
      const UndoQueryRead upstream = ReadUndoQuery(queries.upstream);
      if (upstream.outcome == UndoQueryOutcome::failed) {
        facts.queryFailure =
            L"没能问出分支 " + facts.branchName + L" 的上游配置：" + RefusalWith(upstream, L"for-each-ref 未成功");
        return facts;
      }
      if (upstream.outcome == UndoQueryOutcome::answered && !upstream.lines.empty()) {
        const PullUpstreamInfo info = ParsePullUpstreamLine(upstream.lines.front());
        facts.upstreamRemote = info.remote;
        facts.upstreamTrackingRef = info.trackingRef;
        facts.upstreamRemoteBranch = info.remoteBranch;
        facts.upstreamConfigured = info.configured;
      }
    }

    if (facts.upstreamConfigured) {
      facts.trackingObjectRan = queries.trackingObjectRan;
      if (queries.trackingObjectRan) {
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
    }

    if (!facts.branchName.empty() && queries.configBranchRebaseRan) {
      const UndoQueryRead branchRebase = ReadUndoQuery(queries.configBranchRebase);
      if (branchRebase.outcome == UndoQueryOutcome::failed) {
        facts.queryFailure = L"没能问出 " + BranchRebaseKey(facts.branchName) + L"：" +
                             RefusalWith(branchRebase, L"config 查询未成功");
        return facts;
      }
      if (branchRebase.outcome == UndoQueryOutcome::answered) {
        facts.configBranchRebase = branchRebase.firstLine;
        facts.configBranchRebasePresent = true;  // 存在就说了算——空值也是「设过、按假处理」。
      }
    }
  }

  struct ConfigRead {
    std::wstring_view key;
    const UndoQueryRead* read;
    std::wstring* field;
    bool* present;
  };
  const UndoQueryRead pullRebase = ReadUndoQuery(queries.configPullRebase);
  const UndoQueryRead pullFf = ReadUndoQuery(queries.configPullFf);
  const UndoQueryRead mergeFf = ReadUndoQuery(queries.configMergeFf);
  const ConfigRead configs[] = {
      {L"pull.rebase", &pullRebase, &facts.configPullRebase, &facts.configPullRebasePresent},
      {L"pull.ff", &pullFf, &facts.configPullFf, &facts.configPullFfPresent},
      {L"merge.ff", &mergeFf, &facts.configMergeFf, &facts.configMergeFfPresent},
  };
  for (const ConfigRead& config : configs) {
    if (config.read->outcome == UndoQueryOutcome::failed) {
      facts.queryFailure = L"没能问出 " + std::wstring(config.key) + L" 的配置：" +
                           RefusalWith(*config.read, L"config 查询未成功");
      return facts;
    }
    if (config.read->outcome == UndoQueryOutcome::answered) {
      *config.field = config.read->firstLine;
      *config.present = true;
    }
    // noResult：Git 以退出码 1 明确回答「没有这条配置」——那才是「未设置」；
    // answered + 空行是「设成了空值」，两回事（见 PullTargetFacts 的注释）。
  }

  // 合并等效性配置：merge.<名>.driver（外部合并程序）与 pull.twohead/pull.octopus（遗留策略）。
  // 命中任何一条、或这份清单没能完整读回，冲突预演的结论就降档为「强提示而非保证」；
  // 「没问」「读不全」都不许当成「没有」。
  if (queries.mergeEquivalenceRan) {
    const UndoQueryRead equivalence = ReadUndoQuery(queries.mergeEquivalence);
    if (equivalence.outcome == UndoQueryOutcome::noResult) {
      facts.mergeEquivalence = PullMergeEquivalenceProbe::none;
    } else if (equivalence.outcome == UndoQueryOutcome::answered) {
      std::wstring recordReason;
      if (!NulRecordsAreComplete(queries.mergeEquivalence.utf16Output, recordReason)) {
        facts.mergeEquivalence = PullMergeEquivalenceProbe::unknown;
      } else {
        bool trusted = true;
        std::vector<std::wstring> keys;
        for (const std::wstring& record : SplitNulList(queries.mergeEquivalence.utf16Output)) {
          // 记录形态与 fetch_scope 同一套约定：键<换行>值 或 键=值；裸键省略取值。
          const size_t newline = record.find(L'\n');
          const size_t equals = record.find(L'=');
          size_t end = std::wstring::npos;
          if (newline != std::wstring::npos && (equals == std::wstring::npos || newline < equals)) {
            end = newline;
          } else if (equals != std::wstring::npos) {
            end = equals;
          }
          std::wstring key =
              TrimWide(end == std::wstring::npos ? record : record.substr(0, end));
          if (key.compare(0, 6, L"merge.") != 0 && key.compare(0, 5, L"pull.") != 0) {
            trusted = false;  // 认不出键名的清单整份不采信，而不是丢掉可疑的那一条。
            break;
          }
          keys.push_back(std::move(key));
        }
        if (!trusted) {
          facts.mergeEquivalence = PullMergeEquivalenceProbe::unknown;
        } else {
          std::sort(keys.begin(), keys.end());
          keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
          facts.mergeEquivalenceKeys = std::move(keys);
          facts.mergeEquivalence = facts.mergeEquivalenceKeys.empty()
                                       ? PullMergeEquivalenceProbe::none
                                       : PullMergeEquivalenceProbe::some;
        }
      }
    } else {
      facts.mergeEquivalence = PullMergeEquivalenceProbe::unknown;
    }
  }

  facts.statusRan = queries.statusRan;
  facts.workflow = queries.workflow;
  facts.workflowProbed = queries.workflowProbed;
  if (queries.statusRan) {
    const UndoQueryRead status = ReadUndoQuery(queries.status);
    if (status.outcome != UndoQueryOutcome::answered) {
      facts.statusDetail = RefusalWith(status, L"git status 未成功");
    } else {
      WorkspaceStatusParseResult parsed = ParseWorkspacePorcelainV2(queries.status.utf16Output);
      if (!parsed.error.empty()) {
        facts.statusDetail = parsed.error;
      } else {
        facts.statusOk = true;
        facts.model = std::move(parsed.model);
      }
    }
  }

  facts.queryOk = true;
  return facts;
}

// ---- 阶段二判读 ----

// 兩個階段共用的行拆解工具：SplitWhitespace 服務下面這個計數解析，
// ParseAheadBehindCount 另外要給 push 用（同一個輸出約定，見 pull_plan.h 的宣告處）。

// 「12<TAB>3」这样一行按空白拆成 token。
std::vector<std::wstring> SplitWhitespace(std::wstring_view line) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (const wchar_t c : line) {
    if (c == L' ' || c == L'\t') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

bool ParseAheadBehindCount(std::wstring_view line, long long* ahead, long long* behind) {
  const std::vector<std::wstring> tokens = SplitWhitespace(line);
  if (tokens.size() != 2) {
    return false;
  }
  try {
    const long long left = std::stoll(tokens[0]);
    const long long right = std::stoll(tokens[1]);
    if (left < 0 || right < 0) {
      return false;
    }
    *ahead = left;
    *behind = right;
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

PullRelationshipFacts InterpretPullRelationship(const PullRelationshipQueries& queries) {
  PullRelationshipFacts facts;

  const UndoQueryRead count = ReadUndoQuery(queries.aheadBehind);
  if (count.outcome != UndoQueryOutcome::answered) {
    facts.queryFailure = L"没能问出本地与远端各有几个独有提交：" +
                         RefusalWith(count, L"rev-list --left-right --count 未成功");
    return facts;
  }
  const std::wstring_view line = count.lines.empty() ? std::wstring_view() : std::wstring_view(count.lines.front());
  if (!ParseAheadBehindCount(line, &facts.ahead, &facts.behind)) {
    facts.queryFailure = L"rev-list --left-right --count 的回答不是「左 右」两个非负整数（读到的那一行是：" +
                         std::wstring(line) + L"），无法判断本地与远端的关系。";
    return facts;
  }
  if (facts.ahead == 0 && facts.behind == 0) {
    facts.relationship = PullRelationship::upToDate;
  } else if (facts.ahead == 0) {
    facts.relationship = PullRelationship::fastForward;
  } else if (facts.behind == 0) {
    facts.relationship = PullRelationship::aheadOnly;
  } else {
    facts.relationship = PullRelationship::diverged;
  }

  facts.mergeBaseRan = queries.mergeBaseRan;
  if (queries.mergeBaseRan) {
    const UndoQueryRead base = ReadUndoQuery(queries.mergeBase);
    if (base.outcome != UndoQueryOutcome::answered) {
      facts.queryFailure = L"没能问出本地与远端的共同基准：" + RefusalWith(base, L"merge-base 未成功");
      return facts;
    }
    if (!LooksLikeFullObjectId(base.firstLine)) {
      facts.queryFailure = L"merge-base 给出的共同基准不符合完整对象 ID 的约定。";
      return facts;
    }
    facts.mergeBaseResolved = true;
    facts.mergeBaseObjectId = base.firstLine;
  }

  facts.incomingRan = queries.incomingRan;
  if (queries.incomingRan) {
    const UndoQueryRead incoming = ReadUndoQuery(queries.incoming);
    if (incoming.outcome != UndoQueryOutcome::answered) {
      facts.incomingDetail = RefusalWith(incoming, L"git diff --name-only 未成功");
    } else {
      std::wstring recordReason;
      if (!NulRecordsAreComplete(queries.incoming.utf16Output, recordReason)) {
        // 半个路径不能当路径用：清单不采信，界面按「读取不完整」处理。
        facts.incomingDetail = L"传入改动的文件清单不符合记录约定：" + recordReason;
      } else {
        facts.incomingOk = true;
        // 注意：这里按 NUL 拆原始输出，不用 UndoQueryRead.lines（那是按行的）。
        facts.incomingPaths = SplitNulList(queries.incoming.utf16Output);
      }
    }
  }

  facts.dryRunRan = queries.mergeTreeRan;
  if (queries.mergeTreeRan) {
    const GitQueryResult& result = queries.mergeTree;
    if (!result.started) {
      facts.dryRun = PullMergeDryRun::failed;
      facts.dryRunDetail = L"预演的 Git 进程未能启动";
    } else if (result.timedOut) {
      facts.dryRun = PullMergeDryRun::failed;
      facts.dryRunDetail = L"预演超时";
    } else if (!result.exited) {
      facts.dryRun = PullMergeDryRun::failed;
      facts.dryRunDetail = L"预演未能正常结束";
    } else if (result.exitCode == 0) {
      facts.dryRun = PullMergeDryRun::supported_clean;
    } else if (result.exitCode == 1) {
      // 退出码 1 是 merge-tree --write-tree 的「有冲突」答案，不是失败（实测 Git 2.53）。
      // 输出形态：第一行结果 tree 的对象 ID；紧跟冲突清单（--name-only 下一行一个路径）；
      // 再往后是一个空行分隔的信息段（Auto-merging… / CONFLICT (content): …）。
      if (!result.outputComplete) {
        // 「有冲突」这个结论来自退出码，可以采信；但要列出哪几个文件冲突就必须靠完整输出，
        // 读不全时宁可只说「预演发现有冲突，但清单没读全」，也不交出一份缺条目的冲突表。
        facts.dryRun = PullMergeDryRun::failed;
        facts.dryRunDetail =
            L"预演发现有冲突，但它的输出没能完整读回，无法列出冲突文件：" +
            (result.incompleteReason.empty() ? L"标准输出不完整" : result.incompleteReason);
      } else {
        facts.dryRun = PullMergeDryRun::supported_conflict;
        const std::vector<std::wstring> lines = SplitLines(result.utf16Output);
        for (size_t index = 1; index < lines.size(); ++index) {
          const std::wstring entry = TrimWide(lines[index]);
          if (entry.empty() || entry.find(L':') != std::wstring::npos ||
              entry.find(L' ') != std::wstring::npos) {
            break;  // 信息段开始（或出现了不像单个路径的行）：清单到此为止。
          }
          facts.dryRunConflicts.push_back(entry);
        }
        if (facts.dryRunConflicts.empty()) {
          facts.dryRunDetail = L"预演报告有冲突，但没能从它的输出里认出冲突文件名。";
        }
      }
    } else if (result.exitCode == 128 || result.exitCode == 129) {
      // 129 = 用法错误（这个 Git 不认 merge-tree --write-tree）；128 = 致命错误。
      facts.dryRun = PullMergeDryRun::unsupported;
      facts.dryRunDetail = L"这个 Git 版本的 merge-tree 预演用不了（退出码 " +
                           std::to_wstring(result.exitCode) + L"）";
    } else {
      facts.dryRun = PullMergeDryRun::failed;
      std::wstring detail;
      const RepoError error = ClassifyGitFailure(result, detail);
      facts.dryRunDetail = detail.empty() ? std::wstring(RepoErrorLabel(error)) : detail;
    }
  }

  // 「本地独有提交里有几个合并提交」：一行一个非负整数就是 Git 的完整回答；
  // 读不到时 Known 保持 false——「没问出来」与「0 个」是两种结论。
  facts.localMergeCountRan = queries.localMergeCountRan;
  if (queries.localMergeCountRan) {
    const UndoQueryRead merges = ReadUndoQuery(queries.localMergeCount);
    if (merges.outcome != UndoQueryOutcome::answered) {
      facts.localMergeCountDetail = RefusalWith(merges, L"rev-list --count --merges 未成功");
    } else {
      const std::wstring_view countLine =
          merges.lines.empty() ? std::wstring_view() : std::wstring_view(merges.lines.front());
      const std::vector<std::wstring> tokens = SplitWhitespace(countLine);
      if (tokens.size() == 1) {
        try {
          const long long value = std::stoll(tokens[0]);
          if (value >= 0) {
            facts.localMergeCountKnown = true;
            facts.localMergeCount = value;
          }
        } catch (const std::exception&) {
          // 不是数字：留给下面的 detail。
        }
      }
      if (!facts.localMergeCountKnown) {
        facts.localMergeCountDetail =
            L"rev-list --count --merges 的回答不是一个非负整数（读到的那一行是：" +
            std::wstring(countLine) + L"）";
      }
    }
  }

  facts.queryOk = true;
  return facts;
}

std::wstring_view PullRelationshipLabel(PullRelationship relationship) noexcept {
  switch (relationship) {
    case PullRelationship::upToDate:
      return L"已一致";
    case PullRelationship::fastForward:
      return L"可快进";
    case PullRelationship::aheadOnly:
      return L"本地领先（远端没有新东西）";
    case PullRelationship::diverged:
      return L"已分叉";
    case PullRelationship::unknown:
      return L"未知";
  }
  return L"未知";
}

std::wstring_view PullIntegrateStrategyLabel(PullIntegrateStrategy strategy) noexcept {
  switch (strategy) {
    case PullIntegrateStrategy::fastForward:
      return L"快进（只把分支引用挪到远端那个提交）";
    case PullIntegrateStrategy::merge:
      return L"合并（产生一次合并提交）";
    case PullIntegrateStrategy::rebase:
      return L"变基（把本地独有提交重放到远端提交之上；其中的合并提交会被压平）";
    case PullIntegrateStrategy::rebaseMerges:
      return L"保留合并结构的变基（--rebase-merges：本地合并提交重放后仍是合并提交）";
  }
  return L"未定";
}

std::wstring_view PullMergeDryRunLabel(PullMergeDryRun dryRun) noexcept {
  switch (dryRun) {
    case PullMergeDryRun::notRun:
      return L"未预演";
    case PullMergeDryRun::supported_clean:
      return L"已预演：没有内容冲突";
    case PullMergeDryRun::supported_conflict:
      return L"已预演：有内容冲突";
    case PullMergeDryRun::unsupported:
      return L"这个 Git 不支持无损预演";
    case PullMergeDryRun::failed:
      return L"预演没能完成";
  }
  return L"未预演";
}

// ---- 阶段一方案 ----

PullFetchPlan BuildPullFetchPlan(const PullTargetFacts& facts, std::wstring_view repositoryDirectory) {
  const auto block = [](std::wstring reason) {
    PullFetchPlan blocked;
    blocked.state = PullFetchPlanState::blocked;
    blocked.explanation = std::move(reason);
    blocked.operationId = L"pull-fetch";
    blocked.displayName = L"pull 抓取";
    return blocked;
  };

  const std::wstring refusal = PullPrerequisiteRefusal(facts, repositoryDirectory);
  if (!refusal.empty()) {
    return block(refusal + L"\n\n没有发出任何命令，也没有接触远端或改动仓库。");
  }

  // 抓取阶段与界面 fetch 按钮共用 git/fetch_scope 那一份策略：目标就是分支上游的那个远端，
  // 命令行上能中和的副作用（prune／pruneTags／标签跟随／子模块递归）一律中和，
  // 中和不了的映射（remote.<远端>.fetch 指向别的本地命名空间）在执行前拒绝并点名那一句配置。
  const FetchScopeDecision scope = DecideFetchScope(facts.scope, facts.upstreamRemote);
  if (!scope.allowed) {
    return block(scope.refusal + L"\n\n没有发出任何命令，也没有接触远端或改动仓库。");
  }

  PullFetchPlan plan;
  plan.state = PullFetchPlanState::ready;
  plan.operationId = L"pull-fetch";
  plan.displayName = L"pull 抓取";
  plan.remoteName = facts.upstreamRemote;
  plan.trackingRef = facts.upstreamTrackingRef;
  plan.remoteBranchRef = facts.upstreamRemoteBranch;
  plan.localBranchRef = facts.branchRef;
  plan.arguments = scope.arguments;
  plan.commandLabel = scope.commandLabel;
  plan.explanation = L"本地分支 " + facts.branchName + L" 的上游是远端「" + facts.upstreamRemote +
                     L"」的 " + RemoteBranchDisplay(facts) + L"。";
  plan.notice = L"pull 第一步（获取）范围：" + scope.noticeCore;

  std::wstring leading;
  leading += L"这次 pull 要处理的分支对：\n";
  leading += L" · 本地分支：" + facts.branchRef + L"（现在在 " + ShortObjectId(facts.headObjectId) +
             L"）\n";
  leading += L" · 远端分支：远端「" + facts.upstreamRemote + L"」的 " + RemoteBranchDisplay(facts) + L"\n";
  leading += L" · " + TrackingRefSentence(facts) + L"\n";

  std::wstring stage;
  stage += L"这一步只做上面那一条命令：把远端最新的位置读到本地的 " + facts.upstreamTrackingRef +
           L"，整合是它成功之后的下一步，本程序不会把两件事捆在一起做。\n";
  stage += ConfigSentence(facts);
  stage += L"抓取结束后，本程序会重新读一回仓库现状，把「本地与远端的关系、可预见的风险、"
           L"准备用哪种整合方式」摆给你看，再问一次才动手整合。\n";

  plan.confirmationText = BuildFetchConfirmationText(
      scope, facts.upstreamRemote, L"",
      L"当前分支的上游配置（branch." + facts.branchName + L".remote = " + facts.upstreamRemote + L"）",
      repositoryDirectory, leading, stage);
  return plan;
}

// ---- 阶段二方案 ----

PullIntegratePlan BuildPullIntegratePlan(const PullIntegratePlanInput& input) {
  const auto block = [](std::wstring reason) {
    PullIntegratePlan blocked;
    blocked.state = PullPlanState::blocked;
    blocked.explanation = std::move(reason);
    blocked.operationId = L"pull-integrate";
    blocked.displayName = L"pull 整合";
    return blocked;
  };

  const PullTargetFacts& target = input.target;
  const PullRelationshipFacts& relationship = input.relationship;

  const std::wstring refusal = PullPrerequisiteRefusal(target, input.repositoryRoot);
  if (!refusal.empty()) {
    return block(refusal);
  }
  if (!target.trackingResolved) {
    return block(L"上游配置指向 " + target.upstreamTrackingRef +
                 L"，可这次抓取之后本地仍然没有这个远端跟踪引用（" +
                 (target.trackingDetail.empty() ? std::wstring(L"位置未知") : target.trackingDetail) +
                 L"）。没有可整合的目标——远端那边也许已经把这条分支删掉了。"
                 L"本程序不拿另一个引用顶替，也不改你的上游配置。");
  }
  if (!LooksLikeFullObjectId(target.headObjectId) || !LooksLikeFullObjectId(target.trackingObjectId)) {
    return block(L"本地 HEAD 或远端跟踪引用的对象 ID 形态不合格，无法确定整合目标。");
  }
  if (!relationship.queryOk) {
    return block(relationship.queryFailure.empty()
                     ? std::wstring(L"没能问出本地与远端的关系，本程序不在猜出来的关系上做整合。")
                     : relationship.queryFailure);
  }
  if (relationship.relationship == PullRelationship::unknown) {
    return block(L"本地与远端的关系判不出来（rev-list 的回答不合约定）。");
  }

  // ---- 配置里有原生 git pull 自己都会拒绝的取值：当场如实拒绝整合 ----
  // 原生 pull 在解析配置阶段就 die（本机 2.53 实测：fatal: invalid value for '…'），连远端都不接触。
  // 本程序的获取是独立可见的一步（与 fetch 按钮同源），到整合这一步如实拒绝；
  // 绝不把坏配置当成没设，也不悄悄降级成合并或普通变基。
  const PullStrategyConfig strategyCfg = ResolveStrategyConfig(target);
  const PullFfConfig ffCfg = ResolveFfConfig(target);
  if (strategyCfg.value == PullRebaseConfigValue::invalid) {
    return block(L"配置 " + strategyCfg.key + L" = " + ConfigValueDisplay(strategyCfg.raw) +
                 L" 是 Git 不认的取值：原生 git pull 会在联网之前直接拒绝。可用的取值是 "
                 L"true/false（及其布尔写法 yes/no/on/off/1/0，大小写不敏感）、merges/m（保留合并结构的变基）、"
                 L"interactive/i（交互式，本程序不执行）——其中 merges、interactive 及缩写必须严格小写，"
                 L"preserve/p 已被并入 merges、同样按无效拒绝。\n"
                 L"本程序不会把这句配置当成没设，不会替你猜一个策略，也不会改写你的配置文件。"
                 L"请自己把这条改正（git config --unset " + strategyCfg.key +
                 L"，或设成上面之一）后再点 pull；也可以直接在自己的终端里跑 git pull。");
  }
  if (ffCfg.value == PullFfConfigValue::invalid) {
    return block(L"配置 " + ffCfg.key + L" = " + ConfigValueDisplay(ffCfg.raw) +
                 L" 是 Git 不认的取值：原生 git pull 会在联网之前直接拒绝。pull.ff 可用的取值是 "
                 L"true/false（及布尔写法）与严格小写的 only。\n"
                 L"本程序不把它当成没设，也不改写配置；请自己改正后再点 pull。");
  }

  // ---- 远端没有新东西：无事可做，连命令都不造 ----
  if (relationship.behind == 0) {
    PullIntegratePlan nothing;
    nothing.state = PullPlanState::nothingToIntegrate;
    nothing.operationId = L"pull-integrate";
    nothing.displayName = L"pull 整合";
    nothing.explanation =
        relationship.ahead == 0
            ? L"已经一致：" + target.branchRef + L" 与 " + target.upstreamTrackingRef +
                  L" 停在同一个提交（" + ShortObjectId(target.headObjectId) +
                  L"）。这次 pull 没有可整合的内容，因此一条整合命令也没执行。"
            : L"远端没有新东西：本地 " + target.branchRef + L" 比 " + target.upstreamTrackingRef +
                  L" 还领先 " + std::to_wstring(relationship.ahead) +
                  L" 个提交，整合方向是反的。把本地提交送上去属于 push 的范围，"
                  L"这次 pull 没有执行任何整合命令，也没有改动仓库。";
    return nothing;
  }

  // ---- 策略：配置说了话就照配置；配置没表态且分叉时让用户当场选（默认偏向合并） ----
  // 下面这套判定与 Git 2.53 原生 pull 逐步对齐（pull.c 主流程 + rebase.c/merge.c 的配置解析，
  // 本机实测）：选择的形态、确认的文案与命令参数必须说同一件事。
  // 「识别出 merges 却发不带 --rebase-merges 的普通变基」「识别出 interactive 却当没设」都在杜绝之列。
  const bool canFastForward = relationship.relationship == PullRelationship::fastForward;
  const bool diverged = relationship.relationship == PullRelationship::diverged;

  PullIntegratePlan plan;
  plan.operationId = L"pull-integrate";
  plan.displayName = L"pull 整合";
  std::wstring strategySource;

  // 用户当场选择等价于命令行 --rebase / --no-rebase；配置给的方向等价于纯配置。
  // 原生规则一：配置里的 pull.ff=only 优先于任何策略（源码注释「ff-only takes precedence
  // over rebase」——分叉时 pull 直接拒绝，根本不执行变基）；但命令行一旦显式定过策略，
  // 原生把 --ff-only 降回默认快进（源码注释写明这是留给显式 --rebase/--no-rebase 的口子）。
  const bool userChose = input.choice != PullStrategyChoice::none;
  const bool userChoseRebase = userChose && input.choice == PullStrategyChoice::chooseRebase;
  const bool ffOnlyWins = ffCfg.value == PullFfConfigValue::only && ffCfg.fromPullFf && !userChose;
  const bool configRebase =
      strategyCfg.value == PullRebaseConfigValue::rebase ||
      strategyCfg.value == PullRebaseConfigValue::rebaseMerges ||
      strategyCfg.value == PullRebaseConfigValue::interactive;

  // 交互式配置的拒绝范围：只有「真会执行交互式变基」的场合才拒绝——
  // 可快进时原生根本不开编辑器（can_ff 一律强制 --ff-only 合并），无事可做更轮不到变基；
  // 分叉且没被 ff-only 抢先，才会真的落到 git rebase -i——那种场合如实拒绝。
  if (diverged && !ffOnlyWins && strategyCfg.value == PullRebaseConfigValue::interactive) {
    return block(L"配置 " + strategyCfg.key + L" = " + ConfigValueDisplay(strategyCfg.raw) +
                 L"：原生 git pull 会执行交互式变基（git rebase -i，最终历史由你在编辑器里当场改定的"
                 L"todo 清单决定）。\n"
                 L"本程序的确认框承诺「确认的是什么形态，得到的就是什么形态」；交互式变基的形态要等"
                 L"编辑器关掉之后才定得下来，预检无法对它下结论，那份合并式冲突预演对它也不适用，"
                 L"事后核对更无从谈起。因此本程序拒绝执行这一种策略：不发出任何命令、不动仓库，"
                 L"也不会把它悄悄换成普通变基或合并。\n"
                 L"要按交互式变基走，请在自己的终端里执行 git pull（或 git rebase -i 加远端提交ID）；"
                 L"要让本程序整合，请把这条配置改成 true、false 或 merges——本程序绝不改写你的配置文件。");
  }

  if (diverged && strategyCfg.value == PullRebaseConfigValue::absent && !userChose && !ffOnlyWins) {
    plan.state = PullPlanState::chooseStrategy;
    plan.strategyCandidates = {
        L"合并：远端那 " + std::to_wstring(relationship.behind) + L" 个提交与本地那 " +
            std::to_wstring(relationship.ahead) + L" 个提交之间产生一次合并提交（Git 的默认做法）",
        L"变基：把本地那 " + std::to_wstring(relationship.ahead) +
            L" 个提交重放到远端提交之上（本地这几个提交的 ID 会被改写；其中有合并提交会被压平，"
            L"要保留合并结构请把配置设为 merges）"};
    plan.explanation = L"本地与远端已经分叉：本地独有 " + std::to_wstring(relationship.ahead) +
                       L" 个提交，远端独有 " + std::to_wstring(relationship.behind) + L" 个提交。\n" +
                       ConfigSentence(target) +
                       L"你的仓库没有给出明确的 pull 策略，本程序不替你定、也不写任何配置文件。"
                       L"（原生 git pull 在这种情况下同样拒绝，要求你当场定或写进配置。）"
                       L"请在下面选一个（选完还会再给一次带风险清单的确认）：";
    return plan;
  }

  // 这一路会不会真的重放/合并：ff-only 凌驾与合并路线之外，就是变基路线（rebase 或 rebaseMerges）。
  const bool rebaseRoute =
      !ffOnlyWins && (strategyCfg.value == PullRebaseConfigValue::rebase ||
                      strategyCfg.value == PullRebaseConfigValue::rebaseMerges ||
                      (strategyCfg.value == PullRebaseConfigValue::absent && userChoseRebase));

  // 「按配置让 Git 自己拒绝」的形态：ff-only 遇上分叉时命令照发，Git 会给出原生拒绝；
  // 确认文字里预演那一行要如实说明它不参与。
  bool refusalExpected = false;

  const auto mergeArguments = [&target](std::wstring_view ffFlag) {
    std::vector<std::wstring> arguments{L"-c", L"submodule.recurse=false", L"merge",
                                        L"--no-autostash"};
    if (!ffFlag.empty()) {
      arguments.push_back(std::wstring(ffFlag));
    }
    if (ffFlag != L"--ff-only") {
      arguments.push_back(L"--no-edit");
    }
    arguments.push_back(target.trackingObjectId);
    return arguments;
  };

  // 预演结论的可信档位：仓库里存在改变合并等效性的配置（外部 merge driver、遗留策略），
  // 或那份清单没能完整读回时，「预演说没有冲突」就只是强提示，不是保证。
  std::wstring dryRunCaveat;
  if (target.mergeEquivalence == PullMergeEquivalenceProbe::some) {
    dryRunCaveat = L"仓库里存在改变合并等效性的配置（" + FormatFileList(target.mergeEquivalenceKeys) +
                   L"）：外部 merge driver / 遗留策略配置会在真实合并时接管相应路径，"
                   L"merge-tree 预演的结论在这里只是强提示，不构成保证。";
  } else if (target.mergeEquivalence == PullMergeEquivalenceProbe::unknown ||
             target.mergeEquivalence == PullMergeEquivalenceProbe::notProbed) {
    dryRunCaveat = L"合并等效性配置没能完整读回，真实合并是否照预演的机械进行无从核对："
                   L"预演结论在这里只是强提示，不构成保证。";
  }

  std::vector<std::wstring> risks;
  std::wstring commandNote;

  if (canFastForward) {
    if (configRebase || userChoseRebase) {
      // 原生：变基路线且可快进时根本不开重放，直接强制 --ff-only 合并（源码与实测一致）——
      // 即便配置同时要求「绝不快进」，可快进时也不该产生多余的合并提交。
      plan.strategy = PullIntegrateStrategy::fastForward;
      plan.arguments = mergeArguments(L"--ff-only");
      strategySource =
          (strategyCfg.value != PullRebaseConfigValue::absent
               ? strategyCfg.source
               : std::wstring(L"你刚才在策略选择里点的「变基」（配置里没有可依据的 pull.rebase）")) +
          L"；本地没有独有提交——原生变基在这种场合就是直接快进（不开重放），本程序照此执行";
      if (ffCfg.value == PullFfConfigValue::never) {
        commandNote = L"配置虽要求不快进，但原生在「变基且可快进」时仍强制快进，不产生合并提交，本程序照此";
      }
    } else if (ffCfg.value == PullFfConfigValue::never) {
      // 合并路线 + 配置要求绝不快进：本地虽然可以快进，也不该悄悄违背它——这次会产生合并提交。
      plan.strategy = PullIntegrateStrategy::merge;
      plan.arguments = mergeArguments(L"--no-ff");
      commandNote = L"你的配置要求不快进，因此这次虽然可以快进仍会产生一次合并提交";
      risks.push_back(L"配置「" + ffCfg.source + L"」要求不快进：本地其实可以快进，这次仍会产生一次合并提交"
                      L"（这是你那句配置本身的行为，不是本程序自作主张）。");
    } else {
      plan.strategy = PullIntegrateStrategy::fastForward;
      plan.arguments = mergeArguments(L"--ff-only");
      if (ffCfg.value == PullFfConfigValue::only) {
        commandNote = L"你的配置要求只在可快进时整合，这次正是可快进，一致";
      }
    }
  } else if (diverged && ffOnlyWins) {
    // 配置给的 pull.ff=only 优先于一切策略：分叉时原生 pull 直接拒绝（Not possible to
    // fast-forward, aborting.）。本程序把同一条 merge --ff-only 发出去，让命令窗口里的
    // Git 给出原生的拒绝——仓库零改动，也不假装那句配置不存在。
    plan.strategy = PullIntegrateStrategy::merge;
    plan.arguments = mergeArguments(L"--ff-only");
    refusalExpected = true;
    strategySource =
        (strategyCfg.value != PullRebaseConfigValue::absent
             ? strategyCfg.source +
                   std::wstring(L"；但配置给的 pull.ff=only 在原生规则里优先于策略"
                                L"（分叉时根本不开合并/变基）")
             : std::wstring(L"配置没定策略；而 pull.ff=only 优先于一切：原生只在可快进时整合")) +
        L"。这次双方已分叉，照此发出 merge --ff-only，由 Git 自己给出原生的拒绝";
    risks.push_back(L"本地与远端已分叉，而配置「" + ffCfg.source + L"」要求只在可快进时整合："
                    L"Git 会像原生 git pull 一样拒绝这次整合（Not possible to fast-forward, "
                    L"aborting.），仓库不会有任何改动。本程序不会为了让它「成功」而违背你的配置。");
  } else if (diverged && rebaseRoute) {
    const bool keepMerges = strategyCfg.value == PullRebaseConfigValue::rebaseMerges;
    plan.strategy =
        keepMerges ? PullIntegrateStrategy::rebaseMerges : PullIntegrateStrategy::rebase;
    // --no-autostash 写死：rebase.autoStash / pull.autoStash 不会把「替你 stash」这件事悄悄做掉。
    plan.arguments = {L"-c", L"submodule.recurse=false", L"rebase", L"--no-autostash"};
    if (keepMerges) {
      plan.arguments.push_back(L"--rebase-merges");  // 配置说保留合并结构，命令就真的带上它
    }
    plan.arguments.push_back(target.trackingObjectId);
    if (strategyCfg.value != PullRebaseConfigValue::absent) {
      strategySource = strategyCfg.source;
    }
    if (userChose && ffCfg.value == PullFfConfigValue::only && ffCfg.fromPullFf) {
      commandNote = L"你当场选了策略（等价命令行 --rebase）：原生在这种组合把 pull.ff=only 降回默认，"
                    L"变基照常执行，本程序照此";
    }
    std::wstring rewrite =
        keepMerges ? L"变基（--rebase-merges，保留合并结构）会重写本地那 " : L"变基会重写本地那 ";
    rewrite += std::to_wstring(relationship.ahead) +
               L" 个提交（它们的对象 ID 会全部改变）。如果这些提交已经推送过，其他协作者会与你"
               L"分叉；本程序不会自动 force push。改写前的位置仍在 reflog 里，可以找回。";
    risks.push_back(std::move(rewrite));
    risks.push_back(L"变基是逐提交重放的，冲突可能出现在中间某一步" +
                    std::wstring(keepMerges ? L"（保留合并结构时还包括合并重放那一步）" : L"") +
                    L"。本程序不用合并式预演去声称「变基不会冲突」——那种结论是假的，"
                    L"所以这一条路上没有冲突预演结果。");
    if (keepMerges) {
      if (relationship.localMergeCountKnown) {
        commandNote = L"--rebase-merges：本地独有提交里的 " +
                      std::to_wstring(relationship.localMergeCount) +
                      L" 个合并提交重放后仍是合并提交，父子图保持合并形态";
      } else {
        risks.push_back(L"没能问出本地独有提交里有几个合并提交（" +
                        (relationship.localMergeCountDetail.empty()
                             ? std::wstring(L"原因未知")
                             : relationship.localMergeCountDetail) +
                        L"）；--rebase-merges 会按合并结构重放，这一点不因此改变。");
      }
    } else if (relationship.localMergeCountKnown && relationship.localMergeCount > 0) {
      risks.push_back(L"本地独有提交里有 " + std::to_wstring(relationship.localMergeCount) +
                      L" 个合并提交：普通变基会按线性历史重放，它们的合并结构会被压平、丢弃"
                      L"（这是原生 pull.rebase=true 的行为，本程序如实披露，不假装保留）。"
                      L"要保留合并结构，请把 " + BranchRebaseKey(target.branchName) +
                      L" 或 pull.rebase 设为 merges（本程序会实际带上 --rebase-merges）。");
    } else if (!relationship.localMergeCountKnown) {
      risks.push_back(L"没能问出本地独有提交里有没有合并提交（" +
                      (relationship.localMergeCountDetail.empty()
                           ? std::wstring(L"原因未知")
                           : relationship.localMergeCountDetail) +
                      L"）：普通变基遇到合并提交会将其压平，这次的确切形态无法事先报准。");
    }
  } else {
    // 合并路线（配置说合并、用户选合并、或配置没表态且可快进以外的情形）。因为实际执行的是
    // `git merge`（它看不见 pull.ff），配置里的快进意愿在这里被翻译成命令行上显式的旗标，
    // 「预检声称的形态」与「命令实际做的形态」因此是同一件事。
    plan.strategy = PullIntegrateStrategy::merge;
    std::wstring ffFlag;
    if (ffCfg.value == PullFfConfigValue::only && !ffCfg.fromPullFf) {
      // merge.ff=only：原生由 merge 子进程读到并拒绝分叉整合；这里显式写成同样的 --ff-only。
      ffFlag = L"--ff-only";
      refusalExpected = true;
      risks.push_back(L"配置「" + ffCfg.source + L"」要求只在可以快进时整合，而本地与远端已经分叉："
                      L"这次很可能被 Git 直接拒绝（这正是你那句配置的行为）。本程序不会为了让它"
                      L"「成功」而违背你的配置。");
    } else if (ffCfg.value == PullFfConfigValue::only && ffCfg.fromPullFf && userChose) {
      // 用户当场选过策略 + pull.ff=only（等价命令行 --no-rebase）：原生在这种组合把
      // --ff-only 降回默认——合并提交照常产生，不额外写 ff 旗标。
      commandNote = L"配置 pull.ff=only 在这里按原生规则降回默认：命令行显式定过策略（你当场选的）时，"
                    L"--ff-only 不再是硬约束，合并照常";
    } else if (ffCfg.value == PullFfConfigValue::never) {
      ffFlag = L"--no-ff";
    }
    plan.arguments = mergeArguments(ffFlag);
    if (strategyCfg.value != PullRebaseConfigValue::absent) {
      strategySource = strategyCfg.source;
    }
    switch (relationship.dryRun) {
      case PullMergeDryRun::supported_conflict: {
        std::wstring item = L"内容冲突预演（git merge-tree --write-tree：不改工作区与索引，"
                            L"只向对象库写不可达的结果对象）报告会有冲突";
        if (!relationship.dryRunConflicts.empty()) {
          item += L"：" + FormatFileList(relationship.dryRunConflicts);
        }
        item += L"。真跑一次合并会在这些文件里留下冲突标记并停在「合并进行中」；届时本程序不会 "
                L"abort、不会 reset，也不会替你选任何一方的内容。";
        if (!dryRunCaveat.empty()) {
          item += L"另外，" + dryRunCaveat;
        }
        risks.push_back(std::move(item));
        break;
      }
      case PullMergeDryRun::unsupported:
      case PullMergeDryRun::failed: {
        std::wstring item = L"内容冲突没能预演（" +
                            (relationship.dryRunDetail.empty()
                                 ? std::wstring(PullMergeDryRunLabel(relationship.dryRun))
                                 : relationship.dryRunDetail) +
                            L"），所以「会不会冲突」本程序并不知道，只能保守提醒你先想想远端那 " +
                            std::to_wstring(relationship.behind) + L" 个提交改了什么。";
        if (!dryRunCaveat.empty()) {
          item += L"另外，" + dryRunCaveat;
        }
        risks.push_back(std::move(item));
        break;
      }
      case PullMergeDryRun::supported_clean:
      case PullMergeDryRun::notRun:
      default:
        if (!dryRunCaveat.empty() &&
            relationship.dryRun == PullMergeDryRun::supported_clean) {
          risks.push_back(L"预演说没有内容冲突，但这份结论的档位被降低了：" + dryRunCaveat);
        }
        break;
    }
  }

  if (strategySource.empty()) {
    strategySource =
        strategyCfg.value != PullRebaseConfigValue::absent
            ? strategyCfg.source
            : std::wstring(userChose
                               ? (userChoseRebase
                                      ? L"你刚才在策略选择里点的「变基」（配置里没有可依据的 pull.rebase）"
                                      : L"你刚才在策略选择里点的「合并」（配置里没有可依据的 pull.rebase）")
                               : L"配置没表态：本地没有独有提交时快进即可（原生同场景也是快进）");
  }

  // ---- 工作区重叠风险：本地未提交的东西与这次要带进来的路径撞上 ----
  if (relationship.incomingRan && !relationship.incomingOk) {
    risks.push_back(L"没能问出这次会带进哪些文件（" +
                    (relationship.incomingDetail.empty() ? std::wstring(L"原因未知")
                                                         : relationship.incomingDetail) +
                    L"），「会不会盖到你没提交的东西上」这一条因此没法核对。");
  } else {
    const WorktreeOverlap overlap = CollectIncomingOverlap(target.model, relationship.incomingPaths);
    if (!overlap.dirtyTracked.empty()) {
      risks.push_back(L"你还没提交的改动与这次要整合的文件重叠：" + FormatFileList(overlap.dirtyTracked) +
                      L"。Git 在这种情况下通常直接拒绝整合（宁可不动，也不会把你的修改揉掉）。"
                      L"本程序不会替你 stash，也不会先提交再拉。");
    }
    if (!overlap.untracked.empty()) {
      risks.push_back(L"工作区里有未跟踪文件与远端带来的同名路径撞上了：" +
                      FormatFileList(overlap.untracked) +
                      L"。Git 不会覆盖未跟踪文件，会拒绝这次整合；请先自己处置这些文件。");
    }
  }
  plan.requiresForce = !risks.empty();
  plan.risks = std::move(risks);
  plan.strategySource = strategySource;

  // ---- 确认文字与随操作显示的范围说明 ----
  std::wstring commandLine = L"  git";
  for (const std::wstring& argument : plan.arguments) {
    commandLine += L' ' + argument;
  }
  const std::wstring relationshipSentence =
      L"本地与远端的关系：" + std::wstring(PullRelationshipLabel(relationship.relationship)) +
      L"（本地独有 " + std::to_wstring(relationship.ahead) + L" 个提交、远端独有 " +
      std::to_wstring(relationship.behind) + L" 个提交，共同基准 " +
      (relationship.mergeBaseResolved ? ShortObjectId(relationship.mergeBaseObjectId)
                                       : std::wstring(L"未读出")) +
      L"）";

  std::wstring text;
  text += L"这次 pull 的第二步「整合」将在命令窗口里执行：\n";
  text += commandLine + L"（工作目录：" + input.repositoryRoot + L"）\n\n";
  text += L"本地分支：" + target.branchRef + L"（现在在 " + ShortObjectId(target.headObjectId) + L"）\n";
  text += L"整合目标：" + target.upstreamTrackingRef + L" = " + ShortObjectId(target.trackingObjectId) +
          L"（完整 ID " + target.trackingObjectId + L"）\n";
  text += L"命令里写的是刚刚那次抓取读回来的那一份提交的完整 ID，不是「到时候再看那个引用停在哪儿」。\n";
  text += relationshipSentence + L"\n";
  text += L"整合方式：" + std::wstring(PullIntegrateStrategyLabel(plan.strategy)) + L"\n";
  text += L"　依据：" + (strategySource.empty() ? std::wstring(L"（未记录）") : strategySource) + L"\n";
  if (!commandNote.empty()) {
    text += L"　" + commandNote + L"\n";
  }
  // 预演这一行必须跟着实际执行的策略走：变基路线（含保留合并结构那种）那份合并式预演结果
  // 不适用；「ff-only 由 Git 自己拒绝」的路线根本不会发生合并内容，预演结论也不参与。
  if (plan.strategy == PullIntegrateStrategy::rebase ||
      plan.strategy == PullIntegrateStrategy::rebaseMerges) {
    text += L"　预演：变基路线不做合并式预演（上面那份合并冲突预演结论不适用于它）\n";
  } else if (refusalExpected) {
    text += L"　预演：这次按配置只发快进条件的整合，Git 会先行拒绝（Not possible to fast-forward），"
            L"不会发生合并内容，预演结论不参与\n";
  } else {
    text += L"　预演：" + std::wstring(PullMergeDryRunLabel(relationship.dryRun));
    if (!dryRunCaveat.empty()) {
      text += L"（结论的可信档位被降低，见风险条目）";
    }
    text += L"\n";
  }
  text += ConfigSentence(target);
  text += L"-c submodule.recurse=false 与 --no-autostash 只对这一个子进程生效：不写你的配置文件，"
          L"不会替你 stash（autoStash 类配置也被这次覆盖），子模块不会被递归改动。\n";
  if (plan.strategy != PullIntegrateStrategy::fastForward) {
    text += L"没有 --no-verify：pre-merge / commit-msg 等 hooks 与提交签名照常按你的配置生效。\n";
  }
  if (!relationship.incomingPaths.empty()) {
    text += L"\n这次会从远端带进来 " + std::to_wstring(relationship.incomingPaths.size()) +
            L" 个文件的改动：" + FormatFileList(relationship.incomingPaths) + L"\n";
  } else if (relationship.incomingOk) {
    text += L"\n这次带进来的改动里没有文件路径的差异（可能只是提交本身的内容不同）。\n";
  }
  if (!plan.risks.empty()) {
    text += L"\n风险提示（本程序不认为这些可以忽略）：\n";
    for (size_t index = 0; index < plan.risks.size(); ++index) {
      text += L" " + std::to_wstring(index + 1) + L") " + plan.risks[index] + L"\n";
    }
  } else {
    text += L"\n预检没有发现问题：现状干净，预演也没有冲突。\n";
  }
  text += L"\n预检不是保证：点头之后到 Git 真正跑完之间，远端可能又变、hooks 可能拒绝、"
          L"外部程序可能改同一个仓库，这些都不在这次核对范围里。整合失败或留下冲突时，"
          L"命令窗口里留着 Git 的真实输出，本程序不会 abort、不会 reset，也不会替你选任何一方的内容。";
  plan.confirmationText = std::move(text);
  plan.explanation = relationshipSentence;
  plan.targetObjectId = target.trackingObjectId;
  plan.notice = L"pull 第二步（整合）范围：" + std::wstring(PullIntegrateStrategyLabel(plan.strategy)) +
                L"，目标是刚刚抓回来的 " + ShortObjectId(target.trackingObjectId) +
                L"。除了这条命令本身要改的引用/索引/工作区，本程序不 push、不 reset、不 stash、"
                L"不写任何配置文件、不递归子模块。";
  plan.commandLabel = L"git";
  for (const std::wstring& argument : plan.arguments) {
    plan.commandLabel += L' ' + argument;
  }
  plan.state = PullPlanState::ready;
  return plan;
}

namespace {

// 现状的可比形态：把「哪一侧、哪个路径、什么状态字元」排成一份有序清单。
// 只比条目数会漏掉「改了一个又一个了一个」这种数量不变而变化了的情况，因此逐条比。
std::vector<std::wstring> WorkspaceFingerprint(const WorkspaceModel& model) {
  std::vector<std::wstring> fingerprint;
  for (const ChangeItem& item : model.unstaged) {
    fingerprint.push_back(L"U " + item.statusCode + L' ' + item.path + L'>' + item.oldPath);
  }
  for (const ChangeItem& item : model.staged) {
    fingerprint.push_back(L"S " + item.statusCode + L' ' + item.path + L'>' + item.oldPath);
  }
  std::sort(fingerprint.begin(), fingerprint.end());
  return fingerprint;
}

std::wstring DescribeRef(std::wstring_view ref) {
  return ref.empty() ? std::wstring(L"（不在分支上）") : std::wstring(ref);
}

std::wstring DescribeObject(std::wstring_view objectId) {
  return objectId.empty() ? std::wstring(L"（读不到）") : ShortObjectId(objectId);
}

}  // namespace

std::wstring DescribePullChange(const PullTargetFacts& preflight, const PullTargetFacts& latest) {
  if (!latest.queryOk) {
    return L"执行前的复核没能完成：" +
           (latest.queryFailure.empty() ? std::wstring(L"原因未知") : latest.queryFailure) +
           L"。看不清现状就不执行，本次没有发出任何整合命令。";
  }

  std::vector<std::wstring> changes;
  if (latest.branchRef != preflight.branchRef) {
    changes.push_back(L"当前分支从「" + DescribeRef(preflight.branchRef) + L"」变成了「" +
                      DescribeRef(latest.branchRef) + L"」");
  }
  if (latest.headObjectId != preflight.headObjectId) {
    changes.push_back(L"HEAD 从 " + DescribeObject(preflight.headObjectId) + L" 变成了 " +
                      DescribeObject(latest.headObjectId));
  }
  if (latest.trackingObjectId != preflight.trackingObjectId) {
    changes.push_back(L"远端跟踪引用 " + preflight.upstreamTrackingRef + L" 从 " +
                      DescribeObject(preflight.trackingObjectId) + L" 变成了 " +
                      DescribeObject(latest.trackingObjectId) +
                      L"（预检之后又有过抓取，或引用被外部改动过）");
  }
  // 策略/快进配置与合并等效性清单也在核对之列：确认框是按「那一刻的配置」承诺的形态，
  // 点头之后配置被外部改过的话，实际会执行的行为就可能不再是确认里写的那一种。
  const auto describeConfigValue = [](bool present, const std::wstring& value) {
    if (!present) {
      return std::wstring(L"（没设）");
    }
    return value.empty() ? std::wstring(L"（空值）") : value;
  };
  struct ConfigPair {
    std::wstring key;
    bool preflightPresent;
    const std::wstring* preflightValue;
    bool latestPresent;
    const std::wstring* latestValue;
  };
  const ConfigPair configPairs[] = {
      {BranchRebaseKey(preflight.branchName), preflight.configBranchRebasePresent,
       &preflight.configBranchRebase, latest.configBranchRebasePresent, &latest.configBranchRebase},
      {L"pull.rebase", preflight.configPullRebasePresent, &preflight.configPullRebase,
       latest.configPullRebasePresent, &latest.configPullRebase},
      {L"pull.ff", preflight.configPullFfPresent, &preflight.configPullFf,
       latest.configPullFfPresent, &latest.configPullFf},
      {L"merge.ff", preflight.configMergeFfPresent, &preflight.configMergeFf,
       latest.configMergeFfPresent, &latest.configMergeFf},
  };
  for (const ConfigPair& pair : configPairs) {
    if (pair.preflightPresent != pair.latestPresent ||
        (pair.preflightPresent && *pair.preflightValue != *pair.latestValue)) {
      changes.push_back(L"配置 " + pair.key + L" 从「" +
                        describeConfigValue(pair.preflightPresent, *pair.preflightValue) +
                        L"」变成了「" + describeConfigValue(pair.latestPresent, *pair.latestValue) +
                        L"」（确认框里承诺的策略形态是按旧配置说的）");
    }
  }
  if (latest.mergeEquivalenceKeys != preflight.mergeEquivalenceKeys ||
      latest.mergeEquivalence != preflight.mergeEquivalence) {
    changes.push_back(L"合并等效性配置清单变了（预演结论的可信档位随之变动）");
  }
  if (!latest.statusOk || !preflight.statusOk) {
    changes.push_back(L"工作区/索引的现状没能读回来");
  } else if (WorkspaceFingerprint(preflight.model) != WorkspaceFingerprint(latest.model)) {
    changes.push_back(L"工作区/索引的条目与预检时不一样了（未暂存 " +
                      std::to_wstring(preflight.model.unstaged.size()) + L"→" +
                      std::to_wstring(latest.model.unstaged.size()) + L" 项、已暂存 " +
                      std::to_wstring(preflight.model.staged.size()) + L"→" +
                      std::to_wstring(latest.model.staged.size()) + L" 项，逐条比对也不同）");
  }
  // 流程痕迹：预检之后外部程序可能另开了一个 merge/rebase（那种现场里再整合一次就是搅乱现场）。
  if (latest.workflow.SpecialFlowText() != preflight.workflow.SpecialFlowText()) {
    const auto describe = [](const RepositoryWorkflowState& state) {
      return state.SpecialFlowText().empty() ? std::wstring(L"没有在进行的流程") : state.SpecialFlowText();
    };
    changes.push_back(L"Git 流程状态也变了：预检时「" + describe(preflight.workflow) + L"」，现在「" +
                      describe(latest.workflow) + L"」");
  }
  if (changes.empty()) {
    return std::wstring();
  }
  std::wstring text = L"点头之后、执行之前，仓库又变了：\n";
  for (const std::wstring& change : changes) {
    text += L" · " + change + L"\n";
  }
  text += L"本次没有发出任何整合命令：预检得出的那份方案已经不属于此刻的这个仓库，"
          L"照着它执行就是拿旧结论对新现场。仓库状态正在重读，看清现状后如仍要 pull 请再点一次。";
  return text;
}

PullConflictState InterpretPullConflictState(const GitQueryResult& conflictListing,
                                             const GitQueryResult& symbolicRef,
                                             const GitQueryResult& headObject) {
  PullConflictState state;

  const UndoQueryRead listing = ReadUndoQuery(conflictListing);
  if (listing.outcome != UndoQueryOutcome::answered) {
    state.readFailure = L"没能问出索引里有哪些未合并条目：" +
                        RefusalWith(listing, L"git diff --diff-filter=U 未成功");
    return state;
  }
  std::wstring recordReason;
  if (!NulRecordsAreComplete(conflictListing.utf16Output, recordReason)) {
    // 清单残缺就不能当成「冲突只有这些」：漏掉的那条正是用户下一步要解决的文件。
    state.readFailure = L"未合并条目的清单不符合记录约定：" + recordReason;
    return state;
  }
  // 未合并清单按 NUL 拆原始输出（路径里可能有空格、中文、&）。
  std::vector<std::wstring> paths = SplitNulList(conflictListing.utf16Output);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  state.conflictPaths = std::move(paths);

  // 分支与 HEAD 只是给说明用的补充信息：问不到不算这次取证的失败（现场本来就可能在游离态上）。
  if (const UndoQueryRead symbolic = ReadUndoQuery(symbolicRef);
      symbolic.outcome == UndoQueryOutcome::answered) {
    state.branchRef = symbolic.firstLine;
  }
  if (const UndoQueryRead head = ReadUndoQuery(headObject);
      head.outcome == UndoQueryOutcome::answered && LooksLikeFullObjectId(head.firstLine)) {
    state.headObjectId = head.firstLine;
  }

  state.readOk = true;
  return state;
}

}  // namespace gc::git
