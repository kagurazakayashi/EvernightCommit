#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "platform/windows/git_toolchain.h"

namespace gc::platform {

// Git 验证的后台控制器：验证在独立线程执行，GUI 线程不等待、不冻结；
// 每次请求携带递增序号，只有仍属于“最新一次请求”的结果会被 FetchLatest 取回，
// 较慢完成的旧结果自动作废，不会覆盖用户之后的选择。
// 完成后通过 PostMessageW 通知窗口（wParam=序号），不直接触碰任何控件。
class GitVerifyWorker {
public:
  GitVerifyWorker() = default;
  GitVerifyWorker(const GitVerifyWorker&) = delete;
  GitVerifyWorker& operator=(const GitVerifyWorker&) = delete;
  ~GitVerifyWorker();

  // 提交一次验证请求；线程惰性启动。窗口与通知消息在构造控制器时不需要，逐次传入。
  void RequestVerify(HWND notifyWindow, UINT completionMessage, std::wstring exePath,
                     unsigned long timeoutMilliseconds);

  // UI 线程在收到通知后调用：仅当 completionSerial 仍是最新请求且结果未被消费时返回 true。
  bool FetchLatest(uint64_t completionSerial, GitExeVerification* out);

  // 停止并等待工作线程结束（WM_DESTROY 调用；最长阻塞一次探测的超时时间）。
  void Shutdown();

private:
  void Loop();

  struct PendingRequest {
    std::wstring path;
    HWND window = nullptr;
    UINT message = 0;
    unsigned long timeoutMilliseconds = 0;
    uint64_t serial = 0;
  };

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<PendingRequest> pending_;
  uint64_t submittedSerial_ = 0;
  uint64_t processedSerial_ = 0;
  std::optional<std::pair<uint64_t, GitExeVerification>> completed_;
  bool consumed_ = true;
  bool stopping_ = false;
};

}  // namespace gc::platform
