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

// ===== v4.0-tpu 抓包：最近一次 RX(打印机->BMCU) / TX(BMCU->打印机) 指令短标签 =====
// OLED 抓包页(page_sniffer)只读；bambu_bus_ams.cpp 在各 RX 分发 / TX 发送点写入。
// 标签为 3~4 字符缩写，例如 "MOT"(运动) "RFID"(设料) "VER"(版本) "RD"(读料) "SN"(序列号)
// "ONL"(在线检测) "MC"(MC上线) "STU"(状态) "HB"(心跳) 等，便于调试记录。
extern char     g_last_rx_label[8];    // 最近 RX 指令短标签
extern uint64_t g_last_rx_ms;          // 最近 RX 时间戳(ms)
extern char     g_last_tx_label[8];    // 最近 TX 指令短标签
extern uint64_t g_last_tx_ms;          // 最近 TX 时间戳(ms)
extern uint32_t g_rx_cnt;              // RX 指令累计数(不含心跳/在线等高频包可选)
extern uint32_t g_tx_cnt;              // TX 指令累计数
// 记录最近一次收到的原始指令片段(用于抓包页显示, 仅截前部, 不全文)
extern char     g_last_rx_raw[40];
// 最近"重要指令"独立缓存：心跳包频刷会覆盖 g_last_rx_label，重要指令(非 ONL/MC/MOT/STU)
// 单独留存，抓包页/霸屏优先显示，稳定不被心跳覆盖。
extern char     g_last_imp_rx_label[8];
extern uint64_t g_last_imp_rx_ms;
extern char     g_last_imp_rx_raw[40];
extern void oled_log_rx(const char *label, const char *raw);
extern void oled_log_tx(const char *label);