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

git::EmptyStateTexts AppState::WorkspaceHintTexts() const {
  // 讀取失敗時把已歸類的簡短原因作為第二行，界面才不會把「讀不到」顯示成「空倉庫」。
  const std::wstring_view failure = workspace_.status == git::WorkspaceLoadStatus::failed
                                        ? std::wstring_view(git::RepoErrorLabel(workspace_.error))
                                        : std::wstring_view{};
  return git::WorkspaceEmptyTexts(workspace_.status, failure, repo_.detection.headResolved);
}

std::wstring AppState::WorkspaceBanner() const {
  if (workspace_.status != git::WorkspaceLoadStatus::loaded) {
    return {};
  }
  // 只有真的读到子模块变化才占用底部说明；否则保留“哪些按钮尚未接入”的固定提示。
  return git::SubmoduleExplanationText(workspace_.model);
}

}  // namespace gc::app
