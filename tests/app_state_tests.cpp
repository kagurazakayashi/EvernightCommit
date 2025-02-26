#include "support/tiny_test.h"

#include <string>

#include "app/app_state.h"
#include "git/workspace_model.h"

GC_TEST(app_state_exposes_placeholders_not_fake_data) {
  gc::app::AppState state;

  GC_CHECK(!state.Workspace().HasAnyItems());
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

GC_TEST(workspace_empty_state_text_mentions_missing_git_call) {
  const gc::git::EmptyStateTexts texts = gc::git::NotLoadedTexts();
  GC_CHECK(std::wstring(texts.unstaged).find(L"git status") != std::wstring::npos);
  GC_CHECK(std::wstring(texts.staged).find(L"git status") != std::wstring::npos);
  GC_CHECK(std::wstring(texts.history).find(L"git log") != std::wstring::npos);
}
