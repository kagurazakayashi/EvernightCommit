#include "support/tiny_test.h"

#include <string>

#include "app/app_state.h"
#include "git/workspace_model.h"

GC_TEST(app_state_exposes_placeholders_not_fake_data) {
  gc::app::AppState state;

  GC_CHECK(!state.WorkspaceModel().HasAnyItems());
  GC_CHECK(state.Workspace().status == gc::git::WorkspaceLoadStatus::unloaded);
  GC_CHECK(state.Info().repoPath.empty());
  GC_CHECK(state.Info().gitExePath.empty());
  GC_CHECK(state.BranchDisplay().find(L"尚未") != std::wstring::npos);
  GC_CHECK(state.UpstreamDisplay().find(L"尚未") != std::wstring::npos);
  GC_CHECK(!state.StatusNote().empty());
}

GC_TEST(app_state_keeps_browsed_paths) {
  gc::app::AppState state;
  state.SetRepoPath(L"C:\\项目 目录\\仓库");
  state.SetGitExePath(L"C:\\Program Files\\Git\\bin\\git.exe");
  state.SetStatusNote(L"已记录路径。");

  GC_CHECK(state.Info().repoPath == L"C:\\项目 目录\\仓库");
  GC_CHECK(state.Info().gitExePath == L"C:\\Program Files\\Git\\bin\\git.exe");
  GC_CHECK(state.StatusNote() == L"已记录路径。");
}

GC_TEST(app_state_hint_texts_follow_workspace_lifecycle) {
  gc::app::AppState state;
  // 未选择仓库：说明是“尚未选择可用仓库”，不能长得像读取成功的空列表。
  gc::git::EmptyStateTexts texts = state.WorkspaceHintTexts();
  GC_CHECK(texts.unstaged.find(L"尚未选择可用仓库") != std::wstring::npos);
  GC_CHECK(texts.history.find(L"尚未选择可用仓库") != std::wstring::npos);

  gc::git::WorkspaceSnapshot loading;
  loading.status = gc::git::WorkspaceLoadStatus::loading;
  state.SetWorkspace(std::move(loading));
  texts = state.WorkspaceHintTexts();
  GC_CHECK(texts.staged.find(L"正在读取") != std::wstring::npos);

  // 读取失败时把归类原因交给界面（第二行），界面不需要自己拼接文案。
  gc::git::WorkspaceSnapshot failed;
  failed.status = gc::git::WorkspaceLoadStatus::failed;
  failed.error = gc::git::RepoError::gitTimeout;
  state.SetWorkspace(std::move(failed));
  texts = state.WorkspaceHintTexts();
  GC_CHECK(texts.unstaged.find(L"读取失败") != std::wstring::npos);
  GC_CHECK(texts.unstaged.find(gc::git::RepoErrorLabel(gc::git::RepoError::gitTimeout)) !=
           std::wstring::npos);

  // 只有“读取成功且确实有子模块变化”才占用底部说明。
  gc::git::WorkspaceSnapshot loaded;
  loaded.status = gc::git::WorkspaceLoadStatus::loaded;
  gc::git::ChangeItem item;
  item.kind = gc::git::ChangeKind::submodule;
  item.path = L"libs/shared";
  item.submodule.commitChanged = true;
  loaded.model.unstaged.push_back(item);
  state.SetWorkspace(std::move(loaded));
  GC_CHECK(state.WorkspaceBanner().find(L"提交指针") != std::wstring::npos);

  loaded.model.unstaged.clear();
  state.SetWorkspace(std::move(loaded));
  GC_CHECK(state.WorkspaceBanner().empty());
}

GC_TEST(app_state_git_tool_starts_unverified_and_unusable) {
  gc::app::AppState state;

  GC_CHECK(state.Git().status == gc::app::GitExeStatus::unverified);
  GC_CHECK(state.Git().path.empty());
  GC_CHECK(!state.GitUsable());
}

GC_TEST(app_state_git_tool_verified_enables_usage_invalid_disables) {
  gc::app::AppState state;

  gc::app::GitToolState verified;
  verified.status = gc::app::GitExeStatus::verified;
  verified.path = L"C:\\Program Files\\Git\\cmd\\git.exe";
  verified.version = L"2.55.0.windows.5";
  state.SetGitTool(verified);
  GC_CHECK(state.GitUsable());
  GC_CHECK(state.Git().version == L"2.55.0.windows.5");

  gc::app::GitToolState failed;
  failed.status = gc::app::GitExeStatus::invalid;
  failed.path = L"C:\\Windows\\notepad.exe";
  failed.message = L"该程序没有报告 Git 版本，可能不是 git.exe";
  state.SetGitTool(failed);
  GC_CHECK(!state.GitUsable());
  GC_CHECK(state.Git().status == gc::app::GitExeStatus::invalid);

  // 用户改正路径并重新验证通过后可恢复，不要求重启程序。
  state.SetGitTool(verified);
  GC_CHECK(state.GitUsable());
}
