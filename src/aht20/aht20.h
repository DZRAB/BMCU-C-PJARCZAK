#pragma once
#include <stdint.h>
#include "ch32v20x.h"
#include "ch32v20x_gpio.h"
#include "hal/time_hw.h"

/*
 * AHT20 温湿度传感器驱动（软件 I2C，独立通道）
 *
 *   SCL = PB10  (推挽输出，主设备时钟；AHT20 不拉伸 SCL，故推挽安全)
 *   SDA = PB11  (开漏输出 —— 关键安全：
 *               写时拉低=低电平，释放=靠外部上拉拉高；
 *               读时切输入上拉(高阻)。绝不主动输出强高电平，
 *               避免与从设备电平冲突造成短路/炸电源芯片)
 *
 * 与打印机通讯：读取到的温湿度填入
 *   ams[].filament[].compartment_temperature / compartment_humidity
 * 由现有 ahub / bambu 协议自动上报，无需改动协议层。
 *
 * 注意：本驱动使用软件 I2C，因为 CH32V203 硬件 I2C 存在问题。
 */
class AHT20
{
public:
    AHT20() = default;
    ~AHT20() = default;

    // 初始化软件 I2C 并启动 AHT20（必须在 time_hw_init() 之后调用）
    void init();
    bool is_online() const { return online_; }

    // 阻塞读取一次（用于初始化自检）
    bool read_blocking(float& temperature_c, float& humidity_percent);

    // 非阻塞测量：先 start_measure()，延时约 90ms 后 get_measure()
    void start_measure();
    bool get_measure(float& temperature_c, float& humidity_percent);

    float temperature_c    = 0.0f;  // 最近一次温度 ℃
    float humidity_percent = 0.0f;  // 最近一次湿度 %

private:
    // 注意：GPIOx 是 reinterpret_cast 宏，不能用于 constexpr，故用普通 static 成员
    static GPIO_TypeDef* const IIC_PORT_SCL;
    static const uint16_t      IIC_PIN_SCL  = GPIO_Pin_10;
    static GPIO_TypeDef* const IIC_PORT_SDA;
    static const uint16_t      IIC_PIN_SDA  = GPIO_Pin_11;

    static constexpr uint8_t ADDR_W = 0x70; // 0x38 << 1
    static constexpr uint8_t ADDR_R = 0x71;

    static constexpr uint8_t CMD_SOFT_RESET = 0xBA;
    static constexpr uint8_t CMD_INIT       = 0xE1;
    static constexpr uint8_t CMD_MEASURE    = 0xAC;

    bool online_ = false;
    static uint32_t g_iic_delay_ticks;

    static inline void gpio_hi(GPIO_TypeDef* p, uint16_t pin) { p->BSHR = pin; }
    static inline void gpio_lo(GPIO_TypeDef* p, uint16_t pin) { p->BCR  = pin; }

    // 修改单个引脚的 4-bit CFG（CNF+MODE），不影响同寄存器其他引脚
    static inline void gpio_cfg(GPIO_TypeDef* p, uint16_t pinMask, uint32_t cfg4)
    {
        uint32_t pin = (uint32_t)__builtin_ctz((uint32_t)pinMask);
        volatile uint32_t* cfg = (pin < 8u) ? &p->CFGLR : &p->CFGHR;
        uint32_t shift = (pin & 7u) * 4u;
        uint32_t v = *cfg;
        v = (v & ~(0xFu << shift)) | ((cfg4 & 0xFu) << shift);
        *cfg = v;
    }

    void iic_delay() const { delayTicks32(g_iic_delay_ticks); }

    void sda_input_pu();  // 读：SDA 输入上拉（高阻，靠外部上拉拉高）
    void sda_output_od(); // 写：SDA 开漏（释放=高，拉低=低；绝不强推高）

    void iic_start();
    void iic_stop();
    bool iic_write_byte(uint8_t b);   // 返回 true=收到 ACK
    uint8_t iic_read_byte(bool ack);  // ack=true 时回 ACK

    bool sensor_init();               // 发送 0xE1 0x08 0x00 并校验状态 bit2
    bool read_status(uint8_t& status);
};
