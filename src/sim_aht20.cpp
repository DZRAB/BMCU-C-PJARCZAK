#include "sim_aht20.h"
#include "ams.h"          // ams[] 数组、_ams、filament 结构

// ============================================================================
// 软件模拟 AHT20 温湿度（仅用于【无 AHT20 硬件时】的测试）
//
// 探测方案：在每次“响应打印机 filament_motion_long 查询”时（bambu_bus_ams.cpp
// 的 get_package_stu_motion 内调用本函数）自增温湿度（温度 +1℃、湿度 +10%，
// 分别到 50/100 回绕），使打印机显示随其查询周期同频跳变，便于用秒表测出
// 打印机拉取温湿度的周期。
//
// 开关（见 src/sim_aht20.h 顶部 SIM_AHB_PROBE_ENABLE）：
//   1 = 开启温湿度探测模拟；0 = 关闭（本函数直接 return 空操作）。
//
// ⚠️ AHT20 实物焊接到位后，将 SIM_AHB_PROBE_ENABLE 设为 0，
//    恢复由真实传感器驱动的温湿度上报。
// ============================================================================
void sim_aht20_probe_step(void)
{
#if (SIM_AHB_PROBE_ENABLE == 0)
    return;  // 未开启探测模式：不做任何模拟
#endif

    static int8_t  probe_t = 20;   // 探测温度，每次 +1℃，到 50 回绕到 20
    static uint8_t probe_h = 20;   // 探测湿度，每次 +10%，到 100 回绕到 20

    probe_t = (int8_t)((probe_t >= 50) ? 20 : (probe_t + 1));
    probe_h = (uint8_t)((probe_h >= 100) ? 20 : (probe_h + 10));

    for (uint8_t i = 0u; i < 4u; i++)
    {
        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_temperature = probe_t;
        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_humidity     = probe_h;
    }
}
