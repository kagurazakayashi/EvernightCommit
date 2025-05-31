#!/usr/bin/env bash
# Linux 调试入口：用带符号的 Debug 构建，把参数原样交给可用的调试器。
#
# 用法：
#   bash ./debug.sh                                 # gdb 起 gc_tests（默认跑安全集）
#   bash ./debug.sh -- --group pure                 # -- 之后的参数交给被调试程序
#   bash ./debug.sh --target gc_wx_bootstrap -- --check
#   bash ./debug.sh --batch                         # 非交互：跑一遍，崩了就打回溯
#   bash ./debug.sh --debugger lldb                 # 指定调试器（也可用 GC_DEBUGGER 环境变量）
#
# 固定用 Debug 预设：Release 没有 -g，断点与变量都会失真。产物缺调试段时直接报出来，
# 而不是让调试器在没符号的情况下空转。
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/scripts/linux/common.sh"

gc_require_linux
gc_require_cmake

target="gc_tests"
batch=0
debugger="${GC_DEBUGGER:-}"
prog_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) [[ $# -ge 2 ]] || gc_die "--target 需要一个目标名。"; target="$2"; shift 2 ;;
    --debugger) [[ $# -ge 2 ]] || gc_die "--debugger 需要一个可执行文件名。"; debugger="$2"; shift 2 ;;
    --batch) batch=1; shift ;;
    --) shift; prog_args+=("$@"); break ;;
    *) gc_die "未知参数 \"$1\"。用 --help 看用法。" ;;
  esac
done

preset="linux-x64-debug"
gc_require_configured "$preset"
gc_in_root cmake --build --preset "$preset" --target "$target"

exe="$(gc_bin_path "$preset" "$target")"
[[ -x "$exe" ]] || gc_die "构建后找不到可执行文件：$exe（--target 给的是可执行目标吗？）"

if gc_have readelf; then
  if ! readelf -S -- "$exe" 2>/dev/null | grep -q '\.debug_info'; then
    gc_die "$exe 里没有 .debug_info 段：不是带符号构建。用 bash ./build.sh --clean 重新按 Debug 预设构建。"
  fi
else
  printf '提示：本机没有 readelf，跳过调试段自检。\n' >&2
fi

if [[ -z "$debugger" ]]; then
  for candidate in gdb lldb; do
    if gc_have "$candidate"; then
      debugger="$candidate"
      break
    fi
  done
fi
[[ -n "$debugger" ]] || gc_die "找不到调试器（依次试过 GC_DEBUGGER、gdb、lldb）。Debian/Ubuntu：apt install gdb。"

printf '调试器 %s，带符号产物 %s\n' "$debugger" "$exe"
if [[ "$batch" -eq 1 ]]; then
  # 非交互模式：跑完就退出，崩溃时打回溯——没有终端也能拿到栈。
  exec "$debugger" -q -batch -ex run -ex bt --args "$exe" ${prog_args[@]+"${prog_args[@]}"}
fi
exec "$debugger" --args "$exe" ${prog_args[@]+"${prog_args[@]}"}
