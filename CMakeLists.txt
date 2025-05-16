cmake_minimum_required(VERSION 3.28)

project(EvernightCommit
  VERSION 0.1.0
  DESCRIPTION "Windows 原生 Git 提交工具（Win32 + Common Controls）"
  LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR
    "本工程只支持 Windows：需要 MSVC 与 Windows SDK 提供的 Win32 GUI API。")
endif()

# x64 判定：64 位指针只是必要条件，ARM64 同样为 8 字节。MSVC 下以编译器
# 自报的目标架构为准（VS 生成器来自 -A，Ninja 来自 vcvars 选定的 cl.exe）。
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "只支持 x64 构建（Visual Studio 生成器请使用 -A x64）。")
endif()
if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID AND
   NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  message(FATAL_ERROR
    "只支持 Windows x64：编译器目标架构为 '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'。"
    "Visual Studio 生成器请使用 -A x64；Ninja 请在 x64 开发者环境（vcvars64.bat）中配置。")
elseif(MSVC AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
  # 架构未知时不静默放行：本工程按 x64 设计，宁可显式失败也不产出未验证二进制。
  message(FATAL_ERROR
    "无法确定 MSVC 编译器的目标架构（CMAKE_CXX_COMPILER_ARCHITECTURE_ID 为空）。"
    "请确认使用 Visual Studio 生成器加 -A x64，或在 vcvars64.bat 环境中配置 Ninja。")
endif()

# Windows SDK 下限：界面代码依赖 10.0.14393+ 提供的每显示器 DPI API。
# VS 生成器会解析出所选 SDK 版本；Ninja 环境无法可靠取得时不猜测、不强检。
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND
   CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS "10.0.14393")
  message(FATAL_ERROR
    "需要 Windows 10 SDK 10.0.14393 或更高（GetDpiForWindow 等 API），"
    "当前所选 SDK 为 ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# VC 运行库：默认动态链接（Debug /MDd、Release /MD），但只在用户未通过
# -DCMAKE_MSVC_RUNTIME_LIBRARY 指定时提供该默认值——此前这里的无条件 set
# 会用目录变量遮蔽缓存项，把命令行的显式选择（例如免部署的静态运行库）覆盖掉。
# 该变量是目录作用域，其后的核心库、平台库、UI、主程序与测试目标共用同一策略，
# 避免混合 CRT 链接。部署含义与静态选项见 README 的“运行库部署要求”。
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(GC_WARNINGS_AS_ERRORS "把编译警告视为错误（供 CI 使用）" OFF)

add_library(gc_build_settings INTERFACE)
target_compile_features(gc_build_settings INTERFACE cxx_std_20)
target_compile_definitions(gc_build_settings INTERFACE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  _CRT_SECURE_NO_WARNINGS)

if(MSVC)
  target_compile_options(gc_build_settings INTERFACE
    /utf-8          # 源文件与执行字符集均为 UTF-8，中文宽字符串字面量可直接使用
    /W4
    /permissive-
    /EHsc
    /Zc:__cplusplus
    /Gw)
  if(GC_WARNINGS_AS_ERRORS)
    target_compile_options(gc_build_settings INTERFACE /WX)
  endif()
else()
  message(WARNING "未经非 MSVC 编译器验证；本工程按 MSVC + Windows SDK 设计。")
endif()

# ---------- 应用状态与 Git 数据模型（不依赖窗口系统） ----------
add_library(gc_core STATIC
  src/app/app_state.cpp
  src/app/commit_form_session.cpp
  src/app/list_view_memory.cpp
  src/app/operation_conclusions.cpp
  src/app/operation_gate.cpp
  src/app/persistent_state.cpp
  src/app/submodule_journey.cpp
  src/app/task_coordinator.cpp
  src/git/author_config.cpp
  src/git/command_window.cpp
  src/git/commit_date.cpp
  src/git/commit_history.cpp
  src/git/commit_identity.cpp
  src/git/commit_message.cpp
  src/git/commit_plan.cpp
  src/git/conflict_state.cpp
  src/git/diff_view.cpp
  src/git/fetch_plan.cpp
  src/git/fetch_scope.cpp
  src/git/first_push_plan.cpp
  src/git/git_environment.cpp
  src/git/git_locator.cpp
  src/git/git_probe.cpp
  src/git/pull_plan.cpp
  src/git/push_plan.cpp
  src/git/repository.cpp
  src/git/staging_plan.cpp
  src/git/submodule_navigation.cpp
  src/git/undo_commit_plan.cpp
  src/git/workspace_model.cpp
  src/git/workspace_status.cpp)
target_include_directories(gc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gc_core PUBLIC gc_build_settings)

# ---------- Windows 平台能力 ----------
add_library(gc_platform STATIC
  src/platform/windows/author_config.cpp
  src/platform/windows/command_window_helper.cpp
  src/platform/windows/command_window_runner.cpp
  src/platform/windows/commit_message_file.cpp
  src/platform/windows/commit_probe.cpp
  src/platform/windows/conflict_probe.cpp
  src/platform/windows/environment_block.cpp
  src/platform/windows/fetch_probe.cpp
  src/platform/windows/git_query_result.cpp
  src/platform/windows/git_toolchain.cpp
  src/platform/windows/identity_prompt.cpp
  src/platform/windows/local_time.cpp
  src/platform/windows/locale_text.cpp
  src/platform/windows/path_picker.cpp
  src/platform/windows/pathspec_file.cpp
  src/platform/windows/persistent_store.cpp
  src/platform/windows/platform_init.cpp
  src/platform/windows/pull_probe.cpp
  src/platform/windows/push_probe.cpp
  src/platform/windows/remote_choice_dialog.cpp
  src/platform/windows/repo_detect.cpp
  src/platform/windows/subprocess.cpp
  src/platform/windows/submodule_probe.cpp
  src/platform/windows/undo_probe.cpp
  src/platform/windows/utf_text.cpp
  src/platform/windows/win_path.cpp
  src/platform/windows/workspace_status.cpp)
target_include_directories(gc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 平台层在 Win32 能力之上复用 git/ 的纯逻辑（PATH 搜索、版本输出判定）。
target_link_libraries(gc_platform PUBLIC gc_build_settings gc_core comctl32 ole32 shell32 uuid)

# ---------- Win32 界面 ----------
add_library(gc_ui STATIC
  src/ui/action_bar.cpp
  src/ui/changes_pane.cpp
  src/ui/commit_flow.cpp
  src/ui/commit_form.cpp
  src/ui/controls.cpp
  src/ui/conflict_flow.cpp
  src/ui/fetch_flow.cpp
  src/ui/layout.cpp
  src/ui/main_window.cpp
  src/ui/pull_flow.cpp
  src/ui/push_flow.cpp
  src/ui/repo_bar.cpp
  src/ui/splitter.cpp
  src/ui/submodule_flow.cpp
  src/ui/undo_flow.cpp
  src/ui/ui_metrics.cpp)
target_include_directories(gc_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gc_ui PUBLIC gc_core gc_platform
  comctl32 gdi32 user32 uxtheme)
target_compile_definitions(gc_ui PRIVATE
  GC_VERSION_STRING="${PROJECT_VERSION}"
  GC_BUILD_TYPE="$<CONFIG>")

# ---------- 主程序（GUI 子系统，启动时不显示控制台） ----------
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest" _gc_manifest_native)
string(REPLACE "\\" "\\\\" GC_APP_MANIFEST_RC "${_gc_manifest_native}")
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/EvernightCommit.ico" _gc_icon_native)
string(REPLACE "\\" "\\\\" GC_APP_ICON_RC "${_gc_icon_native}")

# 资源脚本（app.rc）与 C++ 源码共用同一份资源 ID 定义：把 src/ui/resource_ids.h
# 复制到生成目录，使 app.rc 中的 #include "resource_ids.h" 能够解析。
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/ui/resource_ids.h"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/resource_ids.h" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" @ONLY)

add_executable(evernight_commit WIN32
  src/main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")
target_link_libraries(evernight_commit PRIVATE gc_ui)
set_target_properties(evernight_commit PROPERTIES
  OUTPUT_NAME "EvernightCommit"
  WIN32_EXECUTABLE TRUE)
if(MSVC)
  # 只嵌入 resources/app.manifest，禁止链接器再生成一份默认 manifest（否则资源重复）。
  target_link_options(evernight_commit PRIVATE /MANIFEST:NO)
endif()

# ---------- 测试 ----------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
