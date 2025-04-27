#include "git/fetch_scope.h"

#include <algorithm>
#include <string>
#include <vector>

#include "git/push_plan.h"  // MaskPushUrlCredentials：展示 URL 前先过一次凭据掩码
#include "git/undo_commit_plan.h"  // ReadUndoQuery：与撤回/fetch 目标预检同一套「答了/明确没有/失败」判读

namespace gc::git {
namespace {

// 界面与拒绝说明里能出现的配置原文：先限长、再折掉换行与制表、最后做凭据掩码。
// 一条 remote.<x>.url 可能是 https://user:token@host/path，整段摊进确认框就等于把口令抄上屏幕。
[[nodiscard]] std::wstring SampleForUi(std::wstring_view text) {
  constexpr size_t kSampleChars = 120;
  const bool shortened = text.size() > kSampleChars;
  std::wstring sample(text.substr(0, std::min<size_t>(text.size(), kSampleChars)));
  for (wchar_t& c : sample) {
    if (c == L'\r' || c == L'\n' || c == L'\t') {
      c = L' ';
    }
  }
  sample = MaskPushUrlCredentials(sample);
  if (shortened) {
    sample += L"…（已截短）";
  }
  return sample;
}

// 一条记录的展示形态：省略取值的写法（布林意义上的真）必须说出来，不能显示成「值是空的」。
[[nodiscard]] std::wstring RecordForUi(const FetchScopeRecord& record) {
  if (!record.hasValue) {
    return SampleForUi(record.key) + L"（省略取值，Git 的布尔语法里就是真）";
  }
  return SampleForUi(record.key + L" = " + record.value);
}

// --get-regexp 没有命中时的退出码 1 + 空输出是「确实没有这类配置」，不是失败；
// 除此之外 ReadUndoQuery 给出的任何非 answered 结论都当作这份事实没读回来。
bool ReadConfigQuery(const GitQueryResult& result, std::wstring& failure, std::wstring& rawRecords) {
  const UndoQueryRead read = ReadUndoQuery(result);
  if (read.outcome == UndoQueryOutcome::answered) {
    rawRecords = result.utf16Output;
    return true;
  }
  if (read.outcome == UndoQueryOutcome::noResult) {
    return true;  // 明确的「没有」：空清单。
  }
  failure = read.detail.empty() ? std::wstring(L"Git 的配置查询没能完成") : read.detail;
  return false;
}

// 拆一条 `键<换行>值<NUL>` 记录；同时认 `键=值<NUL>` 那种写法（与 git/push_plan.h 对
// config --list --null 的判读同一套规矩）。两个分隔符都没有就是 Git 的「省略取值」形态。
bool ParseConfigRecord(std::wstring_view entry, FetchScopeRecord* record) {
  const size_t newline = entry.find(L'\n');
  const size_t equals = entry.find(L'=');
  size_t separator = std::wstring_view::npos;
  if (newline != std::wstring_view::npos && (equals == std::wstring_view::npos || newline < equals)) {
    separator = newline;
  } else if (equals != std::wstring_view::npos) {
    separator = equals;
  }
  if (separator == std::wstring_view::npos) {
    record->key = std::wstring(entry);
    record->value.clear();
    record->hasValue = false;
    return !record->key.empty();
  }
  record->key = std::wstring(entry.substr(0, separator));
  record->value = std::wstring(entry.substr(separator + 1));
  record->hasValue = true;
  return !record->key.empty();
}

std::vector<std::wstring> SplitNulRecords(std::wstring_view text) {
  std::vector<std::wstring> records;
  std::wstring current;
  for (const wchar_t c : text) {
    if (c == L'\0') {
      // 空记录只在「整份输出为空」时才可能出现，那种场合上面已经按「没有配置」处理过了。
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

std::wstring LowerAscii(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& c : result) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
  }
  return result;
}

// 把一条键拆成「remote / 远端名 / 变量名」。远端名可以含点（实测 `git remote add my.remote`
// 合法），因此必须按「第一个点与最后一个点」切：section 与变量名里都没有点。
bool SplitRemoteKey(std::wstring_view key, std::wstring* remoteName, std::wstring* variable) {
  constexpr std::wstring_view kSection = L"remote.";
  if (key.size() <= kSection.size() || key.compare(0, kSection.size(), kSection) != 0) {
    return false;
  }
  const std::wstring_view rest = key.substr(kSection.size());
  const size_t lastDot = rest.rfind(L'.');
  if (lastDot == std::wstring_view::npos || lastDot == 0 || lastDot + 1 >= rest.size()) {
    return false;
  }
  *remoteName = std::wstring(rest.substr(0, lastDot));
  *variable = std::wstring(rest.substr(lastDot + 1));
  return true;
}

// 读一条配置查询并拆成记录；失败时把原因写进 facts.readFailure。
bool LoadRecords(const GitQueryResult& result, std::wstring_view label, FetchScopeFacts& facts,
                 std::vector<FetchScopeRecord>& out) {
  std::wstring failure;
  std::wstring raw;
  if (!ReadConfigQuery(result, failure, raw)) {
    facts.readFailure = L"没能问出" + std::wstring(label) + L"的配置：" + failure;
    return false;
  }
  std::wstring recordReason;
  if (!NulRecordsAreComplete(raw, recordReason)) {
    facts.readFailure = L"配置清单不符合记录约定：" + recordReason +
                        L"。半条配置看起来也像一条，本程序不在看不全的前提下承诺抓取范围。";
    return false;
  }
  for (const std::wstring& entry : SplitNulRecords(raw)) {
    FetchScopeRecord record;
    if (!ParseConfigRecord(entry, &record)) {
      facts.readFailure = L"配置清单里有一条认不出键名（那一条的开头是：" + SampleForUi(entry) +
                          L"），这份清单不采信。";
      return false;
    }
    out.push_back(std::move(record));
  }
  return true;
}

// 单值布林的「最后一次出现说了算」（Git 读配置就是这个次序），并区分「没设」「明确为假」。
// 取值形态按 Git 文档：true/yes/on/1 与 false/no/off/0（大小写不敏感），省略取值就是真；
// 空字串与其他写法一律 notUnderstood——本模块不解释它。
enum class FetchConfigBool {
  unset = 0,  // 记录里根本没有这一项
  trueValue,
  falseValue,
  notUnderstood,
};

FetchConfigBool LastBoolOf(const std::vector<FetchScopeRecord>& records, std::wstring& outText) {
  if (records.empty()) {
    return FetchConfigBool::unset;
  }
  const FetchScopeRecord& last = records.back();
  outText = RecordForUi(last);
  if (!last.hasValue) {
    return FetchConfigBool::trueValue;  // 省略取值在 Git 的布尔语法里就是真。
  }
  const std::wstring value = LowerAscii(last.value);
  if (value == L"true" || value == L"yes" || value == L"on" || value == L"1") {
    return FetchConfigBool::trueValue;
  }
  if (value == L"false" || value == L"no" || value == L"off" || value == L"0") {
    return FetchConfigBool::falseValue;
  }
  // 空字串与 Git 布尔语法认不出的写法都在这里：Git 自己遇到这种值会直接拒绝执行 fetch，
  // 本程序不去猜它「大概想说什么」，也不在这种取值上承诺「引用不会被删」。
  return FetchConfigBool::notUnderstood;
}

FetchRemoteScope* FindOrAddRemote(FetchScopeFacts* facts, const std::wstring& remoteName) {
  for (size_t index = 0; index < facts->remoteNames.size(); ++index) {
    if (facts->remoteNames[index] == remoteName) {
      return &facts->remotes[index];
    }
  }
  facts->remoteNames.push_back(remoteName);
  facts->remotes.push_back(FetchRemoteScope{});
  return &facts->remotes.back();
}

const FetchRemoteScope* FindRemote(const FetchScopeFacts& facts, std::wstring_view remoteName) {
  for (size_t index = 0; index < facts.remoteNames.size(); ++index) {
    if (facts.remoteNames[index] == remoteName) {
      return &facts.remotes[index];
    }
  }
  return nullptr;
}

FetchScopeDecision Refuse(std::wstring reason) {
  FetchScopeDecision decision;
  decision.allowed = false;
  decision.refusal = std::move(reason);
  return decision;
}

// ---- refspec 验证 ----

// 一条正向映射的目标端是否落在允许命名空间里。返回空串表示合格，非空就是给界面看的那一句。
std::wstring CheckPositiveRefspec(std::wstring_view spec, std::wstring_view remoteName) {
  std::wstring_view body = spec;
  if (!body.empty() && body.front() == L'+') {
    body.remove_prefix(1);
  }
  const size_t colon = body.find(L':');
  if (colon == std::wstring_view::npos) {
    // 没有冒号：Git 会按 refmap 或「只写 FETCH_HEAD」处理它，两种可能本程序无法当场确定
    // 它会不会写进本地命名空间，因此不承诺。
    return L"这条映射没有写目标端（连冒号都没有），本程序无法确定它会把引用写到哪儿";
  }
  const std::wstring_view destination = body.substr(colon + 1);
  if (destination.empty()) {
    // 实测本机 Git 2.53：`refs/heads/*:` 这种「有冒号、目标端为空」的写法直接被 Git 判为无效
    // refspec 而拒绝执行。与其把 Git 的报错当成一次莫名失败，不如在执行前把话说清。
    return L"这条映射写了冒号却没有目标端，Git 自己也会把它当成无效 refspec 拒绝执行";
  }
  const std::wstring allowedPrefix = L"refs/remotes/" + std::wstring(remoteName) + L"/";
  if (destination.size() < allowedPrefix.size() ||
      destination.compare(0, allowedPrefix.size(), allowedPrefix) != 0) {
    return L"这条映射的目标端不在 refs/remotes/" + std::wstring(remoteName) +
           L"/ 之下，会把引用写进别的本地命名空间";
  }
  return std::wstring();
}

// 把逐条核对的结果排进范围正文。
std::wstring RefspecSentence(const FetchRemoteScope& scope, std::wstring_view remoteName) {
  if (scope.fetchRefspecs.empty()) {
    return L" · 这个远端没有配置 fetch 映射，Git 按文档里的默认映射抓取"
           L"（refs/heads/*:refs/remotes/" +
           std::wstring(remoteName) +
           L"/*）：只前进这个远端自己的远端跟踪引用，不写本地分支与标签；\n";
  }
  std::wstring positive;
  size_t negatives = 0;
  for (const std::wstring& spec : scope.fetchRefspecs) {
    if (!spec.empty() && spec.front() == L'^') {
      ++negatives;
      continue;
    }
    positive += L"\n     " + SampleForUi(spec);
  }
  std::wstring text;
  if (positive.empty()) {
    // 一条正向映射都没有（只有 ^ 排除项）：要么 Git 按文档的默认映射走
    // （refs/heads/*:refs/remotes/<名>/*，本来就在允许命名空间里），要么什么都不写。
    // 两种都不会把引用写到别处，所以这里如实说明「没有需要核对的正向映射」。
    text = L" · remote." + std::wstring(remoteName) +
           L".fetch 里一条正向映射也没有（只有下面的排除项）：这次要么按 Git 文档的默认映射"
           L"refs/heads/*:refs/remotes/" +
           std::wstring(remoteName) + L"/* 前进这个远端自己的跟踪引用，要么什么都不写，两种都在承诺的范围之内";
  } else {
    text = L" · remote." + std::wstring(remoteName) +
           L".fetch 已逐条核对，正向映射的目标端全部落在 refs/remotes/" +
           std::wstring(remoteName) + L"/ 之下（下面这些条目就是你此刻的配置）：" + positive;
  }
  if (negatives > 0) {
    text += L"\n";
    text += L"   另有 " + std::to_wstring(negatives) +
            L" 条以 ^ 开头的负 refspec：按 Git 文档它们只排除引用、不含目标端，作用是缩小而不是扩大。\n";
  } else {
    text += L"；\n";
  }
  return text;
}

std::wstring DisclosureParagraph(const FetchScopeDecision& decision) {
  std::wstring text;
  if (decision.disclosures.empty()) {
    return text;
  }
  text += L"本次被命令行中和、或需要你知情的配置（逐条原文，已截短与掩码）：\n";
  for (const std::wstring& item : decision.disclosures) {
    text += L" · " + item + L"\n";
  }
  return text;
}

}  // namespace

std::vector<std::wstring> BuildFetchScopeRemoteConfigArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"config", L"--null", L"--get-regexp",
                                   L"^remote\\..*\\.(fetch|url|prune|prunetags|tagopt|mirror)$"};
}

std::vector<std::wstring> BuildFetchScopeGlobalConfigArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{L"-C", std::wstring(repositoryDirectory), L"--no-optional-locks",
                                   L"config", L"--null", L"--get-regexp",
                                   L"^(fetch\\.(prune|prunetags|all|recursesubmodules)|submodule\\.recurse)$"};
}

FetchScopeFacts InterpretFetchScope(const FetchScopeQueries& queries) {
  FetchScopeFacts facts;

  std::vector<FetchScopeRecord> remoteRecords;
  if (!LoadRecords(queries.remoteConfig, L"影响抓取范围的远端级", facts, remoteRecords)) {
    return facts;
  }
  std::vector<FetchScopeRecord> globalRecords;
  if (!LoadRecords(queries.globalConfig, L"影响抓取范围的全局", facts, globalRecords)) {
    return facts;
  }

  for (const FetchScopeRecord& record : remoteRecords) {
    std::wstring remoteName;
    std::wstring variable;
    if (!SplitRemoteKey(record.key, &remoteName, &variable)) {
      facts.readFailure = L"配置清单里有一条不属于 remote.<远端>.<变量> 形态的记录（" +
                          SampleForUi(record.key) + L"），本程序不在这上面猜哪个远端会被写什么引用。";
      return facts;
    }
    // Git 打印配置键时把 section 与变量名小写化、远端名（subsection）保留大小写
    // （与 git/push_plan.h 对同一份输出的假设一致）。这里再小写一次只为多一层容忍：
    // 远端名一律按 Git 给出的大小写比对，因为 Git 查远端配置本来就区分大小写。
    const std::wstring lowered = LowerAscii(variable);
    FetchRemoteScope* scope = FindOrAddRemote(&facts, remoteName);
    if (lowered == L"fetch") {
      scope->fetchRefspecs.push_back(record.value);
    } else if (lowered == L"url") {
      // 省略取值的 url 记录在 Git 的语义里是「清空这个列表」，这里按空字符串原样收下：
      // 它只会让 Git 自己报「没有可抓取的地址」，不会把范围搬到别处。
      scope->urls.push_back(record.hasValue ? record.value : std::wstring());
    } else if (lowered == L"prune") {
      scope->prune.push_back(record);
    } else if (lowered == L"prunetags") {
      scope->pruneTags.push_back(record);
    } else if (lowered == L"tagopt") {
      scope->tagOpt.push_back(record);
    } else if (lowered == L"mirror") {
      scope->mirror.push_back(record);
    } else {
      facts.readFailure = L"配置清单里有一条本程序没问的远端级变量（" + SampleForUi(record.key) +
                          L"），这份清单不按原样采信。";
      return facts;
    }
  }

  for (const FetchScopeRecord& record : globalRecords) {
    const std::wstring key = LowerAscii(record.key);
    if (key == L"fetch.prune") {
      facts.globalPrune.push_back(record);
    } else if (key == L"fetch.prunetags") {
      facts.globalPruneTags.push_back(record);
    } else if (key == L"fetch.all") {
      facts.globalFetchAll.push_back(record);
    } else if (key == L"fetch.recursesubmodules") {
      facts.globalRecurse.push_back(record);
    } else if (key == L"submodule.recurse") {
      facts.submoduleRecurse.push_back(record);
    } else {
      facts.readFailure = L"配置清单里有一条本程序没问的全局配置项（" + SampleForUi(record.key) +
                          L"），这份清单不按原样采信。";
      return facts;
    }
  }

  facts.readOk = true;
  return facts;
}

FetchScopeDecision DecideFetchScope(const FetchScopeFacts& facts, std::wstring_view remoteName) {
  if (!facts.readOk) {
    return Refuse(L"没能问出会影响这次抓取的配置（" +
                  (facts.readFailure.empty() ? std::wstring(L"原因未知") : facts.readFailure) +
                  L"）。看不清配置就没法承诺「只更新这一个远端的远端跟踪引用」，因此没有发出任何命令。"
                  L"请点“刷新”确认仓库现状后重试。");
  }
  if (remoteName.empty()) {
    return Refuse(L"抓取目标还没有定下来，范围核对无从谈起。");
  }

  const FetchRemoteScope* scope = FindRemote(facts, remoteName);
  FetchRemoteScope absent;
  if (scope == nullptr) {
    scope = &absent;  // 配置里一项都没有：全部按 Git 文档的默认值判（下面逐条说明）。
  }

  FetchScopeDecision decision;
  std::vector<std::wstring> problems;

  // 1) 映射总闸门：正向 refspec 的目标端必须落在允许命名空间里。这条命令行参数绕不过去，
  //    只能验证；不合格就拒绝，绝不改用户配置、也不在命令行上换一套映射来「凑」出承诺。
  std::vector<std::wstring> badSpecs;
  for (const std::wstring& spec : scope->fetchRefspecs) {
    if (!spec.empty() && spec.front() == L'^') {
      continue;  // 负 refspec 只排除，缩小范围。
    }
    const std::wstring problem = CheckPositiveRefspec(spec, remoteName);
    if (!problem.empty()) {
      badSpecs.push_back(SampleForUi(spec) + L"——" + problem);
    }
  }
  if (!badSpecs.empty()) {
    std::wstring text =
        L"远端「" + std::wstring(remoteName) +
        L"」的 fetch 映射超出了本程序承诺的抓取范围（只更新 refs/remotes/" +
        std::wstring(remoteName) + L"/ 之下的远端跟踪引用）：\n";
    for (const std::wstring& item : badSpecs) {
      text += L" · " + item + L"\n";
    }
    text += L"这条映射写在 remote." + std::wstring(remoteName) +
            L".fetch 里，命令行上没有任何参数能覆盖它，而本程序要改命令行上的 refspec 就会连带"
            L"替换掉你自己的分支过滤与上游映射——那是替你猜配置。本程序不改写你的任何配置，"
            L"也不猜一个「更安全」的映射：现在请用自己的终端核对并调整这条映射，再来点这个按钮。";
    return Refuse(std::move(text));
  }

  // 2) 会被命令行中和的副作用：prune / pruneTags / 标签跟随（含 tagOpt）。
  //    取值读不懂时不猜——Git 自己遇到这种值也会拒绝执行 fetch。
  std::wstring boolText;
  const FetchConfigBool remotePrune = LastBoolOf(scope->prune, boolText);
  if (remotePrune == FetchConfigBool::notUnderstood) {
    return Refuse(L"remote." + std::wstring(remoteName) + L".prune 的取值本程序无法按 Git 的布尔语法解释（" +
                  boolText + L"）。这种写法 Git 自己也会拒绝执行 fetch，本程序不在看不懂的取值上承诺「引用不会被删」。");
  }
  const FetchConfigBool globalPrune = LastBoolOf(facts.globalPrune, boolText);
  if (globalPrune == FetchConfigBool::notUnderstood) {
    return Refuse(L"fetch.prune 的取值本程序无法按 Git 的布尔语法解释（" + boolText +
                  L"）。这种写法 Git 自己也会拒绝执行 fetch，本程序不在看不懂的取值上承诺「引用不会被删」。");
  }
  const FetchConfigBool remotePruneTags = LastBoolOf(scope->pruneTags, boolText);
  if (remotePruneTags == FetchConfigBool::notUnderstood) {
    return Refuse(L"remote." + std::wstring(remoteName) +
                  L".pruneTags 的取值本程序无法按 Git 的布尔语法解释（" + boolText + L"）。");
  }
  const FetchConfigBool globalPruneTags = LastBoolOf(facts.globalPruneTags, boolText);
  if (globalPruneTags == FetchConfigBool::notUnderstood) {
    return Refuse(L"fetch.pruneTags 的取值本程序无法按 Git 的布尔语法解释（" + boolText + L"）。");
  }
  const FetchConfigBool globalAll = LastBoolOf(facts.globalFetchAll, boolText);
  if (globalAll == FetchConfigBool::notUnderstood) {
    return Refuse(L"fetch.all 的取值本程序无法按 Git 的布尔语法解释（" + boolText +
                  L"）。命令里虽然已点名这一个远端，本程序也不在看不懂的取值上承诺「不抓别的远端」。");
  }

  for (const FetchScopeRecord& record : scope->tagOpt) {
    const std::wstring value = LowerAscii(record.value);
    if (!record.hasValue) {
      problems.push_back(L"remote." + std::wstring(remoteName) +
                         L".tagOpt 省略了取值，本程序不解释这种写法（Git 文档给的取值是 --tags / --no-tags）。");
      continue;
    }
    if (value.empty() || value == L"--no-tags" || value == L"--tags" || value == L"--prune-tags") {
      continue;  // 命令行上的 --no-tags 与 --no-prune-tags 覆盖它们（文档明示，且实测覆盖成立）。
    }
    problems.push_back(L"remote." + std::wstring(remoteName) + L".tagOpt = " + SampleForUi(record.value) +
                       L" 不是 Git 文档给出的取值，本程序不猜它想让这次抓取做什么。");
  }
  if (!problems.empty()) {
    std::wstring text = L"这个远端上有本程序无法归类的抓取配置：\n";
    for (const std::wstring& item : problems) {
      text += L" · " + item + L"\n";
    }
    text += L"本程序不改写你的配置，也不在猜出来的语义上承诺抓取范围。请先把这些配置改成 Git 文档里的写法，"
            L"或在自己的终端里抓取。";
    return Refuse(std::move(text));
  }

  // 3) 披露：被覆盖的配置、push 专用的 mirror、多地址、递归相关的配置。
  if (remotePrune == FetchConfigBool::trueValue) {
    decision.disclosures.push_back(L"remote." + std::wstring(remoteName) +
                                   L".prune 为真（本来会删除过期的远端跟踪引用）→ 已被命令行上的 --no-prune 覆盖");
  }
  if (globalPrune == FetchConfigBool::trueValue) {
    decision.disclosures.push_back(
        L"fetch.prune 为真（本来会删除过期的远端跟踪引用）→ 已被命令行上的 --no-prune 覆盖");
  }
  if (remotePruneTags == FetchConfigBool::trueValue) {
    decision.disclosures.push_back(L"remote." + std::wstring(remoteName) +
                                   L".pruneTags 为真（本来等同额外声明 refs/tags/*:refs/tags/*，动本地标签）"
                                   L"→ 已被命令行上的 --no-prune-tags 覆盖");
  }
  if (globalPruneTags == FetchConfigBool::trueValue) {
    decision.disclosures.push_back(L"fetch.pruneTags 为真（本来等同额外声明 refs/tags/*:refs/tags/*，动本地标签）"
                                   L"→ 已被命令行上的 --no-prune-tags 覆盖");
  }
  for (const FetchScopeRecord& record : scope->tagOpt) {
    const std::wstring value = LowerAscii(record.value);
    if (value == L"--tags" || value == L"--prune-tags") {
      decision.disclosures.push_back(L"远端级标签配置 " + RecordForUi(record) +
                                     L" → 已被命令行上的 --no-tags 与 --no-prune-tags 覆盖"
                                     L"（Git 文档写明命令行可直接覆盖 tagOpt）");
    }
  }
  const FetchConfigBool remoteMirror = LastBoolOf(scope->mirror, boolText);
  if (remoteMirror == FetchConfigBool::notUnderstood) {
    // mirror 只影响 push（文档明示），读不懂它不该拦住一次 fetch，但也不能不说。
    decision.disclosures.push_back(L"remote." + std::wstring(remoteName) +
                                   L".mirror 的取值本程序读不懂（" + boolText +
                                   L"）；这个配置项只管 push，本次抓取不涉及");
  } else if (remoteMirror == FetchConfigBool::trueValue) {
    decision.disclosures.push_back(L"remote." + std::wstring(remoteName) +
                                   L".mirror 为真——按 Git 文档它只影响 push 的形态，不会扩大这次抓取；"
                                   L"真正会把引用写进本地命名空间的是那条 fetch 映射，上面已逐条核对");
  }
  if (scope->urls.size() > 1) {
    decision.disclosures.push_back(L"这个远端配了 " + std::to_wstring(scope->urls.size()) +
                                   L" 个抓取地址；按 Git 文档 fetch 只用第一个（其余只在 push 时使用），"
                                   L"本程序也不会替你换地址重试");
  }
  if (globalAll == FetchConfigBool::trueValue) {
    decision.disclosures.push_back(L"fetch.all 为真（本来会让 fetch 尝试更新所有远端）"
                                   L"→ 命令行已点名单个远端，按 Git 文档这种显式指定就覆盖它");
  }
  if (!facts.globalRecurse.empty() || !facts.submoduleRecurse.empty()) {
    std::wstring detail;
    for (const FetchScopeRecord& record : facts.globalRecurse) {
      detail += (detail.empty() ? std::wstring() : L"；") + RecordForUi(record);
    }
    for (const FetchScopeRecord& record : facts.submoduleRecurse) {
      detail += (detail.empty() ? std::wstring() : L"；") + RecordForUi(record);
    }
    decision.disclosures.push_back(L"仓库里还有递归抓取相关的配置（" + detail +
                                   L"）→ 命令行上的 --recurse-submodules=no 优先，这一步不递归子模块");
  }

  // 4) 参数与文字：两个入口从此只有这一份。
  decision.allowed = true;
  decision.arguments = {L"fetch", std::wstring(kFetchRecurseSubmodulesOff), std::wstring(kFetchPruneOff),
                        std::wstring(kFetchPruneTagsOff), std::wstring(kFetchTagsOff),
                        std::wstring(remoteName)};
  decision.commandLabel = L"git fetch --recurse-submodules=no --no-prune --no-prune-tags --no-tags " +
                          std::wstring(remoteName);

  std::wstring paragraph;
  paragraph += L"这次抓取的范围（钉在命令行上，不让你的仓库配置来扩大它）：\n";
  paragraph += L" · 只更新远端「" + std::wstring(remoteName) + L"」在 refs/remotes/" +
               std::wstring(remoteName) + L"/ 之下的远端跟踪引用；\n";
  paragraph += L" · 不删除任何引用：命令里显式写了 --no-prune 与 --no-prune-tags，"
               L"你配置的 fetch.prune / remote.<远端>.prune / *.pruneTags 都不会在这次生效；\n";
  paragraph += L" · 不写本地标签、也不写本地分支：命令里的 --no-tags 关掉了 Git 默认的标签自动跟随，"
               L"也覆盖 remote." +
               std::wstring(remoteName) + L".tagOpt；\n";
  paragraph += RefspecSentence(*scope, remoteName);
  paragraph += L" · 不递归子模块：--recurse-submodules=no 已写死在命令里；\n";
  paragraph += L" · 不抓别的远端：命令只点名这一个远端（fetch.all=true 会被显式点名叫停），"
               L"也没有 --all、--multiple 或 remotes.<组> 那种展开；\n";
  paragraph += L" · 不合并、不改上游、不写任何配置文件（那是 pull 第二步才谈的事）。\n";
  paragraph += L"这次真正会被改动的东西：上面那个允许命名空间里的远端跟踪引用、.git/FETCH_HEAD、"
               L"对象库（新抓下来的提交与对象），以及 Git 在 fetch 结束后自己可能做的维护"
               L"（自动 gc、commit-graph）。索引与工作区文件、HEAD 与本地分支都不在这次的范围里——"
               L"但本程序不说「磁盘完全只读」：FETCH_HEAD 与对象库确实会被写。\n";
  paragraph += DisclosureParagraph(decision);
  paragraph += L"最后把边界说清：上面这些核对的是**点确定这一刻读回的配置**。命令行上的中和项跟着进程走，"
               L"覆盖得了同一份配置里的 prune 与标签设置；但点头之后到 Git 真正读配置之间，"
               L"外部程序仍然可以改动这份配置（包括那条 fetch 映射），本程序既不持有 Git 的锁、"
               L"也没有事务级的隔离保证。要绝对把握请在自己的终端里核对配置后再抓。\n";
  decision.scopeParagraph = std::move(paragraph);

  decision.noticeCore =
      L"只更新远端「" + std::wstring(remoteName) + L"」在 refs/remotes/" + std::wstring(remoteName) +
      L"/ 之下的远端跟踪引用：不删引用、不写本地分支与标签、不递归子模块、不抓别的远端、不合并；"
      L"会变的是这些引用、.git/FETCH_HEAD 与对象库，HEAD、索引与工作区不动。";
  return decision;
}

std::wstring BuildFetchConfirmationText(const FetchScopeDecision& decision, std::wstring_view remoteName,
                                        std::wstring_view remoteUrlForDisplay,
                                        std::wstring_view sourceSentence,
                                        std::wstring_view repositoryDirectory,
                                        std::wstring_view leadingText, std::wstring_view stageText) {
  std::wstring text;
  if (!leadingText.empty()) {
    text += leadingText;
  }
  text += L"抓取目标：" + std::wstring(remoteName) + L"　" +
          (remoteUrlForDisplay.empty()
               ? std::wstring(L"（这一次没有读回它的 fetch URL：URL 由 Git 自己按配置解析）")
               : std::wstring(remoteUrlForDisplay)) +
          L"\n";
  text += L"目标来历：" + std::wstring(sourceSentence) + L"\n";
  text += L"将在命令窗口里执行：" + decision.commandLabel;
  if (!repositoryDirectory.empty()) {
    text += L"（工作目录：" + std::wstring(repositoryDirectory) + L"）";
  }
  text += L"\n\n";
  text += decision.scopeParagraph;
  if (!stageText.empty()) {
    text += stageText;
  }
  text += L"需要口令或交互时，命令窗口里由 Git 自己提问，沿用你已有的认证方式；失败时窗口里留着 Git 的真实输出。\n";
  text += L"失败就是失败：本程序不会自动 prune、不会换别的远端或别的地址重试、也不会替你设置上游"
          L"或改动任何配置。确定要执行吗？取消不会打开命令窗口，也不会碰这个仓库。";
  return text;
}

}  // namespace gc::git
