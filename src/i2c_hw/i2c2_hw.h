#pragma once
#include <stdint.h>
#include <stdbool.h>

/* CH32V203 硬件 I2C2 驱动（PB10=SCL, PB11=SDA）
 *
 * 设计目标：替代 AHT20/OLED 共用的软件 I2C bitbang，以节省 Flash。
 * 当前仅实现主设备轮询模式，覆盖 AHT20 和 SSD1306 的读写需求。
 */

// 使能硬件 I2C2。关闭则回退到原有软件 I2C（代码保留，LTO 自动剔除）。
// 注意：当前使用 WCH 标准库函数版本，实测比软件 I2C 多占约 120 字节 Flash，
// 反而加剧 rgb1 变体溢出。故默认禁用，待后续改寄存器直接操作版后再开启。
#define BMCU_USE_HW_I2C2  0

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 I2C2 外设、GPIO(AF_OD) 和时钟。重复调用安全。
bool hw_i2c2_init(void);

// 主设备写：START + 写地址 + data[0..len-1] + (send_stop ? STOP : 不发送 STOP)
// 返回 true=收到所有 ACK；false=NACK/超时/总线错误。
bool hw_i2c2_write(uint8_t addr7, const uint8_t* data, uint8_t len, bool send_stop);

// 主设备读：START + 读地址 + 读 len 字节(最后字节 NACK) + STOP
bool hw_i2c2_read(uint8_t addr7, uint8_t* data, uint8_t len);

// 写后重启读：START + 写地址 + wdata + SR + 读地址 + rdata + STOP
// 适合 AHT20: 先写测量命令/寄存器地址，再读结果。
bool hw_i2c2_write_then_read(uint8_t addr7,
                             const uint8_t* wdata, uint8_t wlen,
                             uint8_t* rdata, uint8_t rlen);

#ifdef __cplusplus
}
#endif
