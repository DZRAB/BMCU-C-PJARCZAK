#pragma once
#include <stdint.h>

/*
 * SSD1306 OLED 驱动（I2C，0.96" 128x64，与 AHT20 共用同一条软件 I2C 总线）
 *
 *   硬件：SSD1306，I2C 地址 0x3C（写=0x78，SA0 接地），128x64 像素。
 *   总线：与 AHT20 同挂 PB10/SCL、PB11/SDA 软件 I2C。底层【复用 AHT20 已验证
 *        稳定的 bus_start/bus_write/bus_stop】（CH32V203 上自写时序不稳，故复用）。
 *
 *   显示规格（必须严格遵守）：
 *     - 8x16 字模 => 屏幕分 4 行（行高 16px），每行 16 列（列宽 8px）。
 *     - 所有文本按 (row, col) 坐标显示，row∈[0,3]，col∈[0,15]。
 *     - 覆盖显示前必须清当前行（clear_line）或整屏（clear），否则字符叠加乱屏。
 *
 *   隔离：整个驱动被 BMCU_OLED 宏包住，默认不编译，常态固件零影响、不占 Flash。
 *        编译开关由 build_one.sh 的 OLED=1 以 -DBMCU_OLED 注入（不改动 platformio.ini）。
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

    // 整屏清（全黑）。覆盖整屏重画前调用。
    static void clear();

    // 清第 row 行（0..3），避免整屏闪烁。覆盖单行内容前调用。
    static void clear_line(uint8_t row);

    // 在 (row, col) 显示单个 8x16 字符（row∈[0,3], col∈[0,15]）。
    static void show_char(uint8_t row, uint8_t col, char ch);

    // 在 (row, col) 显示字符串，自动截断到行宽（16 字符）。
    static void show_text(uint8_t row, uint8_t col, const char* str);

    // 当前 OLED 是否初始化成功（探测 ACK）。供内容层按需判断。
    static bool is_ready() { return s_ready; }

    // ===== 内容层（界面画面，后续可继续扩展）=====
    // 约定：每个 draw_xxx 内部自行 clear_line / clear，避免叠加。

    // 温湿度画面：第0行标题，第1行温度，第2行湿度，第3行状态/留空。
    // online=false 时显示 "AHT20 OFFLINE" 提示。
    static void draw_aht20(bool online, float temperature_c, float humidity_percent);

    // 预留：开机画面 / 初始化画面 / 校准画面 / 工作模式 / 通道信息。
    // 后续实现，框架先留接口，不占逻辑。
    // static void draw_boot();
    // static void draw_init();
    // static void draw_calib();
    // static void draw_mode(uint8_t mode);
    // static void draw_channel(uint8_t ch, const char* info);

private:
    static inline bool s_ready = false;   // init() 探测结果

    // 各行上次显示内容缓存（去闪烁用）：仅当内容变化才重写该行。
    // 每行 16 字符 + 1 结束符；OLED_LINES=4 行。
    static inline char s_line_buf[OLED_LINES][OLED_COLS + 1u];

    // 仅在内容变化时才清行并重写，避免每帧整行擦写导致的闪烁。
    static void draw_line_if_changed(uint8_t row, const char* text);

    // 写命令 / 写数据（经 AHT20 总线底层）
    static void write_cmd(uint8_t c);
    static void write_data(uint8_t d);

    // 设置显示起始位置（页寻址）：page∈[0,7]，col∈[0,127]
    static void set_pos(uint8_t page, uint8_t col);
};

#endif // BMCU_OLED
