#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"
#include "git/command_window.h"
#include "platform/windows/author_config.h"
#include "platform/windows/repo_detect.h"
#include "platform/windows/workspace_status.h"

namespace gc::test {

// RAII 临时目录：在系统临时目录下创建一个唯一命名的根目录。
// 唯一性靠 CreateDirectoryW 的原子失败（ERROR_ALREADY_EXISTS）重试保证，
// 不采用“生成时间戳后假定目录不存在”的写法；只有创建成功的实例才认领所有权，
// 析构时只对本人拥有的目录做递归删除。清理失败不抛异常，只在 stderr 报告残留路径。
// 设置环境变量 GC_TEST_KEEP_TEMP=1 可让析构保留现场（默认一律清理），
// 保留时会打印目录位置。
class TempDirectory {
public:
  TempDirectory() = default;
  ~TempDirectory();
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  // 创建 <系统临时目录>\<prefix><随机后缀>。失败时写入可展示的原因并返回 false。
  bool Create(std::wstring_view prefix, std::string& failureReason);

  [[nodiscard]] const std::wstring& Path() const noexcept { return path_; }
  [[nodiscard]] bool Owned() const noexcept { return owned_; }

private:
  std::wstring path_;
  bool owned_ = false;
};

// 一次 Git 子进程执行的结果：原始退出状态 + 已解码为 UTF-16 的两条输出流。
struct GitRun {
  bool started = false;
  bool exited = false;
  bool timedOut = false;
  bool outputComplete = true;  // 两条流都完整读回；读不全时 Success() 不成立
  unsigned long exitCode = 0;
  std::wstring commandLine;  // 实际命令行，供失败诊断展示
  std::wstring out;          // stdout
  std::wstring err;          // stderr
  [[nodiscard]] bool Success() const noexcept {
    return exited && !timedOut && exitCode == 0 && outputComplete;
  }
};

// 单个用例独占的临时 Git 仓库夹具：
//   - 独立临时根目录（TempDirectory），用例之间互不共享；
//   - git.exe 由 PATH 发现并按 --version 验证，找不到即前置条件失败（不静默跳过）；
//   - 子进程环境块与用户完全隔离：屏蔽 GIT_* / HOME / USERPROFILE / 语言变量，
//     用 GIT_CONFIG_GLOBAL 指向临时空文件、GIT_CONFIG_NOSYSTEM 屏蔽系统配置、
//     GIT_TEMPLATE_DIR 指向空模板目录，用户配置的 hooks、凭据助手、别名一律不可见；
//   - GIT_ALLOW_PROTOCOL=file 只放行本地 file 传输，任何网络 URL 在发起连接前即被 Git 拒绝；
//   - 提交身份固定为测试姓名/邮箱，提交时间取固定基准 + 序号，结果可复现；
//   - 默认分支显式写死 main，不依赖本机 init.defaultBranch。
// 所有命令显式绑定工作目录，且目录必须位于本次临时根之内：夹具永远不会在开发仓库
// 或用户真实仓库执行 init / add / commit。仅测试目标链接使用，不进生产流程。
class GitFixture {
public:
  GitFixture() = default;
  ~GitFixture() = default;
  GitFixture(const GitFixture&) = delete;
  GitFixture& operator=(const GitFixture&) = delete;

  // 创建临时根、装配隔离环境、发现并验证 Git。失败时写入 UTF-8 原因并返回 false，
  // 用例应据此 GC_REQUIRE 中止。
  bool Prepare(std::string& failureReason);

  [[nodiscard]] const std::wstring& GitExe() const noexcept { return gitExe_; }
  [[nodiscard]] const std::wstring& Root() const noexcept { return temp_.Path(); }
  // 最近一次 InitRepository 建立的工作区目录；未初始化时为空。
  [[nodiscard]] const std::wstring& RepoDir() const noexcept { return repoDir_; }

  // 相对临时根的安全路径拼接：折叠为绝对路径后必须仍在根内，否则抛 PrerequisiteFailure。
  std::wstring PathInRoot(std::wstring_view relative) const;

  // 执行一次 Git：program 固定为已验证的 git.exe；workingDirectory 为相对根的路径
  // （相对/绝对都会被折叠校验，越界即拒绝）。失败不抛，由调用方检查 GitRun。
  [[nodiscard]] GitRun Run(const std::vector<std::wstring>& arguments, std::wstring_view workingDirectory);
  [[nodiscard]] GitRun RunInRepo(const std::vector<std::wstring>& arguments);
  // 同上，但启动失败/超时/非 0 退出统一抛 PrerequisiteFailure（含命令行与 stderr 诊断）。
  // 返回值可丢弃（只关心“命令成功”时用 DiscardChecked 亦可），故不加 nodiscard。
  GitRun RunChecked(const std::vector<std::wstring>& arguments, std::wstring_view workingDirectory);
  GitRun RunCheckedInRepo(const std::vector<std::wstring>& arguments);

  // 在夹具的隔离环境之上再叠一层受控覆盖，然后执行一次 Git（工作目录固定在已初始化的仓库里）。
  // 这就是「创建提交」那条路径的真实形态：身份与时间走 GIT_AUTHOR_* / GIT_COMMITTER_DATE，
  // 由 platform::MergeEnvironmentEntries 合进同一个 Unicode 环境块，其余隔离规则一条都不放松。
  [[nodiscard]] GitRun RunWithOverrides(const std::vector<std::wstring>& arguments,
                                       const std::vector<git::EnvironmentOverride>& overrides);

  // 建库：git init（普通工作区或裸仓库）并显式固定默认分支 main；成功后 RepoDir() 指向新目录。
  void InitRepository(std::wstring_view directoryName = L"repo");
  void InitBareRepository(std::wstring_view directoryName = L"bare.git");

  // ---- 多工作区与本地远端（fetch/pull/push 步骤共用） ----
  // 切换当前活动工作区：相对临时根的名字或本夹具建出的绝对路径皆可（折叠后必须仍在根内）；
  // 之后的 RunInRepo / WriteFile / Commit / Push 都作用在它上面。
  void SetActiveRepository(std::wstring_view directory);
  // 从「根内本地路径」克隆出一个新工作区并成为活动工作区；来源同样要过 IsAllowedTestRemote 守卫。
  void CloneRepository(std::wstring_view sourceDirectory, std::wstring_view directoryName);
  // 给活动工作区添加远端；URL 未通过守卫一律抛 PrerequisiteFailure，绝不落到 Git 命令行上。
  void AddRemote(std::wstring_view name, std::wstring_view url);
  // 测试远端守卫（AddRemote 与 CloneRepository 内部执行；用例也可直接验证它的拒绝名单）：
  //   * 含 "://" 的一律拒绝——http/https/ssh/git/file 都不放行。file:// 也算：它同样能写成
  //     file://server/share 指向网络共享，所以不能把「file 协议」当作一定不联网；
  //   * 拒绝 UNC 形态（\\server\share 与 //server/share）；
  //   * 拒绝 scp 形态的 "user@host:path"（冒号不在盘符位）；
  //   * 其余必须是「盘符 + 绝对路径」，且折叠后仍位于本次临时根内（存在性不要求——
  //     「无效本地目标」的场景正需要指着根内一个不存在的目录）。
  [[nodiscard]] bool IsAllowedTestRemote(std::wstring_view url, std::string& reason) const;
  // git push [-u] <remote> <branch>（活动工作区）；失败抛 PrerequisiteFailure。
  void Push(std::wstring_view remote, std::wstring_view branch = L"main", bool setUpstream = false);
  // rev-parse --verify --quiet <revision>：可解析时返回完整 ID，否则返回空串。
  // 断言远端跟踪引用与本地分支各指向哪里就用它，不额外发明第二种读法。
  [[nodiscard]] std::wstring RevParseVerified(std::wstring_view revision);

  // 在工作区内写文件（覆盖式，UTF-8 原始字节，自动建父目录）。返回绝对路径，可丢弃。
  std::wstring WriteFile(std::wstring_view repositoryRelativePath, const std::string& utf8Content);
  void StageAll();
  void Stage(const std::vector<std::wstring>& relativePaths);
  // 固定身份与递增的确定时间提交；提交前不自动 add，暂存状态由用例显式控制。
  void Commit(std::wstring_view subject);

  // ---- 只读查询（验证真实 Git 对象，而不是桩返回值） ----
  [[nodiscard]] std::wstring HeadSha();       // HEAD 不可解析时返回空串
  [[nodiscard]] long CommitCount();           // 无提交时返回 -1
  [[nodiscard]] std::wstring ParentShaOfHead();
  [[nodiscard]] std::wstring HeadSubject();
  [[nodiscard]] std::wstring HeadAuthorIdentity();  // "姓名 <邮箱>"
  // git cat-file commit HEAD 的原始对象文本：author 与 committer 两行是
  // 「姓名 <邮箱> <秒数> <±hhmm>」，核对 Git 究竟记下了哪个瞬间与哪个偏移，这是最直接的证据。
  // HEAD 不可解析（还没有提交）时返回空串。
  [[nodiscard]] std::wstring HeadCommitObject();
  // git status --porcelain=v1 原始行（保留两列状态前缀；已开 core.quotepath=off）。
  [[nodiscard]] std::vector<std::wstring> StatusPorcelain();
  [[nodiscard]] std::string ShowFileAtHead(std::wstring_view repositoryRelativePath);

  // 用本夹具的隔离执行器装配仓库识别依赖，供集成测试直接驱动生产逻辑 DetectRepository。
  [[nodiscard]] platform::RepoDetectDeps MakeDetectDeps();
  // 同上，装配工作区状态读取（git status）的执行依赖。
  [[nodiscard]] platform::WorkspaceStatusDeps MakeStatusDeps();
  // 同上，装配「读有效 Git 身份」（git config --get）的执行依赖。
  // 隔离环境里 GIT_CONFIG_GLOBAL 指向夹具自己的档案、GIT_CONFIG_NOSYSTEM=1，
  // 因此「使用者層」就是這個臨時檔案，絕不會讀到本机真实的用户与系统配置。
  [[nodiscard]] platform::AuthorConfigDeps MakeAuthorConfigDepsForTest();

  // 覆寫夾具的「使用者層」設定檔內容（UTF-8 原始位元組）。
  void WriteUserConfig(const std::string& utf8Content);

private:
  // 把 baseEnvironment_ 拼成 NULL 结尾的 Unicode 环境块。
  [[nodiscard]] std::wstring BuildEnvironmentBlock() const;
  // 同上，但在隔离环境之上再叠一层覆盖（大小写不敏感替换，无值即删除）。
  [[nodiscard]] std::wstring BuildEnvironmentBlock(
      const std::vector<git::EnvironmentOverride>& overrides) const;
  // 用给定的环境块执行一次 Git：RunWith / RunWithOverrides 共用同一条执行与解码路径。
  [[nodiscard]] GitRun RunWithBlock(const std::wstring& program,
                                    const std::vector<std::wstring>& arguments,
                                    const std::wstring& workingDirectory,
                                    const std::wstring& environmentBlock);
  // 折叠为绝对路径并断言仍位于临时根内，越界抛 PrerequisiteFailure。
  [[nodiscard]] std::wstring ResolveOwnedDirectory(std::wstring_view path) const;
  // 核心执行：program 一般为 gitExe_，工作目录必须已解析为根内绝对路径。
  [[nodiscard]] GitRun RunWith(const std::wstring& program, const std::vector<std::wstring>& arguments,
                               const std::wstring& workingDirectory);
  [[nodiscard]] GitRun RunCheckedWith(const std::vector<std::wstring>& arguments,
                                      const std::wstring& workingDirectory);
  void InitRepositoryAt(const std::wstring& directory, bool bare);

  TempDirectory temp_;
  std::wstring gitExe_;
  std::wstring repoDir_;
  std::wstring isolatedHome_;    // <root>\git-home
  std::wstring emptyConfig_;     // <root>\git-home\empty.gitconfig（0 字节）
  std::wstring templateDir_;     // <root>\git-template（空目录）
  // 过滤后的继承项 + 隔离项 + 固定身份/日期项；Commit 就地更新日期两行。
  std::vector<std::wstring> baseEnvironment_;
  long commitSequence_ = 0;
};

// 三方本地远端夹具：一个 bare 远端 + 两个克隆自它的本地工作区（A 与 B）。
// 这就是 fetch/pull/push 各步骤共用的形态：B 提交并推送制造「远端更新」，
// A fetch 观察远端跟踪引用的变化；A/B 各自再提交一条就是「分叉」。
// 远端 URL 只允许临时根内的本地绝对路径（GitFixture::IsAllowedTestRemote 把关），
// 身份、hooks、协议限制沿用夹具同一套隔离环境，绝不接触网络。
class RemoteRig {
public:
  // 建 origin.git（bare）、A（init + 初始提交 + push -u origin main）、B（clone origin）。
  // 完成后活动工作区停在 A；失败写原因并返回 false。
  bool Prepare(std::string& failureReason);

  [[nodiscard]] GitFixture& fixture() noexcept { return fixture_; }
  [[nodiscard]] const std::wstring& OriginUrl() const noexcept { return originUrl_; }
  [[nodiscard]] const std::wstring& DirectoryA() const noexcept { return directoryA_; }
  [[nodiscard]] const std::wstring& DirectoryB() const noexcept { return directoryB_; }

  // 切换活动工作区（之后 fixture() 的 Commit/Push/Fetch 都落在选定的那一侧）。
  void UseA() { fixture_.SetActiveRepository(directoryA_); }
  void UseB() { fixture_.SetActiveRepository(directoryB_); }

private:
  GitFixture fixture_;
  std::wstring originUrl_;
  std::wstring directoryA_;
  std::wstring directoryB_;
};

}  // namespace gc::test
