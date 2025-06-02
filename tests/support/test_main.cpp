#include "support/tiny_test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "support/push_guard.h"

#ifdef _WIN32
// 命令窗口辅助入口只在 Windows 那条执行链上存在（见 main() 里的用法）。
#include "platform/windows/command_window_helper.h"
#endif

namespace gc::test {
namespace {

// ---------------------------------------------------------------------------
// 验收分组（详见 docs/testing.md 的「测试分组」一节）。
//
// 目的是给「AI 会话可自跑」与「必须使用者亲跑」划一条可执行的界线：
//   pure         —— 纯逻辑：不启动任何子进程、不碰真实 Git/文件系统副作用。
//   windows      —— 真实 Win32 集成：启动 cmd.exe/建真实目录与句柄，但绝不启动 Git。
//   linux        —— 真实 Linux 平台基础服务：文件系统/时区/环境变量与后台执行器
//                   （只启动测试自选的系统程序），但绝不启动 Git。
//   git-readonly —— 真实 Git 集成，但逐例核实过「用例体+其调用的夹具辅助函数」都不出现
//                   git commit / git push（init/add/config/fetch/update-ref/hash-object
//                   /merge --no-commit/ls-files/status 等只读或非提交形态允许）。
//   git-commit   —— 会在用例自有的隔离临时仓库里执行真实 git commit，但逐例核实不出现
//                   git push。本轮授权允许 AI 会话跑这一组（隔离仓库、不联网、不动真实仓库）。
//   git-push     —— 会执行 git push（直接写 push、调 GitFixture::Push、用 RemoteRig，
//                   或把 push 形态的方案参数原样交给真实 Git）。按 AGENTS.md 与本轮授权
//                   一律禁止自动运行：没有使用者本人放行就跑不起来，见 support/push_guard.h。
//   unclassified —— 新出现、尚未登记进下表的文件。保守归法：等同 git-push，
//                   任何分组选择与默认运行都不含它，并给出明确警告，
//                   防止「新文件被误当纯逻辑自跑」。
//
// 默认（不写 --group）= 上面前五组，即「安全集」；git-push 与 unclassified 一律不跑，
// 并在汇总里报明被排除的数量——不再有「一次全量把提交和推送混在一起」这个入口。
//
// 维护规则（运行器会强制）：
//   1) 新增测试文件必须登记进 kPureFiles / kWindowsFiles / kLinuxFiles / kFixtureFiles 之一，
//      否则落入 unclassified 并在每次运行时报明；
//   2) kVerifiedReadOnlyCases 与 kVerifiedCommitCases 里的每个名字必须恰好命中一个注册用例，
//      用例改名/删除时必须同步本表，否则启动即失败（防「白名单腐烂」把
//      含推送的用例错放进可自跑组）；两张表不得有同名条目；
//   3) 判定「只读」「只提交」的标准是逐行核实用例体与其调用到的文件内辅助函数，
//      注释不可作数；拿不准就留在 git-push（禁止自动运行那一侧）。
//   4) 表只是选择依据，不是安全边界：真正的边界是夹具与命令窗口里的 push 闸门。
//      归类错了会在执行时被拦下并记成「拒绝 push」，运行以退出码 4 结束，不会当成通过。
// ---------------------------------------------------------------------------

enum class CaseGroup { pure, windows, linux, gitReadonly, gitCommit, gitPush, unclassified };

std::string_view GroupLabel(CaseGroup group) {
  switch (group) {
    case CaseGroup::pure:
      return "pure";
    case CaseGroup::windows:
      return "windows";
    case CaseGroup::linux:
      return "linux";
    case CaseGroup::gitReadonly:
      return "git-readonly";
    case CaseGroup::gitCommit:
      return "git-commit";
    case CaseGroup::gitPush:
      return "git-push";
    case CaseGroup::unclassified:
      return "unclassified";
  }
  return "unclassified";
}

// 可自动运行（AI 会话与默认入口都只碰这些组）。
bool AutoRunnable(CaseGroup group) {
  return group == CaseGroup::pure || group == CaseGroup::windows || group == CaseGroup::linux ||
         group == CaseGroup::gitReadonly || group == CaseGroup::gitCommit;
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
  if (text == "linux") {
    out = CaseGroup::linux;
    return true;
  }
  if (text == "git-readonly") {
    out = CaseGroup::gitReadonly;
    return true;
  }
  if (text == "git-commit") {
    out = CaseGroup::gitCommit;
    return true;
  }
  if (text == "git-push") {
    out = CaseGroup::gitPush;
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
    "commit_changes_tests.cpp",
    "commit_date_tests.cpp",
    "commit_detail_tests.cpp",
    "commit_form_session_tests.cpp",
    "commit_history_tests.cpp",
    "commit_identity_tests.cpp",
    "commit_message_tests.cpp",
    "commit_plan_tests.cpp",
    "conflict_state_tests.cpp",
    "decimal_text_tests.cpp",
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
    "main_thread_dispatch_tests.cpp",
    "local_time_tests.cpp",
    "machine_output_completeness_tests.cpp",
    "operation_conclusions_tests.cpp",
    "operation_gate_tests.cpp",
    "operation_history_tests.cpp",
    "persistent_state_tests.cpp",
    "pull_plan_tests.cpp",
    "push_guard_tests.cpp",  // push 闸门自身的判据（纯逻辑，两个平台都编）。
    "push_plan_tests.cpp",
    "repository_tests.cpp",
    "restore_plan_tests.cpp",
    "staging_plan_tests.cpp",
    "submodule_journey_tests.cpp",
    "shell_text_tests.cpp",
    "submodule_navigation_tests.cpp",
    "subprocess_capture_tests.cpp",
    "subprocess_command_line_tests.cpp",
    "task_coordinator_tests.cpp",
    "text_encoding_tests.cpp",
    "ui_metrics_tests.cpp",
    "undo_commit_plan_tests.cpp",
    "utf_text_tests.cpp",
    "workspace_status_tests.cpp",
};

// 真实 Win32 子进程/文件系统，但逐文件核实过不启动 git.exe。
constexpr const char* kWindowsFiles[] = {
    "changes_pane_tests.cpp",  // 真实窗口上的变更区面板：空状态说明的显隐与清单行是否真的落地。不起子进程、不碰文件系统。
    "command_window_directory_tests.cpp",
    "operation_history_store_file_tests.cpp",  // 真实历史文件读写/锁与剪贴板往返，全部在测试自己的临时根；不启动 Git、不碰用户 %APPDATA%。
    "persistent_store_file_tests.cpp",  // 真实文件读写/锁，全部在测试自己的临时根里；不启动 Git、不碰用户 %APPDATA%。
    "subprocess_capture_fixture_tests.cpp",
    "upstream_write_flow_tests.cpp",  // UI 层控制器的接线断言（假宿主，不起子进程、不启动 Git、不碰文件系统）。
    "win_path_probe_tests.cpp",  // 真实文件系统上的预览预检（存在性/可读性/二进位），只用测试自己的临时目录；不启动 Git。
    "list_redraw_tests.cpp",  // 真实窗口上的消息级断言：重绘暂停必须恢复、布局与文本设置不重复发消息。不起子进程、不碰文件系统。
    "commit_form_time_tests.cpp",  // 真实窗口上的提交表单时间控件：联动取的是看得见的那一半。不起子进程、不碰文件系统。
};

// 真实 Linux 平台基础服务：只在 Linux 目标里编译。碰真实文件系统/时区/环境变量，
// 但绝不启动任何子进程（包括 git），也不联网、不涉及 push。
constexpr const char* kLinuxFiles[] = {
    "linux_environment_tests.cpp",
    "linux_git_toolchain_tests.cpp",
    "linux_local_time_tests.cpp",
    "linux_path_probe_tests.cpp",
    "linux_scratch_file_tests.cpp",
    "linux_subprocess_tests.cpp",
};

// 引用 GitFixture/RemoteRig 的文件；未列入下方两张名单的用例默认含提交/推送，
// 其中「含推送」是禁止自动运行那一侧。
constexpr const char* kFixtureFiles[] = {
    "author_config_fixture_tests.cpp",
    "commit_changes_fixture_tests.cpp",
    "commit_detail_fixture_tests.cpp",
    "command_window_fixture_tests.cpp",
    "conflict_fixture_tests.cpp",
    "commit_history_fixture_tests.cpp",
    "commit_plan_fixture_tests.cpp",
    "diff_view_fixture_tests.cpp",
    "fetch_fixture_tests.cpp",
    "fetch_scope_fixture_tests.cpp",
    "first_push_fixture_tests.cpp",
    "git_environment_fixture_tests.cpp",
    "git_fixture_tests.cpp",
    "linux_git_query_tests.cpp",
    "pull_probe_fixture_tests.cpp",
    "push_fixture_tests.cpp",
    "refresh_repository_tests.cpp",
    "restore_plan_fixture_tests.cpp",
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
    // restore_plan_fixture_tests.cpp —— 只有 init + 一条只读 rev-parse 预检形态核对，
    // 不建提交、不推送、不动任何引用（2026-10-09 操作历史任务逐行核实）。
    "restore_branch_probe_accepts_the_query_shape_on_a_fresh_repository",
    // R2：符号引用探测与 worktree list 两条只读查询的真实形态核对（都不改动仓库）。
    "restore_ref_integrity_queries_match_real_git_contract",
    // R4（本轮）：逐行核实过——用例体只调用 GitFixture::SeedCommitObject（write-tree + hash-object，
    // 夹具注释写明它不执行 git commit/git push、不动 HEAD），以及 init / add（StageAll）/
    // update-ref / symbolic-ref / rev-parse / ls-files / status / for-each-ref /
    // check-ref-format / worktree list / `worktree add -b`（建 linked worktree：不建提交、不推送、
    // 不联网，远端相关的一条都没用）。没有 commit、没有 push。
    // 四条分别是：引用名闸门与 Git 裁定的口径一致性、程序参数数组的实际效果与越界断言、
    // 符号引用（含符号链）的拒绝与「背后分支一动不动」、linked worktree 占用由真实输出判出。
    "undo_restore_refname_gate_matches_gits_authoritative_verdict",
    "restore_update_ref_forms_come_from_the_program_and_touch_nothing_else",
    "restore_refuses_symbolic_refs_and_never_moves_the_branch_behind_them",
    "restore_refuses_branches_checked_out_in_a_linked_worktree_on_real_git",
    // 2026-10-11（可复制命令收口）：逐行核实——用例体只用 init / WriteFile / StageAll（add）/
    // SeedCommitObject（write-tree + hash-object，不执行 commit）/ update-ref / rev-parse，
    // 没有任何 commit、push、联网或远端相关调用。
    "restore_program_arguments_for_shell_significant_names_reach_git_intact",
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
    // commit_changes_fixture_tests.cpp —— 逐行核实：init / WriteFile / StageAll(add) /
    // SeedCommitObject（write-tree + hash-object，夹具注释写明不执行 git commit）/ git show
    // <对象> --name-status 与 git show <对象> -- <pathspec>（两条都是只读查询）。
    // 没有任何 commit、push、联网或引用移动，故 AI 侧可自行运行核验。
    "commit_changes_seeded_object_reports_real_changes_without_commit",
    // commit_detail_fixture_tests.cpp —— 逐行核实：init / WriteFile / StageAll(add) /
    // write-tree / commit-tree（plumbing：造一枚提交对象，不移动 HEAD、不动索引与工作区，
    // 与夹具的 SeedCommitObject 同一形态）/ 生产参数数组交给 git log -n 1 只读查询，
    // 以及 platform::LoadCommitDetail 用夹具隔离执行器跑同一条查询。
    // 全程没有 commit、push、引用移动或联网，故 AI 侧可自行运行核验。
    "commit_detail_arguments_run_on_real_git_and_split_into_seven_fields",
    "commit_detail_platform_load_reads_the_selected_commit",
    "commit_detail_distinguishes_no_description_from_unreadable_object",
    // conflict_fixture_tests.cpp —— 只有 init、只读查询与在临时目录里造物（建目录/写痕迹档案/
    // 写工作区文件但不 add）：不建提交、不推送、不动索引、不改任何配置（逐行核实于 2026-10-08
    // 「冲突处理入口」任务；文件里其余用例都要提交图，按是否写远端归进 git-commit 或 git-push）。
    "conflict_probe_fixture_accepts_the_query_shapes_on_a_fresh_repository",
    "conflict_probe_fixture_separates_unreadable_git_dir_from_no_markers",
};

// Linux 真实 Git 集成：用例体逐行核实过「只 init/config/add/只读查询」，
// 不执行 git commit / git push，也不联网（夹具环境 GIT_ALLOW_PROTOCOL=file 且无远端）。
constexpr const char* kLinuxVerifiedReadOnlyCases[] = {
    "linux_git_query_commit_probe_collects_real_facts",
    "linux_git_query_environment_policy_disables_interaction_and_paging",
    "linux_git_query_incomplete_commit_probe_is_not_approved",
    "linux_git_query_incomplete_output_is_a_failure_not_empty_status",
    "linux_git_query_launch_failure_is_classified",
    "linux_git_query_switch_repository_discards_older_background_result",
    "linux_git_query_timeout_is_classified_as_timeout",
    "linux_git_query_worker_cancel_stops_inflight_child",
};

// Linux 真实 Git 集成：只在用例自有的 PosixTempDirectory 里执行真实 git commit，
// 不执行 git push、不联网；提交身份与配置全部隔离在临时根内。
constexpr const char* kLinuxVerifiedCommitCases[] = {
    "linux_git_query_detects_repository_shapes",
    "linux_git_query_reads_real_status_history_author_detail_and_changes",
};

// 逐例核实过「在自有隔离临时仓库里执行真实 git commit，但不执行 git push」的夹具用例
// （2026-10-17 本轮按当次授权新增：允许测提交，不允许任何推送）。
//
// 判据：用例体与它调用到的文件内辅助函数里，既没有 RemoteRig（其 Prepare 会 push -u）、
// 也没有 GitFixture::Push、更没有把 push 形态的方案参数交给真实 Git；
// 出现在 push_fixture_tests.cpp / first_push_fixture_tests.cpp 这类推送域文件里的用例，
// 只要碰到 BuildPushPlan/BuildFirstPushPlan 的产物参数就一律不进本表。
// 拿不准的留在 git-push（禁止自动运行那一侧）。本表与上表不得有同名条目，运行器会查。
constexpr const char* kVerifiedCommitCases[] = {
    "author_config_query_does_not_touch_the_repository",
    "batch_fixture_add_all_reaches_every_change_but_ignored_paths",
    "batch_fixture_unstage_all_in_unborn_repo_uses_only_index_reset",
    "batch_fixture_unstage_all_refuses_while_a_conflict_is_still_pending",
    "batch_fixture_unstage_all_returns_index_to_head_and_keeps_worktree",
    "command_window_early_close_never_stays_running",
    "command_window_reports_real_success_exit_code",
    "commit_changes_real_empty_commit_lists_nothing_but_succeeds",
    "commit_changes_real_file_diff_plan_does_not_over_match",
    "commit_changes_real_lists_modified_added_deleted_and_rename",
    "commit_changes_real_merge_commit_reports_first_parent_changes",
    "commit_changes_real_missing_object_is_a_read_failure",
    "commit_changes_real_rejects_untrusted_object_id_before_running",
    "commit_history_fixture_capped_at_display_limit",
    "commit_history_fixture_detached_head_still_readable",
    "commit_history_fixture_empty_repository_skips_log",
    "commit_history_fixture_merge_commit_recognized",
    "commit_history_fixture_orders_and_fields_match_real_git",
    "commit_history_fixture_refresh_picks_up_new_commits",
    "commit_history_fixture_show_plan_resolves_in_real_repo",
    "commit_identity_fixture_voids_confirmed_plan_after_a_merge_starts",
    "commit_identity_fixture_voids_confirmed_plan_on_branch_switch_at_same_head",
    "commit_identity_fixture_voids_confirmed_plan_when_staged_content_swaps",
    "commit_plan_fixture_hook_rejection_creates_no_commit",
    "commit_plan_fixture_real_commit_records_identities_times_and_scope",
    "commit_plan_fixture_refuses_real_unresolved_merge_conflict",
    "commit_timestamps_are_deterministic",
    "conflict_continue_fixture_creates_a_merge_commit_and_clears_markers",
    "conflict_continue_fixture_keeps_the_scene_when_a_hook_refuses",
    "conflict_state_fixture_reads_a_real_merge_conflict",
    "conflict_state_fixture_reads_a_real_rebase_with_companion_markers",
    "conflict_state_fixture_separates_rebase_apply_from_git_am",
    "detect_detached_head",
    "detect_inside_git_directory",
    "detect_linked_worktree",
    "detect_plain_repository_reports_branch_and_no_upstream",
    "detect_submodule_working_tree",
    "detect_walks_up_from_subdirectory",
    "diff_view_real_conflicted_item_yields_combined_diff",
    "diff_view_real_deleted_added_and_rename",
    "diff_view_real_literal_pathspec_does_not_over_match",
    "diff_view_real_preview_leaves_repository_untouched",
    "diff_view_real_submodule_shows_pointer_only",
    "diff_view_real_two_sides_show_different_content",
    "diff_view_real_untracked_content_and_binary_notice",
    "executed_plan_is_rejected_when_the_branch_moved_after_confirmation",
    "executing_normal_undo_creates_branch_reflog_even_when_logging_is_off",
    "executing_normal_undo_keeps_index_bytes_and_staged_changes_together",
    "executing_root_commit_undo_leaves_unborn_branch_with_contents_staged",
    "fetch_blocked_without_any_remote_and_leaves_no_config",
    "first_push_config_write_steps_stay_in_local_config_with_worktree_config",
    "first_push_targets_fixture_lists_configured_remotes",
    "fixture_hides_user_git_configuration",
    "fixture_temp_root_is_created_and_cleaned_up",
    "git_env_identity_commit_landing_uses_form_and_config_only",
    "index_lock_failure_is_named_and_the_lock_file_is_left_alone",
    "initial_commit_creates_real_objects",
    "late_result_from_previous_repository_is_discarded",
    "merge_commit_undo_targets_first_parent_and_forces_confirmation",
    "operation_refresh_after_nonzero_exit_still_reads_the_repository",
    "pull_preflight_blocked_without_upstream_and_writes_nothing",
    "read_failure_lands_as_failure_and_recovers_on_next_refresh",
    "real_conflict_and_detached_head_are_refused",
    "refresh_picks_up_changes_made_outside_the_program",
    "remote_bare_origin_marks_commit_as_known_published",
    "restore_symref_update_ref_does_not_follow",
    "restore_update_ref_fixture_moves_branch_back_and_rejects_stale_old",
    "second_commit_links_to_first_as_parent",
    "shallow_clone_depth_one_is_refused_and_keeps_the_branch",
    "shallow_clone_depth_two_undoes_with_force_and_keeps_the_index",
    "staging_fixture_conflicted_path_is_marked_resolved",
    "staging_fixture_many_files_in_one_command",
    "staging_fixture_missing_path_fails_without_partial_stage",
    "staging_fixture_old_git_inline_mode_reaches_same_index",
    "staging_fixture_re_add_updates_index_to_latest_worktree",
    "staging_fixture_selected_only_leaves_unselected_changes_untouched",
    "staging_fixture_special_paths_stay_literal",
    "staging_fixture_submodule_records_pointer_only",
    "staging_fixture_worktree_rename_requires_both_entries",
    "submodule_entry_probe_fixture_allows_real_submodule_and_flags_its_state",
    "submodule_entry_probe_fixture_refuses_uninitialized_clone",
    "submodule_entry_probe_fixture_separates_moved_from_uninitialized",
    "submodule_navigation_probes_do_not_touch_the_repositories",
    "submodule_pointer_states_fixture_from_a_real_submodule",
    "undo_binds_the_confirmed_branch_ref_not_the_mutable_head",
    "undo_probe_reports_real_branch_head_parents_and_summary",
    "unstage_fixture_head_repo_returns_index_to_head_and_keeps_worktree",
    "unstage_fixture_many_selected_paths_in_one_command",
    "unstage_fixture_old_git_inline_mode_reaches_same_index",
    "unstage_fixture_rename_restores_both_paths_at_once",
    "unstage_fixture_rename_single_path_would_leave_half_state",
    "unstage_fixture_reset_form_reaches_the_same_index_when_head_exists",
    "unstage_fixture_special_paths_stay_literal",
    "unstage_fixture_submodule_pointer_returns_to_head",
    "unstage_fixture_vanished_path_fails_without_partial_unstage",
    "working_tree_distinguishes_unstaged_staged_and_untracked",
    "workspace_status_classifies_modified_deleted_added_and_untracked",
    "workspace_status_clean_repository_loads_empty_lists",
    "workspace_status_partially_staged_file_shows_on_both_sides",
    "workspace_status_rename_keeps_both_paths_verbatim",
    "workspace_status_rm_cached_keeps_file_untracked_on_other_side",
    "workspace_status_submodule_pointer_and_internal_states",
    "workspace_status_unmerged_is_conflict_not_plain_delete",
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
  if (InTable(kLinuxFiles, std::size(kLinuxFiles), base)) {
    return CaseGroup::linux;
  }
  if (InTable(kVerifiedReadOnlyCases, std::size(kVerifiedReadOnlyCases), item.name) ||
      InTable(kLinuxVerifiedReadOnlyCases, std::size(kLinuxVerifiedReadOnlyCases), item.name)) {
    return CaseGroup::gitReadonly;
  }
  if (InTable(kVerifiedCommitCases, std::size(kVerifiedCommitCases), item.name) ||
      InTable(kLinuxVerifiedCommitCases, std::size(kLinuxVerifiedCommitCases), item.name)) {
    return CaseGroup::gitCommit;
  }
  // 夹具文件里没进上面两张名单的用例，以及没登记的新文件，一律落到「禁止自动运行」那一侧。
  if (InTable(kFixtureFiles, std::size(kFixtureFiles), base)) {
    return CaseGroup::gitPush;
  }
  return CaseGroup::unclassified;
}

// 校验一张用例名名单：每条必须恰好命中一个注册用例，且该用例来自夹具文件
// （名单的意义就是「从夹具文件里逐例核实过的例外」）。只有真的编译了夹具用例的平台才用得上。
int ValidateNameTable(const char* const* table, size_t count, const char* tableName) {
  int problems = 0;
  for (size_t i = 0; i < count; ++i) {
    const char* caseName = table[i];
    int hits = 0;
    bool inFixtureFile = false;
    for (const RegisteredCase& item : Cases()) {
      if (item.name == caseName) {
        ++hits;
        inFixtureFile = InTable(kFixtureFiles, std::size(kFixtureFiles), BaseName(item.file));
      }
    }
    if (hits != 1 || !inFixtureFile) {
      std::fprintf(stderr,
                   "分组表漂移：%s 里的 \"%s\" 命中 %d 个注册用例（要求恰好 1 个且来自夹具文件）。"
                   "用例改名/删除/搬移时必须同步 test_main.cpp 的这张表。\n",
                   tableName, caseName, hits);
      ++problems;
    }
  }
  return problems;
}

// 分组表自审。返回 0 表示表与注册集一致。
int ValidateGroupTables() {
  int problems = 0;
#ifdef _WIN32
  problems += ValidateNameTable(kVerifiedReadOnlyCases, std::size(kVerifiedReadOnlyCases),
                                "kVerifiedReadOnlyCases");
  problems += ValidateNameTable(kVerifiedCommitCases, std::size(kVerifiedCommitCases),
                                "kVerifiedCommitCases");
  for (const char* commitName : kVerifiedCommitCases) {
    if (InTable(kVerifiedReadOnlyCases, std::size(kVerifiedReadOnlyCases), commitName)) {
      std::fprintf(stderr, "分组表漂移：\"%s\" 同时出现在只读名单与提交名单里。\n", commitName);
      ++problems;
    }
  }
#else
  // Linux 目标现在编译了真实 Git 夹具（support/linux_git_fixture.cpp）。
  problems += ValidateNameTable(kLinuxVerifiedReadOnlyCases, std::size(kLinuxVerifiedReadOnlyCases),
                                "kLinuxVerifiedReadOnlyCases");
  problems += ValidateNameTable(kLinuxVerifiedCommitCases, std::size(kLinuxVerifiedCommitCases),
                                "kLinuxVerifiedCommitCases");
  for (const char* commitName : kLinuxVerifiedCommitCases) {
    if (InTable(kLinuxVerifiedReadOnlyCases, std::size(kLinuxVerifiedReadOnlyCases), commitName)) {
      std::fprintf(stderr, "分组表漂移：\"%s\" 同时出现在只读名单与提交名单里。\n", commitName);
      ++problems;
    }
  }
#endif
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
                 "分组表缺口：文件 %s 有用例未登记（暂按 git-push 对待，任何自动入口都不跑它）。"
                 "新测试文件必须归入 test_main.cpp 的分组表之一。\n",
                 file.c_str());
  }
  return problems == 0 ? 0 : 1;
}

struct GroupCounts {
  int pure = 0;
  int windows = 0;
  int linux = 0;
  int gitReadonly = 0;
  int gitCommit = 0;
  int gitPush = 0;
  int unclassified = 0;

  int Total() const {
    return pure + windows + linux + gitReadonly + gitCommit + gitPush + unclassified;
  }
  // 可自动运行那一侧（默认入口与 --group safe 的范围）。
  int AutoRunnable() const { return pure + windows + linux + gitReadonly + gitCommit; }
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
      case CaseGroup::linux:
        ++counts.linux;
        break;
      case CaseGroup::gitReadonly:
        ++counts.gitReadonly;
        break;
      case CaseGroup::gitCommit:
        ++counts.gitCommit;
        break;
      case CaseGroup::gitPush:
        ++counts.gitPush;
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
               "%s注册总数 %d：pure %d / windows %d / linux %d / git-readonly %d / git-commit %d / "
               "git-push %d / unclassified %d\n",
               prefix.data(), static_cast<int>(Cases().size()), counts.pure, counts.windows,
               counts.linux, counts.gitReadonly, counts.gitCommit, counts.gitPush,
               counts.unclassified);
}

// 一次运行的分组选择结果：跑哪些、被排除多少、要不要放行 push。
enum class SelectMode { safe, single, everything };

struct RunSelection {
  SelectMode mode = SelectMode::safe;
  CaseGroup requested = CaseGroup::pure;
};

bool Selected(const RunSelection& selection, CaseGroup group) {
  switch (selection.mode) {
    case SelectMode::safe:
      return AutoRunnable(group);
    case SelectMode::everything:
      return true;
    case SelectMode::single:
      break;
  }
  return group == selection.requested;
}

// 禁止自动运行那一侧的组要点名放行：既要 --group 指到它，也要环境里带 GC_ALLOW_PUSH=1。
// 只满足一半就当没放行——少一道确认就少一次误跑。返回 false 时 exitCode 已写好原因。
bool PushRunAuthorized(const char* what, int& exitCode) {
  const char* grant = std::getenv("GC_ALLOW_PUSH");
  if (grant != nullptr && std::string_view(grant) == "1") {
    return true;
  }
  const GroupCounts counts = CountGroups();
  std::fprintf(stderr,
               "拒绝运行 %s：这些用例会在隔离夹具里执行真实 git push。\n"
               "  本轮授权允许测 commit、不允许任何 push；推送一律由使用者本人在自己终端里跑。\n"
               "  数量：git-push %d 个、unclassified %d 个（未登记的按禁止侧对待）。\n"
               "  确实要由使用者本人执行时：设 GC_ALLOW_PUSH=1 再点名这一组。\n",
               what, counts.gitPush, counts.unclassified);
  exitCode = 4;
  return false;
}

// 把 --group 文本翻成选择，并据此设定 push 闸门。
// 返回 false 表示应当拒绝启动（未知分组，或点了禁止组却没带放行凭据）。
bool ResolveSelection(std::string_view groupFilter, RunSelection& out, int& exitCode) {
  if (groupFilter.empty() || groupFilter == "safe") {
    out.mode = SelectMode::safe;
    return true;
  }
  if (groupFilter == "all") {
    if (!PushRunAuthorized("\"全部用例\"（含会推送远端的一组）", exitCode)) {
      return false;
    }
    out.mode = SelectMode::everything;
    return true;
  }
  CaseGroup group = CaseGroup::pure;
  if (!ParseGroup(groupFilter, group)) {
    std::fprintf(stderr,
                 "未知分组 \"%s\"。可用：pure / windows / linux / git-readonly / git-commit / "
                 "git-push / unclassified / safe / all。\n",
                 std::string(groupFilter).c_str());
    exitCode = 3;
    return false;
  }
  if (!AutoRunnable(group) && !PushRunAuthorized(("分组 \"" + std::string(groupFilter) + "\"").c_str(),
                                                 exitCode)) {
    return false;
  }
  out.mode = SelectMode::single;
  out.requested = group;
  return true;
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
  // 列名单不碰 Git、不改环境，所以禁止组也允许列出来看——「能看清被禁的是哪些」正是核对清单的手段。
  RunSelection selection;
  if (groupFilter.empty() || groupFilter == "safe") {
    selection.mode = SelectMode::safe;
  } else if (groupFilter == "all") {
    selection.mode = SelectMode::everything;
  } else if (ParseGroup(groupFilter, selection.requested)) {
    selection.mode = SelectMode::single;
  } else {
    std::fprintf(stderr,
                 "未知分组 \"%s\"。可用：pure / windows / linux / git-readonly / git-commit / "
                 "git-push / unclassified / safe / all。\n",
                 std::string(groupFilter).c_str());
    return -1;
  }
  int listed = 0;
  for (const RegisteredCase& item : Cases()) {
    if (!nameFilter.empty() && item.name.find(nameFilter) == std::string::npos) {
      continue;
    }
    if (!Selected(selection, GroupOf(item))) {
      continue;
    }
    std::fprintf(stdout, "%s\t%s\n", GroupLabel(GroupOf(item)).data(), item.name.c_str());
    ++listed;
  }
  return listed;
}

int PrintGroups() {
  const GroupCounts counts = CountGroups();
  struct Row {
    CaseGroup group;
    int GroupCounts::*count;
    const char* authorization;
  };
  const Row rows[] = {
      {CaseGroup::pure, &GroupCounts::pure, "可自动运行（纯逻辑，不起子进程）"},
      {CaseGroup::windows, &GroupCounts::windows, "可自动运行（真实 Win32，但不启动 Git）"},
      {CaseGroup::linux, &GroupCounts::linux, "可自动运行（真实 Linux 基础服务，但不启动 Git）"},
      {CaseGroup::gitReadonly, &GroupCounts::gitReadonly, "可自动运行（真实 Git，逐例核实无提交无推送）"},
      {CaseGroup::gitCommit, &GroupCounts::gitCommit, "可自动运行（隔离临时仓库里真实提交，不推送）"},
      {CaseGroup::gitPush, &GroupCounts::gitPush, "禁止自动运行（会 git push，须使用者本人放行）"},
      {CaseGroup::unclassified, &GroupCounts::unclassified, "禁止自动运行（未登记，按禁止侧对待）"},
  };
  std::fprintf(stdout, "分组账目（本平台已编译的注册用例）：\n");
  for (const Row& row : rows) {
    std::fprintf(stdout, "  %-14s %4d 个  %s\n", GroupLabel(row.group).data(), counts.*(row.count),
                 row.authorization);
  }
  PrintGroupStatsLine("  ");
  std::fprintf(stdout, "  可自动运行合计 %d 个 / 禁止自动运行 %d 个\n", counts.AutoRunnable(),
               counts.Total() - counts.AutoRunnable());
  return 0;
}

int RunAll(std::string_view nameFilter, std::string_view groupFilter) {
  if (const int drift = ValidateGroupTables(); drift != 0) {
    std::fprintf(stdout, "\n分组表校验未通过（%d 处漂移），拒绝在名单与注册集不一致时运行。\n", drift);
    return 3;
  }
  RunSelection selection;
  int refusalExitCode = 0;
  if (!ResolveSelection(groupFilter, selection, refusalExitCode)) {
    return refusalExitCode;
  }
  // push 闸门：只有点名放行过的那两种模式才打开，安全集与单个可自跑组一律关着。
  SetGitPushAllowed(selection.mode == SelectMode::everything ||
                    (selection.mode == SelectMode::single && !AutoRunnable(selection.requested)));

  int matched = 0;
  int failedCases = 0;
  int prerequisiteFailures = 0;
  int pushRefusals = 0;
  for (const RegisteredCase& item : Cases()) {
    if (!nameFilter.empty() && item.name.find(nameFilter) == std::string::npos) {
      continue;
    }
    if (!Selected(selection, GroupOf(item))) {
      continue;
    }
    // 命中 0 绝不报「全通过」：过滤器写错、用例改名、分组表漂移都会在这里现形。
    ++matched;
    CurrentCaseFailures() = 0;
    std::fprintf(stdout, "[ 运行 ] %s（%s）\n", item.name.c_str(),
                 GroupLabel(GroupOf(item)).data());
    std::string abortReason;
    bool refused = false;
    try {
      item.body();
    } catch (const GitPushForbidden& failure) {
      // 归类错了或被禁止的组被强行选中：闸门拦下，单独计数，不混进断言失败。
      refused = true;
      abortReason = failure.what();
    } catch (const PrerequisiteFailure& failure) {
      // 夹具自身的析构（清理临时目录）在栈展开中照常执行。
      abortReason = failure.what();
    } catch (const std::exception& failure) {
      abortReason = std::string("用例抛出未预期异常: ") + failure.what();
    } catch (...) {
      abortReason = "用例抛出未知异常";
    }
    if (refused) {
      ++pushRefusals;
      std::fprintf(stdout, "[ 拒绝 push ] %s：%s\n", item.name.c_str(), abortReason.c_str());
      continue;
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
                 groupFilter.empty() ? "safe" : std::string(groupFilter).c_str());
    return 2;
  }
  if (prerequisiteFailures != 0) {
    std::fprintf(stdout,
                 "\n注意：有 %d 个用例因前置条件失败而中止（Git 不可用或临时夹具创建失败等），"
                 "不是断言失败，但同样必须解决后才能视为测试通过。\n",
                 prerequisiteFailures);
  }
  if (pushRefusals != 0) {
    std::fprintf(stdout,
                 "\n注意：有 %d 个用例被 push 闸门拦下（它们要执行 git push）。"
                 "要么分组归类需要修正，要么这一组本来就该由使用者本人跑。\n",
                 pushRefusals);
  }
  PrintGroupStatsLine("\n");
  const GroupCounts counts = CountGroups();
  std::fprintf(stdout,
               "本次执行 %d 个（未执行 %d 个，其中禁止自动运行 %d 个）："
               "失败 %d 个、前置失败 %d 个、拒绝 push %d 个、断言失败 %d 处。\n",
               matched, static_cast<int>(Cases().size()) - matched,
               counts.gitPush + counts.unclassified, failedCases, prerequisiteFailures,
               pushRefusals, Failures());
  if (counts.unclassified != 0) {
    std::fprintf(stdout, "警告：存在 %d 个未登记用例（unclassified），请把新文件归入 test_main.cpp 分组表。\n",
                 counts.unclassified);
  }
  if (failedCases != 0) {
    return 1;
  }
  // 断言全过但有用例被拦下：不能算通过，单独一个退出码把「归类要修」这件事顶到眼前。
  return pushRefusals == 0 ? 0 : 4;
}

}  // namespace gc::test

namespace {

void PrintUsage() {
  std::fprintf(
      stderr,
      "用法：gc_tests [--group pure|windows|linux|git-readonly|git-commit|git-push|unclassified|safe|all]"
      " [姓名片段]\n"
      "      gc_tests --list [--group G] [姓名片段]     输出「组名<TAB>用例名」名单\n"
      "      gc_tests --groups                          打印各组账目与运行授权\n"
      "不写 --group 时只跑「安全集」= pure + windows + linux + git-readonly + git-commit；\n"
      "git-push 与未登记用例一律不跑（本轮授权允许隔离仓库里测 commit，不允许任何 push）。\n"
      "要点名跑 git-push/all 必须同时设 GC_ALLOW_PUSH=1，由使用者本人执行。\n"
      "退出码：0 全通过 / 1 有失败 / 2 命中 0 个 / 3 分组表漂移或未知分组 / 4 被禁止的 push。\n"
      "分组含义与夹具隔离规则见 docs/testing.md。\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // 命令窗口执行器启动的是「当前可执行文件」的辅助模式：测试二进制同样要认这个入口，
  // 集成用例才会走与正式程序完全相同的那条执行链路（新控制台、说明书、CreateProcessW）。
  int helperExitCode = 0;
  if (gc::platform::RunCommandWindowHelperIfRequested(&helperExitCode)) {
    return helperExitCode;
  }
#endif
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
    if (argument == "--groups") {
      return gc::test::PrintGroups();
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
    if (listed < 0) {
      return 3;  // 未知分组：与 RunAll 同一个退出码，脚本不必分两种写法
    }
    if (listed == 0) {
      std::fprintf(stderr, "--list 命中 0 个用例：过滤器或分组写错了，空名单不算结果。\n");
      return 2;
    }
    return 0;
  }
  return gc::test::RunAll(nameFilter, groupFilter);
}
