#include "app/task_coordinator.h"

#include <utility>

#include "git/repository.h"

namespace gc::app {
namespace {

// 身份比較走折疊路徑：Windows 路徑大小寫不敏感、正反斜杠同義，
// 否則同一倉庫換一種寫法就會被當成「換了倉庫」而白白丟掉列表與選擇狀態。
[[nodiscard]] std::wstring IdentityKey(std::wstring_view path) { return git::CanonicalPathKey(path); }

[[nodiscard]] std::wstring Number(unsigned long long value) { return std::to_wstring(value); }

// 该终态是否带有意义的 Git 退出码：只有 Git 进程真的被创建并跑完（辅助进程在结果行里
// 上报了 CreateProcess 的事实）才是。gitNotStarted 与其余未知/失败形态都没有退出码可谈，
// 文案改走 failureReason，绝不印出一个不属于 Git 的“退出码”。
[[nodiscard]] bool HasExitCode(git::CommandCompletion completion) noexcept {
  return completion == git::CommandCompletion::finished;
}

}  // namespace

bool TaskCoordinator::BindRepository(std::wstring_view gitExePath, std::wstring_view workTreeRoot) {
  if (gitExePath.empty() || workTreeRoot.empty()) {
    // 缺少任一項就沒有安全的查詢落點，等同解綁。
    UnbindRepository();
    return false;
  }
  const std::wstring nextGit = IdentityKey(gitExePath);
  const std::wstring nextRoot = IdentityKey(workTreeRoot);
  const bool same = identity_.Valid() && nextGit == IdentityKey(identity_.gitExePath) &&
                    nextRoot == IdentityKey(identity_.workTreeRoot);
  if (same) {
    return false;  // 同一身份：版本不變，在途讀取仍然算數。
  }
  ++identity_.generation;
  identity_.gitExePath = std::wstring(gitExePath);
  identity_.workTreeRoot = std::wstring(workTreeRoot);
  // 身份已變：本槽不再等這份結果，但保留舊憑證讓遲到的通知能被明確判為「屬於上個倉庫」。
  readInFlight_ = false;
  queuedRefresh_ = false;
  followUpConclusion_.clear();
  return true;
}

void TaskCoordinator::UnbindRepository() {
  if (!identity_.Valid() && !readInFlight_ && !queuedRefresh_) {
    return;
  }
  ++identity_.generation;
  identity_.gitExePath.clear();
  identity_.workTreeRoot.clear();
  readInFlight_ = false;
  queuedRefresh_ = false;
  followUpConclusion_.clear();
}

RefreshSchedule TaskCoordinator::RequestRefresh() {
  if (!identity_.Valid()) {
    return RefreshSchedule::ignored;
  }
  if (readInFlight_) {
    queuedRefresh_ = true;  // 單槽合併：絕不排隊，讀完補一次即止。
    return RefreshSchedule::merged;
  }
  return RefreshSchedule::start;
}

ReadTicket TaskCoordinator::BeginRead() {
  ReadTicket ticket;
  if (!identity_.Valid()) {
    return ticket;  // serial 為 0：這樣的憑證一定不會被採納。
  }
  ++readSerial_;
  readInFlight_ = true;
  queuedRefresh_ = false;
  ticket.serial = readSerial_;
  ticket.generation = identity_.generation;
  pendingRead_ = ticket;
  return ticket;
}

ReadDisposition TaskCoordinator::CompleteRead(unsigned long long serial) {
  if (serial == 0 || serial != pendingRead_.serial) {
    return ReadDisposition::notPending;  // 重复通知，或已被更晚的读取取代。
  }
  readInFlight_ = false;
  if (pendingRead_.generation != identity_.generation) {
    // 期間換了倉庫或換了 Git 程序：這份快照屬於別的倉庫，絕不能覆蓋當前列表。
    // 保留憑證，讓同一序號的重複通知也一律按過期處理。
    queuedRefresh_ = false;
    followUpConclusion_.clear();
    return ReadDisposition::staleRepository;
  }
  pendingRead_ = ReadTicket{};
  return ReadDisposition::accepted;
}

bool TaskCoordinator::BeginOperation(std::wstring_view displayName, unsigned long long* outSerial,
                                    OperationExitPolicy policy) {
  if (operationInFlight_) {
    return false;
  }
  ++operationSerial_;
  operationInFlight_ = true;
  operationName_ = std::wstring(displayName);
  operationExitPolicy_ = policy;
  if (outSerial != nullptr) {
    *outSerial = operationSerial_;
  }
  return true;
}

OperationOutcome TaskCoordinator::FinishOperation(unsigned long long serial, git::CommandCompletion completion,
                                                  long exitCode, std::wstring_view failureReason) {
  OperationOutcome outcome;
  outcome.serial = serial;
  outcome.completion = completion;
  outcome.exitCode = exitCode;
  if (serial == 0 || serial != operationSerial_ || !operationInFlight_) {
    return outcome;  // 不是本槽在途的那次操作：不結案，也不釋放槽位。
  }
  operationInFlight_ = false;
  outcome.recognised = true;
  outcome.displayName = operationName_;
  const bool viewOperation = operationExitPolicy_ == OperationExitPolicy::readOnlyView;
  // 成功與否只認 Git 自己的退出碼：cmd 窗口起得再好、窗口一直開著都不算。
  outcome.succeeded = completion == git::CommandCompletion::finished &&
                      (exitCode == 0 || (viewOperation && exitCode == 1));
  // 失敗的操作同樣可能已經改動倉庫（提交到一半、push 被拒、合併留下衝突），
  // 因此成敗都要重讀一次工作區。
  outcome.refreshRequested = true;

  std::wstring note = outcome.displayName + L" " + std::wstring(git::CommandCompletionLabel(completion));
  if (HasExitCode(completion)) {
    note += L"，Git 退出码 " + Number(static_cast<unsigned long long>(exitCode));
    if (viewOperation) {
      // 查看类操作的 0/1 含义由发起方（git::DescribeDiffViewExitCode）解释：
      // `git diff` 与 `git diff --no-index` 对「有没有差异」用的退出码并不一样，
      // 协调器只知道“这两种都算正常完成”，不该替它下结论。
      if (!outcome.succeeded) {
        note += L"（既不是 0 也不是 1，Git 报了错误，详细输出在命令窗口里查看）";
      }
    } else {
      note += outcome.succeeded ? L"（成功）" : L"（非 0，失败。详细输出在命令窗口里查看）";
    }
  } else if (!failureReason.empty()) {
    note += L"：" + std::wstring(failureReason);
  } else {
    note += L"（未取到 Git 退出码，结果以命令窗口里的输出为准）";
  }
  outcome.note = std::move(note);
  return outcome;
}

OperationOutcome TaskCoordinator::ForgetOperation(unsigned long long serial, std::wstring_view reason) {
  // 通知丢失：按「结果未知」结案。槽位必须释放，否则一个已经消失的操作会把界面永久锁住。
  return FinishOperation(serial, git::CommandCompletion::stillUnknown, 0, reason);
}

std::wstring TaskCoordinator::ReadStartedText() const {
  return L"正在后台刷新（仓库摘要与 git status，均为只读查询，不弹出命令窗口）…";
}

std::wstring TaskCoordinator::ReadFinishedText(std::wstring_view outcome) {
  std::wstring text = std::wstring(outcome);
  if (!followUpConclusion_.empty()) {
    // 這次讀取是為剛結束的操作補做的：操作結論與倉庫現狀一起顯示才看得懂「做完之後怎麼樣了」。
    text = followUpConclusion_ + L" " + text;
    followUpConclusion_.clear();
  }
  return text;
}

void TaskCoordinator::RememberOperationConclusion(std::wstring_view conclusion) {
  followUpConclusion_ = std::wstring(conclusion);
}

}  // namespace gc::app
