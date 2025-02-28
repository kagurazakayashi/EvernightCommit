#include "app/app_state.h"

namespace gc::app {

std::wstring AppState::BranchDisplay() const {
  if (info_.branch.empty()) {
    return L"—（尚未读取仓库信息）";
  }
  return info_.branch;
}

std::wstring AppState::UpstreamDisplay() const {
  if (info_.upstream.empty()) {
    return L"—（尚未读取仓库信息）";
  }
  return info_.upstream;
}

}  // namespace gc::app
