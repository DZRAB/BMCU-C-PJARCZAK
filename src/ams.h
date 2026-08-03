#pragma once
#include <stdint.h>
#define ams_max_number 4

enum class _filament_motion : uint8_t
{
    idle            = 0,
    send_out        = 1,
    on_use          = 2,
    before_pull_back= 3,
    pull_back       = 4,
    before_on_use   = 5,   // 09 A5
    stop_on_use     = 6    // 07 00
};

// v4.0-tpu: 由打印机下发的 bambubus_filament_id 解析得到的耗材材质类型。
// 未定义 BMCU_TPU_MODEL 时本枚举照常存在，但仅用于运行时识别/RGB，
// 不参与送料控制（送料控制在编译期由 BMCU_TPU_MODEL 决定）。
enum class _filament_type : uint8_t
{
    unknown = 0,   // 未识别 / 默认（按刚性料 PLA/PETG 行为）
    pla     = 1,   // GFA**
    petg    = 2,   // GFG**
    abs     = 3,   // GFB**
    pa      = 4,   // GFL**
    tpu     = 5,   // GFU**  (软料，v4.0 专用参数针对此类型)
    other   = 6
};

struct _filament
{
    // 耗材参数信息字段
    char bambubus_filament_id[8] = "GFG00";
    uint8_t color_R = 0xFF;
    uint8_t color_G = 0xFF;
    uint8_t color_B = 0xFF;
    uint8_t color_A = 0xFF;
    int16_t temperature_min = 220;
    int16_t temperature_max = 240;
    char name[20] = "PETG";
    uint64_t xhub_unique_id = 0;
    // 耗材参数信息字段结束

    // 耗材烘干器状态字段
    uint8_t dryer_power = 0;      // 烘干器设定功率W
    int8_t dryer_temperature = 0; // 烘干器设定温度℃
    uint16_t dryer_time_left = 0; // 剩余时间min，向上取整
    // 耗材烘干器状态字段结束

    // 耗材状态信息字段
    bool online = true;
    _filament_motion motion = _filament_motion::idle;
    uint8_t seal_status = 0;            // 0:无密封结构 1:已开盖 2:已合盖
    _filament_type filament_type = _filament_type::unknown; // v4.0-tpu: 运行时识别的材质类型
    int8_t compartment_temperature = 22; // 温度:-128℃到127℃
    uint8_t compartment_humidity = 20;   // 湿度：0%到100%

    float meters __attribute__((aligned(4))) = 1.0f;
    float meters_virtual_count __attribute__((aligned(4))) = 0.0f;

    void init()
    {
        xhub_unique_id = 0;
        bambubus_filament_id[0] = 'G';
        bambubus_filament_id[1] = 'F';
        bambubus_filament_id[2] = 'G';
        bambubus_filament_id[3] = '0';
        bambubus_filament_id[4] = '0';
        bambubus_filament_id[5] = '\0';
        color_R = 0xFF;
        color_G = 0xFF;
        color_B = 0xFF;
        color_A = 0xFF;
        temperature_min = 220;
        temperature_max = 240;
        name[0] = 'P';
        name[1] = 'E';
        name[2] = 'T';
        name[3] = 'G';
        name[4] = '\0';
        meters = 1;
        meters_virtual_count = 0;
        online = true;
        motion = _filament_motion::idle;
        filament_type = _filament_type::unknown;
        compartment_temperature = 22;
        compartment_humidity = 20;
    }

} __attribute__((packed, aligned(4)));

static_assert((__builtin_offsetof(_filament, meters) & 3u) == 0u, "meters misaligned");
static_assert((__builtin_offsetof(_filament, meters_virtual_count) & 3u) == 0u, "meters_virtual_count misaligned");


struct _ams
{
    uint8_t ams_type = 0;
    _filament filament[4];
    uint8_t now_filament_num = 0xFF;
    char name[8]; // ams名称
    uint8_t filament_use_flag = 0;
    uint16_t pressure = 0xFFFF; // 送料压力
    bool online = false;
    void init()
    {
        now_filament_num = 0xFF;
        filament_use_flag = 0;
        pressure = 0xFFFF;
        online = false;
        for (uint8_t i = 0; i < 4; i++)
        {
            filament[i].init();
        }
    }
} __attribute__((aligned(4)));

extern _ams ams[ams_max_number];
extern void ams_init();
extern uint8_t bus_now_ams_num;
