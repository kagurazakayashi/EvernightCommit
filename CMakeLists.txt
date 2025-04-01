cmake_minimum_required(VERSION 3.28)

project(EvernightCommit
  VERSION 0.1.0
  DESCRIPTION "Windows 原生 Git 提交工具（Win32 + Common Controls）"
  LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR
    "本工程只支持 Windows：需要 MSVC 与 Windows SDK 提供的 Win32 GUI API。")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "只支持 x64 构建（Visual Studio 生成器请使用 -A x64）。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 动态链接 VC 运行库：Debug 用 /MDd、Release 用 /MD。
# 若要发布免安装版本，可改为静态运行库（见 README 的“运行库部署要求”）。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

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
  src/git/command_window.cpp
  src/git/git_locator.cpp
  src/git/git_probe.cpp
  src/git/repository.cpp
  src/git/workspace_model.cpp
  src/git/workspace_status.cpp)
target_include_directories(gc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gc_core PUBLIC gc_build_settings)

# ---------- Windows 平台能力 ----------
add_library(gc_platform STATIC
  src/platform/windows/ansi_text.cpp
  src/platform/windows/command_window_runner.cpp
  src/platform/windows/environment_block.cpp
  src/platform/windows/git_toolchain.cpp
  src/platform/windows/locale_text.cpp
  src/platform/windows/path_picker.cpp
  src/platform/windows/platform_init.cpp
  src/platform/windows/repo_detect.cpp
  src/platform/windows/subprocess.cpp
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
  src/ui/commit_form.cpp
  src/ui/controls.cpp
  src/ui/layout.cpp
  src/ui/main_window.cpp
  src/ui/repo_bar.cpp
  src/ui/splitter.cpp
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
