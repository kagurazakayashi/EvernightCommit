#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gc::platform {

// 选择列表里的一个候选远端：显示「名字　　URL」一行。
struct RemoteChoiceItem {
  std::wstring name;
  std::wstring url;  // 可为空（远端没有记录 fetch URL），空时界面显示占位说明
};

// 对话框的像素布局：与身份输入框同一套规矩——资源脚本里的几何只是占位，
// 真正的尺寸按呼叫方（主窗口）传入的 DPI 像素重摆，高 DPI 下才和界面其它部分一致。
struct RemoteChoiceLayout {
  int margin = 0;
  int gap = 0;
  int width = 0;        // 内容宽度（不含左右边距）
  int labelHeight = 0;
  int listHeight = 0;   // 列表区高度（条目再多也在里面滚动）
  int buttonWidth = 0;
  int buttonHeight = 0;
};

struct RemoteChoiceSpec {
  std::wstring title;   // 窗口标题
  std::wstring label;   // 列表上方那句「为什么要在这里选」的说明（fetch 方案的 explanation）
  std::vector<RemoteChoiceItem> items;  // 呼叫方按 Git 给出的顺序传入
  std::wstring okText{L"抓取选中的远端"};
  std::wstring cancelText{L"取消"};
  HFONT font = nullptr;
  RemoteChoiceLayout layout{};
};

struct RemoteChoiceResult {
  bool accepted = false;
  int selectedIndex = -1;  // accepted 时有效，对应 spec.items 的下标
};

// 模态的远端选择框：说明 + 单选列表 + 确定/取消。列表里每一项都是一个既有远端
// （名字与 fetch URL 同行展示），目标必须看得见才谈得上「不猜」。
// 行为与身份输入框一致：双击列表条目等同确定；Esc/关闭按钮等同取消；
// 确定但没有选中项时保持打开并在框内说明。只有一个候选时默认选中的就是它——
// 「直接作为候选展示」，但点头与否仍在用户手里。
[[nodiscard]] RemoteChoiceResult PromptForRemoteChoice(HWND parent, const RemoteChoiceSpec& spec);

}  // namespace gc::platform
