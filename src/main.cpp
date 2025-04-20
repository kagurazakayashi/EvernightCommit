#include <windows.h>

#include "platform/windows/command_window_helper.h"
#include "platform/windows/platform_init.h"
#include "ui/main_window.h"
#include "ui/splitter.h"

namespace {

void ShowStartupFailure() {
  ::MessageBoxW(nullptr, L"程序初始化失败：无法创建主窗口。", gc::ui::kWindowTitle, MB_OK | MB_ICONERROR);
}

}  // namespace

// GUI 子系统入口：启动时不附加控制台窗口。
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  // 命令窗口辅助入口：本程序被自己以 --gc-console-helper 启动时，在这里跑完那条
  // 已由界面创建并核验过的 Git 操作并结束进程 —— 不创建主窗口，也不初始化 COM/通用控件。
  // 判定只看命令行第一个参数，普通启动必然返回 false，走下面的界面流程。
  int helperExitCode = 0;
  if (gc::platform::RunCommandWindowHelperIfRequested(&helperExitCode)) {
    return helperExitCode;
  }
  try {
    gc::platform::CommonControls commonControls;
    gc::platform::ComApartment apartment;  // “浏览…”对话框依赖 COM 套间

    int exitCode = 1;
    {
      gc::ui::MainWindow window;
      if (window.Create(instance, showCommand)) {
        MSG message{};
        for (;;) {
          const int gotten = ::GetMessageW(&message, nullptr, 0, 0);
          if (gotten == 0 || gotten == -1) {
            break;
          }
          // 用对话框管理器提供 Tab / Shift+Tab / Enter 的焦点行为。
          if (::IsDialogMessageW(window.handle(), &message) == FALSE) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
          }
        }
        exitCode = static_cast<int>(message.wParam);
      } else {
        ShowStartupFailure();
      }
      gc::ui::MainWindow::UnregisterWindowClass(instance);
      gc::ui::Splitter::UnregisterWindowClass(instance);
    }
    return exitCode;
  } catch (...) {
    ShowStartupFailure();
    return 2;
  }
}
