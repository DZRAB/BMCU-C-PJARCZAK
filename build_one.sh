#!/usr/bin/env bash
#
# build_one.sh — 单独编译一个固件，并归档到 single_build/ 目录（产物命名与目录结构与批量脚本一致）
#
# 与批量脚本的区别：
#   - 批量脚本用 firmwares/（全量构建专用，会被 .gitignore 忽略）
#   - 本脚本用 single_build/（单独补编专用，同样被 .gitignore 忽略，不与 firmwares/ 混淆）
#
# 用法：
#   bash build_one.sh <MODE> <AUTOLOAD> <RGB> <SLOT> <RETRACT>
#
# 参数：
#   MODE      standard | p1s | softload   （分别对应 standard(A1) / high_force_load(P1S) / soft_load(A1)）
#   AUTOLOAD  1 | 0                        （1=开启双微动开关板 AUTOLOAD）
#   RGB       1 | 0                        （1=ONLINE LED 显示 filament RGB）
#   SLOT      SOLO | A | B | C | D         （AMS 槽位；SOLO 回抽默认 0.095）
#   RETRACT   回抽长度(米)，如 0.30         （SOLO 可省略，默认 0.095）
#
# 示例：
#   bash build_one.sh standard 1 1 SOLO            # single_build/standard(A1)/AUTOLOAD/FILAMENT_RGB_ON/SOLO/solo_0.095f.bin
#   bash build_one.sh softload 1 0 A 0.30          # single_build/soft_load(A1)/AUTOLOAD/FILAMENT_RGB_OFF/AMS_A/ams_a_0.30f.bin
#   bash build_one.sh p1s 0 1 D 0.80               # single_build/high_force_load(P1S)/NO_AUTOLOAD/FILAMENT_RGB_ON/AMS_D/ams_d_0.80f.bin
#
# 产物相对路径结构（子目录层级与固件命名）与 build_all_firmwares_softload.sh 完全一致，仅根目录为 single_build/，可直接并入 firmwares/ 后发布到 Releases。
# 注意：不会删除现有 single_build/，只把单个固件放入对应位置。
set -euo pipefail

cd "$(dirname "$0")"

command -v pio >/dev/null 2>&1 || { echo "ERROR: 未找到 pio，请先安装 PlatformIO 并加入 PATH"; exit 1; }

MODE="${1:-standard}"
AUTOLOAD="${2:-1}"
RGB="${3:-1}"
SLOT="${4:-SOLO}"
RETRACT="${5:-}"

# --- 模式映射 ---
case "${MODE}" in
  standard) mode_dir="standard(A1)";      p1s=0; softload=0 ;;
  p1s)      mode_dir="high_force_load(P1S)"; p1s=1; softload=0 ;;
  softload) mode_dir="soft_load(A1)";     p1s=0; softload=1 ;;
  *) echo "ERROR: MODE 必须是 standard | p1s | softload"; exit 1 ;;
esac

# --- 槽位映射 ---
case "${SLOT}" in
  SOLO) slot_dir="SOLO";  ams_num=0; [[ -z "${RETRACT}" ]] && RETRACT="0.095" ;;
  A)    slot_dir="AMS_A"; ams_num=0 ;;
  B)    slot_dir="AMS_B"; ams_num=1 ;;
  C)    slot_dir="AMS_C"; ams_num=2 ;;
  D)    slot_dir="AMS_D"; ams_num=3 ;;
  *) echo "ERROR: SLOT 必须是 SOLO | A | B | C | D"; exit 1 ;;
esac

[[ "${AUTOLOAD}" == "1" || "${AUTOLOAD}" == "0" ]] || { echo "ERROR: AUTOLOAD 必须是 1 | 0"; exit 1; }
[[ "${RGB}" == "1" || "${RGB}" == "0" ]] || { echo "ERROR: RGB 必须是 1 | 0"; exit 1; }
[[ -n "${RETRACT}" ]] || { echo "ERROR: 非 SOLO 槽位必须提供回抽长度(米)，如 0.30"; exit 1; }

# --- 目录与文件名（与批量脚本一致） ---
if [[ "${SLOT}" == "SOLO" ]]; then
  bin_name="solo_${RETRACT}f.bin"
else
  slot_lower="${SLOT,,}"
  bin_name="ams_${slot_lower}_${RETRACT}f.bin"
fi
dm_dir=$([[ "${AUTOLOAD}" == "1" ]] && echo AUTOLOAD || echo NO_AUTOLOAD)
rgb_dir=$([[ "${RGB}" == "1" ]] && echo FILAMENT_RGB_ON || echo FILAMENT_RGB_OFF)

out_path="single_build/${mode_dir}/${dm_dir}/${rgb_dir}/${slot_dir}/${bin_name}"

echo "=== BUILD: P1S=${p1s} SOFT_LOAD=${softload} DM=${AUTOLOAD} RGB=${RGB} AMS_NUM=${ams_num} RETRACT=${RETRACT}f -> ${out_path}"

BAMBU_BUS_AMS_NUM="${ams_num}" \
AMS_RETRACT_LEN="${RETRACT}f" \
BMCU_DM_TWO_MICROSWITCH="${AUTOLOAD}" \
BMCU_ONLINE_LED_FILAMENT_RGB="${RGB}" \
DBMCU_P1S="${p1s}" \
BMCU_SOFT_LOAD="${softload}" \
pio run -e fw

src=".pio/build/fw/firmware.bin"
[[ -f "${src}" ]] || { echo "ERROR: 缺少 ${src}"; exit 1; }

mkdir -p "$(dirname "${out_path}")"
cp -f "${src}" "${out_path}"

# --- 复制选型指南，仅保留 README.md（目录层级与批量脚本完全一致）---
TXT_MODE="which_to_choose_mode.txt"
TXT_AUTOLOAD="which_to_choose_autoload.txt"
TXT_RGB="which_to_choose_filament_rgb.txt"
TXT_SLOTS="which_to_choose_slots.txt"
# 顶层 single_build/：模式选型指南
[[ -f "${TXT_MODE}" ]]    && cp -f "${TXT_MODE}"    "single_build/README.md"
# 模式层：AUTOLOAD 选型指南
[[ -f "${TXT_AUTOLOAD}" ]] && cp -f "${TXT_AUTOLOAD}" "single_build/${mode_dir}/README.md"
# AUTOLOAD/NO_AUTOLOAD 层：RGB 选型指南
[[ -f "${TXT_RGB}" ]]     && cp -f "${TXT_RGB}"     "single_build/${mode_dir}/${dm_dir}/README.md"
# FILAMENT_RGB 层：slots 选型指南（该层下所有槽位共享，与批量脚本一致）
[[ -f "${TXT_SLOTS}" ]]   && cp -f "${TXT_SLOTS}"   "single_build/${mode_dir}/${dm_dir}/${rgb_dir}/README.md"

echo
echo "DONE. 固件已生成: ${out_path}"
