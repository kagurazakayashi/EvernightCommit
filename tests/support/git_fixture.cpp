#include "support/git_fixture.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "git/repository.h"
#include "platform/windows/environment_block.h"
#include "platform/windows/git_toolchain.h"
#include "platform/windows/subprocess.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"
#include "support/tiny_test.h"

namespace gc::test {
namespace {

constexpr unsigned long kGitTimeoutMs = 20000;
constexpr unsigned long kVerifyGitTimeoutMs = 5000;
// 固定的提交时间基准（Unix 秒），每次提交 +60 秒：可复现，且不依赖本机时钟。
constexpr long long kCommitEpochBase = 1700000000LL;

std::string WideToUtf8(const std::wstring& text) { return platform::Utf16ToUtf8(text); }

std::wstring JoinFolded(const std::wstring& base, std::wstring_view relative) {
  std::wstring joined(base);
  if (!joined.empty() && joined.back() != L'\\' && joined.back() != L'/') {
    joined.push_back(L'\\');
  }
  joined.append(relative);
  std::wstring absolute = platform::ToAbsolutePath(joined);
  return absolute.empty() ? joined : absolute;
}

std::string LastWinErrorText() {
  const DWORD code = ::GetLastError();
  wchar_t buffer[256]{};
  const DWORD written = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr, code, 0, buffer, static_cast<DWORD>(std::size(buffer)),
                                         nullptr);
  std::wstring text;
  if (written != 0) {
    text.assign(buffer, written);
  }
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
    text.pop_back();
  }
  return "Windows 错误码 " + std::to_string(code) + (text.empty() ? "" : "：" + WideToUtf8(text));
}

std::wstring RandomHexSuffix() {
  static std::mt19937_64 engine{std::random_device{}()};
  std::uniform_int_distribution<unsigned> distribution(0u, 0xFFFFFFFFu);
  std::wstring suffix;
  for (int part = 0; part < 2; ++part) {
    wchar_t buffer[9]{};
    std::swprintf(buffer, std::size(buffer), L"%08x", distribution(engine));
    suffix += buffer;
  }
  return suffix;
}

// 递归删除：先清只读位（Git 对象库大量只读文件），失败短暂重试（杀软/索引器占用），
// 仍失败则记录并继续——析构不抛异常，但会明确报告残留路径。
bool DeleteTree(const std::wstring& directory, std::vector<std::string>& failures) {
  WIN32_FIND_DATAW find{};
  const std::wstring pattern = directory + L"\\*";
  const HANDLE search = ::FindFirstFileW(pattern.c_str(), &find);
  if (search == INVALID_HANDLE_VALUE) {
    // 目录已不存在视为删除完成；其他错误按失败记录。
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
  }
  bool allRemoved = true;
  do {
    const std::wstring name = find.cFileName;
    if (name == L"." || name == L"..") {
      continue;
    }
    const std::wstring full = directory + L"\\" + name;
    const bool isDirectory = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool removed = false;
    for (int attempt = 0; attempt < 4 && !removed; ++attempt) {
      if (attempt != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (isDirectory) {
        const bool childrenGone = DeleteTree(full, failures);
        ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        removed = childrenGone && (::RemoveDirectoryW(full.c_str()) != 0 ||
                                   GetLastError() == ERROR_FILE_NOT_FOUND);
      } else {
        ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
        removed = ::DeleteFileW(full.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
      }
    }
    if (!removed) {
      allRemoved = false;
      failures.push_back(WideToUtf8(full) + "（" + LastWinErrorText() + "）");
    }
  } while (::FindNextFileW(search, &find) != 0);
  ::FindClose(search);
  return allRemoved;
}

void RemoveEntireTree(const std::wstring& root) {
  std::vector<std::string> failures;
  const bool childrenGone = DeleteTree(root, failures);
  bool rootGone = false;
  for (int attempt = 0; attempt < 4 && !rootGone; ++attempt) {
    ::SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    rootGone = ::RemoveDirectoryW(root.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND ||
               GetLastError() == ERROR_PATH_NOT_FOUND;
    if (!rootGone) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (childrenGone && rootGone) {
    return;
  }
  std::fprintf(stderr, "[夹具] 临时目录未完全清理（请把列出的根目录整体删除）：\n");
  for (const std::string& failure : failures) {
    std::fprintf(stderr, "  %s\n", failure.c_str());
  }
  std::fprintf(stderr, "  根目录：%s\n", WideToUtf8(root).c_str());
}

bool ShouldKeepTempDirectories() {
  wchar_t value[8]{};
  const DWORD length = ::GetEnvironmentVariableW(L"GC_TEST_KEEP_TEMP", value, std::size(value));
  return length != 0 && length < std::size(value) && value[0] == L'1';
}

// 会被 Git 读取的环境变量前缀/名称：全部剔除后再注入夹具自己的取值，
// 保证用户 shell 里遗留的 GIT_DIR、LANGUAGE 等不影响测试。
bool IsBlockedEnvironmentName(const std::wstring& name) {
  if (_wcsnicmp(name.c_str(), L"GIT_", 4) == 0) {
    return true;
  }
  static constexpr const wchar_t* kBlocked[] = {L"HOME", L"USERPROFILE", L"LANGUAGE", L"LINGUAS",
                                                L"LC_ALL", L"LC_MESSAGES", L"SSH_ASKPASS"};
  for (const wchar_t* blocked : kBlocked) {
    if (_wcsicmp(name.c_str(), blocked) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<std::wstring> InheritedEnvironmentEntries() {
  std::vector<std::wstring> entries;
  const wchar_t* block = ::GetEnvironmentStringsW();
  if (block == nullptr) {
    return entries;
  }
  for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
    const std::wstring entry(cursor);
    const size_t equals = entry.find(L'=');
    if (equals == std::wstring::npos || equals == 0) {
      continue;  // 非法项与 "=C:" 一类每盘符隐藏项。
    }
    if (!IsBlockedEnvironmentName(entry.substr(0, equals))) {
      entries.push_back(entry);
    }
  }
  ::FreeEnvironmentStringsW(const_cast<LPWCH>(block));
  return entries;
}

void PutEnvironmentEntry(std::vector<std::wstring>& entries, const std::wstring& entry) {
  const size_t equals = entry.find(L'=');
  const std::wstring name = entry.substr(0, equals);
  std::erase_if(entries, [&name](const std::wstring& existing) {
    const size_t position = existing.find(L'=');
    return position != std::wstring::npos &&
           _wcsicmp(existing.substr(0, position).c_str(), name.c_str()) == 0;
  });
  entries.push_back(entry);
}

std::string TruncateForDiagnostics(std::string text) {
  constexpr size_t kLimit = 500;
  if (text.size() > kLimit) {
    text.resize(kLimit);
    text += "…（截断）";
  }
  return text;
}

std::string DescribeRun(const GitRun& run) {
  std::string text = "Git 命令失败：" + WideToUtf8(run.commandLine);
  if (!run.started) {
    text += "；进程未启动：" + WideToUtf8(run.err);
  } else if (run.timedOut) {
    text += "；超时被终止（" + std::to_string(kGitTimeoutMs) + " ms）";
  } else {
    text += "；退出码 " + std::to_string(run.exitCode) + "；stderr：" +
            TruncateForDiagnostics(WideToUtf8(run.err));
  }
  return text;
}

std::wstring FirstLineTrimmed(const std::wstring& text) {
  const size_t end = text.find_first_of(L"\r\n");
  return git::TrimWide(text.substr(0, end == std::wstring::npos ? text.size() : end));
}

}  // namespace

TempDirectory::~TempDirectory() {
  if (!owned_) {
    return;
  }
  if (ShouldKeepTempDirectories()) {
    std::fprintf(stderr, "[夹具] GC_TEST_KEEP_TEMP=1，保留临时目录：%s\n", WideToUtf8(path_).c_str());
    return;
  }
  RemoveEntireTree(path_);
}

bool TempDirectory::Create(std::wstring_view prefix, std::string& failureReason) {
  wchar_t tempPath[MAX_PATH]{};
  const DWORD length = ::GetTempPathW(std::size(tempPath), tempPath);
  if (length == 0 || length >= std::size(tempPath)) {
    failureReason = "无法取得系统临时目录（GetTempPathW：" + LastWinErrorText() + "）";
    return false;
  }
  const std::wstring base(tempPath, length);  // GetTempPathW 自带结尾反斜杠。
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::wstring candidate = base + std::wstring(prefix) + RandomHexSuffix();
    if (::CreateDirectoryW(candidate.c_str(), nullptr) != 0) {
      // 只有亲手创建成功的目录才认领所有权，绝不“生成名字后假定不存在就归我”。
      path_ = candidate;
      owned_ = true;
      return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      failureReason = "创建临时目录失败：" + LastWinErrorText();
      return false;
    }
  }
  failureReason = "临时目录名连续 64 次碰撞，拒绝继续（环境异常）";
  return false;
}

bool GitFixture::Prepare(std::string& failureReason) {
  if (!temp_.Create(L"EvernightCommit_git_", failureReason)) {
    return false;
  }
  isolatedHome_ = JoinFolded(temp_.Path(), L"git-home");
  emptyConfig_ = JoinFolded(isolatedHome_, L"empty.gitconfig");
  templateDir_ = JoinFolded(temp_.Path(), L"git-template");

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(isolatedHome_), ec);
  if (ec) {
    failureReason = "创建隔离 HOME 目录失败：" + WideToUtf8(isolatedHome_) + "（" + ec.message() + "）";
    return false;
  }
  std::filesystem::create_directories(std::filesystem::path(templateDir_), ec);
  if (ec) {
    failureReason = "创建空模板目录失败：" + WideToUtf8(templateDir_) + "（" + ec.message() + "）";
    return false;
  }
  {
    std::ofstream config(emptyConfig_, std::ios::binary | std::ios::trunc);
    if (!config.is_open()) {
      failureReason = "创建空全局配置文件失败：" + WideToUtf8(emptyConfig_);
      return false;
    }
  }

  baseEnvironment_ = InheritedEnvironmentEntries();
  PutEnvironmentEntry(baseEnvironment_, L"GIT_CONFIG_NOSYSTEM=1");
  PutEnvironmentEntry(baseEnvironment_, L"GIT_CONFIG_GLOBAL=" + emptyConfig_);
  PutEnvironmentEntry(baseEnvironment_, L"GIT_TEMPLATE_DIR=" + templateDir_);
  PutEnvironmentEntry(baseEnvironment_, L"HOME=" + isolatedHome_);
  PutEnvironmentEntry(baseEnvironment_, L"USERPROFILE=" + isolatedHome_);
  // 只允许本地 file 传输：即便误传网络 URL，Git 也会在发起任何连接前拒绝。
  PutEnvironmentEntry(baseEnvironment_, L"GIT_ALLOW_PROTOCOL=file");
  PutEnvironmentEntry(baseEnvironment_, L"GIT_TERMINAL_PROMPT=0");
  // 固定测试身份；提交时间由 Commit 逐次更新。
  PutEnvironmentEntry(baseEnvironment_, L"GIT_AUTHOR_NAME=Evernight Test");
  PutEnvironmentEntry(baseEnvironment_, L"GIT_COMMITTER_NAME=Evernight Test");
  PutEnvironmentEntry(baseEnvironment_, L"GIT_AUTHOR_EMAIL=test@example.invalid");
  PutEnvironmentEntry(baseEnvironment_, L"GIT_COMMITTER_EMAIL=test@example.invalid");
  PutEnvironmentEntry(baseEnvironment_, L"GIT_AUTHOR_DATE=" + std::to_wstring(kCommitEpochBase) + L" +0000");
  PutEnvironmentEntry(baseEnvironment_,
                      L"GIT_COMMITTER_DATE=" + std::to_wstring(kCommitEpochBase) + L" +0000");

  std::string lastAttempt;
  for (const std::wstring& candidate : platform::DiscoverGitCandidates()) {
    const platform::GitExeVerification verification =
        platform::VerifyGitExe(candidate, kVerifyGitTimeoutMs);
    if (verification.outcome == git::GitProbeOutcome::verified) {
      gitExe_ = candidate;
      return true;
    }
    if (lastAttempt.empty()) {
      lastAttempt = "候选 " + WideToUtf8(candidate) + "：" + WideToUtf8(verification.message);
    }
  }
  failureReason =
      "PATH 中没有可用的 git.exe，集成测试前置条件失败（" + lastAttempt + "）。测试需要本机安装 Git for Windows。";
  return false;
}

std::wstring GitFixture::PathInRoot(std::wstring_view relative) const {
  return ResolveOwnedDirectory(relative);
}

std::wstring GitFixture::ResolveOwnedDirectory(std::wstring_view path) const {
  const std::wstring absolute =
      platform::IsAbsolutePath(path) ? platform::ToAbsolutePath(path) : JoinFolded(temp_.Path(), path);
  if (absolute.empty() || !git::PathIsWithin(absolute, temp_.Path())) {
    throw PrerequisiteFailure("拒绝在夹具临时根之外操作目录：" + WideToUtf8(absolute) + "（根：" +
                              WideToUtf8(temp_.Path()) + "）");
  }
  return absolute;
}

std::wstring GitFixture::BuildEnvironmentBlock() const {
  std::wstring block;
  for (const std::wstring& entry : baseEnvironment_) {
    block += entry;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');  // 环境块以空项收尾。
  return block;
}

std::wstring GitFixture::BuildEnvironmentBlock(
    const std::vector<git::EnvironmentOverride>& overrides) const {
  if (overrides.empty()) {
    return BuildEnvironmentBlock();
  }
  // 与生产路径同一个合并函数：用例叠上去的 GIT_AUTHOR_* / GIT_COMMITTER_DATE
  // 究竟怎么进环境块，和界面点「创建提交」时走的完全是同一段代码。
  std::vector<std::wstring> merged;
  std::wstring reason;
  if (!platform::MergeEnvironmentEntries(baseEnvironment_, overrides, &merged, &reason)) {
    throw PrerequisiteFailure("合并用例的环境覆盖失败：" + WideToUtf8(reason));
  }
  return platform::MakeUnicodeEnvironmentBlock(merged);
}

GitRun GitFixture::RunWithBlock(const std::wstring& program,
                                const std::vector<std::wstring>& arguments,
                                const std::wstring& workingDirectory,
                                const std::wstring& environmentBlock) {
  const platform::SubprocessRunResult raw = platform::RunHiddenCaptured(
      program, arguments, workingDirectory, kGitTimeoutMs, environmentBlock.c_str());
  GitRun run;
  run.started = raw.started;
  run.exited = raw.exited;
  run.timedOut = raw.timedOut;
  run.exitCode = raw.exitCode;
  run.commandLine = raw.commandLine;
  run.out = platform::Utf8ToUtf16(raw.utf8Stdout);
  run.err = platform::Utf8ToUtf16(raw.utf8Stderr);
  if (!raw.started && run.err.empty()) {
    run.err = raw.launchErrorText;
  }
  return run;
}

GitRun GitFixture::RunWith(const std::wstring& program, const std::vector<std::wstring>& arguments,
                           const std::wstring& workingDirectory) {
  return RunWithBlock(program, arguments, workingDirectory, BuildEnvironmentBlock());
}

GitRun GitFixture::RunWithOverrides(const std::vector<std::wstring>& arguments,
                                    const std::vector<git::EnvironmentOverride>& overrides) {
  if (repoDir_.empty()) {
    throw PrerequisiteFailure("夹具尚未初始化仓库，RunWithOverrides 无可用工作目录");
  }
  return RunWithBlock(gitExe_, arguments, repoDir_, BuildEnvironmentBlock(overrides));
}

GitRun GitFixture::RunCheckedWith(const std::vector<std::wstring>& arguments,
                                  const std::wstring& workingDirectory) {
  GitRun run = RunWith(gitExe_, arguments, workingDirectory);
  if (!run.Success()) {
    throw PrerequisiteFailure(DescribeRun(run));
  }
  return run;
}

GitRun GitFixture::Run(const std::vector<std::wstring>& arguments, std::wstring_view workingDirectory) {
  const std::wstring directory =
      workingDirectory.empty() ? temp_.Path() : ResolveOwnedDirectory(workingDirectory);
  return RunWith(gitExe_, arguments, directory);
}

GitRun GitFixture::RunInRepo(const std::vector<std::wstring>& arguments) {
  if (repoDir_.empty()) {
    throw PrerequisiteFailure("夹具尚未初始化仓库，RunInRepo 无可用工作目录");
  }
  return RunWith(gitExe_, arguments, repoDir_);
}

GitRun GitFixture::RunChecked(const std::vector<std::wstring>& arguments,
                              std::wstring_view workingDirectory) {
  const std::wstring directory =
      workingDirectory.empty() ? temp_.Path() : ResolveOwnedDirectory(workingDirectory);
  return RunCheckedWith(arguments, directory);
}

GitRun GitFixture::RunCheckedInRepo(const std::vector<std::wstring>& arguments) {
  if (repoDir_.empty()) {
    throw PrerequisiteFailure("夹具尚未初始化仓库，RunCheckedInRepo 无可用工作目录");
  }
  return RunCheckedWith(arguments, repoDir_);
}

void GitFixture::InitRepositoryAt(const std::wstring& directory, bool bare) {
  // CreateProcessW 要求工作目录已存在，先建好再 init。
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(directory), ec);
  if (ec) {
    throw PrerequisiteFailure("创建仓库目录失败：" + WideToUtf8(directory) + "（" + ec.message() + "）");
  }
  std::vector<std::wstring> arguments{L"init", L"--quiet"};
  if (bare) {
    arguments.push_back(L"--bare");
  }
  // 显式写死 main，不依赖本机 init.defaultBranch 与用户语言环境。
  arguments.insert(arguments.begin() + 2, {L"-b", L"main"});
  GitRun run = RunWith(gitExe_, arguments, directory);
  if (!run.Success()) {
    // 极旧 Git 不认识 -b：回退为普通 init 后显式指回 refs/heads/main。
    std::vector<std::wstring> plain{L"init", L"--quiet"};
    if (bare) {
      plain.push_back(L"--bare");
    }
    static_cast<void>(RunCheckedWith(plain, directory));
    static_cast<void>(RunCheckedWith({L"symbolic-ref", L"HEAD", L"refs/heads/main"}, directory));
  }
  repoDir_ = directory;
}

void GitFixture::InitRepository(std::wstring_view directoryName) {
  InitRepositoryAt(ResolveOwnedDirectory(directoryName), /*bare=*/false);
}

void GitFixture::InitBareRepository(std::wstring_view directoryName) {
  InitRepositoryAt(ResolveOwnedDirectory(directoryName), /*bare=*/true);
}

std::wstring GitFixture::WriteFile(std::wstring_view repositoryRelativePath,
                                   const std::string& utf8Content) {
  if (repoDir_.empty()) {
    throw PrerequisiteFailure("夹具尚未初始化仓库，WriteFile 需要已初始化的工作区");
  }
  const std::wstring full = JoinFolded(repoDir_, repositoryRelativePath);
  std::error_code ec;
  const std::filesystem::path fsPath(full);
  std::filesystem::create_directories(fsPath.parent_path(), ec);
  if (ec) {
    throw PrerequisiteFailure("创建测试文件目录失败：" + WideToUtf8(full) + "（" + ec.message() + "）");
  }
  {
    std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      throw PrerequisiteFailure("无法写入测试文件：" + WideToUtf8(full));
    }
    file.write(utf8Content.data(), static_cast<std::streamsize>(utf8Content.size()));
    if (file.fail()) {
      throw PrerequisiteFailure("写入测试文件失败：" + WideToUtf8(full));
    }
  }
  return full;
}

void GitFixture::StageAll() { RunCheckedInRepo({L"add", L"-A"}); }

void GitFixture::Stage(const std::vector<std::wstring>& relativePaths) {
  std::vector<std::wstring> arguments{L"add", L"--"};
  arguments.insert(arguments.end(), relativePaths.begin(), relativePaths.end());
  RunCheckedInRepo(arguments);
}

void GitFixture::Commit(std::wstring_view subject) {
  ++commitSequence_;
  const std::wstring date =
      std::to_wstring(kCommitEpochBase + commitSequence_ * 60) + L" +0000";
  PutEnvironmentEntry(baseEnvironment_, L"GIT_AUTHOR_DATE=" + date);
  PutEnvironmentEntry(baseEnvironment_, L"GIT_COMMITTER_DATE=" + date);
  RunCheckedInRepo({L"commit", L"--quiet", L"--no-verify", L"-m", std::wstring(subject)});
}

std::wstring GitFixture::HeadSha() {
  const GitRun run = RunInRepo({L"rev-parse", L"--verify", L"--quiet", L"HEAD"});
  return run.Success() ? FirstLineTrimmed(run.out) : std::wstring{};
}

long GitFixture::CommitCount() {
  const GitRun run = RunInRepo({L"rev-list", L"--count", L"HEAD"});
  if (!run.Success()) {
    return -1;
  }
  return std::wcstol(FirstLineTrimmed(run.out).c_str(), nullptr, 10);
}

std::wstring GitFixture::ParentShaOfHead() {
  return FirstLineTrimmed(RunCheckedInRepo({L"rev-parse", L"HEAD^"}).out);
}

std::wstring GitFixture::HeadSubject() {
  return FirstLineTrimmed(RunCheckedInRepo({L"log", L"-1", L"--format=%s"}).out);
}

std::wstring GitFixture::HeadAuthorIdentity() {
  return FirstLineTrimmed(RunCheckedInRepo({L"log", L"-1", L"--format=%an <%ae>"}).out);
}

std::wstring GitFixture::HeadCommitObject() {
  // HEAD 不可解析（还没有任何提交）时返回空串：调用方按「没有提交」断言，而不是让前置失败炸掉。
  const GitRun run = RunInRepo({L"cat-file", L"commit", L"HEAD"});
  return run.Success() ? run.out : std::wstring();
}

std::vector<std::wstring> GitFixture::StatusPorcelain() {
  // core.quotepath=off：中文路径按原始 UTF-8 输出，解码后可直接与宽字符比较。
  const GitRun run =
      RunCheckedInRepo({L"-c", L"core.quotepath=off", L"status", L"--porcelain=v1"});
  std::vector<std::wstring> lines = git::SplitLines(run.out);
  std::erase_if(lines, [](const std::wstring& line) { return line.empty(); });
  return lines;
}

std::string GitFixture::ShowFileAtHead(std::wstring_view repositoryRelativePath) {
  const GitRun run = RunCheckedInRepo({L"show", L"--text", L"HEAD:" + std::wstring(repositoryRelativePath)});
  return WideToUtf8(run.out);
}

platform::RepoDetectDeps GitFixture::MakeDetectDeps() {
  platform::RepoDetectDeps deps;
  deps.runner = [this](const std::wstring& exePath, const std::wstring& directory,
                       const std::vector<std::wstring>& arguments) -> git::GitQueryResult {
    // 识别查询同样绑定显式工作目录并受临时根守卫；执行器带夹具隔离环境块。
    GitRun run = RunWith(exePath.empty() ? gitExe_ : exePath, arguments, ResolveOwnedDirectory(directory));
    git::GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = std::move(run.out);
    result.utf16Error = std::move(run.err);
    return result;
  };
  deps.absolutize = [](const std::wstring& directory, std::wstring_view relative) {
    return platform::ToAbsolutePathInDirectory(directory, relative);
  };
  deps.slashify = [](std::wstring_view path) { return platform::NormalizePathSeparators(path); };
  deps.directoryExists = [](const std::wstring& path) { return platform::IsExistingDirectory(path); };
  deps.gitExeIsFile = [](const std::wstring& path) { return platform::IsExistingRegularFile(path); };
  return deps;
}

platform::WorkspaceStatusDeps GitFixture::MakeStatusDeps() {
  platform::WorkspaceStatusDeps deps;
  deps.runner = [this](const std::wstring& exePath, const std::wstring& directory,
                       const std::vector<std::wstring>& arguments) -> git::GitQueryResult {
    // 走夹具的隔离执行器：工作目录仍受临时根守卫，环境块也不含用户配置。
    GitRun run = RunWith(exePath.empty() ? gitExe_ : exePath, arguments, ResolveOwnedDirectory(directory));
    git::GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = std::move(run.out);
    result.utf16Error = std::move(run.err);
    return result;
  };
  return deps;
}

platform::AuthorConfigDeps GitFixture::MakeAuthorConfigDepsForTest() {
  platform::AuthorConfigDeps deps;
  deps.runner = [this](const std::wstring& exePath, const std::wstring& directory,
                       const std::vector<std::wstring>& arguments) -> git::GitQueryResult {
    // 與工作區讀取同一把執行器：環境塊裡 GIT_CONFIG_GLOBAL 指向夾具檔案、NOSYSTEM 屏蔽系統設定，
    // 因此這裡讀到的「有效身份」只可能是本次臨時倉庫與臨時用户設定湊出來的。
    GitRun run = RunWith(exePath.empty() ? gitExe_ : exePath, arguments, ResolveOwnedDirectory(directory));
    git::GitQueryResult result;
    result.started = run.started;
    result.timedOut = run.timedOut;
    result.exited = run.exited;
    result.exitCode = static_cast<int>(run.exitCode);
    result.utf16Output = std::move(run.out);
    result.utf16Error = std::move(run.err);
    return result;
  };
  return deps;
}

void GitFixture::WriteUserConfig(const std::string& utf8Content) {
  std::ofstream file(std::filesystem::path(emptyConfig_), std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    throw PrerequisiteFailure("无法写入夹具的用户配置文件：" + WideToUtf8(emptyConfig_));
  }
  file.write(utf8Content.data(), static_cast<std::streamsize>(utf8Content.size()));
  if (file.fail()) {
    throw PrerequisiteFailure("写入夹具的用户配置文件失败：" + WideToUtf8(emptyConfig_));
  }
}

}  // namespace gc::test
