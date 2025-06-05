# 测试目标：单一控制台程序 gc_tests + 按分组注册的 CTest 用例。
#
# 两张清单的划分标准只有一条：这份源码脱离 Win32（gc_platform / gc_ui / <windows.h>）
# 还能不能编译并链接。这不是「想不想跑」的偏好，而是当前代码的客观边界——
# 归进 windows-only 的每一个文件都在 docs/testing.md 的「未迁移清单」里点名列出，
# 配置期还会把整张清单打出来，避免「Linux 看着全绿其实少编了一半」。

# ---- 可脱离 Win32 编译（Windows 与 Linux 都编） ----
set(GC_TEST_SOURCES_PORTABLE
  app_state_tests.cpp
  author_config_tests.cpp
  commit_changes_tests.cpp
  commit_date_tests.cpp
  commit_detail_tests.cpp
  commit_form_session_tests.cpp
  commit_history_tests.cpp
  commit_identity_tests.cpp
  commit_message_tests.cpp
  commit_plan_tests.cpp
  command_window_plan_tests.cpp
  conflict_state_tests.cpp
  decimal_text_tests.cpp
  diff_view_tests.cpp
  fetch_plan_tests.cpp
  fetch_scope_tests.cpp
  first_push_plan_tests.cpp
  git_environment_tests.cpp
  git_locator_tests.cpp
  git_probe_tests.cpp
  list_rebuild_tests.cpp
  list_view_memory_tests.cpp
  machine_output_completeness_tests.cpp
  main_thread_dispatch_tests.cpp
  operation_conclusions_tests.cpp
  operation_gate_tests.cpp
  operation_history_tests.cpp
  persistent_state_tests.cpp
  pull_plan_tests.cpp
  push_guard_tests.cpp
  push_plan_tests.cpp
  repository_tests.cpp
  restore_plan_tests.cpp
  shell_text_tests.cpp
  terminal_adapters_tests.cpp
  staging_plan_tests.cpp
  submodule_journey_tests.cpp
  submodule_navigation_tests.cpp
  task_coordinator_tests.cpp
  text_encoding_tests.cpp
  undo_commit_plan_tests.cpp
  workspace_status_tests.cpp)

# ---- 仍依赖 Windows 的用例（本轮未迁移，只在 Windows 目标里编译） ----
set(GC_TEST_SOURCES_WINDOWS_ONLY
  # 真实 Win32 集成：起 cmd.exe / 建真实目录与句柄 / 真窗口上的控件断言
  changes_pane_tests.cpp
  commit_form_time_tests.cpp
  command_window_directory_tests.cpp
  environment_block_tests.cpp
  layout_tests.cpp
  list_redraw_tests.cpp
  local_time_tests.cpp
  operation_history_store_file_tests.cpp
  persistent_store_file_tests.cpp
  subprocess_capture_tests.cpp
  subprocess_command_line_tests.cpp
  ui_metrics_tests.cpp
  upstream_write_flow_tests.cpp
  utf_text_tests.cpp
  win_path_probe_tests.cpp
  # 真实 Git 集成：夹具本身（TempDirectory/环境块/子进程执行器）目前是 Win32 实现
  author_config_fixture_tests.cpp
  command_window_fixture_tests.cpp
  commit_changes_fixture_tests.cpp
  commit_detail_fixture_tests.cpp
  commit_history_fixture_tests.cpp
  commit_plan_fixture_tests.cpp
  conflict_fixture_tests.cpp
  diff_view_fixture_tests.cpp
  fetch_fixture_tests.cpp
  fetch_scope_fixture_tests.cpp
  first_push_fixture_tests.cpp
  git_environment_fixture_tests.cpp
  git_fixture_tests.cpp
  pull_probe_fixture_tests.cpp
  push_fixture_tests.cpp
  refresh_repository_tests.cpp
  repo_detect_fixture_tests.cpp
  restore_plan_fixture_tests.cpp
  staging_plan_fixture_tests.cpp
  submodule_navigation_fixture_tests.cpp
  subprocess_capture_fixture_tests.cpp
  undo_probe_fixture_tests.cpp
  workspace_status_fixture_tests.cpp)

# ---- Linux 平台基础服务的真实集成（只在 Linux 目标里编译） ----
# 这些用例会碰真实文件系统/时区规则/环境变量，但绝不启动 Git、不联网、不涉及 push。
set(GC_TEST_SOURCES_LINUX_ONLY
  linux_environment_tests.cpp
  linux_git_toolchain_tests.cpp
  linux_git_query_tests.cpp
  linux_local_time_tests.cpp
  linux_path_probe_tests.cpp
  linux_scratch_file_tests.cpp
  linux_subprocess_tests.cpp
  linux_terminal_git_tests.cpp
  linux_terminal_lifecycle_tests.cpp
  linux_staging_commit_tests.cpp)

# ---- 完整性审计：任何 *_tests.cpp 没进上面三张表，配置直接失败 ----
# 用 GLOB 只做核对、不做构建输入（构建输入始终是上面三张显式清单）。
file(GLOB GC_TEST_SOURCES_ON_DISK RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/*_tests.cpp")
set(_gc_unclassified "")
foreach(source ${GC_TEST_SOURCES_ON_DISK})
  if(NOT source IN_LIST GC_TEST_SOURCES_PORTABLE AND
     NOT source IN_LIST GC_TEST_SOURCES_WINDOWS_ONLY AND
     NOT source IN_LIST GC_TEST_SOURCES_LINUX_ONLY)
    list(APPEND _gc_unclassified ${source})
  endif()
endforeach()
if(_gc_unclassified)
  list(JOIN _gc_unclassified "\n  " _gc_unclassified_text)
  message(FATAL_ERROR
    "tests/ 里有测试文件没登记进 tests/CMakeLists.txt 的两张清单之一：\n  ${_gc_unclassified_text}\n"
    "新文件必须明确归入「可脱离 Win32 编译」或「仍依赖 Windows」，"
    "同时按 test_main.cpp 的规则登记分组表。不允许靠漏登记把用例悄悄漏掉。")
endif()

# 反向核对：清单里写了但磁盘上没有的文件同样报错（改名/删除时不会留幽灵条目）。
set(_gc_missing "")
foreach(source ${GC_TEST_SOURCES_PORTABLE} ${GC_TEST_SOURCES_WINDOWS_ONLY} ${GC_TEST_SOURCES_LINUX_ONLY})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    list(APPEND _gc_missing ${source})
  endif()
endforeach()
if(_gc_missing)
  list(JOIN _gc_missing "\n  " _gc_missing_text)
  message(FATAL_ERROR "tests/CMakeLists.txt 的清单里有磁盘上不存在的路径：\n  ${_gc_missing_text}")
endif()

set(gc_test_sources support/test_main.cpp ${GC_TEST_SOURCES_PORTABLE})
if(WIN32)
  list(APPEND gc_test_sources support/git_fixture.cpp ${GC_TEST_SOURCES_WINDOWS_ONLY})
else()
  list(APPEND gc_test_sources support/linux_git_fixture.cpp ${GC_TEST_SOURCES_LINUX_ONLY})
endif()

add_executable(gc_tests ${gc_test_sources})
target_include_directories(gc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gc_tests PRIVATE gc_core gc_platform_contract gc_ui_flows)
if(WIN32)
  # Windows 目标继续链完整的界面与平台实现：夹具要用的子进程执行器、环境块、
  # 命令窗口辅助入口都在那两层里。
  target_link_libraries(gc_tests PRIVATE gc_platform gc_ui)
else()
  target_link_libraries(gc_tests PRIVATE gc_platform_linux)
endif()
set_target_properties(gc_tests PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME "gc_tests")

# ---------------------------------------------------------------------------
# CTest 注册：只注册可自动运行的分组，绝不注册「一次跑全部」的入口。
#
# 判定通过的唯一依据是汇总行里三个计数同时为 0：
#     失败 0 个、前置失败 0 个、拒绝 push 0 个
# —— 断言失败、夹具前置失败、被 push 闸门拦下，三种都会让这条正则匹配不上。
# 命中 0 个用例时运行器返回 2 且打印「命中 0 个用例」，同样不算通过。
#
# git-push 与 unclassified 不注册：它们要执行真实 git push（或还没人核实过），
# 按本轮授权只能由使用者本人在自己终端里点名跑（见 docs/testing.md 的授权表）。
# ---------------------------------------------------------------------------
include(CTest)

function(gc_add_group_test group_name)
  add_test(NAME gc_tests_${group_name} COMMAND gc_tests --group ${group_name})
  set_tests_properties(gc_tests_${group_name} PROPERTIES
    # 跑可自跑分组时闸门必须是关的：环境里残留的 GC_ALLOW_PUSH 不能把它顶开。
    ENVIRONMENT "GC_ALLOW_PUSH="
    PASS_REGULAR_EXPRESSION "失败 0 个、前置失败 0 个、拒绝 push 0 个"
    # 只匹配运行器的行标记：汇总行本身含「断言失败 N 处」字样，拿它当失败特征会自己咬自己。
    FAIL_REGULAR_EXPRESSION "\\[ 失败 \\]|\\[ 前置失败 \\]|\\[ 拒绝 push \\]|命中 0 个用例|分组表漂移|分组表缺口")
endfunction()

gc_add_group_test(pure)

# push 闸门必须「自己证明自己拦得住」：这条用例跑的就是拒绝路径本身，不执行任何 Git 命令。
# 这里显式把 GC_ALLOW_PUSH 置空，避免使用者环境里恰好设了 1 反而让这条用例真去推送。
add_test(NAME gc_tests_push_gate_refuses COMMAND gc_tests --group git-push)
set_tests_properties(gc_tests_push_gate_refuses PROPERTIES
  ENVIRONMENT "GC_ALLOW_PUSH="
  PASS_REGULAR_EXPRESSION "拒绝运行"
  FAIL_REGULAR_EXPRESSION "命中 0 个用例")

if(WIN32)
  gc_add_group_test(windows)
  gc_add_group_test(git-readonly)
  gc_add_group_test(git-commit)
  message(STATUS "[测试] 已注册分组：pure / windows / git-readonly / git-commit"
                 "（git-push 与 unclassified 不注册，禁止自动运行）")
else()
  gc_add_group_test(linux)
  gc_add_group_test(git-readonly)
  gc_add_group_test(git-commit)
  message(STATUS "[测试] 已注册分组：pure / linux / git-readonly / git-commit"
                 "（windows 组源码尚未脱离 Win32；git-push 与 unclassified 不注册，禁止自动运行）")
endif()
