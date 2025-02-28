#pragma once

#include <string>
#include <utility>

#include "git/workspace_model.h"

namespace gc::app {

// “Git 程序”验证生命周期。界面按状态展示具体原因并决定依赖 Git 的功能是否可用。
enum class GitExeStatus {
  unverified,  // 尚无有效路径，或输入未提交验证
  verifying,   // 后台 --version 探测进行中
  verified,    // 路径可用，version 有值
  invalid,     // 探测失败，message 为具体原因
};

struct GitToolState {
  GitExeStatus status = GitExeStatus::unverified;
  std::wstring path;     // 规范化绝对路径（所有 Git 调用统一使用该程序路径）
  std::wstring version;  // verified 时的版本号，如 "2.45.0.windows.1"
  std::wstring message;  // 面向界面的状态/失败说明
};

struct RepoInfo {
  std::wstring repoPath;    // “本地仓库”输入框内容
  std::wstring gitExePath;  // “Git 程序”输入框内容
  std::wstring branch;      // 空表示尚未读取
  std::wstring upstream;    // 空表示尚未读取
};

// 界面与后续服务层之间的状态持有者：界面只读写这里，不直接访问 Git。
class AppState {
public:
  // 依赖 Git 的功能（status/暂存/提交/历史/fetch/pull/push）是否已接通；后续步骤逐项打开。
  // 打开前按钮保持禁用；打开后还要 GitUsable() 才允许触发。
  static constexpr bool kGitOperationsImplemented = false;

  void SetRepoPath(std::wstring path) { info_.repoPath = std::move(path); }
  void SetGitExePath(std::wstring path) { info_.gitExePath = std::move(path); }
  void SetStatusNote(std::wstring note) { statusNote_ = std::move(note); }
  void SetGitTool(GitToolState state) { gitTool_ = std::move(state); }

  [[nodiscard]] const RepoInfo& Info() const noexcept { return info_; }
  [[nodiscard]] const std::wstring& StatusNote() const noexcept { return statusNote_; }
  [[nodiscard]] const GitToolState& Git() const noexcept { return gitTool_; }
  [[nodiscard]] const git::WorkspaceModel& Workspace() const noexcept { return workspace_; }

  // Git 程序经 --version 验证可用；这是后续所有 Git 功能的前置条件。
  [[nodiscard]] bool GitUsable() const noexcept { return gitTool_.status == GitExeStatus::verified; }

  [[nodiscard]] std::wstring BranchDisplay() const;
  [[nodiscard]] std::wstring UpstreamDisplay() const;

private:
  RepoInfo info_;
  std::wstring statusNote_{L"未执行任何操作。"};
  GitToolState gitTool_;
  git::WorkspaceModel workspace_;
};

}  // namespace gc::app
