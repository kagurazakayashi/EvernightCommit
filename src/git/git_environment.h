#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"

namespace gc::git {

// Git 子进程的集中环境策略（纯逻辑，不触碰任何 Win32 API）。
//
// 要解决的问题：本程序可能从终端、IDE 或别的 Git 工具里被启动，父进程环境里残留的
// GIT_* 变量会被子进程原样继承。仅指定工作目录或 `-C <目录>` 不足以抵消它们：
// GIT_DIR / GIT_WORK_TREE 会让 Git 无视落点直接换仓库，GIT_INDEX_FILE 让读写落到
// 别人的索引上，GIT_CONFIG_* 能在不碰任何命令行参数的情况下改写语义，
// GIT_AUTHOR_* / GIT_COMMITTER_* 则让"界面显示的身份"和"提交真正记下的身份"不一致。
//
// 因此所有生产 Git 启动路径（后台只读查询与命令窗口执行）都必须经过这里构造的同一份
// 策略，按变量的类别决定保留、删除或受控覆盖：
//
//   1) 仓库定位类（GIT_DIR、GIT_WORK_TREE、GIT_NAMESPACE）—— 删除。
//      仓库由界面选中的目录加显式 `-C` 与工作目录双重绑定；留着它们就等于允许
//      外部环境把一次查询/操作搬到别的仓库上。
//   2) 索引与对象库类（GIT_INDEX_FILE、GIT_OBJECT_DIRECTORY、
//      GIT_ALTERNATE_OBJECT_DIRECTORIES）—— 删除。
//      删除后 Git 回到所选仓库自己的 .git/index 与对象库，也就是界面预期操作的那一份。
//   3) 配置注入与重定向类（GIT_CONFIG_PARAMETERS、GIT_CONFIG_COUNT、
//      GIT_CONFIG_KEY_<n>/GIT_CONFIG_VALUE_<n>、GIT_CONFIG_GLOBAL、GIT_CONFIG_SYSTEM、
//      废弃别名 GIT_CONFIG）—— 删除。
//      `-c` 式的进程内注入和"换一套配置文件"都会同时改变预检与执行的语义；
//      后台显示什么配置、执行时就用什么配置。数字后缀项在 GIT_CONFIG_COUNT 被删后
//      本已失效，但仍逐项删除：防的是操作自己注入 COUNT 时把残留项一并复活。
//   4) 身份与时间类（GIT_AUTHOR_NAME/EMAIL/DATE、GIT_COMMITTER_NAME/EMAIL/DATE）—— 删除。
//      界面显示的身份读自 Git 配置；提交时表单的覆盖由 CommitPlan 的 environmentOverrides
//      在本策略之后重新注入，只影响本次操作，不回写任何配置，也不污染后续进程。
//   5) 交互与凭据类 —— 按执行形态区分（见 GitRunPurpose）：
//      命令窗口：GIT_TERMINAL_PROMPT=1（凭据必须在用户看得见的窗口里问）、
//      GIT_PAGER=cat（分页器会扣住退出码）、删除 GIT_ASKPASS/SSH_ASKPASS
//      （它们会把提问搬成弹窗，违背"原生交互留在窗口里"）。
//      后台只读探测：GIT_TERMINAL_PROMPT=0（stdin 是空的，绝不能停在看不见的问题上）；
//      GIT_ASKPASS 与凭据助手保留——凭据交互沿用用户既有方式，只是不许挂在隐形输入上。
//   6) 普通系统与工具链变量（PATH、HOME、USERPROFILE、TMP/TEMP、SSH_AUTH_SOCK、
//      GIT_SSH、GIT_SSH_COMMAND、GIT_EXEC_PATH、GIT_CONFIG_NOSYSTEM、LANG/LC_* 等）——
//      一律保留。它们的值属于用户自己（代理、SSH 密钥代理、证书定位器、语言），
//      粗暴删光全部 GIT_* 会把正常认证与工具链一起拆掉。
//
// 类别 1～3、5 的命令窗口删除项属于"重定向"：本应该告诉用户"环境里有这些变量、
// 已被移除、操作仍绑定所选仓库"。告知只用变量名，绝不用值（值可能就是凭据）。
// 类别 4 的身份变量同样计入告知，因为"提交的作者变了"对用户同样是意外。

// 一次 Git 子进程执行的形态，决定交互类的处理方式与其余类别（其余两类完全一致）。
enum class GitRunPurpose {
  backgroundProbe,  // 隐藏窗口的只读后台查询（stdin 关闭、输出捕获）
  commandWindow,    // 用户可见的命令窗口执行（真正的写操作走这里）
};

// 环境策略的合并输入：一条有序覆盖序列，交给 platform::MergeEnvironmentEntries 按序应用。
// 顺序即优先级：策略删除 → 策略注入 → 操作自己的覆盖（表单身份/时间、特殊注入）。
// 后面的项覆盖前面的项，所以操作对 GIT_AUTHOR_NAME 的重写一定赢过策略的删除。
struct GitEnvironmentPlan {
  std::vector<EnvironmentOverride> overrides;
  // 「重定向」类别的变量名（大小写不敏感比较用）：调用方拿它对照继承环境，
  // 把"确实存在过并被移除"的那些名字告知用户。只可能是名字，绝不会带值。
  std::vector<std::wstring> redirectNames;
};

// 按执行形态合成完整覆盖序列。operationOverrides 原样排在最后，不改写、不检查内容
// （合法性由合并函数在边界统一校验）。
[[nodiscard]] GitEnvironmentPlan MakeGitEnvironmentPlan(GitRunPurpose purpose,
                                                        const std::vector<EnvironmentOverride>& operationOverrides);

// 名字是否属于"数字后缀配置注入项"：GIT_CONFIG_KEY_<数字> 或 GIT_CONFIG_VALUE_<数字>
// （大小写不敏感，数字不允许负号、不允许前导空白）。这类项的名字随注入条数变化，
// 无法预先枚举，由平台层对继承环境的每一项问这个问题。
[[nodiscard]] bool IsNumberedConfigInjectionName(std::wstring_view name);

// 在继承环境项（"NAME=VALUE" 形态）里找出策略重定向名单中实际存在过的变量名。
// 返回的是名单里的规范写法（去重、保持名单顺序），绝不返回继承项原文——原文含值。
[[nodiscard]] std::vector<std::wstring> FindInheritedRedirects(const std::vector<std::wstring>& baseEntries,
                                                               const std::vector<std::wstring>& redirectNames);

// 把 FindInheritedRedirects 的结果拼成给用户看的一句告知（只含变量名，超上限折叠为"等"）。
// 名单为空返回空串——没有移除过任何东西就不该唠叨。
[[nodiscard]] std::wstring BuildRedirectNoticeText(const std::vector<std::wstring>& foundNames);

}  // namespace gc::git
