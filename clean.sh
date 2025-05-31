#!/usr/bin/env bash
# Linux 清理入口：删掉 CMake 构建目录（缓存、产物、测试程序），源码与 docs 一律不碰。
#
# 用法：
#   bash ./clean.sh                       # 删掉整个 build/
#   bash ./clean.sh --preset linux-x64-debug   # 只删这一个构建目录
#
# .evernight-local/（本地开发记录）不在删除范围内，与 Windows 的 clean.ps1 行为一致。
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/scripts/linux/common.sh"

preset=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) [[ $# -ge 2 ]] || gc_die "--preset 需要一个预设名。"; preset="$2"; shift 2 ;;
    *) gc_die "未知参数 \"$1\"。" ;;
  esac
done

if [[ -n "$preset" ]]; then
  case "$preset" in
    *[!A-Za-z0-9._-]*) gc_die "预设名含非法字符，拒绝当作目录名删除：$preset" ;;
  esac
  target_dir="$GC_ROOT/build/$preset"
  if [[ -d "$target_dir" ]]; then
    rm -rf -- "$target_dir"
    printf '已删除 %s\n' "$target_dir"
  else
    printf '没有 %s，无需清理。\n' "$target_dir"
  fi
  exit 0
fi

if [[ -d "$GC_ROOT/build" ]]; then
  rm -rf -- "$GC_ROOT/build"
  printf '已删除 %s\n' "$GC_ROOT/build"
else
  printf '没有 build 目录，无需清理。\n'
fi
