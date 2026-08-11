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
// 本文件无条件编译进固件：通用固件默认内置 TPU 逻辑，不再有"专用固件"概念。
// 各通道运行时型号由编译期写死表 TPU_FIXED_ID[4] 决定（见文件末尾），
// 仅当打印机将该通道设为 TPU for AMS（GFU98, filament_type==tpu）时生效；
// 设 PLA/PETG 等非 TPU 走正常刚性逻辑，写死表不参与。回传仍用 GFU98 不骗打印机。
// ============================================================================

#pragma once

#include <cstdint>

// ============================================================================
// 参数表与 tpu_param_lookup() 始终编译进固件（无条件），支撑"通用固件 +
// 运行时按通道识别 TPU"的主流场景。各通道编译期写死型号见文件末尾
// TPU_FIXED_ID[4]（由 BMCU_TPU_FIX0..3 宏决定，未定义时保底全 GFU85）。
// ============================================================================

// ---- TPU 型号枚举（与 Bambu filament_id 前缀/型号对应）---------------------
enum class _tpu_model : uint8_t
{
    UNKNOWN = 0,   // 未匹配到已知 TPU 型号 → 用最保守（最软）的默认参数
    TPU_FOR_AMS,   // GFU98  Bambu TPU for AMS       (68D, 最硬，AMS 常规供料)
    TPU_95A_HF,    // GFU00  Bambu TPU 95A HF        (95A, 仅 AMS HT 手动)
    TPU_GEN_AMS,   // GFU02  Generic TPU for AMS     (约68D, AMS 常规供料)
    TPU_95A,       // GFU95  Bambu TPU 95A           (95A, 仅 AMS HT 手动)
    TPU_90A,       // GFU90  Bambu TPU 90A           (90A, 仅 AMS HT 手动)
    TPU_85A,       // GFU85  Bambu TPU 85A           (85A, 最软, 禁用 PTFE 管)
};

// ---- 单型号送料参数 -------------------------------------------------------
// 字段说明（对应 Motion_control.cpp on_use 闭环的可调旋钮）：
//   on_use_target_pct : on_use 目标缓冲头压力%（原 MC_ON_USE_TARGET_PCT 约52-54）
//                       TPU 软料要调低，避免硬顶压缩而非前进。
//   on_use_band_hi    : 带宽上限%（原 MC_ON_USE_BAND_HI_PCT 约60-65）；与 target 拉开，
//                       给软料弹性留出余地，回落即恢复。
//   phase1_ms         : 三段式第 1 段"中力推一把"时长（原 2000ms）
//   phase2_ms         : 第 2 段"轻压保持"时长（原 3000ms）；总顶满阈值 = phase1_ms + phase2_ms
//   jam_ms            : 顶满累计超过该值才算真堵（原 5000ms）；软料弹性大，放宽。
//   phase1_lim        : 第 1 段 PWM 力度上限（原 600.0）
//   phase2_lim        : 第 2 段 PWM 力度上限（原 180.0）；软料要更小防啃料。
//   feed_pwm_hi       : on_use 主路 PWM 上限（推一把力度，原 MC_LOAD_S2_PWM_HI 约480-550）；软料调小防过推。
//   feed_pwm_lo       : on_use 主路 PWM 下限（持续推力上限，原 MC_LOAD_S2_PWM_LO=1000）；软料调小防啃料。
//   pull_comp_m       : 回抽弹性补偿（米），TPU 回弹，固定长度回抽额外多退一点（[待实测]）。
//   push_cycle_ms     : 间歇送料周期（ms）。TPU 软料不能被持续推力顶着, 否则料被压缩挤出缓冲头间隙。
//                       周期内分"推窗口(push_on_ms)"和"停窗口(cycle-on)", 停窗口电机停转让料松弛/被拉走。
//   push_on_ms        : 周期内正向推料窗口时长（ms）。其余时间为停窗口（PWM=0）。
//                       越软 → 周期越长、推窗口占比越小（85A 停最久, 68D 接近连续）。
struct _tpu_param
{
    _tpu_model  model;
    const char *filament_id;   // Bambu filament_id（前 4 字符匹配，如 "GFU98"）
    const char *name;          // 显示名（用于 RGB/调试）
    uint8_t rgb_r;             // v4.0-tpu: RGB 识别色（FILAMENT_RGB 宏关时用于验证识别）
    uint8_t rgb_g;
    uint8_t rgb_b;
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
    uint16_t push_cycle_ms;
    uint16_t push_on_ms;
    float feed_speed;        // v4.0-tpu: Stage1 快送目标线速度(mm/s)。PLA/PETG 用 60，软料调小防猛拽。
};

// v4.0-tpu: FILAMENT_RGB 宏关闭时，非 TPU 材质（PLA/PETG/ABS/PA/未知/other）
// 统一显示此颜色，用于和 TPU 识别色区分（"是不是 TPU"一眼可辨）。
// 用白偏蓝、低亮度，与普通状态色同档，不刺眼。
#define TPU_NON_TPU_RGB_R  0x08u
#define TPU_NON_TPU_RGB_G  0x08u
#define TPU_NON_TPU_RGB_B  0x10u

// ---- 参数表（初值按硬度分级，[待实测] 项为需实校项）-----------------------
// 硬度排序（硬→软）：68D(for AMS) > 95A > 90A > 85A
// 越软 → target 越低、band_hi 越低、力度越小、时间窗越长、回抽补偿越大。
static const _tpu_param TPU_PARAMS[] =
{
    // model             id      name           r   g   b   target band_hi p1ms p2ms jamms p1lim p2lim feedhi feedlo pullcomp  cyc  on   feed_speed
    // 注意：RGB 识别色故意调亮（满量程附近），因 WS2812 在 0x08~0x20 低亮度下肉眼几乎不可辨，
    // 调亮后才能一眼区分各型号（验证写死表是否生效）。
    //
    // v4.0-tpu 推力修正（关键）：代码里 pwm_lo=tpu_p->feed_pwm_lo(第14列), pwm_cap=pwm_fast_onuse=tpu_p->feed_pwm_hi(第13列)。
    // 非 TPU(PLA/PETG) 通道硬编码 pwm_lo=380 / pwm_cap=900 / Stage1 速度=60mm/s。
    // 设计：软料推力应 < 硬料，故所有 TPU 的 feed_pwm_lo/hi 均 < 非 TPU 的 380/900，且 Stage1 速度 < 60。
    // 越软：速度越小、推力越小。硬度升序：85A(最软) < 90A < 95A < 68D(最硬,最接近PLA)。
    { _tpu_model::TPU_FOR_AMS,  "GFU98", "TPU for AMS",  0x00u,0xFFu,0xFFu, 50.0f, 58.0f, 2000, 3000, 6000, 450.0f, 320.0f, 320.0f, 370.0f, 0.01f, 800, 500, 50.0f }, // 68D 青（最硬）
    { _tpu_model::TPU_95A_HF,   "GFU00", "TPU 95A HF",   0x00u,0xFFu,0x00u, 45.0f, 54.0f, 2500, 4000, 7000, 420.0f, 280.0f, 280.0f, 350.0f, 0.02f, 900, 450, 45.0f }, // 95A HF 绿
    { _tpu_model::TPU_GEN_AMS,  "GFU02", "Generic TPU",  0xFFu,0x00u,0xFFu, 50.0f, 58.0f, 2000, 3000, 6000, 450.0f, 320.0f, 320.0f, 370.0f, 0.01f, 800, 500, 50.0f }, // 约68D 紫（最硬）
    { _tpu_model::TPU_95A,      "GFU95", "TPU 95A",      0xFFu,0xD0u,0x00u, 45.0f, 54.0f, 2500, 4000, 7000, 400.0f, 260.0f, 260.0f, 340.0f, 0.02f, 900, 420, 45.0f }, // 95A 黄
    { _tpu_model::TPU_90A,      "GFU90", "TPU 90A",      0xFFu,0x80u,0x00u, 40.0f, 50.0f, 3000, 5000, 8000, 350.0f, 220.0f, 220.0f, 320.0f, 0.03f,1000, 400, 40.0f }, // 90A 橙
    { _tpu_model::TPU_85A,      "GFU85", "TPU 85A",      0xFFu,0x00u,0x00u, 35.0f, 46.0f, 3500, 6000, 9000, 300.0f, 180.0f, 180.0f, 300.0f, 0.04f,1200, 400, 35.0f }, // 85A 红（最软）
};
static const int TPU_PARAMS_N = (int)(sizeof(TPU_PARAMS) / sizeof(TPU_PARAMS[0]));

// v4.0-tpu: 查表返回某型号的 RGB 识别色（FILAMENT_RGB 宏关时用于验证识别）。
// 找不到（UNKNOWN）返回 0,0,0。
static inline void tpu_model_rgb(_tpu_model m, uint8_t &r, uint8_t &g, uint8_t &b)
{
    for (int i = 0; i < TPU_PARAMS_N; ++i)
    {
        if (TPU_PARAMS[i].model == m)
        {
            r = TPU_PARAMS[i].rgb_r;
            g = TPU_PARAMS[i].rgb_g;
            b = TPU_PARAMS[i].rgb_b;
            return;
        }
    }
    r = g = b = 0u;
}

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

// ---- 编译期每通道写死型号表（TPU 4 通道方案）---------------------------
// 来源：构建脚本传入 BMCU_TPU_FIX0..3（值形如 GFU98 / GFU90 / GFU95 / GFU85）。
// 未定义某通道宏时保底写死最软最稳的 GFU85，保证任何配置都能跑（不依赖打印机下发）。
// 仅在打印机将该通道设为 TPU for AMS（filament_type==tpu，即下发 GFU98）时，
// 内部用本表型号跑 TPU 软料参数；非 TPU（PLA/PETG/...）走刚性，本表不参与。
// 回传仍用 GFU98，不骗打印机，避免循环触发。
#ifndef BMCU_TPU_FIX0
#define BMCU_TPU_FIX0  GFU85
#endif
#ifndef BMCU_TPU_FIX1
#define BMCU_TPU_FIX1  GFU85
#endif
#ifndef BMCU_TPU_FIX2
#define BMCU_TPU_FIX2  GFU85
#endif
#ifndef BMCU_TPU_FIX3
#define BMCU_TPU_FIX3  GFU85
#endif

// 宏（标识符，如 GFU85）转字符串字面量，作为 tpu_param_lookup 的 key
#define _TPU_STR1(x)  #x
#define _TPU_STR(x)   _TPU_STR1(x)

// 每通道编译期写死型号字符串表（下标 0..3 对应 CH0..CH3）
static const char *const TPU_FIXED_ID[4] =
{
    _TPU_STR(BMCU_TPU_FIX0),
    _TPU_STR(BMCU_TPU_FIX1),
    _TPU_STR(BMCU_TPU_FIX2),
    _TPU_STR(BMCU_TPU_FIX3),
};

// 取通道 ch(0..3) 的写死型号参数指针（供 on_use 闭环与 RGB 识别色使用）
static inline const _tpu_param *tpu_param_fixed(uint8_t ch)
{
    if (ch >= 4) ch = 3;
    return tpu_param_lookup(TPU_FIXED_ID[ch]);
}
