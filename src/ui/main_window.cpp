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
    L"正在合并／变基／拣选等流程没走完时本程序不做提交，也不会替你终止那种流程。";
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
    L"不猜 origin、不代设 upstream、不替你创建远端分支、不写任何配置文件。\r\n"
    L"branch.<分支>.pushRemote / remote.pushDefault / 独立 push URL / url.*.insteadOf 让实际发布"
    L"地点与抓取的那一侧不同时，会被解析出来明确展示并要求你明确点头。\r\n"
    L"命令窗口报告结束后，还会向确认框上列出的那些**发布目标**逐个发只读 ls-remote，核对那条引用"
    L"到底停在哪：推送成功与否以那份实况为准，本地引用看起来一致不算数；命令报成功却没核上时，"
    L"结论写「已推送但未核实」或「与预期不符」，不会自动重推。";
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
  HWND created = ::CreateWindowExW(0, kMainWindowWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                   CW_USEDEFAULT, metrics_.Scale(kInitialWindowWidth),
                                   metrics_.Scale(kInitialWindowHeight), nullptr, nullptr, instance, this);
  if (created == nullptr) {
    return false;
  }
  window_.Reset(created);
  ::ShowWindow(created, showCommand);
  ::UpdateWindow(created);
  return true;
}

void MainWindow::OnCreate(HWND window) {
  metrics_.UpdateForDpi(DpiForWindowOrSystem(window));
  ::SetWindowPos(window, nullptr, 0, 0, metrics_.Scale(kInitialWindowWidth), metrics_.Scale(kInitialWindowHeight),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

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
  UpdateCommandAvailability();
  InitializeRepoInput(window);
  RefreshTexts(window);
  ApplyWorkspaceLists();  // 首屏的空状态说明：列表不在 RefreshTexts 里重建，这里显式填一次。
  InitializeGitDetection(window);
  InitializeCommandWatching(window);
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
    case kIdTimeSyncCheck:
      if (notifyCode == BN_CLICKED) {
        // 勾选状态由控件自己翻转，这里只负责把联动规则跟上（置灰哪一半、说明怎么写）。
        RefreshTimeControlsState(window);
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
  // 初值取应用启动时的工作目录（只读取，不改变进程工作目录）。
  const std::wstring startupDirectory = platform::CurrentWorkingDirectory();
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
  // 启动即按当前进程 PATH 发现候选（等价 `where git` 的搜索意图，不扫盘）。
  const std::vector<std::wstring> candidates = platform::DiscoverGitCandidates();
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
  tool.status = verification.outcome == git::GitProbeOutcome::verified ? app::GitExeStatus::verified
                                                                       : app::GitExeStatus::invalid;
  state_.SetGitTool(std::move(tool));
  state_.SetStatusNote(verification.message);
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
    ReleaseCommitAttempt(true);
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
  ReleaseCommitAttempt(true);  // 同样也没有地方可提交了：那次还在核对的创建提交作废。
  refreshCycleActive_ = false;
  ClearWorkspace();
  state_.SetAuthor(app::AuthorState{});  // 没有可用工作区就没有可信的身份查询落点。
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
    ReleaseCommitAttempt(true);
    ClearWorkspace();
    state_.SetAuthor(app::AuthorState{});
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
  // 作者默认值与摘要、列表共用这一次刷新：识别成功后一起重读，不另建触发机制。
  RequestAuthorConfig(window);
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
    ReleaseCommitAttempt(true);
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
  if (pendingCommit_.stage == CommitStage::preConfirmRead) {
    // 这一次读取是「创建提交」点下去之后的第一步核对：列表读回来了，才轮到身份预检出场。
    if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
      AbandonCommitAttempt(
          window,
          L"提交前核对仓库现状时没能读到 git status：" + state_.Workspace().message +
              L" 因此没有打开命令窗口，也没有对仓库做任何改动。请改正后再点一次“创建提交”。");
      return;
    }
    RequestCommitPreflightProbe(window);
    if (tasks_.RefreshStillQueued()) {
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
  ::SetTimer(window, kGitOperationTimer, kGitOperationTickMs, nullptr);
}

bool MainWindow::LaunchCommandWindowOperation(HWND window,
                                             const git::CommandWindowOperation& operation,
                                             const CommandLaunchOptions& options) {
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
                                     options.pushOperation};
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
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };

  if (!state_.GitUsable() || !state_.RepoUsable()) {
    refuse(L"Git 或仓库当前不可用，无法" + std::wstring(actionLabel) + L"。请先确认路径并点“刷新”。");
    return false;
  }
  if (tasks_.OperationInFlight()) {
    // 命令窗口操作是单槽的：并发两次写操作会互相抢 index.lock，
    // 也会让“哪个窗口对应哪一次改动”变得含糊。
    refuse(L"已有一个命令窗口操作在进行，请等它结束后再" + std::wstring(actionLabel) + L"。");
    return false;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  if (repositoryRoot.empty() ||
      !git::PathsEqualFolded(repositoryRoot, tasks_.Identity().workTreeRoot)) {
    // 列表里的路径是“那一个工作区”的相对路径：根目录与协调器身份一旦不一致，
    // 这些路径就可能属于上一个仓库，绝不能拿去对新仓库执行写操作。
    refuse(L"仓库工作区已改变，请先点“刷新”再" + std::wstring(actionLabel) + L"。");
    return false;
  }
  return true;
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
        RunFormValidation(window, L"作者栏空着：下次读到这个仓库的有效配置时会自动填上，也可以直接写「姓名 <邮箱>」。");
        return;
      }
      formSession_.NoteUserEdit(Field::author);
      break;
    }
    default:
      return;  // 表單裡其它控件的通知與本函數無關（時間控件不構成本表單的正文）。
  }
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
  if (!formSession_.NeedsSwitchDecision(key)) {
    formSession_.BindRepository(key);
    return;
  }
  // 這裡一定要讓使用者當場選：悄悄沿用會把舊倉庫的默認作者帶進新倉庫，
  // 悄悄清空會把使用者打了幾百字的標題與描述丟掉——兩者都不可逆，程序無權取捨。
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
    // 「是」與「取消」都走保留：只有明確選「否」才丟棄。關閉對話框（等取消）不得吃掉使用者的字。
    formSession_.ChooseKeep(key);
    state_.SetFormNote(answer == IDYES
                             ? L"已保留你输入的表单内容；这里的作者保留下来后不再被新仓库的默认值覆盖。"
                             : L"表单内容一律没动。想改用新仓库的默认作者，清空“作者”那一栏即可。");
  }
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
  const std::wstring restoreHint = activeOperation_.restoreHint;
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
  // 「创建提交」的收尾只认 Git 的退出码：成功才清已提交的正文，失败时表单一个字都不动，
  // 用户可以直接改好再点一次（那种场合最不该丢的就是他刚写下来的东西）。
  if (commitOperation) {
    if (outcome.succeeded) {
      AfterCommitSucceeded(window, committedForm);
    } else {
      state_.SetFormNote(L"提交没有成功：标题、描述、作者与合作者一个字都没动，"
                         L"改好之后再点一次“创建提交”。命令窗口里留着 Git 的完整输出。");
    }
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
  // fetch 的结论必须把范围说死：成功只是「远端跟踪引用按 Git 的回答更新了」，
  // HEAD/本地分支/索引/工作区本来就不归它动；失败则明确「不自动重试、不改配置」，
  // 具体原因看命令窗口里留下的真实输出。紧随其后的自动刷新会重读分支摘要，
  // 下一次点「撤回最近提交」的发布状态判断用的就是这批新读回的引用。
  if (fetchOperation) {
    if (outcome.succeeded) {
      conclusion += L"｜fetch 只按确认框上那份范围更新了远端跟踪引用（.git/FETCH_HEAD 与对象库随抓取变化，"
                    L"这是 fetch 本身的行为）；HEAD、本地分支、索引与工作区不归它动。"
                    L"分支摘要正按新状态重读。";
    } else {
      conclusion += L"｜fetch 未成功：远端跟踪引用是否变化以重读结果为准。本程序不自动重试，"
                    L"也不会因此 prune、换远端或改写任何远端配置；原因看命令窗口里 Git 的真实输出。";
    }
  }
  // pull 的两个阶段各自有各自的结论：获取成功只是「远端跟踪引用按承诺更新了」，
  // 整合的结论才涉及分支/索引/工作区。两者都不把「窗口还开着」当成 Git 成功。
  if (pullFetchOperation) {
    conclusion += outcome.succeeded
                      ? L"｜pull 第一步（获取）完成：只按确认框上那份范围更新了远端跟踪引用"
                        L"（FETCH_HEAD 与对象库随之变化）；正在重读现状并核对本地与远端的关系…"
                      : L"｜pull 停在第一步：命令窗口里那次获取没有成功，因此没有做任何整合。"
                        L"本程序不自动重试，也不会因此 prune、换远端或改写任何远端配置。";
  }
  if (pullIntegrateOperation) {
    conclusion += outcome.succeeded
                      ? L"｜pull 整合完成：本地分支已按刚才预检的那一份提交整合过；"
                        L"本程序没有 push、没有 reset、没有 stash，也没有改任何配置。"
                        L"现在正在重读分支、列表与历史。"
                      : ReportPullIntegrateFailure(window, result.completion, result.exitCode);
  }
  // 推送的结论在这里只写到「命令窗口那头说了什么」：Git 报告成功不等于远端收到了那一份提交
  // （认证被拒、对端钩子拦下、窗口被提前关掉都可能让两者不一致）。
  // 真正的「送到没送到」由紧随其后的核实给出，它回来后会另写一句结论。
  if (pushOperation) {
    conclusion += outcome.succeeded
                      ? std::wstring(L"｜命令窗口里 Git 报告推送成功（退出码 0）。")
                      : std::wstring(L"｜推送没有成功：本程序不自动重试，也不会改用 --force 之类"
                                     L"更激烈的参数。原因看命令窗口里 Git 的真实输出。");
    conclusion += L"正在向确认框上列出的发布目标核实那条引用的实际位置…";
  }
  // 集中环境策略移除过继承的重定向变量时，结论必须把这句话说完：
  // 用户从终端启动本程序时要知道“那些变量被移除了、操作绑定的是界面上选中的仓库”。
  if (!result.environmentNotice.empty()) {
    conclusion += L"｜" + result.environmentNotice;
  }
  tasks_.RememberOperationConclusion(conclusion);
  // 已完成但保留的窗口不影响后续操作，只清理已取回的结果记录。
  commandRunner_.ClearAllResults();
  UpdateCommandAvailability();
  // pull 的第一步（获取）终态不是这次操作的终点：成功就接着做阶段二预检，失败就此为止。
  // 这一句必须排在槽位释放与可用性刷新之后——那一次预检是内部只读查询，不占命令窗口槽位。
  if (pullFetchOperation) {
    OnPullFetchSettled(window, outcome.succeeded);
  } else if (pullIntegrateOperation) {
    // 整合有了终态（含「结果未知」）就结案：这一次 pull 到此为止，要不要再来由用户重新点。
    pendingPull_ = PendingPull{};
  }
  if (pushOperation) {
    // 这一次推送本身到此结案（计划里剩下的只有核实）。核实无论成败都要发：
    // 命令报成功时要拿它当证据，命令报失败时它正是「远端到底动没动」的唯一凭据。
    const bool commandSucceeded = outcome.succeeded;
    const std::wstring commandConclusion = conclusion;
    pendingPush_.stage = PushStage::verifying;
    RequestPushVerification(window, commandSucceeded, commandConclusion);
  }
  // 无论成功还是失败都要重读一次：失败的操作同样可能已经改动仓库
  // （提交到一半、push 被拒、合并留下冲突），只有退出码决定要不要报成功。
  ScheduleRefresh(window);
  RefreshTexts(window);
}

void MainWindow::TickActiveOperations(HWND window) {
  // 轮询只负责把“执行中”刷成最新可见文本，并兜住完成通知丢失的场合
  // （真正的完成判定来自观察线程的通知，不靠这里的文案匹配）。
  if (activeOperation_.serial != 0 &&
      !commandRunner_.DescribeOperation(activeOperation_.runnerId, nullptr, nullptr)) {
    const unsigned long long serial = activeOperation_.serial;
    const std::wstring name = activeOperation_.displayName;
    const bool pullStepInProgress = activeOperation_.pullFetchOperation ||
                                    activeOperation_.pullIntegrateOperation;
    const bool pushStepInProgress = activeOperation_.pushOperation;
    // 这里不删清单文件：通知丢失意味着 Git 可能还在命令窗口里跑，删掉正在被读的文件
    // 会让一次合法的 git add 变成 Git 的报错。%TEMP% 里留下几百字节的清单远小于那个代价。
    activeOperation_ = ActiveOperation{};
    // pull 的那两步本来就靠这条终态通知往下走（获取成功才问关系）。通知既然丢了，
    // 就把这一次 pull 结案：界面不再停在「等待某一步」上，否则下一次点 pull 会被自己锁住。
    // 仓库究竟被改到什么程度只有重读知道，因此这里同样只重读、不替用户猜。
    if (pullStepInProgress) {
      pendingPull_ = PendingPull{};
    }
    // 推送的那几步（预检/复核/命令窗口）同样靠终态通知往下走。通知丢了就把这一次推送结案，
    // 否则界面会永远停在「等那一次推送」上，把下一次点击锁死。核实本来就只在终态之后才发起，
    // 走到这里说明它没机会跑：结论只能以命令窗口那头的输出为准，如实这么说。
    if (pushStepInProgress) {
      pendingPush_ = PendingPush{};
    }
    // 执行器已经不记得这个操作：按“结果未知”结案并释放槽位，
    // 否则一个再也等不到通知的操作会把后续写操作永久锁住。
    const app::OperationOutcome outcome =
        tasks_.ForgetOperation(serial, L"本程序没有收到「" + name + L"」的完成通知");
    if (outcome.recognised) {
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
}

bool MainWindow::BuildCommitTimeChoice(const git::CivilTime& wall, git::CommitTimeChoice* out,
                                       std::wstring* refusal) const {
  *out = git::CommitTimeChoice{};
  const platform::LocalInstant instant = platform::ResolveLocalWallTime(wall);
  if (!instant.valid) {
    *refusal = instant.failureReason;
    return false;
  }
  std::string gitDate;
  std::wstring dateRefusal;
  if (!git::FormatGitInternalDate(instant.utcEpochSeconds, instant.offsetMinutes, &gitDate,
                                 &dateRefusal)) {
    *refusal = dateRefusal;
    return false;
  }
  out->gitDate = std::move(gitDate);
  out->wall = wall;
  out->offsetMinutes = instant.offsetMinutes;
  out->displayText = git::FormatCommitTimeText(wall, instant.offsetMinutes);
  return true;
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
  RefreshTexts(window);
}

void MainWindow::CreateCommit(HWND window) {
  if (!RequireWritePrerequisites(window, L"创建提交")) {
    return;
  }
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };
  if (CommitAttemptActive()) {
    refuse(L"已经有一次“创建提交”正在核对仓库现状（或正在做执行前复核），请等它的确认框出现，"
           L"或先取消那一次。");
    return;
  }
  if (state_.Workspace().status != git::WorkspaceLoadStatus::loaded) {
    refuse(L"还没读到这个仓库的工作区状态（" + state_.Workspace().message +
           L"），无法确定要提交什么。请先点“刷新”。");
    return;
  }

  // 正文与身份的校验：规则在 git/commit_message 里，这里只把它的答案当执行前提。
  const git::CommitFormData data = commitForm_.Capture();
  const git::CommitFormValidity validity =
      git::ValidateCommitForm(data, state_.Author().config.CommitterState());
  if (!validity.Ok()) {
    RunFormValidation(window);
    refuse(L"表单还没通过校验，因此没有提交任何内容：" + validity.StatusText());
    return;
  }

  // 「默认用提交时的当前时间」在这里落地：没人动过时间控件时，先把控件改成此刻，
  // 让用户亲眼看到要用的到底是哪个时间；应用启动时那个时刻绝不代替它。
  if (!commitForm_.TimesUserEdited()) {
    const SYSTEMTIME now = platform::CurrentLocalTime();
    commitForm_.SetTimes(now, now);
    RefreshTimeControlsState(window);
  }

  git::CommitTimeChoice authorTime;
  git::CommitTimeChoice committerTime;
  std::wstring refusal;
  if (!BuildCommitTimeChoice(commitForm_.AuthorWallTime(), &authorTime, &refusal)) {
    refuse(L"作者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }
  if (commitForm_.TimeSyncChecked()) {
    committerTime = authorTime;  // 同步勾选：提交者时间以作者时间为准（界面也已一致显示）。
  } else if (!BuildCommitTimeChoice(commitForm_.CommitterWallTime(), &committerTime, &refusal)) {
    refuse(L"提交者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }

  // 记下点击瞬间界面显示的那份摘要，然后发起一次只读重读：
  // 确认框必须摆出「刚刚读回的仓库现状」，不能拿几分钟前的列表当真。
  pendingCommit_.captured = git::CapturedSnapshot{};
  pendingCommit_.captured.valid = true;
  pendingCommit_.captured.shortSha = state_.Repo().detection.shortSha;
  pendingCommit_.captured.hasHead = state_.Repo().detection.headResolved;
  pendingCommit_.captured.stagedItems = state_.WorkspaceModel().staged.size();
  pendingCommit_.stage = CommitStage::preConfirmRead;
  ScheduleRefresh(window);
  state_.SetStatusNote(L"创建提交前先在后台重读仓库现状（只读查询，不弹命令窗口、不改动仓库），"
                       L"读回来后还要问齐这次提交要绑定的事实，然后给出确认框…");
  RefreshTexts(window);
}

void MainWindow::ReleaseCommitAttempt(bool reclaimMessageFile) {
  if (reclaimMessageFile && !pendingCommit_.messageFile.empty()) {
    // 这份信息文件从没进过命令窗口，Git 不会来读它：随这次尝试一起回收。
    platform::RemoveCommitMessageFile(pendingCommit_.messageFile);
  }
  pendingCommit_ = PendingCommit{};
}

void MainWindow::AbandonCommitAttempt(HWND window, std::wstring_view reason) {
  ReleaseCommitAttempt(true);
  state_.SetStatusNote(std::wstring(reason));
  RefreshTexts(window);
}

void MainWindow::RequestCommitPreflightProbe(HWND window) {
  pendingCommit_.stage = CommitStage::preConfirmProbe;
  platform::CommitProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kCommitProbeTimeoutMs;
  commitWorker_.Request(window, kCommitProbeCompleted, std::move(request),
                        [](const platform::CommitProbeRequest& pending) {
                          return platform::RunCommitProbeLoad(pending);
                        });
  state_.SetStatusNote(L"创建提交前先在后台问齐这次提交要绑定的事实：工作区根与 Git 目录、完整分支引用、"
                       L"HEAD 完整对象 ID、索引内容标识、Git 目录里的流程痕迹、有效配置里的提交者身份。"
                       L"其中算索引内容用的 git write-tree 会把那棵树写进对象库、顺带刷新索引里过期的"
                       L"文件状态（不产生提交、不移动分支、不碰工作区文件）；问回来后给出确认框…");
  RefreshTexts(window);
}

void MainWindow::OnCommitProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::CommitProbeOutcome outcome;
  if (!commitWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间已发起更晚的一趟查询，这份结果不再有意义。
  }
  const CommitStage stage = pendingCommit_.stage;
  if (stage != CommitStage::preConfirmProbe && stage != CommitStage::executionRecheck) {
    return;  // 这一次尝试已经按「取消 / 换仓库 / 作废」结束，迟到的结果原样丢掉。
  }
  if (!state_.RepoUsable() ||
      !git::PathsEqualFolded(outcome.repositoryDirectory, state_.Repo().detection.root)) {
    // 查询是在旧仓库上跑的：那份 HEAD/索引内容对当前界面显示的仓库毫无意义，
    // 表单原样留着，用户对新仓库重新点一次即可。
    AbandonCommitAttempt(window, L"预检完成时仓库已经换掉，这次提交没有发出任何命令，"
                                 L"刚写的提交信息文件已删除。表单里的内容一个字都没动，"
                                 L"请对现在的仓库重新点一次“创建提交”。");
    return;
  }
  if (stage == CommitStage::preConfirmProbe) {
    HandleCommitPreflightProbe(window, outcome);
  } else {
    HandleCommitRecheckProbe(window, outcome);
  }
}

void MainWindow::HandleCommitPreflightProbe(HWND window, const platform::CommitProbeOutcome& outcome) {
  const auto abandon = [&](std::wstring_view reason) { AbandonCommitAttempt(window, reason); };

  const git::CommitFormData data = commitForm_.Capture();
  const git::CommitFormValidity validity =
      git::ValidateCommitForm(data, outcome.facts.committer);
  if (!validity.Ok()) {
    RunFormValidation(window);
    abandon(L"预检回来之后表单校验没通过，因此没有提交任何内容：" + validity.StatusText());
    return;
  }
  git::GitIdentity author;
  std::wstring identityError;
  if (!git::ParseGitIdentity(data.author, &author, &identityError, L"作者")) {
    abandon(L"作者身份没能拆解成「姓名 <邮箱>」，因此没有提交任何内容：" + identityError);
    return;
  }

  git::CommitTimeChoice authorTime;
  git::CommitTimeChoice committerTime;
  std::wstring refusal;
  if (!BuildCommitTimeChoice(commitForm_.AuthorWallTime(), &authorTime, &refusal)) {
    abandon(L"作者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }
  if (commitForm_.TimeSyncChecked()) {
    committerTime = authorTime;
  } else if (!BuildCommitTimeChoice(commitForm_.CommitterWallTime(), &committerTime, &refusal)) {
    abandon(L"提交者时间没能换算成 Git 的记录形态，因此没有提交任何内容：" + refusal);
    return;
  }

  const git::ComposedCommitMessage composed = git::ComposeCommitMessage(data);
  if (!composed.rejectedCoauthors.empty() || composed.message.empty()) {
    // 校验已经过了才会走到这里，出现这种组合说明表单内容在校验之后又被改动了：
    // 宁可不提交，也不按一份没核对过的内容写提交信息。
    abandon(L"提交信息合成时出现了没能解析的合作者条目，因此没有提交任何内容。"
            L"请检查“合作者”列表后重新点击“创建提交”。");
    return;
  }

  const platform::CommitMessageFileWrite written =
      platform::WriteCommitMessageFile(composed.message);
  if (!written.written) {
    abandon(L"没有打开命令窗口，也没有对仓库做任何改动。写提交信息文件失败：" +
            written.failureReason);
    return;
  }
  // 文件已经存在了，先登记保管人：下面任何一条拒绝路径都要顺手回收它。
  pendingCommit_.messageFile = written.path;

  git::CommitPlanInput input;
  input.model = state_.WorkspaceModel();
  input.detection = state_.Repo().detection;
  // 提交者可得性、那串身份文字与流程痕迹一律取自这一趟预检，不再取界面早前存下的那一份：
  // 确认框上写的、复核时比对的必须是同一次查询问回来的同一个东西。
  input.identity = outcome.facts;
  input.gitExecutable = state_.Git().path;
  input.captured = pendingCommit_.captured;
  pendingCommit_.captured = git::CapturedSnapshot{};
  input.author = author;
  input.message = composed.message;
  input.messageUtf8Bytes = written.payloadBytes;
  input.messageFilePath = written.path;
  input.authorTime = authorTime;
  input.committerTime = committerTime;
  input.timesSynced = commitForm_.TimeSyncChecked();

  const git::CommitPlan plan = git::BuildCommitPlan(input);
  if (plan.blocked) {
    // 方案层拒绝时这条命令根本不存在，信息文件也就没人会去读：随这次尝试一起回收。
    AbandonCommitAttempt(window, L"没有打开命令窗口，也没有对仓库做任何改动。" + plan.blockedReason);
    return;
  }

  pendingCommit_.preflight = outcome;
  pendingCommit_.plan = plan;
  pendingCommit_.confirmedForm = data;

  const int answer = ::MessageBoxW(window, plan.previewText.c_str(), L"创建提交前请确认",
                                   MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
  if (answer != IDOK) {
    AbandonCommitAttempt(window,
                         L"已取消：没有打开命令窗口，也没有对仓库做任何改动。刚写的提交信息文件已删除，"
                         L"表单里的内容一个字都没动。");
    return;
  }
  RequestCommitExecutionRecheck(window);
}

void MainWindow::RequestCommitExecutionRecheck(HWND window) {
  pendingCommit_.stage = CommitStage::executionRecheck;
  platform::CommitProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kCommitProbeTimeoutMs;
  commitWorker_.Request(window, kCommitProbeCompleted, std::move(request),
                        [](const platform::CommitProbeRequest& pending) {
                          return platform::RunCommitProbeLoad(pending);
                        });
  state_.SetStatusNote(L"点头之后、发出命令之前，把预检那组只读查询原样重发一遍：工作区根与 Git 目录 / "
                       L"完整分支引用 / HEAD 完整对象 ID / 索引内容标识 / 流程痕迹 / 提交者身份都对得上，"
                       L"才启动那条 git commit…");
  RefreshTexts(window);
}

void MainWindow::HandleCommitRecheckProbe(HWND window, const platform::CommitProbeOutcome& outcome) {
  const std::wstring change =
      git::DescribeCommitIdentityChange(pendingCommit_.plan.identity, outcome.facts);
  if (!change.empty()) {
    // 复核不过：作废的是「这一份现状」，不是用户写下的内容。表单原样留着，信息文件回收，
    // 重读回来后由用户自己决定要不要对新现状再确认一次。
    ::MessageBoxW(window, change.c_str(), L"执行前复核：仓库又变了", MB_OK | MB_ICONWARNING);
    AbandonCommitAttempt(window,
                         L"执行前复核发现现状与确认框上写的不一致，因此没有发出那条提交命令。"
                         L"表单里的内容一个字都没动，仓库状态正在重读，"
                         L"看清现状后如仍要提交请再点一次“创建提交”。");
    ScheduleRefresh(window);
    return;
  }
  // 方案取一份副本：启动流程要把它的内容搬进命令与选项，而这次尝试的记录在启动前就被清掉。
  const git::CommitPlan plan = pendingCommit_.plan;
  const git::CommitFormData committedForm = pendingCommit_.confirmedForm;
  LaunchCommit(window, plan, committedForm);
}

void MainWindow::LaunchCommit(HWND window, const git::CommitPlan& plan,
                              const git::CommitFormData& committedForm) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  // 启动参数一律取自这份「确认框上写的、并且刚刚复核过的」身份，不再回读界面此刻的状态：
  // 命令属于那个仓库，就不能因为界面后来换了显示而跑到别的目录去。
  operation.gitExecutable = plan.identity.gitExecutable;
  operation.repositoryDirectory = plan.identity.repositoryDirectory;
  operation.arguments = plan.arguments;
  operation.environmentOverrides = plan.environmentOverrides;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（" +
                        plan.identity.repositoryDirectory + L"），本次提交 " +
                        std::to_wstring(plan.stagedItems) + L" 项已暂存内容（索引内容标识 " +
                        git::ShortObjectId(plan.identity.indexTreeOid) + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.messageFile = plan.identity.messageFilePath;
  options.commitOperation = true;
  options.committedForm = committedForm;
  // 信息文件的所有权在这里转交给 ActiveOperation：只有拿到终态（或启动失败由执行路径回收）才删，
  // 命令窗口里的 Git 可能还在读它。
  ReleaseCommitAttempt(false);
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    // 启动失败时信息文件已由执行路径回收，这里只补一句表单没动的说明。
    state_.SetStatusNote(L"这次提交没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                         L"表单里的内容一个字都没动。");
    RefreshTexts(window);
    return;
  }
  if (!plan.stateChangeNote.empty()) {
    // 用户已经看过那句「以刚读回的为准」，这一句留在状态栏里，操作结束后还能对上号。
    state_.SetStatusNote(L"已在命令窗口启动创建提交。" + plan.stateChangeNote);
  }
}

namespace {

// 「强制撤回（仅本地）」风险确认框：按钮文字必须是这两个明确的说法，
// MessageBox 的按钮不可自定义，所以用 TaskDialog（Common Controls v6，app.manifest 已声明）。
// 万一 TaskDialog 初始化失败（异常环境），退回是/否形态并在正文里点明哪个按钮是什么——
// 无论哪种形态，「强制」都只是确认越过本程序的风险提示，命令本身一字不变。
bool ShowForceUndoConfirm(HWND window, const std::wstring& preview) {
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = window;
  config.hInstance = ::GetModuleHandleW(nullptr);
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.pszWindowTitle = L"撤回最近提交：风险确认";
  config.pszMainInstruction = L"这条撤回带有需要你自己核对的风险";
  config.pszContent = preview.c_str();
  config.pszMainIcon = TD_WARNING_ICON;
  const TASKDIALOG_BUTTON buttons[] = {
      {IDYES, L"强制撤回（仅本地）"},
      {IDNO, L"取消"},
  };
  config.pButtons = buttons;
  config.cButtons = ARRAYSIZE(buttons);
  config.nDefaultButton = IDNO;
  int button = IDNO;
  const HRESULT hr = ::TaskDialogIndirect(&config, &button, nullptr, nullptr);
  if (FAILED(hr)) {
    const int answer =
        ::MessageBoxW(window,
                      (preview + L"\n\n（风险确认框未能按样式打开：这里点“是”等同“强制撤回（仅本地）”，"
                                L"点“否”取消。）")
                          .c_str(),
                      L"撤回最近提交：风险确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return answer == IDYES;
  }
  return button == IDYES;
}

}  // namespace

void MainWindow::UndoLastCommit(HWND window) {
  if (!RequireWritePrerequisites(window, L"撤回最近提交")) {
    return;
  }
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };
  if (pendingUndo_.probing) {
    refuse(L"已经有一次撤回预检在跑，请等确认框出现，或先取消那一次。");
    return;
  }
  if (CommitAttemptActive()) {
    refuse(L"「创建提交」正在核对仓库现状（或正在做执行前复核），请先等那一步结束，再来撤回。");
    return;
  }

  // 记下点击瞬间界面显示的那份摘要：预检回来后与它对比，不一致时确认框必须说明
  // 「以下以刚读回的为准」（与创建提交同一套原则，不假装旧状态还成立）。
  pendingUndo_.captured = git::CapturedSnapshot{};
  pendingUndo_.captured.valid = true;
  pendingUndo_.captured.shortSha = state_.Repo().detection.shortSha;
  pendingUndo_.captured.hasHead = state_.Repo().detection.headResolved;
  pendingUndo_.captured.stagedItems = state_.WorkspaceModel().staged.size();
  pendingUndo_.probing = true;

  platform::UndoProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kUndoProbeTimeoutMs;
  undoWorker_.Request(window, kUndoProbeCompleted, std::move(request),
                      [](const platform::UndoProbeRequest& pending) {
                        return platform::RunUndoProbeLoad(pending);
                      });
  state_.SetStatusNote(L"撤回最近提交前先在后台重读分支、HEAD、父提交、远端跟踪引用与工作区状态"
                       L"（只读查询，不弹命令窗口、不改动仓库），读回来后给出确认框…");
  RefreshTexts(window);
}

void MainWindow::AbandonUndoAttempt(HWND window, std::wstring_view reason) {
  pendingUndo_.probing = false;
  pendingUndo_.captured = git::CapturedSnapshot{};
  state_.SetStatusNote(std::wstring(reason));
  RefreshTexts(window);
}

void MainWindow::OnUndoProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::UndoProbeOutcome outcome;
  if (!undoWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  if (!pendingUndo_.probing) {
    return;  // 不是等中的那一次（例如已按“仓库切换”作废）。
  }
  pendingUndo_.probing = false;
  if (!state_.RepoUsable() ||
      !git::PathsEqualFolded(outcome.repositoryDirectory, state_.Repo().detection.root)) {
    // 预检是在旧仓库上跑的：那份 HEAD/父提交/远端事实对当前界面显示的仓库毫无意义。
    AbandonUndoAttempt(window,
                       L"预检完成时仓库已经换掉，本次没有执行任何撤回。请对现在的仓库重新点一次“撤回最近提交”。");
    return;
  }
  ConfirmAndLaunchUndo(window, outcome.facts);
}

void MainWindow::ConfirmAndLaunchUndo(HWND window, const git::UndoPreflightFacts& facts) {
  const auto abandon = [&](std::wstring_view reason) { AbandonUndoAttempt(window, reason); };

  git::UndoCommitPlanInput input;
  input.facts = facts;
  input.workflow = platform::ProbeRepositoryWorkflowState(state_.Repo().detection.absoluteGitDir);
  input.repositoryRoot = state_.Repo().detection.root;
  input.captured = pendingUndo_.captured;
  pendingUndo_.captured = git::CapturedSnapshot{};

  const git::UndoCommitPlan plan = git::BuildUndoCommitPlan(input);
  if (plan.blocked) {
    const std::wstring message = L"没有打开命令窗口，也没有对仓库做任何改动。\n\n" + plan.blockedReason;
    ::MessageBoxW(window, message.c_str(), L"无法撤回最近提交", MB_OK | MB_ICONINFORMATION);
    abandon(L"未执行撤回：" + plan.blockedReason);
    return;
  }

  const bool proceed =
      plan.requiresForce
          ? ShowForceUndoConfirm(window, plan.previewText)
          : ::MessageBoxW(window, plan.previewText.c_str(), L"撤回最近提交前请确认",
                          MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
  if (!proceed) {
    abandon(L"已取消：没有打开命令窗口，也没有对仓库做任何改动。");
    return;
  }

  // 用户点头之后再同步核对一次 HEAD/分支：确认框是模态的，但期间外部终端照样可能动过仓库。
  // 对不上就取消并刷新——绝不对着已经变了的目标执行旧方案。
  const git::UndoHeadFacts recheck = platform::CaptureUndoHeadSnapshot(
      state_.Git().path, state_.Repo().detection.root, kUndoRecheckTimeoutMs);
  if (!recheck.queryOk) {
    abandon(L"确认后复核 HEAD 没能完成（" +
            (recheck.queryFailure.empty() ? std::wstring(L"原因未知") : recheck.queryFailure) +
            L"），本次没有执行任何命令。仓库状态正在重读，请看清现状后再来。");
    ScheduleRefresh(window);
    return;
  }
  if (recheck.branchRef != facts.head.branchRef || recheck.headObjectId != facts.head.headObjectId) {
    abandon(L"确认之后、执行之前，HEAD/分支又变了（分支：" +
            (recheck.branchRef.empty() ? std::wstring(L"不在分支上") : recheck.branchRef) +
            L"；HEAD：" +
            (recheck.headObjectId.empty() ? std::wstring(L"尚无提交")
                                          : git::ShortObjectId(recheck.headObjectId)) +
            L"）。本次没有执行任何命令。仓库状态正在重读，看清现状后如仍要撤回请再点一次。");
    ScheduleRefresh(window);
    return;
  }

  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = state_.Repo().detection.root;
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（" +
                        state_.Repo().detection.root + L"），撤回提交 " +
                        git::ShortObjectId(facts.head.headObjectId) + L"，等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.undoOperation = true;
  options.restoreHint = plan.restoreHint;
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    state_.SetStatusNote(L"这次撤回没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    RefreshTexts(window);
    return;
  }
  if (!plan.stateChangeNote.empty()) {
    // 用户已经看过那句「以刚读回的为准」，这一句留在状态栏里，操作结束后还能对上号。
    state_.SetStatusNote(L"已在命令窗口启动撤回最近提交。" + plan.stateChangeNote);
  }
}

void MainWindow::RequestFetch(HWND window) {
  if (!RequireWritePrerequisites(window, L"fetch")) {
    return;
  }
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };
  if (fetchProbing_) {
    refuse(L"已经有一次 fetch 目标预检在跑，请等它的界面出现，再点不会排队。");
    return;
  }
  if (pendingPush_.stage != PushStage::none) {
    refuse(L"「推送」还在走它的预检、复核或核实，请先让那一步结束，再来 fetch。");
    return;
  }
  if (pendingUndo_.probing) {
    refuse(L"「撤回最近提交」的预检还在跑，请先等它的确认框出现，再来 fetch。");
    return;
  }
  if (CommitAttemptActive()) {
    refuse(L"「创建提交」正在核对仓库现状（或正在做执行前复核），请先等那一步结束，再来 fetch。");
    return;
  }

  fetchProbing_ = true;
  platform::FetchProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.timeoutMilliseconds = kFetchProbeTimeoutMs;
  fetchWorker_.Request(window, kFetchProbeCompleted, std::move(request),
                       [](const platform::FetchProbeRequest& pending) {
                         return platform::RunFetchProbeLoad(pending);
                       });
  state_.SetStatusNote(L"fetch 前先在后台只读询问：当前分支、这个分支配置的远端、仓库既有远端清单，"
                       L"外加会影响抓取范围的配置（prune／pruneTags／标签跟随与这个远端的 fetch 映射）"
                       L"——都是只读查询，不弹命令窗口、不接触任何远端；问回来后核对范围并给出抓取目标…");
  RefreshTexts(window);
}

void MainWindow::OnFetchProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::FetchProbeOutcome outcome;
  if (!fetchWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  if (!fetchProbing_) {
    return;  // 不是等中的那一次（例如已按「仓库切换」作废）。
  }
  fetchProbing_ = false;
  const auto settle = [&](std::wstring_view reason) {
    state_.SetStatusNote(std::wstring(reason));
    RefreshTexts(window);
  };
  if (!state_.RepoUsable() ||
      !git::PathsEqualFolded(outcome.repositoryDirectory, state_.Repo().detection.root)) {
    // 预检是在旧仓库上跑的：那份远端清单对当前界面显示的仓库毫无意义。
    settle(L"预检完成时仓库已经换掉，本次没有执行 fetch。请对现在的仓库重新点一次“fetch”。");
    return;
  }
  const std::wstring repositoryRoot = state_.Repo().detection.root;
  git::FetchPlan plan = git::BuildFetchPlan(outcome.facts, repositoryRoot);

  if (plan.state == git::FetchPlanState::chooseRemote) {
    // 分支配置定不下目标：把既有远端一个个摆出来，目标（名字与 URL）必须看得见才谈得上「不猜」。
    platform::RemoteChoiceSpec spec;
    spec.title = L"选择 fetch 的远端";
    spec.label = plan.explanation;
    for (const git::FetchRemoteEntry& entry : plan.candidates) {
      spec.items.push_back({entry.name, entry.fetchUrl});
    }
    spec.font = metrics_.Font();
    spec.layout.margin = metrics_.Margin();
    spec.layout.gap = metrics_.RowGap();
    spec.layout.width = metrics_.Scale(420);
    spec.layout.labelHeight = 3 * metrics_.LabelHeight();  // 「为什么要在这里选」那句说明留三行。
    spec.layout.listHeight = metrics_.Scale(140);
    spec.layout.buttonWidth = metrics_.ButtonWidth(L"抓取选中的远端");
    spec.layout.buttonHeight = metrics_.ControlHeight();
    const platform::RemoteChoiceResult choice = platform::PromptForRemoteChoice(window, spec);
    if (!choice.accepted || choice.selectedIndex < 0 ||
        static_cast<size_t>(choice.selectedIndex) >= plan.candidates.size()) {
      settle(L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。");
      return;
    }
    plan = git::ChooseFetchRemote(outcome.facts,
                                  plan.candidates[static_cast<size_t>(choice.selectedIndex)].name,
                                  repositoryRoot);
  }

  if (plan.state != git::FetchPlanState::ready) {
    const std::wstring message = L"没有打开命令窗口，也没有接触任何远端或改动仓库。\n\n" + plan.explanation;
    ::MessageBoxW(window, message.c_str(), L"现在不能 fetch", MB_OK | MB_ICONINFORMATION);
    settle(L"未执行 fetch。");
    return;
  }

  const int answer = ::MessageBoxW(window, plan.confirmationText.c_str(), L"fetch 前请确认",
                                   MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON2);
  if (answer != IDOK) {
    settle(L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。");
    return;
  }
  LaunchFetch(window, plan);
}

void MainWindow::LaunchFetch(HWND window, const git::FetchPlan& plan) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = state_.Repo().detection.root;
  // 参数按数组提交，不进任何 shell 字符串；目标只认名字，URL 由 Git 自己按配置解析。
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（目标：" + plan.remoteName +
                        L" @ " +
                        (plan.remoteUrl.empty() ? std::wstring(L"URL 未记录") : plan.remoteUrl) +
                        L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.fetchOperation = true;
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    state_.SetStatusNote(L"这次 fetch 没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。");
    RefreshTexts(window);
  }
}

bool MainWindow::ShowPullRiskConfirm(HWND window, const std::wstring& preview) {
  // 与「强制撤回（仅本地）」同一套规矩：按钮文字必须说清点的到底是哪两样。
  // 这里的「继续」只越过本程序的风险提示，不追加任何更激烈的参数——命令与无风险时一字不差，
  // 尤其不含 --force、--no-verify、--autostash，也不会替你 reset/abort。
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = window;
  config.hInstance = ::GetModuleHandleW(nullptr);
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.pszWindowTitle = L"pull 整合：风险确认";
  config.pszMainInstruction = L"这份预检带有需要你自己核对的风险";
  config.pszContent = preview.c_str();
  config.pszMainIcon = TD_WARNING_ICON;
  const TASKDIALOG_BUTTON buttons[] = {
      {IDYES, L"按这份预检继续整合"},
      {IDNO, L"取消"},
  };
  config.pButtons = buttons;
  config.cButtons = ARRAYSIZE(buttons);
  config.nDefaultButton = IDNO;
  int button = IDNO;
  const HRESULT hr = ::TaskDialogIndirect(&config, &button, nullptr, nullptr);
  if (FAILED(hr)) {
    const int answer =
        ::MessageBoxW(window,
                      (preview + L"\n\n（风险确认框未能按样式打开：这里点“是”等同“按这份预检继续整合”，"
                                L"点“否”取消。）")
                          .c_str(),
                      L"pull 整合：风险确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return answer == IDYES;
  }
  return button == IDYES;
}

void MainWindow::AbandonPullAttempt(HWND window, std::wstring_view reason) {
  pendingPull_ = PendingPull{};
  state_.SetStatusNote(std::wstring(reason));
  RefreshTexts(window);
}

void MainWindow::RequestPull(HWND window) {
  if (!RequireWritePrerequisites(window, L"pull")) {
    return;
  }
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };
  if (pendingPull_.stage != PullStage::none) {
    refuse(L"已经有一次 pull 在走流程（预检、获取或整合），请等它结束或先取消那一步。");
    return;
  }
  if (pendingPush_.stage != PushStage::none) {
    refuse(L"「推送」还在走它的预检、复核或核实，请先让那一步结束，再来 pull。");
    return;
  }
  if (fetchProbing_) {
    refuse(L"「fetch」的目标预检还在跑，请先等它的界面出现，再来 pull。");
    return;
  }
  if (pendingUndo_.probing) {
    refuse(L"「撤回最近提交」的预检还在跑，请先等它的确认框出现，再来 pull。");
    return;
  }
  if (CommitAttemptActive()) {
    refuse(L"「创建提交」正在核对仓库现状（或正在做执行前复核），请先等那一步结束，再来 pull。");
    return;
  }

  pendingPull_.stage = PullStage::fetchProbe;
  platform::PullProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.absoluteGitDir = state_.Repo().detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullProbeTimeoutMs;
  request.includeRelationship = false;
  pullWorker_.Request(window, kPullProbeCompleted, std::move(request),
                      [](const platform::PullProbeRequest& pending) {
                        return platform::RunPullProbeLoad(pending);
                      });
  state_.SetStatusNote(L"pull 第一步：先在后台只读问清「在哪个分支、这个分支的上游是谁、你的 "
                       L"pull/rebase 与 ff 配置怎么写的、会影响抓取范围的配置（prune／标签／fetch 映射）、"
                       L"工作区现状」（不弹命令窗口、不接触任何远端），问回来后先把要处理的分支对摆给你看…");
  RefreshTexts(window);
}

void MainWindow::OnPullProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::PullProbeOutcome outcome;
  if (!pullWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  const PullStage stage = pendingPull_.stage;
  if (stage == PullStage::none) {
    return;  // 这一次 pull 已经按「取消 / 换仓库 / 结案」作废，迟到的结果原样丢掉。
  }
  if (!state_.RepoUsable() ||
      !git::PathsEqualFolded(outcome.repositoryDirectory, state_.Repo().detection.root)) {
    AbandonPullAttempt(window,
                       L"预检完成时仓库已经换掉，这次 pull 没有执行任何命令。请对现在的仓库重新点一次「pull」。");
    return;
  }
  switch (stage) {
    case PullStage::fetchProbe:
      HandlePullFetchProbe(window, outcome);
      break;
    case PullStage::integrateProbe:
      HandlePullIntegrateProbe(window, outcome);
      break;
    case PullStage::recheckProbe:
      HandlePullRecheckProbe(window, outcome);
      break;
    default:
      AbandonPullAttempt(window, L"预检回来时这次 pull 已经不在等它了，没有执行任何命令。");
      break;
  }
}

void MainWindow::HandlePullFetchProbe(HWND window, const platform::PullProbeOutcome& outcome) {
  const git::PullFetchPlan plan =
      git::BuildPullFetchPlan(outcome.target, state_.Repo().detection.root);
  if (plan.state == git::PullFetchPlanState::blocked) {
    ::MessageBoxW(window, plan.explanation.c_str(), L"现在不能 pull", MB_OK | MB_ICONINFORMATION);
    AbandonPullAttempt(window,
                       L"未执行 pull：前提不成立（原因见刚才的说明框）。没有打开命令窗口，也没有接触"
                       L"任何远端或改动仓库。");
    return;
  }
  const int answer = ::MessageBoxW(window, plan.confirmationText.c_str(), L"pull 第一步：先获取，请确认",
                                   MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON2);
  if (answer != IDOK) {
    AbandonPullAttempt(window,
                       L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。整合这一步更没有被谈起。");
    return;
  }

  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = state_.Repo().detection.root;
  operation.arguments = plan.arguments;  // 目标只认远端名字，URL 由 Git 自己按配置解析。

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（pull 的第一步：获取；" +
                        plan.remoteName + L" → " + plan.trackingRef + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pullFetchOperation = true;
  pendingPull_.stage = PullStage::fetching;
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    state_.SetStatusNote(L"这次 pull 停在第一步：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                         L"远端没有被接触，仓库也没有改动。");
    pendingPull_ = PendingPull{};
    RefreshTexts(window);
  }
}

void MainWindow::OnPullFetchSettled(HWND window, bool fetchSucceeded) {
  if (pendingPull_.stage != PullStage::fetching) {
    // 通知迟到（期间换了仓库、或这一步已按别的路径结案）：绝不能凭这份旧状态继续往下整合。
    pendingPull_ = PendingPull{};
    return;
  }
  if (!fetchSucceeded) {
    pendingPull_ = PendingPull{};
    state_.SetStatusNote(L"pull 停在第一步：命令窗口里那次获取没有成功，因此没有做任何整合。"
                         L"远端跟踪引用有没有被这次抓取改动，以正在重读的现状为准；本程序不自动重试，"
                         L"也不会因此 prune、改用别的远端或改写任何远端配置。原因看命令窗口里 Git 的真实输出。");
    RefreshTexts(window);
    return;
  }

  pendingPull_.stage = PullStage::integrateProbe;
  pendingPull_.fetchAlreadyRan = true;
  platform::PullProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.absoluteGitDir = state_.Repo().detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullProbeTimeoutMs;
  request.includeRelationship = true;
  pullWorker_.Request(window, kPullProbeCompleted, std::move(request),
                      [](const platform::PullProbeRequest& pending) {
                        return platform::RunPullProbeLoad(pending);
                      });
  state_.SetStatusNote(L"获取已完成。正在后台重读仓库现状，并只读核对本地与远端的关系（各有几个独有提交、"
                       L"共同基准、这次会带进哪些文件、内容冲突预演），判回来后将问你要不要整合…");
  RefreshTexts(window);
}

void MainWindow::HandlePullIntegrateProbe(HWND window, const platform::PullProbeOutcome& outcome) {
  // 这一份事实是确认框与「执行前复核」的比对基准，必须先存下来再弹框。
  pendingPull_.integrateFacts = outcome;
  ComposeAndConfirmPullIntegrate(window, outcome, git::PullStrategyChoice::none);
}

void MainWindow::ComposeAndConfirmPullIntegrate(HWND window, const platform::PullProbeOutcome& outcome,
                                                git::PullStrategyChoice choice) {
  git::PullIntegratePlanInput input;
  input.target = outcome.target;
  input.relationship = outcome.relationship;
  input.repositoryRoot = state_.Repo().detection.root;
  input.choice = choice;
  const git::PullIntegratePlan plan = git::BuildPullIntegratePlan(input);

  // 取消/拒绝时几乎都要带同一句：那次获取已经动了远端跟踪引用，而且不该被退回去。
  const std::wstring fetchLeftover =
      pendingPull_.fetchAlreadyRan
          ? std::wstring(L"要说明的是：刚才那次获取（在命令窗口里跑的那条 fetch）已经把远端跟踪引用更新到"
                         L"远端的位置，这一处改动保留着——本程序不会把它退回去，也不需要你做什么。"
                         L"你取消的是接下来的整合：本地分支、索引与工作区都没有被动过。")
          : std::wstring(L"没有执行整合：本地分支、索引与工作区都没有被动过，也没有接触任何远端。");

  switch (plan.state) {
    case git::PullPlanState::blocked: {
      ::MessageBoxW(window, plan.explanation.c_str(), L"现在不能整合", MB_OK | MB_ICONINFORMATION);
      AbandonPullAttempt(window, L"未执行整合：" + fetchLeftover);
      return;
    }
    case git::PullPlanState::nothingToIntegrate: {
      ::MessageBoxW(window, plan.explanation.c_str(), L"pull：远端没有要整合的内容",
                    MB_OK | MB_ICONINFORMATION);
      AbandonPullAttempt(window, plan.explanation + L"\n" + fetchLeftover);
      return;
    }
    case git::PullPlanState::chooseStrategy: {
      // 分叉而配置没定策略：合并与变基两种做法摆出来，选完还会再有一次带风险清单的确认。
      platform::RemoteChoiceSpec spec;
      spec.title = L"选择这次 pull 的整合方式";
      spec.label = plan.explanation;
      for (const std::wstring& candidate : plan.strategyCandidates) {
        spec.items.push_back({candidate, L""});
      }
      spec.okText = L"按选中的方式整合";
      spec.emptyItemDetail.clear();  // 这里没有第二栏，别补一句与远端无关的占位。
      spec.needSelectionHint = L"先在列表里点选一种整合方式，再按确定。（取消不会执行任何命令）";
      spec.font = metrics_.Font();
      spec.layout.margin = metrics_.Margin();
      spec.layout.gap = metrics_.RowGap();
      spec.layout.width = metrics_.Scale(560);
      spec.layout.labelHeight = 4 * metrics_.LabelHeight();  // 「为什么要在这里选」那句留四行。
      spec.layout.listHeight = metrics_.Scale(110);
      spec.layout.buttonWidth = metrics_.ButtonWidth(L"按选中的方式整合");
      spec.layout.buttonHeight = metrics_.ControlHeight();
      const platform::RemoteChoiceResult picked = platform::PromptForRemoteChoice(window, spec);
      if (!picked.accepted || picked.selectedIndex < 0 ||
          static_cast<size_t>(picked.selectedIndex) >= plan.strategyCandidates.size()) {
        AbandonPullAttempt(window, L"已取消整合方式的选择：" + fetchLeftover);
        return;
      }
      ComposeAndConfirmPullIntegrate(
          window, outcome,
          picked.selectedIndex == 0 ? git::PullStrategyChoice::chooseMerge
                                    : git::PullStrategyChoice::chooseRebase);
      return;
    }
    case git::PullPlanState::ready:
      break;
  }

  const bool proceed =
      plan.requiresForce
          ? ShowPullRiskConfirm(window, plan.confirmationText)
          : ::MessageBoxW(window, plan.confirmationText.c_str(), L"pull 第二步：整合前请确认",
                          MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
  if (!proceed) {
    AbandonPullAttempt(window, L"已取消整合：" + fetchLeftover);
    return;
  }
  pendingPull_.plan = plan;
  RequestPullExecutionRecheck(window);
}

void MainWindow::RequestPullExecutionRecheck(HWND window) {
  pendingPull_.stage = PullStage::recheckProbe;
  platform::PullProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.absoluteGitDir = state_.Repo().detection.absoluteGitDir;
  request.timeoutMilliseconds = kPullProbeTimeoutMs;
  request.includeRelationship = false;
  pullWorker_.Request(window, kPullProbeCompleted, std::move(request),
                      [](const platform::PullProbeRequest& pending) {
                        return platform::RunPullProbeLoad(pending);
                      });
  state_.SetStatusNote(L"点头之后、执行之前，再把预检那套只读查询原样重发一遍核对现状"
                       L"（分支 / HEAD / 远端跟踪引用 / 工作区）：对得上才执行那条整合命令…");
  RefreshTexts(window);
}

void MainWindow::HandlePullRecheckProbe(HWND window, const platform::PullProbeOutcome& outcome) {
  const std::wstring change =
      git::DescribePullChange(pendingPull_.integrateFacts.target, outcome.target);
  if (!change.empty()) {
    ::MessageBoxW(window, change.c_str(), L"执行前复核：仓库又变了", MB_OK | MB_ICONWARNING);
    pendingPull_ = PendingPull{};
    state_.SetStatusNote(L"执行前复核发现现状与预检时不一致，因此没有发出整合命令。"
                         L"仓库状态正在重读，看清现状后如仍要 pull 请再点一次。");
    RefreshTexts(window);
    ScheduleRefresh(window);
    return;
  }
  LaunchPullIntegrate(window, pendingPull_.plan);
}

void MainWindow::LaunchPullIntegrate(HWND window, const git::PullIntegratePlan& plan) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = state_.Repo().detection.root;
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（pull 的第二步：整合；工作目录 " +
                        state_.Repo().detection.root + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pullIntegrateOperation = true;
  pendingPull_.stage = PullStage::integrating;
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    state_.SetStatusNote(L"这次整合没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。" +
                         std::wstring(pendingPull_.fetchAlreadyRan
                                          ? L"之前那次获取只更新了远端跟踪引用，本地没有被改动。"
                                          : L"本地没有任何改动。"));
    pendingPull_ = PendingPull{};
    RefreshTexts(window);
  }
}

std::wstring MainWindow::ReportPullIntegrateFailure(HWND window, git::CommandCompletion completion,
                                                   long exitCode) {
  const platform::PullAftermath aftermath = platform::CapturePullAftermath(
      state_.Git().path, state_.Repo().detection.root, state_.Repo().detection.absoluteGitDir,
      kPullRecheckTimeoutMs);

  std::wstring text;
  text += L"命令窗口里的那次整合没有完成（" + std::wstring(git::CommandCompletionLabel(completion));
  if (completion == git::CommandCompletion::finished) {
    // 只有 finished 携带 Git 真实给过的退出码；gitNotStarted 与未知形态没有数值可列，
    // 由状态标签本身说明情况。
    text += L"，Git 退出码 " + std::to_wstring(static_cast<long long>(exitCode));
  }
  text += L"）。\n\n";
  text += L"当前流程：" +
          (aftermath.workflow.HasSpecialFlowInProgress()
               ? aftermath.workflow.SpecialFlowText()
               : std::wstring(L"没有 Git 流程停在进行中（这次整合没有留下未完成的流程）")) +
          L"\n";
  if (!aftermath.conflict.readOk) {
    text += L"未合并的文件：没能读回来（" +
            (aftermath.conflict.readFailure.empty() ? std::wstring(L"原因未知")
                                                    : aftermath.conflict.readFailure) +
            L"）。以命令窗口里 Git 的真实输出为准。\n";
  } else if (aftermath.conflict.conflictPaths.empty()) {
    text += L"未合并的文件：无（索引里没有未合并条目，这次失败不是留下冲突的那种失败）。\n";
  } else {
    text += L"未合并的文件共 " + std::to_wstring(aftermath.conflict.conflictPaths.size()) + L" 个：";
    const size_t shown = std::min(aftermath.conflict.conflictPaths.size(), size_t{8});
    for (size_t index = 0; index < shown; ++index) {
      text += L"\n  · " + aftermath.conflict.conflictPaths[index];
    }
    if (aftermath.conflict.conflictPaths.size() > shown) {
      text += L"\n  · …（其余 " +
              std::to_wstring(aftermath.conflict.conflictPaths.size() - shown) + L" 个见“未暂存的更改”）";
    }
    text += L"\n这些文件也正列在“未暂存的更改”里（重读完成后带「冲突」状态）。\n";
  }
  if (!aftermath.conflict.branchRef.empty()) {
    text += L"当前分支：" + aftermath.conflict.branchRef + L"\n";
  }
  if (!aftermath.conflict.headObjectId.empty()) {
    text += L"HEAD 现在在：" + git::ShortObjectId(aftermath.conflict.headObjectId) + L"\n";
  }
  text += L"\n本程序不会替你收尾：不会 abort、不会 reset、不会 continue，也不会选任何一方的内容——"
          L"这些决定属于你，命令窗口里留着 Git 的完整输出。\n";
  text += L"你可以继续那个流程（git merge --continue / git rebase --continue），"
          L"或按你自己的判断中止（git merge --abort / git rebase --abort）。";

  ::MessageBoxW(window, text.c_str(), L"pull 整合没有完成", MB_OK | MB_ICONWARNING);

  std::wstring note = L"pull 整合未完成";
  if (aftermath.workflow.HasSpecialFlowInProgress()) {
    note += L"：仓库停在「" + aftermath.workflow.SpecialFlowText() + L"」";
  }
  if (!aftermath.conflict.conflictPaths.empty()) {
    note += L"，未合并 " + std::to_wstring(aftermath.conflict.conflictPaths.size()) + L" 个文件";
  }
  note += L"。本程序没有 abort/reset/continue，处理现场由你决定（详见刚才的说明框）。";
  return note;
}

// ---- push（本步骤）----

bool MainWindow::ShowPushRiskConfirm(HWND window, const std::wstring& preview) {
  // 与 pull 整合、强制撤回同一套规矩：按钮文字说清点的是哪两样。
  // 这里的「仍要推送」只越过本程序的风险提示，命令一个字都不加——
  // 尤其不会补上 --force / --force-with-lease / --no-verify，那种「为了让它成功而更激烈」的事本程序不做。
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = window;
  config.hInstance = ::GetModuleHandleW(nullptr);
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.pszWindowTitle = L"推送：风险确认";
  config.pszMainInstruction = L"这份预检带有需要你自己核对的风险";
  config.pszContent = preview.c_str();
  config.pszMainIcon = TD_WARNING_ICON;
  const TASKDIALOG_BUTTON buttons[] = {
      {IDYES, L"仍要按这份预检推送"},
      {IDNO, L"取消"},
  };
  config.pButtons = buttons;
  config.cButtons = ARRAYSIZE(buttons);
  config.nDefaultButton = IDNO;
  int button = IDNO;
  const HRESULT hr = ::TaskDialogIndirect(&config, &button, nullptr, nullptr);
  if (FAILED(hr)) {
    const int answer =
        ::MessageBoxW(window,
                      (preview + L"\n\n（风险确认框未能按样式打开：这里点“是”等同“仍要按这份预检推送”，"
                                L"点“否”取消。）")
                          .c_str(),
                      L"推送：风险确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return answer == IDYES;
  }
  return button == IDYES;
}

void MainWindow::AbandonPushAttempt(HWND window, std::wstring_view reason) {
  pendingPush_ = PendingPush{};
  state_.SetStatusNote(std::wstring(reason));
  RefreshTexts(window);
}

void MainWindow::RequestPush(HWND window) {
  if (!RequireWritePrerequisites(window, L"推送")) {
    return;
  }
  const auto refuse = [&](std::wstring_view message) {
    state_.SetStatusNote(std::wstring(message));
    RefreshTexts(window);
  };
  if (pendingPush_.stage != PushStage::none) {
    refuse(L"已经有一次推送在走流程（预检、复核或核实），请等它结束或先取消那一步。");
    return;
  }
  if (pendingPull_.stage != PullStage::none) {
    refuse(L"「pull」还在走它的预检、获取或整合，请先让那一步结束，再来推送。");
    return;
  }
  if (fetchProbing_) {
    refuse(L"「fetch」的目标预检还在跑，请先等它的界面出现，再来推送。");
    return;
  }
  if (pendingUndo_.probing) {
    refuse(L"「撤回最近提交」的预检还在跑，请先等它的确认框出现，再来推送。");
    return;
  }
  if (CommitAttemptActive()) {
    refuse(L"「创建提交」正在核对仓库现状（或正在做执行前复核），请先等那一步结束，再来推送。");
    return;
  }

  pendingPush_.stage = PushStage::probe;
  platform::PushProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.absoluteGitDir = state_.Repo().detection.absoluteGitDir;
  request.timeoutMilliseconds = kPushProbeTimeoutMs;
  pushWorker_.Request(window, kPushProbeCompleted, std::move(request),
                      [](const platform::PushProbeRequest& pending) {
                        return platform::RunPushProbeLoad(pending);
                      });
  state_.SetStatusNote(L"推送前先在后台只读问清：在哪个分支、要推哪一份提交、上游是谁、"
                       L"这次实际会推给哪个远端的哪个地址、本地相对上一次抓取领先几个"
                       L"（只读查询，不弹命令窗口、不接触任何远端），问回来后把源分支 / 目标远端 / "
                       L"目标分支一起摆给你确认…");
  RefreshTexts(window);
}

void MainWindow::OnPushProbeCompleted(HWND window, uint64_t completionSerial) {
  platform::PushProbeOutcome outcome;
  if (!pushWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 后台控制器层：期间又发起了更晚的预检，这份结果不再有意义。
  }
  const PushStage stage = pendingPush_.stage;
  if (stage != PushStage::probe && stage != PushStage::recheckProbe) {
    return;  // 这一次推送已经按「取消 / 结案」作废，迟到的结果原样丢掉。
  }
  if (!state_.RepoUsable() ||
      !git::PathsEqualFolded(outcome.repositoryDirectory, state_.Repo().detection.root)) {
    AbandonPushAttempt(window,
                       L"预检完成时仓库已经换掉，这次推送没有执行任何命令。请对现在的仓库重新点一次「推送」。");
    return;
  }
  if (stage == PushStage::probe) {
    HandlePushProbe(window, outcome);
  } else {
    HandlePushRecheckProbe(window, outcome);
  }
}

void MainWindow::HandlePushProbe(HWND window, const platform::PushProbeOutcome& outcome) {
  const git::PushPlan plan = git::BuildPushPlan(outcome.facts, state_.Repo().detection.root);
  if (plan.state == git::PushPlanState::blocked) {
    ::MessageBoxW(window, plan.explanation.c_str(), L"现在不能推送", MB_OK | MB_ICONINFORMATION);
    AbandonPushAttempt(window,
                       L"未执行推送：前提不成立（原因见刚才的说明框）。没有打开命令窗口，"
                       L"也没有接触任何远端、没有改动仓库。");
    return;
  }
  const bool proceed =
      plan.requiresForce
          ? ShowPushRiskConfirm(window, plan.confirmationText)
          : ::MessageBoxW(window, plan.confirmationText.c_str(), L"推送前请确认",
                          MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
  if (!proceed) {
    AbandonPushAttempt(window,
                       L"已取消：没有打开命令窗口，也没有接触任何远端或改动仓库。");
    return;
  }
  pendingPush_.preflight = outcome;  // 复核要拿它当「预检时的那份现状」。
  pendingPush_.plan = plan;
  RequestPushExecutionRecheck(window);
}

void MainWindow::RequestPushExecutionRecheck(HWND window) {
  pendingPush_.stage = PushStage::recheckProbe;
  platform::PushProbeRequest request;
  request.exePath = state_.Git().path;
  request.repositoryDirectory = state_.Repo().detection.root;
  request.absoluteGitDir = state_.Repo().detection.absoluteGitDir;
  request.timeoutMilliseconds = kPushProbeTimeoutMs;
  pushWorker_.Request(window, kPushProbeCompleted, std::move(request),
                      [](const platform::PushProbeRequest& pending) {
                        return platform::RunPushProbeLoad(pending);
                      });
  state_.SetStatusNote(L"点头之后、执行之前，再把预检那套只读查询原样重发一遍：分支 / 要推的那一份提交 / "
                       L"上游 / 发布目标都对得上，才发出那条 push…");
  RefreshTexts(window);
}

void MainWindow::HandlePushRecheckProbe(HWND window, const platform::PushProbeOutcome& outcome) {
  const std::wstring change = git::DescribePushChange(pendingPush_.preflight.facts, outcome.facts);
  if (!change.empty()) {
    ::MessageBoxW(window, change.c_str(), L"执行前复核：仓库又变了", MB_OK | MB_ICONWARNING);
    pendingPush_ = PendingPush{};
    state_.SetStatusNote(L"执行前复核发现现状与预检时不一致，因此没有发出推送命令。"
                         L"仓库状态正在重读，看清现状后如仍要推送请再点一次。");
    RefreshTexts(window);
    ScheduleRefresh(window);
    return;
  }
  LaunchPush(window, pendingPush_.plan);
}

void MainWindow::LaunchPush(HWND window, const git::PushPlan& plan) {
  git::CommandWindowOperation operation;
  operation.operationId = plan.operationId;
  operation.displayName = plan.displayName;
  operation.gitExecutable = state_.Git().path;
  operation.repositoryDirectory = state_.Repo().detection.root;
  // 参数按数组提交，不进任何 shell 字符串：命令源侧是复核确认过的完整提交 ID，目标远端只认名字，
  // URL 不进命令行（由 Git 自己按配置解析，界面展示与核实用的那份是 get-url --push --all 的展开
  // 回答），因此这条链路上都不会出现凭据。
  operation.arguments = plan.arguments;

  CommandLaunchOptions options;
  options.startedNote = L"已在命令窗口启动 " + plan.commandLabel + L"（推送 " +
                        git::ShortObjectId(plan.pushedObjectId) + L" → 远端「" + plan.remoteName +
                        L"」的 " + plan.remoteBranchRef + L"），等待 Git 退出码…";
  options.scopeNotice = plan.notice;
  options.pushOperation = true;
  pendingPush_.stage = PushStage::pushing;
  if (!LaunchCommandWindowOperation(window, operation, options)) {
    state_.SetStatusNote(L"这次推送没有启动：命令窗口未能打开，或启动失败（原因见上一行状态）。"
                         L"远端没有被接触，本地也没有任何改动。");
    pendingPush_ = PendingPush{};
    RefreshTexts(window);
  }
}

void MainWindow::RequestPushVerification(HWND window, bool commandSucceeded,
                                        std::wstring_view commandConclusion) {
  const git::PushPlan& plan = pendingPush_.plan;
  platform::PushVerifyRequest request;
  request.exePath = state_.Git().path;
  // 工作目录取预检时那个仓库的根，而不是界面此刻显示的根：点头到终态之间用户可能已经切了仓库，
  // 但这次核实问的始终是刚才那一条推送的去向。
  request.repositoryDirectory = pendingPush_.preflight.repositoryDirectory;
  request.remoteBranchRef = plan.remoteBranchRef;
  request.expectedObjectId = plan.pushedObjectId;
  request.pushUrls = plan.pushUrls;
  request.pushCommandSucceeded = commandSucceeded;
  request.commandConclusion = std::wstring(commandConclusion);
  request.timeoutMilliseconds = kPushVerifyTimeoutMs;
  pushVerifyWorker_.Request(window, kPushVerifyCompleted, std::move(request),
                           [](const platform::PushVerifyRequest& pending) {
                             return platform::RunPushVerifyLoad(pending);
                           });
  state_.SetStatusNote(L"正在向确认框上列出的那些**发布目标**发只读 ls-remote，核对那条引用到底停在"
                       L"哪一份提交（这一步要按发布目标的地址问，不是按本地那个远端跟踪引用；"
                       L"需要认证时由 Git 自己的方式处理，可能要等一会儿）…");
  RefreshTexts(window);
}

void MainWindow::OnPushVerifyCompleted(HWND window, uint64_t completionSerial) {
  platform::PushVerifyOutcome outcome;
  if (!pushVerifyWorker_.FetchLatest(completionSerial, &outcome)) {
    return;  // 更晚一次的核实已经取代了它（或这一次已经结案）。
  }
  if (pendingPush_.stage != PushStage::verifying) {
    return;
  }
  pendingPush_ = PendingPush{};

  const git::PushVerificationReport& report = outcome.report;
  std::wstring text = report.headline;
  for (const std::wstring& line : report.lines) {
    text += L"\n" + line;
  }
  state_.SetStatusNote(text);
  RefreshTexts(window);

  // 「Git 说成功了却没核实上」与「核实到的位置和推出去的那一份不是一个东西」必须当面讲清楚，
  // 状态栏那行会被后续刷新挤掉。全都对得上的场合不再多弹一次窗。
  const bool needsDialog =
      report.verdict == git::PushVerificationVerdict::mismatched ||
      (outcome.pushCommandSucceeded && report.verdict != git::PushVerificationVerdict::confirmed);
  if (needsDialog) {
    ::MessageBoxW(window, text.c_str(), L"推送结果与发布目标的实况",
                  MB_OK | (outcome.pushCommandSucceeded ? MB_ICONWARNING : MB_ICONINFORMATION));
  }
}

void MainWindow::AfterCommitSucceeded(HWND window, const git::CommitFormData& committedForm) {
  // 清空只清「屏幕上还是当时提交的那一份」的栏目：命令窗口跑的那段时间里，用户完全可能
  // 已经开始写下一段的草稿，把那份新内容一起抹掉比不清更糟。逐栏比对，一致才清。
  const git::CommitFormData current = commitForm_.Capture();
  const bool clearSubject = current.subject == committedForm.subject;
  const bool clearDescription = current.description == committedForm.description;
  const bool clearCoauthors = current.coauthors == committedForm.coauthors;
  {
    // 清空是程序做的事：不该被记成「用户把标题改成了空」，否则紧接着的默认值逻辑会乱套。
    const SuppressCommitFormNotify guard(suppressCommitFormNotify_);
    commitForm_.ClearCommittedFields(clearSubject, clearDescription, clearCoauthors);
  }
  formSession_.NoteCommitted(clearSubject, clearDescription, clearCoauthors);

  // 时间同样按「这期间有没有被人动过」决定：没人动过就回到此刻，作为下一次提交的默认；
  // 用户在那期间另选了时间的话那是新的意图，程序不能拿「提交成功」当理由把它抹掉。
  const bool resetTimes = !commitForm_.TimesUserEdited();
  if (resetTimes) {
    const SYSTEMTIME now = platform::CurrentLocalTime();
    commitForm_.SetTimes(now, now);
    RefreshTimeControlsState(window);
  }
  std::wstring kept;
  const auto appendKept = [&kept](bool cleared, std::wstring_view label) {
    if (cleared) {
      return;
    }
    if (!kept.empty()) {
      kept += L"、";
    }
    kept += label;
  };
  appendKept(clearSubject, L"标题");
  appendKept(clearDescription, L"描述");
  appendKept(clearCoauthors, L"合作者");
  std::wstring note = L"提交已创建：作者那一栏留着下次接着用";
  note += resetTimes ? L"，两个时间也回到此刻。" : L"，你在这期间改过的时间原样留着，没有重置。";
  if (kept.empty()) {
    note += L"标题、描述与合作者里这次提交用掉的内容已清空。";
  } else {
    note += L"其中" + kept + L"在这期间换了内容，因此原样留着、没有清空。";
  }
  note += L"仓库状态正在重读。";
  state_.SetFormNote(note);
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
      OnUndoProbeCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kFetchProbeCompleted:
      OnFetchProbeCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kPullProbeCompleted:
      OnPullProbeCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kPushProbeCompleted:
      OnPushProbeCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kPushVerifyCompleted:
      OnPushVerifyCompleted(window, static_cast<uint64_t>(wParam));
      return 0;
    case kCommitProbeCompleted:
      OnCommitProbeCompleted(window, static_cast<uint64_t>(wParam));
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
        ::DestroyWindow(window);
      }
      return 0;
    case WM_DESTROY:
      // 先停掉后台线程，再交还窗口所有权；旧线程不会再向已销毁窗口发通知。
      gitWorker_.Shutdown();
      repoWorker_.Shutdown();
      workspaceWorker_.Shutdown();
      authorWorker_.Shutdown();
      undoWorker_.Shutdown();
      fetchWorker_.Shutdown();
      pullWorker_.Shutdown();
      pushWorker_.Shutdown();
      pushVerifyWorker_.Shutdown();
      commitWorker_.Shutdown();
      StopOperationWatching();
      ::KillTimer(window, kGitVerifyTimer);
      ::KillTimer(window, kRepoDetectTimer);
      ::KillTimer(window, kGitOperationTimer);
      window_.Disown();
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, message, wParam, lParam);
  }
}

}  // namespace gc::ui
