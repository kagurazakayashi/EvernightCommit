#include "platform/windows/conflict_probe.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "git/pull_plan.h"
#include "platform/windows/git_query_result.h"
#include "platform/windows/raii.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/win_path.h"

namespace gc::platform {
namespace {

// 一次流程冲突现场读取里最慢的一条是 `git diff --name-only -z --diff-filter=U`（大仓库要扫完整棵树），
// 沿用工作区读取的 20 秒放宽值；调用方没给超时时用它兜底，绝不落回「没有超时」。
constexpr unsigned long kConflictQueryTimeoutFallbackMs = 20000;

// 痕迹档案的读取上限：那些档案本来就是一行对象 ID 或一行引用名。超过上限即视为读不回来，
// 不把半截内容当事实（AGENTS：残缺的输出不是「少几条记录」，而是「这份事实不成立」）。
constexpr unsigned long long kConflictFileLimitBytes = 4096;

enum class Existence {
  absent = 0,    // 系统明确回答「没有这个档案/目录」
  present,       // 确实存在
  unknown,       // 判断本身被拒绝（权限等）：这一轮无从知道
};

Existence ClassifyExistence(const std::wstring& path, bool wantDirectory, std::wstring& failureDetail) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return directory == wantDirectory ? Existence::present : Existence::absent;
  }
  switch (::GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return Existence::absent;
    default:
      failureDetail = L"（存在性没能问出来，Windows 错误码 " +
                      std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）";
      return Existence::unknown;
  }
}

// 限长读回一个痕迹档案的内容：严格 UTF-8 解码，去掉行尾换行。
// 返回 false 表示「这一份没读回来」，此时 outText 不做任何承诺。
bool ReadMarkerFile(const std::wstring& path, std::wstring* outText, std::wstring* outReason) {
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                        FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *outReason = L"打不开（Windows 错误码 " +
                 std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）";
    return false;
  }
  UniqueHandle guard(handle);

  LARGE_INTEGER size{};
  if (::GetFileSizeEx(guard.get(), &size) == 0 || size.QuadPart < 0) {
    *outReason = L"大小问不出来";
    return false;
  }
  if (static_cast<unsigned long long>(size.QuadPart) > kConflictFileLimitBytes) {
    *outReason = L"超过 " + std::to_wstring(kConflictFileLimitBytes) + L" 字节上限（这不是一个正常的痕迹档案）";
    return false;
  }
  const auto want = static_cast<DWORD>(size.QuadPart);
  std::string bytes(want, '\0');
  DWORD got = 0;
  if (want > 0 && ::ReadFile(guard.get(), bytes.data(), want, &got, nullptr) == 0) {
    *outReason = L"读不出内容（Windows 错误码 " +
                 std::to_wstring(static_cast<unsigned long>(::GetLastError())) + L"）";
    return false;
  }
  if (got != want) {
    *outReason = L"读到的字节数与档案大小不符（读到 " + std::to_wstring(got) + L"，档案 " +
                 std::to_wstring(want) + L"）";
    return false;
  }
  std::wstring decoded;
  if (!TryUtf8ToUtf16Strict(bytes, decoded)) {
    *outReason = L"内容不是合法的 UTF-8";
    return false;
  }
  while (!decoded.empty() && (decoded.back() == L'\n' || decoded.back() == L'\r')) {
    decoded.pop_back();
  }
  *outText = std::move(decoded);
  return true;
}

class MarkerReader {
public:
  explicit MarkerReader(std::wstring gitDirectory) : base_(std::move(gitDirectory)) {}

  // 三个方法都会把「存在性判断本身被拒绝」记进 unknownReasons：那种情况下这一轮不可采信。
  bool HasFile(std::wstring_view name, std::wstring_view displayName) {
    const std::wstring path = Join(name);
    std::wstring detail;
    const Existence state = ClassifyExistence(path, false, detail);
    if (state == Existence::unknown) {
      unknownReasons_.push_back(std::wstring(displayName) + detail);
      return false;
    }
    return state == Existence::present;
  }

  bool HasDirectory(std::wstring_view name, std::wstring_view displayName) {
    const std::wstring path = Join(name);
    std::wstring detail;
    const Existence state = ClassifyExistence(path, true, detail);
    if (state == Existence::unknown) {
      unknownReasons_.push_back(std::wstring(displayName) + detail);
      return false;
    }
    return state == Existence::present;
  }

  // 读一个已知存在的档案；读不回来时把档案名与原因记进 failures（「读不回来」绝不当成「没有」）。
  bool Read(std::wstring_view name, std::wstring_view displayName, std::wstring* value) {
    std::wstring reason;
    if (ReadMarkerFile(Join(name), value, &reason)) {
      return true;
    }
    failures_.push_back(std::wstring(displayName) + L"：" + reason);
    return false;
  }

  // 存在就读，不存在就留空；只有存在却读不回来才记失败。
  void ReadIfPresent(std::wstring_view name, std::wstring_view displayName, bool present,
                     std::wstring* value) {
    if (!present) {
      return;
    }
    std::wstring read{};
    if (Read(name, displayName, &read)) {
      *value = std::move(read);
    }
  }

  void Absorb(const MarkerReader& other) {
    for (const std::wstring& item : other.unknownReasons_) {
      unknownReasons_.push_back(item);
    }
    for (const std::wstring& item : other.failures_) {
      failures_.push_back(item);
    }
  }

  [[nodiscard]] const std::vector<std::wstring>& UnknownReasons() const noexcept {
    return unknownReasons_;
  }
  [[nodiscard]] const std::vector<std::wstring>& Failures() const noexcept { return failures_; }

private:
  [[nodiscard]] std::wstring Join(std::wstring_view name) const {
    return base_ + std::wstring(name);
  }

  std::wstring base_;
  std::vector<std::wstring> unknownReasons_;  // 存在性都问不成的档案名（这一轮不可采信的根据）
  std::vector<std::wstring> failures_;        // 存在却读不回内容的档案名 + 原因
};

MarkerReader ReadRebaseDir(const std::wstring& gitBase, std::wstring_view directoryName,
                           git::ConflictMarkerFacts* facts) {
  MarkerReader reader(gitBase + std::wstring(directoryName) + L"\\");
  const std::wstring prefix = std::wstring(directoryName) + L"\\";
  const bool headNamePresent = reader.HasFile(L"head-name", (prefix + L"head-name").c_str());
  const bool ontoPresent = reader.HasFile(L"onto", (prefix + L"onto").c_str());
  const bool origHeadPresent = reader.HasFile(L"orig-head", (prefix + L"orig-head").c_str());
  const bool msgnumPresent = reader.HasFile(L"msgnum", (prefix + L"msgnum").c_str());
  const bool endPresent = reader.HasFile(L"end", (prefix + L"end").c_str());
  const bool interactivePresent =
      reader.HasFile(L"interactive", (prefix + L"interactive").c_str());
  reader.ReadIfPresent(L"head-name", (prefix + L"head-name").c_str(), headNamePresent,
                       &facts->rebaseHeadName);
  reader.ReadIfPresent(L"onto", (prefix + L"onto").c_str(), ontoPresent, &facts->rebaseOnto);
  reader.ReadIfPresent(L"orig-head", (prefix + L"orig-head").c_str(), origHeadPresent,
                       &facts->rebaseOrigHead);
  reader.ReadIfPresent(L"msgnum", (prefix + L"msgnum").c_str(), msgnumPresent, &facts->rebaseMsgnum);
  reader.ReadIfPresent(L"end", (prefix + L"end").c_str(), endPresent, &facts->rebaseEnd);
  facts->rebaseInteractiveMark = facts->rebaseInteractiveMark || interactivePresent;
  return reader;
}

git::GitQueryResult RunQuery(const git::GitQueryRunner& runner, const std::wstring& exePath,
                             const std::wstring& directory,
                             const std::vector<std::wstring>& arguments) {
  if (!runner || arguments.empty()) {
    git::GitQueryResult failed;
    failed.started = false;
    return failed;
  }
  return runner(exePath, directory, arguments);
}

// 顿号拼接一串名字（只用于说明文字，不参与任何判定）。
std::wstring JoinNames(const std::vector<std::wstring>& names) {
  std::wstring text;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0) {
      text += L"、";
    }
    text += names[index];
  }
  return text;
}

git::ConflictMarkerFacts UnreadableMarkers(std::wstring reason) {
  git::ConflictMarkerFacts markers;
  markers.probed = false;
  markers.probeFailure = std::move(reason);
  return markers;
}

}  // namespace

git::ConflictMarkerFacts ProbeConflictMarkers(std::wstring_view absoluteGitDir) {
  if (absoluteGitDir.empty()) {
    return UnreadableMarkers(L"这个仓库没有可用的绝对 Git 目录（仓库识别没给出落点），"
                             L"流程痕迹这一轮没探。");
  }
  std::wstring base(absoluteGitDir);
  std::wstring detail;
  if (ClassifyExistence(base, true, detail) != Existence::present) {
    return UnreadableMarkers(L"Git 目录在这一轮问不出形态" +
                             (detail.empty() ? std::wstring(L"（它现在不是一个可读的目录）") : detail) +
                             L"，流程痕迹因此不可采信。");
  }
  if (base.back() != L'\\' && base.back() != L'/') {
    base.push_back(L'\\');
  }

  git::ConflictMarkerFacts markers;
  MarkerReader reader(base);
  markers.mergeHead = reader.HasFile(L"MERGE_HEAD", L"MERGE_HEAD");
  markers.revertHead = reader.HasFile(L"REVERT_HEAD", L"REVERT_HEAD");
  markers.cherryPickHead = reader.HasFile(L"CHERRY_PICK_HEAD", L"CHERRY_PICK_HEAD");
  markers.bisectLog = reader.HasFile(L"BISECT_LOG", L"BISECT_LOG");
  markers.squashMsg = reader.HasFile(L"SQUASH_MSG", L"SQUASH_MSG");
  markers.mergeMode = reader.HasFile(L"MERGE_MODE", L"MERGE_MODE");
  markers.mergeAutostash = reader.HasFile(L"MERGE_AUTOSTASH", L"MERGE_AUTOSTASH");
  markers.rebaseAutostash = reader.HasFile(L"REBASE_AUTOSTASH", L"REBASE_AUTOSTASH");
  markers.indexLock = reader.HasFile(L"index.lock", L"index.lock");
  markers.rebaseMergeDir = reader.HasDirectory(L"rebase-merge", L"rebase-merge\\");
  markers.rebaseApplyDir = reader.HasDirectory(L"rebase-apply", L"rebase-apply\\");
  // sequencer\ 在链接工作树下落在公共 Git 目录那一层，本函数只看界面绑定的这一层：
  // 没看到只意味著「這一层没有」，绝不当成「不是一条序列」，因此它只用于额外说明、不参与拒绝。
  markers.sequencerDir = reader.HasDirectory(L"sequencer", L"sequencer\\");

  reader.ReadIfPresent(L"MERGE_HEAD", L"MERGE_HEAD", markers.mergeHead, &markers.mergeHeadOid);
  reader.ReadIfPresent(L"CHERRY_PICK_HEAD", L"CHERRY_PICK_HEAD", markers.cherryPickHead,
                       &markers.pickHeadOid);
  reader.ReadIfPresent(L"REVERT_HEAD", L"REVERT_HEAD", markers.revertHead, &markers.pickHeadOid);

  if (markers.rebaseMergeDir) {
    reader.Absorb(ReadRebaseDir(base, L"rebase-merge", &markers));
  }
  if (markers.rebaseApplyDir) {
    reader.Absorb(ReadRebaseDir(base, L"rebase-apply", &markers));
  }

  markers.contentFailures = reader.Failures();
  if (!reader.UnknownReasons().empty()) {
    // 存在性本身问不成：这一轮痕迹不可采信，宁可说「问不到」，也绝不说「没有流程停着」。
    std::wstring reason =
        L"这些痕迹档案的存在性在这一轮问不出来：" + JoinNames(reader.UnknownReasons()) +
        L"。看不见不等于没有流程停着，因此这一轮不可采信。";
    return UnreadableMarkers(std::move(reason));
  }
  markers.probed = true;
  return markers;
}

git::ConflictStateFacts CollectConflictState(const ConflictProbeRequest& request,
                                             const ConflictProbeDeps& deps) {
  git::ConflictMarkerFacts markers;
  if (deps.markers) {
    markers = deps.markers(request.absoluteGitDir);
  } else {
    markers = ProbeConflictMarkers(request.absoluteGitDir);
  }

  const std::wstring& dir = request.repositoryDirectory;
  git::GitQueryResult listing;
  git::GitQueryResult symbolicRef;
  git::GitQueryResult headObject;
  if (dir.empty() || !deps.runner) {
    // 没有仓库目录或没有可用的 Git 程序：三条查询一条都不发，判读层把它读成「问不到」，
    // 而不是「没有未合并文件」。
    return git::InterpretConflictState(markers, listing, symbolicRef, headObject);
  }

  listing = RunQuery(deps.runner, request.exePath, dir, git::BuildPullConflictListingArguments(dir));
  if (StopRequested(request.stopFlag)) {
    return git::InterpretConflictState(markers, listing, symbolicRef, headObject);
  }
  symbolicRef =
      RunQuery(deps.runner, request.exePath, dir, git::BuildPullSymbolicRefArguments(dir));
  headObject = RunQuery(deps.runner, request.exePath, dir, git::BuildPullHeadObjectArguments(dir));
  return git::InterpretConflictState(markers, listing, symbolicRef, headObject);
}

ConflictProbeDeps MakeConflictProbeDeps(unsigned long timeoutMilliseconds) {
  ConflictProbeDeps deps;
  deps.runner = [timeoutMilliseconds](const std::wstring& exePath, const std::wstring& directory,
                                      const std::vector<std::wstring>& arguments) {
    // 未合并清单带 -z（路径里有 NUL 分隔），按字节取回才能原样用。
    return RunGitBackgroundQuery(exePath, arguments, directory, timeoutMilliseconds);
  };
  deps.markers = [](const std::wstring& absoluteGitDir) {
    return ProbeConflictMarkers(absoluteGitDir);
  };
  return deps;
}

ConflictProbeOutcome RunConflictProbeLoad(const ConflictProbeRequest& request) {
  ConflictProbeOutcome outcome;
  outcome.repositoryDirectory = request.repositoryDirectory;  // 原样回显：判别在界面线程。
  const unsigned long timeout = request.timeoutMilliseconds != 0 ? request.timeoutMilliseconds
                                                                 : kConflictQueryTimeoutFallbackMs;
  const ConflictProbeDeps deps = MakeConflictProbeDeps(timeout);
  outcome.state = CollectConflictState(request, deps);
  return outcome;
}

}  // namespace gc::platform
