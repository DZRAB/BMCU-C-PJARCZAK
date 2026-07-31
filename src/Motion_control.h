#pragma once
#include <stdint.h>
#include <stdbool.h>

void Motion_control_init();
void Motion_control_set_PWM(uint8_t CHx, int PWM);
void Motion_control_run(int error);
bool Motion_control_save_dm_key_none_thresholds(void);

void MC_PULL_detect_channels_inserted();

// 外部变量声明
extern float   MC_PULL_V_OFFSET[4];
extern float   MC_PULL_V_MIN[4];
extern float   MC_PULL_V_MAX[4];
extern uint8_t MC_PULL_pct[4];
extern int8_t  MC_PULL_POLARITY[4];
extern float   MC_DM_KEY_NONE_THRESH[4];
extern bool    filament_channel_inserted[4];

// 由 platformio.ini 的 -DBAMBU_BUS_AMS_NUM 宏定义
#ifndef BAMBU_BUS_AMS_NUM
#define BAMBU_BUS_AMS_NUM 0
#endif

// 由 platformio.ini 的 -DAMS_RETRACT_LEN 宏定义
#ifndef AMS_RETRACT_LEN
#define AMS_RETRACT_LEN 0.2f
#endif

// platformio.ini: -DBMCU_DM_TWO_MICROSWITCH=1 (DM dual microswitch + autoload assist)
#ifndef BMCU_DM_TWO_MICROSWITCH
#define BMCU_DM_TWO_MICROSWITCH 0
#endif

// 自动回抽（仅双微动开关版有效）：用第二个开关 S2 判断料根位置，无需在编译期选择回抽长度。
// 默认随 BMCU_DM_TWO_MICROSWITCH 派生（双开关=1，单开关=0）；可用 -DBMCU_DM_AUTO_RETRACT=0 强制关闭。
#ifndef BMCU_DM_AUTO_RETRACT
#define BMCU_DM_AUTO_RETRACT (BMCU_DM_TWO_MICROSWITCH)
#endif

// platformio.ini: -DBMCU_ONLINE_LED_FILAMENT_RGB=1 (show filament RGB on ONLINE LED when loaded)
#ifndef BMCU_ONLINE_LED_FILAMENT_RGB
#define BMCU_ONLINE_LED_FILAMENT_RGB 0
#endif

#ifndef motion_control_ams_num
#define motion_control_ams_num BAMBU_BUS_AMS_NUM
#endif

#ifndef motion_control_pull_back_distance
#define motion_control_pull_back_distance AMS_RETRACT_LEN
#endif

#if (BAMBU_BUS_AMS_NUM < 0) || (BAMBU_BUS_AMS_NUM > 3)
#error "BAMBU_BUS_AMS_NUM must be in range 0..3"
#endif