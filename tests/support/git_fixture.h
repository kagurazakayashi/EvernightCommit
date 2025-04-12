#pragma once

#include <string>
#include <string_view>
#include <vector>

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
  unsigned long exitCode = 0;
  std::wstring commandLine;  // 实际命令行，供失败诊断展示
  std::wstring out;          // stdout
  std::wstring err;          // stderr
  [[nodiscard]] bool Success() const noexcept { return exited && !timedOut && exitCode == 0; }
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

  // 建库：git init（普通工作区或裸仓库）并显式固定默认分支 main；成功后 RepoDir() 指向新目录。
  void InitRepository(std::wstring_view directoryName = L"repo");
  void InitBareRepository(std::wstring_view directoryName = L"bare.git");

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

}  // namespace gc::test
