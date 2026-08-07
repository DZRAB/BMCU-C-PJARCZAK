#include "MC_PULL_calibration.h"
#include "ws2812.h"

#include "Flash_saves.h"
#include "Motion_control.h"
#include "_bus_hardware.h"
#include "ams.h"
#include "ahub_bus.h"
#include "bambu_bus_ams.h"
#include "ADC_DMA.h"
#include "Debug_log.h"
#include "aht20/aht20.h"
#include "oled/ssd1306_oled.h"   // OLED 驱动（BMCU_OLED 宏包住，默认不编译）
#include "sim_aht20.h"
#include "hal/time_hw.h"
#include <string.h>

WS2812_class SYS_RGB;
WS2812_class RGBOUT[4];

AHT20 g_aht20;   // 全局可见：OLED 同总线设备需复用其软件 I2C 底层

void RGB_init()
{
    SYS_RGB.init(1, GPIOD, GPIO_Pin_1);
    RGBOUT[0].init(2, GPIOA, GPIO_Pin_11);
    RGBOUT[1].init(2, GPIOA, GPIO_Pin_8);
    RGBOUT[2].init(2, GPIOB, GPIO_Pin_1);
    RGBOUT[3].init(2, GPIOB, GPIO_Pin_0);
}

void RGB_update()
{
    if (!(SYS_RGB.is_dirty() ||
          RGBOUT[0].is_dirty() || RGBOUT[1].is_dirty() ||
          RGBOUT[2].is_dirty() || RGBOUT[3].is_dirty()))
        return;

    static uint32_t last = 0u;

    uint32_t min_gap = time_hw_tpms;
    if (!min_gap) min_gap = 1u;

    const uint32_t now = time_ticks32();
    if (last != 0u && (uint32_t)(now - last) < min_gap)
        return;

    last = now;

    SYS_RGB.updata();
    RGBOUT[0].updata();
    RGBOUT[1].updata();
    RGBOUT[2].updata();
    RGBOUT[3].updata();
}

static uint8_t g_fil_dirty = 0;
static uint8_t g_loaded_ch = 0xFF;
static uint8_t g_state_dirty = 0;

static inline void ram_to_flashinfo(uint8_t fil, Flash_FilamentInfo* o)
{
    const _filament* f = &ams[BAMBU_BUS_AMS_NUM].filament[fil];

    memcpy(o->bambubus_filament_id, f->bambubus_filament_id, sizeof(o->bambubus_filament_id));
    o->color_R = f->color_R;
    o->color_G = f->color_G;
    o->color_B = f->color_B;
    o->color_A = f->color_A;
    o->temperature_min = f->temperature_min;
    o->temperature_max = f->temperature_max;
    memcpy(o->name, f->name, sizeof(o->name));
}

static inline void flashinfo_to_ram(uint8_t fil, const Flash_FilamentInfo* i)
{
    _filament* f = &ams[BAMBU_BUS_AMS_NUM].filament[fil];

    memcpy(f->bambubus_filament_id, i->bambubus_filament_id, sizeof(i->bambubus_filament_id));
    f->color_R = i->color_R;
    f->color_G = i->color_G;
    f->color_B = i->color_B;
    f->color_A = i->color_A;
    f->temperature_min = i->temperature_min;
    f->temperature_max = i->temperature_max;

    memset(f->name, 0, sizeof(f->name));
    memcpy(f->name, i->name, sizeof(i->name));
    f->name[sizeof(f->name) - 1u] = 0;
}

bool ams_datas_read()
{
    bool any = false;

    for (uint8_t fil = 0; fil < 4u; fil++)
    {
        Flash_FilamentInfo fi;
        if (Flash_AMS_filament_read(fil, &fi))
        {
            flashinfo_to_ram(fil, &fi);
            any = true;
        }
    }

    return any;
}

void ams_datas_set_need_to_save()
{
    g_fil_dirty = 0x0Fu;
}

void ams_datas_set_need_to_save_filament(uint8_t filament_idx)
{
    if (filament_idx >= 4u) return;
    g_fil_dirty |= (uint8_t)(1u << filament_idx);
}

void ams_state_set_loaded(uint8_t filament_ch)
{
    if (filament_ch >= 4u) return;
    if (g_loaded_ch != 0xFFu) return;
    g_loaded_ch = filament_ch;
    g_state_dirty = 1u;
}

void ams_state_set_unloaded(uint8_t filament_ch)
{
    if (g_loaded_ch == 0xFFu) return;
    if (filament_ch < 4u && g_loaded_ch != filament_ch) return;
    g_loaded_ch = 0xFFu;
    g_state_dirty = 1u;
}

uint8_t ams_state_get_loaded(void)
{
    return g_loaded_ch;
}

static void ams_state_save_run()
{
    if (!g_state_dirty) return;

    if (Flash_AMS_state_write(g_loaded_ch))
        g_state_dirty = 0u;
}

void ams_datas_save_run()
{
    if (!g_fil_dirty) return;

    uint8_t fil = 0xFFu;
    for (uint8_t i = 0; i < 4u; i++)
    {
        if (g_fil_dirty & (uint8_t)(1u << i))
        {
            fil = i;
            break;
        }
    }

    if (fil == 0xFFu) return;

    Flash_FilamentInfo now;
    ram_to_flashinfo(fil, &now);

    if (Flash_AMS_filament_write(fil, &now))
        g_fil_dirty &= (uint8_t)~(1u << fil);
}

int main(void)
{
    SystemInit();
    SystemCoreClockUpdate();
    time_hw_init();

    __enable_irq();

    WWDG_DeInit();
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_WWDG, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    GPIO_PinRemapConfig(GPIO_Remap_PD01, ENABLE);

    RGB_init();
    delay(10);

    SYS_RGB.set_RGB(0x10, 0x00, 0x00, 0);
    for (int i = 0; i < 4; i++) RGBOUT[i].set_RGB(0, 0, 0, 0);
    RGB_update();
    delay(50);

    DEBUG_init();
    ams_init();
    Flash_saves_init();

    ADC_DMA_init();
    ADC_DMA_wait_full();

    // ===== 每次上电都先检测 AHT20 是否存在（不阻塞后续 BMCU 流程）=====
    // 蓝灯=检测到 AHT20；绿灯=成功读到一次温湿度（可正常工作）；
    // 检测不到或读失败均不阻塞，立即进入正常 BMCU 逻辑（含首次开机校准）。
    // 探测以“能否成功读一次温湿度”为准。
    g_aht20.init();
    {
        float t = 0.0f, h = 0.0f;
        if (g_aht20.read_blocking(t, h))
        {
            SYS_RGB.set_RGB(0x00, 0x00, 0x10, 0);   // 蓝灯：检测到 AHT20 且存在
            RGB_update();
            delay(200);
            SYS_RGB.set_RGB(0x00, 0x10, 0x00, 0);   // 绿灯：成功读到温湿度
            RGB_update();
            delay(200);
        }
        // 读失败=检测不到 AHT20：不亮灯、不阻塞，立即进入正常 BMCU 逻辑
        SYS_RGB.set_RGB(0x00, 0x00, 0x00, 0);
        RGB_update();
    }

#ifdef BMCU_OLED
    // OLED 在 AHT20 自检后立即初始化（复用其 I2C 引脚），让屏从开机最早阶段就参与，
    // 显示系统启动进度，而非等所有初始化跑完才亮。
    SSD1306_OLED::init();
    if (SSD1306_OLED::is_ready())
        SSD1306_OLED::draw_message("BMCU BOOT", "", "", "");
#endif // BMCU_OLED

#ifdef BMCU_OLED
    if (SSD1306_OLED::is_ready())
        SSD1306_OLED::draw_message("BMCU BOOT", "CALIB...", "", "");
#endif // BMCU_OLED
    MC_PULL_calibration_boot();
#ifdef BMCU_OLED
    if (SSD1306_OLED::is_ready())
        SSD1306_OLED::draw_message("BMCU BOOT", "CALIB OK", "", "");
#endif // BMCU_OLED

    ams_datas_read();

#ifdef BMCU_OLED
    if (SSD1306_OLED::is_ready())
        SSD1306_OLED::draw_message("BMCU BOOT", "LOAD...", "", "");
#endif // BMCU_OLED

    {
        uint8_t ch = 0xFFu;
        if (Flash_AMS_state_read(&ch))
        {
            g_loaded_ch = ch;

            if (ch < 4u)
            {
                _ams* a = &ams[BAMBU_BUS_AMS_NUM];

                a->now_filament_num  = ch;
                a->filament_use_flag = 0x04;
                a->pressure          = 0x2B00;

                for (uint8_t i = 0; i < 4u; i++)
                    a->filament[i].motion = _filament_motion::idle;

                a->filament[ch].motion = _filament_motion::on_use;
            }
        }
    }

#ifdef BMCU_OLED
    if (SSD1306_OLED::is_ready())
    {
        // 开机完成：切到常规画面（有 AHT20 显温湿度，无则 NO AHT20）。
        if (g_aht20.is_online())
            SSD1306_OLED::draw_aht20(true, true,
                                     g_aht20.temperature_c,
                                     g_aht20.humidity_percent, true);
        else
            SSD1306_OLED::draw_aht20(false, false, 0.0f, 0.0f, true);
    }
#endif // BMCU_OLED

    Motion_control_init();
    bambubus_init();
    bus_init();

    DEBUG("START\n");

    while (1)
    {
        const ahubus_package_type   ahub_stu     = ahubus_run();
        const bambubus_package_type bambubus_stu = bambubus_run();
        bus_port_to_host.send_package();

        static int error = 0;
        static bool comm_ok = true;   // 通讯是否正常（供心跳灯状态机使用）

        if ((ahub_stu != ahubus_package_type::none) || (bambubus_stu != bambubus_package_type::none))
        {
            if ((ahub_stu != ahubus_package_type::error) || (bambubus_stu != bambubus_package_type::error))
            {
                error = 0;
                comm_ok = true;

                if (bambubus_stu == bambubus_package_type::heartbeat)
                    bus_host_device_type = host_device_type_ams;

                if (ahub_stu == ahubus_package_type::heartbeat)
                    bus_host_device_type = host_device_type_ahub;

                ams_datas_save_run();
                ams_state_save_run();
            }
            else
            {
                error = -1;
                comm_ok = false;   // [测试用] 取消注释以模拟通讯失败，便于不上机测试功能；正式提交前需恢复为通讯成功
            }
        }

        // ===== SYS_RGB 心跳指示灯 =====
        // 有 AHT20 时：白灯常亮会烤高温度，故改为每 3 秒闪一下白光（~150ms），异常时红灯常亮；
        // 无 AHT20 时：保持原白色常亮逻辑（无烤温顾虑，且状态可见）。
        {
            static uint64_t hb_next_ms   = 0;
            static uint64_t hb_off_ms    = 0;
            static bool     hb_lit       = false;
            const uint64_t  now_ms       = time_ms64();
            const uint64_t  HB_PERIOD_MS = 3000u;   // 闪烁周期
            const uint64_t  HB_LIT_MS    = 150u;    // 单次点亮时长

            if (comm_ok)
            {
                if (g_aht20.is_online())
                {
                    // 有 AHT20：闪烁模式，避免常亮烤温
                    if (!hb_lit && (now_ms - hb_next_ms) >= HB_PERIOD_MS)
                    {
                        SYS_RGB.set_RGB(0x38, 0x35, 0x32, 0);
                        hb_lit    = true;
                        hb_off_ms = now_ms + HB_LIT_MS;
                    }
                    else if (hb_lit && now_ms >= hb_off_ms)
                    {
                        SYS_RGB.set_RGB(0x00, 0x00, 0x00, 0);
                        hb_lit    = false;
                        hb_next_ms = now_ms;
                    }
                }
                else
                {
                    // 无 AHT20：原白色常亮
                    SYS_RGB.set_RGB(0x38, 0x35, 0x32, 0);
                    hb_lit = false;
                }
            }
            else
            {
                // 通讯异常：红色常亮
                SYS_RGB.set_RGB(0x10, 0x00, 0x00, 0);
                hb_lit = false;
            }
        }

        // ===== AHT20 环境温湿度（非阻塞采样）=====
        // 在线时 2 秒采样一次；离线后延长至 10 秒尝试恢复，连续 10 次失败则 is_online() 为 false。
        {
            static uint64_t aht20_next_ms  = 0;
            static uint64_t aht20_deadline = 0;
            static bool     aht20_waiting  = false;

            const uint64_t now_ms    = time_ms64();
            const uint64_t period_ms = g_aht20.is_online() ? 2000u : 10000u;

            if (!aht20_waiting && (now_ms - aht20_next_ms) >= period_ms)
            {
                g_aht20.start_measure();
                aht20_waiting  = true;
                aht20_deadline = now_ms + 90u;
                aht20_next_ms  = now_ms;
            }
            if (aht20_waiting && now_ms >= aht20_deadline)
            {
                float t = 0.0f, h = 0.0f;
                g_aht20.get_measure(t, h); // 内部更新失败计数；连续 RUN_FAIL_LIMIT 次失败即判离线
                if (g_aht20.is_online())
                {
                    for (uint8_t i = 0; i < 4u; i++)
                    {
                        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_temperature = (int8_t)(t + 0.5f);
                        ams[BAMBU_BUS_AMS_NUM].filament[i].compartment_humidity     = (uint8_t)(h + 0.5f);
                    }
                }
                aht20_waiting = false;
            }
        }

        // ===== 软件模拟 AHT20 温湿度（测试用，无硬件时）=====
        // 探测模式下温湿度由 bambu_bus_ams.cpp 在每次响应打印机查询时
        // 调用 sim_aht20_probe_step() 自增；无需在此周期调用。
        // AHT20 实物到位后改用真实传感器，本模拟整体停用。

#ifdef BMCU_OLED
        // ===== OLED 显示（复用 AHT20 软件 I2C 总线）=====
        // 独立于 AHT20 采样临界区：上面采样块已完成 get_measure，此处只读取
        // g_aht20.temperature_c / humidity_percent 并刷新屏幕，不触发 AHT20 测量，
        // 因此不会打断 AHT20 的 I2C 时序。
        // 能力矩阵：
        //   - 屏未就绪（s_ready=false）：每 10s 重探 init()，热插拔屏可自动点亮；
        //     重探前后就绪态翻转时清屏，避免残留旧画面。
        //   - 屏就绪 + 有 AHT20：1s 刷新温湿度画面。
        //   - 屏就绪 + 无 AHT20：显示 "NO AHT20"，不刷温湿度（省 I2C 且语义清晰）。
        {
            static uint64_t oled_next_ms   = 0;
            static uint64_t oled_probe_ms  = 0;   // 重探周期计时
            static bool     oled_was_ready = false;
            const uint64_t  now_ms = time_ms64();

            // 运行期重探：屏未就绪时每 10s 重试 init()，支持热插拔。
            if (!SSD1306_OLED::is_ready() && (now_ms - oled_probe_ms) >= 10000u)
            {
                oled_probe_ms = now_ms;
                SSD1306_OLED::init();   // 复用 AHT20 总线，重发初始化序列并探测 ACK
            }

            // 就绪态翻转：清屏，避免屏刚插上残留上电前的乱码/旧画面。
            if (oled_was_ready != SSD1306_OLED::is_ready())
            {
                oled_was_ready = SSD1306_OLED::is_ready();
                if (SSD1306_OLED::is_ready())
                    SSD1306_OLED::clear();
            }

            if ((now_ms - oled_next_ms) >= 1000u)
            {
                oled_next_ms = now_ms;
                // v4.0 OLED 增强：调 tick() 统一调度多页轮询 + 动作覆盖显示。
                // tick 内部按能力矩阵处理：无 AHT20 时温湿度页显示 NO AHT20；
                // 有动作（notify_action 触发）时优先覆盖显示，否则轮询各页。
                const bool aht20_present = g_aht20.is_online();
                SSD1306_OLED::tick(aht20_present, aht20_present,
                                   g_aht20.temperature_c,
                                   g_aht20.humidity_percent,
                                   comm_ok);
            }
        }
#endif // BMCU_OLED

        Motion_control_run(error);
        RGB_update();
    }
}