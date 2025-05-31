#!/usr/bin/env bash
# Linux 开发入口脚本共用的几件事：确认自己跑在 bash 下、找到仓库根、按配置挑预设、
# 检查依赖在不在。被 scripts/linux/*.sh 之外的根目录脚本 source，不要直接执行。
#
# 这里刻意不做的事：不下载安装任何东西、不提权、不猜某台机器上的绝对安装路径。
# 缺依赖一律把「缺什么、按本仓库文档怎么装」说清楚之后以非 0 退出码结束。

set -euo pipefail

# BASH_SOURCE[1] 是 source 本文件的调用者脚本；它放在仓库根，所以 dirname 就是根目录。
if [[ -z "${BASH_VERSION:-}" ]]; then
  echo "这些脚本要用 bash 执行：bash ./build.sh（zsh/dash 下 BASH_SOURCE 不可用）" >&2
  exit 64
fi
if [[ "${#BASH_SOURCE[@]}" -lt 2 ]]; then
  echo "common.sh 是库文件，请通过根目录的 configure.sh/build.sh 等入口使用" >&2
  exit 64
fi

GC_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[1]}")" && pwd -P)"
export GC_ROOT

gc_die() {
  printf '%s\n' "$*" >&2
  exit 1
}

gc_have() {
  command -v "$1" >/dev/null 2>&1
}

# Debug/Release → 预设名。Linux 用单配置 Ninja，构建目录天然按预设分开，
# 两个配置各占一个目录，互不覆盖（Windows 的多配置生成器另有自己的一套）。
gc_preset_for_config() {
  case "$1" in
    Debug) printf 'linux-x64-debug\n' ;;
    Release) printf 'linux-x64-release\n' ;;
    *) gc_die "未知配置 \"$1\"：只支持 Debug 与 Release。" ;;
  esac
}

gc_build_dir() {
  printf '%s/build/%s\n' "$GC_ROOT" "$1"
}

gc_require_cmake() {
  gc_have cmake || gc_die "找不到 cmake。需要 CMake 3.28 及以上（Debian/Ubuntu：apt install cmake ninja-build）。"
}

# 没配置过的构建目录直接给 cmake --build 会吐一串生成器层面的报错，
# 这里提前一步说清楚该先跑哪条命令。
gc_require_configured() {
  local preset="$1"
  local cache
  cache="$(gc_build_dir "$preset")/CMakeCache.txt"
  [[ -f "$cache" ]] || gc_die "构建目录尚未配置（缺 $cache）。先执行：bash ./configure.sh --preset $preset"
}

gc_require_linux() {
  [[ "$(uname -s)" == "Linux" ]] ||
    gc_die "这些 .sh 入口只服务 Linux；Windows 请用根目录的 build.ps1 / run.ps1 / test.ps1 / clean.ps1。"
}

# cmake --build --preset 与 ctest --preset 都是从「当前工作目录」找 CMakePresets.json 的
# （只有 cmake --preset 配 -S 例外），所以从别的目录调用本仓库脚本时必须先切回根目录。
# 用子 shell，不改调用者真正的工作目录；命令的退出码原样传出去。
gc_in_root() {
  (cd -- "$GC_ROOT" && "$@")
}

# 产物路径：Linux 是单配置，bin 下直接放可执行文件（Windows 还要按 <Config> 分一层）。
gc_bin_path() {
  local preset="$1" name="$2"
  printf '%s/bin/%s\n' "$(gc_build_dir "$preset")" "$name"
}
