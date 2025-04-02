#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// 本模块只负责“问 Git 要答案并把答案分类”，不碰任何 Win32 API：
// 子进程执行由平台层以 GitQueryRunner 回调注入，因此分类逻辑可用桩输出完整测试。
// 查询结果在进入本模块前已由平台层完成 UTF-8→UTF-16 解码，本模块只处理宽字符，
// 因此中文分支名、中文路径不会被逐字节提升成乱码。
//
// 判定一律以 Git 自己的回答为准，不用“目录里有没有 .git”推断——
// worktree 与子模块的 .git 可能是文件，裸仓库根本没有 .git 子目录。

// 一次只读 Git 查询的结果。
struct GitQueryResult {
  bool started = false;      // 进程是否成功启动
  bool timedOut = false;     // 是否因超时被终止
  bool exited = false;       // 是否在期限内退出
  int exitCode = 0;          // 退出码（约定以 0 表示成功）
  std::wstring utf16Output;  // 标准输出解码后的文本：机器可读字段，按行取用
  std::wstring utf16Error;   // 标准错误解码后的文本：致命信息，只用于归类与诊断
};

// 以参数数组执行一次 Git 查询。exePath 为已验证的 git.exe，workingDirectory 为
// 该次查询绑定的仓库目录（调用方仍显式传 -C，不依赖也不改动进程全局工作目录）。
using GitQueryRunner =
    std::function<GitQueryResult(const std::wstring& exePath, const std::wstring& workingDirectory,
                                 const std::vector<std::wstring>& arguments)>;

// 仓库形态。detached 与 noCommits 是“分支维度”的叠加状态，其余互斥。
enum class RepoKind {
  unknown,
  plainWorktree,   // 普通工作区（主工作树）
  linkedWorktree,  // git worktree add 出来的链接工作树
  submodule,       // 直接打开的子模块仓库（同时仍是工作树）
  bare,            // 裸仓库：没有工作区，不能进入提交流程
  insideGitDir,    // 落在 .git 目录内部
  notRepository,   // Git 明确回答“不是仓库”
  detached,        // 游离 HEAD
  noCommits,       // 仓库存在但尚无任何提交
  failed,          // 无法判定（Git 安全检查、权限、执行失败等）
};

// 识别失败的具体原因，界面按此给出可操作的说明。
enum class RepoError {
  none,
  inputEmpty,         // 未填写仓库路径
  inputNotDirectory,  // 路径不存在或不是目录
  gitUnavailable,     // Git 程序路径无效或未通过验证
  notRepository,
  dubiousOwnership,   // Git 的 safe.directory 安全检查拦截
  accessDenied,       // 权限不足
  indexLocked,        // 索引等锁文件已存在：另一个 Git 进程正在改动该仓库
  gitTimeout,
  gitLaunchFailed,
  gitFailed,  // 其他非 0 退出
  badOutput,  // 输出无法按约定解析
};

// 形态查询分两组发问，字段按行序号取用。
inline constexpr size_t kShapeFlagCount = 3;
enum ShapeFlagField : size_t {
  kFlagBare = 0,           // --is-bare-repository
  kFlagInsideWorkTree = 1, // --is-inside-work-tree
  kFlagInsideGitDir = 2,   // --is-inside-git-dir
};
enum ShapePathField : size_t {
  kPathAbsoluteGitDir = 0,  // --absolute-git-dir
  kPathCommonDir = 1,       // --git-common-dir
  kPathTopLevel = 2,        // --show-toplevel（裸仓库与 .git 内部时不会输出）
};

// 仓库形态查询用到的两组字段。两组分开发问：
// 布尔组在裸仓库与 .git 内部也恒定逐行作答；路径组在这两种形态下只会先给出前两项
// 再以“must be run in a work tree”中止，因此顺序为“绝对 git 目录、公共 git 目录、工作区根”。
struct RepoShape {
  bool bare = false;
  bool insideWorkTree = false;
  bool insideGitDir = false;
  std::wstring absoluteGitDir;
  std::wstring commonDir;
  std::wstring topLevel;  // 裸仓库与 .git 内部时为空
};

// 解析布尔组输出（三行 true/false）。行数不足或取值不是布尔时返回 false。
[[nodiscard]] bool ParseShapeFlags(const std::vector<std::wstring>& lines, RepoShape* out);

// 解析路径组输出（按序号取用，缺少的字段留空）。至少要有前两项才算有效。
[[nodiscard]] bool ParseShapePaths(const std::vector<std::wstring>& lines, RepoShape* out);

struct RepoHead {
  bool onBranch = false;
  std::wstring branchName;  // onBranch 时的分支名（refs/heads/ 之后的部分）
  std::wstring shortSha;    // 游离时的短提交 ID
  bool hasCommits = false;
  bool unborn = false;  // HEAD 尚不可解析（仓库无任何提交）
};

struct RepoUpstream {
  bool present = false;
  std::wstring name;  // 形如 "origin/main"
};

// 一次仓库识别的完整结果。
struct RepoDetection {
  RepoKind kind = RepoKind::failed;
  RepoError error = RepoError::none;
  bool linked = false;     // 链接工作树（git worktree add 出来的一块工作区）
  bool submodule = false;  // 由父仓库登记的子模块
  std::wstring root;              // 实际工作区根目录；后续 Git 调用一律用它
  std::wstring absoluteGitDir;
  std::wstring superprojectTree;  // 子模块的父仓库工作区
  std::wstring branch;
  std::wstring shortSha;
  std::wstring upstream;      // 空表示未设置上游
  bool upstreamQueried = false;
  bool headResolved = false;  // HEAD 可解析（存在提交）
  std::wstring message;       // 面向界面的完整说明（成功摘要或失败原因）

  // 形态标签：仓库来历（链接工作树/子模块）与分支状态（游离/尚无提交）可能同时成立，
  // 用本函数组合出完整说明，避免后判定的分支状态覆盖掉目录来历。
  [[nodiscard]] std::wstring KindLabel() const;
};

// 解析形态查询输出；行数不足或关键标志取值异常时返回 false。
[[nodiscard]] bool ParseShapeOutput(const std::vector<std::wstring>& lines, RepoShape* out);

// 解析 HEAD 查询的两个结果：symbolicRef 为空表示当前不在分支上（游离或尚无提交）。
[[nodiscard]] RepoHead ParseHeadOutput(std::wstring_view symbolicRef, std::wstring_view shortSha);

// 解析 for-each-ref 的 "%(upstream:remotename)\t%(upstream:refname)" 输出行。
[[nodiscard]] RepoUpstream ParseUpstreamOutput(std::wstring_view line);

// 按顺序检查启动失败/超时/致命错误输出，命中即返回对应原因；退出码为 0 时返回 none。
// 判定顺序很重要：先看结构化标志（启动失败、超时），再按 Git 的固定错误文案关键词归类。
[[nodiscard]] RepoError ClassifyGitFailure(const GitQueryResult& result, std::wstring& detail);

// 折叠路径用于比较：统一斜杠方向、去掉结尾分隔符、ASCII 大小写不敏感。
[[nodiscard]] std::wstring CanonicalPathKey(std::wstring_view path);
[[nodiscard]] bool PathsEqualFolded(std::wstring_view left, std::wstring_view right);
// child 是否等于 parent 或以 parent 为祖先目录。
[[nodiscard]] bool PathIsWithin(std::wstring_view child, std::wstring_view parent);

// 按换行拆行（\r\n 与 \n 都接受，去掉行尾 \r；保留空行以保证按序号取字段）。
[[nodiscard]] std::vector<std::wstring> SplitLines(std::wstring_view text);

// 分类标签。
[[nodiscard]] std::wstring_view RepoKindLabel(RepoKind kind) noexcept;
[[nodiscard]] std::wstring_view RepoErrorLabel(RepoError error) noexcept;

// 失败说明：标签 + Git 给出的细节 + 一句可操作的提示。
[[nodiscard]] std::wstring BuildRepoErrorDetail(RepoError error, std::wstring_view detail);

// 成功说明，形如“已识别仓库：<形态标签>；工作区根目录：<root>”。
// inputDirectory 与 root 不同时追加“已从子目录上溯”的说明。
[[nodiscard]] std::wstring BuildRepoSummary(const RepoDetection& detection,
                                             std::wstring_view inputDirectory);

// 该形态下工作区可用（可继续后续工作区读取；裸仓库、.git 内部与非仓库都不行）。
[[nodiscard]] bool KindHasWorkspace(RepoKind kind) noexcept;

// 分支展示文本：无提交与游离 HEAD 有各自的说明，未设置上游单独展示。
[[nodiscard]] std::wstring FormatBranchDisplay(const RepoDetection& detection);
[[nodiscard]] std::wstring FormatUpstreamDisplay(const RepoDetection& detection);

[[nodiscard]] std::wstring TrimWide(std::wstring_view text);

}  // namespace gc::git
