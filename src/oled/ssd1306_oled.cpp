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
#include <cstring>         // strcmp（过滤高频包 ONL）

// 手写无浮点整数拼串辅助（避免 newlib-nano 拉入浮点 printf 撑爆 FLASH）
static void append_uint(char* buf, uint8_t& k, uint32_t v)
{
    if (!buf) return;
    char tmp[11]; uint8_t ti = 0;
    if (v == 0u) tmp[ti++] = '0';
    else { while (v > 0u) { tmp[ti++] = (char)('0' + (v % 10u)); v /= 10u; } }
    while (ti > 0u && k < 16) buf[k++] = tmp[--ti];
}

// v4.0-tpu 抓包共用绘制：4 行 = RX标签+秒 / TX标签+秒 / 最近RX原始片段 / 累计计数(PKG/SET/rx/tx)。
// draw_sniffer 轮询页与"指令霸屏"共用本函数，避免两份重复拼串撑爆 FLASH。
// 格式化抓包页/霸屏顶部两行："RX LABEL 123s" / "TX LABEL  12s"
static void fmt_pkt_header(char* buf, char c0, char c1, const char* label, uint64_t age_ms)
{
    uint8_t k = 0;
    buf[k++] = c0; buf[k++] = c1; buf[k++] = ' ';
    const char* l = (label && label[0]) ? label : "-";
    for (uint8_t i = 0; l[i] && k < 8; i++) buf[k++] = l[i];
    while (k < 11) buf[k++] = ' ';
    uint32_t dts = (age_ms > 999000ull) ? 999u : (uint32_t)(age_ms / 1000ull);
    if (dts >= 100) buf[k++] = (char)('0' + (dts / 100u % 10u));
    if (dts >= 10)  buf[k++] = (char)('0' + (dts / 10u  % 10u));
    buf[k++] = (char)('0' + (dts % 10u));
    buf[k++] = 's';
    while (k < OLED_COLS) buf[k++] = ' ';
    buf[OLED_COLS] = 0;
}

void SSD1306_OLED::draw_pkt_overlay(uint64_t now)
{
    char buf[OLED_COLS + 1u];

    // 第0行 R：显示“动作指令”缓存（仅 on_use/stop/before_on_use/before_pullb 等动作前后指令），
    // 周期轮询(SN/VER/RD/RFID)与纯心跳已过滤，平时稳定显示上一条动作指令及其秒数。
    // 动作缓存为空（开机还没发生过动作）时回退显示提示符 "--"，而非被 SN 淹没。
    const char* rx_lbl = (g_last_act_rx_label[0] != '\0') ? g_last_act_rx_label : "--";
    const uint64_t rx_ms = (g_last_act_rx_label[0] != '\0') ? g_last_act_rx_ms : 0u;
    const char* rx_raw = (g_last_act_rx_label[0] != '\0') ? g_last_act_rx_raw : g_last_rx_raw;

    fmt_pkt_header(buf, 'R', 'X', rx_lbl,
                   (rx_ms == 0u) ? 0u : (now - rx_ms));
    draw_line_if_changed(0, buf);

    fmt_pkt_header(buf, 'T', 'X', g_last_tx_label,
                   (g_last_tx_ms == 0u) ? 0u : (now - g_last_tx_ms));
    draw_line_if_changed(1, buf);

    // L2: 未知指令轮显（标 '?' 前缀）+ 无未知时显示动作指令原始片段（'>' 前缀）。
    //     未知指令可能连续出现多条（如 A / 3 3 / E06A48220 / S N），用环形缓冲每 ~2.5s 轮切一条，
    //     避免“只显示最后一条”而丢失之前的未知包。无未知时回退显示动作原始片段。
    uint8_t k = 0;
    const char* frag;
    char frag_prefix;
    static uint64_t s_unk_rotate_ms = 0u;   // 轮显计时
    static uint8_t  s_unk_rotate_idx = 0u;  // 当前轮显的环形缓冲逻辑序号
    if (g_unk_ring_cnt > 0u)
    {
        // 每 2500ms 推进一次轮显位置（在已缓存条数内循环）
        if (s_unk_rotate_ms == 0u) s_unk_rotate_ms = now;
        if (now - s_unk_rotate_ms >= 2500u)
        {
            s_unk_rotate_ms = now;
            s_unk_rotate_idx = (uint8_t)((s_unk_rotate_idx + 1u) % g_unk_ring_cnt);
        }
        // 把逻辑序号映射到环形缓冲的物理位置（head 指向“下一个写入”，最老的是 head）
        uint8_t phys = (uint8_t)((g_unk_ring_head + s_unk_rotate_idx) % UNK_RING_N);
        frag = g_unk_ring[phys];
        frag_prefix = '?';
    }
    else
    {
        frag = rx_raw;
        frag_prefix = '>';
    }
    buf[k++] = frag_prefix; buf[k++] = ' ';
    for (uint8_t i = 0; frag[i] && k < OLED_COLS; i++) buf[k++] = frag[i];
    while (k < OLED_COLS) buf[k++] = ' ';
    buf[OLED_COLS] = 0;
    draw_line_if_changed(2, buf);   // 后续由 framebuffer 差值刷新，无需 soft 覆盖

    // L3: 累计计数（收包/设料/RX总/TX总/未知?N）
    k = 0;
    buf[k++] = 'P'; append_uint(buf, k, g_pkg_recv_cnt);
    buf[k++] = ' '; buf[k++] = 'S'; append_uint(buf, k, g_set_filament_cnt);
    buf[k++] = ' '; buf[k++] = 'r'; append_uint(buf, k, g_rx_cnt);
    buf[k++] = ' '; buf[k++] = 't'; append_uint(buf, k, g_tx_cnt);
    buf[k++] = ' '; buf[k++] = '?'; append_uint(buf, k, g_unk_cnt);
    while (k < OLED_COLS) buf[k++] = ' ';
    buf[OLED_COLS] = 0;
    draw_line_if_changed(3, buf);
}

#ifdef BMCU_OLED

// ---------- 硬件 I2C2 底层发送（PB10=SCL, PB11=SDA）----------
// 默认启用硬件 I2C2 以节省 Flash（去掉软件 bitbang）。
// 软件 I2C 代码保留在 aht20.cpp 内，关闭 BMCU_USE_HW_I2C2 即可回退。
#include "i2c_hw/i2c2_hw.h"

// 兼容：若关闭硬件 I2C2，仍复用 g_aht20 的软件 I2C 总线
#if !BMCU_USE_HW_I2C2
extern AHT20 g_aht20;
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
#endif // !BMCU_USE_HW_I2C2

// 单字节命令（init 序列用，本来就零星发）
void SSD1306_OLED::write_cmd(uint8_t c)
{
#if BMCU_USE_HW_I2C2
    const uint8_t buf[2] = {0x00u, c};
    hw_i2c2_write(BMCU_OLED_ADDR, buf, 2, true);
#else
    oled_start_cmd();
    g_aht20.bus_write(c);
    oled_stop();
#endif
}
// 单字节数据（极少用，保留兼容）
void SSD1306_OLED::write_data(uint8_t d)
{
#if BMCU_USE_HW_I2C2
    const uint8_t buf[2] = {0x40u, d};
    hw_i2c2_write(BMCU_OLED_ADDR, buf, 2, true);
#else
    oled_start_data();
    g_aht20.bus_write(d);
    oled_stop();
#endif
}

// 设置显示起始位置（页寻址模式）：page∈[0,7]，col∈[0,127]
void SSD1306_OLED::set_pos(uint8_t page, uint8_t col)
{
    write_cmd(0xB0u + (page & 0x07u));             // 页地址
    write_cmd(0x00u + (col & 0x0Fu));              // 列低 4 位
    write_cmd(0x10u + ((col >> 4u) & 0x0Fu));      // 列高 4 位
}

#if BMCU_USE_HW_I2C2
// 硬件 I2C2 下批量写 SSD1306 的临时缓冲区：control byte + 最多 128 像素字节。
// SSD1306_OLED 是单线程串行使用，static 安全；放在文件作用域避免大数组占栈。
static uint8_t s_oled_bulk_buf[1u + 128u];
#endif

// 连续发数据（一次 I2C 事务）：先 set_pos 定位，再调本函数批量写
static void oled_write_data_bulk(const uint8_t* data, uint8_t n)
{
#if BMCU_USE_HW_I2C2
    if (n == 0u || !data) return;
    if (n > 128u) n = 128u;
    s_oled_bulk_buf[0] = 0x40u;
    for (uint8_t i = 0; i < n; ++i)
        s_oled_bulk_buf[i + 1u] = data[i];
    hw_i2c2_write(BMCU_OLED_ADDR, s_oled_bulk_buf, (uint8_t)(n + 1u), true);
#else
    oled_start_data();
    for (uint8_t i = 0; i < n; i++)
        g_aht20.bus_write(data[i]);
    oled_stop();
#endif
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
#if BMCU_USE_HW_I2C2
    // 硬件 I2C2：发 START + 写地址 + STOP，无数据；ACK 成功则返回 true
    bool ack = hw_i2c2_write(BMCU_OLED_ADDR, nullptr, 0, true);
    delay(1);
    return ack;
#else
    g_aht20.bus_start();
    bool ack = g_aht20.bus_write((uint8_t)(BMCU_OLED_ADDR << 1u));
    g_aht20.bus_stop();
    delay(1);
    return ack;
#endif
}

void SSD1306_OLED::init()
{
#if BMCU_USE_HW_I2C2
    // 硬件 I2C2：由本驱动初始化 PB10/PB11 为 AF_OD，并复位外设
    hw_i2c2_init();
#else
    // 软件 I2C：引脚已由 main 中 g_aht20.init() 配置（PB10/PB11 开漏）
#endif
    delay(200);   // 上电稳定（SSD1306 上电到可收命令需 >100ms，给足余量）

    // 探测 OLED ACK（仅记录，不影响后续发序列；部分模块边缘不回 ACK 但仍可点亮）
    s_ready = probe_ack();

    // 初始化命令序列（显式页寻址 + 整屏范围，最大化兼容性）
    for (uint8_t i = 0; i < sizeof(InitCmd); i++)
        write_cmd(InitCmd[i]);

    SSD1306_OLED::clear();
    // 初始化时真正清屏一次：把 framebuffer 全 0 同步到屏幕与 s_fb_prev。
    for (uint8_t page = 0; page < 8; page++)
    {
        set_pos(page, 0);
        static const uint8_t zero[OLED_W] = {0};
        oled_write_data_bulk(zero, OLED_W);
    }
    memcpy(s_fb_prev, s_fb, sizeof(s_fb_prev));
}

void SSD1306_OLED::clear()
{
    // 仅清空后台 framebuffer（不发 I2C）。下次 flush() 会把全屏写为 0。
    memset(s_fb, 0, sizeof(s_fb));
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
    for (uint8_t p = 0; p < 2; p++)
        memset(s_fb[page0 + p], 0, OLED_W); // 仅清 framebuffer 对应两页
}

void SSD1306_OLED::show_char(uint8_t row, uint8_t col, char ch)
{
    if (row >= OLED_LINES || col >= OLED_COLS) return;
    if (ch < 0x20 || ch > 0x7E) ch = ' ';   // 越界字符显示为空格

    uint8_t x   = (uint8_t)(col * 8u);
    uint8_t pg0 = (uint8_t)(row * 2u);
    const uint8_t* p = OLED_F8x16[OLED_FONT_MAP[(uint8_t)ch - 0x20u]];  // 经映射查紧凑字模

    // 写入 framebuffer（上 8 行 / 下 8 行），不立即发 I2C。
    for (uint8_t i = 0; i < 8; i++) s_fb[pg0][(uint8_t)(x + i)]     = p[i];
    for (uint8_t i = 0; i < 8; i++) s_fb[pg0 + 1u][(uint8_t)(x + i)] = p[i + 8u];
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

// 差值刷新：把 framebuffer 与已发送帧逐字节比对，仅把变化字节发给 SSD1306。
// 按页扫描，对每页找出连续变化段，一次 bulk 写（set_pos 定位一次 + 连续数据）。
// 不变字节完全不碰 I2C => 极低总线占用、绝不整屏清 => 无黑闪。
void SSD1306_OLED::flush()
{
    for (uint8_t page = 0; page < 8; page++)
    {
        uint8_t col = 0;
        while (col < OLED_W)
        {
            if (s_fb[page][col] == s_fb_prev[page][col])
            {
                col++;
                continue;
            }
            // 找到变化段起点，延伸到连续变化结束
            uint8_t start = col;
            uint8_t end = col;
            while (end < OLED_W && s_fb[page][end] != s_fb_prev[page][end])
                end++;
            uint8_t n = (uint8_t)(end - start);
            set_pos(page, start);
            oled_write_data_bulk(&s_fb[page][start], n);
            // 同步已发送帧
            for (uint8_t i = 0; i < n; i++)
                s_fb_prev[page][(uint8_t)(start + i)] = s_fb[page][(uint8_t)(start + i)];
            col = end;
        }
    }
}

// ---------- 内容层：温湿度画面 ----------

// 仅当该行内容与缓存不一致时才清行并重写（去闪烁核心）
void SSD1306_OLED::draw_line_if_changed(uint8_t row, const char* text)
{
    if (row >= OLED_LINES) return;
    if (strcmp(s_line_buf[row], text) == 0) return;   // 未变化，不动屏幕
    strncpy(s_line_buf[row], text, OLED_COLS - 1);
    s_line_buf[row][OLED_COLS - 1] = '\0';
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

// v4.0-tpu 抓包页：轮询显示最近 RX/TX 指令与累计计数（实现见 draw_pkt_overlay 共用函数）。
void SSD1306_OLED::draw_sniffer()
{
    if (!s_ready) return;
    draw_pkt_overlay(time_ms64());
}

// v4.0-tpu 未知指令完整记录页（仅调试模式 BMCU_OLED_DEBUG 编入）：
// 自动翻页滚动显示最近 UNK_RING_N 条未知指令（按到达顺序，带全局递增序号），
// 用于上机完整记录打印机下发的未登记包。每 1.5s 上滚一行；不足满屏时显示实际条数。
#ifdef BMCU_OLED_DEBUG
void SSD1306_OLED::draw_unk_log()
{
    if (!s_ready) return;

    const uint64_t now = time_ms64();
    static uint64_t s_unklog_ms  = 0u;   // 自动翻页计时
    static uint16_t s_unklog_top = 0u;   // 当前最顶行对应的逻辑序号（0..cnt-1）

    if (g_unk_ring_cnt == 0u)
    {
        // 无未知指令：显提示，不滚动
        draw_line_if_changed(0, "UNK LOG EMPTY");
        draw_line_if_changed(1, "");
        draw_line_if_changed(2, "");
        draw_line_if_changed(3, "");
        return;
    }

    // 每 1500ms 上滚一行（在已缓存条数内循环）
    if (s_unklog_ms == 0u) s_unklog_ms = now;
    if (now - s_unklog_ms >= 1500u)
    {
        s_unklog_ms = now;
        s_unklog_top = (uint16_t)((s_unklog_top + 1u) % g_unk_ring_cnt);
    }

    // OLED_LINES=4 行，从 s_unklog_top 起向下列，循环覆盖缓冲
    char buf[OLED_COLS + 1u];
    for (uint8_t r = 0; r < OLED_LINES; r++)
    {
        uint8_t li = (uint8_t)((s_unklog_top + r) % g_unk_ring_cnt);  // 逻辑序号
        uint8_t phys = (uint8_t)((g_unk_ring_head + li) % UNK_RING_N); // 物理位置（最老=head）
        uint32_t seq = g_unk_seq[phys];

        // 行首 "?N=序号 片段"：N=页内行号(1..4)，seq=全局序号，便于对照到达顺序
        uint8_t k = 0;
        buf[k++] = '?';
        buf[k++] = (char)('1' + r);
        buf[k++] = '=';
        // 序号最多 5 位
        if (seq >= 10000u) buf[k++] = (char)('0' + (seq / 10000u % 10u));
        if (seq >= 1000u)  buf[k++] = (char)('0' + (seq / 1000u  % 10u));
        if (seq >= 100u)   buf[k++] = (char)('0' + (seq / 100u    % 10u));
        if (seq >= 10u)    buf[k++] = (char)('0' + (seq / 10u     % 10u));
        buf[k++] = (char)('0' + (seq % 10u));
        buf[k++] = ' ';
        const char* frag = g_unk_ring[phys];
        for (uint8_t i = 0; frag[i] && k < OLED_COLS; i++) buf[k++] = frag[i];
        while (k < OLED_COLS) buf[k++] = ' ';
        buf[OLED_COLS] = 0;
        draw_line_if_changed(r, buf);
    }
}
#endif  // BMCU_OLED_DEBUG

void SSD1306_OLED::notify_action(uint8_t ch, oled_action act)
{
    if (!s_ready) return;                 // 无 OLED：直接返回，业务零影响
    if (ch >= 4) return;
    if (act == oled_action::action_none) return;
    // v4.0-tpu 修复问题2：打印机供料/进料过程中会持续发 on_use 心跳, 每次都进本函数。
    // 若每次都刷新超时, 霸屏会被心跳无限延长, 导致进料完成后 OLED 一直显示 FEEDING,
    // 只有退料(motion 归 idle)才解除。这里对"相同动作+相同通道"不刷新超时,
    // 仅当动作切换(或换通道)时才重置计时 —— 这样最后一次心跳后 OLED_ACTION_HOLD_MS 即回轮询。
    if (s_action == act && s_action_ch == ch)
    {
        return;   // 重复相同动作(典型为 on_use 心跳): 不延长霸屏, 不重置
    }
    // action_idle(打印机明确停止供料) 立即结束霸屏，不等超时，避免一直显 FEEDING
    if (act == oled_action::action_idle)
    {
        s_action = oled_action::action_none;
        s_action_ch = 0xFFu;
        s_action_until_ms = 0u;
        // 不调 clear()，避免整屏黑闪；下一帧由抓包页 draw_line_if_changed 平滑覆盖
        return;
    }
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

    // 注：动作霸屏的退出完全依赖 (a) OLED_ACTION_HOLD_MS 超时，或 (b) 收到
    // action_idle（打印机下发 stop_on_use / 停止供料）。不再依赖 motion==idle 做
    // 治愈——motion 在 on_use/idle 间抖动会导致霸屏反复清除又重建（"一直跳 FEEDING"）。
    // 打印机明确停止供料时会经 set_motion(stop_on_use) -> notify_action(action_idle) 立即结束。

    // ===== 0.5) 页面轮询计时（放在霸屏判断之前，确保霸屏期间计时照常流逝）=====
    // 关键修复：之前页面切换逻辑在霸屏分支“之后”，霸屏期间从不更新 s_page_next_ms，
    // 一旦霸屏结束(指令停发/动作超时)就立刻满足 now>=s_page_next_ms 触发切页 + clear()，
    // 导致轮询页“闪一下就没了”。这里把计时提前，霸屏期间顺延倒计时，霸屏结束后
    // 当前页仍会完整停留 OLED_PAGE_DWELL_MS 再切，不再被瞬间切走。
    if (s_page_next_ms == 0u)
    {
        // 首次进入轮询：先启动倒计时（不切页），避免 setup 阶段耗时吃掉了
        // 首屏 AHT20 页的停留时间，导致开机一闪就跳下一页。
        s_page_next_ms = now + OLED_PAGE_DWELL_MS;
    }
    else if (now >= s_page_next_ms)
    {
        // 切到下一页（按当前模式的轮询范围循环）
#ifdef BMCU_OLED_DEBUG
        s_page = (s_page >= oled_page::page_unk_log)
                     ? oled_page::page_sniffer
                     : (oled_page)((uint8_t)s_page + 1u);
#else
        s_page = (s_page >= oled_page::page_comm)
                     ? oled_page::page_aht20
                     : (oled_page)((uint8_t)s_page + 1u);
#endif
        s_page_next_ms = now + OLED_PAGE_DWELL_MS;
        clear();   // 切页清屏，避免残留
    }
    // 注意：此处只处理“轮询页已到期则切页”，霸屏期间若切了页也无妨（霸屏顶屏覆盖）。
    // 若当前正处于霸屏，下方会直接顶屏，轮询页的切换被自然“暂停”（s_page 已前进，
    // 霸屏结束后显示的就是新一页，且因上面刚刷过 s_page_next_ms，会停留满 5 秒）。

    // ===== 1) 动作覆盖 / 指令霸屏 已移除（v4.0-tpu 调试期）=====
    // 用户需求：不要霸屏顶屏，收到的信息直接显示到抓包页；且只显示“动作前后”
    // 打印机下发的指令，过滤掉一直发的周期轮询(SN/VER/RD/RFID/ONL/MC)与纯心跳。
    // 详见 oled_log_rx()/oled_log_tx() 的过滤逻辑与 draw_pkt_overlay() 的动作缓存显示。

    // ===== 2) 页面轮询绘制 =====
    // 正常模式(无 BMCU_OLED_DEBUG)：轮询前三页（温湿度/四通道/通讯监控）。
    // 调试模式(定义 BMCU_OLED_DEBUG)：只轮询调试页（抓包页 + 未知指令记录页），
    // 跳过前三页——调试时只关心通讯抓包，不需要温湿度等状态页。
    // 旧版临时 `if(true){draw_sniffer;return}` 硬霸屏开关已移除：分模式后轮询即可，
    // 调试模式靠宏只轮询调试页，无需强制单页。

    // 轮询边界（按模式）
#ifdef BMCU_OLED_DEBUG
    const oled_page page_first = oled_page::page_sniffer;
    const oled_page page_last  = oled_page::page_unk_log;
#else
    const oled_page page_first = oled_page::page_aht20;
    const oled_page page_last  = oled_page::page_comm;
#endif

    // 越界保护：若 s_page 不在当前模式合法范围，回到本模式首页
    if (s_page < page_first || s_page > page_last)
    {
        s_page = page_first;
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
#ifdef BMCU_OLED_DEBUG
        case oled_page::page_sniffer:   // v4.0-tpu 抓包页
            draw_sniffer();
            break;
        case oled_page::page_unk_log:   // v4.0-tpu 未知指令完整记录页（自动翻页）
            draw_unk_log();
            break;
#endif
        default:
            break;
    }
    flush();   // 差值刷新：只发变化字节，不闪不卡
}

#endif // BMCU_OLED