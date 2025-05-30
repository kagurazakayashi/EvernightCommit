#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gc::platform {

// ---------- 程序自绘对话框的平台无关契约 ----------
//
// 这两类框（要点单选、问一个字串）是业务流程的一部分：fetch 选远端、pull 选整合方式、
// 首次推送选目标远端与分支名、合作者身份输入。它们提出的问题和取到的答案都是数据，
// 不该带任何原生句柄、字体柄或对话框模板 ID。
//
// 各平台的实现负责把这份数据画出来：Windows 过渡实现见
// src/platform/windows/identity_prompt.cpp 与 remote_choice_dialog.cpp（Win32 模板 + 主窗口字体
// + DPI 像素换算都在那一侧完成）；wxWidgets 实现后续在同一份契约上另写，业务流程不动。
//
// 取消与确认永远是两种不同的答复，绝不用空字符串冒充取消：单选取消返回 accepted=false，
// 文本框取消同样 accepted=false，用户输入的空串则是 accepted=true 且 value 为空，
// 由调用方自己判定这种输入合不合格。

// 单选框里的一条候选：主文字 + 可选的第二栏说明（远端那一条是它的 fetch URL）。
// detail 为空时由实现决定显示占位说明还是不显示第二栏。
struct ChoiceItem {
  std::wstring label;
  std::wstring detail;
};

// 对话框的排版提示：一律以 DIP（96 dpi 逻辑像素）给出，由实现按当前 DPI 换算成像素。
// 这里刻意不给像素，也不给字体——字号、边距、按钮尺寸这些属于界面度量，
// 由主窗口的度量层填进请求，平台实现只照着摆。
struct DialogLayoutHints {
  int labelRows = 3;      // 说明文字预留的行数
  int noteRows = 2;       // 框内校验说明预留的行数（单选框不用）
  int contentWidth = 420; // 内容宽度，不含左右边距
  int listHeight = 140;   // 列表区高度（单选框用，条目多了在里面滚）
};

// 「要点列表让用户单选」的一次请求。
struct SingleChoiceRequest {
  std::wstring title;              // 窗口标题
  std::wstring label;              // 列表上方那句「为什么要在这里选」
  std::vector<ChoiceItem> items;   // 呼叫方按 Git 给出的顺序传入
  std::wstring okText{L"确定"};
  std::wstring cancelText{L"取消"};
  // 条目没有第二栏可显示时的占位，以及「没选中就按确定」的那句提醒。
  // 默认是「选远端」的说法；同一个框用作别的单选（例如 pull 的整合策略）时换掉，
  // 免得一个「选策略」的框里冒出「远端没有记录 fetch URL」。
  std::wstring emptyItemDetail{L"（这个远端没有记录 fetch URL）"};
  std::wstring needSelectionHint{L"先在列表里点选一个远端，再按确定。（取消不会执行任何命令）"};
  DialogLayoutHints hints{};
};

// 「问一个字串」的一次请求。Git 语义（身份格式、引用名规则）都不在本层，
// validate 只做框内的纯形态提示，权威裁定由调用方在拿到输入之后另行发起。
struct TextPromptRequest {
  std::wstring title;
  std::wstring label;         // 输入框上方的说明
  std::wstring initialValue;  // 编辑现有条目时的原文
  std::wstring okText{L"确定"};
  std::wstring cancelText{L"取消"};
  DialogLayoutHints hints{};
  // 校验回调：返回空字串表示可以接受；否则返回显示在框里的原因，对话框保持打开。
  std::function<std::wstring(const std::wstring&)> validate;
};

}  // namespace gc::platform
