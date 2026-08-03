// ============================================================================
// tpu_params.h  ——  v4.0-tpu  专用：TPU 软料送料参数表
// ----------------------------------------------------------------------------
// 背景：BMCU 的 on_use 送料闭环原本假设料是"刚性、低摩擦、可压缩性小"的
//       （PLA/PETG 接近）。TPU 是高弹性、高摩擦、易堆料的软料，直接套用原
//       参数会导致缓冲头（pressure）判定失真、误报堵料、啃料。
//
// 本文件提供一张"以 Bambu filament_id 为 key 的 TPU 送料参数表"，每个型号
// 对应一套独立参数（目标压力带、送料力度上限、三段式顶满时间窗、回抽补偿）。
//
// 数据来源：
//   - filament_id ↔ 型号对应：Bambu Studio 源码 DeviceManager.cpp（GFU 前缀 = TPU）
//   - 各型号 Shore 硬度 / AMS 适用性 / 进料难度：Bambu 官方 Wiki（H2 TPU 指南）
//
// 重要：下表中的数值为"基于硬度分级的初值占位"，并非最终校准值。
//       最终每个型号的参数需由用户在成品板上实测迭代后填定（无串口，
//       只能看动作 + RGB 灯判断）。标有 [待实测] 的项即为需要校准的项。
//
// 本文件只在定义了 BMCU_TPU_MODEL 时被包含；未定义时整个 v4.0 TPU 逻辑
// 不编译，固件行为与 v3.2 完全一致（零差异）。
// ============================================================================

#pragma once

#include <cstdint>

// ============================================================================
// 注意：自 v4.0-tpu 起，参数表与 tpu_param_lookup() 始终编译进固件（无条件），
// 以支撑"通用固件 + 运行时按通道识别 TPU"的主流场景。只有当需要"专用固件"
// （编译期强制某型号、供不支持下发材料型号的打印机使用）时才需定义 BMCU_TPU_MODEL。
// 未定义 BMCU_TPU_MODEL 且运行时无任何通道识别为 TPU 时，固件行为 = v3.2（零差异）。
// ============================================================================

// ---- TPU 型号枚举（与 Bambu filament_id 前缀/型号对应）---------------------
enum class _tpu_model : uint8_t
{
    UNKNOWN = 0,   // 未匹配到已知 TPU 型号 → 用最保守（最软）的默认参数
    TPU_FOR_AMS,   // GFU98  Bambu TPU for AMS       (68D, 最硬，AMS 常规供料)
    TPU_95A_HF,    // GFU00  Bambu TPU 95A HF        (95A, 仅 AMS HT 手动)
    TPU_GEN_AMS,   // GFU02  Generic TPU for AMS     (~68D, AMS 常规供料)
    TPU_95A,       // GFU95  Bambu TPU 95A           (95A, 仅 AMS HT 手动)
    TPU_90A,       // GFU90  Bambu TPU 90A           (90A, 仅 AMS HT 手动)
    TPU_85A,       // GFU85  Bambu TPU 85A           (85A, 最软, 禁用 PTFE 管)
};

// ---- 单型号送料参数 -------------------------------------------------------
// 字段说明（对应 Motion_control.cpp on_use 闭环的可调旋钮）：
//   on_use_target_pct : on_use 目标缓冲头压力%（原 MC_ON_USE_TARGET_PCT ~52-54）
//                       TPU 软料要调低，避免硬顶压缩而非前进。
//   on_use_band_hi    : 带宽上限%（原 MC_ON_USE_BAND_HI_PCT ~60-65）；与 target 拉开，
//                       给软料弹性留出余地，回落即恢复。
//   phase1_ms         : 三段式第 1 段"中力推一把"时长（原 2000ms）
//   phase2_ms         : 第 2 段"轻压保持"时长（原 3000ms）；总顶满阈值 = phase1_ms + phase2_ms
//   jam_ms            : 顶满累计超过该值才算真堵（原 5000ms）；软料弹性大，放宽。
//   phase1_lim        : 第 1 段 PWM 力度上限（原 600.0）
//   phase2_lim        : 第 2 段 PWM 力度上限（原 180.0）；软料要更小防啃料。
//   feed_pwm_hi       : on_use 主路 PWM 上限（推一把力度，原 MC_LOAD_S2_PWM_HI ~480-550）；软料调小防过推。
//   feed_pwm_lo       : on_use 主路 PWM 下限（持续推力上限，原 MC_LOAD_S2_PWM_LO=1000）；软料调小防啃料。
//   pull_comp_m       : 回抽弹性补偿（米），TPU 回弹，固定长度回抽额外多退一点（[待实测]）。
struct _tpu_param
{
    _tpu_model  model;
    const char *filament_id;   // Bambu filament_id（前 4 字符匹配，如 "GFU98"）
    const char *name;          // 显示名（用于 RGB/调试）
    float on_use_target_pct;
    float on_use_band_hi;
    uint16_t phase1_ms;
    uint16_t phase2_ms;
    uint16_t jam_ms;
    float phase1_lim;
    float phase2_lim;
    float feed_pwm_hi;
    float feed_pwm_lo;
    float pull_comp_m;
};

// ---- 参数表（初值按硬度分级，[待实测] 项为需实校项）-----------------------
// 硬度排序（硬→软）：68D(for AMS) > 95A > 90A > 85A
// 越软 → target 越低、band_hi 越低、力度越小、时间窗越长、回抽补偿越大。
static const _tpu_param TPU_PARAMS[] =
{
    // model             id      name                target band_hi p1ms p2ms jamms p1lim p2lim feedhi feedlo pullcomp
    { _tpu_model::TPU_FOR_AMS,  "GFU98", "TPU for AMS",   50.0f, 58.0f, 2000, 3000, 6000, 520.0f, 160.0f, 440.0f, 950.0f, 0.01f }, // 68D, 接近刚性
    { _tpu_model::TPU_95A_HF,   "GFU00", "TPU 95A HF",    45.0f, 54.0f, 2500, 4000, 7000, 420.0f, 120.0f, 400.0f, 900.0f, 0.02f }, // 95A HF
    { _tpu_model::TPU_GEN_AMS,  "GFU02", "Generic TPU",   50.0f, 58.0f, 2000, 3000, 6000, 520.0f, 160.0f, 440.0f, 950.0f, 0.01f }, // ~68D, 同 for AMS
    { _tpu_model::TPU_95A,      "GFU95", "TPU 95A",       45.0f, 54.0f, 2500, 4000, 7000, 420.0f, 120.0f, 400.0f, 900.0f, 0.02f }, // 95A
    { _tpu_model::TPU_90A,      "GFU90", "TPU 90A",       40.0f, 50.0f, 3000, 5000, 8000, 360.0f, 100.0f, 360.0f, 850.0f, 0.03f }, // 90A
    { _tpu_model::TPU_85A,      "GFU85", "TPU 85A",       35.0f, 46.0f, 3500, 6000, 9000, 300.0f,  80.0f, 320.0f, 800.0f, 0.04f }, // 85A, 最软最保守
};
static const int TPU_PARAMS_N = (int)(sizeof(TPU_PARAMS) / sizeof(TPU_PARAMS[0]));

// ---- 根据 filament_id（字符串）查参数表的运行时常量指针 -------------------
// 匹配前 4 字符（Bambu filament_id 形如 "GFU98"）。找不到返回最软的默认项
// （TPU_85A），保证"即使是未知 TPU 也走最保守参数"而不是原刚性参数。
// 该函数无条件可用：通用固件运行时按通道 bambubus_filament_id 查表。
static inline const _tpu_param *tpu_param_lookup(const char *filament_id)
{
    if (filament_id == nullptr)
        return &TPU_PARAMS[TPU_PARAMS_N - 1];   // 默认最软
    // 比较前 4 字符（filament_id 至少 4 字符）
    for (int i = 0; i < TPU_PARAMS_N; ++i)
    {
        const char *id = TPU_PARAMS[i].filament_id;
        if (filament_id[0] == id[0] && filament_id[1] == id[1] &&
            filament_id[2] == id[2] && filament_id[3] == id[3])
            return &TPU_PARAMS[i];
    }
    return &TPU_PARAMS[TPU_PARAMS_N - 1];        // 未知 TPU → 最软默认
}

// ---- 编译期选定的型号（来自 BMCU_TPU_MODEL 宏，值形如 GFU98）------------
// 仅用于"专用固件"：构建脚本传入 -D BMCU_TPU_MODEL=GFU98（宏展开为标识符
// GFU98，非字符串）。用 STRINGIFY 将其转为字符串字面量 "GFU98" 作为 key。
// 编译时强制以该型号参数运行（解决"打印机不支持 TPU 设置"的场景），
// 同时运行时仍读取 bambubus_filament_id 用于 RGB/兼容（见 bambu_bus_ams.cpp）。
// 未定义 BMCU_TPU_MODEL 时（通用固件默认），下方接口不存在，on_use 闭环改为
// 运行时按通道 filament_type 识别 TPU 并查表。
#ifdef BMCU_TPU_MODEL
#define _TPU_STR1(x)  #x
#define _TPU_STR(x)   _TPU_STR1(x)
#define TPU_SELECTED_ID  _TPU_STR(BMCU_TPU_MODEL)

// 编译期选定的 TPU 参数（专供"专用固件"的 on_use 闭环直接使用）
static inline const _tpu_param *tpu_param_selected(void)
{
    return tpu_param_lookup(TPU_SELECTED_ID);
}
#endif // BMCU_TPU_MODEL
