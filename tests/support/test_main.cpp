#include "support/tiny_test.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "platform/windows/command_window_helper.h"

namespace gc::test {
namespace {

// ---------------------------------------------------------------------------
// 验收分组（详见 docs/testing.md 的「测试分组」一节）。
//
// 目的是给「AI 会话可自跑」与「必须用户亲跑」划一条可执行的界线：
//   pure         —— 纯逻辑：不启动任何子进程、不碰真实 Git/文件系统副作用。
//   windows      —— 真实 Win32 集成：启动 cmd.exe/建真实目录与句柄，但绝不启动 Git。
//   git-readonly —— 真实 Git 集成，但逐例核实过「用例体+其调用的夹具辅助函数」都不出现
//                   git commit / git push（init/add/config/fetch/update-ref/hash-object
//                   /merge --no-commit/ls-files/status 等只读或非提交形态允许）。
//   git-mutating —— 夹具内部会执行真实 git commit / git push（含 RemoteRig::Prepare）。
//                   按 AGENTS.md 约束只能由用户运行；本程序绝不代跑。
//   unclassified —— 新出现、尚未登记进下表的文件。保守归法：等同 git-mutating，
//                   只允许全量运行，任何分组选择都不含它，并给出明确警告，
//                   防止「新文件被误当纯逻辑自跑」。
//
// 维护规则（运行器会强制）：
//   1) 新增测试文件必须登记进 kPureFiles / kWindowsFiles / kFixtureFiles 之一，
//      否则落入 unclassified 并在每次运行时报明；
//   2) kVerifiedReadOnlyCases 里的每个名字必须恰好命中一个注册用例，
//      用例改名/删除时必须同步本表，否则启动即失败（防「白名单腐烂」把
//      含提交的用例错放进可自跑组）；
//   3) 判定「只读」的标准是逐行核实用例体与其调用到的文件内辅助函数，
//      注释不可作数；拿不准就留在 git-mutating。
// ---------------------------------------------------------------------------

enum class CaseGroup { pure, windows, gitReadonly, gitMutating, unclassified };

std::string_view GroupLabel(CaseGroup group) {
  switch (group) {
    case CaseGroup::pure:
      return "pure";
    case CaseGroup::windows:
      return "windows";
    case CaseGroup::gitReadonly:
      return "git-readonly";
    case CaseGroup::gitMutating:
      return "git-mutating";
    case CaseGroup::unclassified:
      return "unclassified";
  }
  return "unclassified";
}

bool ParseGroup(std::string_view text, CaseGroup& out) {
  if (text == "pure") {
    out = CaseGroup::pure;
    return true;
  }
  if (text == "windows") {
    out = CaseGroup::windows;
    return true;
  }
  if (text == "git-readonly") {
    out = CaseGroup::gitReadonly;
    return true;
  }
  if (text == "git-mutating") {
    out = CaseGroup::gitMutating;
    return true;
  }
  if (text == "unclassified") {
    out = CaseGroup::unclassified;
    return true;
  }
  return false;
}

constexpr const char* kPureFiles[] = {
    "app_state_tests.cpp",
    "author_config_tests.cpp",
    "command_window_plan_tests.cpp",
    "commit_date_tests.cpp",
    "commit_form_session_tests.cpp",
    "commit_history_tests.cpp",
    "commit_identity_tests.cpp",
    "commit_message_tests.cpp",
    "commit_plan_tests.cpp",
    "diff_view_tests.cpp",
    "environment_block_tests.cpp",  // 仅在本进程内改环境并 RAII 复原；串行运行前提见 docs/testing.md
    "fetch_plan_tests.cpp",
    "fetch_scope_tests.cpp",
    "first_push_plan_tests.cpp",
    "git_environment_tests.cpp",
    "git_locator_tests.cpp",
    "git_probe_tests.cpp",
    "layout_tests.cpp",
    "list_view_memory_tests.cpp",
    "local_time_tests.cpp",
    "machine_output_completeness_tests.cpp",
    "operation_conclusions_tests.cpp",
    "operation_gate_tests.cpp",
    "pull_plan_tests.cpp",
    "push_plan_tests.cpp",
    "repository_tests.cpp",
    "staging_plan_tests.cpp",
    "submodule_journey_tests.cpp",
    "submodule_navigation_tests.cpp",
    "subprocess_capture_tests.cpp",
    "subprocess_command_line_tests.cpp",
    "task_coordinator_tests.cpp",
    "ui_metrics_tests.cpp",
    "undo_commit_plan_tests.cpp",
    "utf_text_tests.cpp",
    "workspace_status_tests.cpp",
};

// 真实 Win32 子进程/文件系统，但逐文件核实过不启动 git.exe。
constexpr const char* kWindowsFiles[] = {
    "command_window_directory_tests.cpp",
    "subprocess_capture_fixture_tests.cpp",
};

// 引用 GitFixture/RemoteRig 的文件；未列入下方只读名单的用例默认含提交/推送。
constexpr const char* kFixtureFiles[] = {
    "author_config_fixture_tests.cpp",
    "command_window_fixture_tests.cpp",
    "commit_history_fixture_tests.cpp",
    "commit_plan_fixture_tests.cpp",
    "diff_view_fixture_tests.cpp",
    "fetch_fixture_tests.cpp",
    "fetch_scope_fixture_tests.cpp",
    "first_push_fixture_tests.cpp",
    "git_environment_fixture_tests.cpp",
    "git_fixture_tests.cpp",
    "pull_probe_fixture_tests.cpp",
    "push_fixture_tests.cpp",
    "refresh_repository_tests.cpp",
    "repo_detect_fixture_tests.cpp",
    "staging_plan_fixture_tests.cpp",
    "submodule_navigation_fixture_tests.cpp",
    "undo_probe_fixture_tests.cpp",
    "workspace_status_fixture_tests.cpp",
};

// 经逐行核实「不执行 git commit / git push」的夹具用例（2026-10-07 验收项 22 的核实记录，
// 证据见 .evernight-local/）。名单只增不减地受运行器保护：任何一条对不上注册用例即启动失败。
constexpr const char* kVerifiedReadOnlyCases[] = {
    // author_config_fixture_tests.cpp —— 只有 init/config 查询
    "author_config_repository_local_wins_over_user_config",
    "author_config_falls_back_to_user_config",
    "author_config_mixes_layers_per_key",
    "author_config_follows_include_directive",
    "author_config_reports_missing_identity",
    "author_config_treats_empty_value_as_missing",
    "author_config_trims_spaces_like_git_does",
    "author_config_failure_is_reported_not_guessed",
    // command_window_fixture_tests.cpp —— 真实命令窗口，全程只读命令
    "command_window_reports_real_failure_exit_code",
    "command_window_delivers_names_to_git_verbatim",
    "command_window_handles_chinese_and_space_paths",
    "command_window_keeps_metacharacter_arguments_literal",
    "command_window_rejects_quote_injection_before_launch",
    "command_window_reports_launch_failure_for_missing_git",
    "command_window_settles_while_console_window_still_open",
    "command_window_reports_git_not_started_without_a_git_exit_code",
    "command_window_running_early_close_settles_terminated",
    "command_window_applies_environment_overrides",
    "command_window_shutdown_does_not_wait_for_git",
    "command_window_rejects_operations_after_shutdown",
    "command_window_works_with_non_ascii_temp_directory",
    "command_window_operation_directories_are_unique_and_reclaimed",
    "command_window_active_operation_survives_stale_sweep",
    "command_window_helper_refuses_foreign_requests",
    // commit_plan_fixture_tests.cpp —— 预检与读回，不含提交
    "commit_plan_fixture_index_lock_is_detected",
    "commit_identity_fixture_reads_the_facts_a_commit_plan_has_to_bind",
    "commit_identity_fixture_index_tree_moves_only_when_the_index_moves",
    "message_file_fixture_writes_utf8_and_is_removed_afterwards",
    // fetch_scope_fixture_tests.cpp A 组 —— init/remote/config/update-ref/fetch 形态
    "fetch_scope_fixture_neutralizes_prune_and_tagopt",
    "fetch_scope_fixture_global_prune_config_is_neutralized",
    "fetch_scope_fixture_refuses_branch_mapping",
    "fetch_scope_fixture_refuses_mirror_clone_mapping",
    "fetch_scope_fixture_allows_filter_and_negative_refspec",
    "fetch_scope_fixture_unreadable_bool_value_is_refused",
    // git_environment_fixture_tests.cpp —— 绑定探针与命令窗口 git add
    "git_env_binding_probe_reads_bind_selected_repo",
    "git_env_binding_command_window_add_binds_selected_repo",
    // repo_detect_fixture_tests.cpp —— 无提交形态的识别
    "detect_empty_repository_as_without_commits",
    "detect_plain_directory_as_not_repository",
    "detect_bare_repository_has_no_workspace",
    // git_fixture_tests.cpp —— 夹具守卫自身
    "fixture_refuses_directories_outside_temp_root",
    "empty_repository_has_unborn_head_and_clean_status",
    "fixture_blocks_network_protocols_before_connecting",
    // diff_view_fixture_tests.cpp —— 首提交前的暂存差异
    "diff_view_real_staged_diff_works_before_initial_commit",
    // staging_plan_fixture_tests.cpp —— 无 HEAD 仓库的暂存/移出
    "unstage_fixture_unborn_repo_leaves_files_untracked_on_disk",
    "unstage_fixture_unborn_index_only_entry_needs_confirmation",
    // workspace_status_fixture_tests.cpp —— 未出生/异常读取形态
    "workspace_status_special_file_names_round_trip",
    "workspace_status_unborn_repository_lists_untracked_only",
    "workspace_status_reports_failure_instead_of_empty_lists",
    // undo_probe_fixture_tests.cpp —— 无提交仓库的预检
    "undo_probe_on_repository_without_commits_blocks_everything",
    // fetch_fixture_tests.cpp —— 远端守卫自身
    "test_remote_guard_rejects_network_forms_and_out_of_root",
    // first_push_fixture_tests.cpp —— 只发 check-ref-format / rev-parse / ls-remote，
    // 不建提交、不推送、不写任何配置（逐行核实于 2026-10-08 首次推送任务）。
    "first_push_ref_format_fixture_verdicts_match_the_contract",
    "first_push_remote_probe_fixture_separates_absent_from_unreachable",
    // submodule_navigation_fixture_tests.cpp —— 只有 init、只读查询与在临时目录里造物
    // （写文件/建目录）：不建提交、不推送、不动索引（2026-10-08 子模块导航任务逐行核实）。
    "submodule_navigation_queries_fixture_are_accepted_by_real_git",
    "submodule_entry_probe_fixture_refuses_paths_that_are_not_gitlinks",
};

bool InTable(const char* const* table, size_t count, std::string_view value) {
  return std::any_of(table, table + count,
                     [value](const char* entry) { return value == std::string_view(entry); });
}

std::string_view BaseName(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

struct RegisteredCase {
  std::string name;
  CaseBody body;
  std::string file;
};

std::vector<RegisteredCase>& Cases() {
  static std::vector<RegisteredCase> cases;
  return cases;
}

int& Failures() {
  static int failures = 0;
  return failures;
}

int& CurrentCaseFailures() {
  static int failures = 0;
  return failures;
}

CaseGroup GroupOf(const RegisteredCase& item) {
  const std::string_view base = BaseName(item.file);
  if (InTable(kPureFiles, std::size(kPureFiles), base)) {
    return CaseGroup::pure;
  }
  if (InTable(kWindowsFiles, std::size(kWindowsFiles), base)) {
    return CaseGroup::windows;
  }
  const bool readOnly = InTable(kVerifiedReadOnlyCases, std::size(kVerifiedReadOnlyCases), item.name);
  if (readOnly) {
    return CaseGroup::gitReadonly;
  }
  if (InTable(kFixtureFiles, std::size(kFixtureFiles), base)) {
    return CaseGroup::gitMutating;
  }
  return CaseGroup::unclassified;
}

// 分组表自审：白名单每条必须恰好命中一个注册用例；命中夹具文件之外的白名单同样按漂移处理
// （只读名单的意义就是「从夹具文件里逐例核实过的例外」）。返回 0 表示表与注册集一致。
int ValidateGroupTables() {
  int problems = 0;
  for (const char* readOnlyName : kVerifiedReadOnlyCases) {
    int hits = 0;
    bool inFixtureFile = false;
    for (const RegisteredCase& item : Cases()) {
      if (item.name == readOnlyName) {
        ++hits;
        inFixtureFile = InTable(kFixtureFiles, std::size(kFixtureFiles), BaseName(item.file));
      }
    }
    if (hits != 1 || !inFixtureFile) {
      std::fprintf(stderr,
                   "分组表漂移：只读名单 \"%s\" 命中 %d 个注册用例（要求恰好 1 个且来自夹具文件）。"
                   "用例改名/删除/搬移时必须同步 test_main.cpp 的 kVerifiedReadOnlyCases。\n",
                   readOnlyName, hits);
      ++problems;
    }
  }
  std::vector<std::string> unclassifiedFiles;
  for (const RegisteredCase& item : Cases()) {
    if (GroupOf(item) == CaseGroup::unclassified &&
        std::find(unclassifiedFiles.begin(), unclassifiedFiles.end(), item.file) ==
            unclassifiedFiles.end()) {
      unclassifiedFiles.push_back(item.file);
    }
  }
  for (const std::string& file : unclassifiedFiles) {
    std::fprintf(stderr,
                 "分组表缺口：文件 %s 有用例未登记（暂按 git-mutating 对待）。"
                 "新测试文件必须归入 test_main.cpp 的分组表之一。\n",
                 file.c_str());
  }
  return problems == 0 ? 0 : 1;
}

struct GroupCounts {
  int pure = 0;
  int windows = 0;
  int gitReadonly = 0;
  int gitMutating = 0;
  int unclassified = 0;
};

GroupCounts CountGroups() {
  GroupCounts counts;
  for (const RegisteredCase& item : Cases()) {
    switch (GroupOf(item)) {
      case CaseGroup::pure:
        ++counts.pure;
        break;
      case CaseGroup::windows:
        ++counts.windows;
        break;
      case CaseGroup::gitReadonly:
        ++counts.gitReadonly;
        break;
      case CaseGroup::gitMutating:
        ++counts.gitMutating;
        break;
      case CaseGroup::unclassified:
        ++counts.unclassified;
        break;
    }
  }
  return counts;
}

void PrintGroupStatsLine(std::string_view prefix) {
  const GroupCounts counts = CountGroups();
  std::fprintf(stdout,
               "%s注册总数 %zu：pure %d / windows %d / git-readonly %d / git-mutating %d / unclassified %d\n",
               prefix.data(), Cases().size(), counts.pure, counts.windows, counts.gitReadonly,
               counts.gitMutating, counts.unclassified);
}

}  // namespace

bool Report(bool passed, const std::string& description, Location location) {
  if (passed) {
    return true;
  }
  ++Failures();
  ++CurrentCaseFailures();
  std::fprintf(stderr, "  断言失败: %s\n    位于 %s:%d\n", description.c_str(), location.file, location.line);
  return false;
}

int FailureCount() { return Failures(); }

Registrar::Registrar(const char* name, CaseBody body, const char* file) {
  Cases().push_back(RegisteredCase{name, body, file});
}

int ListCases(std::string_view nameFilter, std::string_view groupFilter) {
  CaseGroup requested = CaseGroup::unclassified;
  const bool byGroup = !groupFilter.empty() && groupFilter != "all" && ParseGroup(groupFilter, requested);
  int listed = 0;
  for (const RegisteredCase& item : Cases()) {
    if (!nameFilter.empty() && item.name.find(nameFilter) == std::string::npos) {
      continue;
    }
    if (byGroup && GroupOf(item) != requested) {
      continue;
    }
    std::fprintf(stdout, "%s\t%s\n", GroupLabel(GroupOf(item)).data(), item.name.c_str());
    ++listed;
  }
  return listed;
}

int RunAll(std::string_view nameFilter, std::string_view groupFilter) {
  if (const int drift = ValidateGroupTables(); drift != 0) {
    std::fprintf(stdout, "\n分组表校验未通过（%d 处漂移），拒绝在名单与注册集不一致时运行。\n", drift);
    return 3;
  }
  // groupFilter：空或 "all" 表示不分组；其余必须能解析，解析失败同样拒绝运行。
  bool matchGroup = false;
  CaseGroup requested = CaseGroup::pure;
  if (!groupFilter.empty() && groupFilter != "all") {
    if (!ParseGroup(groupFilter, requested)) {
      std::fprintf(stderr,
                   "未知分组 \"%s\"。可用：pure / windows / git-readonly / git-mutating / unclassified / all。\n",
                   std::string(groupFilter).c_str());
      return 3;
    }
    matchGroup = true;
  }

  int matched = 0;
  int failedCases = 0;
  int prerequisiteFailures = 0;
  for (const RegisteredCase& item : Cases()) {
    if (!nameFilter.empty() && item.name.find(nameFilter) == std::string::npos) {
      continue;
    }
    if (matchGroup && GroupOf(item) != requested) {
      continue;
    }
    // 命中 0 绝不报「全通过」：过滤器写错、用例改名、分组表漂移都会在这里现形。
    ++matched;
    CurrentCaseFailures() = 0;
    std::fprintf(stdout, "[ 运行 ] %s（%s）\n", item.name.c_str(),
                 GroupLabel(GroupOf(item)).data());
    std::string abortReason;
    try {
      item.body();
    } catch (const PrerequisiteFailure& failure) {
      // 夹具自身的析构（清理临时目录）在栈展开中照常执行。
      abortReason = failure.what();
    } catch (const std::exception& failure) {
      abortReason = std::string("用例抛出未预期异常: ") + failure.what();
    } catch (...) {
      abortReason = "用例抛出未知异常";
    }
    if (!abortReason.empty()) {
      ++prerequisiteFailures;
      ++failedCases;
      std::fprintf(stdout, "[ 前置失败 ] %s：%s\n", item.name.c_str(), abortReason.c_str());
      continue;
    }
    if (CurrentCaseFailures() == 0) {
      std::fprintf(stdout, "[ 通过 ] %s\n", item.name.c_str());
    } else {
      ++failedCases;
      std::fprintf(stdout, "[ 失败 ] %s（%d 处断言失败）\n", item.name.c_str(), CurrentCaseFailures());
    }
  }
  if (matched == 0) {
    std::fprintf(stdout,
                 "\n过滤器命中 0 个用例（nameFilter=\"%s\" group=\"%s\"）：没有运行任何东西，不算通过。\n",
                 std::string(nameFilter).c_str(),
                 groupFilter.empty() ? "all" : std::string(groupFilter).c_str());
    return 2;
  }
  if (prerequisiteFailures != 0) {
    std::fprintf(stdout,
                 "\n注意：有 %d 个用例因前置条件失败而中止（Git 不可用或临时夹具创建失败等），"
                 "不是断言失败，但同样必须解决后才能视为测试通过。\n",
                 prerequisiteFailures);
  }
  PrintGroupStatsLine("\n");
  std::fprintf(stdout,
               "本次执行 %d 个（组外未执行 %zu 个）：失败 %d 个、前置失败 %d 个、断言失败 %d 处。\n",
               matched, Cases().size() - static_cast<size_t>(matched), failedCases,
               prerequisiteFailures, Failures());
  if (const GroupCounts counts = CountGroups(); counts.unclassified != 0) {
    std::fprintf(stdout, "警告：存在 %d 个未登记用例（unclassified），请把新文件归入 test_main.cpp 分组表。\n",
                 counts.unclassified);
  }
  return failedCases == 0 ? 0 : 1;
}

}  // namespace gc::test

namespace {

void PrintUsage() {
  std::fprintf(
      stderr,
      "用法：gc_tests.exe [--group pure|windows|git-readonly|git-mutating|all] [姓名片段]\n"
      "      gc_tests.exe --list [--group G] [姓名片段]\n"
      "分组含义与运行授权见 docs/testing.md；git-mutating 组含真实 git commit/push，只由用户运行。\n");
}

}  // namespace

int main(int argc, char** argv) {
  // 命令窗口执行器启动的是「当前可执行文件」的辅助模式：测试二进制同样要认这个入口，
  // 集成用例才会走与正式程序完全相同的那条执行链路（新控制台、说明书、CreateProcessW）。
  int helperExitCode = 0;
  if (gc::platform::RunCommandWindowHelperIfRequested(&helperExitCode)) {
    return helperExitCode;
  }
  // 行缓冲：重定向到文件时也能即时看到进度，不会把最后几行憋在块缓冲区里。
  // MSVC 调试版 CRT 要求显式给出缓冲区大小（0 会触发断言对话框）。
  std::setvbuf(stdout, nullptr, _IOLBF, 512);

  std::string_view groupFilter;
  std::string_view nameFilter;
  bool listMode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--list") {
      listMode = true;
      continue;
    }
    if (argument == "--group") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--group 需要一个分组名。\n");
        PrintUsage();
        return 3;
      }
      groupFilter = argv[++i];
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      PrintUsage();
      return 0;
    }
    if (!argument.empty() && argument[0] == '-') {
      std::fprintf(stderr, "未知选项 \"%s\"。\n", std::string(argument).c_str());
      PrintUsage();
      return 3;
    }
    nameFilter = argument;  // 位置参数仍是姓名片段，与旧用法兼容
  }

  if (listMode) {
    const int listed = gc::test::ListCases(nameFilter, groupFilter);
    if (listed == 0) {
      std::fprintf(stderr, "--list 命中 0 个用例：过滤器或分组写错了，空名单不算结果。\n");
      return 2;
    }
    return 0;
  }
  return gc::test::RunAll(nameFilter, groupFilter);
}
