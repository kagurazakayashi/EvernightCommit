add_executable(gc_tests
  support/test_main.cpp
  support/git_fixture.cpp
  app_state_tests.cpp
  author_config_tests.cpp
  author_config_fixture_tests.cpp
  command_window_directory_tests.cpp
  command_window_directory_tests.cpp
  command_window_fixture_tests.cpp
  command_window_plan_tests.cpp
  commit_date_tests.cpp
  commit_form_session_tests.cpp
  commit_history_fixture_tests.cpp
  commit_history_tests.cpp
  commit_identity_tests.cpp
  commit_message_tests.cpp
  commit_plan_fixture_tests.cpp
  commit_plan_tests.cpp
  conflict_fixture_tests.cpp
  conflict_state_tests.cpp
  diff_view_fixture_tests.cpp
  diff_view_tests.cpp
  environment_block_tests.cpp
  fetch_fixture_tests.cpp
  fetch_plan_tests.cpp
  fetch_scope_fixture_tests.cpp
  fetch_scope_tests.cpp
  first_push_fixture_tests.cpp
  first_push_plan_tests.cpp
  git_fixture_tests.cpp
  git_environment_fixture_tests.cpp
  git_environment_tests.cpp
  git_locator_tests.cpp
  git_probe_tests.cpp
  layout_tests.cpp
  list_view_memory_tests.cpp
  local_time_tests.cpp
  machine_output_completeness_tests.cpp
  operation_conclusions_tests.cpp
  operation_gate_tests.cpp
  refresh_repository_tests.cpp
  repo_detect_fixture_tests.cpp
  repository_tests.cpp
  subprocess_command_line_tests.cpp
  subprocess_capture_fixture_tests.cpp
  subprocess_capture_tests.cpp
  staging_plan_fixture_tests.cpp
  staging_plan_tests.cpp
  submodule_journey_tests.cpp
  submodule_navigation_fixture_tests.cpp
  submodule_navigation_tests.cpp
  task_coordinator_tests.cpp
  ui_metrics_tests.cpp
  pull_plan_tests.cpp
  pull_probe_fixture_tests.cpp
  push_plan_tests.cpp
  push_fixture_tests.cpp
  undo_commit_plan_tests.cpp
  undo_probe_fixture_tests.cpp
  utf_text_tests.cpp
  workspace_status_tests.cpp
  workspace_status_fixture_tests.cpp)

target_include_directories(gc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gc_tests PRIVATE gc_core gc_platform gc_ui)
set_target_properties(gc_tests PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME "gc_tests")

add_test(NAME gc_tests COMMAND gc_tests)
set_tests_properties(gc_tests PROPERTIES PASS_REGULAR_EXPRESSION "失败 0 个")
