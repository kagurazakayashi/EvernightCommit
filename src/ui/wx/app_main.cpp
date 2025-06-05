// 统一 wxWidgets 界面的入口。
//
// Linux：先处理终端辅助入口（终端里启动的就是本程序自己的 --gc-terminal-helper），
// 再进入 wx 事件循环；--version/--check 不碰显示服务。
// Windows：由 wxIMPLEMENT_APP 提供 WinMain；辅助入口在 OnInit 里先于任何窗口创建处理。
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <wx/app.h>
#include <wx/version.h>

#include "ui/wx/main_frame.h"

#ifdef _WIN32
#include "platform/windows/command_window_helper.h"
#else
#include "platform/linux/command_window_helper.h"
#endif

namespace {

#ifndef _WIN32
void PrintVersions() {
  std::printf("EvernightCommit wx\n");
#ifdef GC_VERSION_STRING
  std::printf("  程序版本：%s\n", GC_VERSION_STRING);
#endif
#ifdef GC_WX_VERSION_STRING
  std::printf("  wxWidgets：配置期发现 %s，头文件 %d.%d.%d\n", GC_WX_VERSION_STRING,
              wxMAJOR_VERSION, wxMINOR_VERSION, wxRELEASE_NUMBER);
#else
  std::printf("  wxWidgets：头文件 %d.%d.%d\n", wxMAJOR_VERSION, wxMINOR_VERSION,
              wxRELEASE_NUMBER);
#endif
  std::printf("  wchar_t：%zu 字节（Windows 是 UTF-16，Linux 是 32 位单元）\n", sizeof(wchar_t));
}
#endif

class EvernightCommitApp final : public wxApp {
public:
  bool OnInit() override {
#ifdef _WIN32
    int exitCode = 0;
    if (gc::platform::RunCommandWindowHelperIfRequested(&exitCode)) {
      std::exit(exitCode);
    }
#endif
    // 应用图标由各窗口设置（MainFrame::SetIcon，Windows 任务栏同样取窗口图标）；
    // wxApp::SetIcon 只在 MSW 存在，不在这里做平台分支。
    auto* frame = new gc::ui::wx::MainFrame();
    frame->Show();
    return true;
  }
};

}  // namespace

#ifdef _WIN32
wxIMPLEMENT_APP(EvernightCommitApp);
#else
wxIMPLEMENT_APP_NO_MAIN(EvernightCommitApp);

int main(int argc, char** argv) {
  int helperExitCode = 0;
  if (gc::platform::RunTerminalHelperIfRequested(argc, argv, &helperExitCode)) {
    return helperExitCode;
  }
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "--check") == 0) {
      PrintVersions();
      return 0;
    }
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::printf("用法：evernight_commit_wx [--version|--check|--help]\n"
                  "  无参数：打开统一主界面（仓库浏览、只读查看、暂存与创建提交；需要 DISPLAY 或 WAYLAND_DISPLAY）。\n"
                  "  --version/--check：只打印版本与链接形态，不碰显示服务。\n");
      return 0;
    }
  }
  return wxEntry(argc, argv);
}
#endif
