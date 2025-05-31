# wxWidgets 依赖获取与链接（被根 CMakeLists.txt 在 GC_WITH_WX=ON 时 include）。
#
# 这个文件只做一件事：把「wxWidgets 从哪来、怎么链」收敛成一条不含机器绝对路径的
# 发现顺序，并在缺失时给出可直接照做的提示。它不下载、不安装、不提权——
# 需要装依赖时由使用者自己执行提示里的命令。
#
# 可传入的缓存变量（都不必填）：
#   GC_WX_CONFIG        wx-config 可执行文件的绝对路径；留空时依次尝试 $ENV{WX_CONFIG}
#                       与 PATH 上的 wx-config。自定义前缀（源码装到 $HOME 下）就用它。
#   GC_WX_ROOT_DIR      wxWidgets 安装根（Windows 官方二进制的 cmake 发现方式）。
#   GC_WX_LIB_DIR       wx 库目录（Windows 上 .../lib/vc_x64_lib 那一层）。
#   GC_WX_CONFIGURATION Windows 上的配置目录名，例如 msw库u（Debug 为 mswud 形态）。
#   GC_WX_COMPONENTS    需要的组件，默认 "core;base"。
# 结果：提供 INTERFACE 目标 gc_wx（含头文件路径、宏、编译与链接参数）。

set(GC_WX_MIN_VERSION "3.2.0" CACHE STRING "允许的最低 wxWidgets 版本")

if(NOT GC_WX_CONFIG AND DEFINED ENV{WX_CONFIG})
  set(GC_WX_CONFIG "$ENV{WX_CONFIG}")
endif()
if(GC_WX_CONFIG)
  # FindwxWidgets 在 Unix 上就是靠 wx-config 取参数，给它明确的路径即可，
  # 不需要也不应该去猜某个用户机器上的安装目录。
  set(wxWidgets_CONFIG_EXECUTABLE "${GC_WX_CONFIG}")
endif()
if(GC_WX_ROOT_DIR)
  set(wxWidgets_ROOT_DIR "${GC_WX_ROOT_DIR}")
endif()
if(GC_WX_LIB_DIR)
  set(wxWidgets_LIB_DIR "${GC_WX_LIB_DIR}")
endif()
if(GC_WX_CONFIGURATION)
  set(wxWidgets_CONFIGURATION "${GC_WX_CONFIGURATION}")
endif()
if(NOT GC_WX_COMPONENTS)
  set(GC_WX_COMPONENTS core base)
endif()

find_package(wxWidgets "${GC_WX_MIN_VERSION}" COMPONENTS ${GC_WX_COMPONENTS} MODULE)

if(NOT wxWidgets_FOUND)
  if(GC_PLATFORM STREQUAL "linux")
    # 只点名本机实测过的包名；其他发行版给的是同一类东西（wxGTK 3.2 开发包），不逐一宣称验证过。
    set(_gc_wx_hint
      "Linux（本机实测过的就是这一条）：安装发行版打包的 wxWidgets 开发包。
        Debian/Ubuntu：sudo apt install -y libwxgtk3.2-dev wx-common
        其他发行版装对应的 wxGTK 3.2 开发包即可；装好后 wx-config --version 应给出 3.2.x。
        不用系统包时，把源码装到自己的前缀，再用 -DGC_WX_CONFIG=<前缀>/bin/wx-config 指过来。")
  else()
    set(_gc_wx_hint
      "Windows 两条路（本仓库本轮没有 Windows 机器可验，都算未实测，第一次构建请先只跑配置看版本行）：
        ① vcpkg：vcpkg install wxWidgets:x64-windows，配置时带
           -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
        ② 官方二进制：从 wxWidgets 发布页下载对应 MSVC 工具集的包解压后，带
           -DGC_WX_ROOT_DIR=<解压根> -DGC_WX_LIB_DIR=<解压根>/lib/vc_x64_lib
           （Debug 还要 -DGC_WX_CONFIGURATION=mswud 这类目录名）
        两条路都要求编译器与 wx 那套库是同一个 MSVC 工具集（v142/v143/v148 混用会在链接期
        报缺符号），这一点无法在配置期可靠判定，只能如实链接失败。")
  endif()
  message(FATAL_ERROR
    "找不到 wxWidgets（要求版本 >= ${GC_WX_MIN_VERSION}）。本工程不代为下载安装，"
    "也不会静默跳过这个目标。
  ${_gc_wx_hint}
  确认过不需要 wx 目标时可以用 -DGC_WITH_WX=OFF 关掉它；
  已安装却没找到时，用 -DGC_WX_CONFIG=… 或 -DGC_WX_ROOT_DIR=… 明确指路。")
endif()

include(UsewxWidgets)

if(wxWidgets_VERSION_STRING AND wxWidgets_VERSION_STRING VERSION_LESS GC_WX_MIN_VERSION)
  message(FATAL_ERROR
    "发现的 wxWidgets 版本 ${wxWidgets_VERSION_STRING} 低于要求的 ${GC_WX_MIN_VERSION}。")
endif()

add_library(gc_wx INTERFACE)
target_include_directories(gc_wx INTERFACE ${wxWidgets_INCLUDE_DIRS})
target_compile_definitions(gc_wx INTERFACE ${wxWidgets_DEFINITIONS})
if(wxWidgets_DEFINITIONS_DEBUG)
  target_compile_definitions(gc_wx INTERFACE $<$<CONFIG:Debug>:${wxWidgets_DEFINITIONS_DEBUG}>)
endif()
if(wxWidgets_CXX_FLAGS)
  separate_arguments(wxWidgets_CXX_FLAGS_LIST NATIVE_COMMAND "${wxWidgets_CXX_FLAGS}")
  target_compile_options(gc_wx INTERFACE ${wxWidgets_CXX_FLAGS_LIST})
endif()
target_link_libraries(gc_wx INTERFACE ${wxWidgets_LIBRARIES})

# 版本与链接形态必须留痕：交接文档里写的是这两行给出的实测值，不是「应该能用」。
message(STATUS "[wx] 版本 ${wxWidgets_VERSION_STRING}，组件 ${GC_WX_COMPONENTS}，"
               "配置 ${wxWidgets_CONFIGURATION}，库目录 ${wxWidgets_LIB_DIR}")
message(STATUS "[wx] 编译器 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
               "（wx 侧要求同一编译器系列；本机实测基线见 docs/building.md）")
