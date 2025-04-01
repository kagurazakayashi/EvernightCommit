// 用真实临时仓库驱动生产工作区读取路径（platform::LoadWorkspaceStatus + porcelain v2 解析），
// 断言的是「解析出来的分类、状态与路径」，不是命令文本。覆盖新增、修改、删除、重命名、
// 部分已暂存、特殊文件名、未跟踪目录展开、未合并项与本地子模块（来源只能是本机临时仓库）。
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "git/workspace_model.h"
#include "git/workspace_status.h"
#include "platform/windows/utf_text.h"
#include "platform/windows/workspace_status.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::WorkspaceSnapshot;
using gc::test::GitFixture;

void PrepareFixture(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
}

// 直接调用生产读取路径：夹具只提供隔离执行器，不参与任何解析或分类。
WorkspaceSnapshot Load(GitFixture& fixture, const std::wstring& directory) {
  gc::platform::WorkspaceStatusRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = directory;
  request.timeoutMilliseconds = 20000;
  return gc::platform::LoadWorkspaceStatus(request, fixture.MakeStatusDeps());
}

std::string ToUtf8(const std::wstring& text) { return gc::platform::Utf16ToUtf8(text); }

// 失败诊断用：把 Git 给出的说明与两侧条目数列出来，避免只看到一条断言名字。
std::string Describe(const WorkspaceSnapshot& snapshot) {
  std::string text = ToUtf8(snapshot.message);
  text += "；未暂存 " + std::to_string(snapshot.model.unstaged.size()) +
          " 项，已暂存 " + std::to_string(snapshot.model.staged.size()) + " 项";
  return text;
}

const ChangeItem* Find(const std::vector<ChangeItem>& items, std::wstring_view path) {
  for (const ChangeItem& item : items) {
    if (item.path == path) {
      return &item;
    }
  }
  return nullptr;
}

size_t CountKind(const std::vector<ChangeItem>& items, ChangeKind kind) {
  return static_cast<size_t>(std::count_if(items.begin(), items.end(),
                                           [kind](const ChangeItem& item) { return item.kind == kind; }));
}

// 建立一个有一次提交、内容干净的最小仓库（含一个子目录内的已跟踪文件）。
void InitWithCommit(GitFixture& fixture, std::wstring_view directoryName = L"repo") {
  fixture.InitRepository(directoryName);
  fixture.WriteFile(L"base.txt", "base\n");
  fixture.WriteFile(L"nested/keep.txt", "keep\n");
  fixture.StageAll();
  fixture.Commit(L"基线提交");
}

}  // namespace

GC_TEST(workspace_status_clean_repository_loads_empty_lists) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  GC_CHECK(snapshot.model.unstaged.empty());
  GC_CHECK(snapshot.model.staged.empty());
  GC_CHECK(snapshot.error == gc::git::RepoError::none);
  // 干净仓库的摘要必须说明「读到 0 项」，界面才与“读取失败/尚未读取”区分开。
  GC_CHECK_MESSAGE(snapshot.message.find(L"未暂存 0 项") != std::wstring::npos, ToUtf8(snapshot.message));
}

GC_TEST(workspace_status_classifies_modified_deleted_added_and_untracked) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"base.txt", "base changed\n");                     // 修改（未暂存）
  fixture.RunCheckedInRepo({L"rm", L"-f", L"--quiet", L"--", L"nested/keep.txt"});  // 删除并暂存
  fixture.WriteFile(L"brand-new.txt", "new\n");                         // 新文件
  fixture.Stage({L"brand-new.txt"});                                    //   → 新增（已暂存）
  fixture.WriteFile(L"untracked-dir/deep/a.txt", "a\n");                // 未跟踪目录展开
  fixture.WriteFile(L"untracked-dir/deep/b.txt", "b\n");

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const auto& unstaged = snapshot.model.unstaged;
  const auto& staged = snapshot.model.staged;

  const ChangeItem* modified = Find(unstaged, L"base.txt");
  GC_REQUIRE_MESSAGE(modified != nullptr, "已跟踪文件的改动应归入未暂存");
  GC_CHECK(modified->kind == ChangeKind::modified);
  GC_CHECK_MESSAGE(modified->statusCode == L".M", "工作区侧状态应保留 Git 原文：" + ToUtf8(modified->statusCode));

  // 删除已被 git rm 暂存：只出现在已暂存侧，索引侧状态为 D。
  const ChangeItem* deleted = Find(staged, L"nested/keep.txt");
  GC_REQUIRE_MESSAGE(deleted != nullptr && deleted->kind == ChangeKind::deleted,
                   "git rm 的删除应归入已暂存");
  GC_CHECK_MESSAGE(Find(unstaged, L"nested/keep.txt") == nullptr, "文件已不在工作区，未暂存侧不该再有该条目");

  const ChangeItem* added = Find(staged, L"brand-new.txt");
  GC_REQUIRE_MESSAGE(added != nullptr && added->kind == ChangeKind::added, "暂存后的新文件应归入已暂存");
  GC_CHECK(added->statusCode == L"A.");

  // 未跟踪目录必须展开成可单独选择的文件条目，而不是只列一条目录。
  GC_CHECK_MESSAGE(Find(unstaged, L"untracked-dir/deep/a.txt") != nullptr &&
                       Find(unstaged, L"untracked-dir/deep/b.txt") != nullptr,
                   "未跟踪目录应展开为单个文件");
  GC_CHECK_MESSAGE(Find(unstaged, L"untracked-dir") == nullptr, "不应只列出一个目录条目");
  GC_CHECK_MESSAGE(CountKind(unstaged, ChangeKind::untracked) == 2, "未跟踪条目数应与展开后的文件数一致");
}

GC_TEST(workspace_status_rm_cached_keeps_file_untracked_on_other_side) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  // git rm --cached：索引里删除、工作区文件仍在 —— 同一路径同时属于两个列表。
  fixture.RunCheckedInRepo({L"rm", L"--cached", L"--quiet", L"--", L"base.txt"});

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* stagedDelete = Find(snapshot.model.staged, L"base.txt");
  GC_REQUIRE_MESSAGE(stagedDelete != nullptr && stagedDelete->kind == ChangeKind::deleted,
                     "索引侧应报删除");
  const ChangeItem* untracked = Find(snapshot.model.unstaged, L"base.txt");
  GC_REQUIRE_MESSAGE(untracked != nullptr && untracked->kind == ChangeKind::untracked,
                     "工作区侧同一文件应报未跟踪，而不是消失");
}

GC_TEST(workspace_status_partially_staged_file_shows_on_both_sides) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  fixture.WriteFile(L"base.txt", "first edit\n");
  fixture.Stage({L"base.txt"});                 // 暂存一次修改
  fixture.WriteFile(L"base.txt", "second\n");   // 继续编辑，索引与工作区再次分叉

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* staged = Find(snapshot.model.staged, L"base.txt");
  const ChangeItem* unstaged = Find(snapshot.model.unstaged, L"base.txt");
  GC_REQUIRE_MESSAGE(staged != nullptr && unstaged != nullptr, "同一路径应同时出现在左右两侧");
  GC_CHECK(staged->kind == ChangeKind::modified);
  GC_CHECK(unstaged->kind == ChangeKind::modified);
  GC_CHECK_MESSAGE(staged->statusCode == L"MM" && unstaged->statusCode == L"MM",
                   "两侧来自同一条 XY 记录，状态互不覆盖：" + ToUtf8(staged->statusCode) + "/" +
                       ToUtf8(unstaged->statusCode));
}

GC_TEST(workspace_status_rename_keeps_both_paths_verbatim) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  // 目标名同时含中文、空格与 cmd 元字符：-z 下 Git 不做引号转义，路径应原样回来。
  const std::wstring target = L"nested/保留 &^%().txt";
  fixture.RunCheckedInRepo({L"mv", L"--", L"nested/keep.txt", target});

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* renamed = Find(snapshot.model.staged, target);
  GC_REQUIRE_MESSAGE(renamed != nullptr, "重命名后的路径应出现在已暂存侧：" + ToUtf8(target));
  GC_CHECK(renamed->kind == ChangeKind::renamed);
  GC_CHECK_MESSAGE(renamed->oldPath == L"nested/keep.txt",
                   "来源路径必须原样保留：" + ToUtf8(renamed->oldPath));
  GC_CHECK_MESSAGE(!renamed->similarity.empty() && renamed->similarity[0] == L'R',
                   "相似度字段应保留 Git 原文：" + ToUtf8(renamed->similarity));
  GC_CHECK(renamed->statusCode == L"R.");
  GC_CHECK_MESSAGE(Find(snapshot.model.staged, L"nested/keep.txt") == nullptr,
                   "来源路径不应再单独占一条已暂存条目");
  // 显示用「旧 → 新」，而操作参数只从 path/oldPath 原始字段取。
  GC_CHECK(renamed->PathLabel() == L"nested/keep.txt → nested/保留 &^%().txt");
}

GC_TEST(workspace_status_special_file_names_round_trip) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  const std::vector<std::wstring> names = {
      L"中文 文件.txt",
      L"space  name & ^ % ( ) [ ] .txt",
      L"dots.and-dashes_#1.txt",
      L"点号文件.hidden",
  };
  for (const std::wstring& name : names) {
    fixture.WriteFile(name, "content\n");
  }
  fixture.Stage({names[0]});

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* staged = Find(snapshot.model.staged, names[0]);
  GC_REQUIRE_MESSAGE(staged != nullptr, "暂存的中文路径应原样回来：" + ToUtf8(names[0]));
  GC_CHECK(staged->kind == ChangeKind::added);
  for (size_t index = 1; index < names.size(); ++index) {
    const ChangeItem* item = Find(snapshot.model.unstaged, names[index]);
    GC_CHECK_MESSAGE(item != nullptr, "未跟踪路径应原样回来：" + ToUtf8(names[index]));
    if (item != nullptr) {
      GC_CHECK(item->kind == ChangeKind::untracked);
    }
  }
}

GC_TEST(workspace_status_unmerged_is_conflict_not_plain_delete) {
  GitFixture fixture;
  PrepareFixture(fixture);
  InitWithCommit(fixture);
  // 造一次真实的内容冲突：两条分支改同一行，合并留在冲突现场。
  fixture.RunCheckedInRepo({L"checkout", L"-qb", L"theirs"});
  fixture.WriteFile(L"base.txt", "theirs\n");
  fixture.RunCheckedInRepo({L"commit", L"-qam", L"对方修改"});
  fixture.RunCheckedInRepo({L"checkout", L"-q", L"main"});
  fixture.WriteFile(L"base.txt", "ours\n");
  fixture.RunCheckedInRepo({L"commit", L"-qam", L"本方修改"});
  const auto merge = fixture.RunInRepo({L"merge", L"--no-commit", L"--no-ff", L"theirs"});
  GC_REQUIRE(!merge.Success(), "用例前提：合并应当真的产生冲突");

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* conflict = Find(snapshot.model.unstaged, L"base.txt");
  GC_REQUIRE_MESSAGE(conflict != nullptr, "冲突条目应在未暂存侧");
  GC_CHECK_MESSAGE(conflict->kind == ChangeKind::conflicted,
                   "必须是冲突标识，不能是普通修改或删除：" + ToUtf8(conflict->statusCode));
  GC_CHECK_MESSAGE(conflict->statusCode == L"UU", "未合并记录的 XY 应保留 Git 原文：" +
                                                       ToUtf8(conflict->statusCode));
  GC_CHECK(snapshot.model.HasConflicts());
  GC_CHECK_MESSAGE(Find(snapshot.model.staged, L"base.txt") == nullptr,
                   "未解决前不应把它当成已暂存的正常变化");
  // 摘要要把冲突单独点出来，避免用户以为只是普通的增删。
  GC_CHECK_MESSAGE(snapshot.message.find(L"冲突") != std::wstring::npos, ToUtf8(snapshot.message));
}

GC_TEST(workspace_status_submodule_pointer_and_internal_states) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 子模块来源只能是本机临时仓库：先做一个可引用的本地仓库，再在父仓库登记它。
  fixture.InitRepository(L"child-origin");
  fixture.WriteFile(L"inside.txt", "base\n");
  fixture.StageAll();
  fixture.Commit(L"子模块基线");
  const std::wstring childSource = fixture.RepoDir();

  InitWithCommit(fixture, L"parent");
  fixture.RunCheckedInRepo(
      {L"-c", L"protocol.file.allow=always", L"submodule", L"add", L"--", childSource, L"child"});
  fixture.RunCheckedInRepo({L"commit", L"-qm", L"登记子模块"});
  const std::wstring childDir = fixture.PathInRoot(L"parent/child");

  // 1) 只有内部改动与内部未跟踪文件：提交指针没有移动。
  fixture.WriteFile(L"child/inside.txt", "dirty\n");
  fixture.WriteFile(L"child/scratch.tmp", "untracked\n");

  auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* submodule = Find(snapshot.model.unstaged, L"child");
  GC_REQUIRE_MESSAGE(submodule != nullptr, "子模块变化应在未暂存侧出现一条 gitlink 记录");
  GC_CHECK_MESSAGE(submodule->kind == ChangeKind::submodule,
                   "必须是子模块条目：" + ToUtf8(submodule->statusCode));
  GC_CHECK_MESSAGE(submodule->submodule.trackedChanges, "内部已跟踪文件的改动应被识别");
  GC_CHECK_MESSAGE(submodule->submodule.untrackedChanges, "内部未跟踪文件应被识别");
  GC_CHECK_MESSAGE(!submodule->submodule.commitChanged, "指针未移动时不得报告指针变化");
  // 关键边界：父仓库列表里绝不能出现子模块内部的文件，它们不属于父仓库的暂存范围。
  GC_CHECK_MESSAGE(Find(snapshot.model.unstaged, L"child/inside.txt") == nullptr &&
                       Find(snapshot.model.unstaged, L"child/scratch.tmp") == nullptr,
                   "子模块内部文件不应作为父仓库条目列出");

  // 2) 在子模块里提交：指针移动，父仓库应报告 commitChanged，但仍未暂存。
  fixture.RunChecked({L"add", L"-A"}, childDir);
  fixture.RunChecked({L"commit", L"-qm", L"子模块内部提交"}, childDir);
  snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  submodule = Find(snapshot.model.unstaged, L"child");
  GC_REQUIRE_MESSAGE(submodule != nullptr, "指针移动后仍应有子模块条目");
  GC_CHECK_MESSAGE(submodule->submodule.commitChanged, "子模块 HEAD 移动后父仓库应报告指针变化");
  GC_CHECK(submodule->kind == ChangeKind::submodule);
  GC_CHECK_MESSAGE(snapshot.model.staged.empty(), "父仓库尚未暂存该指针，已暂存侧应为空");

  // 3) 在父仓库暂存子模块：只记录提交指针，条目移到已暂存侧。
  fixture.RunCheckedInRepo({L"add", L"--", L"child"});
  snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  const ChangeItem* stagedPointer = Find(snapshot.model.staged, L"child");
  GC_CHECK_MESSAGE(stagedPointer != nullptr && stagedPointer->kind == ChangeKind::submodule,
                   "暂存子模块应只在已暂存侧留下 gitlink 条目");
  GC_CHECK_MESSAGE(Find(snapshot.model.staged, L"child/inside.txt") == nullptr,
                   "父仓库暂存不得把子模块内部文件当成自己的暂存内容");
  // 界面说明必须解释这个范围限制：内部文件要到子模块工作区另行提交。
  const std::wstring note = gc::git::SubmoduleExplanationText(snapshot.model);
  GC_CHECK_MESSAGE(note.find(L"提交指针") != std::wstring::npos, ToUtf8(note));
  GC_CHECK_MESSAGE(note.find(L"进入子模块") != std::wstring::npos, ToUtf8(note));
}

GC_TEST(workspace_status_unborn_repository_lists_untracked_only) {
  GitFixture fixture;
  PrepareFixture(fixture);
  fixture.InitRepository(L"repo");
  fixture.WriteFile(L"first.txt", "x\n");

  const auto snapshot = Load(fixture, fixture.RepoDir());
  GC_REQUIRE_MESSAGE(snapshot.status == gc::git::WorkspaceLoadStatus::loaded, Describe(snapshot));
  GC_CHECK(snapshot.model.staged.empty());
  GC_CHECK_MESSAGE(CountKind(snapshot.model.unstaged, ChangeKind::untracked) == 1,
                   "无提交仓库也应正常读到未跟踪文件");
}

GC_TEST(workspace_status_reports_failure_instead_of_empty_lists) {
  GitFixture fixture;
  PrepareFixture(fixture);
  // 非仓库目录：读取必须报告原因，而不是给出一份“干净的空列表”。
  const std::wstring plain = fixture.PathInRoot(L"not-a-repo");
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(plain), ec);
  GC_REQUIRE(!ec, "创建非仓库目录失败：" + ec.message());

  const auto snapshot = Load(fixture, plain);
  GC_CHECK(snapshot.status == gc::git::WorkspaceLoadStatus::failed);
  GC_CHECK(snapshot.error != gc::git::RepoError::none);
  GC_CHECK_MESSAGE(snapshot.message.find(L"不是") != std::wstring::npos ||
                       snapshot.message.find(L"失败") != std::wstring::npos,
                   ToUtf8(snapshot.message));
  GC_CHECK(snapshot.model.unstaged.empty() && snapshot.model.staged.empty());
}
