#!/usr/bin/env bash
# Linux 测试入口 —— 也是 AI 会话的安全入口。
#
# 用法：
#   bash ./test.sh                      # 默认：ctest 跑「已注册的安全分组」
#   bash ./test.sh --groups             # 打印各组账目与运行授权
#   bash ./test.sh --list --group pure  # 列名单（不执行任何 Git 命令）
#   bash ./test.sh --group git-commit   # 直接跑某一组（禁止组会被运行器拒绝，退出码 4）
#   bash ./test.sh --config Release
#
# 为什么默认走 ctest：tests/CMakeLists.txt 只注册 pure / windows / git-readonly / git-commit
# 四组和一个「push 闸门必须拒绝」的自证用例，会执行 git push 的那一组根本没有注册入口，
# 所以这条命令不可能间接把提交推到远端。未注册的分组要跑，必须由使用者本人
# 设 GC_ALLOW_PUSH=1 再点名（见 docs/testing.md 的授权表）。
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/scripts/linux/common.sh"

gc_require_linux
gc_require_cmake

preset=""
config="Debug"
mode="ctest"
runner_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) [[ $# -ge 2 ]] || gc_die "--preset 需要一个预设名。"; preset="$2"; shift 2 ;;
    --config) [[ $# -ge 2 ]] || gc_die "--config 需要 Debug 或 Release。"; config="$2"; shift 2 ;;
    --group)
      [[ $# -ge 2 ]] || gc_die "--group 需要一个分组名。"
      mode="runner"
      runner_args+=(--group "$2")
      shift 2
      ;;
    --groups) mode="runner"; runner_args+=(--groups); shift ;;
    --list) mode="runner"; runner_args+=(--list); shift ;;
    --) shift; runner_args+=("$@"); break ;;
    *) gc_die "未知参数 \"$1\"。用 bash ./test.sh --groups 看可用分组。" ;;
  esac
done

[[ -n "$preset" ]] || preset="$(gc_preset_for_config "$config")"
gc_require_configured "$preset"
build_dir="$(gc_build_dir "$preset")"

if [[ "${GC_ALLOW_PUSH:-}" == "1" ]]; then
  printf '注意：环境里带了 GC_ALLOW_PUSH=1，本次运行被允许执行真实 git push。\n' >&2
  printf '      这是使用者本人的授权开关；AI 会话不应设置它。\n' >&2
fi

# 先确保测试程序是最新的：ctest 只跑已注册的用例，不负责重建。
gc_in_root cmake --build --preset "$preset" --target gc_tests

if [[ "$mode" == "ctest" ]]; then
  printf '安全测试入口：ctest --preset %s\n' "$preset"
  gc_in_root ctest --preset "$preset"
  exit $?
fi

exe="$(gc_bin_path "$preset" gc_tests)"
[[ -x "$exe" ]] || gc_die "找不到测试程序：$exe"
printf '%s %s\n' "$exe" "${runner_args[*]}"
"$exe" "${runner_args[@]}"
