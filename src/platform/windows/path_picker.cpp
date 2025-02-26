#include "platform/windows/path_picker.h"

#include <objbase.h>
#include <shobjidl.h>

#include <iterator>

namespace gc::platform {
namespace {

std::optional<std::wstring> OpenWithDialog(HWND owner, std::wstring_view title, std::wstring_view initialPath,
                                           DWORD options, const COMDLG_FILTERSPEC* filters, UINT filterCount) {
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
      dialog == nullptr) {
    return std::nullopt;
  }

  std::optional<std::wstring> result;
  DWORD currentOptions = 0;
  if (SUCCEEDED(dialog->GetOptions(&currentOptions))) {
    dialog->SetOptions(currentOptions | options);
  }

  const std::wstring dialogTitle(title);
  if (!dialogTitle.empty()) {
    dialog->SetTitle(dialogTitle.c_str());
  }

  const std::wstring startPath(initialPath);
  if (!startPath.empty()) {
    IShellItem* folder = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(startPath.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
        folder != nullptr) {
      dialog->SetFolder(folder);
      folder->Release();
    }
  }

  if (filters != nullptr && filterCount > 0) {
    dialog->SetFileTypes(filterCount, filters);
    dialog->SetFileTypeIndex(1);
  }

  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
        result = std::wstring(path);
        ::CoTaskMemFree(path);
      }
      item->Release();
    }
  }

  dialog->Release();
  return result;
}

}  // namespace

std::wstring CurrentWorkingDirectory() {
  const DWORD needed = ::GetCurrentDirectoryW(0, nullptr);
  if (needed == 0) {
    return {};
  }
  std::wstring path(needed, L'\0');
  const DWORD written = ::GetCurrentDirectoryW(needed, path.data());
  if (written == 0 || written >= needed) {
    return {};
  }
  path.resize(written);
  return path;
}

std::optional<std::wstring> BrowseForFolder(HWND owner, std::wstring_view title, std::wstring_view initialPath) {
  return OpenWithDialog(owner, title, initialPath,
                        FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOVALIDATE, nullptr, 0);
}

std::optional<std::wstring> BrowseForExecutable(HWND owner, std::wstring_view title, std::wstring_view initialPath) {
  static const COMDLG_FILTERSPEC kFilters[] = {
      {L"可执行程序 (*.exe)", L"*.exe"},
      {L"所有文件", L"*.*"},
  };
  return OpenWithDialog(owner, title, initialPath, FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST,
                        kFilters, static_cast<UINT>(std::size(kFilters)));
}

}  // namespace gc::platform
