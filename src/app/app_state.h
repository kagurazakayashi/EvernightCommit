#pragma once

#include <string>
#include <utility>

#include "git/workspace_model.h"

namespace gc::app {

struct RepoInfo {
  std::wstring repoPath;    // “本地仓库”输入框内容
  std::wstring gitExePath;  // “Git 程序”输入框内容
  std::wstring branch;      // 空表示尚未读取
  std::wstring upstream;    // 空表示尚未读取
};

// 界面与后续服务层之间的状态持有者：界面只读写这里，不直接访问 Git。
class AppState {
public:
  // Git 访问层自步骤 2 起接入；在此之前依赖 Git 的按钮一律禁用。
  static constexpr bool kGitAccessConnected = false;

  void SetRepoPath(std::wstring path) { info_.repoPath = std::move(path); }
  void SetGitExePath(std::wstring path) { info_.gitExePath = std::move(path); }
  void SetStatusNote(std::wstring note) { statusNote_ = std::move(note); }

  [[nodiscard]] const RepoInfo& Info() const noexcept { return info_; }
  [[nodiscard]] const std::wstring& StatusNote() const noexcept { return statusNote_; }
  [[nodiscard]] const git::WorkspaceModel& Workspace() const noexcept { return workspace_; }

  [[nodiscard]] std::wstring BranchDisplay() const;
  [[nodiscard]] std::wstring UpstreamDisplay() const;

private:
  RepoInfo info_;
  std::wstring statusNote_{L"未执行任何操作。"};
  git::WorkspaceModel workspace_;
};

}  // namespace gc::app
