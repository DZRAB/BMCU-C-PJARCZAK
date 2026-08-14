#include "aht20.h"
#include "ch32v20x_rcc.h"

uint32_t AHT20::g_iic_delay_ticks = 1;

// 软件 I2C 引脚：PB10=SCL(开漏)，PB11=SDA(开漏)
GPIO_TypeDef* const AHT20::IIC_PORT_SCL = GPIOB;
GPIO_TypeDef* const AHT20::IIC_PORT_SDA = GPIOB;

// 软件 I2C 实现。硬件 I2C2 备用路径已删除（实测比软件 I2C 更费 Flash 且不省空间）。

// SDA 全程开漏输出：释放=置高（靠外部上拉拉高），拉低=低电平。
// 关键修复：CH32V203 上动态切换 CNF/MODE（输出<->输入上拉）不可靠，
// 故读 SDA 时也保持开漏输出，靠外部 4.7k 上拉读 IDR，绝不切输入模式。
void AHT20::sda_release()
{
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA); // 开漏释放 -> 外部上拉拉高
}

void AHT20::iic_start()
{
    sda_release();
    iic_delay();
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);
    gpio_hi(IIC_PORT_SCL,  IIC_PIN_SCL);
    iic_delay();
    gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);   // SDA 下降沿 = START
    iic_delay();
    gpio_lo(IIC_PORT_SCL,  IIC_PIN_SCL);
}

void AHT20::iic_stop()
{
    gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);
    iic_delay();
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    sda_release();                        // SDA 上升沿 = STOP（开漏释放）
    iic_delay();
}

bool AHT20::iic_write_byte(uint8_t b)
{
    for (uint8_t m = 0x80u; m; m >>= 1)
    {
        // 对齐验证版 I2C_WriteBit：先设 SDA，延时建立稳定，再拉高 SCL 采样
        if (b & m) sda_release();
        else       gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);
        iic_delay();
        gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
        iic_delay();
        gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    }
    // 第 9 个时钟读 ACK：SDA 全程开漏，释放靠上拉，读 IDR
    sda_release();
    iic_delay();
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    bool ack = ((IIC_PORT_SDA->INDR & IIC_PIN_SDA) == 0u); // 从设备拉低 = ACK
    gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    return ack;
}

uint8_t AHT20::iic_read_byte(bool ack)
{
    uint8_t b = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        // 对齐验证版 I2C_ReadBit：先移位，再释放 SDA 由从机驱动，SCL 高期间采样
        b <<= 1;
        sda_release();                    // 释放 SDA，从机驱动数据
        iic_delay();
        gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
        iic_delay();
        if (IIC_PORT_SDA->INDR & IIC_PIN_SDA) b |= 1u;
        gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    }
    // 回 ACK / NACK：仍全程开漏
    if (ack) gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);   // ACK = 拉低
    else     sda_release();                        // NACK = 释放高
    iic_delay();
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    sda_release();                        // 对齐验证版：读完后释放 SDA 回 idle 高
    return b;
}

bool AHT20::read_status(uint8_t& status)
{
    iic_start();
    if (!iic_write_byte(ADDR_R)) { iic_stop(); return false; }
    status = iic_read_byte(false);        // NACK 结束
    iic_stop();
    return true;
}

bool AHT20::sensor_init()
{
    // 官方手册：AHT20 上电即就绪，无任何初始化/软复位命令（手册没有 0xBA/0xE1 这类指令）。
    // 存在性由 read_blocking() 发 0xAC 测量命令的 ACK 判定，这里直接返回 true。
    return true;
}

void AHT20::init()
{
    g_iic_delay_ticks = 5u * time_hw_ticks_per_us(); // ~100kHz
    if (!g_iic_delay_ticks) g_iic_delay_ticks = 1u;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    // 空闲保持高电平
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);

    GPIO_InitTypeDef gi = {0};
    gi.GPIO_Speed = GPIO_Speed_50MHz;

    gi.GPIO_Mode = GPIO_Mode_Out_OD;      // SCL 开漏（符合标准 I2C 规范）
    gi.GPIO_Pin  = IIC_PIN_SCL;
    GPIO_Init(IIC_PORT_SCL, &gi);

    gi.GPIO_Mode = GPIO_Mode_Out_OD;      // SDA 开漏（关键安全，避免短路）
    gi.GPIO_Pin  = IIC_PIN_SDA;
    GPIO_Init(IIC_PORT_SDA, &gi);

    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);

    delay(200);                           // 上电稳定

    online_ = sensor_init();
}

void AHT20::start_measure()
{
    iic_start();
    iic_write_byte(ADDR_W);
    iic_write_byte(CMD_MEASURE);
    iic_write_byte(0x33);
    iic_write_byte(0x00);
    iic_stop();
}

bool AHT20::get_measure(float& temperature_c, float& humidity_percent)
{
    uint8_t buf[7] = {0};

    iic_start();
    if (!iic_write_byte(ADDR_R)) {
        iic_stop();
        if (run_fail_cnt_ < RUN_FAIL_LIMIT) run_fail_cnt_++;
        return false;
    }
    for (int i = 0; i < 6; i++) buf[i] = iic_read_byte(true);  // ACK
    buf[6] = iic_read_byte(false);                            // NACK
    iic_stop();

    uint8_t status = buf[0];

    if (status & STATUS_BUSY_MASK) {     // bit7=1 仍忙
        if (run_fail_cnt_ < RUN_FAIL_LIMIT) run_fail_cnt_++;
        return false;
    }

    // bit3 校准使能：AHT20 正常上电后应为 1，作为存在性指纹
    if ((status & STATUS_CAL_ENABLE_MASK) == 0u) {
        if (run_fail_cnt_ < RUN_FAIL_LIMIT) run_fail_cnt_++;
        return false;
    }

    uint32_t raw_h = ((uint32_t)buf[1] << 12) |
                     ((uint32_t)buf[2] << 4)  |
                     ((uint32_t)buf[3] >> 4);
    uint32_t raw_t = ((uint32_t)(buf[3] & 0x0Fu) << 16) |
                     ((uint32_t)buf[4] << 8) |
                     (uint32_t)buf[5];

    humidity_percent = (float)raw_h / 1048576.0f * 100.0f;
    temperature_c    = (float)raw_t / 1048576.0f * 200.0f - 50.0f;

    // 物理范围校验（AHT20 数据手册范围：温度 -40~85℃，湿度 0~100%）
    if (humidity_percent < 0.0f || humidity_percent > 100.0f ||
        temperature_c < -40.0f || temperature_c > 85.0f) {
        if (run_fail_cnt_ < RUN_FAIL_LIMIT) run_fail_cnt_++;
        return false;
    }

    run_fail_cnt_ = 0u;

    this->humidity_percent = humidity_percent;
    this->temperature_c    = temperature_c;
    return true;
}

bool AHT20::read_blocking(float& temperature_c, float& humidity_percent)
{
    // 上电自检：多次尝试判定是否存在，避免 SDA 浮空导致单次 ACK 误判
    for (uint8_t attempt = 0; attempt < PROBE_ATTEMPTS; ++attempt) {
        start_measure();

        // 手册要求：发送测量命令后等待 >=80ms 测量完成。
        // 先保底 delay(80)，再轮询忙标志（状态字 bit7==0 表示就绪），超时 100ms。
        delay(80);
        uint8_t status = 0;
        bool got_status = false;
        for (int t = 0; t < 100; t++) {
            if (!read_status(status)) break;
            got_status = true;
            if ((status & STATUS_BUSY_MASK) == 0u) break;          // 就绪
            delay(1);
        }

        if (got_status && ((status & STATUS_BUSY_MASK) == 0u)) {
            if (get_measure(temperature_c, humidity_percent)) {
                online_ = true;
                return true;
            }
        }
    }

    online_ = false;
    return false;
}
