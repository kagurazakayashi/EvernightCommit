#include "platform/windows/identity_prompt.h"

#include "ui/resource_ids.h"

namespace gc::platform {
namespace {

constexpr WORD kTextInputLimit = 2048;  // 單行編輯框的最大長度（身分用不到這麼長，這裡只是兜底）。

// 取回編輯框當前文字：用 WM_GETTEXT 而不是拿空緩衝區去探長度，後者在空指標上的行為沒有文件保證。
std::wstring ReadEditText(HWND dialog) {
  const HWND edit = ::GetDlgItem(dialog, IDC_IDENTITY_EDIT);
  const auto length = static_cast<size_t>(::SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0));
  std::wstring buffer(length + 1, L'\0');
  const LRESULT gotten = ::SendMessageW(edit, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
                                        reinterpret_cast<LPARAM>(buffer.data()));
  buffer.resize(gotten < 0 ? 0 : static_cast<size_t>(gotten));
  return buffer;
}

// 對話框的狀態：由 DialogBoxParamW 的 lParam 帶進來，存在 DWLP_USER。
struct DialogState {
  const IdentityPromptSpec* spec = nullptr;
  IdentityPromptResult* result = nullptr;
};

// 資源腳本裡的座標只是佔位：真正的尺寸全部按呼叫方傳入的 DPI 像素重擺，
// 否則高 DPI 下會按 96 DPI 的對話框單位縮放，和主窗口的其它控件對不齐。
void ArrangeLayout(HWND dialog, const IdentityPromptLayout& layout) {
  const int contentLeft = layout.margin;
  const int width = layout.width;
  int y = layout.margin;

  ::MoveWindow(::GetDlgItem(dialog, IDC_IDENTITY_LABEL), contentLeft, y, width, layout.labelHeight, TRUE);
  y += layout.labelHeight + layout.gap;

  ::MoveWindow(::GetDlgItem(dialog, IDC_IDENTITY_EDIT), contentLeft, y, width, layout.editHeight, TRUE);
  y += layout.editHeight + layout.gap;

  ::MoveWindow(::GetDlgItem(dialog, IDC_IDENTITY_NOTE), contentLeft, y, width, layout.noteHeight, TRUE);
  y += layout.noteHeight + layout.gap;

  const int buttonTop = y;
  const int right = contentLeft + width;
  ::MoveWindow(::GetDlgItem(dialog, IDOK), right - layout.buttonWidth, buttonTop, layout.buttonWidth,
               layout.buttonHeight, TRUE);
  ::MoveWindow(::GetDlgItem(dialog, IDCANCEL), right - layout.buttonWidth - layout.gap - layout.buttonWidth,
               buttonTop, layout.buttonWidth, layout.buttonHeight, TRUE);

  // 非客戶區（標題列與邊框）由系統加，這裡只算客戶區大小。
  const int clientHeight = buttonTop + layout.buttonHeight + layout.margin;
  const RECT client{0, 0, contentLeft + width + layout.margin, clientHeight};
  RECT window = client;
  ::AdjustWindowRect(&window, static_cast<DWORD>(::GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE);
  ::SetWindowPos(dialog, nullptr, 0, 0, window.right - window.left, window.bottom - window.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

INT_PTR CALLBACK DialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
  try {
    DialogState* state = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
      case WM_INITDIALOG: {
        state = reinterpret_cast<DialogState*>(lParam);
        ::SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (state == nullptr || state->spec == nullptr) {
          return FALSE;
        }
        const IdentityPromptSpec& spec = *state->spec;
        ::SetWindowTextW(dialog, spec.title.c_str());
        if (spec.font != nullptr) {
          // 資源腳本的字體只影響對話框單位換算；顯示用的字體在這裡換成主窗口那一個，
          // 中文才不會出現同一個界面裡兩種字形。
          for (const int id : {IDC_IDENTITY_LABEL, IDC_IDENTITY_EDIT, IDC_IDENTITY_NOTE, IDOK, IDCANCEL}) {
            ::SendDlgItemMessageW(dialog, id, WM_SETFONT, reinterpret_cast<WPARAM>(spec.font), TRUE);
          }
        }
        ArrangeLayout(dialog, spec.layout);
        ::SetDlgItemTextW(dialog, IDC_IDENTITY_LABEL, spec.label.c_str());
        ::SetDlgItemTextW(dialog, IDOK, spec.okText.c_str());
        ::SetDlgItemTextW(dialog, IDCANCEL, spec.cancelText.c_str());
        ::SetDlgItemTextW(dialog, IDC_IDENTITY_NOTE, L"");
        // 先立長度上限再回填文字：順序反過來時超長的原文會被靜默截斷而使用者毫無察覺。
        ::SendDlgItemMessageW(dialog, IDC_IDENTITY_EDIT, EM_SETLIMITTEXT, kTextInputLimit, 0);
        ::SetDlgItemTextW(dialog, IDC_IDENTITY_EDIT, spec.initialValue.c_str());
        ::SetFocus(::GetDlgItem(dialog, IDC_IDENTITY_EDIT));
        return TRUE;  // 焦點自己設過了，不讓系統再設一次。
      }
      case WM_COMMAND:
        if (state == nullptr || state->spec == nullptr) {
          return FALSE;
        }
        if (LOWORD(wParam) == IDOK) {
          const std::wstring text = ReadEditText(dialog);
          const std::wstring reason = state->spec->validate ? state->spec->validate(text) : L"";
          if (!reason.empty()) {
            // 校驗不過：說明寫進框裡、保持打開，讓使用者原地改，而不是弹完提示再丟失輸入。
            ::SetDlgItemTextW(dialog, IDC_IDENTITY_NOTE, reason.c_str());
            ::SetFocus(::GetDlgItem(dialog, IDC_IDENTITY_EDIT));
            return TRUE;
          }
          if (state->result != nullptr) {
            state->result->accepted = true;
            state->result->value = text;
          }
          ::EndDialog(dialog, IDOK);
          return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
          ::EndDialog(dialog, IDCANCEL);
          return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        // 關閉等同取消：不接受任何內容，原有條目不變。
        ::EndDialog(dialog, IDCANCEL);
        return TRUE;
      default:
        return FALSE;
    }
  } catch (...) {
    // 異常絕不穿過對話框過程：按取消收場，倉庫與表單都不會被改動。
    ::EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
}

}  // namespace

IdentityPromptResult PromptForIdentity(HWND parent, const IdentityPromptSpec& spec) {
  IdentityPromptResult result;
  HINSTANCE instance = nullptr;
  if (parent != nullptr) {
    instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
  }
  if (instance == nullptr) {
    instance = ::GetModuleHandleW(nullptr);
  }

  // 狀態與結果放在調用方的棧上：模態循環在 DialogBoxParamW 內部跑完才返回，整個期間它們必須活著。
  DialogState state{&spec, &result};
  static_cast<void>(::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_COMMIT_IDENTITY_DIALOG), parent,
                                      &DialogProcedure, reinterpret_cast<LPARAM>(&state)));
  return result;
}

}  // namespace gc::platform
