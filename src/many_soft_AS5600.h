#pragma once
#include <stdint.h>

#include "ch32v20x_gpio.h"
#include "ch32v20x.h"

class AS5600_soft_IIC_many
{
public:
    enum _AS5600_magnet_stu
    {
        low     = 1,
        high    = 2,
        offline = -1,
        normal  = 0
    };

    AS5600_soft_IIC_many();
    ~AS5600_soft_IIC_many();

    // 端口 / 引脚（通道顺序）
    void init(GPIO_TypeDef* const* GPIO_SCL_port, const uint16_t* GPIO_SCL_pin,
              GPIO_TypeDef* const* GPIO_SDA_port, const uint16_t* GPIO_SDA_pin,
              int num);

    // API
    void updata_stu();
    void updata_angle();

    // 公开成员（一一对应）
    bool*               online;       // [numbers]
    _AS5600_magnet_stu* magnet_stu;   // [numbers]
    uint16_t*           raw_angle;    // [numbers]
    uint16_t*           data;         // [numbers]
    int                 numbers;

private:
    static constexpr int kMax = 4;

    // 静态缓冲区
    bool               online_buf[kMax];
    _AS5600_magnet_stu magnet_buf[kMax];
    uint16_t           raw_buf[kMax];
    uint16_t           data_buf[kMax];
    int                error_buf[kMax];

    GPIO_TypeDef*      port_SDA_buf[kMax];
    GPIO_TypeDef*      port_SCL_buf[kMax];
    uint16_t           pin_SDA_buf[kMax];
    uint16_t           pin_SCL_buf[kMax];

    // 内部指针
    int*          error;
    GPIO_TypeDef** port_SDA;
    GPIO_TypeDef** port_SCL;
    uint16_t*      pin_SDA;
    uint16_t*      pin_SCL;

    void init_iic();
    void start_iic(unsigned char ADR);
    void stop_iic();
    void write_iic(uint8_t byte);
    void read_iic(bool ack);
    void wait_ack_iic();
    void clear_datas();
    void read_reg8(uint8_t reg);
    void read_reg16(uint8_t reg);

    // WCH 辅助函数
    void enable_gpio_clock(GPIO_TypeDef* p);
    void sda_mode_ipu(int i); // INPUT_PULLUP
    void sda_mode_od(int i);  // OUTPUT_OD

    // SET_H/SET_L 一一对应（屏蔽 error[i]）
    void set_h(GPIO_TypeDef* const* port, const uint16_t* pin);
    void set_l(GPIO_TypeDef* const* port, const uint16_t* pin);
};
