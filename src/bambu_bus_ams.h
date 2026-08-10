#pragma once
#include <stdint.h>

enum class bambubus_package_type
{
    error = -1,
    none = 0,
    filament_motion_short,
    filament_motion_long,
    online_detect,
    REQx6,
    NFC_detect,
    set_filament_info,
    MC_online,
    read_filament_info,
    set_filament_info_type2,
    version,
    serial_number,
    heartbeat,
    ETC,

    __BambuBus_package_packge_type_size
};

void bambubus_init(void);
void bambubus_heartbeat_seen_fast(void);
extern bambubus_package_type bambubus_run();

// ===== 通讯监控统计（定义于 bambu_bus_ams.cpp，OLED 调试页只读）=====
extern uint32_t g_pkg_recv_cnt;        // 成功解析的打印机包总数
extern uint32_t g_set_filament_cnt;    // set_filament 被调用次数
extern char     g_last_filament_id[8]; // 最近一次收到的 filament_id（如 "GFU98"）
extern uint64_t g_last_pkg_ms;         // 最近一次成功收包的时间戳（ms）