// wxWidgets 启动目标：唯一目的是把「依赖从哪来、能不能链接、窗口起不起得来」这三件事
// 在迁移业务界面之前验到可复核的程度上。这里没有任何 Git 业务流程，也不给尚未实现的
// Linux 平台服务填空壳——界面契约（src/platform/）与控制器接进来是下一步的事。
#include <wx/app.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/version.h>
#include <wx/versioninfo.h>

#include <cstdio>
#include <cstring>

namespace {

// 运行期真正链接到的那份 wx 库版本：只能在 wxApp 已经创建之后问
// （在 main 早期调用会因为平台信息还没初始化而触发断言，Debug 库里尤其吵）。
std::string LinkedWxVersion() {
  return wxGetLibraryVersionInfo().ToString().utf8_str().data();
}

class BootstrapWindow final : public wxFrame {
public:
  BootstrapWindow() : wxFrame(nullptr, wxID_ANY, wxString::FromUTF8("EvernightCommit wx 启动检查")) {
    auto* label =
        new wxStaticText(this, wxID_ANY,
                         wxString::FromUTF8("wxWidgets 已链接：") +
                             wxString::FromUTF8(LinkedWxVersion().c_str()));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(label, 1, wxALL | wxALIGN_CENTER_HORIZONTAL, 16);
    SetSizer(sizer);
    SetClientSize(480, 120);
    Centre();
  }
};

class BootstrapApp final : public wxApp {
public:
  bool OnInit() override {
    auto* window = new BootstrapWindow();
    window->Show();
    return true;
  }
};

void PrintVersions() {
  std::printf("gc_wx_bootstrap\n");
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
  std::printf("  wchar_t：%zu 字节（Windows 是 UTF-16，Linux 是 32 位单元，"
              "同一段代码在两侧不等于同一含义）\n",
              sizeof(wchar_t));
}

}  // namespace

// 宏按文件作用域使用，并且自带一条声明、必须补分号：它在这里定义 wxGetApp()、
// wxCreateApp() 与 wxAppInitializer，而 main() 由下面的 wxEntry 手工驱动
// （这样 --version 这种不需要消息循环的入口才能自己决定退出码）。
wxIMPLEMENT_APP_NO_MAIN(BootstrapApp);

int main(int argc, char** argv) {
  // --version/--check 不需要显示服务：无人值守环境与只核依赖时用得上。
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "--check") == 0) {
      PrintVersions();
      return 0;
    }
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::printf("用法：gc_wx_bootstrap [--version|--check|--help]\n"
                  "  无参数：在 $DISPLAY 上开一个检查窗口（关掉窗口即退出）。\n"
                  "  --version/--check：只打印版本与链接形态，不碰显示服务。\n");
      return 0;
    }
  }
  return wxEntry(argc, argv);
}
