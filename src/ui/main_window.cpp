#include "ui/main_window.h"

#include <commctrl.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "git/commit_history.h"
#include "git/commit_identity.h"
#include "git/commit_message.h"
#include "git/diff_view.h"
#include "git/staging_plan.h"
#include "git/workspace_model.h"
#include "app/decimal_text.h"
#include "app/operation_conclusions.h"
#include "platform/windows/clipboard.h"
#include "platform/windows/commit_message_file.h"
#include "platform/windows/git_toolchain.h"
#include "platform/windows/identity_prompt.h"
#include "platform/windows/local_time.h"
#include "platform/windows/locale_text.h"
#include "platform/windows/path_picker.h"
#include "platform/windows/pathspec_file.h"
#include "platform/windows/remote_choice_dialog.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"
#include "ui/commands.h"
#include "ui/resource_ids.h"
namespace gc::ui {
namespace {

constexpr int kSplitterLeftId = 901;
constexpr int kSplitterRightId = 902;

constexpr int kInitialWindowWidth = 1100;
constexpr int kInitialWindowHeight = 780;

// 命令窗口的一种终态 → 历史记录里分立的结果。绝不把「结果未知 / 启动失败 / Git 未创建」
// 混进「成功 / 失败」：那几种各有措辞，恢复入口对「未知」一律拒绝照记录执行、只重新核实。
app::HistoryOutcome DescribeHistoryOutcome(git::CommandCompletion completion, bool succeeded) {
  switch (completion) {
    case git::CommandCompletion::launchFailed:
      return app::HistoryOutcome::launchFailed;
    case git::CommandCompletion::gitNotStarted:
    case git::CommandCompletion::helperNeverStarted:
      return app::HistoryOutcome::gitNotStarted;
    case git::CommandCompletion::terminated:
    case git::CommandCompletion::stillUnknown:
      return app::HistoryOutcome::unknown;
    case git::CommandCompletion::finished:
      return succeeded ? app::HistoryOutcome::succeeded : app::HistoryOutcome::failed;
    default:
      return app::HistoryOutcome::unknown;
  }
}

constexpr std::wstring_view kTipBrowseRepo =
    L"选择本地仓库目录；也可在输入框直接键入路径（停顿后自动识别）。支持仓库的子目录，识别时会上溯到工作区根。";
constexpr std::wstring_view kTipBrowseGit = L"浏览选择 git.exe；选定后立即在后台运行 git --version 验证。";
constexpr std::wstring_view kTipFetch =
    L"在新命令窗口里执行 git fetch：显示真实命令与输出，Git 结束窗口仍保留；本程序通过结果文件获知退出码。\r\n"
    L"抓取目标优先取当前分支明确配置的远端（branch.<分支名>.remote）；没有这种配置时列出既有远端"
    L"供你选择——不猜 origin、不自动创建远端、也不改动任何配置。\r\n"
    L"fetch 只更新远端跟踪引用（refs/remotes/ 下）：不移动 HEAD、不改本地分支、不动索引与工作区，"
    L"也不顺带 pull；不隐式 prune，不一次抓所有远端，--recurse-submodules=no 明确关闭子模块递归。\r\n"
    L"需要口令或交互时由命令窗口里的 Git 自己提问，沿用你已有的认证方式；失败（不可达、认证不过、"
    L"取消）时窗口里留着真实输出，本程序按退出码重读一次仓库现状，不反复自动重试。";
constexpr std::wstring_view kTipPull =
    L"pull 分两步，两步都在新命令窗口里看得见真实命令与输出：先「获取」（git fetch），再按判定出的"
    L"关系做「整合」（git merge / git rebase）。中间每次弹框之前都在后台只读重读仓库现状。\r\n"
    L"前提由 Git 的回答决定：必须在分支上、这个分支有明确的上游；无上游、游离 HEAD、尚有未解决冲突、"
    L"或有 merge/rebase 等流程在走时，本程序给出具体原因并拒绝——不猜 origin、不代设 upstream、"
    L"不替你改任何配置。\r\n"
    L"整合方式尊重你已有的配置（branch.<分支>.rebase > pull.rebase，pull.ff > merge.ff）并在确认框里"
    L"说明是哪条配置定的；分叉而配置未明确时由你当场选（默认偏向合并）。--no-autostash 与 "
    L"-c submodule.recurse=false 写死在命令里：不会替你 stash，也不会递归改动子模块。\r\n"
    L"风险预检用不改动工作区/索引的 git merge-tree 预演内容冲突，并核对未提交改动、未跟踪文件与"
    L"这次要带进来的路径是否重叠；预检不支持的 Git 版本会降级为保守提示。变基路线不做合并式预演，"
    L"也不会拿合并的预演结果去声称变基无冲突。\r\n"
    L"预检不是保证：点头之后到 Git 跑完之间仓库仍可能被外部改动，因此在执行前还会再核对一次现状，"
    L"对不上就不执行。真留下冲突时本程序保留现场，绝不 abort/reset/continue，也不替你选任何一方的内容。";
constexpr std::wstring_view kTipStatus =
    L"在新命令窗口里执行 git status：显示真实命令与输出，Git 结束窗口仍保留；本程序通过结果文件获知退出码。";
constexpr std::wstring_view kTipStageAdd =
    L"把“未暂存的更改”里选中的条目交给 git add：在新命令窗口里显示真实命令与 Git 的完整输出，"
    L"执行结束后窗口保留，本程序自动重读两个列表。\r\n"
    L"只处理选中的行（可多选）：没有选中就是“什么都不做”，绝不会代为暂存全部改动，"
    L"也不会用 git add . 或 git add -A。\r\n"
    L"选中条目按字面路径传给 Git（清单文件以 NUL 分隔，每条都带 :(literal) 标记），文件名里的 [ ] # ! % "
    L"与空格、中文都不会被当成通配、取反或选项，因此不会顺带改动没选中的文件。\r\n"
    L"“重命名”在工作区里是“旧路径已删除 + 新路径未跟踪”两条记录，要暂存这次重命名请把两条一起选中。\r\n"
    L"子模块条目只会让父仓库记录提交指针：子模块内部尚未提交的文件必须由那个仓库自己暂存并提交。";
constexpr std::wstring_view kTipStageRemove =
    L"把“已暂存的更改”里选中的条目从索引撤回：在新命令窗口里显示真实命令与 Git 的完整输出，"
    L"执行结束后窗口保留，本程序自动重读两个列表。\r\n"
    L"这一步只写索引，绝不改写工作区：磁盘上的文件内容原样保留，不会用 git restore 工作区、"
    L"git reset --hard 或 git clean 这类会丢弃改动的命令。\r\n"
    L"只处理选中的行（可多选）：没有选中就是“什么都不做”，绝不会代为取消整个仓库的暂存，"
    L"也不会执行不带路径的 git reset（只有那种形态才会改动 HEAD）。\r\n"
    L"有最近一次提交时命令是 git restore --staged；仓库还没有任何提交时没有可退回的 HEAD，"
    L"改用同样只写索引的 git reset -q -- <所选路径>：选中的条目从索引移除，文件留在磁盘上、回到未跟踪状态。\r\n"
    L"“重命名”条目会同时撤回旧路径与新路径两条索引记录，只撤一条会留下半套暂存。\r\n"
    L"同一文件左右两侧各有一份改动时，这里只取消已暂存的那一份，工作区里的最新内容不受影响。\r\n"
    L"冲突（未合并）条目不会被取消暂存：那不等于解决冲突，程序会拒绝并说明原因；"
    L"内容只存在于索引里（磁盘上已没有那个文件）的条目会先弹出确认。";
constexpr std::wstring_view kTipRefresh =
    L"重新读取当前仓库的摘要、两个更改列表与提交历史：只在本程序后台执行只读 Git 查询，不弹出命令窗口。\r\n"
    L"在外部终端里改过仓库、或命令窗口里的 Git 已结束，都可以点这里恢复真实状态。";
constexpr std::wstring_view kTipCreateCommit =
    L"把“已暂存的更改”里现在的索引内容提交出去：在新命令窗口里执行 git commit，显示真实命令与输出，"
    L"执行结束后窗口保留，本程序自动重读仓库状态。\r\n"
    L"点击后先做一次只读重读（仓库摘要 + git status），确认框里摆出的是刚刚读回的现状："
    L"提交范围、提交信息、两个身份与两个时间；界面原先显示的东西若已经变了，会先说明以刚读回的为准。\r\n"
    L"只提交索引里那一份：绝不带 -a，也不会替你暂存任何文件；未暂存的改动留在工作区。\r\n"
    L"提交信息走临时 UTF-8 文件（git commit -F），正文不进命令行；命令里带 --cleanup=verbatim，"
    L"所以你写的空行与行尾文字不会被 Git 或任何 commit.cleanup 设置收拾掉。\r\n"
    L"作者身份、作者时间与提交者时间只覆盖这一次 Git 子进程：不写你的环境变量，也不碰任何配置文件；"
    L"提交者身份仍由这个仓库的有效 Git 配置决定。\r\n"
    L"命令里没有 --no-verify：仓库的 hooks 与签名设置照常生效，需要口令时由命令窗口自己提问。\r\n"
    L"正在合并／变基／拣选等流程没走完时本程序不做提交，也不在提交这一步里替你终止那种流程"
    L"（要看清或收尾那个流程，用的是「查看冲突状态」「继续该流程」「中止该流程」，每一步都要你确认）。";
constexpr std::wstring_view kTipUndoCommit =
    L"把当前分支引用挪回最近一次提交的父提交：只移动你确认的那一个完整分支引用（refs/heads/…），"
    L"索引与工作区一个字节都不动，原提交的改动会表现为“已暂存的更改”。\r\n"
    L"执行的是带预期旧值的原子引用更新 git update-ref <分支> <父完整ID> <原完整ID>：Git 只在分支"
    L"确实还指向原完整 ID 时才动它，对不上就非 0 拒绝；不再用 git reset --soft（它没有这个前提），"
    L"也不用可变的 HEAD 当目标名（两个分支可以指向同一个提交）。\r\n"
    L"这不是 revert（不生成反向提交），更不是 hard reset（不丢弃任何改动），也绝不触碰远端。\r\n"
    L"点击后先在后台重读分支、HEAD 完整 ID、父提交、提交对象自己记录的 parent 行、这个仓库是不是"
    L"浅仓库、目标父对象在本地读不读得到、远端跟踪引用与工作区状态（全部只读），确认框摆出的是刚刚"
    L"读回的现状；本地引用显示该提交可能已推送、判断不了发布状态、这是合并提交、或仓库是浅仓库时，"
    L"需要你明确点“强制撤回（仅本地）”才执行——那也只是确认风险，命令不变。\r\n"
    L"父关系在两份证据之间对不上（浅仓库的历史边界会把父关系藏起来）或那个父对象读不到时明确拒绝，"
    L"并说明要先补全历史；本程序不联网、不自动 unshallow。\r\n"
    L"只有对象与历史视图都确认没有父提交、且仓库明确不是浅仓库时，才算真正的第一个提交，"
    L"才走同样带预期旧值的 git update-ref -d <分支> <完整ID>：分支回到“尚无提交”，全部改动留在暂存区。\r\n"
    L"游离 HEAD、合并／变基进行中、有未解决冲突时明确拒绝；确认之后、执行之前还会再核对一次"
    L"HEAD 与分支，变了就取消并刷新。撤回后确认框与结果里都留着原提交完整 ID 与按它的找回命令，"
    L"本程序不自动恢复。";
constexpr std::wstring_view kTipPush =
    L"把当前分支送到它上游所对应的那个远端分支（git push）。\r\n"
    L"点击后先在后台只读问清：在哪个分支、要推哪一份提交、上游是谁、这次实际推给哪个远端的哪个地址、"
    L"本地相对上一次抓取领先几个（只读查询，不弹命令窗口、不接触任何远端），问回来后把"
    L"源分支 / 要推的完整提交 ID / 目标远端与逐条展开的发布 URL / 目标分支一起摆给你确认。\r\n"
    L"命令带完整的显式 refspec，源侧写死你确认的那份完整提交 ID（不写会变的分支名），目标侧是完整"
    L"引用，并且写死 --recurse-submodules=no：push.default、remote.<远端>.push、push.followTags、"
    L"remote.<远端>.tagOpt 都不会把这次的范围扩大；仓库里配了 remote.<远端>.mirror 时只为这一个"
    L"子进程临时置 false（实测它会让带 refspec 的 push 直接被 Git 拒绝）。\r\n"
    L"发布 URL 由 `git remote get-url --push --all` 让 Git 自己逐条展开（pushurl 优先、insteadOf/"
    L"pushInsteadOf 改写已叠好）；展开不成时直接拒绝，不拿配置里的原样地址顶替。\r\n"
    L"绝不带 --force / --force-with-lease / --mirror / --all / --tags，也不推标签、不动子模块；"
    L"Git 认为不是快进时就会把它拒绝，本程序不会为了让它“成功”而更激烈。\r\n"
    L"没有上游、游离 HEAD、分支还没有提交、发布目标问不出一个明确地址时一律拒绝，并给出具体原因："
    L"不猜 origin、不替你创建远端分支、不写任何配置文件。\r\n"
    L"分支还没有上游而仓库里有配好地址的远端时，改走「首次推送」：先逐个远端问出 Git 自己展开的实际"
    L"发布地址（本地只读），再让你当场选目标远端、输入目标分支名（这个名字交给 git check-ref-format "
    L"裁定，不合格就不发任何命令）、决定要不要设上游，然后向每个发布地址只读问一次「那条引用在不在」。"
    L"对端已有那条引用时明确列成风险：仍然不带 --force，非快进会被 Git 拒绝。\r\n"
    L"「推送」与「设置上游」是两步各自报告的事：上游写入是两条 git config（branch.<分支>.remote、"
    L".merge），推送确实成功后才逐条在命令窗口里跑，一条一个退出码；没写成就停下并说清写到哪一条，"
    L"不自动重发、不自动回退。取消或推送失败时一个配置都不写。\r\n"
    L"branch.<分支>.pushRemote / remote.pushDefault / 独立 push URL / url.*.insteadOf 让实际发布"
    L"地点与抓取的那一侧不同时，会被解析出来明确展示并要求你明确点头。\r\n"
    L"命令窗口报告结束后，还会向确认框上列出的那些**发布目标**逐个发只读 ls-remote，核对那条引用"
    L"到底停在哪：推送成功与否以那份实况为准，本地引用看起来一致不算数；命令报成功却没核上时，"
    L"结论写「已推送但未核实」或「与预期不符」，不会自动重推。";
constexpr std::wstring_view kTipConflictView =
    L"只读地看清这个仓库此刻停在什么状态：Git 目录里的流程痕迹（MERGE_HEAD、rebase-merge\\、"
    L"rebase-apply\\、CHERRY_PICK_HEAD、REVERT_HEAD、BISECT_LOG、SQUASH_MSG、index.lock 等）、"
    L"索引里还有哪些未合并条目、当前分支与 HEAD 现在在哪（git diff --diff-filter=U、symbolic-ref、"
    L"rev-parse，全部后台只读查询，不弹命令窗口、不改动仓库）。\r\n"
    L"展示的是：流程类型、当前分支与目标（合并进来哪一份提交、变基要把哪条分支落到哪、走到第几步）、"
    L"未合并的文件（超过 8 个只报个数并列前 8 个）、继续的前提，以及「继续」「中止」现在各自"
    L"可用还是不可用、为什么。\r\n"
    L"认不出或不一致的形态一律明说：多种痕迹并排、只有 rebase-apply\\ 却读不回 head-name/onto"
    L"（分不清是变基还是 git am）、只有 SQUASH_MSG 没有 MERGE_HEAD、停在二分定位——"
    L"这些都只报告看到了什么，不猜一种「大概能恢复」的做法，也不发任何命令。\r\n"
    L"没探到 Git 目录时说的是「问不到」，绝不把「看不见」当成「没有流程停着」。";
constexpr std::wstring_view kTipConflictContinue =
    L"把已经停着的那个流程的 git --continue 交进命令窗口：merge / rebase / cherry-pick / revert "
    L"按刚才读回的痕迹选那一个，本程序不猜该用哪一个。\r\n"
    L"这一步是「由 Git 建立提交」的那一步：Git 文档写明 merge --continue 会先确认真有中断中的合并，"
    L"然后调用 git commit，提交内容就是你当前暂存区里的那一份树。\r\n"
    L"没有 --no-verify，也没有 --no-edit 或 -m：钩子（按文档这一步跑的是 pre-commit 与 commit-msg，"
    L"prepare-commit-msg 本来也不受 --no-verify 影响）、提交签名（commit.gpgsign）与是否打开编辑器，"
    L"全部按 Git 自己的规则与你的配置生效；钩子失败或取消编辑器都会让这一步以非 0 收场，"
    L"流程痕迹原样留着。变基的合并后端按文档会在 --continue 时打开编辑器让你改提交说明。\r\n"
    L"索引里还有未合并条目时不发这条命令，也不谎称可以继续——先把它们解决并暂存"
    L"（解决内容由你自己写：本程序没有内置合并器，不替你选 ours/theirs，也不替你 add 任何文件）。\r\n"
    L"点击后的顺序：后台读现场 → 判读成方案 → 确认框（写明由 Git 建立提交与上述后果）→ "
    L"点头之后再把同一组只读查询原样重发一遍核对现场 → 对得上才启动命令窗口。"
    L"现场变了就取消并刷新；外部终端已经把那个流程走完或中止时，会明说「已经没有痕迹」，"
    L"不会把一条对着空气的 --continue 发出去。\r\n"
    L"命令没做成时：现场原样留着并读回来如实说明——不 reset --hard、不 clean、不 stash、"
    L"不删锁、不自动重试，你写在提交表单里的字一个字不动。";
constexpr std::wstring_view kTipConflictAbort =
    L"把已经停着的那个流程的 git --abort 交进命令窗口：merge / rebase / cherry-pick / revert "
    L"按刚才读回的痕迹选那一个。\r\n"
    L"这一步会改动你的工作区与索引，不是只读操作：按 Git 文档，merge --abort 是「终止当前的冲突"
    L"解决过程，试图重建合并开始前的状态」（MERGE_HEAD 存在时等同于 git reset --merge，"
    L"有 MERGE_AUTOSTASH 时改为把那份自动 stash 应用回工作区），文档还明确写着合并开始前就存在的"
    L"未提交改动在某些情况下无法重建；rebase --abort 把 HEAD 复位回原来的分支，已经重放出来的那些"
    L"提交不再被那条引用指向；cherry-pick/revert 的 --abort 取消整条序列并回到序列开始前的状态。\r\n"
    L"你在冲突文件里已经写好的解决内容会随之丢弃。因此这一条走「风险确认」框，逐条写明之后必须由"
    L"你亲手点确认：绝不后台自动执行，也不作为任何失败之后的「自动恢复」——"
    L"本程序不 stash、不 clean、不 reset --hard、不删 index.lock。\r\n"
    L"只想撤掉流程痕迹而保住工作区里已经写好的东西，Git 那边另有 --quit（文档写明它留下索引与"
    L"工作区不动）；那不是这个按钮做的事，本程序不代为执行。\r\n"
    L"仓库里没有流程痕迹时不会生成任何 --abort 命令（那种命令只会被 Git 当场拒绝，"
    L"本程序不拿它去试）；不一致或认不出的形态同样拒绝，只报告看到了什么。";
constexpr std::wstring_view kTipEnterSubmodule =
    L"把界面绑定的仓库换成所选子模块自己的工作区（git 状态、最近提交、作者默认值都按那个仓库读）。\r\n"
    L"点下去先在后台只读核对三件事：那个目录在不在、Git 认不认它是**当前这个**父仓库登记的子模块"
    L"（--show-superproject-working-tree 答的必须是现在这个父仓库）、父索引里那条记录是不是 160000 gitlink。\r\n"
    L"任何一条不过就明确拒绝并说清原因：\r\n"
    L"  · 目录不存在 → 说“不见了”，不替你把东西找回来；\r\n"
    L"  · 目录在但 Git 沿它往上找到的是父仓库本身 → 那是子模块还没有初始化；初始化会联网并改动"
    L"工作区，绝不在点导航时隐式触发；\r\n"
    L"  · 那是一个独立仓库、或父仓库对不上（被移动过、另一份克隆）→ 来历不同，不进去；\r\n"
    L"  · 裸仓库 / .git 内部 / 识别问不成 → 照实拒绝。\r\n"
    L"父仓库界面上那份未提交的表单草稿会在切换前收进代管（按工作区根记），回到那个仓库时原样交还；"
    L"交还前若界面上又写了新东西，会先问一句，两份都不会被悄悄覆盖。\r\n"
    L"这一步不改父仓库索引、不产生提交、不访问远端，也不会递归处理任何子模块。\r\n"
    L"提醒：提交子模块内部的内容与提交父仓库的指针是两个不同仓库里的两个操作；"
    L"父仓库显示干净不证明子模块那次提交已经发布。";
constexpr std::wstring_view kTipReturnToParent =
    L"把界面绑定的仓库退回刚才那个父仓库（嵌套子模块就一层一层退）。\r\n"
    L"只有当前绑定的确实是栈顶记下的那个子模块时才允许返回，否则一句拒绝——"
    L"把父仓库那份草稿交到一个不相干的仓库上，比不返回更糟；代管内容一律原样留着。\r\n"
    L"回去之后自动做一次只读核对：父索引里那条 gitlink、父提交里记的那一份、"
    L"以及子模块自己现在的 HEAD，然后按结果提示下一步该由谁做：\r\n"
    L"  · 三者一致 → 父仓库这一条显示干净（同时提醒：干净不等于已发布）；\r\n"
    L"  · 索引与子模块一致、父提交还旧 → 指针已暂存，等的是在父仓库创建提交；\r\n"
    L"  · 索引与子模块不一致 → 该由你选中那一条点“加入暂存区”，本程序不替你 add；\r\n"
    L"  · 问不成、或那条记录已经不是 gitlink → 说“没问全”，不猜也不说干净。\r\n"
    L"整个过程不 add、不 commit、不 push、不联网，也不递归处理别的子模块。";
constexpr std::wstring_view kTipUnstagedList =
    L"未暂存的更改来自只读的 git status（porcelain v2，机器可读格式）：\r\n"
    L"包含已跟踪文件的修改/删除/重命名、未跟踪文件（目录已展开为单个文件），以及待解决的冲突项。\r\n"
    L"被 Git 忽略的文件不列出，也不会被强制加入。\r\n"
    L"同一文件可以同时出现在左右两侧：暂存一次修改后继续编辑，两侧各显示自己的状态。\r\n"
    L"“子模块”条目只表示父仓库记录的提交指针与子模块内部状态；在父仓库暂存子模块不会提交它内部的任何文件。\r\n"
    L"\r\n"
    L"双击某一行：在命令窗口里执行 git diff（工作区相对索引），只用这一条路径限定范围。\r\n"
    L"未跟踪文件没有正常差异，改用 git diff --no-index 对照 /dev/null 直接把内容显示在窗口里；"
    L"二进制文件由 Git 给出“Binary files differ”的说明，不会把字节当文本倒出来。";
constexpr std::wstring_view kTipStagedList =
    L"已暂存的更改是索引相对最近一次提交的差异，同样来自只读的 git status。\r\n"
    L"创建提交时只会写入这里的内容；未暂存那一侧的改动仍留在工作区。\r\n"
    L"“重命名”条目会显示“旧路径 → 新路径”，内部同时保留两个原始路径。\r\n"
    L"\r\n"
    L"双击某一行：在命令窗口里执行 git diff --cached（索引相对 HEAD），只用这一条路径限定范围；"
    L"重命名条目会同时带上旧路径，否则 Git 只能把它显示成“新增文件”。\r\n"
    L"同一个文件在左右两侧的同名条目各对应自己那一份改动，两边看到的内容不一样。"
    L"仓库还没有任何提交时以空树为基准，暂存的新文件照样能看。";
constexpr std::wstring_view kTipHistoryList =
    L"最近提交是从当前 HEAD 可达的历史（只读 git log，默认 detached HEAD 也一样读），"
    L"最多 100 条：更早的历史不读取，摘要里会注明已达上限。\r\n"
    L"四列依次是短 ID（只显示前 8 位，命令一律用条目里保存的完整对象 ID）、标题、作者与作者时间；"
    L"时间按本机时区显示，中文、制表符与任何长度的标题都不参与字段切分。\r\n"
    L"\r\n"
    L"双击某一行：在命令窗口里执行 git show，显示这条提交的完整元数据（--format=fuller）、"
    L"正文与差异；合并提交的组合差异可能为空，那是正常的。\r\n"
    L"这一列是只读的：不提供、也不会顺手执行 checkout/reset/revert 之类的历史改写。";
constexpr std::wstring_view kTipSummary =
    L"标题：单行、必填，不能全是空白。\r\n"
    L"不要求任何前缀格式（feat:/fix: 之类一概不检查），程序也不会替你改写、翻译或润色文字。\r\n"
    L"校验结论即时写在上方“任务状态”那一行。";
constexpr std::wstring_view kTipDescription =
    L"描述：可以空着，也可以多行（Enter 换行）。\r\n"
    L"合成提交信息时的排版是：标题、空一行、描述、空一行、合作者 trailer；"
    L"描述中间的空行与行尾文字原样保留。\r\n"
    L"行尾统一按 LF 处理（界面里输入的是 CRLF，不会带进提交信息）。";
constexpr std::wstring_view kTipAuthor =
    L"作者：写成「姓名 <邮箱>」。\r\n"
    L"初值取自这个仓库的有效 Git 配置（user.name / user.email），优先序由 Git 自己决定："
    L"仓库本体的 .git/config 优先生效，其次才是用户与系统配置，includeIf 也一样认。\r\n"
    L"在这里改动只影响这一次提交，本程序不会写回你的任何 Git 配置文件。\r\n"
    L"提交者身份仍由有效 Git 配置决定，不在这里输入；配置凑不出完整身份时会在这里提示你。";
constexpr std::wstring_view kTipCoauthor =
    L"合作者：可以一条也没有，也可以多条。\r\n"
    L"添加／修改用输入框（Enter 确定，Esc 取消），删除按列表里的选中项（可多选）。\r\n"
    L"每条同样要写成「姓名 <邮箱>」；格式不对时输入框会说明原因并保持打开，"
    L"不会把坏条目收进列表。\r\n"
    L"每人一条标准 trailer：Co-authored-by: 姓名 <邮箱>，排在提交信息最末。";
constexpr std::wstring_view kTipCoauthorList =
    L"双击某一行可以修改那一条合作者。\r\n"
    L"同一位（姓名相同、邮箱大小写不同也算同一位）在列表里重复添加会被拒绝；"
    L"提交信息里的重复判定只看描述的最后一段，规则见“描述”的说明。";
constexpr std::wstring_view kTipDateInput =
    L"可用键盘直接输入年、月、日；范围限于 1970 到 2099 年（Git 实测收不下更早或更晚的时刻）。";
constexpr std::wstring_view kTipClockInput =
    L"可用键盘直接输入时、分、秒。右侧说明里的偏移是这个日期时间在本机实际生效的 UTC 偏移"
    L"（夏令时前后可能不同），交给 Git 的值带着它，因此不会按“读它的那个进程的时区”被挪动。";
constexpr std::wstring_view kTipTimeSync =
    L"勾选（默认）：提交者时间跟着作者时间走，上面那两块控件同时置灰，改一处就行。\r\n"
    L"取消勾选：提交者时间可以自己单独设，例如让提交时刻晚于作者时刻。\r\n"
    L"这里的“提交者时间”就是 GIT_COMMITTER_DATE，只覆盖这一次提交。";
constexpr std::wstring_view kTipTimeReset =
    L"把作者时间与提交者时间都改回此刻，并清掉“你亲手改过时间”的记号。\r\n"
    L"没人改过时间时，创建提交用的就是提交那一刻的时间（不是程序启动时的时刻）；"
    L"手动改过之后，那份时间会一直保留到点这个按钮或提交成功为止。";

// ---- 持久化（可选新增功能）的界面文字 ----
// 这三句都要把「存了什么、存在哪、怎么删」说全：草稿可能含私人内容，位置必须问得出。
constexpr std::wstring_view kTipPersistRecords =
    L"总开关：保存最近打开的仓库、验证过的 Git 程序路径、窗口位置大小与列宽，"
    L"以及（若“保存草稿”也开着）每个仓库一份未提交的提交表单草稿。\r\n"
    L"保存位置在当前用户的应用数据目录（%APPDATA%\\EvernightCommit\\state.prefs），"
    L"不写进项目目录、不写 Git 配置、不写任何口令或环境变量。\r\n"
    L"取消勾选后程序不再读写这些记录（已有文件保留，可用“清除已存记录”删掉）。";
constexpr std::wstring_view kTipPersistDrafts =
    L"只管提交表单草稿（标题/描述/作者/合作者与挑选的时间）这一类内容。\r\n"
    L"草稿是你自己写的文字，可能存在私人内容；取消勾选会立刻把已保存的草稿从记录文件里删掉"
    L"（屏幕上的字不动）。按仓库的工作区身份分开保存，同一项目的不同 worktree 互不覆盖。";
constexpr std::wstring_view kTipClearPrefs =
    L"删除记录文件里的全部用户内容：最近仓库、Git 程序路径、窗口布局与所有草稿，"
    L"对这台机器上所有 EvernightCommit 窗口生效。两个开关的选择会保留。\r\n"
    L"不改动 Git 仓库、你的 Git 配置或屏幕上正在输入的内容。";

// 首次使用说明：把保存内容、存储位置、不含之物与反悔入口一次说清。
constexpr std::wstring_view kConsentTitle = L"EvernightCommit：是否保存使用记录？";
constexpr std::wstring_view kConsentBodyHead =
    L"EvernightCommit 想把下面几类信息保存到当前用户的应用数据目录，用来减少重复输入、"
    L"并在程序异常退出后找回没提交的草稿：\r\n"
    L"  · 最近打开的仓库（工作区目录）\r\n"
    L"  · 验证过的 Git 程序路径\r\n"
    L"  · 窗口位置、大小与两个列宽比例\r\n"
    L"  · 每个仓库一份未提交的提交表单草稿（标题、描述、作者、合作者与所选时间）\r\n"
    L"\r\n";
constexpr std::wstring_view kConsentBodyTail =
    L"\r\n"
    L"不会保存：口令、令牌、带凭据的 URL、进程环境变量。草稿是你自己写的文字，"
    L"可能存在私人内容——界面右下角有“保存记录”“保存草稿”两个开关和“清除已存记录”按钮，"
    L"也可以随时手动删除上面那个文件。\r\n"
    L"\r\n"
    L"是：全部保存（含草稿）\r\n"
    L"否：只保存仓库、Git 程序与窗口布局，不保存草稿\r\n"
    L"取消：这次运行什么都不保存（下次启动会再问一次）\r\n";

// 切換倉庫時表單內容的去留：只有明確選「否」才會丟棄使用者打過的字，
// 「取消」與關窗口都按保留處理——丟棄是不可逆的，預設值必須落在安全的那一邊。
constexpr std::wstring_view kFormSwitchTitle = L"切换仓库：提交表单里已有你输入的内容";
constexpr std::wstring_view kFormSwitchQuestion =
    L"仓库要换了，但标题/描述/作者/合作者里还有你输入的内容。\r\n"
    L"\r\n"
    L"是：保留这些内容（作者保留下来后不再被新仓库的默认值覆盖）\r\n"
    L"否：放弃这些内容，改用新仓库的有效 Git 配置重新填作者（作者与提交者时间也回到此刻）\r\n"
    L"取消：先什么都不改（内容同样保留，稍后自己调整）\r\n";

constexpr std::wstring_view kRepoInputPlaceholder = L"（未设置本地仓库路径）";

// 没有选中任何条目时的说明。这一句必须把「没有选中 ≠ 处理全部」讲明白，
// 否则用户会以为空选择是一次“全量暂存/全量取消暂存”的快捷写法。
constexpr std::wstring_view kNoSelectionHint =
    L"没有选中任何条目，因此没有执行任何 Git 命令。“加入暂存区 →”只处理选中的行，"
    L"绝不会代为暂存整个仓库。请在“未暂存的更改”里点选一行或多行（Ctrl/Shift 可多选）后再点击。";
constexpr std::wstring_view kNoSelectionHintUnstage =
    L"没有选中任何条目，因此没有执行任何 Git 命令。“← 移出暂存区”只处理选中的行，"
    L"绝不会代为取消整个仓库的暂存（不带路径的 git reset 会连 HEAD 一起动，本程序不使用）。"
    L"请在“已暂存的更改”里点选一行或多行（Ctrl/Shift 可多选）后再点击。";

constexpr std::wstring_view kPickRepoTitle = L"选择本地仓库目录";
constexpr std::wstring_view kPickGitTitle = L"选择 Git 程序（git.exe）";

// 从本程序资源中加载应用图标。窗口类在创建窗口之前注册，此时拿不到窗口 DPI，
// 因此按系统度量取尺寸（大图标取 SM_CXICON，小图标取 SM_CXSMICON）。
HICON LoadAppIcon(HINSTANCE instance, bool largest) {
  const int cx = largest ? 0 : ::GetSystemMetrics(SM_CXSMICON);
  const int cy = largest ? 0 : ::GetSystemMetrics(SM_CYSMICON);
  const UINT flags = largest ? (LR_DEFAULTSIZE | LR_DEFAULTCOLOR) : LR_DEFAULTCOLOR;
  return static_cast<HICON>(
      ::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, cx, cy, flags));
}

// 程序改写“本地仓库”输入框时抑制 EN_CHANGE 通知，避免自己触发的文本变更又被当成一次新的用户选择。
// 标志恒为“作用域内为 true、出作用域复位”，中途抛异常也不会泄漏成永久抑制。
class SuppressRepoEditNotify {
public:
  explicit SuppressRepoEditNotify(bool& flag) : flag_(flag) { flag_ = true; }
  SuppressRepoEditNotify(const SuppressRepoEditNotify&) = delete;
  SuppressRepoEditNotify& operator=(const SuppressRepoEditNotify&) = delete;
  ~SuppressRepoEditNotify() { flag_ = false; }

private:
  bool& flag_;
};

// 同一套抑制規則的提交表單版：程序把「作者」默認值填進輸入框時，EN_CHANGE 一樣會發上來，
// 若不加抑制，這個自動填入的值就會被記成「使用者親手寫的內容」，
// 之後換倉庫時既不會更新默認值、又會多問一次「要不要保留」。
class SuppressCommitFormNotify {
public:
  explicit SuppressCommitFormNotify(bool& flag) : flag_(flag) { flag_ = true; }
  SuppressCommitFormNotify(const SuppressCommitFormNotify&) = delete;
  SuppressCommitFormNotify& operator=(const SuppressCommitFormNotify&) = delete;
  ~SuppressCommitFormNotify() { flag_ = false; }

private:
  bool& flag_;
};

}  // namespace

bool MainWindow::RegisterWindowClass(HINSTANCE instance) {
  WNDCLASSEXW existing{sizeof(existing)};
  if (::GetClassInfoExW(instance, kMainWindowWindowClass, &existing) != 0) {
    return true;
  }
  WNDCLASSEXW description{};
  description.cbSize = sizeof(description);
  description.style = CS_HREDRAW | CS_VREDRAW;
  description.lpfnWndProc = &MainWindow::Thunk;
  description.hInstance = instance;
  description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // 窗口与任务栏图标；资源缺失时 hIcon/hIconSm 为 nullptr，系统回退到默认图标。
  description.hIcon = LoadAppIcon(instance, /*largest=*/true);
  description.hIconSm = LoadAppIcon(instance, /*largest=*/false);
  description.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
  description.lpszClassName = kMainWindowWindowClass;
  return ::RegisterClassExW(&description) != 0;
}

void MainWindow::UnregisterWindowClass(HINSTANCE instance) {
  ::UnregisterClassW(kMainWindowWindowClass, instance);
}

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
  if (!RegisterWindowClass(instance) || !Splitter::RegisterWindowClass(instance)) {
    return false;
  }
  // 先读记录再建窗口：窗口几何要按上次的位置出现，不能建完再跳一下。
  // 读取本身不写任何东西（目录都没创建），首次使用因此完全无痕。
  LoadPersistedPreferences();
  // 操作历史与偏好各自独立成文件；同样在创建窗口之前读，读取本身不写任何东西（首次完全无痕）。
  LoadOperationHistory();
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  int width = metrics_.Scale(kInitialWindowWidth);
  int height = metrics_.Scale(kInitialWindowHeight);
  if (PersistenceApplicable() && prefs_.window.valid &&
      platform::IsWindowRectReachable(prefs_.window.x, prefs_.window.y, prefs_.window.width,
                                      prefs_.window.height)) {
    // 恢复上次位置只是「摆在哪」，不是任何授权；可达性（还落在某台显示器上）不成立就回退默认。
    x = prefs_.window.x;
    y = prefs_.window.y;
    width = prefs_.window.width;
    height = prefs_.window.height;
  }
  // WS_CLIPCHILDREN：客户区里被子控件占住的格子不归父窗口擦。没有这一条，父窗口每次擦背景都会把
  // 分组框、列表这些子控件的区域先刷成一片底色，等子控件自己重绘——用户看到的就是一块块矩形闪。
  HWND created = ::CreateWindowExW(0, kMainWindowWindowClass, kWindowTitle,
                                   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, nullptr, nullptr,
                                   instance, this);
  if (created == nullptr) {
    return false;
  }
  window_.Reset(created);
  ::ShowWindow(created, showCommand);
  if (PersistenceApplicable() && prefs_.window.valid && prefs_.window.maximized &&
      (showCommand == SW_SHOWDEFAULT || showCommand == SW_SHOWNORMAL)) {
    ::ShowWindow(created, SW_SHOWMAXIMIZED);  // 上次是最大化关的，这次也按最大化出现
  }
  ::UpdateWindow(created);
  return true;
}

void MainWindow::OnCreate(HWND window) {
  metrics_.UpdateForDpi(DpiForWindowOrSystem(window));
  const bool restoredGeometry = PersistenceApplicable() && prefs_.window.valid &&
                                platform::IsWindowRectReachable(prefs_.window.x, prefs_.window.y,
                                                                prefs_.window.width, prefs_.window.height);
  if (!restoredGeometry) {
    ::SetWindowPos(window, nullptr, 0, 0, metrics_.Scale(kInitialWindowWidth),
                   metrics_.Scale(kInitialWindowHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  // 上次拖过的两列宽度直接接进布局参数（UpdateLayoutSpecs 只改 DPI 尺寸、不动比例）。
  if (PersistenceApplicable() && prefs_.columns.valid) {
    changesSpec_.leftRatio = static_cast<double>(prefs_.columns.leftPermille) / 1000.0;
    changesSpec_.middleRatio = static_cast<double>(prefs_.columns.middlePermille) / 1000.0;
  }

  repoBar_.Create(window);
  infoBar_.Create(window);
  changesPane_.Create(window);
  leftSplitter_.Create(window, kSplitterLeftId);
  rightSplitter_.Create(window, kSplitterRightId);
  commitForm_.Create(window);
  actionBar_.Create(window);

  programInfo_ = L"EvernightCommit 界面骨架 v" + platform::Utf8ToUtf16(GC_VERSION_STRING) + L"（" +
                 platform::Utf8ToUtf16(GC_BUILD_TYPE) + L"）";
  const SYSTEMTIME now = platform::CurrentLocalTime();
  commitForm_.SetTimes(now, now);
  // 本机时区那句说明与「时间同步修改」的联动都在这一步落地：控件刚设完初值，
  // 偏移要按那一段时刻算，提交者控件的可用状态要跟勾选项一致。
  RefreshTimeControlsState(window);

  ApplyFonts(window);
  tooltips_.Create(window, metrics_.Font());
  RegisterTooltips();
  // 两个开关按读回来的策略显示（BM_SETCHECK 不发 BN_CLICKED，不会被当成用户刚点过）。
  actionBar_.SetPreferenceChecks(prefs_.persistenceEnabled, prefs_.draftSavingEnabled);
  // 历史开关按读回来的策略显示（默认关；BM_SETCHECK 不发 BN_CLICKED，不会被当成用户刚点过）。
  ::SendMessageW(actionBar_.historyCheck(), BM_SETCHECK,
                 historyLog_.historyEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  UpdateCommandAvailability();
  InitializeRepoInput(window);
  RefreshTexts(window);
  ApplyWorkspaceLists();  // 首屏的空状态说明：列表不在 RefreshTexts 里重建，这里显式填一次。
  InitializeGitDetection(window);
  InitializeCommandWatching(window);
  if (!prefsLoadNote_.empty()) {
    state_.SetStatusNote(prefsLoadNote_);
    RefreshTexts(window);
  }
  if (!historyLoadNote_.empty()) {
    state_.SetStatusNote(historyLoadNote_);
    RefreshTexts(window);
  }
  if (prefsConsentPending_) {
    // WM_CREATE 里不弹模态框：创建完成、控件都就位之后再问。
    ::PostMessageW(window, kPersistentConsentNotice, 0, 0);
  }
}

void MainWindow::RegisterTooltips() {
  tooltips_.Add(repoBar_.repoEdit(), L"输入本地仓库目录（也可以是其子目录）；识别在后台只读执行，不会改动仓库。");
  tooltips_.Add(repoBar_.repoBrowse(), kTipBrowseRepo);
  tooltips_.Add(repoBar_.gitBrowse(), kTipBrowseGit);
  tooltips_.Add(repoBar_.fetchButton(), kTipFetch);
  tooltips_.Add(repoBar_.pullButton(), kTipPull);
  tooltips_.Add(repoBar_.statusButton(), kTipStatus);
  tooltips_.Add(changesPane_.stageAddButton(), kTipStageAdd);
  tooltips_.Add(changesPane_.stageRemoveButton(), kTipStageRemove);
  tooltips_.Add(changesPane_.unstagedList(), kTipUnstagedList);
  tooltips_.Add(changesPane_.stagedList(), kTipStagedList);
  tooltips_.Add(changesPane_.historyList(), kTipHistoryList);
  tooltips_.Add(commitForm_.SummaryEdit(), kTipSummary);
  tooltips_.Add(commitForm_.DescriptionEdit(), kTipDescription);
  tooltips_.Add(commitForm_.AuthorEdit(), kTipAuthor);
  tooltips_.Add(commitForm_.CoauthorList(), kTipCoauthorList);
  tooltips_.Add(commitForm_.CoauthorAdd(), kTipCoauthor);
  tooltips_.Add(commitForm_.CoauthorRemove(), kTipCoauthor);
  tooltips_.Add(commitForm_.SyncCheckbox(), kTipTimeSync);
  tooltips_.Add(commitForm_.TimeResetButton(), kTipTimeReset);
  tooltips_.Add(commitForm_.AuthorDate(), kTipDateInput);
  tooltips_.Add(commitForm_.AuthorClock(), kTipClockInput);
  tooltips_.Add(commitForm_.CommitterDate(), kTipDateInput);
  tooltips_.Add(commitForm_.CommitterClock(), kTipClockInput);
  tooltips_.Add(actionBar_.refreshButton(), kTipRefresh);
  tooltips_.Add(actionBar_.createCommitButton(), kTipCreateCommit);
  tooltips_.Add(actionBar_.undoCommitButton(), kTipUndoCommit);
  tooltips_.Add(actionBar_.pushButton(), kTipPush);
  tooltips_.Add(actionBar_.conflictViewButton(), kTipConflictView);
  tooltips_.Add(actionBar_.conflictContinueButton(), kTipConflictContinue);
  tooltips_.Add(actionBar_.conflictAbortButton(), kTipConflictAbort);
  tooltips_.Add(changesPane_.enterSubmoduleButton(), kTipEnterSubmodule);
  tooltips_.Add(changesPane_.returnToParentButton(), kTipReturnToParent);
  tooltips_.Add(actionBar_.persistRecordsCheck(), kTipPersistRecords);
  tooltips_.Add(actionBar_.persistDraftsCheck(), kTipPersistDrafts);
  tooltips_.Add(actionBar_.clearPrefsButton(), kTipClearPrefs);
  tooltips_.Add(
      actionBar_.historyCheck(),
      L"记录操作历史（可选，默认关）：勾选后才把这台机器上本程序对仓库做过的写操作留一条本地记录"
      L"（唯一 ID、时间、仓库工作区、操作类型、已确认的分支/引用与完整对象 ID、启动与终态、逐目标核实结果、"
      L"引用级恢复线索）。存在当前用户的 %APPDATA%\\EvernightCommit\\history.prefs，不进项目、不进 Git 配置。"
      L"不记文件内容、提交正文、凭据 URL、令牌、环境与原始输出。关上不删已有内容，要清空用「操作历史…」里的清除。");
  tooltips_.Add(
      actionBar_.historyBrowseButton(),
      L"操作历史…：查看这些记录，导出脱敏副本、查看存储位置、清除、调整保留期限/条数；"
      L"选中一条「引用级可回退」的记录可重新预检现状后确认恢复（走命令窗口、带预期旧值、只动一个本地引用）。"
      L"涉及已推送的远端历史只解释协作影响与复制命令，绝不自动 force push / hard reset / clean / 批量撤销。");
}

void MainWindow::UpdateCommandAvailability() {
  // 依赖 Git 的控件要同时满足：功能已接通、Git 程序验证可用、仓库已识别为可用工作区；
  // 任一条件失效（改正路径、切换仓库、识别失败、裸仓库）都会立即重新禁用。
  // 写操作还要再加一条：同一工作区同时只允许一个由本程序发起的操作，否则会互相抢仓库锁。
  // 「创建提交」「撤回最近提交」「fetch」「pull」「推送」各自有单独的接通开关：
  // 接一个不该连带放开别的（也便于日后单独退回某一步）。
  const auto readyFor = [this](bool implemented) {
    return BOOL(implemented && state_.GitUsable() && state_.RepoUsable() && !tasks_.OperationInFlight());
  };
  ::EnableWindow(actionBar_.pushButton(), readyFor(app::AppState::kPushImplemented));
  ::EnableWindow(actionBar_.createCommitButton(),
                 readyFor(app::AppState::kCreateCommitImplemented));
  ::EnableWindow(actionBar_.undoCommitButton(), readyFor(app::AppState::kUndoCommitImplemented));
  ::EnableWindow(repoBar_.fetchButton(), readyFor(app::AppState::kFetchImplemented));
  ::EnableWindow(repoBar_.pullButton(), readyFor(app::AppState::kPullImplemented));
  // 冲突与暂停流程的三个入口：与前面那几个同一套条件（功能已接通 + Git/仓库可用 + 命令窗口空闲）。
  // 刻意不拿「仓库里此刻有没有流程停着」去灰掉「继续/中止」：灰掉会让人以为功能坏了，
  // 而「没有流程」本来就是一句该由点下去之后给出的具体说明（子模块导航那两个按钮的既有做法相同）。
  // 判据仍然只有两份：准入判定看阶段，命令看不看得到痕迹由现场读取与方案层决定——
  // 没有痕迹时不会出现任何 --abort，还有未合并条目时不会出现任何 --continue。
  ::EnableWindow(actionBar_.conflictViewButton(),
                 readyFor(app::AppState::kConflictHandlingImplemented));
  ::EnableWindow(actionBar_.conflictContinueButton(),
                 readyFor(app::AppState::kConflictHandlingImplemented));
  ::EnableWindow(actionBar_.conflictAbortButton(),
                 readyFor(app::AppState::kConflictHandlingImplemented));
  // 合作者的增刪改只動表單文字，不碰倉庫、也不需要 Git 可用，因此常開。
  // （真正的規則檢查在 git/commit_identity 裡，在這裡點按鈕不會發出任何命令。）
  ::EnableWindow(commitForm_.CoauthorAdd(), TRUE);
  ::EnableWindow(commitForm_.CoauthorRemove(), TRUE);
  // 两个暂存方向都已接通，条件与 status 一致：Git 可用 + 仓库已识别 + 没有别的操作在跑。
  // 这里不要求“已选中条目”：没选中也要能点，点了才会得到那句“没有选中就是什么都不做”的说明；
  // 把按钮 disabled 掉反而让用户以为功能坏了。
  const BOOL stagingReady =
      (state_.GitUsable() && state_.RepoUsable() && !tasks_.OperationInFlight()) ? TRUE : FALSE;
  ::EnableWindow(changesPane_.stageAddButton(), stagingReady);
  ::EnableWindow(changesPane_.stageRemoveButton(), stagingReady);
  // 子模块导航的两个按钮同理：不拿「选没选中子模块」「栈里有没有来路」去禁用按钮——
  // 灰掉让人以为功能坏了，点下去得到那句具体说明才知道下一步该做什么。
  // 只有一条硬性条件：Git 可用（导航全靠只读查询），而且这一次导航本身没有在途
  // （两次切换叠在一起，先回来的那份就会被后一次的识别抢掉仓库绑定）。
  const BOOL navigationReady =
      (state_.GitUsable() && !submoduleFlow_.Active() && !tasks_.OperationInFlight()) ? TRUE : FALSE;
  ::EnableWindow(changesPane_.enterSubmoduleButton(), navigationReady);
  ::EnableWindow(changesPane_.returnToParentButton(), navigationReady);
  // 刷新是内部只读重读，不占用命令窗口、也不写仓库，因此在有操作在跑时依然可用：
  // 外部终端改了仓库、或某个操作的输出看不清时，用户总要能恢复真实状态。
  // 上一次识别失败也保持可用：仓库在外部被修好（或改回原样）后，点刷新就能恢复，
  // 不必先把路径清空再重选。
  const BOOL refreshReady = (state_.GitUsable() && !state_.Info().repoPath.empty()) ? TRUE : FALSE;
  ::EnableWindow(actionBar_.refreshButton(), refreshReady);
  // status 是首个接入命令窗口执行器的操作：除上述条件外，同一时刻只允许一个操作在跑，
  // 避免并发提交让“哪个窗口对应哪次操作”变得含糊。
  const BOOL statusReady =
      (state_.GitUsable() && state_.RepoUsable() && !tasks_.OperationInFlight()) ? TRUE : FALSE;
  ::EnableWindow(repoBar_.statusButton(), statusReady);
}

std::wstring MainWindow::TaskStatusNote() const {
  // 进行中/已完成的操作说明优先于常规任务状态：用户必须能看到“命令窗口里正在跑什么”。
  if (activeOperation_.serial != 0) {
    std::wstring status;
    if (commandRunner_.DescribeOperation(activeOperation_.runnerId, &status, nullptr)) {
      std::wstring text = L"命令窗口操作：" + status;
      // 查看类操作的范围说明要跟着一起显示：窗口里的输出可能只有“Binary files differ”，
      // 为什么不是全文、子模块为什么只显示指针，都靠这一句交代。
      if (!activeOperation_.scopeNotice.empty()) {
        text += L"｜" + activeOperation_.scopeNotice;
      }
      return text;
    }
    // 执行器已经不认识这个 ID：完成通知丢了或被别处回收。这里只说明状态无法确认，
    // 真正的结案（释放槽位 + 安排刷新）由 TickActiveOperations 负责。
    return L"命令窗口操作：" + activeOperation_.displayName + L" 的结果已无法确认，将重新读取仓库状态…";
  }
  if (tasks_.ReadInFlight()) {
    return tasks_.ReadStartedText();
  }
  return state_.StatusNote();
}

std::wstring MainWindow::OperationBanner() const {
  // 底部状态条：有任务在进行时用实时状态取代“功能未接入”的固定说明。
  if (activeOperation_.serial != 0 || tasks_.ReadInFlight()) {
    return TaskStatusNote();
  }
  // 读到子模块变化时，这条说明比固定提示更有价值：父仓库暂存的范围必须当场讲清楚。
  const std::wstring submoduleNote = state_.WorkspaceBanner();
  if (!submoduleNote.empty()) {
    return submoduleNote;
  }
  // 提交表单自己的说明（校验结论、合作者增删、作者默认值的来路）。放在这一条通道里，
  // 是为了让它不和「任务状态」那行的操作结论互相覆盖：两边都是使用者要看的，但看的时机不同。
  if (!state_.FormNote().empty()) {
    return state_.FormNote();
  }
  // 依赖 Git 的写操作（暂存/提交/撤回/fetch/pull/推送）全部接通了，这里没有「尚未接入」可说：
  // 任务状态那句就是使用者要看的结论。
  return state_.StatusNote();
}

void MainWindow::UpdateLayoutSpecs(HWND /*window*/) {
  bandSpec_ = BandSpec{};
  bandSpec_.repoBarHeight = RepoBar::MinimumHeight(metrics_);
  bandSpec_.infoRowHeight = RepoInfoBar::MinimumHeight(metrics_);
  bandSpec_.actionRowHeight = ActionBar::MinimumHeight(metrics_);
  bandSpec_.gap = metrics_.RowGap();
  bandSpec_.minChangesHeight = ChangesPane::MinimumHeight(metrics_);
  bandSpec_.minFormHeight = CommitForm::MinimumHeight(metrics_);
  bandSpec_.minWidth = std::max({repoBar_.MinimumWidth(metrics_), infoBar_.MinimumWidth(metrics_),
                                 actionBar_.MinimumWidth(metrics_)});

  // 分隔条比例是用户状态，只更新与 DPI 相关的尺寸，不重置比例。
  changesSpec_.splitterWidth = metrics_.SplitterWidth();
  changesSpec_.arrowColumnWidth = metrics_.ArrowColumnWidth();
  changesSpec_.minListWidth = metrics_.MinListWidth();
  changesSpec_.minArrowColumnWidth = metrics_.MinArrowColumnWidth();
  changesSpec_.gap = metrics_.ColGap();
}

void MainWindow::RefreshTexts(HWND window) {
  infoBar_.Refresh(L"仓库类型：" + state_.RepoTypeDisplay(), L"当前分支：" + state_.BranchDisplay(),
                   L"上游：" + state_.UpstreamDisplay(), L"任务状态：" + TaskStatusNote(), programInfo_);
  // 两个列表不在这里重建：本函数每 500 毫秒的状态轮询也会被调用，
  // 逐次清空再填入会让列表反复闪、选中项被冲掉。内容真正变化时由 ApplyWorkspaceLists 落地。
  actionBar_.SetStatus(OperationBanner());
  DoLayout(window);
}

void MainWindow::ApplyWorkspaceLists() {
  changesPane_.ShowWorkspace(state_.WorkspaceModel(), state_.WorkspaceHintTexts());
}

void MainWindow::ApplyFonts(HWND window) {
  ApplyFontToChildTree(window, metrics_.Font());
  if (tooltips_.handle() != nullptr) {
    ::SendMessageW(tooltips_.handle(), WM_SETFONT, reinterpret_cast<WPARAM>(metrics_.Font()), TRUE);
  }
}

void MainWindow::DoLayout(HWND window) {
  UpdateLayoutSpecs(window);

  RECT client{};
  if (::GetClientRect(window, &client) == 0) {
    return;
  }
  const int margin = metrics_.Margin();
  client.left += margin;
  client.top += margin;
  client.right -= margin;
  client.bottom -= margin;

  Bands bands{};
  ComputeBands(client, bandSpec_, &bands);

  repoBar_.Layout(bands.repoBar, metrics_);
  infoBar_.Layout(bands.infoRow, metrics_);

  changesArea_ = bands.changes;
  ChangesColumns columns{};
  ComputeChangesColumns(bands.changes, changesSpec_, &columns);
  leftSplitter_.SetBounds(columns.splitterLeft);
  rightSplitter_.SetBounds(columns.splitterRight);
  changesPane_.Layout(columns, metrics_);

  commitForm_.Layout(bands.commitForm, metrics_);
  actionBar_.Layout(bands.actions, metrics_);
}

SIZE MainWindow::MinimumWindowSize(HWND window) const {
  const MinimumClientSize minimum = MinimumClientFor(bandSpec_, changesSpec_);
  const int margin = metrics_.Margin();
  RECT frame{0, 0, minimum.width + 2 * margin, minimum.height + 2 * margin};
  ::AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE)), metrics_.Dpi());
  return SIZE{frame.right - frame.left, frame.bottom - frame.top};
}

void MainWindow::OnCommand(HWND window, WPARAM wParam) {
  const int commandId = LOWORD(wParam);
  const UINT notifyCode = HIWORD(wParam);
  switch (commandId) {
    case kIdRepoBrowse:
      BrowseRepoPath(window);
      break;
    case kIdGitBrowse:
      BrowseGitPath(window);
      break;
    case kIdGitCombo:
      OnGitComboNotify(window, notifyCode);
      break;
    case kIdRepoEdit:
      OnRepoEditNotify(window, notifyCode);
      break;
    case kIdStatusButton:
      if (notifyCode == BN_CLICKED) {
        LaunchStatusOperation(window);
      }
      break;
    case kIdFetchButton:
      if (notifyCode == BN_CLICKED) {
        RequestFetch(window);
      }
      break;
    case kIdPullButton:
      if (notifyCode == BN_CLICKED) {
        RequestPull(window);
      }
      break;
    case kIdConflictViewButton:
      if (notifyCode == BN_CLICKED) {
        ShowConflictState(window);
      }
      break;
    case kIdConflictContinueButton:
      if (notifyCode == BN_CLICKED) {
        ContinueConflictFlow(window);
      }
      break;
    case kIdConflictAbortButton:
      if (notifyCode == BN_CLICKED) {
        AbortConflictFlow(window);
      }
      break;
    case kIdRefreshButton:
      if (notifyCode == BN_CLICKED) {
        ScheduleRefresh(window);
      }
      break;
    case kIdStageAddButton:
      if (notifyCode == BN_CLICKED) {
        StageSelectedUnstaged(window);
      }
      break;
    case kIdStageRemoveButton:
      if (notifyCode == BN_CLICKED) {
        UnstageSelectedStaged(window);
      }
      break;
    case kIdEnterSubmoduleButton:
      if (notifyCode == BN_CLICKED) {
        EnterSubmodule(window);
      }
      break;
    case kIdReturnToParentButton:
      if (notifyCode == BN_CLICKED) {
        ReturnToParent(window);
      }
      break;
    case kIdSummaryEdit:
    case kIdDescriptionEdit:
    case kIdAuthorEdit:
      if (notifyCode == EN_CHANGE) {
        OnCommitFormEdit(window, commandId);
      }
      break;
    case kIdCoauthorAdd:
      if (notifyCode == BN_CLICKED) {
        AddCoauthor(window);
      }
      break;
    case kIdCoauthorRemove:
      if (notifyCode == BN_CLICKED) {
        RemoveSelectedCoauthors(window);
      }
      break;
    case kIdCreateCommitButton:
      if (notifyCode == BN_CLICKED) {
        CreateCommit(window);
      }
      break;
    case kIdUndoCommitButton:
      if (notifyCode == BN_CLICKED) {
        UndoLastCommit(window);
      }
      break;
    case kIdPushButton:
      if (notifyCode == BN_CLICKED) {
        RequestPush(window);
      }
      break;
    case kIdTimeResetButton:
      if (notifyCode == BN_CLICKED) {
        ResetCommitTimesToNow(window);
      }
      break;
    case kIdPersistRecordsCheck:
      if (notifyCode == BN_CLICKED) {
        const bool checked = ::SendMessageW(actionBar_.persistRecordsCheck(), BM_GETCHECK, 0, 0) ==
                             BST_CHECKED;
        OnPersistenceCheckClicked(window, commandId, checked);
      }
      break;
    case kIdPersistDraftsCheck:
      if (notifyCode == BN_CLICKED) {
        const bool checked = ::SendMessageW(actionBar_.persistDraftsCheck(), BM_GETCHECK, 0, 0) ==
                             BST_CHECKED;
        OnPersistenceCheckClicked(window, commandId, checked);
      }
      break;
    case kIdClearPrefsButton:
      if (notifyCode == BN_CLICKED) {
        ClearPersistedRecords(window);
      }
      break;
    case kIdHistoryCheck:
      if (notifyCode == BN_CLICKED) {
        const bool checked =
            ::SendMessageW(actionBar_.historyCheck(), BM_GETCHECK, 0, 0) == BST_CHECKED;
        OnHistoryCheckClicked(window, checked);
      }
      break;
    case kIdHistoryBrowseButton:
      if (notifyCode == BN_CLICKED) {
        ShowOperationHistory(window);
      }
      break;
    case kIdTimeSyncCheck:
      if (notifyCode == BN_CLICKED) {
        // 勾选状态由控件自己翻转，这里只负责把联动规则跟上（置灰哪一半、说明怎么写）。
        RefreshTimeControlsState(window);
        CaptureDraftIntoPreferences();
        state_.SetFormNote(commitForm_.TimeSyncChecked()
                               ? L"“时间同步修改”已勾选：提交者时间跟着作者时间，"
                                 L"上面那两块提交者时间控件已置灰。"
                               : L"“时间同步修改”已取消：作者时间与提交者时间各改各的，两块控件都可用。");
        RefreshTexts(window);
      }
      break;
    default:
      break;  // 其余按钮保持禁用，不会收到命令通知。
  }
}

void MainWindow::OnGitComboNotify(HWND window, UINT notifyCode) {
  switch (notifyCode) {
    case CBN_EDITCHANGE:
      // 键入逐字符通知：只重排防抖定时器，停顿后统一验证，避免每敲一键启动一个子进程。
      ::KillTimer(window, kGitVerifyTimer);
      ::SetTimer(window, kGitVerifyTimer, kGitVerifyDebounceMs, nullptr);
      break;
    case CBN_SELCHANGE:
    case CBN_KILLFOCUS:
      ::KillTimer(window, kGitVerifyTimer);
      CommitGitInput(window);
      break;
    default:
      break;
  }
}

void MainWindow::OnRepoEditNotify(HWND window, UINT notifyCode) {
  if (suppressRepoEditNotify_) {
    return;  // 程序改写输入框（绝对化、浏览回填）不是用户的新选择。
  }
  switch (notifyCode) {
    case EN_CHANGE:
      // 键入逐字符通知：只重排防抖定时器，停顿后统一识别，避免每敲一键发起一批 Git 查询。
      ::KillTimer(window, kRepoDetectTimer);
      ::SetTimer(window, kRepoDetectTimer, kRepoDetectDebounceMs, nullptr);
      break;
    case EN_KILLFOCUS:
      ::KillTimer(window, kRepoDetectTimer);
      CommitRepoInput(window);
      break;
    default:
      break;
  }
}

void MainWindow::BrowseRepoPath(HWND window) {
  std::wstring start = GetControlText(repoBar_.repoEdit());
  if (start.empty()) {
    start = platform::CurrentWorkingDirectory();
  }
  const auto picked = platform::BrowseForFolder(window, kPickRepoTitle, start);
  // 用户取消，或结果不是可用的文件系统路径时，不改动状态、也不给出成功提示。
  if (!picked.has_value() || picked->empty()) {
    return;
  }
  ::KillTimer(window, kRepoDetectTimer);  // 明确选择不再防抖。
  {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), *picked);
  }
  CommitRepoInput(window);
}

void MainWindow::InitializeRepoInput(HWND /*window*/) {
  // 初值：用户同意保存且记着最近仓库时用最近的那一个；否则退回启动工作目录（只读，不改进程目录）。
  // 注意这只是「填进输入框等待识别」：识别照样走完整的后台只读查询，上次有效不等于这次可用。
  std::wstring startupDirectory;
  if (PersistenceApplicable() && !prefs_.recentRepositories.empty()) {
    startupDirectory = prefs_.recentRepositories.front();
  } else {
    startupDirectory = platform::CurrentWorkingDirectory();
  }
  const std::wstring absolute = platform::ToAbsolutePath(startupDirectory);
  const std::wstring initial = absolute.empty() ? startupDirectory : absolute;
  {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), initial);
  }
  state_.SetRepoPath(initial);

  app::RepoState repo;
  repo.status = app::RepoLoadStatus::unloaded;
  if (initial.empty()) {
    repo.detection.error = git::RepoError::inputEmpty;
    state_.SetRepo(std::move(repo));
    state_.SetStatusNote(std::wstring(kRepoInputPlaceholder) + L"。" +
                         git::BuildRepoErrorDetail(repo.detection.error, {}));
    return;
  }
  // 识别要等 Git 程序验证通过才能开始（见 OnGitProbeCompleted），这里只挂起等待状态；
  // 不能假装“正在识别”，否则没有任何查询会把它结束掉。
  repo.detection.error = git::RepoError::gitUnavailable;
  state_.SetRepo(std::move(repo));
  state_.SetStatusNote(L"等待 Git 程序验证通过后识别仓库：" + initial);
}

void MainWindow::BrowseGitPath(HWND window) {
  std::wstring start = GetControlText(repoBar_.gitCombo());
  if (start.empty()) {
    start = platform::CurrentWorkingDirectory();
  }
  const auto picked = platform::BrowseForExecutable(window, kPickGitTitle, start);
  if (!picked.has_value() || picked->empty()) {
    return;
  }
  ::KillTimer(window, kGitVerifyTimer);  // 明确选择不再防抖。
  const std::wstring normalized = platform::NormalizeGitExeInput(*picked);
  SetControlText(repoBar_.gitCombo(), normalized);
  RequestGitVerification(window, normalized);
}

void MainWindow::InitializeGitDetection(HWND window) {
  // 启动即按当前进程 PATH 发现候选（效果对应 `where git` 的搜索意图，不扫盘、不读注册表）。
  std::vector<std::wstring> candidates = platform::DiscoverGitCandidates();
  // 上次验证过的 Git 路径放在最前——但「放在最前」不等于「直接当本次授权」：
  // 它照样要经过同一次后台 git --version 验证；文件已经不在原位置时明说并退回自动发现。
  if (PersistenceApplicable() && !prefs_.gitExecutablePath.empty()) {
    const std::wstring saved = prefs_.gitExecutablePath;
    if (platform::IsExistingRegularFile(saved)) {
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                      [&](const std::wstring& c) {
                                        return gc::git::PathsEqualFolded(c, saved);
                                      }),
                       candidates.end());
      candidates.insert(candidates.begin(), saved);
      repoBar_.SetGitCandidates(candidates);
      SetControlText(repoBar_.gitCombo(), saved);
      RequestGitVerification(window, saved);
      return;
    }
    state_.SetStatusNote(L"上次保存的 Git 程序路径已失效：" + saved +
                         L"。已退回按 PATH 自动发现，验证通过后会重新记住新路径。");
    gitPathInvalidNotice_ = saved;  // 验证结论回来时把这句话带上，不被「正在验证」淹没
  }
  repoBar_.SetGitCandidates(candidates);
  if (candidates.empty()) {
    const std::wstring message = L"当前进程 PATH 中未找到 git.exe，请手动输入路径或用“浏览…”选择。";
    app::GitToolState tool;
    tool.status = app::GitExeStatus::unverified;
    tool.message = message;
    state_.SetGitTool(std::move(tool));
    // 没有可用的 Git 就无从识别仓库；置为“Git 程序不可用”，用户补好路径后会自动补做识别。
    SetRepoFailed(window, state_.Info().repoPath, git::RepoError::gitUnavailable, message);
    return;
  }
  SetControlText(repoBar_.gitCombo(), candidates.front());
  RequestGitVerification(window, candidates.front());
}

void MainWindow::CommitGitInput(HWND window) {
  const std::wstring raw = GetControlText(repoBar_.gitCombo());
  const std::wstring normalized = platform::NormalizeGitExeInput(raw);
  if (normalized.empty()) {
    app::GitToolState tool;
    tool.status = app::GitExeStatus::unverified;
    tool.message = L"未选择 Git 程序。";
    state_.SetGitTool(std::move(tool));
    state_.SetStatusNote(L"Git 未设置：请输入 git.exe 路径或用“浏览…”选择。");
    UpdateCommandAvailability();
    RefreshTexts(window);
    return;
  }
  const app::GitToolState& current = state_.Git();
  if (normalized == current.path &&
      (current.status == app::GitExeStatus::verifying || current.status == app::GitExeStatus::verified)) {
    return;  // 编程式改写文本回环触发的重复提交：跳过。
  }
  if (normalized != raw) {
    SetControlText(repoBar_.gitCombo(), normalized);
  }
  RequestGitVerification(window, normalized);
}

void MainWindow::RequestGitVerification(HWND window, const std::wstring& normalizedPath) {
  app::GitToolState tool;
  tool.status = app::GitExeStatus::verifying;
  tool.path = normalizedPath;
  tool.message = L"正在后台验证 Git 程序（git --version）…";
  state_.SetGitTool(std::move(tool));
  state_.SetGitExePath(normalizedPath);
  state_.SetStatusNote(L"正在验证 Git 程序：" + normalizedPath);
  UpdateCommandAvailability();
  RefreshTexts(window);
  // 任务体只在工作线程执行；序号由控制器管理，旧结果按序号作废。
  gitWorker_.Request(window, kGitProbeCompleted, normalizedPath, [timeout = kGitProbeTimeoutMs](const std::wstring& exe) {
    return platform::VerifyGitExe(exe, timeout);
  });
}

void MainWindow::OnGitProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::GitExeVerification verification;
  if (!gitWorker_.FetchLatest(completionSerial, &verification)) {
    return;  // 较慢完成的旧结果：已被更新的选择取代，丢弃。
  }
  // 换 Git 程序同样是一次身份变化：旧 Git 读回来的摘要与列表都要作废。
  const std::wstring previousGitPath = state_.Git().path;
  app::GitToolState tool;
  tool.path = verification.path;
  tool.version = verification.version;
  tool.message = verification.message;
  if (!gitPathInvalidNotice_.empty() && verification.outcome == git::GitProbeOutcome::verified) {
    // 失效的那条旧路径要说完整句再展示：验证消息本身会被刷新成「正在/已验证 新路径」。
    tool.message += L"｜上次保存的 Git 路径已失效：" + gitPathInvalidNotice_ + L"，本条已取代它。";
    gitPathInvalidNotice_.clear();
  }
  tool.status = verification.outcome == git::GitProbeOutcome::verified ? app::GitExeStatus::verified
                                                                       : app::GitExeStatus::invalid;
  const std::wstring statusLine = tool.message;  // 带着失效说明一起进状态行，不被「正在验证」刷掉
  state_.SetGitTool(std::move(tool));
  state_.SetStatusNote(statusLine);
  if (verification.outcome == git::GitProbeOutcome::verified) {
    NoteGitPathForPreferences(verification.path);  // 只记「这次真的验证过」的路径
  } else {
    gitPathInvalidNotice_.clear();  // 这次验证也没过：那句失效提示不再有意义
  }
  UpdateCommandAvailability();
  const bool switchedGit = verification.outcome == git::GitProbeOutcome::verified &&
                           !previousGitPath.empty() && previousGitPath != verification.path &&
                           !state_.Info().repoPath.empty();
  if (switchedGit) {
    // 用另一个 Git 程序重新识别同一个仓库路径：识别成功后会接着重读工作区，
    // 在途的旧结果由身份版本判为过期。
    RequestRepoDetection(window, state_.Info().repoPath, RepoDetectMode::initial);
  } else {
    // Git 一开始不可用、随后才验证通过的场合：自动补一次仓库识别，用户无需重选路径。
    ResumeRepoDetectionWhenGitReady(window);
  }
  RefreshTexts(window);
}

void MainWindow::CommitRepoInput(HWND window) {
  const std::wstring raw = git::TrimWide(GetControlText(repoBar_.repoEdit()));
  if (raw.empty()) {
    SetRepoFailed(window, {}, git::RepoError::inputEmpty, {});
    return;
  }
  // 相对路径按启动工作目录展开；展示与内部状态都用绝对路径（仓库识别只读，不改任何东西）。
  const std::wstring absolute = platform::ToAbsolutePath(raw);
  const std::wstring normalized = absolute.empty() ? raw : absolute;
  if (normalized != raw) {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), normalized);
  }
  state_.SetRepoPath(normalized);
  if (state_.Repo().status == app::RepoLoadStatus::detecting) {
    return;  // 上一次识别还在进行：它会以最新一次提交为准，无需重复排队。
  }
  RequestRepoDetection(window, normalized, RepoDetectMode::initial);
}

void MainWindow::RequestRepoDetection(HWND window, const std::wstring& normalizedPath, RepoDetectMode mode) {
  if (!state_.GitUsable()) {
    SetRepoFailed(window, normalizedPath, git::RepoError::gitUnavailable, {});
    return;
  }
  if (mode == RepoDetectMode::initial) {
    app::RepoState repo;
    repo.status = app::RepoLoadStatus::detecting;
    state_.SetRepo(std::move(repo));
    // 换仓库（或换 Git 程序）等于取消那次「还在核对」的创建提交：
    // 确认框要核对的是这个仓库的现状，仓库都换了，点下的那一次提交自然作废，
    // 那时候写下的提交信息文件也没有 Git 会去读，一并回收。
    commitFlow_.ReleaseAttempt(true);
    // 上一个仓库读来的身份默认值同样要作废：新仓库的配置可能完全不同，
    // 留着旧值会让界面把「旧仓库的默认作者」显示成新仓库的。
    state_.SetAuthor(app::AuthorState{});
    // 换仓库的第一步就是作废旧身份并清空旧列表：识别还没回来时宁可看到“正在读取”，
    // 也不能让上一个仓库的未暂存/已暂存条目留在屏上被当成当前状态。
    refreshCycleActive_ = false;
    tasks_.UnbindRepository();
    ClearWorkspace();
    state_.SetStatusNote(L"正在后台识别仓库（只读查询，不会改动仓库）：" + normalizedPath);
  } else {
    // 刷新：仓库身份没变，摘要与列表都留在原位，等新结果回来再就地替换，
    // 免得每点一次刷新就整屏空一下、选中项也没了。
    refreshCycleActive_ = true;
    state_.SetStatusNote(tasks_.ReadStartedText());
  }
  UpdateCommandAvailability();
  RefreshTexts(window);

  platform::RepoDetectRequest request;
  request.exePath = state_.Git().path;
  request.directory = normalizedPath;
  request.timeoutMilliseconds = kRepoDetectTimeoutMs;
  repoWorker_.Request(window, kRepoDetectCompleted, std::move(request),
                      [](const platform::RepoDetectRequest& pending) {
                        return platform::RunRepositoryDetection(pending);
                      });
}

void MainWindow::SetRepoFailed(HWND window, const std::wstring& normalizedPath, git::RepoError error,
                               std::wstring_view detail) {
  const std::wstring message = git::BuildRepoErrorDetail(error, detail);
  app::RepoState repo;
  repo.status = (error == git::RepoError::inputEmpty) ? app::RepoLoadStatus::unloaded
                                                      : app::RepoLoadStatus::failed;
  repo.detection.kind = (error == git::RepoError::notRepository) ? git::RepoKind::notRepository
                                                                 : git::RepoKind::failed;
  repo.detection.error = error;
  repo.detection.root = normalizedPath;
  repo.detection.message = message;
  state_.SetRepo(std::move(repo));
  // 没有可用的工作区身份，就没有任何安全的查询落点：在途结果一律作废，列表清空。
  tasks_.UnbindRepository();
  commitFlow_.ReleaseAttempt(true);  // 同样也没有地方可提交了：那次还在核对的创建提交作废。
  refreshCycleActive_ = false;
  ClearWorkspace();
  state_.SetAuthor(app::AuthorState{});  // 没有可用工作区就没有可信的身份查询落点。
  // 草稿作用域同样随身份消失：没有可用的工作区根，屏幕内容就不知道该记到哪一名下，
  // 宁可什么都不记，也绝不猜一个键。
  draftScopeRoot_.clear();
  draftScopeKey_.clear();
  suppressDraftRestoreOnce_ = false;  // 导航没落地：抑制标记到此为止，不留挂
  // 目标目录连识别都没过（路径不存在、不是目录、safe.directory 拦截等）：导航同样没有落地，
  // 控制器要放弃它的阶段标记并把代管内容原样留着。
  submoduleFlow_.OnRepositoryArrivalFailed(*this, CaptureOperationContext());
  // 失败原因写进任务状态；若尚未选择仓库则给出占位说明而不是错误。
  state_.SetStatusNote(normalizedPath.empty() ? std::wstring(kRepoInputPlaceholder) + L"。" + message
                                              : message);
  UpdateCommandAvailability();
  RefreshTexts(window);
}

void MainWindow::OnRepoDetectCompleted(HWND window, uint64_t completionSerial) {
  git::RepoDetection detection;
  if (!repoWorker_.FetchLatest(completionSerial, &detection)) {
    return;  // 较慢完成的旧结果：已被更新的仓库选择取代，丢弃。
  }
  const bool wasRefresh = refreshCycleActive_;
  app::RepoState repo;
  const bool answeredByGit =
      detection.error == git::RepoError::none || detection.kind == git::RepoKind::notRepository;
  repo.status = answeredByGit ? app::RepoLoadStatus::loaded : app::RepoLoadStatus::failed;
  repo.detection = std::move(detection);
  state_.SetRepo(std::move(repo));

  if (!state_.RepoUsable()) {
    // 识别失败，或形态根本没有工作区（裸仓库、.git 内部、非仓库）：
    // 旧身份必须作废，否则之前发出的读取结果会被当成当前状态留在屏上。
    state_.SetStatusNote(state_.Repo().detection.message);
    tasks_.UnbindRepository();
    refreshCycleActive_ = false;
    // 这一趟刷新本来就是「创建提交」点出来的：仓库现在没有可用工作区，那次尝试既等不到
    // 工作区读取的结果，也没有地方再发查询，就地作废（不然阶段标记会一直挡着别的写操作）。
    commitFlow_.ReleaseAttempt(true);
    ClearWorkspace();
    state_.SetAuthor(app::AuthorState{});
    // 导航发起的那一次切换如果没有工作区落地，控制器要收好自己的阶段标记：
    // 代管里的草稿原样留着（那是用户写的字），一句说明写进状态栏。
    submoduleFlow_.OnRepositoryArrivalFailed(*this, CaptureOperationContext());
    UpdateCommandAvailability();
    RefreshTexts(window);
    return;
  }

  // 刷新链路：识别成功后接着读工作区；识别本身失败时不会走到这里。
  const bool identityChanged = tasks_.BindRepository(state_.Git().path, state_.Repo().detection.root);
  if (identityChanged) {
    // 工作区根与上次不同（选了另一个仓库，或同一 Git 程序解析出不同的根）：
    // 上一个仓库的条目一律作废，等新的读取结果重新填。
    state_.SetStatusNote(state_.Repo().detection.message);
    ClearWorkspace();
  } else if (!wasRefresh) {
    state_.SetStatusNote(state_.Repo().detection.message);
  }
  // 表单内容属于「哪一个工作区」在这里定案：换了仓库而表单里已有用户输入时，
  // 由用户当场决定保留还是放弃，程序不替人猜，也不会让旧作者悄悄变成新仓库的默认值。
  ResolveFormOnRepositorySwitch(window, FormRepositoryKey());
  if (identityChanged) {
    // 换到新绑定的工作区就把「最近仓库」记到账：这是识别成功（Git 亲口答出的根）之后才记的，
    // 认不出的路径不会进列表。
    NoteRecentRepository(state_.Repo().detection.root);
  }
  // 作者默认值与摘要、列表共用这一次刷新：识别成功后一起重读，不另建触发机制。
  RequestAuthorConfig(window);
  // 导航落地点：只有「这次识别正是某次导航发起的」才会被控制器认下（它按等待中的根目录核对），
  // 用户自己改路径或普通刷新都会被告知不是导航的一站而直接忽略。认下时交还代管的表单草稿，
  // 返回父仓库那一站还要接着核那三份位置。
  submoduleFlow_.OnRepositoryArrived(*this, CaptureOperationContext());
  UpdateCommandAvailability();
  StartWorkspaceRead(window);
  RefreshTexts(window);
}

void MainWindow::ClearWorkspace() {
  // 未发起读取或读取前提消失：状态回到 unloaded，模型为空，列表显示“尚未选择可用仓库”。
  state_.SetWorkspace(git::WorkspaceSnapshot{});
  ApplyWorkspaceLists();
}

void MainWindow::ScheduleRefresh(HWND window) {
  if (!state_.GitUsable() || state_.Info().repoPath.empty()) {
    return;  // 连要刷新哪个仓库都不知道（此时“刷新”按钮也已禁用）。
  }
  // 短时间内的多次触发——连点“刷新”、几个操作接连结束——共用同一个定时器，到点只跑一轮。
  ::KillTimer(window, kRefreshTimer);
  ::SetTimer(window, kRefreshTimer, kRefreshDebounceMs, nullptr);
  state_.SetStatusNote(tasks_.ReadStartedText());
  RefreshTexts(window);
}

void MainWindow::RunRefreshCycle(HWND window) {
  if (!state_.GitUsable() || state_.Info().repoPath.empty()) {
    refreshCycleActive_ = false;
    // 这一轮根本不会发起读取：还在核对仓库现状的那次创建提交必须一并取消，
    // 否则那个阶段标记会一直挂着，将来某次无关的读取完成时凭空弹出确认框。
    commitFlow_.ReleaseAttempt(true);
    return;
  }
  // 一次刷新同时覆盖顶部摘要与两个列表：在外部终端里切分支、提交、拉取之后，
  // 分支与上游也一样会变，只重读 git status 会留下半新半旧的界面。
  // 上一次识别失败时，这一趟同时也是“外部把仓库修好了”之后的恢复入口。
  RequestRepoDetection(window, state_.Info().repoPath,
                       state_.RepoUsable() ? RepoDetectMode::refresh : RepoDetectMode::initial);
}

void MainWindow::StartWorkspaceRead(HWND window) {
  if (tasks_.RequestRefresh() != app::RefreshSchedule::start) {
    return;  // merged：已有读取在途，读完会补这一次；ignored：身份不再可用。
  }
  const app::ReadTicket ticket = tasks_.BeginRead();
  if (ticket.serial == 0) {
    refreshCycleActive_ = false;
    return;
  }
  platform::WorkspaceStatusRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kWorkspaceStatusTimeoutMs;
  // HEAD 是否可解析取仓库识别时 Git 自己的回答：尚无提交的仓库不在这次读取里追问 git log。
  request.repositoryHasCommits = state_.Repo().detection.headResolved;
  // 凭据随请求交给工作线程，完成通知原样带回：界面只看它来决定这份结果还算不算数。
  request.readSerial = ticket.serial;
  request.bindingGeneration = ticket.generation;

  if (!refreshCycleActive_) {
    // 换仓库或首次识别：先把列表置成“正在读取”，旧仓库的行不留。
    git::WorkspaceSnapshot loading;
    loading.status = git::WorkspaceLoadStatus::loading;
    state_.SetWorkspace(std::move(loading));
    ApplyWorkspaceLists();
  }
  state_.SetStatusNote(tasks_.ReadStartedText());
  workspaceWorker_.Request(window, kWorkspaceStatusCompleted, std::move(request),
                           [](const platform::WorkspaceStatusRequest& pending) {
                             return platform::RunWorkspaceStatusLoad(pending);
                           });
}

void MainWindow::OnWorkspaceLoadCompleted(HWND window, uint64_t completionSerial) {
  platform::WorkspaceLoadOutcome outcome;
  if (!workspaceWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间已提交更晚的读取，这份结果不再有意义。
  }
  refreshCycleActive_ = false;
  if (tasks_.CompleteRead(outcome.readSerial) != app::ReadDisposition::accepted) {
    // 期间切换了仓库或换了 Git 程序：属于旧身份的快照绝不落地，
    // 当前列表保持原样，等属于新身份的那一次读取来填。
    return;
  }
  state_.SetWorkspace(std::move(outcome.snapshot));
  state_.SetStatusNote(tasks_.ReadFinishedText(state_.Workspace().message));
  // 成功摘要与非 0 失败原因都原样带上：失败时列表回到“读取失败 + 原因”，
  // 而不是保留一份看不准的旧内容；用户改正情况后再点刷新即可恢复。
  ApplyWorkspaceLists();
  UpdateCommandAvailability();
  RefreshTexts(window);
  // 「创建提交」点下去之后的第一步核对认领这一次读取：列表读回来了，才轮到身份预检出场；
  // 读不回来则那次尝试就地作废（控制器经 CommitOperationHost 说明原因，表单一个字不动）。
  const bool loadSucceeded = state_.Workspace().status == git::WorkspaceLoadStatus::loaded;
  if (commitFlow_.OnWorkspaceLoaded(*this, CaptureOperationContext(), loadSucceeded)) {
    if (loadSucceeded && tasks_.RefreshStillQueued()) {
      // 核对期间又有人点过刷新：这一次不能因为弹了确认框就被吞掉。
      ScheduleRefresh(window);
    }
    return;
  }
  if (tasks_.RefreshStillQueued()) {
    ScheduleRefresh(window);  // 在途期间又请求过刷新：合并成这一趟补读。
  }
}

void MainWindow::ResumeRepoDetectionWhenGitReady(HWND window) {
  // Git 刚刚可用时补上没做成的识别：包含首次启动（等待 Git 验证）与用户改正路径两种情况。
  if (!state_.GitUsable()) {
    return;
  }
  const app::RepoState& repo = state_.Repo();
  const std::wstring& path = state_.Info().repoPath;
  if (path.empty()) {
    return;
  }
  const bool waitingForGit = repo.detection.error == git::RepoError::gitUnavailable &&
                             (repo.status == app::RepoLoadStatus::unloaded ||
                              repo.status == app::RepoLoadStatus::failed);
  if (!waitingForGit) {
    return;  // 只补因为“Git 不可用”而没能识别的场合，其他失败原因不该被反复重试。
  }
  RequestRepoDetection(window, path, RepoDetectMode::initial);
}

void MainWindow::InitializeCommandWatching(HWND window) {
  commandRunner_.Startup(window);
  // 看门狗定时器只在「确实有一个命令窗口操作在跑」期间挂着（见 LaunchCommandWindowOperation）：
  // 常驻的 500 毫秒轮询会一遍遍重画整个界面，空闲时就是可看见的闪烁。
}

bool MainWindow::LaunchCommandWindowOperation(HWND window,
                                             const git::CommandWindowOperation& operation,
                                             const CommandLaunchOptions& options) {
  // 公共边界上先核对「这次要落的那条历史记录本身成立不成立」：漏了仓库工作区根或没说清是哪类
  // 操作，记录会在写盘时被整条拒掉——那等于一件做过的操作在历史里查无此事。宁可当场不启动，
  // 也不要「执行了但没有任何线索」。历史开关没开时一律放行（不落盘就没有可核对的记录）。
  const std::wstring historyRefusal = app::DescribeHistoryCaptureRefusal(
      options.history, HistoryApplicable());
  if (!historyRefusal.empty()) {
    state_.SetStatusNote(historyRefusal + L"（没有启动命令窗口，仓库没有被改动。）");
    platform::RemoveNulPathspecFile(options.pathspecFile);
    platform::RemoveCommitMessageFile(options.messageFile);
    RefreshTexts(window);
    return false;
  }
  unsigned long long serial = 0;
  if (!tasks_.BeginOperation(operation.displayName, &serial, options.policy)) {
    // 槽位被占：这次根本没跑起来，清单文件也就没人会去读，立刻回收。
    platform::RemoveNulPathspecFile(options.pathspecFile);
    platform::RemoveCommitMessageFile(options.messageFile);
    return false;  // 同一时刻只跑一个命令窗口操作（按钮此时也已禁用）。
  }
  uint64_t operationId = 0;
  platform::CommandWindowResult failure;
  if (!commandRunner_.Start(operation, &operationId, &failure)) {
    // 连 Git 都没启动起来：立刻释放槽位，别让一次没跑起来的启动把界面永久锁住。
    const app::OperationOutcome outcome =
        tasks_.FinishOperation(serial, failure.completion, failure.exitCode, failure.failureReason);
    platform::RemoveNulPathspecFile(options.pathspecFile);  // Git 从未运行，文件同样没人要读了。
    platform::RemoveCommitMessageFile(options.messageFile);
    std::wstring note = outcome.note;
    if (!failure.environmentNotice.empty()) {
      note += L"｜" + failure.environmentNotice;
    }
    if (options.history.record) {
      // 启动失败也是一种终态：命令窗口没打开、Git 一个字都没跑，落一条明确「启动失败」的记录。
      const platform::LocalInstant now = platform::CurrentLocalInstant();
      const long long epoch = now.valid ? now.utcEpochSeconds : 0;
      app::HistoryTerminalInfo terminal;
      terminal.outcome = app::HistoryOutcome::launchFailed;
      terminal.startedEpoch = epoch;
      terminal.terminalEpoch = epoch;
      terminal.completionLabel = std::wstring(git::CommandCompletionLabel(failure.completion));
      terminal.conclusion = note;
      CommitHistoryRecord(options.history, terminal);
    }
    state_.SetStatusNote(note);
    UpdateCommandAvailability();
    RefreshTexts(window);
    return false;
  }
  activeOperation_ = ActiveOperation{serial,
                                     operationId,
                                     operation.displayName,
                                     options.scopeNotice,
                                     options.viewKind,
                                     options.pathspecFile,
                                     options.messageFile,
                                     options.commitOperation,
                                     options.committedForm,
                                     options.undoOperation,
                                     options.restoreHint,
                                     options.fetchOperation,
                                     options.pullFetchOperation,
                                     options.pullIntegrateOperation,
                                     options.pushOperation,
                                     options.upstreamWriteStep,
                                     options.conflictContinueOperation,
                                     options.conflictAbortOperation,
                                     options.restoreOperation,
                                     options.history};
  // Git 已经在命令窗口里跑起来了，才挂这枚看门狗：它兜的是「完成通知永远没来」那一种事故，
  // 到 TickActiveOperations 发现没有在途操作时会自己摘掉，空闲期间一秒两次的整屏重画就此结束。
  ::SetTimer(window, kGitOperationTimer, kGitOperationTickMs, nullptr);
  // 记下启动时刻：终态落账时用「启动→终态」这对时间，而不是让 startedEpoch 空着。
  if (options.history.record) {
    const platform::LocalInstant now = platform::CurrentLocalInstant();
    activeOperation_.historyStartedEpoch =
        now.valid ? static_cast<unsigned long long>(now.utcEpochSeconds) : 0ULL;
    // 当场落一条「已启动，未见结果」并立刻写盘：用户可能允许关掉界面让命令继续跑，进程也可能
    // 就在这几分钟里被强杀。没有这一条，发生过的事会在历史里读成「什么都没做」。
    // 终态（或「通知丢失 → 结果未知」）之后都用同一个 ID 更新这一条。
    activeOperation_.historyRecordId = BeginInProgressHistory(
        window, options.history, activeOperation_.historyStartedEpoch);
  }
  state_.SetStatusNote(options.startedNote);
  UpdateCommandAvailability();
  RefreshTexts(window);
  return true;
}

void MainWindow::LaunchStatusOperation(HWND window) {
  if (!state_.GitUsable() || !state_.RepoUsable()) {
    return;
  }
  const std::wstring gitExe = state_.Git().path;
  std::wstring repository = state_.Repo().detection.root;
  if (repository.empty()) {
    repository = state_.Info().repoPath;
  }

  git::CommandWindowOperation operation;
  operation.operationId = L"status";
  operation.displayName = L"status";
  operation.gitExecutable = gitExe;
  operation.repositoryDirectory = repository;
  // 只读地查看工作区状态；参数按数组提交，不进任何 shell 字符串。
  operation.arguments = {L"status"};

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 git status（" + repository + L"），等待 Git 退出码…";
  static_cast<void>(LaunchCommandWindowOperation(window, operation, options));
}

bool MainWindow::RequireWritePrerequisites(HWND window, std::wstring_view actionLabel) {
  // 判定与文案收在 app/operation_gate：暂存/查看类只走共同前提这一层
  // （它们不与被编排的六个操作互斥——这是既有行为的原样保留）。
  app::WritePrerequisites prereq;
  prereq.gitUsable = state_.GitUsable();
  prereq.repoUsable = state_.RepoUsable();
  prereq.commandWindowBusy = tasks_.OperationInFlight();
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  prereq.workspaceStillBound = !repositoryRoot.empty() &&
                               git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot);
  const std::wstring refusal = app::DescribeWritePrerequisiteRefusal(prereq, actionLabel);
  if (!refusal.empty()) {
    state_.SetStatusNote(refusal);
    RefreshTexts(window);
    return false;
  }
  return true;
}

bool MainWindow::AdmitGitFlow(HWND window, app::GitFlow requested, std::wstring_view actionLabel) {
  // AdmitWriteFlow 自带共同前提裁决（与 RequireWritePrerequisites 同一份实现），这里不再叠一遍。
  // 六个被编排的操作各自还占着「预检/复核/核实」这类不碰命令窗口槽位的中间阶段；
  // 谁拦谁、怎么解释，全在 app/operation_gate（有纯逻辑测试钉住）。
  const app::GitFlowActivity activity{
      /*commit=*/commitFlow_.Active(),
      /*undo=*/undoFlow_.Active(),
      /*fetch=*/fetchFlow_.Active(),
      /*pull=*/pullFlow_.Active(),
      /*push=*/pushFlow_.Active(),
      /*conflict=*/conflictFlow_.Active(),
      /*restore=*/restoreFlow_.Active(),
  };
  app::WritePrerequisites prereq;
  prereq.gitUsable = state_.GitUsable();
  prereq.repoUsable = state_.RepoUsable();
  prereq.commandWindowBusy = tasks_.OperationInFlight();
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  prereq.workspaceStillBound = !repositoryRoot.empty() &&
                               git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot);
  const std::wstring refusal = app::AdmitWriteFlow(requested, activity, prereq, actionLabel);
  if (!refusal.empty()) {
    state_.SetStatusNote(refusal);
    RefreshTexts(window);
    return false;
  }
  return true;
}

OperationContext MainWindow::CaptureOperationContext() const {
  OperationContext ctx;
  ctx.notifyWindow = window_.get();
  ctx.gitExecutable = state_.Git().path;
  ctx.detection = state_.Repo().detection;
  ctx.repoUsable = state_.RepoUsable();
  // 协调器的仓库身份：控制器据此辨别「这份现状还是不是它绑定时那一个仓库」。只比路径字符串
  // 看不出切走又切回同一路径、或重新识别过仓库。
  ctx.repositoryIdentity = tasks_.Identity();
  ctx.committerState = state_.Author().config.CommitterState();
  ctx.workspaceStatus = state_.Workspace().status;
  ctx.workspaceMessage = state_.Workspace().message;
  ctx.stagedItemCount = state_.WorkspaceModel().staged.size();
  return ctx;
}

std::vector<git::ChangeItem> MainWindow::CaptureCheckedSelection(
    HWND list, const std::vector<int>& rows, const std::vector<git::ChangeItem>& items, git::ChangeSide side,
    std::wstring_view objectLabel, std::wstring_view buttonTitle, std::wstring* refusal) const {
  // 逐行核对：显示的行数、行号对应的条目、条目的路径与状态，都要和刚读回来的模型一致。
  // 任何一处对不上都拒绝执行 —— 宁可让用户重新点一次，也不能把写操作发到别的文件上。
  *refusal = {};
  if (changesPane_.ListRowCount(list) != static_cast<int>(items.size())) {
    *refusal = L"列表行数与已读取的仓库状态对不上，为避免" + std::wstring(objectLabel) +
               L"错文件，本次没有执行任何命令。点“刷新”后重试。";
    return {};
  }
  std::vector<git::ChangeItem> selection;
  selection.reserve(rows.size());
  for (const int row : rows) {
    git::ChangeSide shownSide = git::ChangeSide::unstaged;
    const git::ChangeItem* shown = changesPane_.ItemAt(list, row, &shownSide);
    if (shown == nullptr || shownSide != side || row < 0 || static_cast<size_t>(row) >= items.size()) {
      *refusal = L"选中的行已不在当前列表里（内容刚被刷新），请重新选择后再点“" + std::wstring(buttonTitle) +
                 L"”。";
      return {};
    }
    const git::ChangeItem& item = items[static_cast<size_t>(row)];
    if (item.path != shown->path || item.kind != shown->kind) {
      *refusal = L"选中的某一行已经换成另一个文件，为避免" + std::wstring(objectLabel) +
                 L"错文件，本次没有执行任何命令。请重新选择。";
      return {};
    }
    // 拷贝一份：点击之后哪怕列表被刷新掉，本次执行的范围也已固定。
    selection.push_back(item);
  }
  return selection;
}

bool MainWindow::RunStagingPlan(HWND window, const git::StagingPlan& plan, const StagingLaunchWords& words) {
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };
  const std::wstring repositoryRoot = state_.Repo().detection.root;

  if (plan.delivery == git::StagingDelivery::blocked) {
    const std::wstring message = L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason;
    ::MessageBoxW(window, message.c_str(), std::wstring(words.blockedTitle).c_str(), MB_OK | MB_ICONINFORMATION);
    refuse(L"未执行 " + plan.commandLabel + L"：" + plan.blockedReason);
    return false;
  }

  // 未合并条目、子模块条目与「内容只剩索引里那一份」的条目，语义都与“普通改动”不同，
  // 而且 Git 不会替用户解释，所以执行前把话说清楚，由用户决定继续还是取消（取消不碰仓库）。
  if (!plan.confirmationText.empty()) {
    const std::wstring message =
        L"这次" + std::wstring(words.objectLabel) + L"涉及需要特别说明的条目：\n\n" + plan.confirmationText;
    const int answer = ::MessageBoxW(window, message.c_str(), std::wstring(words.confirmTitle).c_str(),
                                     MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDOK) {
      refuse(words.cancelledHint);
      return false;
    }
  }

  std::vector<std::wstring> arguments = plan.arguments;
  std::wstring pathspecFile;
  if (plan.delivery == git::StagingDelivery::pathspecFile) {
    const platform::PathspecFileWrite written =
        platform::WriteNulPathspecFile(plan.pathspecEntries, words.pathspecFilePrefix);
    if (!written.written) {
      refuse(L"没有打开命令窗口，也没有对仓库做任何改动。写路径清单失败：" + written.failureReason);
      return false;
    }
    pathspecFile = written.path;
    if (!git::AppendPathspecFileOptions(&arguments, pathspecFile)) {
      platform::RemoveNulPathspecFile(pathspecFile);
      refuse(L"没有打开命令窗口：清单文件的路径无法安全交给命令窗口（含引号或控制字符）。");
      return false;
    }
  }

  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = repositoryRoot;
  operation.arguments = std::move(arguments);

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（" + repositoryRoot +
                        L"），本次" + std::wstring(words.rangeVerb) + L" " +
                        std::to_wstring(plan.selectedItems) + L" 项，等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pathspecFile = pathspecFile;
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    refuse(L"这次" + std::wstring(words.rangeVerb) +
           L"没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    return false;
  }
  return true;
}

void MainWindow::StageSelectedUnstaged(HWND window) {
  if (!RequireWritePrerequisites(window, L"暂存")) {
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  const std::vector<int> rows = changesPane_.SelectedUnstagedRows();
  if (rows.empty()) {
    state_.SetStatusNote(std::wstring(kNoSelectionHint));
    RefreshTexts(window);
    return;
  }
  std::wstring refusal;
  const std::vector<git::ChangeItem> selection = CaptureCheckedSelection(
      changesPane_.unstagedList(), rows, model.unstaged, git::ChangeSide::unstaged, L"暂存", L"加入暂存区 →",
      &refusal);
  if (!refusal.empty()) {
    state_.SetStatusNote(refusal);
    RefreshTexts(window);
    return;
  }

  git::StagingPlanOptions planOptions;
  // 传递方式按这个 Git 程序实际报出的版本号判定（不是猜，也不是写死本机版本）。
  planOptions.pathspecFileSupported = git::SupportsPathspecFileDelivery(state_.Git().version);
  planOptions.totalUnstagedItems = model.unstaged.size();
  const git::StagingPlan plan = git::BuildStagingAddPlan(selection, planOptions);

  StagingLaunchWords words;
  words.objectLabel = L"暂存";
  words.buttonTitle = L"加入暂存区 →";
  words.blockedTitle = L"无法加入暂存区";
  words.confirmTitle = L"加入暂存区前请确认";
  words.cancelledHint = L"已取消：没有打开命令窗口，也没有执行任何 Git 命令。";
  words.pathspecFilePrefix = L"gc-add";
  words.rangeVerb = L"暂存";
  static_cast<void>(RunStagingPlan(window, plan, words));
}

void MainWindow::UnstageSelectedStaged(HWND window) {
  if (!RequireWritePrerequisites(window, L"取消暂存")) {
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  const std::vector<int> rows = changesPane_.SelectedStagedRows();
  if (rows.empty()) {
    state_.SetStatusNote(std::wstring(kNoSelectionHintUnstage));
    RefreshTexts(window);
    return;
  }
  std::wstring refusal;
  const std::vector<git::ChangeItem> selection = CaptureCheckedSelection(
      changesPane_.stagedList(), rows, model.staged, git::ChangeSide::staged, L"取消暂存", L"← 移出暂存区",
      &refusal);
  if (!refusal.empty()) {
    state_.SetStatusNote(refusal);
    RefreshTexts(window);
    return;
  }

  git::StagingPlanOptions planOptions;
  planOptions.pathspecFileSupported = git::SupportsPathspecFileDelivery(state_.Git().version);
  planOptions.totalStagedItems = model.staged.size();
  // 有没有 HEAD 由仓库识别时 Git 自己的回答决定（headResolved）：尚无任何提交时
  // git restore --staged 会当场失败，方案层据此改用同样只写索引的 git reset -q -- <所选路径>。
  // 同理，restore 这个子命令也是 2.23 起才有的，太旧的 Git 一律走 reset 形态。
  // 这两句判断即使因为外部操作而过期，两条命令都不会改动工作区，失败后紧跟的刷新会给出真实状态。
  planOptions.repositoryHasHead = state_.Repo().detection.headResolved;
  planOptions.restoreCommandSupported = git::SupportsRestoreCommand(state_.Git().version);
  const git::StagingPlan plan = git::BuildStagingUnstagePlan(selection, planOptions);

  StagingLaunchWords words;
  words.objectLabel = L"取消暂存";
  words.buttonTitle = L"← 移出暂存区";
  words.blockedTitle = L"无法移出暂存区";
  words.confirmTitle = L"移出暂存区前请确认";
  words.cancelledHint = L"已取消：没有打开命令窗口，也没有执行任何 Git 命令。";
  words.pathspecFilePrefix = L"gc-unstage";
  words.rangeVerb = L"移出暂存区";
  static_cast<void>(RunStagingPlan(window, plan, words));
}

void MainWindow::OnCommitFormEdit(HWND window, int controlId) {
  if (suppressCommitFormNotify_) {
    return;  // 程序回填的作者默認值不是使用者的編輯。
  }
  using Field = app::CommitFormSession::Field;
  switch (controlId) {
    case kIdSummaryEdit:
      formSession_.NoteUserEdit(Field::subject);
      break;
    case kIdDescriptionEdit:
      formSession_.NoteUserEdit(Field::description);
      break;
    case kIdAuthorEdit: {
      const std::wstring text = GetControlText(commitForm_.AuthorEdit());
      if (text.empty()) {
        // 作者欄被清空＝「我要用倉庫配置裡的默認值」。不給這條退路，
        // 用過一次作者欄就再也拿不到默認值，使用者只能反覆手打同一個身份。
        formSession_.NoteAuthorCleared();
        CaptureDraftIntoPreferences();
        RunFormValidation(window, L"作者栏空着：下次读到这个仓库的有效配置时会自动填上，也可以直接写「姓名 <邮箱>」。");
        return;
      }
      formSession_.NoteUserEdit(Field::author);
      break;
    }
    default:
      return;  // 表單裡其它控件的通知與本函數無關（時間控件不構成本表單的正文）。
  }
  CaptureDraftIntoPreferences();  // 每次改动都记下这份屏幕内容（防抖后才落盘）
  RunFormValidation(window);
}

void MainWindow::RunFormValidation(HWND window, std::wstring_view prefixNote) {
  // 校驗與合成規則全在 git/commit_message 裡；界面只負責把當前表單內容交給它、再把結論顯示出來。
  // 一張沒人動過的空表單不值得佔用底部說明：那裡該顯示「哪些功能還沒接入」。
  // 只有「讀配置讀出了問題」這一類前因，才允許在使用者還沒開始寫的時候顯形。
  if (!formSession_.HasUserContent()) {
    if (prefixNote.empty()) {
      return;
    }
    state_.SetFormNote(std::wstring(prefixNote));
    RefreshTexts(window);
    return;
  }
  const git::CommitFormData data = commitForm_.Capture();
  const git::CommitFormValidity validity =
      git::ValidateCommitForm(data, state_.Author().config.CommitterState());
  std::wstring note = validity.StatusText();
  if (!prefixNote.empty()) {
    note = std::wstring(prefixNote) + L" " + note;
  }
  state_.SetFormNote(note);
  RefreshTexts(window);
}

platform::IdentityPromptSpec MainWindow::MakeCoauthorPrompt(const std::wstring& title,
                                                           const std::wstring& initial,
                                                           int editingRow) const {
  platform::IdentityPromptSpec spec;
  spec.title = title;
  spec.label = L"合作者（姓名 <邮箱>）";
  spec.initialValue = initial;
  spec.font = metrics_.Font();
  spec.layout.margin = metrics_.Margin();
  spec.layout.gap = metrics_.RowGap();
  spec.layout.width = metrics_.Scale(320);
  spec.layout.labelHeight = metrics_.LabelHeight();
  spec.layout.editHeight = metrics_.ControlHeight();
  spec.layout.noteHeight = 2 * metrics_.LabelHeight();  // 校驗說明留兩行，長原因不被裁掉。
  spec.layout.buttonWidth = metrics_.ButtonWidth(L"确定");
  spec.layout.buttonHeight = metrics_.ControlHeight();

  // 校驗在框裡跑：格式不對或與既有條目重複時不關閉、原地把原因寫給使用者。
  // 比較鍵由 git/commit_identity 決定（姓名摺疊空白、郵箱只摺疊 ASCII 大小寫），界面不另立一套規則。
  const std::vector<std::wstring> existing = commitForm_.Coauthors();
  spec.validate = [existing, editingRow](const std::wstring& text) -> std::wstring {
    const std::wstring formatIssue = git::ValidateGitIdentity(text, L"合作者");
    if (!formatIssue.empty()) {
      return formatIssue;
    }
    const std::wstring key = git::CanonicalIdentityKey(text);
    for (size_t index = 0; index < existing.size(); ++index) {
      if (static_cast<int>(index) == editingRow) {
        continue;  // 修改自己那一條時不與自己重複。
      }
      if (git::CanonicalIdentityKey(existing[index]) == key) {
        return L"第 " + std::to_wstring(index + 1) +
               L" 条已经是同一位合作者（邮箱大小写不同也算同一位），不需要重复。";
      }
    }
    return {};
  };
  return spec;
}

void MainWindow::AddCoauthor(HWND window) {
  const platform::IdentityPromptResult entered =
      platform::PromptForIdentity(window, MakeCoauthorPrompt(L"添加合作者", L"", -1));
  if (!entered.accepted) {
    state_.SetFormNote(L"没有添加合作者：输入框已取消，列表与表单内容都没改动。");
    RefreshTexts(window);
    return;
  }
  git::GitIdentity parsed;
  std::wstring error;
  if (!git::ParseGitIdentity(entered.value, &parsed, &error, L"合作者")) {
    // 輸入框已經校過一次，走到這裡只能是環境異常；照實報告，不把壞條目收進列表。
    state_.SetFormNote(L"这条合作者没有收进列表：" + error);
    RefreshTexts(window);
    return;
  }
  std::vector<std::wstring> entries = commitForm_.Coauthors();
  // 收進模型的是修剪過的寫法：頭尾空白不會進提交信息，也不影響 Co-authored-by 的行數。
  entries.push_back(parsed.Format());
  commitForm_.SetCoauthors(std::move(entries));
  formSession_.NoteUserEdit(app::CommitFormSession::Field::coauthors);
  CaptureDraftIntoPreferences();
  RunFormValidation(window);
}

void MainWindow::RemoveSelectedCoauthors(HWND window) {
  const std::vector<int> rows = commitForm_.SelectedCoauthorRows();
  if (rows.empty()) {
    state_.SetFormNote(L"没有选中任何合作者条目，因此列表没有改动。请在“合作者”里点选要删除的行（可多选）。");
    RefreshTexts(window);
    return;
  }
  std::vector<std::wstring> entries = commitForm_.Coauthors();
  // 倒序刪除：行號是模型下標，正序刪會讓後面的行號全部錯位。
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
    const int row = *it;
    if (row < 0 || static_cast<size_t>(row) >= entries.size()) {
      continue;  // 列表剛被重建過的殘留行號：跳過，不憑空刪掉別的條目。
    }
    entries.erase(entries.begin() + row);
  }
  commitForm_.SetCoauthors(std::move(entries));
  formSession_.NoteUserEdit(app::CommitFormSession::Field::coauthors);
  CaptureDraftIntoPreferences();
  RunFormValidation(window, L"已删除选中的合作者条目。");
}

void MainWindow::EditCoauthorAt(HWND window, int row) {
  const std::vector<std::wstring> current = commitForm_.Coauthors();
  if (row < 0 || static_cast<size_t>(row) >= current.size()) {
    state_.SetFormNote(L"这一行已不在合作者列表里（列表刚被重建过），请重新双击。");
    RefreshTexts(window);
    return;
  }
  const platform::IdentityPromptResult entered =
      platform::PromptForIdentity(window, MakeCoauthorPrompt(L"修改合作者", current[static_cast<size_t>(row)], row));
  if (!entered.accepted) {
    state_.SetFormNote(L"没有改动这条合作者：输入框已取消。");
    RefreshTexts(window);
    return;
  }
  git::GitIdentity parsed;
  std::wstring error;
  if (!git::ParseGitIdentity(entered.value, &parsed, &error, L"合作者")) {
    state_.SetFormNote(L"这条合作者没有被改上去：" + error);
    RefreshTexts(window);
    return;
  }
  std::vector<std::wstring> entries = current;
  entries[static_cast<size_t>(row)] = parsed.Format();
  commitForm_.SetCoauthors(std::move(entries));
  commitForm_.SelectCoauthorRows({row});  // 重建列表會丟掉選中項，改完還讓它停在原來那一行。
  formSession_.NoteUserEdit(app::CommitFormSession::Field::coauthors);
  CaptureDraftIntoPreferences();
  RunFormValidation(window);
}

std::wstring MainWindow::FormRepositoryKey() const {
  const std::wstring& root = state_.Repo().detection.root;
  if (root.empty()) {
    return {};
  }
  // 只按工作區根判「是不是同一個倉庫」：換 Git 程序不改變 user.name/user.email 的取值，
  // 不該為此多問一次要不要保留表單。
  return git::CanonicalPathKey(root);
}

void MainWindow::ResolveFormOnRepositorySwitch(HWND window, const std::wstring& key) {
  if (key.empty()) {
    return;  // 沒有可用的工作區根（識別失敗那條路已經把身份清掉），不動表單狀態。
  }
  if (!draftScopeKey_.empty() && key == draftScopeKey_) {
    return;  // 还是同一个工作区（刷新重识别也会走到这里）：不记、不问、不恢复。
  }
  // 1) 屏幕上的内容先按「旧作用域」落进草稿——这是它还算旧仓库内容的最后一步。
  const bool hasPreviousScope = !draftScopeKey_.empty();
  if (hasPreviousScope && formSession_.HasUserContent()) {
    CaptureDraftIntoPreferences();
  }
  // 2) 既有的会话决定流程。这里一定要让用户当场选：悄悄沿用会把旧仓库的默认作者带进新仓库，
  //    悄悄清空会把用户打了几百字的标题与描述丢掉——两者都不可逆，程序无权取舍。
  if (!formSession_.NeedsSwitchDecision(key)) {
    formSession_.BindRepository(key);
  } else {
    const int answer = ::MessageBoxW(window, std::wstring(kFormSwitchQuestion).c_str(),
                                     std::wstring(kFormSwitchTitle).c_str(),
                                     MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (answer == IDNO) {
      {
        const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
        commitForm_.ClearFields();
        const SYSTEMTIME now = platform::CurrentLocalTime();
        commitForm_.SetTimes(now, now);
      }
      formSession_.ChooseDiscard(key);
      state_.SetFormNote(L"已放弃原来输入的表单内容；作者改用这个仓库的有效 Git 配置。");
    } else {
      // 「是」与「取消」都走保留：只有明确选「否」才丢弃。关闭对话框不得吃掉用户的字。
      formSession_.ChooseKeep(key);
      state_.SetFormNote(answer == IDYES
                             ? L"已保留你输入的表单内容；这里的作者保留下来后不再被新仓库的默认值覆盖。"
                             : L"表单内容一律没动。想改用新仓库的默认作者，清空“作者”那一栏即可。");
    }
  }
  // 3) 作用域换到新工作区根：之后的每一次草稿记录都算新仓库的。
  draftScopeRoot_ = state_.Repo().detection.root;
  draftScopeKey_ = key;
  // 4) 这个仓库上次保存过的草稿能否恢复：屏幕上已经有用户内容就不动它（另行提醒），
  //    导航在途/导航落地的那一次一律不动——交还与恢复两套动作互相踩必出串仓库。
  if (suppressDraftRestoreOnce_) {
    suppressDraftRestoreOnce_ = false;  // 导航发起的换绑定：那份内容归旅程代管
  } else {
    TryApplyPersistedDraft(window);
  }
  RefreshTexts(window);
}

// ---- 持久化：读取、保存、恢复（界面与 app/platform 两层在这里汇合）----

std::wstring MainWindow::PreferencesStorageLine() const {
  if (!prefsPaths_.valid) {
    return prefsPaths_.failureReason.empty() ? std::wstring(L"（记录目录尚未解析）")
                                             : prefsPaths_.failureReason;
  }
  return app::DescribeStoragePath(prefsPaths_.directory, platform::kPersistentStateFileName);
}

bool MainWindow::PersistenceApplicable() const {
  // 三个条件缺一不可：文件写得动、用户对「是否保存」做过明确选择、总开关开着。
  // 「上次有效」从来不是本次的授权：这条判断只管「用不用旧记录」，用之前每样内容照样重新验证。
  return prefsWritable_ && prefs_.consentRecorded && prefs_.persistenceEnabled;
}

bool MainWindow::DraftSavingApplicable() const {
  return PersistenceApplicable() && prefs_.draftSavingEnabled;
}

void MainWindow::LoadPersistedPreferences() {
  prefsPaths_ = platform::ResolvePersistentStorePaths();
  prefsWritable_ = prefsPaths_.valid;
  if (!prefsPaths_.valid) {
    prefsWritable_ = false;
    prefsLoadNote_ = L"无法使用应用数据目录，记录功能停用：" + prefsPaths_.failureReason;
    return;
  }
  const platform::PersistentReadResult read = platform::ReadPersistentStateFile(prefsPaths_.stateFile);
  switch (read.kind) {
    case platform::PersistentReadKind::absent:
      prefsConsentPending_ = true;  // 首次使用：先说明、后动笔；读取本身到现在都没写过任何东西。
      return;
    case platform::PersistentReadKind::unreadable:
      // 读不成不等于没有内容：这次既不使用也不覆盖，免得把可能完好的记录踩掉。
      prefsWritable_ = false;
      prefsLoadNote_ = read.detail + L"本次运行不使用也不会覆盖这份记录文件。";
      return;
    case platform::PersistentReadKind::oversized:
    case platform::PersistentReadKind::invalidUtf8:
      // 形态不成立：不使用；第一次保存时由保存侧先把原件改名保留、再重新开始（损坏恢复）。
      prefsConsentPending_ = true;
      prefsLoadNote_ = read.detail + L"第一次保存时会把原件改名保留在旁边，再重新开始记录。";
      return;
    case platform::PersistentReadKind::loaded:
      break;
  }
  const app::PersistentLoadResult parsed = app::ParsePersistentState(read.text);
  switch (parsed.status) {
    case app::PersistentLoadStatus::empty:
      prefsConsentPending_ = true;
      return;
    case app::PersistentLoadStatus::corrupt:
      // 读不出的内容连同「是否保存过」的选择一起作废：宁可重新问一次，也不替用户猜策略。
      prefsConsentPending_ = true;
      prefsLoadNote_ = L"记录文件读不成立（" + parsed.reason +
                       L"）。第一次保存时会把原件改名保留，再从头开始；本次不使用其中的内容。";
      return;
    case app::PersistentLoadStatus::tooNew:
      // 更高版本写下的字段没有判读依据：整份不应用，也绝不写回一个降级版本。
      prefsWritable_ = false;
      prefsLoadNote_ = L"盘上的记录文件由更新版本的程序写出（版本 " +
                       std::to_wstring(parsed.detectedVersion) +
                       L"），本次不读取、不覆盖。用那个版本打开，或手动删除记录文件后再回来。";
      return;
    case app::PersistentLoadStatus::migrated:
      prefsLoadNote_ = parsed.reason + L"；内容照用，首次说明会再出现一次以确认你的保存选择。";
      [[fallthrough]];
    case app::PersistentLoadStatus::loaded:
      prefs_ = parsed.state;
      if (!prefs_.consentRecorded) {
        prefsConsentPending_ = true;
      }
      return;
  }
}

void MainWindow::SchedulePersistentSave() {
  if (!prefsWritable_ || window_.get() == nullptr) {
    return;
  }
  prefsSavePending_ = true;
  // 连打、连续拖分隔条都只重排同一个定时器；关窗时 RunPersistentSave 会把还挂着的一次补掉。
  ::KillTimer(window_.get(), kPrefsSaveTimer);
  ::SetTimer(window_.get(), kPrefsSaveTimer, kPrefsSaveDebounceMs, nullptr);
}

void MainWindow::RunPersistentSave(HWND window) {
  static_cast<void>(window);
  prefsSavePending_ = false;
  if (window_.get() != nullptr) {
    ::KillTimer(window_.get(), kPrefsSaveTimer);
  }
  app::PersistentWriteIntents intents = prefsIntents_;
  prefsIntents_ = app::PersistentWriteIntents{};  // 先清；写不成再原样放回，脏意不丢
  const bool anything = intents.policy || intents.replaceAll || intents.gitPath ||
                        intents.windowGeometry || intents.columns || intents.recentList ||
                        intents.drafts;
  if (!anything) {
    return;
  }
  if (!prefsWritable_ || !prefs_.consentRecorded) {
    return;  // 没同意过就一个字节都不写（取消首次说明的场合）
  }
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  const platform::PersistentSaveOutcome outcome =
      platform::SavePersistentState(prefsPaths_, prefs_, intents, epoch);
  switch (outcome.status) {
    case platform::PersistentSaveStatus::saved:
    case platform::PersistentSaveStatus::recoveredCorrupt: {
      prefs_ = outcome.merged;  // 盘上现在就是这一份：下一次合并以它为基准
      if (!outcome.detail.empty()) {
        state_.SetStatusNote(outcome.detail);
      }
      if (!outcome.report.newerDraftsKeptFromDisk.empty()) {
        // 明确报告「谁的草稿更新」，绝不静默丢屏幕内容：这正是不做最后写入者通吃的地方。
        std::wstring note = L"另一个窗口保存了更晚的草稿（";
        size_t shown = 0;
        for (const std::wstring& root : outcome.report.newerDraftsKeptFromDisk) {
          if (shown != 0) {
            note += L"、";
          }
          if (shown >= 3) {
            note += L"……共 " + std::to_wstring(outcome.report.newerDraftsKeptFromDisk.size()) +
                    L" 个仓库";
            break;
          }
          note += root;
          ++shown;
        }
        note += L"）；本次保留盘上更新的那份，本窗口没有把它盖掉——"
                L"屏幕上的内容一个字没动，确认以哪份为准后再继续。";
        state_.SetFormNote(note);
      }
      return;
    }
    case platform::PersistentSaveStatus::busy: {
      // 另一个实例正持锁：把本次的脏意原样放回，交给下一次防抖/关窗重试；文件没被碰过。
      prefsIntents_.policy = prefsIntents_.policy || intents.policy;
      prefsIntents_.replaceAll = prefsIntents_.replaceAll || intents.replaceAll;
      prefsIntents_.gitPath = prefsIntents_.gitPath || intents.gitPath;
      prefsIntents_.windowGeometry = prefsIntents_.windowGeometry || intents.windowGeometry;
      prefsIntents_.columns = prefsIntents_.columns || intents.columns;
      prefsIntents_.recentList = prefsIntents_.recentList || intents.recentList;
      prefsIntents_.drafts = prefsIntents_.drafts || intents.drafts;
      SchedulePersistentSave();
      state_.SetStatusNote(outcome.detail);
      return;
    }
    case platform::PersistentSaveStatus::refusedTooNew:
      prefsWritable_ = false;
      state_.SetStatusNote(outcome.detail);
      return;
    case platform::PersistentSaveStatus::failed:
      prefsIntents_.policy = prefsIntents_.policy || intents.policy;
      prefsIntents_.replaceAll = prefsIntents_.replaceAll || intents.replaceAll;
      prefsIntents_.gitPath = prefsIntents_.gitPath || intents.gitPath;
      prefsIntents_.windowGeometry = prefsIntents_.windowGeometry || intents.windowGeometry;
      prefsIntents_.columns = prefsIntents_.columns || intents.columns;
      prefsIntents_.recentList = prefsIntents_.recentList || intents.recentList;
      prefsIntents_.drafts = prefsIntents_.drafts || intents.drafts;
      state_.SetStatusNote(L"使用记录没有保存：" + outcome.detail +
                           L"（屏幕与内存里的内容都原样保留，稍后会再试。）");
      return;
  }
}

void MainWindow::CaptureDraftIntoPreferences() {
  if (!DraftSavingApplicable() || draftScopeKey_.empty()) {
    return;  // 没有可用的工作区身份就没有归属：宁可什么都不记，也绝不猜一个键。
  }
  if (submoduleFlow_.Active()) {
    return;  // 导航在途：屏幕那份由旅程代管，交还时会再落一次账；这里插一脚就是两套恢复互相踩。
  }
  app::PersistentDraft draft;
  draft.repositoryRoot = draftScopeRoot_;
  draft.repositoryKey = draftScopeKey_;
  draft.form = commitForm_.Capture();
  draft.authorWall = commitForm_.AuthorWallTime();
  draft.committerWall = commitForm_.CommitterWallTime();
  draft.timesSynced = commitForm_.TimeSyncChecked();
  draft.timesUserEdited = commitForm_.TimesUserEdited();
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  draft.savedAtEpoch = now.valid ? now.utcEpochSeconds : 0;
  std::wstring refusal;
  app::RecordPersistentDraftSnapshot(&prefs_, std::move(draft), &refusal);
  if (!refusal.empty()) {
    state_.SetFormNote(refusal);
  }
  prefsIntents_.drafts = true;
  SchedulePersistentSave();
}

void MainWindow::TryApplyPersistedDraft(HWND window) {
  if (!DraftSavingApplicable()) {
    return;
  }
  if (submoduleFlow_.Active()) {
    return;  // 与上面同理：导航在途时交还那份归旅程管，不在这里重复恢复
  }
  const app::PersistentDraft* draft = app::FindPersistentDraft(prefs_, draftScopeKey_);
  if (draft == nullptr || app::PersistentDraftBodyIsEmpty(*draft)) {
    return;
  }
  if (formSession_.HasUserContent()) {
    // 屏幕上已经写着这次的字：两份都在，程序无权替人挑，所以不动屏幕、只把另一份的存在说出来。
    state_.SetFormNote(L"这个仓库还存着一份未提交的草稿（保存于 " +
                       (draft->savedAtEpoch > 0
                            ? platform::FormatLocalEpochSeconds(draft->savedAtEpoch)
                            : std::wstring(L"时间不明")) +
                       L"），但表单里已有你正在输入的内容，没有自动覆盖。想恢复那一份：先清空标题、"
                       L"描述与合作者（清空会被记成新草稿），再重新进入这个仓库。");
    return;
  }
  // 落回控件的整套记号与「旅程代管交还」同一语义：恢复的是用户写的字，全部按用户内容登记，
  // 之后到达的仓库默认作者不能悄悄盖掉它（身份配置变了也不沿用错身份）。
  {
    const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
    commitForm_.SetSummaryText(draft->form.subject);
    commitForm_.SetDescriptionText(draft->form.description);
    commitForm_.SetAuthorText(draft->form.author);
    std::vector<std::wstring> coauthors = draft->form.coauthors;
    commitForm_.SetCoauthors(std::move(coauthors));
    if (draft->timesUserEdited) {
      const SYSTEMTIME authorTime = platform::SystemTimeFromCivil(draft->authorWall);
      const SYSTEMTIME committerTime = platform::SystemTimeFromCivil(draft->committerWall);
      commitForm_.SetTimes(authorTime, committerTime);
      // SetTimes 自己会撤掉「用户改过时间」的记号，而这份草稿的时间恰恰是用户挑过的：
      // 记号必须补回去，否则下一次提交会悄悄改用此刻的时间，把当时那份时间意图抹掉。
      commitForm_.NoteTimesUserEdited();
    } else {
      // 没人动过时间：回到此刻。上次保存时的那个时刻属于上一次运行，不能伪装成这次的时间意图。
      const SYSTEMTIME now = platform::CurrentLocalTime();
      commitForm_.SetTimes(now, now);
    }
    commitForm_.SetTimeSyncChecked(draft->timesSynced);
  }
  using Field = app::CommitFormSession::Field;
  if (!draft->form.subject.empty()) {
    formSession_.NoteUserEdit(Field::subject);
  }
  if (!draft->form.description.empty()) {
    formSession_.NoteUserEdit(Field::description);
  }
  if (!draft->form.coauthors.empty()) {
    formSession_.NoteUserEdit(Field::coauthors);
  }
  if (!draft->form.author.empty()) {
    formSession_.NoteUserEdit(Field::author);
  }
  RefreshTimeControlsState(window);
  RunFormValidation(window);
  state_.SetFormNote(L"已恢复这个仓库上次未提交的草稿（保存于 " +
                     (draft->savedAtEpoch > 0 ? platform::FormatLocalEpochSeconds(draft->savedAtEpoch)
                                              : std::wstring(L"时间不明")) +
                     L"）。作者那一栏按你输入的内容对待，不会被仓库默认身份盖掉；"
                     L"想改回默认身份就手动清空作者栏。");
}

void MainWindow::NoteGitPathForPreferences(const std::wstring& verifiedPath) {
  if (!PersistenceApplicable() || verifiedPath.empty()) {
    return;
  }
  if (prefs_.gitExecutablePath == verifiedPath) {
    return;  // 同一个路径不重复记账（每次刷新验证都会走到这里）。
  }
  prefs_.gitExecutablePath = verifiedPath;
  prefsIntents_.gitPath = true;
  SchedulePersistentSave();
}

void MainWindow::NoteRecentRepository(const std::wstring& root) {
  if (!PersistenceApplicable() || root.empty()) {
    return;
  }
  app::TouchPersistentRecent(&prefs_, root);
  prefsIntents_.recentList = true;
  SchedulePersistentSave();
}

void MainWindow::NoteLayoutForPreferences(bool geometry, bool columns) {
  if (!PersistenceApplicable()) {
    return;
  }
  bool changed = false;
  if (geometry) {
    RECT rc{};
    if (::GetWindowRect(window_.get(), &rc) != 0) {
      prefs_.window.valid = true;
      prefs_.window.x = rc.left;
      prefs_.window.y = rc.top;
      prefs_.window.width = rc.right - rc.left;
      prefs_.window.height = rc.bottom - rc.top;
      prefs_.window.maximized = ::IsZoomed(window_.get()) != FALSE;
      prefsIntents_.windowGeometry = true;
      changed = true;
    }
  }
  if (columns) {
    const int leftPermille =
        static_cast<int>(changesSpec_.leftRatio * app::kPersistentColumnPermilleBase + 0.5);
    const int middlePermille =
        static_cast<int>(changesSpec_.middleRatio * app::kPersistentColumnPermilleBase + 0.5);
    if (leftPermille > 0 && middlePermille > 0 &&
        leftPermille + middlePermille < app::kPersistentColumnPermilleBase) {
      prefs_.columns.valid = true;
      prefs_.columns.leftPermille = leftPermille;
      prefs_.columns.middlePermille = middlePermille;
      prefsIntents_.columns = true;
      changed = true;
    }
  }
  if (changed) {
    SchedulePersistentSave();
  }
}

void MainWindow::ShowPersistentConsentPrompt(HWND window) {
  prefsConsentPending_ = false;
  const std::wstring body = std::wstring(kConsentBodyHead) + L"保存位置：" +
                            PreferencesStorageLine() + L"\r\n" + std::wstring(kConsentBodyTail);
  const int answer = ::MessageBoxW(window, body.c_str(), std::wstring(kConsentTitle).c_str(),
                                   MB_YESNOCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON1);
  if (answer != IDYES && answer != IDNO) {
    // 取消＝这次运行一个字节都不写；选择本身也不落盘（下次启动重新问）。
    // 右下角的开关仍然是现成的入口：用户主动点它，就是明确同意保存。
    state_.SetStatusNote(L"好：这次运行什么都不保存。右下角的「保存记录」「保存草稿」随时可以改主意，"
                         L"点下去的那一刻才开始保存。");
    RefreshTexts(window);
    return;
  }
  prefs_.consentRecorded = true;
  prefs_.persistenceEnabled = true;
  prefs_.draftSavingEnabled = (answer == IDYES);
  actionBar_.SetPreferenceChecks(true, prefs_.draftSavingEnabled);
  prefsIntents_.policy = true;
  // 同意之后，本次会话已经发生的事实（验证过的 Git、绑上的仓库、当前窗口布局）
  // 一并补记；然后一次落盘——「他同意了什么」当场生效，不留悬挂状态。
  if (state_.GitUsable()) {
    NoteGitPathForPreferences(state_.Git().path);
  }
  if (state_.RepoUsable()) {
    NoteRecentRepository(state_.Repo().detection.root);
  }
  NoteLayoutForPreferences(/*geometry=*/true, /*columns=*/true);
  RunPersistentSave(window);
  std::wstring note = prefs_.draftSavingEnabled
                          ? L"开始保存使用记录与提交草稿。位置："
                          : L"开始保存使用记录（不含草稿）：最近仓库、Git 程序与窗口布局。位置：";
  note += PreferencesStorageLine();
  if (!prefsWritable_) {
    note = L"位置当前不可写，本次运行没有保存任何东西：" + PreferencesStorageLine();
  }
  if (!prefsLoadNote_.empty()) {
    note = prefsLoadNote_ + L"｜" + note;
  }
  state_.SetStatusNote(note);
  RefreshTexts(window);
}

void MainWindow::OnPersistenceCheckClicked(HWND window, int controlId, bool checked) {
  // 主动点开关＝对「是否保存」做过明确选择：首次说明视为已展示（含当初点取消的场合），
  // 排队里的说明也不再补弹——用户已经用行动回答过了。
  prefs_.consentRecorded = true;
  prefsConsentPending_ = false;
  if (!prefsWritable_) {
    state_.SetStatusNote(L"当前没有可写的记录位置（" + PreferencesStorageLine() +
                         L"），开关只记录了你的选择，本次运行不会写盘。");
  }
  if (controlId == kIdPersistRecordsCheck) {
    prefs_.persistenceEnabled = checked;
    if (checked) {
      state_.SetStatusNote(L"已开始保存记录：最近仓库、Git 程序、窗口布局" +
                           (prefs_.draftSavingEnabled ? std::wstring(L"与草稿") : std::wstring()) +
                           L"。位置：" + PreferencesStorageLine());
      // 重新打开：把当前屏幕的草稿与此刻的布局立刻补进账（不丢正在写的字）。
      CaptureDraftIntoPreferences();
      NoteLayoutForPreferences(/*geometry=*/true, /*columns=*/true);
    } else {
      state_.SetStatusNote(L"已停止使用与保存记录：这个开关的状态会被记住，盘上已有的内容保留不动；"
                           L"想删干净点「清除已存记录」。");
    }
    prefsIntents_.policy = true;
    RunPersistentSave(window);  // 开关本身要落地，即使总开关刚被关掉
  } else {
    prefs_.draftSavingEnabled = checked;
    prefsIntents_.policy = true;
    prefsIntents_.drafts = true;  // 关闭时合并表会连盘上的草稿一起清掉；打开时这份当前草稿要落账
    if (checked) {
      CaptureDraftIntoPreferences();
      state_.SetStatusNote(L"已恢复保存草稿：未提交的表单按仓库保存（同一项目的不同 worktree 各自独立）。");
    } else {
      state_.SetStatusNote(L"已停止保存草稿：记录文件里的草稿已删除（屏幕上的字没有动）。"
                           L"其余记录照常保存。");
    }
    RunPersistentSave(window);
  }
  RefreshTexts(window);
}

void MainWindow::ClearPersistedRecords(HWND window) {
  const std::wstring body =
      L"将删除记录文件里的全部用户内容：\r\n"
      L"  · 最近仓库（最多 " + std::to_wstring(app::kMaxRecentRepositories) + L" 条）\r\n"
      L"  · Git 程序路径\r\n"
      L"  · 窗口位置、大小与列宽\r\n"
      L"  · 所有仓库保存的提交草稿（草稿可能含私人内容）\r\n\r\n"
      L"位置：" + PreferencesStorageLine() + L"\r\n"
      L"对这台机器上所有 EvernightCommit 窗口同时生效；"
      L"不会改动 Git 仓库、你的 Git 配置，也不会动屏幕上正在输入的文字。\r\n\r\n确定清除吗？";
  if (::MessageBoxW(window, body.c_str(), L"清除已存记录",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    state_.SetStatusNote(L"没有清除任何记录。");
    RefreshTexts(window);
    return;
  }
  if (!prefsPaths_.valid) {
    state_.SetStatusNote(L"没有可清除的记录：" + prefsPaths_.failureReason);
    RefreshTexts(window);
    return;
  }
  if (!prefsWritable_) {
    // 盘上那份本版本判读不了（更高版本、读不成）：不保证能安全合并就不碰它，
    // 把「怎么手动删」说清楚，由用户处理。
    state_.SetStatusNote(L"盘上的记录文件本版本不能安全处理（见上一条说明），没有动它。"
                         L"要删除请手动移除这个文件：" +
                         PreferencesStorageLine());
    RefreshTexts(window);
    return;
  }
  if (!prefs_.consentRecorded) {
    std::wstring reason;
    if (platform::DeletePersistentStateFile(prefsPaths_, &reason)) {
      state_.SetStatusNote(L"还没有保存过任何记录；没有新建文件，也没有写入你的选择。");
    } else {
      state_.SetStatusNote(L"没有清除成功：" + reason);
    }
    RefreshTexts(window);
    return;
  }
  app::ClearPersistedUserData(&prefs_);
  prefsIntents_ = app::PersistentWriteIntents{};
  prefsIntents_.replaceAll = true;
  prefsIntents_.policy = true;
  RunPersistentSave(window);  // 立即写出（不防抖）：清除的结果必须当场可验证
  state_.SetStatusNote(L"已清除保存的最近仓库、Git 程序、窗口布局与全部草稿"
                       L"（屏幕上的内容没有动）。记录文件现在只剩开关与策略：" +
                       PreferencesStorageLine());
  RefreshTexts(window);
}


void MainWindow::RequestAuthorConfig(HWND window) {
  if (!state_.GitUsable() || !state_.RepoUsable()) {
    state_.SetAuthor(app::AuthorState{});
    return;
  }
  app::AuthorState loading;
  loading.status = app::AuthorLoadStatus::loading;
  state_.SetAuthor(std::move(loading));

  platform::AuthorConfigRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kAuthorConfigTimeoutMs;
  // 只讀查詢：git config --get 不寫設定檔、不碰索引，也不需要命令窗口。
  authorWorker_.Request(window, kAuthorConfigCompleted, std::move(request),
                        [](const platform::AuthorConfigRequest& pending) {
                          return platform::RunAuthorIdentityLoad(pending);
                        });
}

void MainWindow::OnAuthorConfigCompleted(HWND window, uint64_t completionSerial) {
  platform::AuthorConfigOutcome outcome;
  if (!authorWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 期间又提交了更晚的读取：这份结果作废。
  }
  const std::wstring currentRoot = state_.Repo().detection.root;
  if (currentRoot.empty() || !git::PathsEqualFolded(currentRoot, outcome.repositoryDirectory)) {
    // 迟到的结果属于另一个工作区：既不落地，也绝不动作者输入框。
    return;
  }

  app::AuthorState author;
  author.status =
      outcome.config.ReadFailed() ? app::AuthorLoadStatus::failed : app::AuthorLoadStatus::loaded;
  author.config = std::move(outcome.config);
  const git::AuthorIdentityConfig& config = author.config;
  const std::wstring identity = config.Identity();
  const std::wstring notice = git::BuildIdentityConfigNotice(config);
  const git::CommitterIdentityState committer = config.CommitterState();
  state_.SetAuthor(std::move(author));

  if (committer != git::CommitterIdentityState::available) {
    // 查不到或配置凑不出身份：作者欄一律不動（包括查詢失敗的那次），
    // 否則界面會把「讀不到」顯示成「這個倉庫沒有作者」，使用者剛打的身份憑空消失。
    RunFormValidation(window, notice);
    return;
  }
  if (!formSession_.AcceptsAuthorDefault()) {
    RunFormValidation(window, L"作者用的是你输入的内容，仓库配置里的身份（" + identity +
                                  L"）没有覆盖它。");
    return;
  }
  if (identity != formSession_.AppliedAuthorDefault()) {
    // 只有值真的變了才重寫輸入框：原地重寫會抹掉光標與選區，而這裡每 0.3 秒的刷新都會走到。
    SetAuthorField(window, identity);
    RunFormValidation(window, L"作者已按这个仓库的有效 Git 配置填好。");
    return;
  }
  RunFormValidation(window);
}

void MainWindow::SetAuthorField(HWND window, const std::wstring& text) {
  static_cast<void>(window);
  {
    const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
    commitForm_.SetAuthorText(text);
  }
  formSession_.NoteAuthorDefaultApplied(text);
}

void MainWindow::OnChangesListDoubleClicked(HWND window, HWND list, int row) {
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };

  if (!state_.GitUsable() || !state_.RepoUsable()) {
    refuse(L"Git 或仓库当前不可用，无法查看差异。请先确认路径并点“刷新”。");
    return;
  }
  if (tasks_.OperationInFlight()) {
    // 命令窗口操作是单槽的：并发发起两次会让“哪个窗口对应哪一行”变得含糊。
    refuse(L"已有一个命令窗口操作在进行，请等它结束后再双击查看。");
    return;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  if (repositoryRoot.empty() ||
      !git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot)) {
    // 界面显示的条目属于“已经绑定过的那个工作区”：根目录与协调器里的身份不一致时，
    // 哪怕只读命令也不能发出去，否则可能把上一个仓库的路径用到新仓库上。
    refuse(L"仓库工作区已改变，请先点“刷新”再查看差异。");
    return;
  }

  git::ChangeSide side = git::ChangeSide::unstaged;
  const git::ChangeItem* shown = changesPane_.ItemAt(list, row, &side);
  if (shown == nullptr) {
    refuse(L"这一行已不在当前列表里（内容刚被刷新），请重新选择后再双击。");
    return;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  const std::vector<git::ChangeItem>& items =
      side == git::ChangeSide::staged ? model.staged : model.unstaged;
  if (changesPane_.ListRowCount(list) != static_cast<int>(items.size()) ||
      static_cast<size_t>(row) >= items.size()) {
    refuse(L"列表行数与已读取的仓库状态对不上，为避免看错文件，本次没有执行任何命令。点“刷新”后重试。");
    return;
  }
  const git::ChangeItem& item = items[static_cast<size_t>(row)];
  if (item.path != shown->path || item.kind != shown->kind) {
    refuse(L"这一行已经换成另一个文件，为避免看错文件，本次没有执行任何命令。请重新双击。");
    return;
  }

  // 未跟踪文件没有“正常差异”，要先把档案本身问清楚（存在？可读？二进制？多大？）再决定显示什么。
  // 已跟踪条目（含删除项）不需要这一步：差异由 Git 自己给出，程序不会去打开已不存在的路径。
  git::WorktreeFileFacts facts;
  if (item.kind == git::ChangeKind::untracked) {
    facts = platform::ProbeWorktreeFileForPreview(
        git::JoinWorktreeFilePath(repositoryRoot, item.path));
  }
  const git::DiffViewPlan plan = git::BuildDiffViewPlan(side, item, facts);
  if (plan.kind == git::DiffViewKind::blocked) {
    const std::wstring message =
        L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason + L"\n\n条目：" +
        item.PathLabel();
    ::MessageBoxW(window, message.c_str(), L"无法查看该条目", MB_OK | MB_ICONINFORMATION);
    refuse(L"未查看 " + item.PathLabel() + L"：" + plan.blockedReason);
    return;
  }

  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = repositoryRoot;
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.policy = app::OperationExitPolicy::readOnlyView;
  options.viewKind = plan.kind;
  options.scopeNotice = plan.notice;
  options.startedNote = L"已在命令窗口执行 " + std::wstring(git::DiffViewKindLabel(plan.kind)) +
                        L"（" + repositoryRoot + L"）：" + item.PathLabel() + L"，等待 Git 退出码…";
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    refuse(L"这次查看没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
  }
}

void MainWindow::OnHistoryCommitDoubleClicked(HWND window, int row) {
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };

  if (!state_.GitUsable() || !state_.RepoUsable()) {
    refuse(L"Git 或仓库当前不可用，无法查看提交。请先确认路径并点“刷新”。");
    return;
  }
  if (tasks_.OperationInFlight()) {
    // 命令窗口操作是单槽的：并发发起两次会让“哪个窗口对应哪条提交”变得含糊。
    refuse(L"已有一个命令窗口操作在进行，请等它结束后再双击查看提交。");
    return;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  if (repositoryRoot.empty() ||
      !git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot)) {
    refuse(L"仓库工作区已改变，请先点“刷新”再查看提交。");
    return;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    refuse(L"这一轮仓库状态没有读成功，提交历史不可用。请点“刷新”后重试。");
    return;
  }

  const std::vector<git::CommitItem>& commits = state_.WorkspaceModel().recentCommits;
  const git::CommitItem* shown = changesPane_.CommitItemAt(row);
  if (changesPane_.ListRowCount(changesPane_.historyList()) != static_cast<int>(commits.size()) ||
      shown == nullptr) {
    refuse(L"列表行数与已读取的提交历史对不上，为避免看错提交，本次没有执行任何命令。点“刷新”后重试。");
    return;
  }
  const git::CommitItem& item = commits[static_cast<size_t>(row)];
  // 行号与模型的对应关系两头核对：显示的那一条和模型的那一条必须是同一个对象 ID，
  // 命令只会用这个已验证的完整 ID，绝不从「提交」列的短 ID 反解。
  if (shown->objectId != item.objectId) {
    refuse(L"这一行已经换成另一条提交，为避免看错提交，本次没有执行任何命令。请重新双击。");
    return;
  }

  const git::CommitShowPlan plan = git::BuildCommitShowPlan(item);
  if (!plan.allowed) {
    const std::wstring message =
        L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.refusalReason +
        L"\n\n提交：" + git::ShortObjectId(item.objectId) + L" " + item.summary;
    ::MessageBoxW(window, message.c_str(), L"无法查看该提交", MB_OK | MB_ICONINFORMATION);
    refuse(L"未查看提交 " + git::ShortObjectId(item.objectId) + L"：对象 ID 不符合约定。");
    return;
  }

  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = repositoryRoot;
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  // 退出码按常规判定：git show 读到对象就是 0，报错误非 0——不像 --no-index 那样有「1 也算正常」的语义。
  options.scopeNotice = plan.notice;
  options.startedNote = L"已在命令窗口执行 git show（" + repositoryRoot + L"）：查看提交 " +
                        git::ShortObjectId(item.objectId) + L"「" + item.summary + L"」，等待 Git 退出码…";
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    refuse(L"这次查看没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
  }
}

void MainWindow::OnCommandWindowCompleted(HWND window, uint64_t operationId) {
  if (activeOperation_.serial == 0 || activeOperation_.runnerId != operationId) {
    return;  // 不是本窗口当前拥有的操作（保留的旧窗口、或已按通知丢失结案的迟到通知）。
  }
  platform::CommandWindowResult result;
  if (!commandRunner_.TakeResult(operationId, &result)) {
    return;  // 理论上不会发生：结果在通知之前登记；这里不删除 ID，等下一次通知或退出确认。
  }
  const unsigned long long serial = activeOperation_.serial;
  const std::optional<git::DiffViewKind> viewKind = activeOperation_.viewKind;
  const std::wstring pathspecFile = activeOperation_.pathspecFile;
  const std::wstring messageFile = activeOperation_.messageFile;
  const bool commitOperation = activeOperation_.commitOperation;
  const git::CommitFormData committedForm = activeOperation_.committedForm;
  const bool undoOperation = activeOperation_.undoOperation;
  const bool fetchOperation = activeOperation_.fetchOperation;
  const bool pullFetchOperation = activeOperation_.pullFetchOperation;
  const bool pullIntegrateOperation = activeOperation_.pullIntegrateOperation;
  const bool pushOperation = activeOperation_.pushOperation;
  const int upstreamWriteStep = activeOperation_.upstreamWriteStep;
  const bool conflictContinueOperation = activeOperation_.conflictContinueOperation;
  const bool conflictAbortOperation = activeOperation_.conflictAbortOperation;
  const std::wstring restoreHint = activeOperation_.restoreHint;
  const app::HistoryCapture historyCapture = activeOperation_.history;
  const unsigned long long historyStartedEpoch = activeOperation_.historyStartedEpoch;
  const std::wstring historyRecordId = activeOperation_.historyRecordId;
  const bool restoreOperation = activeOperation_.restoreOperation;
  activeOperation_ = ActiveOperation{};
  // 清单临时文件的回收：只在“Git 肯定不会再来读它”的终态删除 ——
  // result.txt 是 Git 退出之后才写完的（finished），launchFailed/gitNotStarted/helperNeverStarted 里
  // Git 从未运行；而 terminated、stillUnknown 表示 Git 可能还活着，此时宁可让 %TEMP% 留一个
  // 几百字节的清单文件，也绝不能把 Git 正在读的那份删掉。
  const bool gitWillNotRead = result.completion == git::CommandCompletion::finished ||
                              result.completion == git::CommandCompletion::launchFailed ||
                              result.completion == git::CommandCompletion::gitNotStarted ||
                              result.completion == git::CommandCompletion::helperNeverStarted;
  if (gitWillNotRead) {
    platform::RemoveNulPathspecFile(pathspecFile);
    platform::RemoveCommitMessageFile(messageFile);
  }
  const app::OperationOutcome outcome =
      tasks_.FinishOperation(serial, result.completion, result.exitCode, result.failureReason);
  if (!outcome.recognised) {
    return;
  }
  // 一次写操作拿到终态就落一条历史（没勾「记录操作历史」时 CommitHistoryRecord 返回空、不写盘）。
  // pull 整合失败、conflict 命令失败这两条提前结案的路径也各自调用它，绝不漏记某一种终态。
  auto settleHistory = [&](const std::wstring& concl) {
    if (!historyCapture.record) {
      return;
    }
    app::HistoryTerminalInfo terminal;
    terminal.outcome = DescribeHistoryOutcome(result.completion, outcome.succeeded);
    terminal.startedEpoch = static_cast<long long>(historyStartedEpoch);
    const platform::LocalInstant now = platform::CurrentLocalInstant();
    terminal.terminalEpoch = now.valid ? now.utcEpochSeconds : 0;
    terminal.exitCodeKnown = result.completion == git::CommandCompletion::finished;
    terminal.exitCode = result.exitCode;
    terminal.completionLabel = std::wstring(git::CommandCompletionLabel(result.completion));
    terminal.conclusion = concl;
    const std::wstring recordId = CommitHistoryRecord(historyCapture, terminal, historyRecordId);
    if (pushOperation) {
      lastPushRecordId_ = recordId;
    }
  };
  // 「创建提交」的收尾只认 Git 的退出码：成功才清已提交的正文（先逐栏比对再清），
  // 失败时表单一个字都不动，用户可以直接改好再点一次（那种场合最不该丢的就是他刚写下来的东西）。
  if (commitOperation) {
    commitFlow_.OnCommandSettled(*this, outcome.succeeded, committedForm);
  }
  // 结论交给协调器保管：紧随其后的自动刷新会把它和仓库现状并排显示在同一行里。
  std::wstring conclusion = outcome.note;
  if (viewKind.has_value() && result.completion == git::CommandCompletion::finished) {
    // 查看类操作的退出码语义在这里补完整：`git diff` 有差异时也是 0，
    // `git diff --no-index` 则用 1 表示“有差异”。不解释就会被读成“操作失败”。
    // gitNotStarted 等没有退出码的终态不走这里：那句解释需要的是一个 Git 真实给过的数值。
    conclusion += git::DescribeDiffViewExitCode(*viewKind, result.exitCode);
  }
  // 撤回成功的结论必须带上恢复线索（原提交完整 ID + 找回方式）：用户之后在状态栏里
  // 还能查到那条提交去了哪。失败/结果未知时不追加——那句「未成功」本身说明没有移动引用，
  // 具体原因命令窗口和上一行结论里都有。
  if (undoOperation && outcome.succeeded && !restoreHint.empty()) {
    conclusion += L"｜" + restoreHint;
  }
  // fetch / pull 两步 / 推送命令的终态措辞收在 app/operation_conclusions：每一句都对应
  // 各自的长期承诺（范围、不自动重试、不改配置、成功与否以发布目标的核实为准）。
  if (fetchOperation) {
    conclusion += app::DescribeFetchConclusion(outcome.succeeded);
  }
  if (pullFetchOperation) {
    conclusion += app::DescribePullFetchConclusion(outcome.succeeded);
  }
  if (pullIntegrateOperation) {
    if (outcome.succeeded) {
      conclusion += app::DescribePullIntegrateSuccessConclusion();
    } else {
      // 整合没跑成：那句结论要以「现场读回来的实况」为准。现场读取走后台任务
      // （GUI 线程不再在终态之后同步等子进程），这条结论的补完、说明框与收尾重读
      // 一并交给 pull 控制器；这里直接返回，槽位释放后不碰 Remember/ScheduleRefresh。
      commandRunner_.ClearAllResults();
      UpdateCommandAvailability();
      settleHistory(conclusion);
      pullFlow_.BeginIntegrationFailedReport(*this, CaptureOperationContext(), std::move(conclusion),
                                             result.completion, result.exitCode,
                                             result.environmentNotice);
      RefreshTexts(window);
      return;
    }
  }
  if (conflictContinueOperation || conflictAbortOperation) {
    if (outcome.succeeded) {
      // 成功那句只按退出码说话：那一步由 Git 建立提交（或按它自己的规则中止），
      // 这个仓库现在还剩什么，交给紧随其后的重读，不在这里替 Git 打保票。
      conclusion += conflictContinueOperation ? app::DescribeConflictContinueSuccessConclusion()
                                              : app::DescribeConflictAbortSuccessConclusion();
    } else {
      // 那条流程命令没做成（含「窗口被提前关掉」「超过观察期限仍没有结果」这类结果未知）：
      // 结论要以「现场读回来的实况」为准。现场读取走后台任务，这条结论的补完、说明框与
      // 收尾重读一并交给冲突流程控制器；这里直接返回，槽位释放后不碰 Remember/ScheduleRefresh。
      // 绝不在这里顺手补一条 abort/reset——现场怎么处理由用户决定。
      commandRunner_.ClearAllResults();
      UpdateCommandAvailability();
      settleHistory(conclusion);
      conflictFlow_.BeginFailedReport(*this, CaptureOperationContext(),
                                      conflictContinueOperation ? ConflictFlow::Entry::continueFlow
                                                                : ConflictFlow::Entry::abortFlow,
                                      std::move(conclusion), result.completion, result.exitCode,
                                      result.environmentNotice);
      RefreshTexts(window);
      return;
    }
  }
  if (pushOperation) {
    conclusion += app::DescribePushCommandConclusion(outcome.succeeded);
  }
  // 上游写入的那几条 git config：结论里不追加推送那句承诺（它们不是推送），
  // 成败交回推送控制器逐条判定——「推送成功」与「配置写成没有」必须分开说。
  const bool upstreamWriteOperation = upstreamWriteStep != 0;
  // 集中环境策略移除过继承的重定向变量时，结论必须把这句话说完：
  // 用户从终端启动本程序时要知道“那些变量被移除了、操作绑定的是界面上选中的仓库”。
  if (!result.environmentNotice.empty()) {
    conclusion += L"｜" + result.environmentNotice;
  }
  tasks_.RememberOperationConclusion(conclusion);
  settleHistory(conclusion);
  // 已完成但保留的窗口不影响后续操作，只清理已取回的结果记录。
  commandRunner_.ClearAllResults();
  UpdateCommandAvailability();
  // pull 的第一步（获取）终态不是这次操作的终点：成功就接着做阶段二预检，失败就此为止。
  // 这一句必须排在槽位释放与可用性刷新之后——那一次预检是内部只读查询，不占命令窗口槽位。
  if (pullFetchOperation) {
    pullFlow_.OnFetchSettled(*this, CaptureOperationContext(), outcome.succeeded);
  } else if (pullIntegrateOperation) {
    // 整合有了终态（成功；失败那条路已在上面转交控制器）就结案：这一次 pull 到此为止。
    pullFlow_.AbandonFlow();
  }
  if (pushOperation) {
    // 这一次推送本身到此结案（计划里剩下的只有核实）。核实无论成败都要发：
    // 命令报成功时要拿它当证据，命令报失败时它正是「远端到底动没动」的唯一凭据。
    pushFlow_.BeginVerification(*this, CaptureOperationContext(), outcome.succeeded, conclusion);
  }
  if (upstreamWriteOperation) {
    // 首次推送之后的上游写入：这一步的终态交回推送控制器。成功就发下一条，
    // 失败就停下并分开说清「推送已经成的那部分」与「配置只写了一半」——绝不自动重发。
    pushFlow_.OnUpstreamStepSettled(*this, CaptureOperationContext(), upstreamWriteStep,
                                    outcome.succeeded, conclusion);
  }
  if (restoreOperation) {
    // 按记录恢复的那条引用更新到此结案：恢复控制器在此之前一直占着自己的流程状态，现在才放开。
    // 成功与失败（含「结果未知」）的措辞由它自己说，本处不替它判定成成功。
    restoreFlow_.OnCommandSettled(*this, outcome.succeeded, conclusion);
  }
  // 无论成功还是失败都要重读一次：失败的操作同样可能已经改动仓库
  // （提交到一半、push 被拒、合并留下冲突），只有退出码决定要不要报成功。
  ScheduleRefresh(window);
  RefreshTexts(window);
}

void MainWindow::TickActiveOperations(HWND window) {
  // 轮询只负责把“执行中”刷成最新可见文本，并兜住完成通知丢失的场合
  // （真正的完成判定来自观察线程的通知，不靠这里的文案匹配）。
  if (activeOperation_.serial == 0) {
    // 没有在途的命令窗口操作就不该有这一拍：定时器随启动挂上，这里就地摘掉，
    // 否则它会一直每 500 毫秒把整个界面重画一遍。
    ::KillTimer(window, kGitOperationTimer);
    return;
  }
  if (!commandRunner_.DescribeOperation(activeOperation_.runnerId, nullptr, nullptr)) {
    const unsigned long long serial = activeOperation_.serial;
    const std::wstring name = activeOperation_.displayName;
    const std::wstring inFlightHistoryId = activeOperation_.historyRecordId;
    const bool pullStepInProgress = activeOperation_.pullFetchOperation ||
                                    activeOperation_.pullIntegrateOperation;
    const bool pushStepInProgress =
        activeOperation_.pushOperation || activeOperation_.upstreamWriteStep != 0;
    const bool restoreStepInProgress = activeOperation_.restoreOperation;
    // 这里不删清单文件：通知丢失意味着 Git 可能还在命令窗口里跑，删掉正在被读的文件
    // 会让一次合法的 git add 变成 Git 的报错。%TEMP% 里留下几百字节的清单远小于那个代价。
    activeOperation_ = ActiveOperation{};
    // pull 的那两步本来就靠这条终态通知往下走（获取成功才问关系）。通知既然丢了，
    // 就把这一次 pull 结案：界面不再停在「等待某一步」上，否则下一次点 pull 会被自己锁住。
    // 仓库究竟被改到什么程度只有重读知道，因此这里同样只重读、不替用户猜。
    if (pullStepInProgress) {
      pullFlow_.AbandonFlow();
    }
    // 推送的那几步（预检/复核/命令窗口）同样靠终态通知往下走。通知丢了就把这一次推送结案，
    // 否则界面会永远停在「等那一次推送」上，把下一次点击锁死。核实本来就只在终态之后才发起，
    // 走到这里说明它没机会跑：结论只能以命令窗口那头的输出为准，如实这么说。
    if (pushStepInProgress) {
      pushFlow_.AbandonFlow();
    }
    // 恢复同理：那条引用更新没拿到终态就是「结果未知」，控制器放开流程状态，
    // 既不说成功也不说失败，更不自动重发。
    if (restoreStepInProgress) {
      restoreFlow_.Forget();
    }
    // 执行器已经不记得这个操作：按“结果未知”结案并释放槽位，
    // 否则一个再也等不到通知的操作会把后续写操作永久锁住。
    const app::OperationOutcome outcome =
        tasks_.ForgetOperation(serial, L"本程序没有收到「" + name + L"」的完成通知");
    if (outcome.recognised) {
      // 同一条在途记录补成「结果未知」：这句不是成功、不是失败，也不是「已撤销」。
      MarkHistoryRecordUnknown(inFlightHistoryId, outcome.note);
      tasks_.RememberOperationConclusion(outcome.note);
      state_.SetStatusNote(outcome.note);
      UpdateCommandAvailability();
      ScheduleRefresh(window);  // 结果未知时更要重读：仓库究竟被改到什么程度只有 Git 自己知道。
    }
  }
  RefreshTexts(window);
}

void MainWindow::StopOperationWatching() {
  if (window_.get() != nullptr) {
    ::KillTimer(window_.get(), kGitOperationTimer);
    ::KillTimer(window_.get(), kRefreshTimer);
  }
  activeOperation_ = ActiveOperation{};
  commandRunner_.Shutdown();
}

bool MainWindow::ConfirmCloseWithActiveOperations(HWND window) {
  const std::vector<std::wstring> active = commandRunner_.ActiveOperationNames();
  if (active.empty()) {
    return true;
  }
  std::wstring message =
      L"以下 Git 操作仍在命令窗口中执行：\n";
  for (const std::wstring& name : active) {
    message += L"  • " + name + L"\n";
  }
  message += L"\n关闭本程序不会中断它们，也不会替你读取结果。\n是否仍要关闭？";
  const int answer = ::MessageBoxW(window, message.c_str(), L"Git 操作仍在进行",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  return answer == IDYES;
}

void MainWindow::OnSplitterDragged(HWND window, int splitterId, int parentX) {
  const int index = (splitterId == kSplitterLeftId) ? 0 : 1;
  RatioFromMouseX(changesArea_, changesSpec_, index, parentX, &changesSpec_.leftRatio, &changesSpec_.middleRatio);
  DoLayout(window);
  NoteLayoutForPreferences(/*geometry=*/false, /*columns=*/true);
}

void MainWindow::RefreshTimeControlsState(HWND window) {
  static_cast<void>(window);
  const bool sync = commitForm_.TimeSyncChecked();
  // 联动规则要看得见：勾着时提交者那两块控件既跟着作者时间，也置灰，
  // 否则用户会以为自己填的那一半真的生效了。
  commitForm_.SetCommitterTimeEnabled(!sync);
  if (sync) {
    commitForm_.MirrorCommitterTime();
  }
  // 「本机时区」那句说明报的是所选作者时间实际生效的偏移，而不是此刻的偏移：
  // 挑一个夏令时里的日子时，两者差一个小时，显示错了就等于骗过用户一次。
  std::wstring text = L"本机时区：" + platform::LocalTimeZoneName();
  const platform::LocalInstant instant =
      platform::ResolveLocalWallTime(commitForm_.AuthorWallTime());
  if (instant.valid) {
    const std::wstring offset = git::FormatOffsetText(instant.offsetMinutes);
    text += offset.empty() ? L"｜所选时间的偏移无法表示" : L"｜作者时间 " + offset;
  } else {
    text += L"｜所选时间没能换算成一个时刻";
  }
  commitForm_.SetTimeZoneText(text);
}

void MainWindow::ResetCommitTimesToNow(HWND window) {
  const SYSTEMTIME now = platform::CurrentLocalTime();
  commitForm_.SetTimes(now, now);  // 同时清掉「用户改过时间」的记号：这是那条明确的退路。
  RefreshTimeControlsState(window);
  const platform::LocalInstant instant = platform::CurrentLocalInstant();
  std::wstring note = L"作者时间与提交者时间都已回到此刻";
  if (instant.valid) {
    note += L"：" + git::FormatCommitTimeText(instant.wall, instant.offsetMinutes);
  }
  note += L"。“恢复当前时间”就是改错之后的退路；没人改过时间时，提交用的就是提交那一刻。";
  state_.SetFormNote(note);
  CaptureDraftIntoPreferences();  // 时间回到此刻也是草稿内容的变化
  RefreshTexts(window);
}

// ---- 六个被编排操作的入口与控制器桥 ----
// 入口只做三件事：准入裁决（app/operation_gate）→ 采集只读快照 → 把流程交给控制器；
// 下面的 OperationHost/CommitOperationHost 实现则是控制器对界面的唯一接触面：
// 每个方法只呈现给定的内容或对整界面做一次重画，不返回任何可变引用。

void MainWindow::CreateCommit(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::commit, L"创建提交")) {
    return;
  }
  commitFlow_.Start(*this, CaptureOperationContext(), state_.WorkspaceModel());
  RefreshTexts(window);
}

void MainWindow::UndoLastCommit(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::undo, L"撤回最近提交")) {
    return;
  }
  undoFlow_.Start(*this, CaptureOperationContext());
  RefreshTexts(window);
}

void MainWindow::RequestFetch(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::fetch, L"fetch")) {
    return;
  }
  fetchFlow_.Start(*this, CaptureOperationContext());
  RefreshTexts(window);
}

void MainWindow::RequestPull(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::pull, L"pull")) {
    return;
  }
  pullFlow_.Start(*this, CaptureOperationContext());
  RefreshTexts(window);
}

void MainWindow::RequestPush(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::push, L"推送")) {
    return;
  }
  pushFlow_.Start(*this, CaptureOperationContext());
  RefreshTexts(window);
}

bool MainWindow::CapturedSubmoduleSelection(git::ChangeItem* item, std::wstring* refusal) const {
  *refusal = {};
  if (item == nullptr) {
    return false;
  }
  const git::WorkspaceModel& model = state_.WorkspaceModel();
  HWND list = nullptr;
  std::vector<int> rows;
  git::ChangeSide side = git::ChangeSide::unstaged;
  const std::vector<git::ChangeItem>* items = nullptr;
  const std::vector<int> unstagedRows = changesPane_.SelectedUnstagedRows();
  const std::vector<int> stagedRows = changesPane_.SelectedStagedRows();
  if (!unstagedRows.empty()) {
    list = changesPane_.unstagedList();
    rows = unstagedRows;
    side = git::ChangeSide::unstaged;
    items = &model.unstaged;
  } else if (!stagedRows.empty()) {
    list = changesPane_.stagedList();
    rows = stagedRows;
    side = git::ChangeSide::staged;
    items = &model.staged;
  } else {
    *refusal = L"先在「未暂存的更改」或「已暂存的更改」里点选一条子模块记录，再按这个按钮。"
               L"父仓库对子模块只有一条 gitlink 记录，别的条目不是可进入的仓库。";
    return false;
  }
  if (rows.size() != 1) {
    *refusal = L"一次只能进入一个子模块（现在选了 " + std::to_wstring(rows.size()) +
               L" 条）。导航是把界面绑定的仓库整个换掉，不是一批换好几个。";
    return false;
  }
  // 行号与条目的对应关系只在「最后一次显示落地」的内容上成立，因此仍走暂存那套逐行核对：
  // 显示的行数、行对应的条目、路径与类别都要和模型一致，否则宁可拒绝也不进错目录。
  std::wstring selectionRefusal;
  const std::vector<git::ChangeItem> picked =
      CaptureCheckedSelection(list, rows, *items, side, L"子模块", L"进入子模块", &selectionRefusal);
  if (picked.size() != 1) {
    *refusal = selectionRefusal;
    return false;
  }
  if (picked[0].kind != git::ChangeKind::submodule) {
    *refusal = L"选中的这一条不是子模块条目（它的状态列不是「子模块」）。"
               L"没有换绑任何仓库，也没有执行任何命令。";
    return false;
  }
  *item = picked[0];
  return true;
}

void MainWindow::ShowConflictState(HWND window) {
  // 三个入口共用一台控制器，因此准入用的是同一个流程位（app::GitFlow::conflict）：
  // 「查看」本身只读，但它与「继续/中止」等的是同一份现场读取，两条并跑只会互相作废结果。
  if (!AdmitGitFlow(window, app::GitFlow::conflict, L"查看冲突与暂停流程")) {
    return;
  }
  conflictFlow_.Start(*this, CaptureOperationContext(), ConflictFlow::Entry::view);
  RefreshTexts(window);
}

void MainWindow::ContinueConflictFlow(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::conflict, L"继续该流程")) {
    return;
  }
  conflictFlow_.Start(*this, CaptureOperationContext(), ConflictFlow::Entry::continueFlow);
  RefreshTexts(window);
}

void MainWindow::AbortConflictFlow(HWND window) {
  if (!AdmitGitFlow(window, app::GitFlow::conflict, L"中止该流程")) {
    return;
  }
  conflictFlow_.Start(*this, CaptureOperationContext(), ConflictFlow::Entry::abortFlow);
  RefreshTexts(window);
}

void MainWindow::EnterSubmodule(HWND window) {
  // 导航不写任何东西（不改父索引、不提交、不联网），但它会把界面绑定的仓库整个换掉：
  // 因此任何在途的被编排操作都得先结束，措辞与被编排操作的互斥同源（app/operation_gate）。
  app::GitFlowActivity activity{
      /*commit=*/commitFlow_.Active(),
      /*undo=*/undoFlow_.Active(),
      /*fetch=*/fetchFlow_.Active(),
      /*pull=*/pullFlow_.Active(),
      /*push=*/pushFlow_.Active(),
      /*conflict=*/conflictFlow_.Active(),
      /*restore=*/restoreFlow_.Active(),
  };
  app::WritePrerequisites prereq;
  prereq.gitUsable = state_.GitUsable();
  prereq.repoUsable = state_.RepoUsable();
  prereq.commandWindowBusy = tasks_.OperationInFlight();
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  prereq.workspaceStillBound = !repositoryRoot.empty() &&
                               git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot);
  const std::wstring refusal =
      app::DescribeNavigationRefusal(prereq, activity, L"进入子模块");
  if (!refusal.empty()) {
    state_.SetStatusNote(refusal);
    RefreshTexts(window);
    return;
  }
  git::ChangeItem item;
  std::wstring selectionRefusal;
  if (!CapturedSubmoduleSelection(&item, &selectionRefusal)) {
    state_.SetStatusNote(selectionRefusal);
    RefreshTexts(window);
    return;
  }
  submoduleFlow_.Enter(*this, CaptureOperationContext(), item);
  RefreshTexts(window);
}

void MainWindow::ReturnToParent(HWND window) {
  submoduleFlow_.ReturnToParent(*this, CaptureOperationContext());
  RefreshTexts(window);
}

void MainWindow::SetStatus(std::wstring note) {
  state_.SetStatusNote(std::move(note));
  RefreshTexts(window_.get());
}

void MainWindow::SetFormNote(std::wstring note) {
  state_.SetFormNote(std::move(note));
  RefreshTexts(window_.get());
}

void MainWindow::RefreshUi() {
  UpdateCommandAvailability();
  RefreshTexts(window_.get());
}

bool MainWindow::Confirm(const std::wstring& title, const std::wstring& body, bool warningIcon) {
  return ::MessageBoxW(window_.get(), body.c_str(), title.c_str(),
                       MB_OKCANCEL | (warningIcon ? MB_ICONWARNING : MB_ICONINFORMATION) |
                           MB_DEFBUTTON2) == IDOK;
}

void MainWindow::ShowInfo(const std::wstring& title, const std::wstring& body) {
  ::MessageBoxW(window_.get(), body.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

void MainWindow::ShowWarning(const std::wstring& title, const std::wstring& body) {
  ::MessageBoxW(window_.get(), body.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

bool MainWindow::RiskConfirm(const std::wstring& title, const std::wstring& mainInstruction,
                             const std::wstring& yesButton, const std::wstring& body) {
  // 「强制/仍要」类风险确认框：按钮文字必须是两个明确的说法，
  // MessageBox 的按钮不可自定义，所以用 TaskDialog（Common Controls v6，app.manifest 已声明）。
  // 万一 TaskDialog 初始化失败（异常环境），退回是/否形态并在正文里点明哪个按钮是什么——
  // 无论哪种形态，「强制」都只是确认越过本程序的风险提示，命令本身一字不变。
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = window_.get();
  config.hInstance = ::GetModuleHandleW(nullptr);
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.pszWindowTitle = title.c_str();
  config.pszMainInstruction = mainInstruction.c_str();
  config.pszContent = body.c_str();
  config.pszMainIcon = TD_WARNING_ICON;
  const TASKDIALOG_BUTTON buttons[] = {
      {IDYES, yesButton.c_str()},
      {IDNO, L"取消"},
  };
  config.pButtons = buttons;
  config.cButtons = ARRAYSIZE(buttons);
  config.nDefaultButton = IDNO;
  int button = IDNO;
  const HRESULT hr = ::TaskDialogIndirect(&config, &button, nullptr, nullptr);
  if (FAILED(hr)) {
    const int answer =
        ::MessageBoxW(window_.get(),
                      (body + L"\n\n（风险确认框未能按样式打开：这里点“是”等同“" + yesButton +
                       L"”，点“否”取消。）")
                          .c_str(),
                      title.c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return answer == IDYES;
  }
  return button == IDYES;
}

std::optional<size_t> MainWindow::PromptRemoteChoice(platform::RemoteChoiceSpec spec,
                                                     const RemoteChoiceLayoutHints& hints) {
  // 内容（标题、说明、条目、按钮文案）由控制器给出；几何与字体在这里按主窗口的 DPI 度量补齐，
  // 与界面其它地方一致，也让控制器不接触任何度量状态。
  spec.font = metrics_.Font();
  spec.layout.margin = metrics_.Margin();
  spec.layout.gap = metrics_.RowGap();
  spec.layout.width = metrics_.Scale(hints.contentWidth);
  spec.layout.labelHeight = hints.labelRows * metrics_.LabelHeight();
  spec.layout.listHeight = metrics_.Scale(hints.listHeight);
  spec.layout.buttonWidth = metrics_.ButtonWidth(spec.okText);
  spec.layout.buttonHeight = metrics_.ControlHeight();
  const platform::RemoteChoiceResult choice = platform::PromptForRemoteChoice(window_.get(), spec);
  if (!choice.accepted || choice.selectedIndex < 0) {
    return std::nullopt;
  }
  return static_cast<size_t>(choice.selectedIndex);
}

std::optional<std::wstring> MainWindow::PromptForText(platform::IdentityPromptSpec spec,
                                                     const TextInputLayoutHints& hints) {
  // 与「合作者」那一套输入框共用同一份对话框实现与同一套 DPI 度量：控制器只给内容与行数，
  // 几何一律在这里按本窗口的度量填，高 DPI 下和界面其它部分一致。
  spec.font = metrics_.Font();
  spec.layout.margin = metrics_.Margin();
  spec.layout.gap = metrics_.RowGap();
  spec.layout.width = metrics_.Scale(hints.contentWidth);
  spec.layout.labelHeight = hints.labelRows * metrics_.LabelHeight();
  spec.layout.editHeight = metrics_.ControlHeight();
  spec.layout.noteHeight = hints.noteRows * metrics_.LabelHeight();
  spec.layout.buttonWidth = metrics_.ButtonWidth(spec.okText);
  spec.layout.buttonHeight = metrics_.ControlHeight();
  const platform::IdentityPromptResult result = platform::PromptForIdentity(window_.get(), spec);
  if (!result.accepted) {
    return std::nullopt;
  }
  return result.value;
}

bool MainWindow::LaunchCommandWindow(const git::CommandWindowOperation& operation,
                                     const CommandLaunchOptions& options) {
  return LaunchCommandWindowOperation(window_.get(), operation, options);
}

void MainWindow::ScheduleRefresh() { ScheduleRefresh(window_.get()); }

void MainWindow::RememberOperationConclusion(std::wstring_view conclusion) {
  tasks_.RememberOperationConclusion(conclusion);
}

bool MainWindow::NavigateRepository(std::wstring_view directory, std::wstring_view statusNote) {
  // 换绑定的仓库走的就是「用户自己填路径」那条既有链路：绝对化 → 后台识别 → 绑定/作废 →
  // 重读。这里只是把目录代填进去，不另开第二套换绑逻辑，也不在这上面加任何写操作。
  if (directory.empty()) {
    state_.SetStatusNote(L"导航没有发起：这次要去的那个子模块没有可用的目录路径。"
                         L"父索引、表单与代管内容都没有动。");
    UpdateCommandAvailability();
    RefreshTexts(window_.get());
    return false;
  }
  if (!state_.GitUsable()) {
    state_.SetStatusNote(L"导航没有发起：Git 程序当前不可用，没有可识别的落点。请先确认路径并点“刷新”。");
    UpdateCommandAvailability();
    RefreshTexts(window_.get());
    return false;
  }
  const std::wstring target = std::wstring(directory);
  {
    const SuppressRepoEditNotify guard(suppressRepoEditNotify_);
    SetControlText(repoBar_.repoEdit(), target);
  }
  state_.SetRepoPath(target);
  suppressDraftRestoreOnce_ = true;  // 这次换绑定由旅程发起：落地时不叠加草稿恢复
  RequestRepoDetection(window_.get(), target, RepoDetectMode::initial);
  // 导航那句说明排在识别提示之后：识别那条是既有链路的固定文字，这句才是「为什么仓库突然变了」。
  state_.SetStatusNote(std::wstring(statusNote));
  RefreshTexts(window_.get());
  return true;
}

bool MainWindow::CommitFormHasUserContent() const {
  return formSession_.HasUserContent();
}

void MainWindow::ClearFormForNavigation(std::wstring_view note) {
  // 内容已由导航控制器收进代管（按当前工作区根存键），这里把界面倒空只是「让位」，不是丢弃。
  {
    const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
    commitForm_.ClearFields();
    const SYSTEMTIME now = platform::CurrentLocalTime();
    commitForm_.SetTimes(now, now);
  }
  formSession_.Reset();
  state_.SetFormNote(std::wstring(note));
  RefreshTimeControlsState(window_.get());
}

void MainWindow::ApplyHeldFormForNavigation(const app::HeldForm& held, std::wstring_view note) {
  // 交还的是用户当时写的字：全部以「用户内容」的身份落回控件，并把这些记号一并标上。
  // 这样随后到达的「这个仓库的默认作者」不会把它悄悄盖掉——身份串用就是这么来的。
  {
    const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
    commitForm_.SetSummaryText(held.form.subject);
    commitForm_.SetDescriptionText(held.form.description);
    commitForm_.SetAuthorText(held.form.author);
    std::vector<std::wstring> coauthors = held.form.coauthors;
    commitForm_.SetCoauthors(std::move(coauthors));
    const SYSTEMTIME authorTime = platform::SystemTimeFromCivil(held.authorWall);
    const SYSTEMTIME committerTime = platform::SystemTimeFromCivil(held.committerWall);
    commitForm_.SetTimes(authorTime, committerTime);
    commitForm_.SetTimeSyncChecked(held.timesSynced);
    if (held.timesUserEdited) {
      // SetTimes 自己会撤掉「用户改过时间」的记号，而这份草稿的时间恰恰是用户挑过的：
      // 记号必须补回去，否则下一次提交会悄悄改用此刻的时间，把当时那份时间意图抹掉。
      commitForm_.NoteTimesUserEdited();
    }
  }
  using Field = app::CommitFormSession::Field;
  if (!held.form.subject.empty()) {
    formSession_.NoteUserEdit(Field::subject);
  }
  if (!held.form.description.empty()) {
    formSession_.NoteUserEdit(Field::description);
  }
  if (!held.form.coauthors.empty()) {
    formSession_.NoteUserEdit(Field::coauthors);
  }
  if (!held.form.author.empty()) {
    formSession_.NoteUserEdit(Field::author);
  }
  state_.SetFormNote(std::wstring(note));
  RefreshTimeControlsState(window_.get());
  RunFormValidation(window_.get());
  CaptureDraftIntoPreferences();  // 交还回来的那份同样按当前仓库落账
}

CommitFormSnapshot MainWindow::CaptureCommitForm() const {
  CommitFormSnapshot snapshot;
  snapshot.data = commitForm_.Capture();
  snapshot.authorWall = commitForm_.AuthorWallTime();
  snapshot.committerWall = commitForm_.CommitterWallTime();
  snapshot.timesSynced = commitForm_.TimeSyncChecked();
  return snapshot;
}

bool MainWindow::CommitTimesUserEdited() const { return commitForm_.TimesUserEdited(); }

void MainWindow::ApplyDefaultTimesToNow() {
  const SYSTEMTIME now = platform::CurrentLocalTime();
  commitForm_.SetTimes(now, now);
  RefreshTimeControlsState(window_.get());
}

void MainWindow::RunFormValidation(std::wstring_view prefixNote) {
  RunFormValidation(window_.get(), prefixNote);
}

void MainWindow::ApplyCommittedFormCleanup(const app::CommittedFormCleanup& cleanup) {
  {
    // 清空是程序做的事：不该被记成「用户把标题改成了空」，否则紧接着的默认值逻辑会乱套。
    const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
    commitForm_.ClearCommittedFields(cleanup.clearSubject, cleanup.clearDescription,
                                     cleanup.clearCoauthors);
  }
  formSession_.NoteCommitted(cleanup.clearSubject, cleanup.clearDescription, cleanup.clearCoauthors);
  // 时间同样按「这期间有没有被人动过」决定：没人动过就回到此刻，作为下一次提交的默认；
  // 用户在那期间另选了时间的话那是新的意图，程序不能拿「提交成功」当理由把它抹掉。
  if (cleanup.resetTimes) {
    const SYSTEMTIME now = platform::CurrentLocalTime();
    commitForm_.SetTimes(now, now);
    RefreshTimeControlsState(window_.get());
  }
  // 草稿与屏幕同步收尾：提交用掉的那部分从盘上也得消失（空正文会留删除墓碑），
  // 用户在命令窗口跑的那段时间里新写的部分原样进新草稿——绝不因为「提交成功」
  // 就顺手抹掉他后来写的字；失败与结果未知根本走不到这里，草稿一个字不动。
  CaptureDraftIntoPreferences();
}

void MainWindow::BeginStopAllBackgroundWorkers() {
  gitWorker_.BeginStop();
  repoWorker_.BeginStop();
  workspaceWorker_.BeginStop();
  authorWorker_.BeginStop();
  commitFlow_.BeginStop();
  undoFlow_.BeginStop();
  fetchFlow_.BeginStop();
  pullFlow_.BeginStop();
  pushFlow_.BeginStop();
  conflictFlow_.BeginStop();
  submoduleFlow_.BeginStop();
  restoreFlow_.BeginStop();
}

void MainWindow::JoinAllBackgroundWorkers() {
  gitWorker_.Join();
  repoWorker_.Join();
  workspaceWorker_.Join();
  authorWorker_.Join();
  commitFlow_.JoinWorkers();
  undoFlow_.JoinWorkers();
  fetchFlow_.JoinWorkers();
  pullFlow_.JoinWorkers();
  pushFlow_.JoinWorkers();
  conflictFlow_.JoinWorkers();
  submoduleFlow_.JoinWorkers();
  restoreFlow_.JoinWorkers();
}

LRESULT CALLBACK MainWindow::Thunk(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  MainWindow* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<MainWindow*>(create->lpCreateParams);
    ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (self == nullptr) {
    return ::DefWindowProcW(window, message, wParam, lParam);
  }
  try {
    return self->HandleMessage(window, message, wParam, lParam);
  } catch (...) {
    // 异常不能越过系统回调；退回到默认处理并保持消息循环继续运行。
    return ::DefWindowProcW(window, message, wParam, lParam);
  }
}

LRESULT MainWindow::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      OnCreate(window);
      return 0;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) {
        DoLayout(window);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
      const SIZE minimum = MinimumWindowSize(window);
      info->ptMinTrackSize.x = minimum.cx;
      info->ptMinTrackSize.y = minimum.cy;
      return 0;
    }
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      ::SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      metrics_.UpdateForDpi(LOWORD(wParam));
      ApplyFonts(window);
      DoLayout(window);
      return 0;
    }
    case WM_COMMAND:
      OnCommand(window, wParam);
      return 0;
    case WM_EXITSIZEMOVE:
      // 拖动/缩放结束才记一次几何：WM_SIZE 每像素都来，不该每来一次就重排保存。
      NoteLayoutForPreferences(/*geometry=*/true, /*columns=*/false);
      return 0;
    case kPersistentConsentNotice:
      // 队列里等到现在可能已经被「用户直接点了开关」顶掉了：只在还挂着时才问。
      if (prefsConsentPending_) {
        ShowPersistentConsentPrompt(window);
      }
      return 0;
    case WM_NOTIFY: {
      // ListView 的双击以 WM_NOTIFY / NM_DBLCLK 上报父窗口。只认两块更改列表、提交历史列表
      // 与合作者列表，其余通知（包括提交表单里的其它控件）一律交回默认处理，不替别人吞掉消息。
      const auto* header = reinterpret_cast<const NMHDR*>(lParam);
      if (header != nullptr && header->code == NM_DBLCLK &&
          (header->idFrom == kIdUnstagedList || header->idFrom == kIdStagedList)) {
        const auto* activated = reinterpret_cast<const NMLISTVIEW*>(lParam);
        // iItem 是命中测试得到的行号：点在空白处为 -1。多选时也只查看被双击的这一行，
        // 不把选中的其它文件一起塞进同一个窗口（一次一条才看得清，命令也只限定一条路径）。
        OnChangesListDoubleClicked(window, header->hwndFrom,
                                  activated != nullptr ? activated->iItem : -1);
        return 0;
      }
      if (header != nullptr && header->code == NM_DBLCLK && header->idFrom == kIdHistoryList) {
        // 双击某条提交：命令窗口里 git show 完整详情。同样只认命中行，点在空白处不动作。
        const auto* activated = reinterpret_cast<const NMLISTVIEW*>(lParam);
        OnHistoryCommitDoubleClicked(window, activated != nullptr ? activated->iItem : -1);
        return 0;
      }
      if (header != nullptr && header->code == NM_DBLCLK && header->idFrom == kIdCoauthorList) {
        // 双击合作者那一行＝修改它（行号 == 模型下标，和更改列表同一套约定）。
        const auto* activated = reinterpret_cast<const NMLISTVIEW*>(lParam);
        EditCoauthorAt(window, activated != nullptr ? activated->iItem : -1);
        return 0;
      }
      if (header != nullptr && header->code == DTN_DATETIMECHANGE &&
          (header->idFrom == kIdAuthorDate || header->idFrom == kIdAuthorClock ||
           header->idFrom == kIdCommitterDate || header->idFrom == kIdCommitterClock)) {
        // 日期时间控件每改一次都会发这条通知，程序自己写入时同样会发（见 CommitForm 的说明），
        // 所以那一次必须忽略，否则“没人动过时间”永远不成立，提交时刻就再也回不到“此刻”。
        if (!commitForm_.AcceptsTimeNotify()) {
          return 0;
        }
        commitForm_.NoteTimesUserEdited();
        if (commitForm_.TimeSyncChecked() &&
            (header->idFrom == kIdAuthorDate || header->idFrom == kIdAuthorClock)) {
          // 同步勾选时只有作者那半边能改：把提交者那一边的显示跟着挪过去，
          // 让用户看到的两块控件与真正交给 Git 的两个值一致。
          commitForm_.MirrorCommitterTime();
        }
        RefreshTimeControlsState(window);
        CaptureDraftIntoPreferences();  // 挑过的时间属于草稿的一部分（恢复时要分清“谁改的”）
        RefreshTexts(window);
        return 0;
      }
      return ::DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_TIMER:
      if (wParam == kGitVerifyTimer) {
        ::KillTimer(window, kGitVerifyTimer);
        CommitGitInput(window);
      } else if (wParam == kRepoDetectTimer) {
        ::KillTimer(window, kRepoDetectTimer);
        CommitRepoInput(window);
      } else if (wParam == kGitOperationTimer) {
        TickActiveOperations(window);
      } else if (wParam == kRefreshTimer) {
        ::KillTimer(window, kRefreshTimer);
        RunRefreshCycle(window);
      } else if (wParam == kPrefsSaveTimer) {
        ::KillTimer(window, kPrefsSaveTimer);
        RunPersistentSave(window);
      } else if (wParam == kHistorySaveTimer) {
        ::KillTimer(window, kHistorySaveTimer);
        RunHistorySave(window);
      }
      return 0;
    case kGitProbeCompleted:
      OnGitProbeCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kRepoDetectCompleted:
      OnRepoDetectCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kWorkspaceStatusCompleted:
      OnWorkspaceLoadCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kAuthorConfigCompleted:
      OnAuthorConfigCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kUndoProbeCompleted:
      undoFlow_.OnProbeCompleted(*this, CaptureOperationContext(),
                                 static_cast<uint64_t>(wParam));
      return 0;
    case kUndoRecheckCompleted:
      undoFlow_.OnRecheckCompleted(*this, CaptureOperationContext(),
                                   static_cast<uint64_t>(wParam));
      return 0;
    case kFetchProbeCompleted:
      fetchFlow_.OnProbeCompleted(*this, CaptureOperationContext(),
                                  static_cast<uint64_t>(wParam));
      return 0;
    case kPullProbeCompleted:
      pullFlow_.OnProbeCompleted(*this, CaptureOperationContext(),
                                 static_cast<uint64_t>(wParam));
      return 0;
    case kPullAftermathCompleted:
      pullFlow_.OnAftermathCompleted(*this, static_cast<uint64_t>(wParam));
      return 0;
    case kPushProbeCompleted:
      pushFlow_.OnProbeCompleted(*this, CaptureOperationContext(),
                                 static_cast<uint64_t>(wParam));
      return 0;
    case kPushVerifyCompleted:
      pushFlow_.OnVerifyCompleted(*this, CaptureOperationContext(),
                                  static_cast<uint64_t>(wParam));
      return 0;
    case kFirstPushTargetsCompleted:
      pushFlow_.OnFirstPushTargetsCompleted(*this, CaptureOperationContext(),
                                            static_cast<uint64_t>(wParam));
      return 0;
    case kFirstPushProbeCompleted:
      pushFlow_.OnFirstPushProbeCompleted(*this, CaptureOperationContext(),
                                          static_cast<uint64_t>(wParam));
      return 0;
    case kSubmoduleEntryProbeCompleted:
      submoduleFlow_.OnEntryProbeCompleted(*this, CaptureOperationContext(),
                                           static_cast<uint64_t>(wParam));
      return 0;
    case kSubmodulePointerProbeCompleted:
      submoduleFlow_.OnPointerProbeCompleted(*this, CaptureOperationContext(),
                                             static_cast<uint64_t>(wParam));
      return 0;
    case kConflictProbeCompleted:
      conflictFlow_.OnProbeCompleted(*this, CaptureOperationContext(),
                                     static_cast<uint64_t>(wParam));
      return 0;
    case kConflictRecheckCompleted:
      conflictFlow_.OnRecheckCompleted(*this, CaptureOperationContext(),
                                       static_cast<uint64_t>(wParam));
      return 0;
    case kConflictAftermathCompleted:
      conflictFlow_.OnAftermathCompleted(*this, static_cast<uint64_t>(wParam));
      return 0;
    case kRestoreProbeCompleted:
      restoreFlow_.OnProbeCompleted(*this, CaptureOperationContext(), static_cast<uint64_t>(wParam));
      return 0;
    case kRestoreRecheckCompleted:
      restoreFlow_.OnRecheckCompleted(*this, CaptureOperationContext(), static_cast<uint64_t>(wParam));
      return 0;
    case kCommitProbeCompleted:
      commitFlow_.OnProbeCompleted(*this, CaptureOperationContext(), state_.WorkspaceModel(),
                                   static_cast<uint64_t>(wParam));
      return 0;
    case platform::CommandWindowRunner::kCompletionMessage:
      OnCommandWindowCompleted(
          window, static_cast<uint64_t>(static_cast<uint32_t>(wParam)) |
                      (static_cast<uint64_t>(static_cast<int64_t>(lParam)) << 32));
      return 0;
    case kSplitterDragged:
      OnSplitterDragged(window, static_cast<int>(wParam), static_cast<int>(lParam));
      return 0;
    case WM_CLOSE:
      if (ConfirmCloseWithActiveOperations(window)) {
        // 关窗前把防抖里没来得及写的记录补上：小文件、本线程、有界等待（锁占用最多约 0.6 秒），
        // 不启动任何子进程。写不成也只说明「这次没保存」，绝不阻塞退出。
        RunPersistentSave(window);
        // 操作历史同样是独立文件、独立锁：关窗前把防抖里没来得及落盘的最后一次记录也补上，
        // 小文件、本线程、有界等待，不启动任何子进程；写不成只说明「这次没保存」，绝不阻塞退出。
        RunHistorySave(window);
        ::DestroyWindow(window);
      }
      return 0;
    case WM_DESTROY:
      // 两阶段收尾：先对所有后台 worker 喊停——在途查询跑完当前这一条后，剩余查询被停止
      // 信号短路（多目标核实尤其如此）——再统一等待线程退出。总等待按「最长的一条在途
      // 查询」计，而不是把十个探测各自的超时逐个相加；全部 Join 完成后才交还窗口所有权，
      // 不会释放任何还被线程引用的对象。旧线程不会再向已销毁窗口发有效通知。
      BeginStopAllBackgroundWorkers();
      JoinAllBackgroundWorkers();
      StopOperationWatching();
      ::KillTimer(window, kGitVerifyTimer);
      ::KillTimer(window, kRepoDetectTimer);
      ::KillTimer(window, kGitOperationTimer);
      ::KillTimer(window, kPrefsSaveTimer);
      window_.Disown();
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, message, wParam, lParam);
  }
}

// ============================================================================
// 操作历史与恢复入口（可选新增功能）
// ============================================================================

void MainWindow::LoadOperationHistory() {
  historyPaths_ = platform::ResolveHistoryStorePaths();
  historyWritable_ = historyPaths_.valid;
  if (!historyPaths_.valid) {
    return;  // 取不到用户应用数据目录：历史停用，绝不写到别处。
  }
  const platform::HistoryReadResult read = platform::ReadHistoryFile(historyPaths_.historyFile);
  switch (read.kind) {
    case platform::HistoryReadKind::absent:
      return;  // 首次使用：没有历史可读，开关默认关。
    case platform::HistoryReadKind::unreadable:
      historyWritable_ = false;  // 读不成≠没有：这次不使用也不覆盖。
      historyLoadNote_ = read.detail + L"本次运行不使用也不会覆盖这份历史文件。";
      return;
    case platform::HistoryReadKind::oversized:
    case platform::HistoryReadKind::invalidUtf8:
      historyLoadNote_ = read.detail + L"下一次保存时会把原件改名保留在旁边，再重新开始记录。";
      return;
    case platform::HistoryReadKind::loaded:
      break;
  }
  const app::HistoryLoadResult parsed = app::ParseOperationLog(read.text);
  switch (parsed.status) {
    case app::HistoryLoadStatus::empty:
      return;
    case app::HistoryLoadStatus::corrupt:
      historyLoadNote_ = L"历史文件读不成立（" + parsed.reason +
                         L"）。下一次保存时会把原件改名保留，再从头开始。";
      return;
    case app::HistoryLoadStatus::tooNew:
      historyWritable_ = false;
      historyLoadNote_ = L"盘上的历史文件由更新版本的程序写出（版本 " +
                         std::to_wstring(parsed.detectedVersion) + L"），本次不读取、不覆盖。";
      return;
    case app::HistoryLoadStatus::loaded:
      historyLog_ = parsed.log;
      return;
  }
}

bool MainWindow::HistoryApplicable() const {
  return historyWritable_ && historyLog_.historyEnabled;
}

std::wstring MainWindow::HistoryStorageLine() const {
  if (!historyPaths_.valid) {
    return historyPaths_.failureReason.empty() ? std::wstring(L"（历史目录尚未解析）")
                                               : historyPaths_.failureReason;
  }
  return app::DescribeHistoryStoragePath(historyPaths_.directory, platform::kHistoryFileName);
}

std::wstring MainWindow::NextHistoryId() {
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  ++historyRecordCounter_;
  return L"op-" + std::to_wstring(epoch < 0 ? 0 : epoch) + L"-" +
         std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(historyRecordCounter_);
}

void MainWindow::ScheduleHistorySave() {
  if (!HistoryApplicable()) {
    return;
  }
  historyIntents_.append = true;
  historySavePending_ = true;
  if (window_.get() != nullptr) {
    ::SetTimer(window_.get(), kHistorySaveTimer, kHistorySaveDebounceMs, nullptr);
  }
}

std::wstring MainWindow::CommitHistoryRecord(const app::HistoryCapture& capture,
                                             const app::HistoryTerminalInfo& terminal,
                                             const std::wstring& existingId) {
  if (!capture.record || !HistoryApplicable()) {
    return {};  // 没勾「记录操作历史」就一个字节都不写，也不在内存里堆记录。
  }
  // existingId 非空 = 更新启动当场落下的那条在途记录（同 ID 由 AppendHistoryRecord 合并，
  // 只有更确定的终态会替换已记录的终态，绝不会把已定结果降级）。
  const std::wstring id = existingId.empty() ? NextHistoryId() : existingId;
  app::OperationRecord record = app::ComposeHistoryRecord(id, capture.workTreeRoot, capture, terminal);
  std::wstring refusal;
  if (!app::AppendHistoryRecord(&historyLog_, std::move(record), &refusal)) {
    state_.SetStatusNote(L"操作历史没有记录：" + refusal);
    return {};
  }
  ScheduleHistorySave();
  return id;
}

std::wstring MainWindow::BeginInProgressHistory(HWND window,
                                               const app::HistoryCapture& capture,
                                               unsigned long long startedEpoch) {
  app::HistoryTerminalInfo terminal;
  terminal.outcome = app::HistoryOutcome::inProgress;
  terminal.startedEpoch = static_cast<long long>(startedEpoch);
  terminal.terminalEpoch = 0;  // 还没有终态时刻：这一条只说明「发出去了」
  terminal.completionLabel = L"执行中";
  terminal.conclusion = L"命令窗口已经启动，本程序还没有拿到它的结果。";
  const std::wstring id = CommitHistoryRecord(capture, terminal);
  if (id.empty()) {
    return {};
  }
  // 当场写盘，不等防抖：这条记录的意义正是「进程此刻没了也还查得到」。
  historySavePending_ = false;
  if (window != nullptr) {
    ::KillTimer(window, kHistorySaveTimer);
  }
  RunHistorySave(window);
  return id;
}

void MainWindow::MarkHistoryRecordUnknown(const std::wstring& recordId,
                                          std::wstring_view why) {
  if (recordId.empty() || !HistoryApplicable()) {
    return;
  }
  // 完成通知没送到：那次操作既没被报成功也没被报失败。把在途那条判成「结果未知」，
  // 绝不猜成成功、失败或已撤销；命令窗口里 Git 可能还在跑，所以也不自动重发。
  for (const app::OperationRecord& existing : historyLog_.records) {
    if (existing.id == recordId) {
      if (app::TerminalHasSettled(existing.terminal)) {
        return;  // 已经有更确定的结论（迟到的终态先到）：不降级、不改写。
      }
      break;
    }
  }
  app::OperationRecord unknown;
  unknown.id = recordId;
  unknown.terminal = app::HistoryTerminal::unknown;
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  unknown.terminalEpoch = now.valid ? now.utcEpochSeconds : 0;
  unknown.completionLabel = L"没有收到完成通知";
  unknown.outcomeNote = std::wstring(why);
  std::wstring refusal;
  if (!app::AppendHistoryRecord(&historyLog_, std::move(unknown), &refusal)) {
    state_.AppendStatusNote(L"另外：这条在途操作的历史没能补成「结果未知」（" +
                            refusal + L"）；Git 那一步的结果不受影响。");
    return;
  }
  ScheduleHistorySave();
}

void MainWindow::RecordPushVerification(std::wstring_view verificationSummary) {
  if (!HistoryApplicable() || lastPushRecordId_.empty()) {
    return;
  }
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  // 核实结果作为「追加证据」登记进最近那条推送记录：命令窗口退出码是各目标合计，
  // 「哪一个真收到那一份」只有这份 ls-remote 能回答。追加，不改命令那一步的终态。
  app::AppendHistoryReview(&historyLog_, lastPushRecordId_, epoch, verificationSummary);
  ScheduleHistorySave();
}

void MainWindow::RunHistorySave(HWND window) {
  static_cast<void>(window);
  historySavePending_ = false;
  if (window_.get() != nullptr) {
    ::KillTimer(window_.get(), kHistorySaveTimer);
  }
  app::HistoryWriteIntents intents = historyIntents_;
  historyIntents_ = app::HistoryWriteIntents{};  // 先清；写不成再原样放回，脏意不丢
  const bool anything = intents.policy || intents.append || intents.replaceAll;
  if (!anything || !historyWritable_) {
    return;  // 目录不可写或没被更高版本占着时才写；关着也要能落「关」这个选择（policy intent）。
  }
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  const platform::HistorySaveOutcome outcome =
      platform::SaveOperationHistory(historyPaths_, historyLog_, intents, epoch);
  const auto requeue = [&]() {
    historyIntents_.append = historyIntents_.append || intents.append;
    historyIntents_.policy = historyIntents_.policy || intents.policy;
    historyIntents_.replaceAll = historyIntents_.replaceAll || intents.replaceAll;
  };
  switch (outcome.status) {
    case platform::HistorySaveStatus::saved:
    case platform::HistorySaveStatus::recoveredCorrupt:
      historyLog_ = outcome.merged;  // 盘上现在就是这一份：下一次合并以它为基准
      if (!outcome.report.keptFromDiskStrongerTerminal.empty()) {
        state_.SetFormNote(L"另一个窗口已记下某些操作更确定的结果（" +
                           std::to_wstring(outcome.report.keptFromDiskStrongerTerminal.size()) +
                           L" 条）；本次以先记录到终态的那份为准，没有把较弱的信息盖上去。");
      }
      if (!outcome.detail.empty()) {
        // 这是「历史文件那边」的补充信息（损坏原件已改名保留等），与刚才那条操作结论并列显示，
        // 不能把后者盖掉：用户要同时看到「Git 做得怎么样」和「记录写得怎么样」。
        state_.AppendStatusNote(outcome.detail);
      }
      return;
    case platform::HistorySaveStatus::busy:
      requeue();
      ScheduleHistorySave();
      return;
    case platform::HistorySaveStatus::refusedTooNew:
      historyWritable_ = false;
      state_.AppendStatusNote(outcome.detail);
      return;
    case platform::HistorySaveStatus::failed:
      // 写不进去不等于 Git 那一步没做成：两句分开说，脏意留回队列稍后再试一次。
      requeue();
      ScheduleHistorySave();
      state_.AppendStatusNote(L"操作历史这次没写成功（" + outcome.detail +
                              L"）；Git 那一步的结果不受影响。");
      return;
  }
}

void MainWindow::OnHistoryCheckClicked(HWND window, bool checked) {
  historyLog_.historyEnabled = checked;
  if (!historyWritable_) {
    state_.SetStatusNote(L"当前没有可写的历史位置（" + HistoryStorageLine() +
                         L"），开关只记录了你的选择，本次运行不会写盘。");
    RefreshTexts(window);
    return;
  }
  if (checked) {
    state_.SetStatusNote(L"已开始记录操作历史：本程序对仓库做过的写操作会留在 " + HistoryStorageLine() +
                         L"。");
  } else {
    state_.SetStatusNote(L"已停止记录操作历史：不再新增记录，盘上已有的保留不动；要清空用「操作历史…」里的清除。");
  }
  historyIntents_.policy = true;
  RunHistorySave(window);  // 开关本身要落地
  RefreshTexts(window);
}

void MainWindow::ClearOperationHistory(HWND window) {
  const std::wstring body =
      L"将删除历史记录里的全部操作条目（唯一 ID、时间、仓库、引用与对象 ID、终态、逐目标核实与恢复线索）。\r\n"
      L"这不会改动 Git 仓库、不会改你的 Git 配置，也不会撤销任何已经做过的操作。\r\n\r\n"
      L"位置：" +
      HistoryStorageLine() + L"\r\n对这台机器上所有 EvernightCommit 窗口同时生效。\r\n\r\n确定清除全部操作历史吗？";
  if (::MessageBoxW(window, body.c_str(), L"清除操作历史", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
      IDYES) {
    state_.SetStatusNote(L"没有清除任何历史。");
    RefreshTexts(window);
    return;
  }
  if (!historyPaths_.valid) {
    state_.SetStatusNote(L"没有可清除的历史：" + historyPaths_.failureReason);
    RefreshTexts(window);
    return;
  }
  if (!historyWritable_) {
    state_.SetStatusNote(
        L"盘上的历史文件本版本不能安全处理（见上一条说明），没有动它。要删除请手动移除：" +
        HistoryStorageLine());
    RefreshTexts(window);
    return;
  }
  historyLog_.records.clear();
  historyIntents_ = app::HistoryWriteIntents{};
  historyIntents_.replaceAll = true;
  historyIntents_.policy = true;
  RunHistorySave(window);
  state_.SetStatusNote(L"已清除全部操作历史（开关与保留设置保留）。位置：" + HistoryStorageLine());
  RefreshTexts(window);
}

void MainWindow::ExportOperationHistory(HWND window) {
  if (!historyPaths_.valid) {
    state_.SetStatusNote(L"没有可导出的历史位置：" + historyPaths_.failureReason);
    RefreshTexts(window);
    return;
  }
  const platform::LocalInstant now = platform::CurrentLocalInstant();
  const long long epoch = now.valid ? now.utcEpochSeconds : 0;
  std::wstring outPath;
  std::wstring reason;
  if (platform::ExportOperationHistoryFile(historyPaths_, historyLog_, epoch, &outPath, &reason)) {
    state_.SetStatusNote(L"已导出脱敏的历史副本：" + outPath);
  } else {
    state_.SetStatusNote(L"导出失败：" + reason);
  }
  RefreshTexts(window);
}

// 一条记录的一行摘要（列表主列）与详情（次列）。只取元数据，绝不展开被排除的内容。
namespace {
std::wstring HistoryRowPrimary(const app::OperationRecord& record) {
  const std::wstring time = record.startedEpoch > 0
                                ? platform::FormatLocalEpochSeconds(record.startedEpoch)
                                : std::wstring(L"时间未知");
  return time + L"　" + std::wstring(app::HistoryFlowLabel(record.flow)) + L"　" +
         std::wstring(app::HistoryTerminalLabel(record.terminal));
}
std::wstring HistoryRowDetail(const app::OperationRecord& record) {
  std::wstring detail;
  if (!record.workTreeRoot.empty()) {
    detail += record.workTreeRoot;
  }
  if (!record.restoreBranchRef.empty()) {
    detail += L"　引用 " + record.restoreBranchRef;
  }
  if (!record.sourceObjectId.empty()) {
    detail += L"　" + git::ShortObjectId(record.sourceObjectId);
  }
  if (!record.restoreNote.empty()) {
    detail += L"　" + record.restoreNote;
  }
  return detail;
}
}  // namespace

void MainWindow::ShowOperationHistory(HWND window) {
  RemoteChoiceLayoutHints hints;
  hints.contentWidth = 620;
  hints.listHeight = 260;

  platform::RemoteChoiceSpec menu;
  menu.title = L"操作历史";
  menu.label = L"选择要对这份操作历史做的操作（取消不改动任何东西）。位置：" + HistoryStorageLine();
  menu.items = {
      {L"查看某条记录 / 恢复",
       std::wstring(L"共 ") + std::to_wstring(historyLog_.records.size()) + L" 条记录"},
      {L"导出脱敏副本", L"把当前历史写成一个导出文件（与存储同样脱敏）"},
      {L"清除全部历史", L"删除全部操作条目（不撤销任何已做过的操作）"},
      {L"查看存储位置", HistoryStorageLine()},
      {L"调整保留期限与条数",
       std::wstring(L"当前：保留 ") + std::to_wstring(historyLog_.retentionDays) +
           L" 天（0＝不按天）、上限 " + std::to_wstring(historyLog_.maxRecords) + L" 条"},
  };
  menu.okText = L"继续";
  menu.emptyItemDetail = L"";
  menu.needSelectionHint = L"先选一项，再按确定。（取消不执行任何操作）";
  hints.labelRows = 3;
  const std::optional<size_t> choice = PromptRemoteChoice(menu, hints);
  if (!choice.has_value()) {
    return;
  }
  switch (*choice) {
    case 1:
      ExportOperationHistory(window);
      return;
    case 2:
      ClearOperationHistory(window);
      return;
    case 3:
      ShowInfo(L"操作历史存储位置", HistoryStorageLine());
      return;
    case 4: {
      platform::IdentityPromptSpec spec;
      spec.title = L"设置操作历史的保留期限与条数";
      spec.label =
          L"输入「保留天数,最大条数」，逗号分隔。保留天数 0＝不按天淘汰（上限 3650）；条数 1.." +
          std::to_wstring(app::kMaxHistoryRecords) + L"。";
      spec.initialValue = std::to_wstring(historyLog_.retentionDays) + L"," +
                          std::to_wstring(historyLog_.maxRecords);
      spec.okText = L"保存设置";
      spec.validate = [](const std::wstring& text) -> std::wstring {
        // 与记录文件同一套带上下界解析：越界只拒绝，不会先溢出回绕。
        const auto parse = [](std::wstring_view piece, long long upperInclusive, long long* out) {
          return app::ParseNonNegativeDecimal(piece, upperInclusive, out);
        };
        const size_t comma = text.find(L',');
        if (comma == std::wstring::npos) {
          return L"格式应为「天数,条数」，两个都用非负整数。";
        }
        long long days = 0;
        long long records = 0;
        if (!parse(std::wstring_view(text).substr(0, comma), app::kMaxHistoryRetentionDays,
                   &days) ||
            !parse(std::wstring_view(text).substr(comma + 1),
                   static_cast<long long>(app::kMaxHistoryRecords), &records) ||
            records < 1) {
          return L"超出可接受范围（天数≤3650；条数 1.." +
                 std::to_wstring(app::kMaxHistoryRecords) + L"）。";
        }
        return {};
      };
      TextInputLayoutHints textHints;
      textHints.labelRows = 3;
      textHints.noteRows = 2;
      textHints.contentWidth = 460;
      const std::optional<std::wstring> value = PromptForText(spec, textHints);
      if (!value.has_value()) {
        return;
      }
      const size_t comma = value->find(L',');
      long long days = 0;
      long long records = 0;
      // 校验通过的值再按同一套解析读一遍：两处用同一个函数，不会再出现「一处限长、
      // 一处不限长」的差异；读不回来就当没改过，不落盘。
      if (!app::ParseNonNegativeDecimal(std::wstring_view(*value).substr(0, comma),
                                        app::kMaxHistoryRetentionDays, &days) ||
          !app::ParseNonNegativeDecimal(
              std::wstring_view(*value).substr(comma + 1),
              static_cast<long long>(app::kMaxHistoryRecords), &records)) {
        return;
      }
      historyLog_.retentionDays = static_cast<int>(days);
      historyLog_.maxRecords = static_cast<size_t>(records);
      app::ApplyHistoryRetention(&historyLog_, platform::CurrentLocalInstant().utcEpochSeconds);
      historyIntents_.policy = true;
      historyIntents_.append = true;
      RunHistorySave(window);
      state_.SetStatusNote(L"已更新操作历史的保留期限（" + std::to_wstring(historyLog_.retentionDays) +
                           L" 天）与条数上限（" + std::to_wstring(historyLog_.maxRecords) + L" 条）。");
      RefreshTexts(window);
      return;
    }
    case 0:
    default:
      break;
  }

  // 「查看某条记录 / 恢复」：列出现有记录。
  if (historyLog_.records.empty()) {
    ShowInfo(L"操作历史", L"还没有任何记录。勾上「记录操作历史」之后的写操作才会被记录下来。");
    return;
  }
  platform::RemoteChoiceSpec list;
  list.title = L"操作历史记录";
  list.label = L"选中一条记录后，可按其恢复线索重新预检并确认恢复（仅引用级可回退者）；取消不执行任何操作。";
  // 最新排在最前，便于查看；下标映射回真实记录。
  for (auto it = historyLog_.records.rbegin(); it != historyLog_.records.rend(); ++it) {
    list.items.push_back({HistoryRowPrimary(*it), HistoryRowDetail(*it)});
  }
  list.okText = L"处理选中记录";
  list.emptyItemDetail = L"";
  list.needSelectionHint = L"先在列表里点选一条记录，再按确定。（取消不执行任何操作）";
  hints.listHeight = 320;
  const std::optional<size_t> picked = PromptRemoteChoice(list, hints);
  if (!picked.has_value()) {
    return;
  }
  const size_t fromEnd = *picked;
  if (fromEnd >= historyLog_.records.size()) {
    return;
  }
  const app::OperationRecord& record =
      historyLog_.records[historyLog_.records.size() - 1 - fromEnd];

  if (record.restoreKind == app::HistoryRestoreKind::refMove) {
    const int answer =
        ::MessageBoxW(window,
                      (L"这条记录支持「引用级恢复」：把分支 " + record.restoreBranchRef + L" 从 " +
                       record.restoreExpectedCurrentId + L" 挪回 " + record.restoreUndoToObjectId +
                       L"。\n\n恢复之前会先在后台重新预检现状、并要你确认；不改动索引与工作区，"
                       L"也不自动撤销任何东西。\n\n是＝开始恢复流程；否＝只复制恢复命令；取消＝关闭。")
                          .c_str(),
                      L"恢复这条记录？", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer == IDYES) {
      BeginRestoreFromRecord(window, record);
    } else if (answer == IDNO) {
      CopyRestoreCommandForRecord(window, record);
    }
    return;
  }
  if (record.restoreKind == app::HistoryRestoreKind::manualRemote) {
    const int answer = ::MessageBoxW(
        window,
        (L"这条记录涉及已推送到远端的历史：\n\n" + record.restoreNote +
         L"\n\n本程序不会自动 force push / hard reset / clean，也不提供这种命令。"
         L"可以复制的是这条记录的说明与目标信息（一段说明文字，不是可直接执行的命令），"
         L"由你自己看清目标与风险后再决定怎么做。\n\n是＝复制说明；否＝关闭。")
            .c_str(),
        L"涉及远端历史：只解释与复制", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer == IDYES) {
      CopyRestoreCommandForRecord(window, record);
    }
    return;
  }
  ShowInfo(L"这条记录没有引用级恢复",
           record.restoreNote.empty()
               ? std::wstring(L"这次操作不产生「把某个本地引用挪回去」的可审查恢复形态（例如抓取只更新"
                              L"远端跟踪引用、拉取整合/冲突流程会重写多提交与工作区）。本程序不自动撤销，"
                              L"也不提供 force push / hard reset / clean。")
               : record.restoreNote);
}

namespace {
// 记录 → 恢复线索：只有一处拷贝代码。「开始恢复」与「复制恢复命令」都从它出发，
// 于是这两条路看到的必须是同一个目标（分支、两个完整对象 ID、是否删除形态）。
git::RestoreClues CluesFromRecord(const app::OperationRecord& record) {
  git::RestoreClues clues;
  clues.valid = true;
  clues.repositoryRoot = record.workTreeRoot;
  clues.branchRef = record.restoreBranchRef;
  clues.expectedCurrentOid = record.restoreExpectedCurrentId;
  clues.moveToOid = record.restoreUndoToObjectId;
  clues.isRootDeletion = record.restoreIsRoot;
  clues.originalNote = record.restoreNote;
  return clues;
}
}  // namespace

void MainWindow::BeginRestoreFromRecord(HWND window, const app::OperationRecord& record) {
  if (record.restoreKind != app::HistoryRestoreKind::refMove) {
    return;
  }
  // 准入走与其它六个入口同一套规则（app/operation_gate）：恢复等全部六个在途流程，
  // 自己已在走时也给出「再点不会排队」的明确说法——不在按钮里零散加条件。
  if (!AdmitGitFlow(window, app::GitFlow::restore, L"按记录恢复")) {
    return;
  }
  const OperationContext ctx = CaptureOperationContext();
  // 记录属于另一个仓库时绝不恢复：把别的仓库的引用挪走是纯粹的错。
  if (!ctx.repoUsable || !git::PathsEqualFolded(ctx.detection.root, record.workTreeRoot)) {
    state_.SetStatusNote(L"这条记录属于仓库 " + record.workTreeRoot + L"，当前绑定的是 " +
                         (ctx.detection.root.empty() ? std::wstring(L"（没有可用仓库）")
                                                     : ctx.detection.root) +
                         L"。请先切回那个仓库，再选这条记录恢复。");
    RefreshTexts(window);
    return;
  }
  restoreFlow_.Start(*this, ctx, CluesFromRecord(record));
}

void MainWindow::CopyRestoreCommandForRecord(HWND window, const app::OperationRecord& record) {
  // 复制的两条路要分清：引用级恢复复制的是「与实际执行同一份构造」的命令；涉及远端历史的记录
  // 压根没有可执行命令，复制的只是说明与目标信息——文案必须这么说。
  std::wstring payload;
  std::wstring note;
  if (record.restoreKind == app::HistoryRestoreKind::refMove) {
    const git::RestoreCopyText copy = git::DescribeRestoreCopy(CluesFromRecord(record),
                                                               git::ShellDialect::cmdInteractive);
    if (!copy.cluesUsable) {
      state_.SetStatusNote(L"这条记录的恢复线索不成立，没有可复制的命令：" + copy.refusal);
      RefreshTexts(window);
      return;
    }
    if (!copy.copyableCommand.empty()) {
      payload = copy.copyableCommand;
      note = L"已复制" + copy.dialectLabel +
             L"的一行恢复命令（未执行，也不代表已恢复远端）。" + copy.copyNote +
             L"粘贴前请核对目标与风险；换到别的 shell 就要重新核对这一行的写法。";
    } else {
      // 不能安全粘成一行：复制字段清单，不替换任何字符，也不替用户决定换哪个 shell。
      payload = copy.structuredFacts;
      note = L"这一串参数不能安全地粘成一行（" + copy.copyRefusal +
             L"），所以复制的是逐段列出的字段清单，不是命令。本程序没有替换任何字符；"
             L"要执行请在终端里自己按段输入。";
    }
  } else {
    payload = record.restoreNote;
    if (payload.empty()) {
      state_.SetStatusNote(L"这条记录没有可复制的恢复说明。");
      RefreshTexts(window);
      return;
    }
    // 历史里的自由文本按外部输入对待：它是给人看的线索，绝不包装成「可执行命令」。
    note = L"已复制这条记录的恢复说明（一段说明文字，不是可直接执行的命令，也没有执行任何东西）。";
    if (record.restoreKind == app::HistoryRestoreKind::manualRemote) {
      note += L"涉及远端历史的回退要你自己判断协作影响；本程序不提供 force push 这类命令。";
    }
  }
  std::wstring failure;
  if (platform::CopyTextToClipboard(window, payload, &failure)) {
    state_.SetStatusNote(note);
  } else {
    state_.SetStatusNote(L"复制到剪贴板失败：" + failure);
  }
  RefreshTexts(window);
}

}  // namespace gc::ui
