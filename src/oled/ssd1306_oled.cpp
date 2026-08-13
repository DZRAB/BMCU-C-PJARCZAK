// SSD1306 OLED（128x64, I2C, 4引脚）驱动 —— 软件 I2C 复用 AHT20 总线
//
#include "bambu_bus_ams.h"   // 通讯监控：读取 g_pkg_recv_cnt / g_set_filament_cnt / g_last_filament_id
//
// 设计要点：
//  - OLED 与 AHT20 共用同一条软件 I2C 总线（PB10/SCL, PB11/SDA），地址不同不冲突。
//  - 底层软件 I2C 时序【复用 AHT20 已验证稳定的 bus_start/bus_write/bus_stop】，
//    OLED 驱动不再自写一套（CH32V203 上自写时序不稳，导致屏不亮）。
//  - g_aht20 是全局实例（main.cpp 定义，去除 static 后 extern 可见），其 bus_* 为 public 转发。
//  - 字模移植自江协科技 OLED 库（STM32 参考），仅作字模数据来源；本文件不 #include 也不调用
//    该参考库的任何函数。实际硬件为 BMCU 主板 CH32V203。
//  - 显示规格：8x16 字模 => 4 行 x 16 列；所有文本按 (row,col) 坐标显示；
//    覆盖显示前必须 clear_line / clear，避免叠加乱屏。

#include "ssd1306_oled.h"
#include "aht20/aht20.h"   // AHT20 类：bus_start/bus_write/bus_stop + 全局 g_aht20
#include "hal/time_hw.h"   // delay()
#include "ams.h"           // ams[] / _filament / _filament_motion / BAMBU_BUS_AMS_NUM（只读通道数据）
#include "tpu_params.h"    // TPU_FIXED_ID[4] v4.0-tpu 写死型号表

#ifdef BMCU_OLED

// g_aht20 全局实例在 main.cpp 定义，此处 extern 引用（复用其软件 I2C 底层）
extern AHT20 g_aht20;

// ---------- 轻量字符串辅助（避免引入 <cstring>，省 Flash）----------
static bool str_eq(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return (*a == *b);
}
static void str_cpy(char* dst, const char* src)
{
    if (!dst) return;
    if (!src) { *dst = 0; return; }
    while (*src) *dst++ = *src++;
    *dst = 0;
}

// ---------- 底层 I2C 发送（复用 AHT20 总线）----------
// SSD1306 支持「连续写」：一次 start + 地址 + 控制字节(0x40) 之后，
// 可以连续发任意多个数据字节，最后才 stop。相比「逐字节一次完整事务」，
// 可把 I2C 事务次数从 N 降到 1，软件 I2C 下帧率提升非常明显（去卡顿核心）。
static void oled_start_data(void)
{
    g_aht20.bus_start();
    g_aht20.bus_write((uint8_t)(BMCU_OLED_ADDR << 1u)); // 地址写
    g_aht20.bus_write(0x40u);                           // 0x40=后续为数据
}
static void oled_start_cmd(void)
{
    g_aht20.bus_start();
    g_aht20.bus_write((uint8_t)(BMCU_OLED_ADDR << 1u)); // 地址写
    g_aht20.bus_write(0x00u);                           // 0x00=后续为命令
}
static void oled_stop(void)
{
    g_aht20.bus_stop();
}

// 单字节命令（init 序列用，本来就零星发）
void SSD1306_OLED::write_cmd(uint8_t c)
{
    oled_start_cmd();
    g_aht20.bus_write(c);
    oled_stop();
}
// 单字节数据（极少用，保留兼容）
void SSD1306_OLED::write_data(uint8_t d)
{
    oled_start_data();
    g_aht20.bus_write(d);
    oled_stop();
}

// 设置显示起始位置（页寻址模式）：page∈[0,7]，col∈[0,127]
void SSD1306_OLED::set_pos(uint8_t page, uint8_t col)
{
    write_cmd(0xB0u + (page & 0x07u));             // 页地址
    write_cmd(0x00u + (col & 0x0Fu));              // 列低 4 位
    write_cmd(0x10u + ((col >> 4u) & 0x0Fu));      // 列高 4 位
}

// 连续发数据（一次 I2C 事务）：先 set_pos 定位，再调本函数批量写
static void oled_write_data_bulk(const uint8_t* data, uint8_t n)
{
    oled_start_data();
    for (uint8_t i = 0; i < n; i++)
        g_aht20.bus_write(data[i]);
    oled_stop();
}

// ---------- 8x16 ASCII 字模（移植自江协科技 OLED 库，字符 0x20~0x7E，每字符 16 字节）----------
// ---------- 8x16 ASCII 字模（裁剪版：仅含屏幕实际用到的 38 个字符，省 Flash）----------
static const uint8_t OLED_F8x16[38u][16u] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},// ' '
    {0xF0,0x08,0xF0,0x00,0xE0,0x18,0x00,0x00,0x00,0x21,0x1C,0x03,0x1E,0x21,0x1E,0x00},// '%'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01},// '-'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00,0x00,0x00},// '.'
    {0x00,0x00,0x00,0x00,0x80,0x60,0x18,0x04,0x00,0x60,0x18,0x06,0x01,0x00,0x00,0x00},// '/'
    {0x00,0xE0,0x10,0x08,0x08,0x10,0xE0,0x00,0x00,0x0F,0x10,0x20,0x20,0x10,0x0F,0x00},// '0'
    {0x00,0x10,0x10,0xF8,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00},// '1'
    {0x00,0x70,0x08,0x08,0x08,0x88,0x70,0x00,0x00,0x30,0x28,0x24,0x22,0x21,0x30,0x00},// '2'
    {0x00,0x30,0x08,0x88,0x88,0x48,0x30,0x00,0x00,0x18,0x20,0x20,0x20,0x11,0x0E,0x00},// '3'
    {0x00,0x00,0xC0,0x20,0x10,0xF8,0x00,0x00,0x00,0x07,0x04,0x24,0x24,0x3F,0x24,0x00},// '4'
    {0x00,0xF8,0x08,0x88,0x88,0x08,0x08,0x00,0x00,0x19,0x21,0x20,0x20,0x11,0x0E,0x00},// '5'
    {0x00,0xE0,0x10,0x88,0x88,0x18,0x00,0x00,0x00,0x0F,0x11,0x20,0x20,0x11,0x0E,0x00},// '6'
    {0x00,0x38,0x08,0x08,0xC8,0x38,0x08,0x00,0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},// '7'
    {0x00,0x70,0x88,0x08,0x08,0x88,0x70,0x00,0x00,0x1C,0x22,0x21,0x21,0x22,0x1C,0x00},// '8'
    {0x00,0xE0,0x10,0x08,0x08,0x10,0xE0,0x00,0x00,0x00,0x31,0x22,0x22,0x11,0x0F,0x00},// '9'
    {0x00,0x00,0x00,0xC0,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00},// ':'
    {0x00,0x70,0x48,0x08,0x08,0x08,0xF0,0x00,0x00,0x00,0x00,0x30,0x36,0x01,0x00,0x00},// '?'
    {0x00,0x00,0xC0,0x38,0xE0,0x00,0x00,0x00,0x20,0x3C,0x23,0x02,0x02,0x27,0x38,0x20},// 'A'
    {0x08,0xF8,0x88,0x88,0x88,0x70,0x00,0x00,0x20,0x3F,0x20,0x20,0x20,0x11,0x0E,0x00},// 'B'
    {0xC0,0x30,0x08,0x08,0x08,0x08,0x38,0x00,0x07,0x18,0x20,0x20,0x20,0x10,0x08,0x00},// 'C'
    {0x08,0xF8,0x08,0x08,0x08,0x10,0xE0,0x00,0x20,0x3F,0x20,0x20,0x20,0x10,0x0F,0x00},// 'D'
    {0x08,0xF8,0x88,0x88,0xE8,0x08,0x10,0x00,0x20,0x3F,0x20,0x20,0x23,0x20,0x18,0x00},// 'E'
    {0x08,0xF8,0x88,0x88,0xE8,0x08,0x10,0x00,0x20,0x3F,0x20,0x00,0x03,0x00,0x00,0x00},// 'F'
    {0xC0,0x30,0x08,0x08,0x08,0x38,0x00,0x00,0x07,0x18,0x20,0x20,0x22,0x1E,0x02,0x00},// 'G'
    {0x08,0xF8,0x08,0x00,0x00,0x08,0xF8,0x08,0x20,0x3F,0x21,0x01,0x01,0x21,0x3F,0x20},// 'H'
    {0x00,0x08,0x08,0xF8,0x08,0x08,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00},// 'I'
    {0x08,0xF8,0x88,0xC0,0x28,0x18,0x08,0x00,0x20,0x3F,0x20,0x01,0x26,0x38,0x20,0x00},// 'K'
    {0x08,0xF8,0x08,0x00,0x00,0x00,0x00,0x00,0x20,0x3F,0x20,0x20,0x20,0x20,0x30,0x00},// 'L'
    {0x08,0xF8,0xF8,0x00,0xF8,0xF8,0x08,0x00,0x20,0x3F,0x00,0x3F,0x00,0x3F,0x20,0x00},// 'M'
    {0x08,0xF8,0x30,0xC0,0x00,0x08,0xF8,0x08,0x20,0x3F,0x20,0x00,0x07,0x18,0x3F,0x00},// 'N'
    {0xE0,0x10,0x08,0x08,0x08,0x10,0xE0,0x00,0x0F,0x10,0x20,0x20,0x20,0x10,0x0F,0x00},// 'O'
    {0x08,0xF8,0x08,0x08,0x08,0x08,0xF0,0x00,0x20,0x3F,0x21,0x01,0x01,0x01,0x00,0x00},// 'P'
    {0x08,0xF8,0x88,0x88,0x88,0x88,0x70,0x00,0x20,0x3F,0x20,0x00,0x03,0x0C,0x30,0x20},// 'R'
    {0x00,0x70,0x88,0x08,0x08,0x08,0x38,0x00,0x00,0x38,0x20,0x21,0x21,0x22,0x1C,0x00},// 'S'
    {0x18,0x08,0x08,0xF8,0x08,0x08,0x18,0x00,0x00,0x00,0x20,0x3F,0x20,0x00,0x00,0x00},// 'T'
    {0x08,0xF8,0x08,0x00,0x00,0x08,0xF8,0x08,0x00,0x1F,0x20,0x20,0x20,0x20,0x1F,0x00},// 'U'
    {0xF8,0x08,0x00,0xF8,0x00,0x08,0xF8,0x00,0x03,0x3C,0x07,0x00,0x07,0x3C,0x03,0x00},// 'W'
    {0x08,0x38,0xC8,0x00,0xC8,0x38,0x08,0x00,0x00,0x00,0x20,0x3F,0x20,0x00,0x00,0x00},// 'Y'
};

// ASCII(0x20~0x7E) -> 紧凑字模索引 映射表
static const uint8_t OLED_FONT_MAP[95u] = {
    0,0,0,0,0,1,0,0,0,0,0,0,0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0,0,0,0,16,0,17,18,19,20,21,22,23,24,25,0,26,27,28,29,30,31,0,32,33,34,35,0,36,0,37,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// ---------- 初始化命令序列（严格对齐江协科技 OLED_Init，字节一致，不擅自增删）----------
static const uint8_t InitCmd[] = {
    0xAE,        // 关闭显示
    0xD5,0x80,   // 设置时钟分频
    0xA8,0x3F,   // 多路复用比 1/64
    0xD3,0x00,   // 显示偏移
    0x40,        // 起始行
    0xA1,        // 段重映射（左右翻转）
    0xC8,        // 扫描方向（上下翻转）
    0xDA,0x12,   // COM 引脚配置
    0x81,0xCF,   // 对比度
    0xD9,0xF1,   // 预充电周期
    0xDB,0x30,   // VCOMH 电平
    0xA4,        // 全屏点不强制亮
    0xA6,        // 正常显示（非反色）
    0x8D,0x14,   // 电荷泵开启
    0xAF         // 开启显示
};

// 探测 OLED ACK（不修改 s_ready，仅返回是否应答）。用于运行时掉线/热插拔检测。
bool SSD1306_OLED::probe_ack()
{
    g_aht20.bus_start();
    bool ack = g_aht20.bus_write((uint8_t)(BMCU_OLED_ADDR << 1u));
    g_aht20.bus_stop();
    delay(1);
    return ack;
}

void SSD1306_OLED::init()
{
    // 引脚已由 main 中 g_aht20.init() 配置（PB10/PB11 开漏），此处直接发序列。
    delay(200);   // 上电稳定（SSD1306 上电到可收命令需 >100ms，给足余量）

    // 探测 OLED ACK（仅记录，不影响后续发序列；部分模块边缘不回 ACK 但仍可点亮）
    s_ready = probe_ack();

    // 初始化命令序列（显式页寻址 + 整屏范围，最大化兼容性）
    for (uint8_t i = 0; i < sizeof(InitCmd); i++)
        write_cmd(InitCmd[i]);

    SSD1306_OLED::clear();
}

void SSD1306_OLED::clear()
{
    static const uint8_t zero[OLED_W] = {0};   // 全 0 即清屏像素
    for (uint8_t page = 0; page < 8; page++)
    {
        set_pos(page, 0);
        oled_write_data_bulk(zero, OLED_W);     // 一次事务清一整页（128 字节）
    }
    // 同步清空行缓存：清屏后屏上无内容，但 s_line_buf 仍残留旧文本会导致
    // draw_line_if_changed 误判"未变化"而不重绘（标题/内容消失）。清空缓存
    // 可使下一次绘制强制全量重写。
    for (uint8_t r = 0; r < OLED_LINES; r++)
        s_line_buf[r][0] = '\0';
}

void SSD1306_OLED::clear_line(uint8_t row)
{
    if (row >= OLED_LINES) return;
    uint8_t page0 = (uint8_t)(row * 2u);   // 8x16 字模占 2 页
    static const uint8_t zero[OLED_W] = {0};
    for (uint8_t p = 0; p < 2; p++)
    {
        set_pos((uint8_t)(page0 + p), 0);
        oled_write_data_bulk(zero, OLED_W); // 一次事务清两页（256 字节）
    }
}

void SSD1306_OLED::show_char(uint8_t row, uint8_t col, char ch)
{
    if (row >= OLED_LINES || col >= OLED_COLS) return;
    if (ch < 0x20 || ch > 0x7E) ch = ' ';   // 越界字符显示为空格

    uint8_t x   = (uint8_t)(col * 8u);
    uint8_t pg0 = (uint8_t)(row * 2u);
    const uint8_t* p = OLED_F8x16[OLED_FONT_MAP[(uint8_t)ch - 0x20u]];  // 经映射查紧凑字模

    // 上 8 行：一次事务连续写 8 字节
    set_pos(pg0, x);
    oled_write_data_bulk(p, 8u);
    // 下 8 行：一次事务连续写 8 字节
    set_pos((uint8_t)(pg0 + 1u), x);
    oled_write_data_bulk(p + 8u, 8u);
}

void SSD1306_OLED::show_text(uint8_t row, uint8_t col, const char* str)
{
    if (!str) return;
    uint8_t c = col;
    while (*str && c < OLED_COLS)
    {
        show_char(row, c++, *str);
        str++;
    }
}

// ---------- 内容层：温湿度画面 ----------

// 仅当该行内容与缓存不一致时才清行并重写（去闪烁核心）
void SSD1306_OLED::draw_line_if_changed(uint8_t row, const char* text)
{
    if (row >= OLED_LINES) return;
    if (str_eq(s_line_buf[row], text)) return;   // 未变化，不动屏幕
    str_cpy(s_line_buf[row], text);
    clear_line(row);
    show_text(row, 0, text);
}

void SSD1306_OLED::draw_message(const char* line0, const char* line1,
                                 const char* line2, const char* line3)
{
    if (!s_ready) return;   // 屏未就绪，不画

    draw_line_if_changed(0, line0 ? line0 : "");
    draw_line_if_changed(1, line1 ? line1 : "");
    draw_line_if_changed(2, line2 ? line2 : "");
    draw_line_if_changed(3, line3 ? line3 : "");
}

void SSD1306_OLED::draw_aht20(bool aht20_present, bool online, float temperature_c, float humidity_percent, bool comm_ok)
{
    if (!s_ready)
    {
        // 未探测到 OLED：仍尝试提示，方便判断（部分模块边缘能亮）
        draw_line_if_changed(0, "OLED NO ACK");
        return;
    }

    // 板上无 AHT20：不显示任何温湿度，明确告知用户，避免误以为传感器坏。
    if (!aht20_present)
    {
        draw_line_if_changed(0, "NO AHT20");
        draw_line_if_changed(1, "");
        draw_line_if_changed(2, "");
        draw_line_if_changed(3, "");
        return;
    }

    // 第0行标题（固定，仅首次写）
    draw_line_if_changed(0, "AHT20 T/H");

    if (!online)
    {
        draw_line_if_changed(1, "AHT20");
        draw_line_if_changed(2, "OFFLINE");
        draw_line_if_changed(3, "");
        return;
    }

    // 温度/湿度用整数运算拼字符串（避免依赖 printf 浮点，省 Flash 更稳）
    // T/H 合并到同一行： T:23.5C H:45.6%
    int16_t ti = (int16_t)(temperature_c * 10.0f);   // 放大 10 倍
    uint16_t hi = (uint16_t)(humidity_percent * 10.0f);

    char thbuf[OLED_COLS + 1u];
    for (uint8_t i = 0; i < OLED_COLS; i++) thbuf[i] = ' ';
    thbuf[OLED_COLS] = 0;
    int n = 0;
    thbuf[n++] = 'T'; thbuf[n++] = ':';
    if (ti < 0) {
        ti = (int16_t)(-ti);
        thbuf[n++] = '-';
    }
    thbuf[n++] = (char)('0' + (ti / 100) % 10);
    thbuf[n++] = (char)('0' + (ti / 10) % 10);
    thbuf[n++] = '.';
    thbuf[n++] = (char)('0' + (ti % 10));
    thbuf[n++] = 'C';
    // 间隔空格
    thbuf[n++] = ' '; thbuf[n++] = ' ';
    thbuf[n++] = 'H'; thbuf[n++] = ':';
    thbuf[n++] = (char)('0' + (hi / 100) % 10);
    thbuf[n++] = (char)('0' + (hi / 10) % 10);
    thbuf[n++] = '.';
    thbuf[n++] = (char)('0' + (hi % 10));
    thbuf[n++] = '%';
    draw_line_if_changed(1, thbuf);

    // 第2行：通讯状态
    draw_line_if_changed(2, comm_ok ? "COMM OK " : "COMM ERR");

    // 第3行：写死 TPU 型号摘要（LOCK 标识，紧凑显示末两位：GFU02->02 ...）
    char lbuf[OLED_COLS + 1u];
    for (uint8_t i = 0; i < OLED_COLS; i++) lbuf[i] = ' ';
    lbuf[OLED_COLS] = 0;
    int ln = 0;
    lbuf[ln++] = 'L'; lbuf[ln++] = ':';
    for (uint8_t c = 0; c < 4; c++)
    {
        const char* id = TPU_FIXED_ID[c];
        int idlen = 0;
        while (id && id[idlen]) idlen++;
        if (idlen >= 2) { lbuf[ln++] = id[idlen - 2]; lbuf[ln++] = id[idlen - 1]; }
        else if (id && idlen == 1) { lbuf[ln++] = id[0]; }
        if (c < 3) lbuf[ln++] = '/';
    }
    draw_line_if_changed(3, lbuf);
}

// ---------- 多页面轮询 + 动作覆盖显示（v4.0 OLED 增强）----------

// 通道运动态 -> 屏上短标签（≤4字符）
const char* SSD1306_OLED::motion_label(_filament_motion m)
{
    switch (m)
    {
        case _filament_motion::idle:           return "IDLE";
        case _filament_motion::send_out:       return "LOAD";  // 进料
        case _filament_motion::before_on_use:  return "PREP";  // 准备上料
        case _filament_motion::on_use:         return "FEED";  // 供料中
        case _filament_motion::stop_on_use:    return "STOP";  // 停止供料
        case _filament_motion::before_pull_back: return "PRET"; // 准备退料
        case _filament_motion::pull_back:      return "UNLD";  // 退料
        default:                               return "----";
    }
}

// 通道材质/TPU型号 -> 屏上短标签（≤6字符）
const char* SSD1306_OLED::material_label(uint8_t ch)
{
    if (ch >= 4) return "----";
    const _filament& f = ams[BAMBU_BUS_AMS_NUM].filament[ch];

    // v4.0-tpu: 若是 TPU 材质，显示内部真实写死型号（如 GFU98/GFU90...）
    if (f.filament_type == _filament_type::tpu)
    {
        const char* id = TPU_FIXED_ID[ch];
        if (id && id[0]) return id;            // 如 "GFU90"
        return "TPU?";
    }
    // 非 TPU：优先用 Bambu 下发的材质 id（bambubus_filament_id，如 GFG00/GFA00）
    // 直接按前 3 字符映射到材质名（屏宽有限，尽量短、直观）。
    // bambubus filament_id 全部以 "GF" 开头, 第三位表示材质 (依据 Bambu Studio 官方预设 JSON):
    // GFA=PLA / GFG=PETG / GFU=TPU / GFB=ABS / GFC=PC / GFL=PA ...
    if (f.bambubus_filament_id[0] == 'G' && f.bambubus_filament_id[1] == 'F' && f.bambubus_filament_id[2])
    {
        const char c = f.bambubus_filament_id[2];
        if (c == 'G') return "PETG";
        if (c == 'A') return "PLA";
        if (c == 'B') return "ABS";   // GFB00
        if (c == 'C') return "PC";    // GFC00
        if (c == 'L') return "PA";    // GFLxx
        if (c == 'U') return "TPU";   // GFU98 (理论上已走上面 tpu 分支, 这里兜底)
        // 其它非 TPU 材质：用后两位代码兜底（如 U98）
        static char buf[8];
        buf[0] = c; buf[1] = f.bambubus_filament_id[3]; buf[2] = f.bambubus_filament_id[4]; buf[3] = 0;
        return buf;
    }
    // 都没有：用 name 或 unknown
    if (f.name[0]) return f.name;
    return "UNKN";
}

// 把 RGB 转换成肉眼易读的 3 字母颜色简写
// 注意：本函数用于"打印机下发的真实耗材色"显示。Bambu 色卡常见值：
//   红 (255,0,0) / 橙 (255,128,0) / 黄 (255,255,0) / 绿 (0,255,0)
//   / 青 (0,255,255) / 蓝 (0,0,255) / 紫 (255,0,255) / 白 (255,255,255)
const char* SSD1306_OLED::color_name(uint8_t r, uint8_t g, uint8_t b)
{
    // 先判断灰度/黑白
    if (r < 30 && g < 30 && b < 30) return "BLA";
    if (r > 225 && g > 225 && b > 225) return "WHI";
    // 主色判定（取最大分量）
    if (r >= g && r >= b)
    {
        // 红为主
        if (g > 90) return "ORA";   // 红+绿明显 -> 橙（橙 r255 g128）
        if (b > 90) return "PUR";   // 红+蓝 -> 紫
        return "RED";               // 纯红
    }
    if (g >= r && g >= b)
    {
        // 绿为主
        if (r > 90 && b > 90) return "CYA";   // 绿+红+蓝 -> 青
        if (b > 90) return "CYA";             // 绿+蓝 -> 青
        if (r > 90) return "YEL";             // 绿+红 -> 黄
        return "GRE";
    }
    // 蓝色最大
    if (r > 90 && g > 90) return "CYA";
    if (r > 90) return "PUR";
    if (g > 90) return "CYA";
    return "BLU";
}

// 四通道概览页：4 行即 4 个通道（无独立表头行，避免 CH3 被挤出屏幕）。
// 每行格式 "CHx:状态/耗材 颜色"
//   - 通道未在线(无通道)         -> "0:ERR"
//   - 通道在线但无料(meters<=0)  -> "0:NULL  COL"
//   - 通道有料                  -> "0:MAT   COL"  (MAT=型号, TPU显写死型号)
void SSD1306_OLED::draw_channels()
{
    if (!s_ready) return;

    for (uint8_t r = 0; r < 4; r++)
    {
        // 整行先填空格（避免栈残留导致切页时带上其它页碎片，如 "ERR85 WHI"）
        char line[OLED_COLS + 1u];
        for (uint8_t i = 0; i < OLED_COLS; i++) line[i] = ' ';
        line[OLED_COLS] = 0;

        const _filament& f = ams[BAMBU_BUS_AMS_NUM].filament[r];

        int n = 0;
        // 实际通道为 1~4，屏上显示 +1（数组下标 r 仍为 0~3）
        line[n++] = (char)('0' + (r + 1));
        line[n++] = ':';

        if (f.meters <= 0.05f)
        {
            // v4.0-tpu: empty channel shows NULL, not ERR
            line[n++] = 'N'; line[n++] = 'U'; line[n++] = 'L'; line[n++] = 'L';
        }
        else
        {
            // 有料: 材质名 + 下发型号 (e.g. "PETG GFG00" / "TPU GFU90")
            const char* mat = material_label(r);
            for (int i = 0; mat[i] && n < 7; i++) line[n++] = mat[i];
            if (n < 7) line[n++] = ' ';
            const char* id = f.bambubus_filament_id;
            for (int i = 0; i < 5 && id[i] && n < OLED_COLS; i++) line[n++] = id[i];
        }

        draw_line_if_changed(r, line);   // 直接占 4 行，CH3 也能显示
    }
}

// 通讯监控页：4 行
//   COMM OK / ERR      通讯状态（comm_ok）
//   PKG 123456         总收包计数（持续增长=没漏包）
//   SET 000123         set_filament 调用次数（打印机下发设料计数）
//   ID  GFU98          最近收到的 filament_id（未收到则 "---"）
// 把开机运行时间格式化为 "DDd HH:MM:SS"（最多 16 列），写入 out
static void fmt_uptime(char* out, uint8_t cap)
{
    if (!out || cap == 0u) return;
    uint64_t sec = time_ms64() / 1000ull;
    const uint32_t s  = (uint32_t)(sec % 60ull);
    const uint32_t m  = (uint32_t)((sec / 60ull) % 60ull);
    const uint32_t h  = (uint32_t)((sec / 3600ull) % 24ull);
    uint32_t d  = (uint32_t)(sec / 86400ull);
    int k = 0;
    if (d > 0)
    {
        if (d > 99) d = 99;   // 防溢出
        out[k++] = (char)('0' + (d / 10)); out[k++] = (char)('0' + (d % 10));
        out[k++] = 'd'; out[k++] = ' ';
    }
    out[k++] = (char)('0' + (h / 10)); out[k++] = (char)('0' + (h % 10));
    out[k++] = ':';
    out[k++] = (char)('0' + (m / 10)); out[k++] = (char)('0' + (m % 10));
    out[k++] = ':';
    out[k++] = (char)('0' + (s / 10)); out[k++] = (char)('0' + (s % 10));
    while (k < cap) out[k++] = ' ';
    out[cap] = 0;
}

void SSD1306_OLED::draw_comm(bool comm_ok)
{
    // 布局（4 行，互不冲突）：
    //   L0: COMM OK/ERR            —— 通讯状态
    //   L1: PKG nnnnn SET nnnnn     —— 两个计数合并一行（左 PKG / 右 SET）
    //   L2: ID xxxxx                —— 当前 filament_id
    //   L3: RUN HH:MM:SS            —— 运行时间独占一行（丝滑原地重写）
    // 注意：L0 必须每次都交给 draw_line_if_changed 判定。tick() 切页时会 clear()
    // 清空屏幕并重置 s_line_buf，若用"状态翻转才重绘"的缓存会跳过、导致 L0 空白。
    // draw_line_if_changed 本身已对未变化内容跳过重绘，不会闪烁。
    const char* cstat = comm_ok ? "OK" : "ERR";

    // --- L0 通讯状态（每次判定，切页后自动重绘）---
    {
        char l0[OLED_COLS + 1u];
        int k = 0;
        l0[k++] = 'C'; l0[k++] = 'O'; l0[k++] = 'M'; l0[k++] = 'M'; l0[k++] = ' ';
        l0[k++] = cstat[0]; l0[k++] = cstat[1];
        while (k < 16) l0[k++] = ' ';
        l0[16] = 0;
        draw_line_if_changed(0, l0);
    }

    // --- L1：PKG + SET 同行（各占约 8 列，互不覆盖）---
    char num[12]; int ni = 0;
    uint32_t v;
    char l1[OLED_COLS + 1u];
    int k = 0;
    // 左半：PKG nnnnn（右对齐到列 7）
    l1[k++] = 'P'; l1[k++] = 'K'; l1[k++] = 'G'; l1[k++] = ' ';
    v = g_pkg_recv_cnt; ni = 0;
    if (v == 0) num[ni++] = '0';
    while (v > 0) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = ni - 1; i >= 0; i--) { if (k < 7) l1[k++] = num[i]; }
    while (k < 8) l1[k++] = ' ';
    // 右半：SET nnnnn（从列 8 起）
    l1[k++] = 'S'; l1[k++] = 'E'; l1[k++] = 'T'; l1[k++] = ' ';
    v = g_set_filament_cnt; ni = 0;
    if (v == 0) num[ni++] = '0';
    while (v > 0) { num[ni++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = ni - 1; i >= 0; i--) { if (k < 15) l1[k++] = num[i]; }
    while (k < 16) l1[k++] = ' ';
    l1[16] = 0;
    draw_line_if_changed(1, l1);

    // --- L2：ID xxxxx ---
    char l2[OLED_COLS + 1u];
    k = 0;
    l2[k++] = 'I'; l2[k++] = 'D'; l2[k++] = ' ';
    if (g_last_filament_id[0] != 0)
    {
        for (int i = 0; g_last_filament_id[i] && k < 15; i++) l2[k++] = g_last_filament_id[i];
    }
    else
    {
        l2[k++] = '-'; l2[k++] = '-'; l2[k++] = '-';
    }
    while (k < 16) l2[k++] = ' ';
    l2[16] = 0;
    draw_line_if_changed(2, l2);

    // --- L3：运行时间独占一行 ---
    // 直接整行 show_text 原地写（不调用 clear_line，因此不会整行黑闪）。
    // 内容未变时写的是相同字模（无视觉变化），仅变化的秒位原地更新 → 丝滑时钟效果。
    // 切页回来时本行整体重写，自动覆盖上一页残留内容。
    {
        char up[16];
        fmt_uptime(up, 15);
        char l3[OLED_COLS + 1u];
        int k3 = 0;
        l3[k3++] = 'R'; l3[k3++] = 'U'; l3[k3++] = 'N'; l3[k3++] = ' ';
        for (uint8_t i = 0; up[i] && k3 < OLED_COLS; i++) l3[k3++] = up[i];
        while (k3 < OLED_COLS) l3[k3++] = ' ';
        l3[OLED_COLS] = 0;
        show_text(3, 0, l3);
    }
}

void SSD1306_OLED::notify_action(uint8_t ch, oled_action act)
{
    if (!s_ready) return;                 // 无 OLED：直接返回，业务零影响
    if (ch >= 4) return;
    if (act == oled_action::action_none) return;
    s_action     = act;
    s_action_ch  = ch;
    s_action_until_ms = time_ms64() + OLED_ACTION_HOLD_MS;
    // 立即打断轮询，切到动作显示（下一 tick 会优先画动作）
    s_page_next_ms = 0;
}

void SSD1306_OLED::tick(bool aht20_present, bool aht20_online,
                        float temperature_c, float humidity_percent,
                        bool comm_ok)
{
    if (!s_ready) return;                 // 无 OLED：直接返回

    // 运行时掉线/热插拔检测：屏若被拔掉，ACK 丢失则复位 s_ready，
    // 由 main 的 10s 重探逻辑重新 init 点亮（修复"开机有屏→热插拔不亮"）。
    if (!probe_ack())
    {
        s_ready = false;
        return;
    }

    const uint64_t now = time_ms64();

    // ===== 1) 动作覆盖优先 =====
    if (s_action != oled_action::action_none && now < s_action_until_ms)
    {
        char l0[OLED_COLS + 1u];
        const char* actxt = "NONE";
        switch (s_action)
        {
            case oled_action::action_load:  actxt = "LOADING";  break;  // 进料
            case oled_action::action_unload:actxt = "UNLOAD";   break;  // 退料
            case oled_action::action_feed:  actxt = "FEEDING";  break;  // 送料
            case oled_action::action_idle:  actxt = "STOP";     break;  // 停止
            default:                        actxt = "NONE";     break;
        }
        // 第0行："CHx LOADING"，通道号显示 1~4（实际通道），数组下标 +1
        int k = 0;
        l0[k++] = 'C'; l0[k++] = 'H'; l0[k++] = (char)('0' + (s_action_ch + 1u)); l0[k++] = ' ';
        for (int i = 0; actxt[i] && k < 16; i++) l0[k++] = actxt[i];
        while (k < 16) l0[k++] = ' ';
        l0[16] = 0;
        draw_line_if_changed(0, l0);

        // 第1行：该通道材质/型号
        draw_line_if_changed(1, material_label(s_action_ch));
        // 第2行：动作含义提示
        const char* hint = "";
        switch (s_action)
        {
            case oled_action::action_load:   hint = "feed in";  break;
            case oled_action::action_unload: hint = "pull out"; break;
            case oled_action::action_feed:   hint = "supplying";break;
            default:                         hint = "";         break;
        }
        draw_line_if_changed(2, hint);
        draw_line_if_changed(3, "");
        return;   // 动作期间不轮询
    }
    // 动作结束：清动作状态，回轮询
    if (s_action != oled_action::action_none)
    {
        s_action = oled_action::action_none;
        s_action_ch = 0xFFu;
        clear();   // 整屏清，回到轮询干净重画
    }

    // ===== 2) 页面轮询 =====
    // [临时调试] 设 true 时 OLED 只显示通讯监控页(第3页)，方便上机盯 PKG/SET/ID。
    // 正常发布时改为 false（或整段注释掉）。
    if (false)
    {
        draw_comm(comm_ok);
        return;
    }

    if (s_page_next_ms == 0u)
    {
        // 首次进入轮询：先启动倒计时（不切页），避免 setup 阶段耗时吃掉了
        // 首屏 AHT20 页的停留时间，导致开机一闪就跳下一页。
        s_page_next_ms = now + OLED_PAGE_DWELL_MS;
    }
    else if (now >= s_page_next_ms)
    {
        // 切到下一页
        s_page = (oled_page)((uint8_t)s_page + 1u);
        if (s_page >= oled_page::page_count) s_page = oled_page::page_aht20;
        s_page_next_ms = now + OLED_PAGE_DWELL_MS;
        clear();   // 切页清屏，避免残留
    }

    switch (s_page)
    {
        case oled_page::page_aht20:
            draw_aht20(aht20_present, aht20_online, temperature_c, humidity_percent, comm_ok);
            break;
        case oled_page::page_channels:
            draw_channels();
            break;
        case oled_page::page_comm:
            draw_comm(comm_ok);
            break;
        default:
            break;
    }
}

#endif // BMCU_OLED