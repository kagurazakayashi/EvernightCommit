#include "ui/controls.h"

#include <commctrl.h>

#include <algorithm>
#include <deque>

namespace gc::ui {
namespace {

constexpr DWORD kChildVisible = WS_CHILD | WS_VISIBLE;

[[nodiscard]] HINSTANCE InstanceOf(HWND parent) noexcept {
  return reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
}

[[nodiscard]] HWND Make(HWND parent, LPCWSTR className, std::wstring_view text, DWORD style, DWORD exStyle, int id) {
  const std::wstring title(text);
  return ::CreateWindowExW(exStyle, className, title.c_str(), style, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), InstanceOf(parent), nullptr);
}

BOOL CALLBACK FontEnumProc(HWND child, LPARAM font) {
  ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
  return TRUE;
}

}  // namespace

HWND CreateLabel(HWND parent, std::wstring_view text, int id) {
  return Make(parent, L"STATIC", text, kChildVisible | SS_LEFTNOWORDWRAP | SS_NOPREFIX, 0, id);
}

HWND CreateCenteredLabel(HWND parent, std::wstring_view text, int id) {
  // SS_CENTER 支持多行绘制；纯中文文案没有可断行的空格，因此空状态说明由文案自带换行符分行。
  return Make(parent, L"STATIC", text, kChildVisible | SS_CENTER | SS_NOPREFIX, 0, id);
}

HWND CreateGroupBox(HWND parent, std::wstring_view text, int id) {
  return Make(parent, L"BUTTON", text, kChildVisible | BS_GROUPBOX, 0, id);
}

HWND CreateSingleLineEdit(HWND parent, std::wstring_view text, int id) {
  return Make(parent, L"EDIT", text, kChildVisible | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, WS_EX_CLIENTEDGE, id);
}

HWND CreateMultilineEdit(HWND parent, std::wstring_view text, int id) {
  return Make(parent, L"EDIT", text,
              kChildVisible | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
              WS_EX_CLIENTEDGE, id);
}

HWND CreatePushButton(HWND parent, std::wstring_view text, int id) {
  return Make(parent, L"BUTTON", text, kChildVisible | WS_TABSTOP | BS_PUSHBUTTON, 0, id);
}

HWND CreateCheckBox(HWND parent, std::wstring_view text, int id, bool checked) {
  HWND box = Make(parent, L"BUTTON", text, kChildVisible | WS_TABSTOP | BS_AUTOCHECKBOX, 0, id);
  ::SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  return box;
}

HWND CreateReportListView(HWND parent, int id, const std::vector<ListColumn>& columns) {
  HWND list = Make(parent, WC_LISTVIEWW, L"", kChildVisible | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
                                                  LVS_NOSORTHEADER,
                   WS_EX_CLIENTEDGE, id);
  if (list == nullptr) {
    return nullptr;
  }
  ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
  for (size_t index = 0; index < columns.size(); ++index) {
    std::wstring title(columns[index].title);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
    column.pszText = title.data();
    column.iSubItem = static_cast<int>(index);
    column.cx = 60;
    ::SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&column));
  }
  return list;
}

HWND CreateDateTimePicker(HWND parent, int id, bool timeOnly) {
  const DWORD format = timeOnly ? DTS_TIMEFORMAT : DTS_SHORTDATECENTURYFORMAT;
  return Make(parent, DATETIMEPICK_CLASSW, L"", kChildVisible | WS_TABSTOP | format, 0, id);
}

HWND CreateEditableCombo(HWND parent, int id) {
  return Make(parent, L"COMBOBOX", L"", kChildVisible | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS,
              0, id);
}

void SetComboCandidates(HWND combo, const std::vector<std::wstring>& items) {
  if (combo == nullptr) {
    return;
  }
  const std::wstring current = GetControlText(combo);
  ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  for (const std::wstring& item : items) {
    ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
  }
  // CB_RESETCONTENT 会连带清空编辑框，这里恢复用户当前文本。
  SetControlText(combo, current);
}

void SetControlText(HWND target, std::wstring_view text) {
  if (target == nullptr) {
    return;
  }
  const std::wstring value(text);
  ::SetWindowTextW(target, value.c_str());
}

std::wstring GetControlText(HWND target) {
  if (target == nullptr) {
    return {};
  }
  const int length = ::GetWindowTextLengthW(target);
  std::wstring value(static_cast<size_t>(length) + 1, L'\0');
  const int written = ::GetWindowTextW(target, value.data(), static_cast<int>(value.size()));
  value.resize(static_cast<size_t>(std::max(0, written)));
  return value;
}

void ApplyListColumnWidths(HWND list, const std::vector<ListColumn>& columns, int width) {
  if (list == nullptr || width <= 0 || columns.empty()) {
    return;
  }
  int totalPercent = 0;
  for (const ListColumn& column : columns) {
    totalPercent += std::max(1, column.percent);
  }
  const int usable = std::max(60, width - 4);
  int used = 0;
  for (size_t index = 0; index < columns.size(); ++index) {
    int cx = static_cast<int>(static_cast<long long>(usable) * std::max(1, columns[index].percent) / totalPercent);
    if (index + 1 == columns.size()) {
      cx = usable - used;
    }
    cx = std::max(24, cx);
    used += cx;
    ::SendMessageW(list, LVM_SETCOLUMNWIDTH, static_cast<WPARAM>(index), static_cast<LPARAM>(cx));
  }
}

void ApplyFontToChildTree(HWND parent, HFONT font) {
  if (parent == nullptr) {
    return;
  }
  ::SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  ::EnumChildWindows(parent, FontEnumProc, reinterpret_cast<LPARAM>(font));
}

ToolTips::~ToolTips() {
  if (window_ != nullptr) {
    ::DestroyWindow(window_);
    window_ = nullptr;
  }
}

bool ToolTips::Create(HWND owner, HFONT font) {
  window_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"", WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr,
                              InstanceOf(owner), nullptr);
  if (window_ == nullptr) {
    return false;
  }
  if (font != nullptr) {
    ::SendMessageW(window_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
  ::SendMessageW(window_, TTM_SETMAXTIPWIDTH, 0, 480);
  ::SendMessageW(window_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
  return true;
}

void ToolTips::Add(HWND target, std::wstring_view text) {
  if (window_ == nullptr || target == nullptr) {
    return;
  }
  texts_.emplace_back(text);
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  info.hwnd = ::GetParent(target);
  info.uId = reinterpret_cast<UINT_PTR>(target);
  info.lpszText = texts_.back().data();
  ::SendMessageW(window_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

}  // namespace gc::ui
