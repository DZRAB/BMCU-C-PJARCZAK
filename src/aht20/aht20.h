#pragma once
#include <stdint.h>
#include "ch32v20x.h"
#include "ch32v20x_gpio.h"
#include "hal/time_hw.h"

// BMCU_AHT20：AHT20 温湿度传感器总开关（编译宏，缺省 1）。
//   1 = 编入 AHT20 驱动（新主板有传感器）；0 = 剥离 AHT20 驱动（老主板无传感器，省 Flash/RAM）。
//   可由编译脚本注入 -DBMCU_AHT20=0 覆盖（见 build_one.sh / build_all_firmwares_fast.py）。
//   注意：关闭 AHT20 时，OLED 复用其软件 I2C 总线（PB10/PB11 开漏）将失去底层，
//        故关闭 AHT20 时 OLED 也无法工作（老主板无屏无传感器可同时 BMCU_OLED=0）。
#ifndef BMCU_AHT20
#define BMCU_AHT20 1
#endif

/*
 * AHT20 温湿度传感器驱动（软件 I2C，独立通道）
 *
 *   SCL = PB10  (开漏输出，符合标准 I2C 规范)
 *   SDA = PB11  (开漏输出，全程开漏：
 *               写时拉低=低电平，释放=靠外部上拉拉高；
 *               读时也保持开漏、释放 SDA 靠上拉读 IDR，绝不切输入模式。
 *               避免 CH32V203 上动态改 CNF/MODE 引起的不稳定)
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

    // 阻塞读取一次（用于初始化自检，内部多次重试）
    bool read_blocking(float& temperature_c, float& humidity_percent);

    // 非阻塞测量：先 start_measure()，延时约 90ms 后 get_measure()
    void start_measure();
    bool get_measure(float& temperature_c, float& humidity_percent);

    // 传感器当前是否在线：上电自检成功 且 运行期未连续失败达到阈值
    bool is_online() const { return online_ && (run_fail_cnt_ < RUN_FAIL_LIMIT); }

    float temperature_c    = 0.0f;  // 最近一次温度 ℃
    float humidity_percent = 0.0f;  // 最近一次湿度 %

    // ===== 软件 I2C 总线对外转发（仅供同总线设备如 OLED 复用，不破坏 AHT20 私有逻辑）=====
    // OLED 复用 AHT20 已验证稳定的软件 I2C 时序（CH32V203 上自写时序不稳，故复用）。
    // 调用前必须保证本对象已 init()（引脚已配置为 PB10/PB11 开漏）。
    inline void bus_start()        { iic_start(); }
    inline bool bus_write(uint8_t b) { return iic_write_byte(b); }   // 返回 true=ACK
    inline void bus_stop()         { iic_stop(); }

private:
    // 注意：GPIOx 是 reinterpret_cast 宏，不能用于 constexpr，故用普通 static 成员
    static GPIO_TypeDef* const IIC_PORT_SCL;
    static const uint16_t      IIC_PIN_SCL  = GPIO_Pin_10;
    static GPIO_TypeDef* const IIC_PORT_SDA;
    static const uint16_t      IIC_PIN_SDA  = GPIO_Pin_11;

    static constexpr uint8_t ADDR_W = 0x70; // 0x38 << 1
    static constexpr uint8_t ADDR_R = 0x71;

    static constexpr uint8_t CMD_MEASURE    = 0xAC;

    // 状态字关键位（见 AHT20 数据手册表 9）
    static constexpr uint8_t STATUS_BUSY_MASK        = 0x80u;  // bit[7] 忙闲指示
    static constexpr uint8_t STATUS_CAL_ENABLE_MASK  = 0x08u;  // bit[3] 校准计算使能，上电后应为 1
    static constexpr uint8_t STATUS_CRC_FLAG_MASK    = 0x10u;  // bit[4] CRC_flag

    static constexpr uint8_t RUN_FAIL_LIMIT = 10u;   // 运行期连续失败阈值，达到则判离线
    static constexpr uint8_t PROBE_ATTEMPTS = 5u;  // 上电自检最多尝试次数

    bool online_ = false;           // 上电自检是否成功检测到 AHT20
    uint8_t run_fail_cnt_ = 0u;     // 运行期连续失败计数
    static uint32_t g_iic_delay_ticks;

    static inline void gpio_hi(GPIO_TypeDef* p, uint16_t pin) { p->BSHR = pin; }
    static inline void gpio_lo(GPIO_TypeDef* p, uint16_t pin) { p->BCR  = pin; }

    void iic_delay() const { delayTicks32(g_iic_delay_ticks); }

    void sda_release();    // SDA 全程开漏：释放=置高，靠外部上拉拉高（读时仍用此状态）

    void iic_start();
    void iic_stop();
    bool iic_write_byte(uint8_t b);   // 返回 true=收到 ACK
    uint8_t iic_read_byte(bool ack);  // ack=true 时回 ACK

    // AHT20 上电即就绪，无需额外初始化命令；存在性由 read_blocking() 做多次判定。
    bool sensor_init();
    bool read_status(uint8_t& status);
};
