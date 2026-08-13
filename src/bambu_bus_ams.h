#pragma once
#include <stdint.h>

// v4.0-tpu 方案A: 打印完成后定时软复位(模拟拔插) 总开关
//   正式版保持开启(=1); TPU 测试阶段临时关闭(=0), 不动编译脚本
//   注意: 改这里即全工程生效(main.cpp / bambu_bus_ams.cpp 共用)
#define BMCU_AUTO_REBOOT_ENABLE 1

// v4.0-tpu: TPU 送料逻辑总开关
//   1 = 开启(精准按通道隔离): 仅"被打印机设为 TPU(filament_type==tpu)"的通道走 TPU 写死表分支;
//      设 PLA/PETG/ABS 等非 TPU 通道 tpu_p==nullptr -> 走 v3.2.1-fix105 原刚性常量(零差异, 不污染);
//      设 TPU 再改回 PETG -> filament_type 变 petg -> 完全恢复 v3.2.1 行为。
//      另外 RGB_OFF 模式下, 设 TPU 的通道会显示"内部真实写死表型号"专属色(用户预期功能)。
//   TPU 判定只看运行期 filament_type(set_filament 实时写入, 拔料清 unknown), 不依赖 Flash 残留型号,
//   故相互绝对隔离、可随时切回。当前默认 1(出稳定版: 不设 TPU 即全走 v3.2.1, 行为稳定)。
#define BMCU_TPU_ENABLE 1

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