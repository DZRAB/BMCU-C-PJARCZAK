#ifndef SIM_AHT20_H
#define SIM_AHT20_H

// 软件模拟 AHT20 温湿度（仅用于【无 AHT20 硬件时】的测试）
// 在 main.cpp 主循环中调用 sim_aht20_run()，每 5 秒刷新一次温湿度。
// AHT20 实物焊接到位后，注释掉 main.cpp 中对本函数的调用即可。
void sim_aht20_run(void);

#endif // SIM_AHT20_H
