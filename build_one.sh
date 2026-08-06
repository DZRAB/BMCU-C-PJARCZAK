#!/usr/bin/env bash
#
# build_one.sh — 单独编译一个固件，并归档到 single_build/ 目录（产物命名与目录结构与批量脚本一致）
#
# 与批量脚本的区别：
#   - 批量脚本用 firmwares/（全量构建专用，会被 .gitignore 忽略）
#   - 本脚本用 single_build/（单独补编专用，同样被 .gitignore 忽略，不与 firmwares/ 混淆）
#
# 用法：
#   bash build_one.sh <MODE> <AUTOLOAD> <RGB> <SLOT> <RETRACT> [AUTO_RETRACT] [TPU0 TPU1 TPU2 TPU3]
#
# 参数：
#   MODE      standard | p1s | softload   （分别对应 standard(A1) / high_force_load(P1S) / soft_load(A1)）
#   AUTOLOAD  1 | 0                        （1=双微动开关板；0=单微动开关板）
#   RGB       1 | 0                        （1=ONLINE LED 显示 filament RGB）
#   SLOT      SOLO | A | B | C | D         （AMS 槽位；SOLO 回抽默认 0.095）
#   RETRACT   回抽长度(米)，如 0.30         （SOLO 可省略，默认 0.095；AUTOLOAD=1 且 AUTO_RETRACT=1 时忽略，回抽由 S2 自动判定）
#   AUTO_RETRACT  (可选) 1 | 0              （默认 1；仅对 AUTOLOAD=1 生效。1=自动回抽 _auto；0=双开关也按固定长度矩阵编译，与 2.0 一致）
#   TPU0..3  (可选) 各通道 Bambu filament_id 写死型号（GFU98/GFU00/GFU02/GFU95/GFU90/GFU85）。固件内置 TPU 逻辑，
#            仅当打印机将该通道设为 TPU for AMS(GFU98) 时生效，内部用写死型号跑；不提供则全部保底 GFU85。
#            与普通固件数量一致，不新增变体维度。
#
# 示例：
#   bash build_one.sh standard 1 1 SOLO            # 双开关自动回抽 -> solo_2.00f_auto.bin
#   bash build_one.sh standard 1 1 A  0.30 0       # 双开关固定长度模式 -> ams_a_0.30f.bin（同 2.0）
#   bash build_one.sh p1s 0 1 D 0.80               # 单开关 NO_AUTOLOAD -> ams_d_0.80f.bin
#   bash build_one.sh standard 1 1 A 0.30 1 GFU98 GFU90 GFU95 GFU85   # 4 通道写死型号
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
AUTO_RETRACT="${6:-1}"
# v4.0-tpu: 4 通道写死型号（第 7~10 位置参数），缺省保底 GFU85（最软最稳）
TPU_FIX0="${7:-GFU85}"
TPU_FIX1="${8:-GFU85}"
TPU_FIX2="${9:-GFU85}"
TPU_FIX3="${10:-GFU85}"

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

# --- 双开关自动回抽（AUTOLOAD=1 且 AUTO_RETRACT=1）：忽略回抽长度参数，用 2.00m 作为安全上限 ---
# 实际回抽到位由第二个微动开关 S2 自动判定，固件文件名带 _auto 后缀，无需用户选择回抽长度。
# AUTO_RETRACT=0（固定长度模式）：双开关也按固定长度矩阵编译，与 2.0 一致，文件名不带 _auto。
# 单开关（AUTOLOAD=0）：固定长度逻辑，保留用户传入的 RETRACT，文件名不带 _auto。
if [[ "${AUTOLOAD}" == "1" && "${AUTO_RETRACT}" == "0" ]]; then
  # 双开关固定长度模式：注入 =0 强制走固定长度分支（等价 2.0），必须提供回抽长度
  RETRACT="${RETRACT:-0.095}"
  AUTO_SUFFIX=""
  AUTO_RETRACT_FLAG="0"
  [[ -n "${RETRACT}" ]] || { echo "ERROR: AUTO_RETRACT=0 时双开关必须提供回抽长度(米)，如 0.30"; exit 1; }
elif [[ "${AUTOLOAD}" == "1" ]]; then
  # AUTO_RETRACT 缺省或=1：自动回抽（_auto）。宏不注入，由 Motion_control.h 派生为
  # BMCU_DM_TWO_MICROSWITCH（双开关=1），即默认自动回抽行为。
  RETRACT="2.00"
  AUTO_SUFFIX="_auto"
  AUTO_RETRACT_FLAG=""
else
  # 单开关（AUTOLOAD=0）：固定长度逻辑，保留用户传入的回抽长度（SOLO 已在前面默认 0.095）
  AUTO_SUFFIX=""
  AUTO_RETRACT_FLAG=""
fi

if [[ "${AUTOLOAD}" == "0" ]]; then
  [[ -n "${RETRACT}" ]] || { echo "ERROR: 非 SOLO 槽位必须提供回抽长度(米)，如 0.30"; exit 1; }
fi

# --- 目录与文件名（与批量脚本一致） ---
if [[ "${SLOT}" == "SOLO" ]]; then
  bin_name="solo_${RETRACT}f${AUTO_SUFFIX}.bin"
else
  slot_lower="${SLOT,,}"
  bin_name="ams_${slot_lower}_${RETRACT}f${AUTO_SUFFIX}.bin"
fi
dm_dir=$([[ "${AUTOLOAD}" == "1" ]] && echo AUTOLOAD || echo NO_AUTOLOAD)
rgb_dir=$([[ "${RGB}" == "1" ]] && echo FILAMENT_RGB_ON || echo FILAMENT_RGB_OFF)

out_path="single_build/${mode_dir}/${dm_dir}/${rgb_dir}/${slot_dir}/${bin_name}"

echo "=== BUILD: P1S=${p1s} SOFT_LOAD=${softload} DM=${AUTOLOAD} RGB=${RGB} AMS_NUM=${ams_num} RETRACT=${RETRACT}f TPU_FIX=${TPU_FIX0},${TPU_FIX1},${TPU_FIX2},${TPU_FIX3} -> ${out_path}"

# v4.0-tpu: 通用固件内置 TPU 逻辑，无条件注入 4 通道写死型号宏（缺省 GFU85）。
# 通过 PLATFORMIO_BUILD_FLAGS 环境变量注入（PlatformIO 自动追加到所有环境 build_flags），
# 不修改 platformio.ini。
pio_env=()
pio_env+=( BAMBU_BUS_AMS_NUM="${ams_num}" )
pio_env+=( AMS_RETRACT_LEN="${RETRACT}f" )
pio_env+=( BMCU_DM_TWO_MICROSWITCH="${AUTOLOAD}" )
pio_env+=( BMCU_ONLINE_LED_FILAMENT_RGB="${RGB}" )
pio_env+=( DBMCU_P1S="${p1s}" )
pio_env+=( BMCU_SOFT_LOAD="${softload}" )
pio_env+=( BMCU_DM_AUTO_RETRACT="${AUTO_RETRACT_FLAG}" )
pio_env+=( PLATFORMIO_BUILD_FLAGS="-DBMCU_TPU_FIX0=${TPU_FIX0} -DBMCU_TPU_FIX1=${TPU_FIX1} -DBMCU_TPU_FIX2=${TPU_FIX2} -DBMCU_TPU_FIX3=${TPU_FIX3}" )

env "${pio_env[@]}" pio run -e fw

src=".pio/build/fw/firmware.bin"
[[ -f "${src}" ]] || { echo "ERROR: 缺少 ${src}"; exit 1; }

mkdir -p "$(dirname "${out_path}")"
cp -f "${src}" "${out_path}"

# --- 复制选型指南，仅保留 README.md（目录层级与批量脚本完全一致）---
TXT_MODE="which_to_choose_mode.txt"
TXT_AUTOLOAD="which_to_choose_autoload.txt"
TXT_RGB="which_to_choose_filament_rgb.txt"
TXT_SLOTS="which_to_choose_slots.txt"
# 各层选型指南目录（cp 不会自动建父目录，先 mkdir -p）
# v4.0-tpu：选型指南目录与固件目录对齐（无 TPU 独立层，固件内置写死表不新增维度）
guide_base="single_build/"
mkdir -p "${guide_base}${mode_dir}/${dm_dir}/${rgb_dir}"
# 顶层 single_build/ 或 single_build/TPU_xxx/：模式选型指南
[[ -f "${TXT_MODE}" ]]    && cp -f "${TXT_MODE}"    "${guide_base}README.md"
# 模式层：AUTOLOAD 选型指南
[[ -f "${TXT_AUTOLOAD}" ]] && cp -f "${TXT_AUTOLOAD}" "${guide_base}${mode_dir}/README.md"
# AUTOLOAD/NO_AUTOLOAD 层：RGB 选型指南
[[ -f "${TXT_RGB}" ]]     && cp -f "${TXT_RGB}"     "${guide_base}${mode_dir}/${dm_dir}/README.md"
# FILAMENT_RGB 层：slots 选型指南（该层下所有槽位共享，与批量脚本一致）
[[ -f "${TXT_SLOTS}" ]]   && cp -f "${TXT_SLOTS}"   "${guide_base}${mode_dir}/${dm_dir}/${rgb_dir}/README.md"

echo
echo "DONE. 固件已生成: ${out_path}"
