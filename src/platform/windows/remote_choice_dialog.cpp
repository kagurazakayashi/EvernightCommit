#include "platform/windows/remote_choice_dialog.h"

#include "ui/resource_ids.h"

namespace gc::platform {
namespace {

// 列表一行的展示文本：名字与 fetch URL 同行，目标一眼能对上号。
// 没有记录 fetch URL 的远端（罕见形态）显示占位说明，绝不显示成「看起来没配远端」。
std::wstring FormatItemText(const RemoteChoiceItem& item) {
  std::wstring text = item.name + L"　　";
  text += item.url.empty() ? L"（这个远端没有记录 fetch URL）" : item.url;
  return text;
}

struct DialogState {
  const RemoteChoiceSpec* spec = nullptr;
  RemoteChoiceResult* result = nullptr;
};

void ArrangeLayout(HWND dialog, const RemoteChoiceLayout& layout) {
  const int contentLeft = layout.margin;
  const int width = layout.width;
  int y = layout.margin;

  ::MoveWindow(::GetDlgItem(dialog, IDC_REMOTE_LABEL), contentLeft, y, width, layout.labelHeight, TRUE);
  y += layout.labelHeight + layout.gap;

  ::MoveWindow(::GetDlgItem(dialog, IDC_REMOTE_LIST), contentLeft, y, width, layout.listHeight, TRUE);
  y += layout.listHeight + layout.gap;

  const int buttonTop = y;
  const int right = contentLeft + width;
  ::MoveWindow(::GetDlgItem(dialog, IDOK), right - layout.buttonWidth, buttonTop, layout.buttonWidth,
               layout.buttonHeight, TRUE);
  ::MoveWindow(::GetDlgItem(dialog, IDCANCEL), right - layout.buttonWidth - layout.gap - layout.buttonWidth,
               buttonTop, layout.buttonWidth, layout.buttonHeight, TRUE);

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
        const RemoteChoiceSpec& spec = *state->spec;
        ::SetWindowTextW(dialog, spec.title.c_str());
        ::SetDlgItemTextW(dialog, IDC_REMOTE_LABEL, spec.label.c_str());
        ::SetDlgItemTextW(dialog, IDOK, spec.okText.c_str());
        ::SetDlgItemTextW(dialog, IDCANCEL, spec.cancelText.c_str());
        if (spec.font != nullptr) {
          for (const int id : {IDC_REMOTE_LABEL, IDC_REMOTE_LIST, IDOK, IDCANCEL}) {
            ::SendDlgItemMessageW(dialog, id, WM_SETFONT, reinterpret_cast<WPARAM>(spec.font), TRUE);
          }
        }
        ArrangeLayout(dialog, spec.layout);

        const HWND list = ::GetDlgItem(dialog, IDC_REMOTE_LIST);
        for (const RemoteChoiceItem& item : spec.items) {
          static_cast<void>(::SendMessageW(list, LB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(FormatItemText(item).c_str())));
        }
        if (!spec.items.empty()) {
          // 只有一个候选时默认选中那一个：候选直接摆在那里由人确认，程序不越俎代庖。
          static_cast<void>(::SendMessageW(list, LB_SETCURSEL, 0, 0));
        }
        ::SetFocus(list);
        return TRUE;  // 焦点自己设过了，不让系统再设一次。
      }
      case WM_COMMAND:
        if (state == nullptr || state->spec == nullptr) {
          return FALSE;
        }
        if (LOWORD(wParam) == IDOK) {
          const LRESULT selected =
              ::SendDlgItemMessageW(dialog, IDC_REMOTE_LIST, LB_GETCURSEL, 0, 0);
          if (selected == LB_ERR || selected < 0 ||
              static_cast<size_t>(selected) >= state->spec->items.size()) {
            // 没选中就按确定：说明写回标题区并保持打开——关掉丢掉选择比多读一句更糟。
            ::SetDlgItemTextW(dialog, IDC_REMOTE_LABEL,
                              L"先在列表里点选一个远端，再按确定。（取消不会执行任何命令）");
            ::SetFocus(::GetDlgItem(dialog, IDC_REMOTE_LIST));
            return TRUE;
          }
          if (state->result != nullptr) {
            state->result->accepted = true;
            state->result->selectedIndex = static_cast<int>(selected);
          }
          ::EndDialog(dialog, IDOK);
          return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
          ::EndDialog(dialog, IDCANCEL);
          return TRUE;
        }
        if (LOWORD(wParam) == IDC_REMOTE_LIST) {
          const UINT notification = HIWORD(wParam);
          if (notification == LBN_DBLCLK) {
            // 双击条目等同按确定：列表里的每一行本来就是要抓的那个远端。
            return DialogProcedure(dialog, WM_COMMAND, static_cast<WPARAM>(IDOK), lParam);
          }
          if (notification == LBN_SELCHANGE) {
            // 重新选中后把说明恢复回原始那句，免得「先点选一个」的提醒赖着不走。
            ::SetDlgItemTextW(dialog, IDC_REMOTE_LABEL, state->spec->label.c_str());
          }
        }
        return FALSE;
      case WM_CLOSE:
        // 关闭等同取消：不发起任何 fetch。
        ::EndDialog(dialog, IDCANCEL);
        return TRUE;
      default:
        return FALSE;
    }
  } catch (...) {
    // 异常绝不穿过对话框过程：按取消收场，仓库不会被动到一根指头。
    ::EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
}

}  // namespace

RemoteChoiceResult PromptForRemoteChoice(HWND parent, const RemoteChoiceSpec& spec) {
  RemoteChoiceResult result;
  HINSTANCE instance = nullptr;
  if (parent != nullptr) {
    instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
  }
  if (instance == nullptr) {
    instance = ::GetModuleHandleW(nullptr);
  }

  // 状态与结果放在调用方的栈上：模态循环在 DialogBoxParamW 内部跑完才返回，
  // 整个期间它们必须活着。
  DialogState state{&spec, &result};
  static_cast<void>(::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_FETCH_REMOTE_DIALOG), parent,
                                      &DialogProcedure, reinterpret_cast<LPARAM>(&state)));
  return result;
}

}  // namespace gc::platform
