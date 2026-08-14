#pragma once
#include <stdint.h>

// v4.0-tpu 方案A: 打印完成后定时软复位(模拟拔插) 总开关
//   默认 0(关闭): 仅【部分机型】打印完成后需要软复位来恢复在线, 绝大多数机型不需要;
//      故交由编译脚本注入(环境变量 BMCU_AUTO_REBOOT_ENABLE=1)针对特定机型开启, 不默认开。
//   注意: 用 #ifndef 包裹, 脚本通过 -DBMCU_AUTO_REBOOT_ENABLE=1 覆盖; main.cpp / bambu_bus_ams.cpp 共用
#ifndef BMCU_AUTO_REBOOT_ENABLE
#define BMCU_AUTO_REBOOT_ENABLE 0
#endif

// v4.0-tpu: TPU 送料逻辑总开关
//   1 = 开启(精准按通道隔离): 仅"被打印机设为 TPU(filament_type==tpu)"的通道走 TPU 写死表分支;
//      设 PLA/PETG/ABS 等非 TPU 通道 tpu_p==nullptr -> 走 v3.2.1-fix105 原刚性常量(零差异, 不污染);
//      设 TPU 再改回 PETG -> filament_type 变 petg -> 完全恢复 v3.2.1 行为。
//      另外 RGB_OFF 模式下, 设 TPU 的通道会显示"内部真实写死表型号"专属色(用户预期功能)。
//   TPU 判定只看运行期 filament_type(set_filament 实时写入, 拔料清 unknown), 不依赖 Flash 残留型号,
//   故相互绝对隔离、可随时切回。当前默认 1(出稳定版: 不设 TPU 即全走 v3.2.1, 行为稳定)。
//   用 #ifndef 包裹, 脚本可通过 -DBMCU_TPU_ENABLE=0 关闭(老主板不需要 TPU 逻辑时)
#ifndef BMCU_TPU_ENABLE
#define BMCU_TPU_ENABLE 1
#endif

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
// 动作指令显示缓存（v4.0-tpu 调试精简）：仅 PREP/FEED/STOP/PREU/LOAD 等
// 动作前后指令写入，周期轮询(SN/VER/RD/RFID/ONL/MC)与纯心跳已过滤，供抓包页稳定显示。
extern char     g_last_act_rx_label[16];
extern uint64_t g_last_act_rx_ms;
extern char     g_last_act_rx_raw[40];
// 未知指令专用缓存（v4.0-tpu 调试）：代码未登记的类型（如可能的"成功/暂停/停止"）。
// 不过滤、抓包页以 '?' 前缀特别显示，g_unk_cnt 累计次数。
extern char     g_last_unk_label[8];
extern uint64_t g_last_unk_ms;
extern char     g_last_unk_raw[40];
extern uint32_t g_unk_cnt;
// 未知指令环形缓冲：最近 UNK_RING_N 条原始片段，抓包页第2行轮显。
// 调试模式(BMCU_OLED_DEBUG)扩展为 24 条，供完整记录页（draw_unk_log）使用；否则 4 条省 RAM。
#ifdef BMCU_OLED_DEBUG
#define UNK_RING_N 24u
#else
#define UNK_RING_N 4u
#endif
extern char     g_unk_ring[UNK_RING_N][40];
extern uint64_t g_unk_ring_ms[UNK_RING_N];
extern uint32_t g_unk_seq[UNK_RING_N];   // 每条未知指令的全局递增序号（记录页按序显示）
extern uint32_t g_unk_seq_next;          // 全局序号发生器（写入时 ++）
extern uint8_t  g_unk_ring_head;
extern uint8_t  g_unk_ring_cnt;
extern void oled_log_rx(const char *label, const char *raw);
extern void oled_log_tx(const char *label);