#include "sim_aht20.h"
#include "ams.h"          // ams[] 数组、_ams、filament 结构
#include "hal/time_hw.h"  // time_ms64()

// ============================================================================
// 软件模拟 AHT20 温湿度（仅用于【无 AHT20 硬件时】的测试）
//
// - 每 5 秒更新一次 ams[].filament[].compartment_temperature / compartment_humidity
// - 温度在 20~50℃ 之间、每次变化 5℃ 循环；湿度在 10~100% 之间、每次变化 10% 循环
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

    // 三角波状态：在上下限之间按步进往返循环
    static int8_t  t     = 20;   // 当前模拟温度 ℃
    static int8_t  t_dir = 5;    // 温度变化步进 (+5/-5)
    static uint8_t h     = 10;   // 当前模拟湿度 %
    static int8_t  h_dir = 10;   // 湿度变化步进 (+10/-10)

    t += t_dir;
    if (t >= 50)      { t = 50; t_dir = -5; }
    else if (t <= 20) { t = 20; t_dir =  5; }

    h = (uint8_t)(h + h_dir);
    if (h >= 100)     { h = 100; h_dir = -10; }
    else if (h <= 10) { h = 10;  h_dir =  10; }

    // 写入当前 AMS 槽位下的 4 个 filament，供上位机通过协议读取
    for (uint8_t i = 0u; i < 4u; i++)
    {
        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_temperature = t;
        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_humidity     = h;
    }
}
