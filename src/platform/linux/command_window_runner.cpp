#include "platform/linux/command_window_runner.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "git/shell_text.h"
#include "platform/linux/command_window_helper.h"
#include "platform/linux/operation_directory.h"
#include "platform/linux/scratch_files.h"
#include "platform/path.h"
#include "platform/terminal_adapters.h"
#include "platform/text_encoding.h"

namespace gc::platform {
namespace {

using Clock = std::chrono::steady_clock;

constexpr unsigned long kPollIntervalMs = 200;
constexpr unsigned long kNoTraceGraceMs = 15000;
constexpr unsigned long kProcessExitRetryWindowMs = 1500;
constexpr unsigned long kProcessExitRetryIntervalMs = 100;
constexpr std::uint64_t kStaleOperationAgeSeconds = 60ULL * 60ULL;
constexpr size_t kPreservedDirectoryLogLimit = 32;

std::wstring ErrnoText(int error) {
  return L"errno " + std::to_wstring(error) + L"（" + Utf8ToWide(std::strerror(error)) + L"）";
}

bool GenerateNonce(std::string* outHex) {
  static constexpr char kHex[] = "0123456789abcdef";
  try {
    std::random_device entropy;
    outHex->clear();
    outHex->reserve(32);
    for (int group = 0; group < 8; ++group) {
      const unsigned int value = entropy();
      for (int nibble = 7; nibble >= 0; --nibble) {
        outHex->push_back(kHex[(value >> (nibble * 4)) & 0xFu]);
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::wstring CurrentExecutablePath() {
  std::vector<char> buffer(4096);
  for (int attempt = 0; attempt < 4; ++attempt) {
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      return {};
    }
    if (static_cast<size_t>(length) < buffer.size()) {
      std::wstring wide;
      if (!TryWidePathFromBytes(std::string_view(buffer.data(), static_cast<size_t>(length)), wide)) {
        return {};
      }
      return wide;
    }
    buffer.resize(buffer.size() * 2);
  }
  return {};
}

bool OpenScratchRootForSweep(int* outFd, std::wstring* failureReason) {
  const ScratchDirectory scratch = ResolveScratchDirectory();
  if (!scratch.ok) {
    *failureReason = scratch.failureReason;
    return false;
  }
  std::string bytes;
  if (!TryPathBytesFromWide(scratch.path, bytes)) {
    *failureReason = L"临时目录路径无法无损编码成 UTF-8：" + scratch.path;
    return false;
  }
  while (bytes.size() > 1 && bytes.back() == '/') {
    bytes.pop_back();
  }
  const int fd = ::open(bytes.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    *failureReason = L"无法打开临时目录：" + ErrnoText(errno);
    return false;
  }
  *outFd = fd;
  return true;
}

struct WatchState {
  uint64_t id = 0;
  std::wstring requestOperationId;
  std::wstring displayName;
  std::wstring directory;
  std::wstring repositoryDirectory;
  std::wstring commandLine;
  std::wstring title;
  std::wstring statusText;
  std::string directoryName;
  std::string asciiNonce;
  int dirFd = -1;
  int leaseFd = -1;
  std::uint64_t launcherProcessId = 0;
  bool completionRecorded = false;
  bool threadFinished = false;
  CommandWindowResult result;

  ~WatchState() {
    if (leaseFd >= 0) {
      ::close(leaseFd);
    }
    if (dirFd >= 0) {
      ::close(dirFd);
    }
  }
};

}  // namespace

struct RunnerImpl {
  mutable std::mutex mutex;
  std::condition_variable stopCondition;
  std::vector<std::shared_ptr<WatchState>> records;
  std::vector<std::thread> watchers;
  std::vector<std::wstring> preservedDirectories;
  std::shared_ptr<MainThreadDispatcher> dispatcher;
  uint64_t sourceId = 0;
  bool stopping = false;
  bool hasTerminalOverride = false;
  TerminalSelection terminalOverride;

  std::shared_ptr<WatchState> Find(uint64_t id) const {
    for (const std::shared_ptr<WatchState>& state : records) {
      if (state->id == id) {
        return state;
      }
    }
    return nullptr;
  }

  void RecordPreserved(std::wstring entry) {
    const std::lock_guard<std::mutex> lock(mutex);
    preservedDirectories.push_back(std::move(entry));
    if (preservedDirectories.size() > kPreservedDirectoryLogLimit) {
      preservedDirectories.erase(preservedDirectories.begin());
    }
  }
};

namespace {

struct FileFacts {
  bool startMarkerSeen = false;
  bool startMarkerExists = false;
  OperationFileLockState startMarkerLock = OperationFileLockState::absent;
  bool resultParsed = false;
  long exitCode = 0;
  bool gitProcessLaunched = false;
};

FileFacts ReadFacts(int dirFd, std::string_view expectedNonce) {
  FileFacts facts;
  std::string bytes;
  bool exists = false;
  std::wstring ignored;
  if (ReadOperationFileAt(dirFd, git::kStartMarkerFileName, 64u * 1024u, &bytes, &exists, &ignored)) {
    facts.startMarkerExists = exists;
    if (exists) {
      facts.startMarkerSeen = git::ParseCommandWindowStartMarker(bytes, expectedNonce);
    }
  }
  facts.startMarkerLock =
      facts.startMarkerExists
          ? ProbeOperationFileLockAt(dirFd, git::kStartMarkerFileName)
          : OperationFileLockState::absent;

  bytes.clear();
  exists = false;
  if (ReadOperationFileAt(dirFd, git::kResultFileName, 64u * 1024u, &bytes, &exists, &ignored) &&
      exists) {
    facts.resultParsed = git::ParseCommandWindowResult(bytes, expectedNonce, &facts.exitCode,
                                                       &facts.gitProcessLaunched);
  }
  return facts;
}

bool HelperStillAlive(const FileFacts& facts) {
  return facts.startMarkerSeen && facts.startMarkerLock == OperationFileLockState::held;
}

bool HelperDefinitelyGone(const FileFacts& facts) {
  return facts.startMarkerSeen &&
         (facts.startMarkerLock == OperationFileLockState::free ||
          facts.startMarkerLock == OperationFileLockState::absent);
}

git::CommandWindowObservation MakeObservation(const FileFacts& facts) {
  git::CommandWindowObservation observation;
  observation.createProcessSucceeded = true;
  observation.processExited = HelperDefinitelyGone(facts);
  observation.startMarkerSeen = facts.startMarkerSeen;
  observation.resultParsed = facts.resultParsed;
  observation.exitCode = facts.exitCode;
  observation.gitProcessLaunched = facts.gitProcessLaunched;
  return observation;
}

void PostCompletion(RunnerImpl& impl, uint64_t operationId) {
  std::shared_ptr<MainThreadDispatcher> dispatcher;
  uint64_t sourceId = 0;
  {
    const std::lock_guard<std::mutex> lock(impl.mutex);
    dispatcher = impl.dispatcher;
    sourceId = impl.sourceId;
  }
  if (dispatcher && !dispatcher->Closed()) {
    static_cast<void>(dispatcher->Post(MainThreadDispatcher::Notice{sourceId, operationId}));
  }
}

void Watch(RunnerImpl& impl, const std::shared_ptr<WatchState>& state) {
  CommandWindowResult result = state->result;
  git::CommandCompletion completion = git::CommandCompletion::launched;
  long exitCode = 0;
  bool settled = false;
  const auto startedAt = Clock::now();

  while (!settled) {
    {
      std::unique_lock<std::mutex> lock(impl.mutex);
      if (!impl.stopping) {
        impl.stopCondition.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs),
                                    [&impl] { return impl.stopping; });
      }
      if (impl.stopping) {
        completion = git::CommandCompletion::stillUnknown;
        result.failureReason =
            L"应用退出时操作仍在进行，结果未知（终端与 Git 未被终止）。";
        break;
      }
    }

    FileFacts facts = ReadFacts(state->dirFd, state->asciiNonce);
    exitCode = 0;
    completion = git::DecideCommandCompletion(MakeObservation(facts), &exitCode);
    const bool helperAlive = HelperStillAlive(facts);

    // 辅助进程刚发布结果就释放开始标记的锁；给它一个短宽限，避免“结果已落盘、
    // 锁恰好先被观察到释放”被误判成窗口提前关闭。
    if (completion == git::CommandCompletion::terminated) {
      const auto deadline =
          Clock::now() + std::chrono::milliseconds(kProcessExitRetryWindowMs);
      while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kProcessExitRetryIntervalMs));
        FileFacts retry = ReadFacts(state->dirFd, state->asciiNonce);
        long retryExitCode = 0;
        const git::CommandCompletion retryCompletion =
            git::DecideCommandCompletion(MakeObservation(retry), &retryExitCode);
        if (retryCompletion == git::CommandCompletion::finished ||
            retryCompletion == git::CommandCompletion::gitNotStarted) {
          facts = retry;
          completion = retryCompletion;
          exitCode = retryExitCode;
          break;
        }
      }
    }

    if (git::IsCommandCompletionTerminal(completion)) {
      if (completion == git::CommandCompletion::finished) {
        result.exitCode = exitCode;
      } else {
        result.exitCode = 0;
      }
      if (completion == git::CommandCompletion::gitNotStarted) {
        result.failureReason =
            L"Git 进程未能创建（辅助进程直接报告：Git 程序在执行期间被移动/删除，或系统拒绝创建进程）。";
      } else if (completion == git::CommandCompletion::terminated) {
        result.failureReason =
            L"终端在拿到 Git 退出码之前关闭，或辅助进程被终止；Git 可能跑到一半，结果未知。";
      } else if (completion == git::CommandCompletion::stillUnknown) {
        result.failureReason =
            L"超过观察期限仍没有执行痕迹（终端可能没有真正打开，或辅助程序没有启动），结果未知。";
      }
      settled = true;
      break;
    }

    {
      const std::lock_guard<std::mutex> lock(impl.mutex);
      if (completion == git::CommandCompletion::running) {
        state->statusText = L"执行中（终端里可交互）";
      } else if (helperAlive) {
        state->statusText = L"终端已启动，辅助进程正在准备 Git";
      } else {
        state->statusText = L"已启动终端，等待辅助进程就绪";
      }
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt).count();
    if (completion == git::CommandCompletion::launched &&
        elapsedMs > static_cast<long long>(kNoTraceGraceMs)) {
      // 没有开始标记就无法证明窗口里发生过什么；不谎报“已启动”，也不无限等下去。
      completion = git::CommandCompletion::stillUnknown;
      result.failureReason =
          L"超过观察期限仍没有执行痕迹（终端可能没有真正打开，或辅助程序没有启动），结果未知。";
      settled = true;
      break;
    }
  }

  result.completion = completion;
  {
    const std::lock_guard<std::mutex> lock(impl.mutex);
    state->result = result;
    state->completionRecorded = true;
    state->statusText = std::wstring(git::CommandCompletionLabel(completion));
    if (completion == git::CommandCompletion::finished) {
      state->statusText += L"，Git 退出码 " + std::to_wstring(result.exitCode);
    }
  }

  // 先释放租约，再尝试回收。终端里的辅助进程若还握着开始标记，回收探测会拒绝删除，
  // 目录留给下一次启动的陈旧清扫；这里绝不强删它还要用的说明书或结果。
  if (state->leaseFd >= 0) {
    ::close(state->leaseFd);
    state->leaseFd = -1;
  }
  if (state->dirFd >= 0) {
    ::close(state->dirFd);
    state->dirFd = -1;
  }
  std::wstring preservedReason;
  if (!ReclaimOperationDirectoryByPath(state->directory, /*requireSpecConsumed=*/true,
                                      &preservedReason) &&
      !preservedReason.empty()) {
    impl.RecordPreserved(state->directory + L" —— " + preservedReason);
  }

  PostCompletion(impl, state->id);
  {
    const std::lock_guard<std::mutex> lock(impl.mutex);
    state->threadFinished = true;
  }
}

}  // namespace

LinuxCommandWindowRunner::LinuxCommandWindowRunner(
    std::shared_ptr<MainThreadDispatcher> dispatcher, uint64_t sourceId)
    : impl_(new RunnerImpl()) {
  impl_->dispatcher = std::move(dispatcher);
  impl_->sourceId = sourceId;
}

LinuxCommandWindowRunner::~LinuxCommandWindowRunner() {
  Shutdown();
  delete impl_;
  impl_ = nullptr;
}

void LinuxCommandWindowRunner::SetCompletionDispatcher(
    std::shared_ptr<MainThreadDispatcher> dispatcher, uint64_t sourceId) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->dispatcher = std::move(dispatcher);
  impl_->sourceId = sourceId;
}

void LinuxCommandWindowRunner::SetTerminalSelectionForTest(TerminalSelection selection) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->hasTerminalOverride = true;
  impl_->terminalOverride = std::move(selection);
}

void LinuxCommandWindowRunner::SweepStaleOperationDirectories() {
  int rootFd = -1;
  std::wstring failure;
  if (!OpenScratchRootForSweep(&rootFd, &failure)) {
    return;
  }
  struct RootGuard {
    int fd;
    ~RootGuard() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  } rootGuard{rootFd};

  const int scanFd = ::dup(rootFd);
  if (scanFd < 0) {
    return;
  }
  DIR* dir = ::fdopendir(scanFd);
  if (dir == nullptr) {
    ::close(scanFd);
    return;
  }
  struct DirGuard {
    DIR* dir;
    ~DirGuard() { ::closedir(dir); }
  } dirGuard{dir};

  while (dirent* entry = ::readdir(dir)) {
    const std::string name(entry->d_name);
    std::wstring nameWide;
    if (!TryUtf8ToWideStrict(name, nameWide) ||
        !git::IsSafeOperationDirectoryName(nameWide)) {
      continue;  // 名字不是本程序生成的形态：连看都不看里面有什么。
    }
    if (!IsOperationDirectoryOlderThan(rootFd, name, kStaleOperationAgeSeconds)) {
      continue;
    }
    std::wstring preserved;
    if (!TryReclaimOperationDirectory(rootFd, name, /*requireSpecConsumed=*/false, &preserved) &&
        !preserved.empty()) {
      impl_->RecordPreserved(Utf8ToWide(name) + L" —— " + preserved);
    }
  }
}

void LinuxCommandWindowRunner::Shutdown() {
  std::vector<std::thread> watchers;
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->stopping) {
      return;
    }
    impl_->stopping = true;
    impl_->dispatcher.reset();
    watchers.swap(impl_->watchers);
  }
  impl_->stopCondition.notify_all();
  // join 的是观察线程，不是终端或 Git —— 关闭 GUI 不会打断正在跑的操作。
  for (std::thread& watcher : watchers) {
    if (watcher.joinable()) {
      watcher.join();
    }
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const std::shared_ptr<WatchState>& state : impl_->records) {
    if (!state->completionRecorded) {
      state->result.operationId = state->id;
      state->result.requestOperationId = state->requestOperationId;
      state->result.displayName = state->displayName;
      state->result.commandLine = state->commandLine;
      state->result.repositoryDirectory = state->repositoryDirectory;
      state->result.directory = state->directory;
      state->result.completion = git::CommandCompletion::stillUnknown;
      state->result.failureReason =
          L"应用退出时操作仍在进行，结果未知（终端与 Git 未被终止）。";
      state->completionRecorded = true;
      state->threadFinished = true;
      state->statusText =
          std::wstring(git::CommandCompletionLabel(git::CommandCompletion::stillUnknown));
      // 观察线程已经 join：这里只释放本进程的租约与目录句柄，不删除说明书——
      // 终端里的辅助进程可能仍需要它继续跑；开始标记的 flock 仍能阻止陈旧清扫误删。
      if (state->leaseFd >= 0) {
        ::close(state->leaseFd);
        state->leaseFd = -1;
      }
      if (state->dirFd >= 0) {
        ::close(state->dirFd);
        state->dirFd = -1;
      }
    }
  }
}

bool LinuxCommandWindowRunner::Start(const git::CommandWindowOperation& operation,
                                     const TerminalPreferences& preferences,
                                     uint64_t* outOperationId,
                                     CommandWindowResult* failure) {
  if (outOperationId != nullptr) {
    *outOperationId = 0;
  }
  CommandWindowResult localFailure;
  localFailure.displayName = operation.displayName;
  localFailure.requestOperationId = operation.operationId;
  localFailure.commandLine = operation.gitExecutable;
  localFailure.repositoryDirectory = operation.repositoryDirectory;
  const auto reportFailure = [&](git::CommandCompletion completion, std::wstring reason,
                                 std::wstring guidance = {}) {
    localFailure.completion = completion;
    localFailure.failureReason = std::move(reason);
    if (!guidance.empty()) {
      localFailure.failureReason += L" " + guidance;
    }
    if (failure != nullptr) {
      *failure = localFailure;
    }
    return false;
  };

  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping) {
      return reportFailure(git::CommandCompletion::launchFailed,
                           L"执行器已停止，未启动终端。");
    }
  }

  std::wstring displayLine;
  git::CommandPlanReject reject = git::CommandPlanReject::none;
  std::wstring detail;
  if (!git::BuildGitCommandLineForDialect(operation, git::CommandLineDialect::posixArgv,
                                          &displayLine, &reject, &detail)) {
    return reportFailure(git::CommandCompletion::launchFailed, detail);
  }
  localFailure.commandLine = displayLine;

  if (!IsExecutableRegularFile(operation.gitExecutable)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"Git 程序不存在或不可执行：" + operation.gitExecutable);
  }
  if (!IsExistingDirectory(operation.repositoryDirectory)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"仓库目录不存在或不是目录：" + operation.repositoryDirectory);
  }

  TerminalSelection selection;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->hasTerminalOverride && impl_->terminalOverride.usable) {
      selection = impl_->terminalOverride;
    }
  }
  if (!selection.usable) {
    selection = DiscoverAndSelectTerminal(preferences);
  }
  if (!selection.usable) {
    return reportFailure(git::CommandCompletion::launchFailed, selection.failureReason,
                         selection.guidance);
  }

  static std::atomic<uint64_t> sequence{0};
  const uint64_t id = sequence.fetch_add(1) + 1;
  localFailure.operationId = id;

  ClaimedOperationDirectory directory = ClaimOperationDirectoryLinux();
  if (!directory.ok) {
    return reportFailure(git::CommandCompletion::launchFailed, directory.failureReason);
  }
  localFailure.directory = directory.path;
  bool handedOff = false;
  struct ClaimGuard {
    ClaimedOperationDirectory* directory;
    bool* handedOff;
    ~ClaimGuard() {
      if (*handedOff) {
        return;
      }
      CloseOperationDirectory(directory);
      std::wstring preserved;
      static_cast<void>(ReclaimOperationDirectoryByPath(directory->path, false, &preserved));
    }
  } claimGuard{&directory, &handedOff};

  std::string asciiNonce;
  if (!GenerateNonce(&asciiNonce) || !git::IsSafeNonce(Utf8ToWide(asciiNonce))) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"无法生成本次操作的随机口令，辅助进程无法与它绑定。");
  }
  const std::wstring nonce = Utf8ToWide(asciiNonce);
  const std::wstring title =
      git::MakeSafeConsoleTitle(L"EvernightCommit - 终端", operation.displayName,
                                Utf8ToWide(directory.name));

  std::wstring specText;
  reject = git::CommandPlanReject::none;
  detail.clear();
  if (!git::BuildCommandWindowSpecText(operation, Utf8ToWide(directory.name), title, nonce,
                                       &specText, &reject, &detail,
                                       git::CommandLineDialect::posixArgv)) {
    return reportFailure(git::CommandCompletion::launchFailed, detail);
  }
  std::string specBytes;
  if (!TryWideToUtf8Strict(specText, specBytes)) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"操作内容含无法用 UTF-8 表达的码元，已拒绝执行。");
  }
  std::wstring writeFailure;
  if (!WriteExclusiveFileAt(directory.dirFd, git::kSpecFileName, specBytes, &writeFailure)) {
    return reportFailure(git::CommandCompletion::launchFailed, writeFailure);
  }

  const std::wstring helperPath = CurrentExecutablePath();
  if (helperPath.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"无法取得本程序自身的路径（/proc/self/exe），不能启动终端辅助入口。");
  }
  // 显式把认领时用的临时根交给 helper：终端服务若复用已有进程，helper 继承到的
  // TMPDIR/XDG_RUNTIME_DIR 可能与本进程不同；根路径是启动参数里的数据，不是 shell 代码。
  const TerminalLaunchPlan plan = BuildTerminalLaunchPlan(
      selection.selected, title, helperPath,
      {Utf8ToWide(kTerminalHelperSwitch), directory.path, nonce, directory.rootPath});
  if (!plan.failureReason.empty()) {
    return reportFailure(git::CommandCompletion::launchFailed, plan.failureReason);
  }

  const TerminalLaunchResult launched = LaunchTerminal(plan);
  if (!launched.started) {
    return reportFailure(git::CommandCompletion::launchFailed, launched.failureReason,
                         launched.guidance);
  }

  auto state = std::make_shared<WatchState>();
  state->id = id;
  state->requestOperationId = operation.operationId;
  state->displayName = operation.displayName;
  state->directory = directory.path;
  state->repositoryDirectory = operation.repositoryDirectory;
  state->commandLine = displayLine;
  state->title = title;
  state->statusText = L"已启动终端，等待辅助进程就绪";
  state->directoryName = directory.name;
  state->asciiNonce = asciiNonce;
  state->dirFd = directory.dirFd;
  state->leaseFd = directory.leaseFd;
  state->launcherProcessId = launched.launcherProcessId;
  state->result.operationId = id;
  state->result.requestOperationId = operation.operationId;
  state->result.displayName = operation.displayName;
  state->result.commandLine = displayLine;
  state->result.repositoryDirectory = operation.repositoryDirectory;
  state->result.directory = directory.path;
  directory.dirFd = -1;
  directory.leaseFd = -1;
  handedOff = true;

  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->records.push_back(state);
  }
  std::thread watcher;
  try {
    watcher = std::thread([this, state] { Watch(*impl_, state); });
  } catch (const std::system_error&) {
    {
      const std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->records.erase(std::find(impl_->records.begin(), impl_->records.end(), state));
    }
    if (state->leaseFd >= 0) {
      ::close(state->leaseFd);
      state->leaseFd = -1;
    }
    if (state->dirFd >= 0) {
      ::close(state->dirFd);
      state->dirFd = -1;
    }
    std::wstring preserved;
    static_cast<void>(ReclaimOperationDirectoryByPath(state->directory, true, &preserved));
    return reportFailure(git::CommandCompletion::launchFailed,
                         L"无法创建结果观察线程；终端已打开，但本程序不会得知 Git 的退出码。");
  }
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->watchers.push_back(std::move(watcher));
  }
  if (outOperationId != nullptr) {
    *outOperationId = id;
  }
  return true;
}

std::vector<std::wstring> LinuxCommandWindowRunner::ActiveOperationNames() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::wstring> names;
  for (const std::shared_ptr<WatchState>& state : impl_->records) {
    if (!state->completionRecorded) {
      names.push_back(state->displayName.empty() ? state->requestOperationId : state->displayName);
    }
  }
  return names;
}

size_t LinuxCommandWindowRunner::ActiveCount() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  size_t count = 0;
  for (const std::shared_ptr<WatchState>& state : impl_->records) {
    if (!state->completionRecorded) {
      ++count;
    }
  }
  return count;
}

bool LinuxCommandWindowRunner::TakeResult(uint64_t operationId, CommandWindowResult* out) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::shared_ptr<WatchState> state = impl_->Find(operationId);
  if (state == nullptr || !state->completionRecorded) {
    return false;
  }
  if (out != nullptr) {
    *out = state->result;
  }
  return true;
}

bool LinuxCommandWindowRunner::DescribeOperation(uint64_t operationId, std::wstring* status,
                                                 CommandWindowResult* result) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::shared_ptr<WatchState> state = impl_->Find(operationId);
  if (state == nullptr) {
    return false;
  }
  if (status != nullptr) {
    *status = state->statusText;
  }
  if (result != nullptr && state->completionRecorded) {
    *result = state->result;
  }
  return true;
}

std::wstring LinuxCommandWindowRunner::OperationDirectory(uint64_t operationId) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::shared_ptr<WatchState> state = impl_->Find(operationId);
  return state == nullptr ? std::wstring() : state->directory;
}

unsigned long long LinuxCommandWindowRunner::LauncherProcessId(uint64_t operationId) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::shared_ptr<WatchState> state = impl_->Find(operationId);
  return state == nullptr ? 0 : state->launcherProcessId;
}

std::vector<std::wstring> LinuxCommandWindowRunner::PreservedOperationDirectories() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->preservedDirectories;
}

void LinuxCommandWindowRunner::ClearAllResults() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  std::erase_if(impl_->records, [](const std::shared_ptr<WatchState>& state) {
    return state->completionRecorded && state->threadFinished;
  });
}

}  // namespace gc::platform
