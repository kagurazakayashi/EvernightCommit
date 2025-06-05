#include "ui/wx/app_icon.h"

#include <cstring>

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/base64.h>

#include "ui/wx/app_icon_data.h"

namespace gc::ui::wx {

wxIcon MakeAppIcon() {
  // 部分发行版的 wxGTK 不会在启动时自动注册图像处理器（实测 Debian 13 的 3.2.8：
  // 不显式初始化就报「No image handler for type 15」）。这里补一次，且只补一次。
  static const bool handlersInitialized = [] {
    wxInitAllImageHandlers();
    return true;
  }();
  static_cast<void>(handlersInitialized);
  const wxMemoryBuffer decoded = wxBase64Decode(kAppIconPngBase64, std::strlen(kAppIconPngBase64));
  if (decoded.IsEmpty()) {
    return wxIcon();
  }
  wxMemoryInputStream stream(decoded.GetData(), decoded.GetDataLen());
  wxImage image;
  if (!image.LoadFile(stream, wxBITMAP_TYPE_PNG) || !image.IsOk()) {
    return wxIcon();
  }
  wxIcon icon;
  icon.CopyFromBitmap(wxBitmap(image));
  return icon;
}

}  // namespace gc::ui::wx
