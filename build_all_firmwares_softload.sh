#!/usr/bin/env bash
#
# build_all_firmwares_softload.sh
# 一键编译全部固件（含 soft_load(A1) 模式），覆盖三种打印机模式：
#   standard(A1) / high_force_load(P1S) / soft_load(A1)
#
# 行为说明：
#   1. 三种模式全覆盖（DBMCU_P1S=0/1 与 BMCU_SOFT_LOAD=0/1 组合）
#   2. 各层 README.md 为对应 which_to_choose_*.txt 的改名副本（还原作者做法：
#      现有 firmwares/ 里的 README.md 实为选型指南 txt 改名，并非根目录主文档）
#   3. 三种模式格式统一：每个模式各层仅生成 README.md（不再生成 which_to_choose.txt 副本）
#
# 用法： bash build_all_firmwares_softload.sh
# 注意：开头会 rm -rf firmwares 后重建，原有 firmwares/ 内容将被覆盖。
set -euo pipefail

cd "$(dirname "$0")"

command -v pio >/dev/null 2>&1 || { echo "ERROR: 未找到 pio，请先安装 PlatformIO 并加入 PATH"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: 未找到 python3"; exit 1; }

OUT_DIR="firmwares"
PIO_ENV="fw"

TXT_MODE="which_to_choose_mode.txt"
TXT_AUTOLOAD="which_to_choose_autoload.txt"
TXT_RGB="which_to_choose_filament_rgb.txt"
TXT_SLOTS="which_to_choose_slots.txt"
OUT_GUIDE="README.md"

[[ -f "${TXT_MODE}" ]]     || { echo "ERROR: 缺少 ${TXT_MODE}"; exit 1; }
[[ -f "${TXT_AUTOLOAD}" ]] || { echo "ERROR: 缺少 ${TXT_AUTOLOAD}"; exit 1; }
[[ -f "${TXT_RGB}" ]]      || { echo "ERROR: 缺少 ${TXT_RGB}"; exit 1; }
[[ -f "${TXT_SLOTS}" ]]    || { echo "ERROR: 缺少 ${TXT_SLOTS}"; exit 1; }

SOLO_RETRACT="0.095f"
# 双开关自动回抽的安全上限（米）：实际回抽到位由 S2 自动判定，此值仅作兜底上限，须 >= 任何实际 PTFE 长度
AUTO_RETRACT_CAP="2.00"
RETRACTS=(
  "0.10" "0.15" "0.20" "0.25" "0.30" "0.35"
  "0.40" "0.45" "0.50" "0.55" "0.60" "0.65"
  "0.70" "0.75" "0.80" "0.85" "0.90" "0.95"
  "1.00" "1.05" "1.10" "1.15" "1.20" "1.25"
  "1.30" "1.35" "1.40" "1.45" "1.50" "1.55"
  "1.60" "1.65" "1.70" "1.75" "1.80" "1.85"
  "1.90" "1.95" "2.00"
)

# 模式定义: 目录名 | DBMCU_P1S | BMCU_SOFT_LOAD
# standard(A1):         p1s=0 softload=0
# high_force_load(P1S): p1s=1 softload=0
# soft_load(A1):        p1s=0 softload=1
MODES=(
  "standard(A1)|0|0"
  "high_force_load(P1S)|1|0"
  "soft_load(A1)|0|1"
)

build_and_copy() {
  local out_path="$1" ams_num="$2" retract_len="$3" dm="$4" rgb="$5" p1s="$6" softload="$7"
  echo "=== BUILD: P1S=${p1s} SOFT_LOAD=${softload} DM=${dm} RGB=${rgb} AMS_NUM=${ams_num} RETRACT=${retract_len} -> ${out_path}"

  BAMBU_BUS_AMS_NUM="${ams_num}" \
  AMS_RETRACT_LEN="${retract_len}" \
  BMCU_DM_TWO_MICROSWITCH="${dm}" \
  BMCU_ONLINE_LED_FILAMENT_RGB="${rgb}" \
  DBMCU_P1S="${p1s}" \
  BMCU_SOFT_LOAD="${softload}" \
  pio run -e "${PIO_ENV}"

  local src=".pio/build/${PIO_ENV}/firmware.bin"
  [[ -f "${src}" ]] || { echo "ERROR: 缺少 ${src}"; exit 1; }

  mkdir -p "$(dirname "${out_path}")"
  cp -f "${src}" "${out_path}"
}

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

# 顶层：模式选型指南（同时另存为 README.md，与现有 firmwares/ 格式一致）
cp -f "${TXT_MODE}" "${OUT_DIR}/${OUT_GUIDE}"
cp -f "${TXT_MODE}" "${OUT_DIR}/README.md"

for entry in "${MODES[@]}"; do
  IFS='|' read -r mode_dir p1s softload <<< "${entry}"
  mode_base="${OUT_DIR}/${mode_dir}"
  mkdir -p "${mode_base}"

  # 模式层：autoload 选型指南（仅保留 README.md）
  cp -f "${TXT_AUTOLOAD}" "${mode_base}/README.md"
  cp -f "${TXT_AUTOLOAD}" "${mode_base}/${OUT_GUIDE}"

  for dm in 1 0; do
    if [[ "${dm}" == "1" ]]; then dm_dir="AUTOLOAD"; else dm_dir="NO_AUTOLOAD"; fi
    dm_base="${mode_base}/${dm_dir}"
    mkdir -p "${dm_base}"
    cp -f "${TXT_RGB}" "${dm_base}/README.md"
    cp -f "${TXT_RGB}" "${dm_base}/${OUT_GUIDE}"

    for rgb in 1 0; do
      if [[ "${rgb}" == "1" ]]; then rgb_dir="FILAMENT_RGB_ON"; else rgb_dir="FILAMENT_RGB_OFF"; fi
      base="${dm_base}/${rgb_dir}"
      mkdir -p "${base}"/{SOLO,AMS_A,AMS_B,AMS_C,AMS_D}
      cp -f "${TXT_SLOTS}" "${base}/README.md"
      cp -f "${TXT_SLOTS}" "${base}/${OUT_GUIDE}"

      # 双开关(AUTOLOAD=1)：自动回抽，每槽 1 个（2.00m 仅作安全上限）
      # 单开关(NO_AUTOLOAD=0)：保留全部回抽长度矩阵（与旧版一致，供 Release）
      if [[ "${dm}" == "1" ]]; then
        build_and_copy "${base}/SOLO/solo_${AUTO_RETRACT_CAP}f_auto.bin" 0 "${AUTO_RETRACT_CAP}" "${dm}" "${rgb}" "${p1s}" "${softload}"
        for slot in A B C D; do
          case "${slot}" in
            A) ams_num=0 ;;
            B) ams_num=1 ;;
            C) ams_num=2 ;;
            D) ams_num=3 ;;
          esac
          build_and_copy "${base}/AMS_${slot}/ams_${slot,,}_${AUTO_RETRACT_CAP}f_auto.bin" "${ams_num}" "${AUTO_RETRACT_CAP}" "${dm}" "${rgb}" "${p1s}" "${softload}"
        done
      else
        build_and_copy "${base}/SOLO/solo_${SOLO_RETRACT}.bin" 0 "${SOLO_RETRACT}" "${dm}" "${rgb}" "${p1s}" "${softload}"
        for slot in A B C D; do
          case "${slot}" in
            A) ams_num=0 ;;
            B) ams_num=1 ;;
            C) ams_num=2 ;;
            D) ams_num=3 ;;
          esac
          for r in "${RETRACTS[@]}"; do
            build_and_copy \
              "${base}/AMS_${slot}/ams_${slot,,}_${r}f.bin" \
              "${ams_num}" \
              "${r}f" \
              "${dm}" \
              "${rgb}" \
              "${p1s}" \
              "${softload}"
          done
        done
      fi
    done
  done
done

python3 - "${OUT_DIR}" > "${OUT_DIR}/manifest.txt" <<'PY'
import sys, os, zlib, hashlib

root = sys.argv[1]
entries = []

for dirpath, _, filenames in os.walk(root):
    for fn in filenames:
        p = os.path.join(dirpath, fn)
        rel = os.path.relpath(p, root).replace(os.sep, "/")

        crc = 0
        size = 0
        h = hashlib.sha256()

        with open(p, "rb") as f:
            while True:
                b = f.read(1024 * 1024)
                if not b:
                    break
                size += len(b)
                crc = zlib.crc32(b, crc)
                h.update(b)

        entries.append((rel, h.hexdigest(), f"{crc & 0xffffffff:08X}", size))

entries.sort(key=lambda x: x[0])

out = sys.stdout
out.write("# format: SHA256_HEX CRC32_HEX SIZE_BYTES REL_PATH\n")
for rel, sha256_hex, crc32_hex, size in entries:
    out.write(f"{sha256_hex} {crc32_hex} {size} {rel}\n")
PY

echo
echo "DONE. 结果在: ${OUT_DIR}/"
echo "Manifest: ${OUT_DIR}/manifest.txt"
