#include "aht20.h"
#include "ch32v20x_rcc.h"

uint32_t AHT20::g_iic_delay_ticks = 1;

// 软件 I2C 引脚：PB10=SCL(开漏)，PB11=SDA(开漏)
GPIO_TypeDef* const AHT20::IIC_PORT_SCL = GPIOB;
GPIO_TypeDef* const AHT20::IIC_PORT_SDA = GPIOB;

// SDA 切输入上拉：CNF/MODE = 1000b（输入 + 上拉/下拉，ODR=1 选上拉）
void AHT20::sda_input_pu()
{
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);
    gpio_cfg(IIC_PORT_SDA, IIC_PIN_SDA, 0x8u);
}

// SDA 切开漏输出 50MHz：CNF/MODE = 0111b（开漏，释放=高，拉低=低）
void AHT20::sda_output_od()
{
    gpio_cfg(IIC_PORT_SDA, IIC_PIN_SDA, 0x7u);
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA); // 释放为高电平
}

void AHT20::iic_start()
{
    sda_output_od();
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
    sda_output_od();
    gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);
    iic_delay();
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);   // SDA 上升沿 = STOP
    iic_delay();
}

bool AHT20::iic_write_byte(uint8_t b)
{
    sda_output_od();
    for (uint8_t m = 0x80u; m; m >>= 1)
    {
        iic_delay();
        if (b & m) gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);
        else       gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);
        gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
        iic_delay();
        gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    }
    // 第 9 个时钟读 ACK
    gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);   // 释放 SDA（开漏，靠上拉）
    sda_input_pu();
    iic_delay();
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    bool ack = ((IIC_PORT_SDA->INDR & IIC_PIN_SDA) == 0u); // 从设备拉低 = ACK
    gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    sda_output_od();
    return ack;
}

uint8_t AHT20::iic_read_byte(bool ack)
{
    sda_input_pu();                        // SDA 输入（高阻）
    uint8_t b = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        iic_delay();
        gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
        iic_delay();
        b <<= 1;
        if (IIC_PORT_SDA->INDR & IIC_PIN_SDA) b |= 1u;
        gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    }
    // 回 ACK / NACK
    sda_output_od();
    iic_delay();
    if (ack) gpio_lo(IIC_PORT_SDA, IIC_PIN_SDA);   // ACK = 拉低
    else     gpio_hi(IIC_PORT_SDA, IIC_PIN_SDA);   // NACK = 释放高
    gpio_hi(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
    gpio_lo(IIC_PORT_SCL, IIC_PIN_SCL);
    iic_delay();
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
    // 软复位（确保干净状态）
    iic_start();
    iic_write_byte(ADDR_W);
    iic_write_byte(CMD_SOFT_RESET);
    iic_stop();
    delay(20);

    // 初始化命令 0xE1 0x08 0x00
    iic_start();
    iic_write_byte(ADDR_W);
    iic_write_byte(CMD_INIT);
    iic_write_byte(0x08);
    iic_write_byte(0x00);
    iic_stop();
    delay(10);

    // 校验校准使能位 bit2（=1 表示已校准）
    uint8_t status = 0;
    if (!read_status(status)) return false;
    return (status & 0x04u) != 0u;
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
    uint8_t buf[7];
    iic_start();
    if (!iic_write_byte(ADDR_R)) { iic_stop(); return false; }
    for (int i = 0; i < 6; i++) buf[i] = iic_read_byte(true);  // ACK
    buf[6] = iic_read_byte(false);                            // NACK
    iic_stop();

    if (buf[0] & 0x80u) return false;     // bit7=1 仍忙

    uint32_t raw_h = ((uint32_t)buf[1] << 12) |
                     ((uint32_t)buf[2] << 4)  |
                     ((uint32_t)buf[3] >> 4);
    uint32_t raw_t = ((uint32_t)(buf[3] & 0x0Fu) << 16) |
                     ((uint32_t)buf[4] << 8) |
                     (uint32_t)buf[5];

    humidity_percent = (float)raw_h / 1048576.0f * 100.0f;
    temperature_c    = (float)raw_t / 1048576.0f * 200.0f - 50.0f;

    this->humidity_percent = humidity_percent;
    this->temperature_c    = temperature_c;
    return true;
}

bool AHT20::read_blocking(float& temperature_c, float& humidity_percent)
{
    if (!online_ && !sensor_init()) { online_ = false; return false; }
    online_ = true;
    start_measure();
    delay(90);                            // 等待测量完成（典型 ~80ms）
    return get_measure(temperature_c, humidity_percent);
}
