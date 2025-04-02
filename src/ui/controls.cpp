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

void ClearListItems(HWND list) {
  if (list != nullptr) {
    ::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
  }
}

int GetListItemCount(HWND list) {
  if (list == nullptr) {
    return 0;
  }
  return static_cast<int>(::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
}

void SetListRowCells(HWND list, int row, const std::vector<std::wstring>& cells) {
  if (list == nullptr || row < 0 || row >= GetListItemCount(list) || cells.empty()) {
    return;
  }
  // ListView 要求 pszText 指向可写缓冲区（控件不会保留指针），这里逐个单元格取本地副本。
  for (size_t column = 0; column < cells.size(); ++column) {
    std::wstring text(cells[column]);
    LVITEMW cell{};
    cell.mask = LVIF_TEXT;
    cell.iItem = row;
    cell.iSubItem = static_cast<int>(column);
    cell.pszText = text.data();
    ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&cell));
  }
}

void InsertListRow(HWND list, int row, const std::vector<std::wstring>& cells) {
  if (list == nullptr || cells.empty()) {
    return;
  }
  const int count = GetListItemCount(list);
  const int at = std::clamp(row, 0, count);  // 越界的插入点夹紧到尾部，控件不会因为算式失准而报错
  std::wstring primary(cells[0]);
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = at;
  item.iSubItem = 0;
  item.pszText = primary.data();
  const int inserted =
      static_cast<int>(::SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
  if (inserted < 0) {
    return;
  }
  for (size_t column = 1; column < cells.size(); ++column) {
    std::wstring text(cells[column]);
    LVITEMW cell{};
    cell.mask = LVIF_TEXT;
    cell.iItem = inserted;
    cell.iSubItem = static_cast<int>(column);
    cell.pszText = text.data();
    ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(inserted), reinterpret_cast<LPARAM>(&cell));
  }
}

void RemoveListRow(HWND list, int row) {
  if (list == nullptr || row < 0 || row >= GetListItemCount(list)) {
    return;
  }
  ::SendMessageW(list, LVM_DELETEITEM, static_cast<WPARAM>(row), 0);
}

std::vector<int> GetListSelectedRows(HWND list) {
  std::vector<int> rows;
  if (list == nullptr) {
    return rows;
  }
  // 多选：从 -1 起逐个取下一个带 LVNI_SELECTED 的行，控件自己维护顺序。
  int row = static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                            MAKELPARAM(LVNI_SELECTED, 0)));
  while (row >= 0) {
    rows.push_back(row);
    row = static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(row),
                                          MAKELPARAM(LVNI_SELECTED, 0)));
  }
  return rows;
}

void RestoreListSelection(HWND list, const std::vector<int>& rows) {
  if (list == nullptr || GetListItemCount(list) == 0) {
    return;
  }
  // iItem = -1 且 stateMask 含 LVIS_SELECTED：清空所有行的选中状态（控件支持这种批量写法）。
  // 真正决定“改哪几位状态”的是 stateMask，只填 mask 不会改动任何状态位。
  LVITEMW clear{};
  clear.stateMask = LVIS_SELECTED;
  clear.state = 0;
  ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&clear));
  const int count = GetListItemCount(list);
  for (const int row : rows) {
    if (row < 0 || row >= count) {
      continue;  // 刷新后条目数变少：这一行已不存在，记忆的其他行照常恢复。
    }
    // 只改 LVIS_SELECTED：不碰 LVIS_STATEIMAGEMASK（那是复选框用的位），也不设焦点，
    // 设焦点会把控件视口拉到该行，覆盖随后要恢复的滚动位置。
    LVITEMW item{};
    item.iItem = row;
    item.stateMask = LVIS_SELECTED;
    item.state = LVIS_SELECTED;
    ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
  }
}

ListRedrawPause::ListRedrawPause(HWND list) : list_(list) {
  if (list_ != nullptr) {
    // 返回 TRUE 表示原本开启，析构时才能只在这里恢复，不把别人关掉的重绘打开。
    paused_ = ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0) == TRUE;
  }
}

ListRedrawPause::~ListRedrawPause() {
  if (paused_) {
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list_, nullptr, TRUE);
  }
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
