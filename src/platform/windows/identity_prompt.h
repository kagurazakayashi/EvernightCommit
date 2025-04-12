#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace gc::platform {

// 模態輸入框的像素佈局。平台層不引用界面層的度量類型，因此由呼叫方把 DPI 換算好的
// 像素值傳進來（同一套縮放規則才能與主窗口一致，而不是各按各的對話框單位猜）。
struct IdentityPromptLayout {
  int margin = 0;
  int gap = 0;
  int width = 0;          // 內容寬度（不含左右邊距）
  int labelHeight = 0;
  int editHeight = 0;
  int noteHeight = 0;     // 校驗說明的行高（預留兩行的餘量由呼叫方給）
  int buttonWidth = 0;
  int buttonHeight = 0;
};

// 一次「姓名 <邮箱>」輸入框的參數。
struct IdentityPromptSpec {
  std::wstring title;              // 標題列文字
  std::wstring label;              // 輸入框上方的說明
  std::wstring initialValue;       // 編輯現有條目時的原文
  std::wstring okText{L"确定"};
  std::wstring cancelText{L"取消"};
  HFONT font = nullptr;            // 與主窗口同一個字體，避免中文顯示不一致
  IdentityPromptLayout layout{};
  // 校驗回調：返回空字串表示可以接受；否則返回顯示在框裡的原因，對話框保持打開。
  // Git 語義（身分格式）不進本模組，由呼叫方帶入，平台層只負責「問一個字串」。
  std::function<std::wstring(const std::wstring&)> validate;
};

struct IdentityPromptResult {
  bool accepted = false;
  std::wstring value;
};

// 以應用資源裡的對話框模板（IDD_COMMIT_IDENTITY_DIALOG）彈一個模態輸入框：
// 標籤 + 單行編輯框 + 校驗說明 + 確定/取消。Enter 等同確定、Esc 等同取消、關閉按鈕等同取消。
// 校驗不過時不關閉，說明寫在框裡，使用者原地改。
// 模板幾何只是佔位：控件位置與窗口大小一律按呼叫方給的像素度量重擺，因此高 DPI 下
// 與主窗口的其它控件用同一套縮放。parent 為 nullptr 時置於螢幕中央（DS_CENTER）。
[[nodiscard]] IdentityPromptResult PromptForIdentity(HWND parent, const IdentityPromptSpec& spec);

}  // namespace gc::platform
