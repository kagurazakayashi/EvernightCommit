cmake_minimum_required(VERSION 3.28)

project(EvernightCommit
  VERSION 0.1.0
  DESCRIPTION "Git 图形操作工具：共用业务逻辑 + 平台适配层（Windows 界面是 Win32 过渡实现，Linux 尚未有界面）"
  LANGUAGES CXX)

# ---------- 平台与架构判定 ----------
# 交付范围只有两个已验证形态：Windows x64（MSVC + Windows SDK，完整程序）与
# Linux x86_64（GCC，本轮只构建共用层 + 测试 + 最小 wxWidgets 启动目标）。
# RC 资源语言只在 Windows 需要：Linux 上没有 .rc，也没有 app.manifest。
if(WIN32)
  set(GC_PLATFORM "windows")
  enable_language(RC)
elseif(UNIX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(GC_PLATFORM "linux")
else()
  message(FATAL_ERROR
    "本工程只支持 Windows x64 与 Linux x86_64，当前系统为 ${CMAKE_SYSTEM_NAME}。"
    "macOS 尚未验证过任何目标，不按「大概率能编译」放行。")
endif()

# 指针宽度只是 x64 的必要条件（ARM64 同样是 8 字节），所以两侧都还要看真实架构。
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "只支持 64 位构建：当前指针宽度为 ${CMAKE_SIZEOF_VOID_P} 字节。")
endif()

if(GC_PLATFORM STREQUAL "windows")
  # x64 判定：MSVC 下以编译器自报的目标架构为准
  # （VS 生成器来自 -A，Ninja 来自 vcvars 选定的 cl.exe）。
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
else()
  # Linux 侧只实测过 x86_64：aarch64 等架构未经运行验证，不在配置期放行。
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR
      "只支持 Linux x86_64：当前 CMAKE_SYSTEM_PROCESSOR 为 '${CMAKE_SYSTEM_PROCESSOR}'。"
      "其他架构需要先补构建与运行验证，不能只靠配置通过就宣称支持。")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- 本轮的构建范围开关 ----------
# 两者都是「平台能力到哪一步就构建到哪一步」的闸门，不是留着以后用的花哨选项：
#   * Win32 过渡界面与主程序只有 Windows 能编译（依赖 Win32 平台实现与 RC 资源）；
#     Linux 上强行打开只会得到一堆与业务无关的编译错误，所以配置期直接拒绝。
#   * wxWidgets 启动目标在 Linux 已实测；Windows 的依赖获取（vcpkg / 官方二进制）
#     本轮没有机器可验，默认关，免得把未验证的链接方式塞进默认构建里。
option(GC_BUILD_WIN32_UI "构建 Win32 过渡界面、平台实现与主程序（只有 Windows 能开）" ${WIN32})
if(GC_PLATFORM STREQUAL "linux")
  set(_gc_wx_default ON)
else()
  set(_gc_wx_default OFF)
endif()
option(GC_WITH_WX "构建 wxWidgets 启动目标（需要已装好的 wxWidgets 3.2+）" ${_gc_wx_default})
if(GC_BUILD_WIN32_UI AND NOT WIN32)
  message(FATAL_ERROR
    "GC_BUILD_WIN32_UI 只能在 Windows 打开：Win32 界面与 src/platform/windows/ 需要 MSVC、"
    "Windows SDK 与 RC 资源编译器。Linux 侧请保持默认 OFF（本轮没有 Linux 平台实现）。")
endif()

# VC 运行库：默认动态链接（Debug /MDd、Release /MD），但只在用户未通过
# -DCMAKE_MSVC_RUNTIME_LIBRARY 指定时提供该默认值——此前这里的无条件 set
# 会用目录变量遮蔽缓存项，把命令行的显式选择（例如免部署的静态运行库）覆盖掉。
# 该变量是目录作用域，其后的核心库、平台库、UI、主程序与测试目标共用同一策略，
# 避免混合 CRT 链接。部署含义与静态选项见 README 的“运行库部署要求”。
# 只对 MSVC 有意义：GCC/Clang 没有 MSVC_RUNTIME_LIBRARY 这套策略。
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(GC_WARNINGS_AS_ERRORS "把编译警告视为错误（供 CI 使用）" OFF)

add_library(gc_build_settings INTERFACE)
target_compile_features(gc_build_settings INTERFACE cxx_std_20)
if(WIN32)
  # Unicode 宽字符 API、LEAN_AND_MEAN、NOMINMAX 与 _WIN32_WINNT 都是 Win32 头的事，
  # 出了 Windows 一个都不该定义（Linux 上 UNICODE 会污染第三方头文件的条件编译）。
  target_compile_definitions(gc_build_settings INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00)
endif()
if(MSVC)
  target_compile_definitions(gc_build_settings INTERFACE _CRT_SECURE_NO_WARNINGS)
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
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Linux 侧按本工程实际用得到的告警起步：共用层要长期跨平台，
  # 未初始化、符号比较、阴影变量这类真问题不能靠 MSVC 的 /W4 兜住。
  # -Wmissing-field-initializers 关掉：测试与模型代码大量用「聚合初始化只写关心的前几个成员」
  # 这种写法（其余成员靠默认初值），这是刻意的而不是疏漏，报出来只会盖掉真信号。
  target_compile_options(gc_build_settings INTERFACE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -fno-omit-frame-pointer)
  if(GC_WARNINGS_AS_ERRORS)
    target_compile_options(gc_build_settings INTERFACE -Werror)
  endif()
else()
  message(FATAL_ERROR
    "未识别的编译器 ${CMAKE_CXX_COMPILER_ID}：本工程只在 MSVC（Windows）与 GCC/Clang（Linux）"
    "下验证过告警与字符集选项，不做未验证的放行。")
endif()

# ---------- 应用状态与 Git 数据模型（不依赖窗口系统） ----------
add_library(gc_core STATIC
  src/app/app_state.cpp
  src/app/commit_form_session.cpp
  src/app/list_view_memory.cpp
  src/app/operation_conclusions.cpp
  src/app/operation_gate.cpp
  src/app/operation_history.cpp
  src/app/persistent_state.cpp
  src/app/submodule_journey.cpp
  src/app/task_coordinator.cpp
  src/git/author_config.cpp
  src/git/command_window.cpp
  src/git/commit_changes.cpp
  src/git/commit_date.cpp
  src/git/commit_detail.cpp
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
  src/git/restore_plan.cpp
  src/git/shell_text.cpp
  src/git/staging_plan.cpp
  src/git/submodule_navigation.cpp
  src/git/undo_commit_plan.cpp
  src/git/workspace_model.cpp
  src/git/workspace_status.cpp)
target_include_directories(gc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gc_core PUBLIC gc_build_settings)
# 后台任务（task_coordinator、GitTaskWorker）用 std::thread：
# Windows 的 MSVC 运行时自带线程支持，Linux 的 glibc 必须显式 -pthread，
# 否则链接期才报 std::thread 相关未定义符号，而且 pthread 栈大小与信号语义不对。
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(gc_core PUBLIC Threads::Threads)
endif()

# ---------- 平台无关的能力契约（只有头文件，不含任何原生句柄） ----------
# src/platform/*.h：后台任务代次、完成通知投递目标、本机时区换算、对话框内容。
# 这一层只有声明与模板，具体的系统调用留在 windows/；wxWidgets 界面将来接同一份契约。
add_library(gc_platform_contract INTERFACE)
target_include_directories(gc_platform_contract INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gc_platform_contract INTERFACE gc_build_settings gc_core)

# ---------- Windows 平台能力 / 界面 / 主程序（只在 Windows 构建） ----------
# 下面三段全部受 GC_BUILD_WIN32_UI 管辖：Linux 上既没有 Win32 平台实现，也不该出现
# 第二套「看着能配过、实际是空壳」的目标。要接 Linux 平台实现时，按 src/platform/
# 那份契约新写 gc_platform_linux，而不是把这些文件里的调用改成条件编译。
if(GC_BUILD_WIN32_UI)
add_library(gc_platform STATIC
  src/platform/windows/author_config.cpp
  src/platform/windows/clipboard.cpp
  src/platform/windows/commit_changes.cpp
  src/platform/windows/commit_detail.cpp
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
  src/platform/windows/notification.cpp
  src/platform/windows/operation_history_store.cpp
  src/platform/windows/path_picker.cpp
  src/platform/windows/pathspec_file.cpp
  src/platform/windows/persistent_store.cpp
  src/platform/windows/platform_init.cpp
  src/platform/windows/pull_probe.cpp
  src/platform/windows/push_probe.cpp
  src/platform/windows/remote_choice_dialog.cpp
  src/platform/windows/repo_detect.cpp
  src/platform/windows/restore_probe.cpp
  src/platform/windows/subprocess.cpp
  src/platform/windows/submodule_probe.cpp
  src/platform/windows/undo_probe.cpp
  src/platform/windows/utf_text.cpp
  src/platform/windows/win_path.cpp
  src/platform/windows/workspace_status.cpp)
target_include_directories(gc_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 平台层在 Win32 能力之上复用 git/ 的纯逻辑（PATH 搜索、版本输出判定），
# 并实现 src/platform/ 里那份与操作系统无关的契约。
target_link_libraries(gc_platform PUBLIC gc_build_settings gc_platform_contract
  comctl32 ole32 shell32 uuid)

# ---------- 界面 ----------
# 两类文件分开列，边界就是这次拆分的结果：
#   * 操作控制器（flow_*、upstream_write_flow）只认 ui/operation_host.h 那条窄接口和
#     src/platform/ 的契约，将来 wxWidgets 界面接着用；
#   * Win32 过渡实现（main_window、控件、布局、消息号表）是本轮要被 wxWidgets 替换掉的部分。
add_library(gc_ui STATIC
  # ---- 操作控制器（跨平台，随统一界面保留） ----
  src/ui/commit_flow.cpp
  src/ui/conflict_flow.cpp
  src/ui/fetch_flow.cpp
  src/ui/pull_flow.cpp
  src/ui/push_flow.cpp
  src/ui/restore_flow.cpp
  src/ui/submodule_flow.cpp
  src/ui/undo_flow.cpp
  src/ui/upstream_write_flow.cpp
  # ---- Win32 过渡实现（只服务当前的 Windows 界面，待 wxWidgets 取代） ----
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
target_link_libraries(gc_ui PUBLIC gc_core gc_platform_contract gc_platform
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

  message(STATUS "[范围] Windows：核心层 + Win32 平台实现 + 过渡界面 + 主程序 + 测试")
else()
  message(STATUS "[范围] Linux：核心层 + 契约层 + 测试"
                 "（本轮没有 src/platform/linux/ 实现，"
                 "不构建 gc_platform/gc_ui/EvernightCommit.exe）")
endif()

# ---------- wxWidgets 启动目标 ----------
# 目的只有一个：把「依赖怎么来、怎么链接、能不能真起一个窗口」这三件事在
# 迁移业务界面之前先落到可验证的程度上，不夹带任何业务流程。
if(GC_WITH_WX)
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/GcWxWidgets.cmake")
  add_executable(gc_wx_bootstrap src/ui/wx/bootstrap_main.cpp)
  target_link_libraries(gc_wx_bootstrap PRIVATE gc_build_settings gc_wx)
  target_compile_definitions(gc_wx_bootstrap PRIVATE GC_VERSION_STRING="${PROJECT_VERSION}"
                                                     GC_WX_VERSION_STRING="${wxWidgets_VERSION_STRING}")
  set_target_properties(gc_wx_bootstrap PROPERTIES OUTPUT_NAME "gc_wx_bootstrap")
  if(WIN32)
    # GUI 子系统：Windows 下启动不附控制台窗口（与主程序一致）。
    set_target_properties(gc_wx_bootstrap PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()
endif()

# ---------- 测试 ----------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
