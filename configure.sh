#!/usr/bin/env bash
# Linux 配置入口：cmake --preset linux-x64-debug / linux-x64-release。
#
# 用法：
#   bash ./configure.sh                       # 默认 Debug
#   bash ./configure.sh --config Release      # Release
#   bash ./configure.sh --preset linux-x64-debug -DGC_WARNINGS_AS_ERRORS=ON
#   bash ./configure.sh --list                # 看本机可用的预设
#   bash ./configure.sh --wx-config /opt/wx/bin/wx-config   # 用非系统安装的 wxWidgets
#
# 需要：CMake 3.28+、Ninja、GCC/Clang（C++20）、wxWidgets 3.2+（GC_WITH_WX=OFF 时可不要）。
# 本脚本不安装任何依赖：缺什么会说明缺什么、按 docs/building.md 怎么装，然后非 0 退出。
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/scripts/linux/common.sh"

gc_require_linux
gc_require_cmake

preset=""
config="Debug"
extra=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      [[ $# -ge 2 ]] || gc_die "--preset 需要一个预设名。"
      preset="$2"
      shift 2
      ;;
    --config)
      [[ $# -ge 2 ]] || gc_die "--config 需要 Debug 或 Release。"
      config="$2"
      shift 2
      ;;
    --list)
      cmake --list-presets
      exit $?
      ;;
    --wx-config)
      [[ $# -ge 2 ]] || gc_die "--wx-config 需要一个 wx-config 路径。"
      [[ -x "$2" ]] || gc_die "wx-config 不可执行：$2"
      extra+=("-DGC_WX_CONFIG=$2")
      shift 2
      ;;
    --no-wx)
      extra+=("-DGC_WITH_WX=OFF")
      shift
      ;;
    -D*)
      # 原样转给 cmake，让使用者能覆盖任意缓存项（脚本不解释、不拦截）。
      extra+=("$1")
      shift
      ;;
    -h | --help)
      sed -n '2,12p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      gc_die "未知参数 \"$1\"。用 --help 看用法。"
      ;;
  esac
done

if [[ -z "$preset" ]]; then
  preset="$(gc_preset_for_config "$config")"
fi

printf '配置：%s → build/%s\n' "$preset" "$preset"
# 退出码直接由 cmake 决定（不吞、不改写），失败时 shell 以同一码结束。
cmake -S "$GC_ROOT" --preset "$preset" ${extra[@]+"${extra[@]}"}
