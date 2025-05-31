#!/usr/bin/env bash
# Linux 运行入口。本轮 Linux 上唯一能跑起来的界面目标是 wxWidgets 启动检查窗口
# （gc_wx_bootstrap）：正式主程序 EvernightCommit 还是 Win32 实现，Linux 上跑不了，
# 这里不做「假装能跑」的空壳。
#
# 用法：
#   bash ./run.sh                    # 开检查窗口（需要 $DISPLAY 或 Wayland）
#   bash ./run.sh --config Release
#   bash ./run.sh --check            # 只打印版本与链接形态，不需要显示服务
#   bash ./run.sh -- --version       # -- 之后的参数原样转给程序
#
# 产物不存在时先构建（Debug 预设），构建失败按 cmake 的退出码结束。
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/scripts/linux/common.sh"

gc_require_linux
gc_require_cmake

preset=""
config="Debug"
prog_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) [[ $# -ge 2 ]] || gc_die "--preset 需要一个预设名。"; preset="$2"; shift 2 ;;
    --config) [[ $# -ge 2 ]] || gc_die "--config 需要 Debug 或 Release。"; config="$2"; shift 2 ;;
    --check | --version | --help) prog_args+=("$1"); shift ;;
    --) shift; prog_args+=("$@"); break ;;
    *) gc_die "未知参数 \"$1\"。用 --help 看用法。" ;;
  esac
done

[[ -n "$preset" ]] || preset="$(gc_preset_for_config "$config")"
exe="$(gc_bin_path "$preset" gc_wx_bootstrap)"

if [[ ! -x "$exe" ]]; then
  printf '产物不存在，先构建：%s\n' "$exe"
  gc_in_root cmake --build --preset "$preset" --target gc_wx_bootstrap
fi
[[ -x "$exe" ]] || gc_die "构建后仍找不到产物：$exe"

# 只有真要开窗口时才要求显示服务；--check/--version 不碰它。
need_display=1
for arg in ${prog_args[@]+"${prog_args[@]}"}; do
  case "$arg" in
    --check | --version | --help | -h) need_display=0 ;;
  esac
done
if [[ "$need_display" -eq 1 && -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  gc_die "没有 DISPLAY/WAYLAND_DISPLAY，开不了窗口。无显示环境下用 bash ./run.sh --check 只核对依赖与链接。"
fi

printf '启动 %s\n' "$exe"
"$exe" ${prog_args[@]+"${prog_args[@]}"}
