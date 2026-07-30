#include "sim_aht20.h"
#include "ams.h"          // ams[] 数组、_ams、filament 结构
#include "hal/time_hw.h"  // time_ms64()

// ============================================================================
// 软件模拟 AHT20 温湿度（仅用于【无 AHT20 硬件时】的测试）
//
// - 每 5 秒更新一次 ams[].filament[].compartment_temperature / compartment_humidity
// - 温度在 20~30℃ 之间、湿度在 30~70% 之间做三角波往返变化，便于上位机观察刷新
// - 纯整数逻辑，不依赖任何数学库 / 浮点运算
//
// ⚠️ AHT20 实物焊接到位后，请注释掉 main.cpp 中对 sim_aht20_run() 的调用，
//    恢复由真实传感器驱动的温湿度上报。
// ============================================================================
void sim_aht20_run(void)
{
    static uint64_t next_ms = 0u;
    const uint64_t now = time_ms64();

    // 每 5 秒刷新一次
    if ((now - next_ms) < 5000u)
    {
        return;
    }
    next_ms = now;

    // 三角波状态：在上下限之间往返
    static int8_t  t     = 20;   // 当前模拟温度 ℃
    static int8_t  t_dir = 1;    // 温度变化方向 (+1/-1)
    static uint8_t h     = 30;   // 当前模拟湿度 %
    static int8_t  h_dir = 1;    // 湿度变化方向 (+1/-1)

    t += t_dir;
    if (t >= 30)      { t = 30; t_dir = -1; }
    else if (t <= 20) { t = 20; t_dir =  1; }

    h = (uint8_t)(h + h_dir);
    if (h >= 70)      { h = 70; h_dir = -1; }
    else if (h <= 30) { h = 30; h_dir =  1; }

    // 写入当前 AMS 槽位下的 4 个 filament，供上位机通过协议读取
    for (uint8_t i = 0u; i < 4u; i++)
    {
        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_temperature = t;
        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_humidity     = h;
    }
}
