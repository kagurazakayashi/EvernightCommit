#include "app/app_state.h"

namespace gc::app {

std::wstring AppState::BranchDisplay() const {
  std::wstring branch = git::FormatBranchDisplay(repo_.detection);
  if (!branch.empty()) {
    return branch;
  }
  switch (repo_.status) {
    case RepoLoadStatus::detecting:
      return L"正在识别…";
    case RepoLoadStatus::failed:
      return L"—（仓库未能识别）";
    case RepoLoadStatus::loaded:
      // 识别成功但没有分支概念：裸仓库、.git 内部、非仓库。
      return std::wstring(L"—（") + std::wstring(git::RepoKindLabel(repo_.detection.kind)) + L"）";
    case RepoLoadStatus::unloaded:
      break;
  }
  return L"—（尚未读取仓库信息）";
}

std::wstring AppState::UpstreamDisplay() const {
  std::wstring upstream = git::FormatUpstreamDisplay(repo_.detection);
  if (!upstream.empty()) {
    return upstream;
  }
  if (repo_.status == RepoLoadStatus::detecting) {
    return L"正在识别…";
  }
  if (repo_.status == RepoLoadStatus::failed) {
    return L"—（仓库未能识别）";
  }
  if (repo_.status == RepoLoadStatus::loaded) {
    return L"—（不适用）";
  }
  return L"—（尚未读取仓库信息）";
}

std::wstring AppState::RepoTypeDisplay() const {
  switch (repo_.status) {
    case RepoLoadStatus::unloaded:
      return L"未设置";
    case RepoLoadStatus::detecting:
      return L"正在识别…";
    case RepoLoadStatus::failed:
      return std::wstring(L"未能识别（") + std::wstring(git::RepoErrorLabel(repo_.detection.error)) + L"）";
    case RepoLoadStatus::loaded:
      return std::wstring(git::RepoKindLabel(repo_.detection.kind));
  }
  return L"未设置";
}

}  // namespace gc::app
