#pragma once
#include <stdint.h>

#include "ams.h"   // 提供 ams_max_number 与 _filament_motion(方案A嗅探表需要)

#define xMCU

enum class ahubus_package_type : uint8_t
{
    heartbeat = 0x01,
    query = 0x02,
    set = 0x03,
    none,
    error,
};
enum class ahubus_set_type : uint8_t
{
    filament_info = 0x02,
    dryer_stu = 0x05,
    all_filament_stu=0x06,
};

extern void ahubus_init();
extern ahubus_package_type ahubus_run();

// v4.0-tpu 方案A: 嗅探总线上其它 AMS/BMCU 的 motion 状态, 用于判定"打印是否真完成"
// (仅当总线上所有 AMS 所有电机都 idle, 才认为打印真结束 —— 并联切换时另一台在工作则不会误触发复位)
extern uint8_t g_other_ams_motion[ams_max_number][4]; // 每 AMS 每通道 motion(0x7F掩码); 0xFF=尚未嗅探到该 AMS
extern uint8_t g_other_ams_seen[ams_max_number];      // 最近是否嗅探到该 AMS 在线(1=见过响应, 0=总线无此设备/忽略)
extern void    ahubus_sniff_other_ams(uint8_t* buf);  // 在 ahubus_run 内对每个 0x33 包旁路调用, 扒其它 AMS 的响应
extern bool    all_remote_ams_all_idle(void);         // 所有嗅探到的别的 AMS 的所有通道均 idle 才返回 true