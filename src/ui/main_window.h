#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "app/app_state.h"
#include "platform/windows/git_verify_worker.h"
#include "platform/windows/raii.h"
#include "platform/windows/repo_detect.h"
#include "ui/action_bar.h"
#include "ui/changes_pane.h"
#include "ui/commit_form.h"
#include "ui/controls.h"
#include "ui/layout.h"
#include "ui/repo_bar.h"
#include "ui/splitter.h"
#include "ui/ui_metrics.h"

namespace gc::ui {

inline constexpr const wchar_t* kMainWindowWindowClass = L"EvernightCommit.MainWindow";
inline constexpr const wchar_t* kWindowTitle = L"Git 提交工具";

// 主窗口：只做窗口过程分发、子面板装配与布局调用，业务状态留在 app::AppState。
class MainWindow {
public:
  MainWindow() = default;
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  [[nodiscard]] static bool RegisterWindowClass(HINSTANCE instance);
  static void UnregisterWindowClass(HINSTANCE instance);

  [[nodiscard]] bool Create(HINSTANCE instance, int showCommand);
  [[nodiscard]] HWND handle() const noexcept { return window_.get(); }

private:
  static LRESULT CALLBACK Thunk(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

  // 下面这些辅助函数一律使用传入的窗口句柄：WM_CREATE 期间成员 window_ 尚未赋值。
  void OnCreate(HWND window);
  void DoLayout(HWND window);
  void RefreshTexts(HWND window);
  // 三块列表的空状态文案随识别结果变化（未加载 / 已识别但工作区读取未实现）。
  [[nodiscard]] gc::git::EmptyStateTexts WorkspaceStateTexts() const;
  void UpdateCommandAvailability();
  void RegisterTooltips();
  void ApplyFonts(HWND window);
  void UpdateLayoutSpecs(HWND window);
  [[nodiscard]] SIZE MinimumWindowSize(HWND window) const;
  void OnCommand(HWND window, WPARAM wParam);
  void OnGitComboNotify(HWND window, UINT notifyCode);
  void OnRepoEditNotify(HWND window, UINT notifyCode);
  void BrowseRepoPath(HWND window);
  void BrowseGitPath(HWND window);
  void OnSplitterDragged(HWND window, int splitterId, int parentX);

  // Git 可执行文件发现/选择/验证（步骤 2）。
  void InitializeGitDetection(HWND window);
  void CommitGitInput(HWND window);
  void RequestGitVerification(HWND window, const std::wstring& normalizedPath);
  void OnGitProbeCompleted(HWND window, uint64_t completionSerial);

  // 仓库路径选择与识别（步骤 3）。
  void InitializeRepoInput(HWND window);
  void CommitRepoInput(HWND window);
  void RequestRepoDetection(HWND window, const std::wstring& normalizedPath);
  void SetRepoFailed(HWND window, const std::wstring& normalizedPath, gc::git::RepoError error,
                     std::wstring_view detail);
  void OnRepoDetectCompleted(HWND window, uint64_t completionSerial);
  void ResumeRepoDetectionWhenGitReady(HWND window);

  platform::UniqueWindow window_;
  platform::GitVerifyWorker gitWorker_;
  platform::RepoDetectWorker repoWorker_;
  app::AppState state_;
  UiMetrics metrics_;
  std::wstring programInfo_;
  bool suppressRepoEditNotify_ = false;  // 程序改写输入框时不再触发一次识别

  BandSpec bandSpec_{};
  ChangesSpec changesSpec_{};
  RECT changesArea_{};

  RepoBar repoBar_;
  RepoInfoBar infoBar_;
  ChangesPane changesPane_;
  CommitForm commitForm_;
  ActionBar actionBar_;
  Splitter leftSplitter_;
  Splitter rightSplitter_;
  ToolTips tooltips_;
};

}  // namespace gc::ui
