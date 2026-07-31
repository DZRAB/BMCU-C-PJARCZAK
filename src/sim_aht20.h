#ifndef SIM_AHT20_H
#define SIM_AHT20_H

// 软件模拟 AHT20 温湿度（仅用于【无 AHT20 硬件时】的测试）
//
// 温湿度探测方案：在每次“响应打印机 filament_motion_long 查询”时
// （bambu_bus_ams.cpp 的 get_package_stu_motion 内调用 sim_aht20_probe_step()）
// 自增温湿度（温度 +1℃、湿度 +10%，分别到 50/100 回绕），使打印机显示
// 随其查询周期同频跳变，便于用秒表测出打印机拉取温湿度的周期。
//
// 【开关】直接改下面这行的 1/0 即可，无需任何命令行或编译宏传参：
//   1 = 开启温湿度探测模拟；0 = 关闭（sim_aht20_probe_step 变为空操作）。
// 改完保存，重新编译即可。对所有编译方式（pio / 全量脚本 / 快速脚本）通用。
// ⚠️ AHT20 实物焊接到位后，将本开关设为 0，恢复由真实传感器驱动温湿度上报。
#define SIM_AHB_PROBE_ENABLE 0

void sim_aht20_probe_step(void);

#endif // SIM_AHT20_H
