#!/usr/bin/env bash
# Linux 构建入口：cmake --build --preset linux-x64-debug / linux-x64-release。
#
# 用法：
#   bash ./build.sh                        # 默认 Debug，全部目标
#   bash ./build.sh --config Release
#   bash ./build.sh --target gc_tests      # 只构建某个目标
#   bash ./build.sh --clean                # 先清掉这个构建目录再配（干净重建）
#   bash ./build.sh --verbose
#
# 没配置过的构建目录会先按同一预设自动配置一次（干净目录一条命令即可跑通）。
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/scripts/linux/common.sh"

gc_require_linux
gc_require_cmake

preset=""
config="Debug"
target=""
clean=0
verbose=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) [[ $# -ge 2 ]] || gc_die "--preset 需要一个预设名。"; preset="$2"; shift 2 ;;
    --config) [[ $# -ge 2 ]] || gc_die "--config 需要 Debug 或 Release。"; config="$2"; shift 2 ;;
    --target) [[ $# -ge 2 ]] || gc_die "--target 需要一个目标名。"; target="$2"; shift 2 ;;
    --clean) clean=1; shift ;;
    --verbose) verbose+=(--verbose); shift ;;
    -h | --help) sed -n '2,11p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) gc_die "未知参数 \"$1\"。用 --help 看用法。" ;;
  esac
done

[[ -n "$preset" ]] || preset="$(gc_preset_for_config "$config")"
build_dir="$(gc_build_dir "$preset")"

if [[ "$clean" -eq 1 ]]; then
  # 只删本脚本这一套预设对应的构建目录，不碰别的配置、也不碰源码。
  printf '清理构建目录：%s\n' "$build_dir"
  rm -rf -- "$build_dir"
fi

if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
  printf '构建目录尚未配置，先按预设 %s 配置。\n' "$preset"
  cmake -S "$GC_ROOT" --preset "$preset"
fi

args=(--preset "$preset")
[[ -n "$target" ]] && args+=(--target "$target")
[[ ${#verbose[@]} -gt 0 ]] && args+=("${verbose[@]}")

printf '构建：cmake --build %s\n' "${args[*]}"
gc_in_root cmake --build "${args[@]}"

printf '\n产物目录：%s/bin\n' "$build_dir"
ls -1 "$build_dir/bin" 2>/dev/null || true
