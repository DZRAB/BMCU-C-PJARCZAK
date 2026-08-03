#!/usr/bin/env bash
# clean_build.sh — 一键清理 BMCU 编译产物（跨编译脚本通用）
#
# 删除以下由 PlatformIO 及各编译脚本生成的目录：
#   .pio            PlatformIO 构建目录（含 .pio/build/fw/firmware.bin 等中间产物）
#   .pioenvs       旧版 PlatformIO 构建目录（兼容）
#   firmwares       全部编译 / 快速编译输出（build_all_firmwares_*.sh / *.py，v3.2 常规矩阵）
#   firmwares-tpu   v4.0-tpu 专用固件输出（build_all_firmwares_fast.py 设 BMCU_TPU_MODEL 时生成）
#   firmwares_Release  发布归档目录（本地发布打包用，不入库）
#   single_build    单独编译输出（build_one.sh，含 TPU_*/ 子目录）
#   .pio_parallel   快速编译脚本缓存目录（build_all_firmwares_fast.py）
#
# 注意：以上目录均已写入 .gitignore，不会提交 git；清理仅影响本地构建产物。
#
# 用法（在 Git Bash 中运行）：
#   bash clean_build.sh             # 列出并询问确认后删除（默认，安全）
#   bash clean_build.sh -f          # 直接强制删除（真正一键）
#   bash clean_build.sh -n          # 只显示将要删除的内容，不删除（预览）
set -uo pipefail
cd "$(dirname "$0")"

DIRS=(.pio .pioenvs firmwares firmwares-tpu firmwares_Release single_build .pio_parallel)

FORCE=0
DRY=0
for a in "$@"; do
  case "$a" in
    -f|--force) FORCE=1 ;;
    -n|--dry-run) DRY=1 ;;
  esac
done

echo "待清理的编译产物目录："
found=()
for d in "${DIRS[@]}"; do
  if [[ -d "$d" ]]; then
    if du -sh "$d" >/dev/null 2>&1; then
      size=$(du -sh "$d" 2>/dev/null | awk '{print $1}')
      echo "  - $d/  ($size)"
    else
      echo "  - $d/"
    fi
    found+=("$d")
  fi
done

if [[ ${#found[@]} -eq 0 ]]; then
  echo "（无）当前工作区没有可清理的编译产物，已经很干净。"
  exit 0
fi

if [[ $DRY -eq 1 ]]; then
  echo "[dry-run] 未做任何删除。"
  exit 0
fi

if [[ $FORCE -eq 0 ]]; then
  read -r -p "确认删除以上 ${#found[@]} 个目录? [y/N] " ans
  case "$ans" in
    y|Y|yes|YES) ;;
    *) echo "已取消，未删除任何文件。"; exit 0 ;;
  esac
fi

for d in "${found[@]}"; do
  rm -rf "$d"
  echo "已删除: $d/"
done
echo "清理完成。"
