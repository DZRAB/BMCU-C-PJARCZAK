#pragma once
#include <stdint.h>
#include "ams.h"           // _filament_motion / _filament_type / ams[]（OLED 内容层只读通道数据）
#include "tpu_params.h"    // TPU_FIXED_ID[4] v4.0-tpu 写死型号表
#include "bambu_bus_ams.h" // v4.0-tpu 抓包：g_last_rx_label / g_last_tx_label 等全局（只读）

/*
 * SSD1306 OLED 驱动（I2C，0.96" 128x64，与 AHT20 共用同一条软件 I2C 总线）
 *
 *   硬件：SSD1306，I2C 地址 0x3C（写=0x78，SA0 接地），128x64 像素。
 *   总线：与 AHT20 同挂 PB10/SCL、PB11/SDA 软件 I2C。底层【复用 AHT20 已验证
 *        稳定的 bus_start/bus_write/bus_stop】（CH32V203 上自写时序不稳，故复用）。
 *
 *   显示规格（必须严格遵守）：
 *     - 分辨率 0.96" 128×64；8×16 字模 => 4 行，每行 16 字符（行高 16px，列宽 8px）。
 *     - OLED 是按坐标（行/列）生成显示的，不是行缓冲自动换行。
 *     - 覆盖显示前必须先 clear()（或清当前行区域 clear_line），否则字符叠加乱屏。
 *     - 显示位置要算准：列 = 字符序号 × 8（像素），行 = 行号 × 16（像素）；
 *       或按页寻址写命令 0xB0 + 页号（每页 8px，两页拼一行 16px）。
 *     - 所有文本按 (row, col) 坐标显示，row∈[0,3]，col∈[0,15]，row 与像素行号换算遵循上一条。
 *
 *   默认编译：本驱动默认即编入固件（头文件顶部已默认 #define BMCU_OLED），
 *        运行时自动探测屏是否存在（ACK），无屏则跳过显示、零影响。
 *        若确不需要 OLED，注释掉头文件顶部那行 #define BMCU_OLED 即可（不改动 platformio.ini）。
 *
 *   分层设计：
 *     - 驱动层（driver）：init / clear / clear_line / show_char / show_text / 坐标换算。
 *       不含任何业务逻辑，纯屏幕操作。
 *     - 内容层（content）：各种画面 draw_xxx()，调用驱动层拼出界面。
 *       当前仅实现 draw_aht20（温湿度）；后续扩展 draw_boot / draw_init / draw_calib /
 *       draw_mode / draw_channel 等，只需在内容层增函数，不动驱动层。
 *
 *   字模：移植自江协科技 OLED 库 8x16 ASCII（可见字符 0x20~0x7E）。
 */

// 默认编入 OLED 驱动；如不需要，直接注释掉下面这三行（使 BMCU_OLED 不被定义，
// 则下方 #ifdef BMCU_OLED 整段不编译）即可，无需改 platformio.ini。
#ifndef BMCU_OLED
#define BMCU_OLED
#endif

#ifdef BMCU_OLED

// ---------- 屏幕几何常量 ----------
#define OLED_W       128u
#define OLED_H       64u
#define OLED_LINE_H  16u   // 每行 16 像素（8x16 字模）
#define OLED_LINES   (OLED_H / OLED_LINE_H)   // 4 行
#define OLED_COLS    16u   // 每行 16 个 8px 字符

// OLED I2C 地址（7 位）。本模块 SA0 接地 = 0x3C（写地址 0x78）。可用 -DBMCU_OLED_ADDR 覆盖。
#ifndef BMCU_OLED_ADDR
#define BMCU_OLED_ADDR  0x3Cu
#endif

class SSD1306_OLED
{
public:
    // ===== 驱动层（纯屏幕操作，无业务逻辑）=====

    // 初始化 OLED：复用 AHT20 总线（g_aht20.init() 已在 main 中调用，引脚已配置）。
    // 发送完整初始化命令序列并清屏。无论是否 ACK 都发序列（部分模块边缘但能点亮）。
    static void init();

    // 运行时探测 OLED ACK（不修改 s_ready，仅返回是否应答）。供热插拔/掉线检测。
    static bool probe_ack();

    // 整屏清（全黑）：仅清空后台 framebuffer（s_fb），不发 I2C。
    static void clear();

    // 清第 row 行（0..3）：仅清空 framebuffer 对应两页，不发 I2C。
    static void clear_line(uint8_t row);

    // 在 (row, col) 显示单个 8x16 字符（row∈[0,3], col∈[0,15]）：写入 framebuffer。
    static void show_char(uint8_t row, uint8_t col, char ch);

    // 在 (row, col) 显示字符串，自动截断到行宽（16 字符）：写入 framebuffer。
    static void show_text(uint8_t row, uint8_t col, const char* str);

    // 差值刷新：把 framebuffer（s_fb）与已发送帧（s_fb_prev）做逐字节比对，
    // 仅把变化字节经 I2C 发给 SSD1306。不变区域零 I2C 流量、永不整屏清 => 不闪、低耗。
    // 由绘制完成后（tick 末尾）统一调用一次。
    static void flush();

    // 当前 OLED 是否初始化成功（探测 ACK）。供内容层按需判断。
    static bool is_ready() { return s_ready; }

    // ===== 内容层（界面画面，后续可继续扩展）=====
    // 约定：每个 draw_xxx 内部自行 clear_line / clear，避免叠加。

    // 温湿度画面：第0行标题，第1行温度，第2行湿度，第3行状态/留空。
    // aht20_present=false 时（板上无 AHT20），不显示任何温湿度，改显 "NO AHT20" 提示；
    // aht20_present=true 但 online=false 时显示 "AHT20 OFFLINE" 提示。
    static void draw_aht20(bool aht20_present, bool online, float temperature_c, float humidity_percent, bool comm_ok);

    // 通用提示画面：4 行文本（每行最多 16 字符，不足填空串）。用于无 AHT20 时显示
    // 系统状态（如 "NO AHT20" / 工作模式 / 通道等），由各调用方自行组织文本。
    static void draw_message(const char* line0, const char* line1,
                             const char* line2, const char* line3);

    // ===== 多页面轮询 + 动作覆盖显示（v4.0 OLED 增强）=====
    //
    // 设计目标（用户需求）：
    //   - 不破坏现有结构，仅在内容层新增页面；无 OLED / 无 AHT20 时零影响。
    //   - 轮询显示：AHT20 温湿度 -> 四通道概览 -> TPU 写死型号；每页停留数秒自动切。
    //   - 动作优先：进料/退料/送料等动作发生时，立即覆盖当前页显示，持续数秒后
    //     自动回到轮询。无论当前显示什么，动作都插队最优先。
    //
    // 业务数据来源（外部全局，OLED 只读）：
    //   - ams[BAMBU_BUS_AMS_NUM].filament[ch] : 通道材质/颜色/运动态/米数/TPU型号
    //   - g_aht20 : 温湿度（含 is_online() 判定板上是否有 AHT20）
    //   - TPU_FIXED_ID[4] : v4.0-tpu 每通道写死型号（tpu_params.h）

    // 页面索引（轮询顺序）
    enum class oled_page : uint8_t
    {
        page_aht20 = 0,   // 温湿度（含写死 TPU 摘要）
        page_channels,    // 四通道概览（材质/颜色/有无料/运动态）
        page_comm,        // 通讯监控（BMCU<->打印机 收包/设料统计）
        page_sniffer,     // v4.0-tpu 抓包页（RX/TX 指令记录）
        page_count
    };

    // 动作类型（用于覆盖显示）
    enum class oled_action : uint8_t
    {
        action_none = 0,
        action_load,     // 进料（send_out / before_on_use / on_use 起始送料）
        action_unload,   // 退料（pull_back / before_pull_back）
        action_feed,     // 送料（on_use 正常供料中）
        action_idle      // 空闲/停止
    };

    // 通道动作通知：由业务层（bambu_bus_ams.cpp 设置 motion 时）调用。
    // 触发后 OLED 立即覆盖显示该通道动作，约 OLED_ACTION_HOLD_MS 后恢复轮询。
    // 无 OLED 时本函数内部直接返回，对业务零影响。
    static void notify_action(uint8_t ch, oled_action act);

    // 由主循环每秒左右调用一次：内部处理页面轮询与动作覆盖的调度与绘制。
    // 无 OLED 时直接返回。aht20_present/on_line 用于温湿度页正确显示。
    static void tick(bool aht20_present, bool aht20_online,
                     float temperature_c, float humidity_percent,
                     bool comm_ok);

    // 四通道概览页：每行一个通道，显示 [材质/颜色/有无料/运动态]。
    static void draw_channels();

    // 通讯监控页：显示 BMCU<->打印机 通讯统计（COMM 状态/总收包/设料次数/最近料号）。
    static void draw_comm(bool comm_ok);
    // v4.0-tpu 抓包共用绘制（draw_sniffer 与指令霸屏共用），4 行 = RX/TX 标签+秒 / 原始片段 / 计数
    static void draw_pkt_overlay(uint64_t now);

    // v4.0-tpu 抓包页：显示最近一次 RX(打印机->BMCU)/TX(BMCU->打印机) 指令短标签与时间戳，
    // 以及收包/设料累计计数，便于调试记录打印机与 BMCU 的通讯内容。
    static void draw_sniffer();


    // 把 RGB 转成 3 字母颜色简写（RED/GRE/YEL/BLU/CYA/MAG/PUR/ORA/WHI/BLA）。
    static const char* color_name(uint8_t r, uint8_t g, uint8_t b);

private:
    static inline bool s_ready = false;   // init() 探测结果

    // 后台显存 framebuffer：模拟整块 OLED 显存（8 页 × 128 列），所有绘制先写这里，
    // 再由 flush() 只把与 s_fb_prev 不同的字节发到 SSD1306。这是去闪+降耗的核心。
    static inline uint8_t s_fb[8][OLED_W];      // 当前帧（绘制目标）
    static inline uint8_t s_fb_prev[8][OLED_W]; // 已发送到 OLED 的上一帧（差值基准）

    // 各行上次显示内容缓存（去闪烁用）：仅当内容变化才重写该行。
    // 每行 16 字符 + 1 结束符；OLED_LINES=4 行。
    static inline char s_line_buf[OLED_LINES][OLED_COLS + 1u];

    // 页面轮询状态
    static inline oled_page s_page = oled_page::page_aht20;
    static inline uint64_t  s_page_next_ms = 0;        // 下次切页时间
    static constexpr  uint64_t OLED_PAGE_DWELL_MS = 5000u;   // 每页停留（放慢，便于看清）
    static constexpr  uint64_t OLED_ACTION_HOLD_MS = 2500u;  // 动作覆盖持续
    static constexpr  uint64_t OLED_PKT_HOLD_MS = 3000u;     // v4.0-tpu 指令霸屏持续（3秒自动回轮询）

    // 动作覆盖状态
    static inline oled_action s_action = oled_action::action_none;
    static inline uint8_t     s_action_ch = 0xFFu;
    static inline uint64_t    s_action_until_ms = 0;   // 覆盖结束时间

    // v4.0-tpu 指令霸屏（抓包）：最近一次"有意义"的 RX 指令在 OLED_PKT_HOLD_MS 内时，
    // 覆盖显示该 RX 指令内容（RX/TX 标签 + 原始片段），超时自动回轮询；高频包(ONL/HB)不霸屏。
    static inline uint64_t    s_pkt_until_ms = 0;      // 指令霸屏结束时间


    // 把运动态枚举转成屏上短标签（≤4字符）
    static const char* motion_label(_filament_motion m);

    // 把材质/TPU型号转成屏上短标签
    static const char* material_label(uint8_t ch);

    // 仅在内容变化时才清行并重写，避免每帧整行擦写导致的闪烁。
    static void draw_line_if_changed(uint8_t row, const char* text);

    // 写命令 / 写数据（经 AHT20 总线底层）
    static void write_cmd(uint8_t c);
    static void write_data(uint8_t d);

    // 设置显示起始位置（页寻址）：page∈[0,7]，col∈[0,127]
    static void set_pos(uint8_t page, uint8_t col);
};

#endif // BMCU_OLED
