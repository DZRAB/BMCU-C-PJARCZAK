#include "_bus_hardware.h"

#include "ch32v20x.h"
#include "ch32v20x_rcc.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_usart.h"
#include "ch32v20x_dma.h"
#include "ch32v20x_misc.h"
#include "core_riscv.h"
#include "crc_bus.h"

uint16_t bus_host_device_type=0x0000;

DMA_InitTypeDef bus_uart1_dma_init_structure;
void bus_uart1_init();
void bus_uart1_dma_send(uint8_t *data, uint16_t length);


_bus_port_deal bus_port_to_host;

#define uart1_port_irq(data) bus_port_to_host.irq(data)
#define uart1_port_idle bus_port_to_host.idle
#define bus_port_to_host_send_func bus_uart1_dma_send

void bus_init()
{
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_CRC, ENABLE);
    bus_crc_init();
    bus_port_to_host.init(bus_port_to_host_send_func);
    bus_uart1_init();
}

// v4.0-tpu 方案A: 假离线 —— 关 USART1(停止收发+停 RX 中断), 打印机判定 BMCU 掉线; 主要用于"打印完成定时软复位"前模拟拔插
void bus_host_disconnect(void)
{
    USART_Cmd(USART1, DISABLE);            // 停收发包, RX 中断不再产生 -> 打印机视为掉线
    USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
}

// v4.0-tpu 方案A: 恢复在线 —— 重新使能 USART1(相当于给打印机重新插上 BMCU)
void bus_host_reconnect(void)
{
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1, ENABLE);
}

void bus_uart1_init()
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    USART_InitTypeDef USART_InitStructure = {0};
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    /* USART1 TX-->A.9   RX-->A.10   DE-->A.12*/
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9; // TX
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10; // RX
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12; // DE
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIOA->BCR = GPIO_Pin_12;

    USART_InitStructure.USART_BaudRate = 1250000;
    USART_InitStructure.USART_WordLength = USART_WordLength_9b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_Even;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;

    USART_Init(USART1, &USART_InitStructure);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_ITConfig(USART1, USART_IT_TC, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    // Configure DMA1 channel 4 for USART1 TX
    bus_uart1_dma_init_structure.DMA_PeripheralBaseAddr = (uint32_t)&USART1->DATAR;
    bus_uart1_dma_init_structure.DMA_MemoryBaseAddr = (uint32_t)0;
    bus_uart1_dma_init_structure.DMA_DIR = DMA_DIR_PeripheralDST;
    bus_uart1_dma_init_structure.DMA_Mode = DMA_Mode_Normal;
    bus_uart1_dma_init_structure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    bus_uart1_dma_init_structure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    bus_uart1_dma_init_structure.DMA_Priority = DMA_Priority_VeryHigh;
    bus_uart1_dma_init_structure.DMA_M2M = DMA_M2M_Disable;
    bus_uart1_dma_init_structure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    bus_uart1_dma_init_structure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    bus_uart1_dma_init_structure.DMA_BufferSize = 0;
    DMA_Init(DMA1_Channel4, &bus_uart1_dma_init_structure);

    USART_Cmd(USART1, ENABLE);
}

void bus_uart1_dma_send(unsigned char *data, uint16_t length)
{
    if (!bus_port_to_host.idle) return;

    bus_port_to_host.idle = false;

    DMA1_Channel4->CFGR &= (uint16_t)(~DMA_CFGR1_EN);

    DMA1_Channel4->MADDR = (uint32_t)data;
    DMA1_Channel4->CNTR  = length;

    // DE = TX
    GPIOA->BSHR = GPIO_Pin_12;

    // 清除 TC 标志
    USART_ClearITPendingBit(USART1, USART_IT_TC);

    USART1->CTLR3 |= USART_DMAReq_Tx;
    DMA1_Channel4->CFGR |= DMA_CFGR1_EN;
}

extern "C" void USART1_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        const uint8_t d = (uint8_t)USART_ReceiveData(USART1);
        if (bus_port_to_host.idle) uart1_port_irq(d);
    }
    if (USART_GetITStatus(USART1, USART_IT_TC) != RESET)
    {
        USART_ClearITPendingBit(USART1, USART_IT_TC);
        USART1->CTLR3 &= ~USART_DMAReq_Tx;
        DMA1_Channel4->CFGR &= (uint16_t)(~DMA_CFGR1_EN);

        // DE = RX
        GPIOA->BCR = GPIO_Pin_12;

        // TX done
        bus_port_to_host.idle = true;
    }
}