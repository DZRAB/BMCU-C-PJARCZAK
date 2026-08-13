#include "i2c_hw/i2c2_hw.h"
#include "ch32v20x.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_rcc.h"
#include "ch32v20x_i2c.h"
#include "hal/time_hw.h"

// PB10 = I2C2_SCL, PB11 = I2C2_SDA（默认复用，无需 remap）
static const uint32_t HW_I2C2_SPEED = 100000u; // 100 kHz，兼容 AHT20/OLED

// 简单忙等待超时（单位：任意轮询计数）。SYSCLK=144MHz 时约 1ms 数量级。
static bool i2c2_wait_flag(uint32_t flag, uint8_t wait_set)
{
    for (volatile uint32_t t = 0; t < 200000u; ++t) {
        if (wait_set) {
            if (I2C_GetFlagStatus(I2C2, flag) != RESET) return true;
        } else {
            if (I2C_GetFlagStatus(I2C2, flag) == RESET) return true;
        }
    }
    return false;
}

bool hw_i2c2_init(void)
{
    GPIO_InitTypeDef  gi = {0};
    I2C_InitTypeDef   ii = {0};

    // 时钟：GPIOB(AFIO 不需要，I2C2 在 PB10/11 是默认复用)
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);

    // PB10/PB11 复用开漏
    gi.GPIO_Pin   = GPIO_Pin_10;
    gi.GPIO_Mode  = GPIO_Mode_AF_OD;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gi);

    gi.GPIO_Pin   = GPIO_Pin_11;
    GPIO_Init(GPIOB, &gi);

    // 外设初始化
    ii.I2C_ClockSpeed          = HW_I2C2_SPEED;
    ii.I2C_Mode                = I2C_Mode_I2C;
    ii.I2C_DutyCycle           = I2C_DutyCycle_2;
    ii.I2C_OwnAddress1         = 0x00;
    ii.I2C_Ack                 = I2C_Ack_Enable;
    ii.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C2, &ii);

    I2C_Cmd(I2C2, ENABLE);

    // 等待总线空闲
    return i2c2_wait_flag(I2C_FLAG_BUSY, 0);
}

// 发送 START，等待 EV5
static bool i2c2_start(void)
{
    if (!i2c2_wait_flag(I2C_FLAG_BUSY, 0)) return false;
    I2C_GenerateSTART(I2C2, ENABLE);
    return i2c2_wait_flag(I2C_FLAG_SB, 1); // EV5
}

// 发送 7bit 地址 + 方向，等待 EV6
static bool i2c2_send_addr(uint8_t addr7, uint8_t direction)
{
    I2C_Send7bitAddress(I2C2, (uint8_t)(addr7 << 1u), direction);
    if (direction == I2C_Direction_Transmitter) {
        return i2c2_wait_flag(I2C_FLAG_ADDR, 1); // EV6
    } else {
        return i2c2_wait_flag(I2C_FLAG_ADDR, 1); // EV6
    }
}

// 清 ADDR 标志（读 SR1 再读 SR2）
static __attribute__((always_inline)) void i2c2_clear_addr(void)
{
    (void)I2C2->STAR1;
    (void)I2C2->STAR2;
}

bool hw_i2c2_write(uint8_t addr7, const uint8_t* data, uint8_t len, bool send_stop)
{
    if (!data && len) return false;

    if (!i2c2_start()) return false;
    if (!i2c2_send_addr(addr7, I2C_Direction_Transmitter)) {
        I2C_GenerateSTOP(I2C2, ENABLE);
        return false;
    }
    i2c2_clear_addr();

    for (uint8_t i = 0; i < len; ++i) {
        if (!i2c2_wait_flag(I2C_FLAG_TXE, 1)) {
            I2C_GenerateSTOP(I2C2, ENABLE);
            return false;
        }
        I2C_SendData(I2C2, data[i]);
    }

    // 等最后一字节发送完成
    if (!i2c2_wait_flag(I2C_FLAG_BTF, 1)) {
        I2C_GenerateSTOP(I2C2, ENABLE);
        return false;
    }

    if (send_stop) {
        I2C_GenerateSTOP(I2C2, ENABLE);
    }
    return true;
}

bool hw_i2c2_read(uint8_t addr7, uint8_t* data, uint8_t len)
{
    if (!data || len == 0) return false;

    if (!i2c2_start()) return false;

    // 单字节读：先 disable ACK，再发地址，清 ADDR，然后发 STOP，等 RXNE
    if (len == 1u) {
        I2C_AcknowledgeConfig(I2C2, DISABLE);
        if (!i2c2_send_addr(addr7, I2C_Direction_Receiver)) {
            I2C_GenerateSTOP(I2C2, ENABLE);
            return false;
        }
        i2c2_clear_addr();
        I2C_GenerateSTOP(I2C2, ENABLE);
        if (!i2c2_wait_flag(I2C_FLAG_RXNE, 1)) return false;
        data[0] = I2C_ReceiveData(I2C2);
        return true;
    }

    // 多字节读
    I2C_AcknowledgeConfig(I2C2, ENABLE);
    if (!i2c2_send_addr(addr7, I2C_Direction_Receiver)) {
        I2C_GenerateSTOP(I2C2, ENABLE);
        return false;
    }
    i2c2_clear_addr();

    for (uint8_t i = 0; i < len; ++i) {
        if (i == len - 1u) {
            I2C_AcknowledgeConfig(I2C2, DISABLE);
            I2C_GenerateSTOP(I2C2, ENABLE);
        }
        if (!i2c2_wait_flag(I2C_FLAG_RXNE, 1)) return false;
        data[i] = I2C_ReceiveData(I2C2);
    }
    return true;
}

bool hw_i2c2_write_then_read(uint8_t addr7,
                             const uint8_t* wdata, uint8_t wlen,
                             uint8_t* rdata, uint8_t rlen)
{
    // 第一阶段：写，不发送 STOP（SR=Repeated START）
    if (!hw_i2c2_write(addr7, wdata, wlen, false)) return false;

    // 第二阶段：读。hw_i2c2_read 会发 START + 读地址 + STOP
    return hw_i2c2_read(addr7, rdata, rlen);
}
