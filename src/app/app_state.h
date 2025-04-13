#pragma once

#include <string>
#include <utility>

#include "git/author_config.h"
#include "git/repository.h"
#include "git/workspace_model.h"
#include "git/workspace_status.h"

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

// 仓库识别生命周期。界面按状态显示“正在识别…”或具体失败原因。
enum class RepoLoadStatus {
  unloaded,  // 尚未发起识别（路径为空）
  detecting, // 后台只读查询进行中
  loaded,    // Git 已回答，仓库形态见 detection.kind
  failed,    // 无法判定，原因见 detection.message
};

struct RepoInfo {
  std::wstring repoPath;    // “本地仓库”输入框内容（绝对路径）
  std::wstring gitExePath;  // “Git 程序”输入框内容
};

// 仓库识别结果：形态、工作区根目录、分支与上游，以及面向界面的说明。
struct RepoState {
  RepoLoadStatus status = RepoLoadStatus::unloaded;
  git::RepoDetection detection;
};

// 「作者默認身份」的讀取生命週期。作者初值取自這個倉庫的有效 Git 配置，由 Git 自己按優先序回答。
enum class AuthorLoadStatus {
  unloaded,  // 還沒有可讀的倉庫
  loading,   // 背景只讀查詢進行中
  loaded,    // Git 已回答（配置是否完整看 config，不完整時由界面提示需要填寫）
  failed,    // 查不到（Git 啟動失敗、設定檔損壞等），原因在 config 的細節裡
};

// 一個倉庫的有效身份讀取結果。界面只讀它來填「作者」與判定提交者身份是否可用，
// 絕不把它寫回任何設定檔——改配置是用戶在 Git 裡的事。
struct AuthorState {
  AuthorLoadStatus status = AuthorLoadStatus::unloaded;
  git::AuthorIdentityConfig config;
};

// 界面与后续服务层之间的状态持有者：界面只读写这里，不直接访问 Git。
class AppState {
public:
  // 依赖 Git 的“写操作”（暂存/提交/撤回/fetch/pull/push）是否已接通；后续步骤逐项打开。
  // 打开前按钮保持禁用；打开后还要 GitUsable() 且 RepoUsable() 并无其他操作在跑才允许触发。
  // 只读路径（仓库识别、工作区读取、手动刷新）不受本开关约束，已各自接入。
  // 剩下的写操作（撤回/fetch/pull/push）仍归本开关：本步骤只单独打开「创建提交」。
  static constexpr bool kGitOperationsImplemented = false;

  // 「创建提交」是否已接通：只影响这一个按钮，
  // 这样后续步骤接「撤回最近提交」时可以单独打开它，不必连带放开 fetch/pull/push。
  static constexpr bool kCreateCommitImplemented = true;

  void SetRepoPath(std::wstring path) { info_.repoPath = std::move(path); }
  void SetGitExePath(std::wstring path) { info_.gitExePath = std::move(path); }
  void SetStatusNote(std::wstring note) { statusNote_ = std::move(note); }
  // 提交表单自己的说明（校验结论、合作者增删的回报、作者默认值的来源）。
  // 它與 statusNote_ 分開兩條通道：後者是「任務/倉庫現在怎麼樣」，會随刷新不断刷新；
  // 表单那句属于使用者正在做的事，不能被紧随其后的读取结论抹掉，也不该反过来盖掉操作结论。
  void SetFormNote(std::wstring note) { formNote_ = std::move(note); }
  void SetGitTool(GitToolState state) { gitTool_ = std::move(state); }
  void SetRepo(RepoState state) { repo_ = std::move(state); }
  // 工作區快照整体替換：切換倉庫或讀取失敗時不會留下上一次的列表內容。
  void SetWorkspace(git::WorkspaceSnapshot snapshot) { workspace_ = std::move(snapshot); }
  // 作者默認身份同樣整體替換：換倉庫後絕不把上個倉庫的配置當成當前默認值。
  void SetAuthor(AuthorState author) { author_ = std::move(author); }

  [[nodiscard]] const RepoInfo& Info() const noexcept { return info_; }
  [[nodiscard]] const std::wstring& StatusNote() const noexcept { return statusNote_; }
  [[nodiscard]] const std::wstring& FormNote() const noexcept { return formNote_; }
  [[nodiscard]] const GitToolState& Git() const noexcept { return gitTool_; }
  [[nodiscard]] const RepoState& Repo() const noexcept { return repo_; }
  [[nodiscard]] const git::WorkspaceSnapshot& Workspace() const noexcept { return workspace_; }
  [[nodiscard]] const git::WorkspaceModel& WorkspaceModel() const noexcept { return workspace_.model; }
  [[nodiscard]] const AuthorState& Author() const noexcept { return author_; }

  // Git 程序经 --version 验证可用；这是后续所有 Git 功能的前置条件。
  [[nodiscard]] bool GitUsable() const noexcept { return gitTool_.status == GitExeStatus::verified; }

  // 仓库已识别且形态允许工作区操作（裸仓库、.git 内部与非仓库都不允许）。
  [[nodiscard]] bool RepoUsable() const noexcept {
    return repo_.status == RepoLoadStatus::loaded && git::KindHasWorkspace(repo_.detection.kind) &&
           !repo_.detection.root.empty();
  }

  [[nodiscard]] std::wstring BranchDisplay() const;
  [[nodiscard]] std::wstring UpstreamDisplay() const;
  [[nodiscard]] std::wstring RepoTypeDisplay() const;
  // 三块列表在“该列没有条目”时的说明：区分无变化、正在读取与读取失败。
  [[nodiscard]] git::EmptyStateTexts WorkspaceHintTexts() const;

  // 已读取成功后返回“工作区状态摘要 + 子模块变化的范围说明”，其余状态返回空串。
  [[nodiscard]] std::wstring WorkspaceBanner() const;

private:
  RepoInfo info_;
  std::wstring statusNote_{L"未执行任何操作。"};
  std::wstring formNote_;
  GitToolState gitTool_;
  RepoState repo_;
  git::WorkspaceSnapshot workspace_;
  AuthorState author_;
};

}  // namespace gc::app
