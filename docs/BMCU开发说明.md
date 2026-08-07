# BMCU-C 固件开发说明

> 面向开发者的架构、实现逻辑与通讯协议说明。无需逐行读源码即可理解整体设计。
> 源码注释已翻译为中文（见各 `.cpp/.h`；本仓库背景与版本说明见 `README.md`）。

---

## 1. 项目概述

BMCU-C 是 **Bambu Lab AMS（自动多色换料系统）的开源替代固件**，运行在第三方 BMCU 370C 硬件上，使 4 通道 filament 切料/换料器能像原厂 AMS 一样被打印机识别与控制。

- **目标硬件**：BMCU 370C（带霍尔传感器 + AS5600 磁编码器）
- **MCU**：CH32V203C8T6（RISC‑V，144 MHz，WCH）
- **SDK**：WCH 原生 SDK + `noneos`（无操作系统，纯裸机中断驱动）
- **功能**：多色自动换料、filament 装入/回抽/卸载、空通道校准、堵塞与防卡保护、AUTOLOAD 自动装载、filament 元数据与已装入状态掉电持久化、RGB 状态指示

固件通过 **UART（半双工 RS485）** 与打印机通讯，协议分两套：
- **BambuBus**（`0x3D` 帧）：BMCU 直接挂在打印机 AMS 总线上时
- **AHUB**（`0x33` 帧）：BMCU 通过 AMS HUB 串联时

---

## 版本说明

本仓库（BMCU-C）基于原作者 **jarczakpawel** 的 BMCU 固件（基线 **V10.5**）二次开发：

- 固件向打印机上报的版本号保持 `10.50`（原作者 V10.5），以维持打印机兼容性识别。
- 本仓库自身的迭代使用 git 标签管理（如 `v1.0-baseline`、`v2.0-aht20`、`v2.1-aht20`、`v3.0-autoretract`、`v3.1-autoretract`、`v3.2-fix105`），后续版本递增。

其中 `v1.0-baseline` 是基于原作者 V10.5 整理出的可编译、有中文文档的干净基线（修复编译问题、新增编译脚本与说明文档），作为后续开发的起点；`v2.0-aht20` 在其基础上新增 AHT20 温湿度传感器支持（见第 7 章），最新 `v2.1-aht20` 在此基础上新增温湿度探测模式（见第 6 章使用指南）。`v3.0-autoretract` 起引入双开关自动回抽（用 S2 自动判定料根，见第 6.3 节）；`v3.1-autoretract` 修复双开关自动回抽退料后不自动送料（详见第 6.3 节与第 13 章）；**`v3.2.1-fix105` 为当前最新，专修 AHT20 温湿度传感器驱动缺陷**（见第 12 章 AHT20 驱动修复）；`v3.2-fix105` 仅修 bug、不改功能——① 打印机发暂停/停止时 BMCU 若正在送料会立即停机（不再无视指令继续转），② 进料电机控制改为「黄灯三步法」避让（顶满先中力推 2s → 轻压 3s → 超 5s 才报真堵红灯），见第 6.2 节与第 13 章。`v4.0-tpu` 分支在 TPU 软料送料（见第 14 章）之外，**新增 SSD1306 OLED 状态屏支持（`BMCU_OLED`，复用 AHT20 软件 I2C 总线，见第 15 章）**。回退到基线：`git checkout v1.0-baseline`。使用与固件选型见 [`BMCU使用指南.md`](./BMCU使用指南.md)。

> **上报版本名 `AMS08` ↔ `N3F05` 的来龙去脉**：固件向打印机上报的"型号名"字段（`long_packge_version_version_and_name_AMS08[]`，`bambu_bus_ams.cpp`，`0x103` 版本包）在 V2.1 及以前是 `AMS08`，打印机不会拉黑但不显示环境温湿度具体数值；`v3.1-autoretract` 中曾把它改成 `N3F05` 以便 Bambu Studio 显示温湿度数值，但实测运行两次即被打印机拉黑，故 **`v3.2-fix105` 改回 `AMS08`**（上报号仍保持 `10.50`）。如需显示数值的 `N3F05` 方案需另行解决被拉黑问题，目前不采用。

---

## 2. 硬件平台

| 资源 | 用途 |
|---|---|
| USART1 (PA9=TX, PA10=RX, PA12=DE) | 与打印机通讯的半双工 RS485 总线 |
| TIMER / PWM | 4 路电机驱动（每通道一路） |
| ADC + DMA | 采集 buffer 位置电压（空通道检测、DM 开关） |
| 软 I2C（GPIO 模拟开漏） | 4 路 AS5600 磁编码器（测 buffer 轮角度→距离/速度） |
| 软 I2C（独立通道 PB10/PB11，两线开漏） | AHT20 温湿度传感器（环境温湿度；不与 AS5600 共用总线） |
| WS2812 RGB | 1 颗系统灯（SYS_RGB）+ 4 颗通道灯（RGBOUT[0..3]） |
| 内部 Flash 末 4KB 扇区 | NVM 持久化（校准、filament 元数据、状态） |

**电机与传感器布局**（见 `Motion_control.cpp` 引脚定义）：
- AS5600 SCL：PB15/PB14/PB13/PB12
- AS5600 SDA：PD0/PC15/PC14/PC13
- 每通道：1 个送料电机 + 1 个 AS5600 + 1 个 buffer（缓冲轮，带磁体）

---

## 3. 软件架构与代码模块

```
main.cpp              初始化 + 主循环（调度总线/运动/LED）
├─ bambu_bus_ams.cpp  BambuBus 协议：帧解析、命令分发、回复构造
├─ ahub_bus.cpp       AHUB 协议：heartbeat/query/set 从机处理
├─ _bus_hardware.cpp  UART1 + DMA + RS485 方向控制(DE) + 中断收包状态机
├─ crc_bus.c          CRC8 / CRC16 查表实现
├─ Motion_control.cpp 运动控制状态机（换料执行核心）
├─ many_soft_AS5600.cpp 软 I2C AS5600 磁编码器驱动（4 通道并行）
├─ aht20.cpp            AHT20 温湿度传感器驱动（独立软件 I2C 通道 PB10/PB11）
├─ ADC_DMA.cpp        ADC+DMA 采集 buffer 电压
├─ MC_PULL_calibration.cpp  空通道/霍尔极性校准
├─ Flash_saves.cpp    NVM 持久化（页式 + magic + CRC）
├─ ams.cpp/.h         AMS/filament 数据结构与初始化
├─ ws2812.cpp         RGB LED 驱动
├─ Debug_log.cpp      调试串口日志
└─ hal/               中断屏蔽(time_hw/irq_wch) + 时间基准
```

**数据中枢**：全局 `ams[4]`（`ams.h`）保存 4 个 AMS 单元（本机只用 `BAMBU_BUS_AMS_NUM` 那一个）各自的 4 通道 filament 状态。总线层写入 `ams[]`，运动层读取 `ams[]` 驱动电机，二者通过 `ams[].filament[].motion` 这一个字段解耦。

---

## 4. 上电启动与主循环

`src/main.cpp`：

1. `SystemInit` → 时钟 144 MHz → 开中断 → 关看门狗
2. `RGB_init`、DEBUG、AMS、Flash 初始化
3. `ADC_DMA_init` + 等待首帧采样
4. `MC_PULL_calibration_boot`：首次启动（空通道）做空检测校准
5. `ams_datas_read`：从 Flash 恢复 filament 元数据
6. 若 Flash 中记录“上次已装入某通道”，则恢复 `now_filament_num`/`motion=on_use` 等（断电续打）
7. `Motion_control_init` / `bambubus_init` / `bus_init`
8. **主循环 `while(1)`**：
   ```c
   ahubus_run();        // 处理 AHUB 帧
   bambubus_run();      // 处理 BambuBus 帧
   bus_port_to_host.send_package(); // 若有待发数据则 DMA 发出
   Motion_control_run(error);       // 执行运动状态机
   RGB_update();        // 刷新 LED
   ```
   `error` 来自总线：收到心跳→正常；总线超时/CRC 错→`error=-1`（红灯，运动层据此进入安全态）。

---

## 5. 通讯协议（重点）

### 5.1 物理层

`src/_bus_hardware.cpp` 配置 USART1：
- 波特率 **1.25 Mbps**
- 数据位 **9**、停止位 1、**偶校验（Even）**
- 半双工：PA12 = DE（驱动使能），高=发、低=收
- **TX 走 DMA1_Channel4**；**RX 走 RXNE 中断**，每收到 1 字节调用 `bus_port_to_host.irq(data)` 喂给收包状态机

### 5.2 收包状态机（`_bus_port_deal::irq`）

按字节流状态机重组帧，靠首字节区分协议：
- `0x3D` → BambuBus
- `0x33` → AHUB

关键判定（`idx==1` 看 `data & 0x80`）：
- **短帧**：长度在 `buf[2]`，头 CRC8 在 `buf[3]`
- **长帧**：长度在 `buf[4..5]`（16 位），头 CRC8 在 `buf[6]`

帧头 CRC8 校验通过才继续收；收满 `length` 字节后，若主缓冲区空闲则“翻转双缓冲”把包交给主循环（`recv_data_len`、`bus_package_type` 置位）。**基于快照**解析，避免 RX/TX 竞争。

### 5.3 BambuBus 协议（`0x3D`）

#### 短帧（打印机→BMCU 控制命令，flag = `0xC5`）

| 偏移 | 字段 |
|---|---|
| 0 | magic `0x3D` |
| 1 | flag `0xC5` |
| 2 | length |
| 3 | CRC8（覆盖前 3 字节） |
| 4 | **command** |
| 5.. | 数据 |
| n-2,n-1 | CRC16 |

`command` 与处理函数（`bambu_bus_ams.cpp::get_packge_type`）：

| command | 含义 | 处理 |
|---|---|---|
| `0x03` | filament_motion_short | `get_package_motion` → 回复运动短包 |
| `0x04` | filament_motion_long | `get_package_stu_motion` → 回复运动长包（含温湿度） |
| `0x05` | online_detect | `get_package_online_detect` → 在线检测/注册握手 |
| `0x06` | REQx6 | （保留） |
| `0x07` | NFC_detect | （保留） |
| `0x08` | set_filament_info | `get_package_set_filament` → 接收 filament 元数据 |
| `0x20` | heartbeat | 心跳（维持在线，触发 HMS 图标但**不阻断运行**） |

#### 长帧（flag = `0x04`/`0x05`，目标地址=`host_device_type_ams`=0x0700）

长帧头结构（`bambubus_long_packge_data`）：`package_number(2)+package_length(2)+crc8(1)+target(2)+source(2)+type(2)`，后接 `datas[]`，末尾 CRC16。
`type` 字段与处理：

| type | 含义 | 处理 |
|---|---|---|
| `0x21A` | MC_online | `get_package_long_packge_MC_online` |
| `0x211` | read_filament_info | `get_package_long_packge_filament` → 回传 filament 元数据 |
| `0x218` | set_filament_info_type2 | `get_package_set_filament_type2` → 接收元数据(长格式) |
| `0x103` | version | `get_package_long_packge_version` → 回传版本/名称 `AMS08` |
| `0x402` | serial_number | `get_package_long_packge_serial_number` → 回传 SN |

#### BMCU 回复包（BMCU→打印机）

- **运动短包**（`bambubus_ams_motion_package_struct`，`0x3D 0xC0 ...`）：含 `filament_use_flag`、`filament_channel`、剩余 `meters`(float)、`pressure`(uint16)、`filament_stu_flag` 等。
- **运动长包**（`bambubus_ams_stu_motion_package_struct`）：额外含温湿度、在线标志。
- **在线检测回复**：29 字节固定结构，携带版本/SN 前缀。
- 序列号由 **MCU 硬件 UID** 经 FNV‑1a 哈希生成（`bambubus_build_static_serial`），保证每台设备唯一。

### 5.4 AHUB 协议（`0x33`）

用于 BMCU 挂在 AMS HUB 下时，HUB 作为主机轮询。

帧布局（`ahubus_package_query_head` / `set_head`）：

| 偏移 | 字段 |
|---|---|
| 0 | magic `0x33` |
| 1 | flag（`0x80` 主机发出） |
| 2 | length（单位：32 位字数 − 2） |
| 3 | CRC8（覆盖前 3 字节） |
| 4 | command：`0x01` heartbeat / `0x02` query / `0x03` set |
| 5 | query/set type |
| 6 | address（AMS 地址；xMCU 下高 4 位为索引） |
| 7 | data_struct_count |
| 8.. | 数据 |
| 末尾 | **CRC32（硬件 CRC 外设）** |

`command` 与处理（`ahub_bus.cpp::ahubus_run`）：

| command | 含义 | 处理 |
|---|---|---|
| `0x01` heartbeat | HUB 心跳 | `ahubus_slave_get_package_heartbeat` → 回报在线 AMS 列表(EQPT) |
| `0x02` query | 查询 | `ahubus_slave_get_package_query`：ams_name / filament_info / filament_stu / dryer_stu / all_filament_stu |
| `0x03` set | 设置 | `ahubus_slave_get_package_set`：filament_info / dryer_stu / all_filament_stu（同步各 AMS 的 motion 状态） |

AHUB 用 `CRC->DATAR` 硬件 CRC 外设做 32 位校验（`ahubus_package_add_crc`）。

### 5.5 CRC 校验算法

| 协议 | 算法 | 实现 |
|---|---|---|
| BambuBus 头 CRC8 | 查表法，初始 `0x66`（自定义表，非标准 CRC8） | `bus_crc8` |
| BambuBus 帧 CRC16 | 查表法，初始 `0x913D`（自定义表） | `bus_crc16` |
| AHUB CRC32 | CH32 硬件 CRC 外设（CRC‑32/MPEG‑2 风格） | `CRC->DATAR` |

`package_add_crc` 同时填头 CRC8 与尾部 CRC16；接收端 `package_check_crc16` 校验。

---

## 6. 换料命令与状态机

### 6.1 命令映射（`set_motion`，`bambu_bus_ams.cpp`）

打印机下发 `statu_flags` + `motion_flag` 组合，固件映射到 `ams[].filament[].motion`（`_filament_motion`）：

| statu_flags | motion_flag | 目标 motion | 含义 |
|---|---|---|---|
| `0x03` | `0x00` | `send_out` | 送料出（装填中） |
| `0x09` | `0x7F`/`0xA5` | `before_on_use` | 即将使用（预送料） |
| `0x07` | `0x7F` | `on_use` | 使用中 |
| `0x07` | `0x00` | `stop_on_use` | 停止使用 |
| `0x09` | `0x3F` | `before_pull_back` | 准备回抽 |
| 通道=`0xFF` | `0x03`/`0x00` | `pull_back` | 回抽/卸载 |

同时维护 `now_filament_num`（当前通道）、`filament_use_flag`、`pressure`（上报给打印机的送料压力值）、`online` 标志。状态切换有互斥保护（`allow_any`/`allow_stop`），防止多通道冲突。

### 6.2 运动执行（`Motion_control_run`，`Motion_control.cpp`）

每主循环按每个通道的 `motion` 驱动 PWM：
- **on_use**：最小 PWM + 防堵转（anti‑stall，微秒级高 PWM 累积检测）；`g_on_use_jam_latch` 真堵塞→上报 `0xF06F`；`g_on_use_low_latch` 低压力锁定→红灯闪烁。
  - **进料缓冲避让（v3.2-fix105 改进）**：料过五通/到挤出机入口把缓冲头顶满时，原版会一直死命硬推（空转啃料、易误报堵）。因 BMCU 与打印机无「料到哪儿」的通讯，无法靠电机转速区分「已过五通正常送料」与「真堵」，故改**时间配合法**（`pressure_ctrl_on_use`，见 `Motion_control.cpp`）：顶满累计计时 `g_on_use_full_ms[]`，① 0-2s 中力推一把（PWM cap 约600）帮过五通；② 2-5s 减到轻压（cap 约180）只保持不后退、等打印机拉走，此间缓冲头回落到正常带内即清零计时、恢复正常送料（绿灯）；③ ≥5s 仍顶满才算真堵，置 `g_on_use_jam_latch` 报红灯停机。**顶满但 <5s 亮黄灯**（保护避让、非故障），仅真堵亮红灯。
- **pull_back / before_pull_back**：回抽，末端 **线性减速**（`PULL_RAMP_M=15mm` 区），速度从 `PULL_V_FAST=60mm/s` 降到 `PULL_V_END=12mm/s`。
- **idle / send_out / redetect**：按 buffer 位置与有无 filament 决定动作；空闲 10s 后仅在末端动作。
- **AUTOLOAD**（DM 双微动开关板）：触碰第一个开关触发装入，第二个开关（挤出机后）确认完全插入，再送约 120mm；防卡保护：buffer 卡住则回抽重试（最多 3 次）。

### 6.3 双开关自动回抽（`BMCU_DM_AUTO_RETRACT`，v3.0-autoretract 起）

宏 `BMCU_DM_AUTO_RETRACT` 默认随 `BMCU_DM_TWO_MICROSWITCH` 派生（双开关=1 开、单开关=0 关）；即双开关板默认开启自动回抽、单开关板本就关闭。它仅由双开关派生得到，可用 `-DBMCU_DM_AUTO_RETRACT=0` 在编译期强制关闭。

> **关闭 = 固定回抽长度模式**：`BMCU_DM_AUTO_RETRACT=0` 让双开关板放弃 S2 自动判定，走固定回抽长度逻辑——此时 `AMS_RETRACT_LEN`（编译进去的回抽长度）从“安全上限”变回“真实回抽目标长度”，且必须像 `NO_AUTOLOAD` 那样编译全长度矩阵。
> 那它有什么实际用处？单开关板（`NO_AUTOLOAD`）本来就提供固定长度全矩阵，所以**绝大多数情况你不需要关它**；唯一不被冗余覆盖的场景是：**双开关硬件（不能刷 `NO_AUTOLOAD` 固件）却想要固定长度回抽**——例如该机器的 S2 传感不可靠、或你就是偏好某个固定长度。除此之外，它只是无害的代码级开关（用 `#ifndef` 定义、默认干净派生）；v3.0 已暴露该开关：`build_one.sh` 第 6 参数 `AUTO_RETRACT=0`、`build_all_firmwares_fast.py` 环境变量 `AUTO_RETRACT=0` 均可触发，触发后双开关也出 39 档固定长度矩阵（总固件数从默认 30 升到 942，全量合计 1884）。`build_all_firmwares_softload.sh` 则固定只出双开关自动回抽版（不读该开关）。

原理：回抽（`pull_back`）时不再依赖编译期固定的 `AMS_RETRACT_LEN`，而是用第二个微动开关 S2 判定料根位置：

1. 进入回抽即**冻结** `MC_ONLINE_key_stu` 对打印机的上报（避免 S2 跳变误触发打印机逻辑），自检改用**原始开关状态** `dm_key_raw`（不含手势覆盖）。
2. 子状态 `AR_RETRACT_WAIT_S2`：持续退料，直到 `ks` 由“两开关都按（状态 1）”变为“仅 S1 按下（状态 2）”——即 S2 释放，表示料根已退到 S2 处，退料到此为止。
3. 退料完成后由 `dm_ar_finish_pullback()` **主动放行** `dm_autoload_gate`：机构内 S1 永远被 BMG 压着，退料不会回到 `ks==0`，而 `dm_autoload_gate` 只在 `ks==0+idle` 时复位，不清的话 `dm_auto` 的 Stage1（S1_DEBOUNCE）会被永久挡住、导致退完不自动送料。放行后交回原版 `dm_auto` 自动装载流程：压上 S2 变 `ks==1`、自动再送约 12cm 就位。

> **v3.1-autoretract 为什么改（退料后不自动送料的根因）**：`v3.0` 的双开关自动回抽在"退料到 S2 释放"后还保留了一段 `AR_RESEAT_WAIT_S1S2` 回推定位阶段（退到 `ks==0` 再正推 `AR_RESEAT_MAX_M≈5cm` 直到 S2 重新按下）。但 BMCU 370C 机构里 **S1（进料口开关）始终被 BMG 滚轮压着**，`ks` 实际只会从"两开关都按(状态 1)"变到"仅 S1 按下(状态 2)"，**永远不会到 `ks==0`**。结果 `AR_RESEAT_WAIT_S1S2` 阶段永远等不到 `ks==0`，只能靠 200 周期超时兜底才退出，且退出后 `dm_autoload_gate` 未复位（`dm_auto` 的 Stage1 被挡），于是退完不自动送料、需手动干预。v3.1 的改法：① 删掉 `AR_RESEAT_WAIT_S1S2` 阶段与 `AR_RESEAT_MAX_M`/`dm_ar_reseat_*` 相关变量；② 退料判定由"等到 `ks==0`"改为"检测到 `SW2 释放`（`ks==2u`，即仅 S1 按下）即停"（`dm_key_to_state` 阈值也相应重标定：none<`none_thr`、`>1.65`=both、`>1.25`=仅S1(=SW2释放)、其余=仅S2内侧异常）；③ `dm_ar_finish_pullback()` 末尾显式 `dm_autoload_gate[i] = 0u` 放行，并清 `dm_loaded[num]=0u` 交 `dm_auto` 重新送料定位。详见第 13 章。

多重兜底：`AMS_RETRACT_LEN`（编译为 `2.00f`）作为安全上限，若退料超过该距离仍未检测到 S2 释放，自动停电机并进入 `filament_redetect`，绝不卡死。状态机顶部有保护：若因中断等离开回抽状态，立即解冻上报、复位自检相位，防止冻结卡死。

> 单开关板只有 1 个开关，无法用 S2 判定料根，**自动回抽无效**，仍走固定长度逻辑（需按 PTFE 长度编译对应固件）。

**暂停/停止即时响应（v3.2-fix105 改进）**：打印机发来暂停/停止时，若 BMCU 正在 `send_out` 送料（此时 `loaded=0xFF`，原版因 `allow_stop=(loaded==ch)` 而忽略指令、继续转），或正在 DM 自动装载送料，都会**立即退出送料并停机**。`send_out` 场景由 `bambu_bus_ams.cpp` 的 `is_stop_on_use` 放宽 `allow_stop` 限制处理；DM 自动装载送料场景由 `Motion_control_request_stop_dm_autoload()`（`Motion_control.cpp`）经 `g_dm_autoload_stop_req[]` latch 在 DM 状态机开头中止。

---

## 7. 运动控制与传感器

- **AS5600 磁编码器**（`many_soft_AS5600.cpp`）：软 I2C（开漏 50MHz，正确 ACK/NACK/START/STOP），4 通道并行轮询，限速约 1ms/次。输出角度→距离：`kAS5600_MM_PER_CNT = -(π*7.5)/4096`（7.5mm 轮半径）。`updata_stu` 判磁铁在线/强弱，`updata_angle` 算速度。
- **AS5600 健康门控**：连续失败 `kAS5600_FAIL_TRIP=3` 次判离线并隔离该通道；恢复需 `kAS5600_OK_RECOVER=2` 次连续正常，防止失控。
- **ADC_DMA**（`ADC_DMA.cpp`）：并行扫描 ADC1+ADC2，DMA 半满/全满后台滤波，约 5ms 更新一次；用于空通道检测电压、DM 微动开关电压。
- **校准**（`MC_PULL_calibration.cpp`）：首次空通道启动记录每通道“无 filament”检测点（`MC_PULL_V_OFFSET/MIN/MAX`）、霍尔极性（`MC_PULL_POLARITY`）、DM 开关阈值（`MC_DM_KEY_NONE_THRESH`）；按住 buffer 约 5s 可重新校准。
- **AHT20 温湿度传感器**（`aht20.cpp`）：独立软件 I2C 通道（PB10=SCL / PB11=SDA，两线均开漏 `GPIO_Mode_Out_OD`，符合标准 I2C 规范），**不与** 4 路 AS5600 共用总线。写时拉低=低、释放=靠外部上拉拉高，读 SDA 时切输入上拉（高阻），绝不主动输出强高电平，从根源避免主从电平冲突短路。上电 `init()` 发送 `0xBA` 软复位、`0xE1 0x08 0x00` 初始化；`init()` 返回后由 `is_online()` 持久记录握手结果（成功=接好，失败=没接/坏了）。**之后仅在 `is_online()` 为真时**才由 `main.cpp` 主循环非阻塞采样：**每 10 秒**一次（先 `start_measure()` 触发测量，约 90ms 后 `get_measure()` 读取 6 字节温湿度，状态位校验）；单次读取失败会**在本轮窗口内重试最多 3 次**，任一次成功即更新值，连续 3 次失败则本次采样失败、温湿度**沿用上一次成功值**（从未成功则保持 `ams.h` 默认值 22℃/20%），下一轮（10s 后）再试。结果写入 `ams[].filament[].compartment_temperature`（℃）/ `compartment_humidity`（%），由现有 ahub / bambu 协议自动上报打印机。AHT20 不在线时 BMCU 完全不采样、温湿度走默认 22/20，核心送料逻辑零影响（详见第 13.10 节）。

---

## 8. Flash 持久化（`Flash_saves.cpp`）

NVM 位于 Flash 末 **4KB 扇区**（`0x0800F000`，CH32V203C8 结束于 `0x08010000`）：

| 区域 | 地址 | 内容 |
|---|---|---|
| CAL | `+0x100` (1×256B) | 校准：offset/vmin/vmax/polarity[4] |
| MOT | `+0x200` (1×256B) | 运动状态 |
| AMS | `+0x300`+ (4×256B) | 每通道 filament 元数据（颜色/温度/名称/ID） |

- 每页 256B，页头 `NVM256_HDR{magic, ver, len, rsv}`，页尾 CRC（offset 252）。
- 采用 **追加式日志 + skip‑if‑unchanged**，仅当页写满才擦除，显著减少擦除次数、掉电安全（部分写入记录被忽略）。
- 幻数：`FIL1`/`CAL2`/`MOT1`/`STA1`。

---

## 9. 构建变体与宏定义（`platformio.ini`）

通过编译宏区分固件变体（见 `platformio.ini` 大量 `env:ams_a_*` 等）：

| 宏 | 含义 |
|---|---|
| `BAMBU_BUS_AMS_NUM` (0..3) | 本机在 AMS 链中的编号（AMS_A..D）；决定回复的 AMS 地址与 SN 后缀 |
| `AMS_RETRACT_LEN` (米) | filament 回抽长度（从 AMS 分线器末端起算）；SOLO 固定 0.095，AMS_A-D **支持 0.10-2.00 米（步长 5cm，共 39 档）**，可由编译参数/环境变量自定义 |
| `BMCU_DM_TWO_MICROSWITCH` | DM 双微动开关板 + AUTOLOAD 辅助 |
| `BMCU_DM_AUTO_RETRACT` | 自动回抽（默认随 `BMCU_DM_TWO_MICROSWITCH` 派生：双开关=1 开、单开关=0 关）；双开关用 S2 判定料根、无需固定回抽长度；`-D` 置 0 可强制关闭，双开关改用固定回抽长度（须编译全长度矩阵） |
| `BMCU_P1S` | P1/P1S/X1 打印机适配（更长 PTFE 路径） |
| `BMCU_SOFT_LOAD` | soft_load(A1)：更低装入力（弱弹簧单元） |
| `BMCU_ONLINE_LED_FILAMENT_RGB` | 装入时 ONLINE LED 显示 filament RGB 颜色 |

`env:fw` 通过环境变量注入上述宏；`env:moj` 为开发者默认（单 BMCU/SOLO，AMS 总线编号 0，回抽 0.095m）。

---

## 10. 关键常量速查

| 常量 | 值 | 含义 |
|---|---|---|
| `PULL_V_FAST` | 60 mm/s | 回抽起始速度 |
| `PULL_V_END` | 12 mm/s | 末端速度 |
| `PULL_RAMP_M` | 0.015 m | 末端线性减速区(15mm) |
| `PULL_PWM_MIN` | 400 | 回抽最小 PWM（“顶推”） |
| `kAS5600_MM_PER_CNT` | −(π·7.5)/4096 | 角度→距离换算 |
| `kAS5600_FAIL_TRIP` / `OK_RECOVER` | 3 / 2 | 传感器离线判定/恢复 |
| 总线波特率 | 1.25 Mbps, 9E1 | USART1 配置 |
| `host_device_type_ams` | `0x0700` | BambuBus 目标地址 |

---

## 11. 调试与排错提示

- 系统灯（SYS_RGB，主板上单独一颗，非通道灯）：只显示 BMCU 与打印机的通讯心跳——正常时浅灰白（`0x38,0x35,0x32`）；有 AHT20 时每 3 秒闪一下（约 150ms，防灯珠烤温）；总线错误时红色（`0x10,0,0`）常亮。AHT20 事件**不在系统灯上显示**。详见第 13.10 节与 `docs/rgb_led_meaning.md` 第 1 节。
- 打印机启动会报 **HMS 警告**（来自 `0x20` 心跳握手），属已知可接受行为，不阻断打印。
- 首次刷写必须**所有通道为空**；否则取出 filament 后按住 buffer 约 5s 重新校准。

---


## 12. AHT20 驱动修复细节（v3.2.1-fix105）

### 12.1 问题背景

早期 AHT20 驱动（v2.0/v2.1 引入）在实机上**读不到温湿度**：上电自检只能亮红灯，打印机长期显示默认 22℃/20%。本版（v3.2.1-fix105）在不改动 3.2 核心功能代码的前提下，仅重构 `src/aht20/` 与 `src/main.cpp` 自检逻辑，彻底修复。

### 12.2 根因：旧驱动发了官方手册不存在的指令

AHT20 官方手册的测量流程只有两步：

1. VDD 上电后等 5ms，发写测量命令 `0x70 0xAC 0x33 0x00`，等待 80ms 测量完成；
2. 发 `0x71` 读传感器，取状态字 + SRH[19:0] + ST[19:0] + CRC。

旧驱动在 `init()` 里额外发了 `0xBA`（软复位）、`0xE1 0x08 0x00`（初始化）这类**手册上不存在的命令**，给刚上电的 AHT20 发未知命令会使其进入异常状态，导致后续 `0xAC` 测量一直不应答/忙，自检必失败。本版**彻底移除**这些指令，`sensor_init()` 直接返回 `true`（上电即就绪），存在性交由 `read_blocking()` 发 `0xAC` 的 ACK 判定。

### 12.3 配套时序修复（软件 I2C 底层）

逐位对比验证过跑通的 `CH32V203C8T` 测试程序（`AS2/i2c_ch32.c`），修正本仓库手写软件 I2C：

- **开漏全程**：SDA/SCL 均 `GPIO_Mode_Out_OD`，读 SDA 也保持开漏、释放后靠外部 4.7k 上拉读 `IDR`，**绝不切输入模式**（CH32V203 动态改 CNF/MODE 不可靠，是另一潜在不稳定源）。
- **建立时间**：写位先设定 SDA 电平 → `iic_delay()` → 拉高 SCL 采样，满足 100kHz 时序。
- **SDA 释放**：读字节结束时补 `sda_release()`，让总线回到 idle 高电平，避免下一 START 采样到错误电平。
- **bit 顺序**：先 `b<<=1` 再采样，与验证版一致。
- **延时基准**：`g_iic_delay_ticks = 5 * time_hw_ticks_per_us()`（SysTick 源 HCLK/8 ≈18 ticks/us → 5us = 100kHz）已在 `init()` 正确计算。

### 12.4 上电自检逻辑（不阻塞主程序）

`src/main.cpp` 在 `MC_PULL_calibration_boot()`（首次开机校准）**之前**插入自检块：

```cpp
g_aht20.init();
{
    float t = 0.0f, h = 0.0f;
    if (g_aht20.read_blocking(t, h)) {      // 成功读到一次温湿度
        SYS_RGB.set_RGB(0x00,0x00,0x10,0);  // 蓝灯：检测到 AHT20 存在
        RGB_update(); delay(200);
        SYS_RGB.set_RGB(0x00,0x10,0x00,0);  // 绿灯：成功读到温湿度
        RGB_update(); delay(200);
    }
    SYS_RGB.set_RGB(0x00,0x00,0x00,0);      // 读失败=不亮灯、不阻塞
    RGB_update();
}
```

- 每次上电都先自检，亮蓝→绿即证明 AHT20 在线且可读；读失败不亮灯、直接进主程序（含校准），**绝不影响换料/通讯**。
- 自检成功仅代表上电那一刻读到一次；之后每 2 秒由主循环非阻塞采样（`start_measure` → 90ms 后 `get_measure`）持续刷新 `ams[].filament[].compartment_temperature/humidity`，由现有 ahub/bambu 协议自动上报。
- 自检灯是**系统灯（SYS_RGB）**短暂闪一下，与 4 颗通道灯的状态指示互不冲突。

### 12.5 上电指示灯时序对照（实机现象）

| 阶段 | 灯序 | 含义 |
|------|------|------|
| 开机固定 | 红（短） | 正常启动红灯 |
| AHT20 自检 | 蓝 → 绿（短） | 检测到并读到温湿度 |
| 校准/初始化 | 灭（首次久、二次快） | 首次开机校准耗时较长 |
| 主机通讯就绪 | 浅灰 / 依总线状态 | 进入主循环，由 ahub/bambu 协议驱动 |

> 自检蓝→绿亮过即说明驱动修复成功；之后若常亮红灯，是 BMCU 与打印机握手 `error` 分支（与 AHT20 无关，属原有通讯逻辑）。

### 12.6 AHT20 在线判定怎么做的（v3.2.1）

AHT20 的状态字（读 `0x71` 返回的第 1 字节）里**没有厂商 ID 位**，没法像常见传感器那样"读 ID 确认型号"，所以判断"板子上有没有焊 AHT20"得用别的办法。3.2.1 这套判定是这样设计的：

**上电自检（决定开机那一刻有没有 AHT20）**
- `read_blocking()` 发 `PROBE_ATTEMPTS=5` 次标准测量命令，每次量完都做下面三道校验，任一次通过就认在线（`online_=true`），5 次全败认无传感器（`online_=false`）：
  1. **校准使能位（bit[3]）**：AHT20 上电后这一位硬件自动置 1，软件写不了也清不掉，是最可靠的"芯片真存在"指纹。没这位基本就是噪声/悬空总线。
  2. **物理范围**：湿度必须在 `0~100%`、温度必须在 `-40~85℃`，越界就是垃圾数据。
  3. **忙标志（bit[7]）**：测量还没完成时为 1，这种读数作废。

**运行期监测（工作中传感器坏没坏）**
- 主循环采样时每次失败 `run_fail_cnt_++`（封顶 `RUN_FAIL_LIMIT=10`），连续 10 次失败就当作传感器掉了，`is_online()` 返回 `false`，固件切回"无 AHT20"逻辑（温湿度走默认 22/20、停止采样）；中间只要成功一次计数就清零。

**两个维度合起来**：`is_online()` = `online_ && (run_fail_cnt_ < 10)`——开机有且工作中没坏，才算在线。

**代码位置**：`src/aht20/aht20.h`（`STATUS_BUSY_MASK` / `STATUS_CAL_ENABLE_MASK` / `RUN_FAIL_LIMIT` / `PROBE_ATTEMPTS` / `online_` / `run_fail_cnt_` / `is_online()`）；`src/aht20/aht20.cpp`（`get_measure` 三道校验、`read_blocking` 5 次探测）。

### 12.7 SYS_RGB 系统灯逻辑（v3.2.1）

AHT20 在板子上离 SYS_RGB 灯珠很近，灯一直亮白光会发热，把 AHT20 的读数烤高。所以 3.2.1 把系统灯改成下面这样（`src/main.cpp` 主循环心跳灯状态机）：

- **有 AHT20**：不常亮，改成每 3 秒闪一下白光（约 150ms，亮度 `0x38,0x35,0x32`），其余时间灭——既能看到在线、又不持续烤温。
- **无 AHT20**：白色常亮（没传感器可烤，保持原样）。
- **通讯异常**（收到打印机 `error` 包）：红色常亮 `0x10,0,0`。
- 闪烁周期（3 秒）和温湿度采样周期（在线 2 秒 / 离线 10 秒）互不干扰，灯靠 `time_ms64()` 自己计时，不阻塞。

---

> 文档基于源码静态分析整理，覆盖功能、实现逻辑与通讯协议主干。具体字节级帧样例可参考 `bambu_bus_ams.cpp` 中 `// 3D C5 ...` 形式的抓包注释。

---

## 13. 版本迭代开发记录（v3.0 → v3.2-fix105）

> 本章集中记录 3.0/3.1/3.2 三个标签的**改动动机、根因分析、具体代码改动点**，供回溯。代码行号/符号以各标签对应提交为准；功能面描述见 README「版本说明」。

### 13.1 v3.0-autoretract：双开关自动回抽（S2 判定料根）

- **新增能力**：双微动开关板（S1=进料口、S2=挤出机后）在回抽时不再依赖编译期固定的 `AMS_RETRACT_LEN`，改用 **S2 释放** 自动判定料根位置，自动决定回抽距离。
- **关键宏**：`BMCU_DM_AUTO_RETRACT`（默认随 `BMCU_DM_TWO_MICROSWITCH` 派生；双开关=1、单开关=0）。关闭则双开关也走固定回抽长度（需编译全长度矩阵），见第 6.3 节说明。
- **代码位置**：`Motion_control.cpp` 中 `dm_autoretract_phase_enum`、`dm_ar_phase[]`、`dm_ar_finish_pullback` lambda；自检用 `dm_key_raw[]`（原始开关状态，不含手势覆盖）、`dm_ar_freeze_report[]`（冻结对打印机上报）。
- **原理**：退料期间冻结 MC_ONLINE 上报避免 S2 跳变误触发打印机；以 `ks`（由 `dm_key_to_state()` 解码）判料根；`AMS_RETRACT_LEN(=2.00f)` 退化为安全上限兜底。

### 13.2 v3.1-autoretract：修复双开关自动回抽退料后不自动送料

#### 13.2.1 现象
v3.0 双开关自动回抽退完料后，不会自动向前送料就位，需用户手动干预。

#### 13.2.2 根因（读代码确认）
BMCU 370C 机构中 **S1（进料口微动开关）永远被 BMG 滚轮压住**，`ks` 的物理真实取值只能是：
- `1` = 两开关都按（both，S1+S2，≈1.8V）
- `2` = 仅 S1 按下（≈1.5V，即 S2 已释放）
- `3` = 仅 S2 内侧（≈1.0V，异常/进料中）
- **永远到不了 `0`（none）**

v3.0 的回抽状态机有两段：
1. `AR_RETRACT_WAIT_S2`：退料，等到 `ks==0` 才算完全退出；
2. `AR_RESEAT_WAIT_S1S2`：再正推 `AR_RESEAT_MAX_M≈5cm`，直到 S2 重新按下（`ks==1`）或超时 200 周期。

由于 `ks` 到不了 `0`，第 1 段靠 `d >= safety_max` 兜底退出，第 2 段则靠 200 周期超时退出；退出后 `dm_autoload_gate` 仍保持置位（`dm_auto` 的 Stage1 `S1_DEBOUNCE` 仅在 `ks==0 + idle` 时复位），导致 `dm_auto` 自动装载流程被永久挡住 → **退完不自动送料**。

#### 13.2.3 改动清单（`Motion_control.cpp` + `bambu_bus_ams.cpp`）
- 删除 `AR_RESEAT_WAIT_S1S2` 阶段及相关变量 `AR_RESEAT_MAX_M`、`dm_ar_reseat_start_m[]`、`dm_ar_reseat_cycles[]`；枚举仅保留 `AR_IDLE`、`AR_RETRACT_WAIT_S2`。
- 退料判定改为 **SW2 释放即停**：`const bool sw2_released = (ks == 2u);` 命中即 `dm_ar_finish_pullback()`；删除等待 `ks==0` 分支。
- `dm_key_to_state()` 阈值重标定（与实际机构电压对应）：
  - `v < none_thr` → `0`（none）
  - `v > 1.65` → `1`（both）
  - `v > 1.25` → `2`（仅 S1，即 SW2 释放）★ 退料判定点
  - 其余 → `3`（仅 S2 内侧，异常）
- `dm_ar_finish_pullback()` 末尾新增：`dm_autoload_gate[i] = 0u;`（主动放行，否则 `dm_auto` Stage1 被永久挡）→ 交回 `dm_auto` 自动装载流程（压上 S2 变 `ks==1`，再送约 12cm 就位）。
- `motor_motion_switch()` 退料分支新增：`dm_loaded[num] = 0u;`（退料后清空装载标志，交 `dm_auto` 重新送料定位）。
- 注释同步更新（原注释写的"退到 S2 释放再正推至 S2 再按下定位"与机构事实不符，已改为"退到 SW2 释放即停，然后交 dm_auto 送 12cm"）。
- `bambu_bus_ams.cpp` 附属改动：`long_packge_version_version_and_name_AMS08[]` 的型号名字段由 `AMS08`（0x41,0x4D,0x53,0x30,0x38）改为 `N3F05`（0x4E,0x33,0x46,0x30,0x35）——为让 Bambu Studio 显示温湿度数值（此改动在 v3.2 被回退，见 13.3「上报版本名回退」）。

#### 13.2.4 验证点
- 双开关板退料后无需手动干预即自动送料就位；
- 单开关板（`BMCU_DM_AUTO_RETRACT=0`）走固定长度，不受影响；
- `AMS_RETRACT_LEN` 仍作为安全上限兜底防卡死。

### 13.3 v3.2-fix105：仅修 bug（不丢 3.1 功能）

> 分支 `bugfix/v3.2-fix105`，基于 `v3.1-autoretract`。**仅修两个遗留 bug，功能/上报号 `10.50` 完全不变**。具体功能面说明见 README。

#### 13.3.1 Bug1：手动暂停 / 停止时 BMCU 不停

**现象**：打印机发来暂停或停止指令时，若 BMCU 正处于"自己送料"状态（自动回抽后的 `send_out` 送料，或 DM 自动装载 `dm_auto` 送料），会无视停止指令继续转动，直到自己送完才停。

**根因（读代码确认）**：
- `bambu_bus_ams.cpp` 的 `set_motion()` 中 `is_stop_on_use` 分支原逻辑：
  ```cpp
  if (!allow_stop) return true;   // 直接忽略暂停
  ```
  其中 `allow_stop = (ams_ptr->filament[ch].loaded == ch)`。`send_out`（自动回抽后正在送料）时 `loaded == 0xFF`，`allow_stop` 为 `false`，于是暂停指令被 `return true` 直接吞掉，BMCU 继续转。
- DM 自动装载（`dm_auto` 状态机）的 `forward` 分支根本不在 `set_motion()` 的 `motion` 切换覆盖范围内，`stop_on_use` 覆盖不到它，故也在继续转。

**改动清单**：
- `src/bambu_bus_ams.cpp`
  - 顶部新增 `#include "motion_control.h"`。
  - `is_stop_on_use` 分支：在 `if (!allow_stop) return true;` 之前先调用 `Motion_control_request_stop_dm_autoload(ch);`（请求中止正在进行的 DM 自动装载送料）。
  - 放宽 `allow_stop` 限制：`if (!allow_stop && (prev != _filament_motion::send_out)) return true;` —— 仅当**不是 `send_out`** 且 `!allow_stop` 时才忽略；`send_out` 状态也被纳入可停止集合（`prev == send_out` → 置 `stop_on_use`）。
- `src/Motion_control.h`：新增声明 `void Motion_control_request_stop_dm_autoload(uint8_t ch);`
- `src/Motion_control.cpp`：
  - 新增静态 latch `g_dm_autoload_stop_req[4]` 与 setter `Motion_control_request_stop_dm_autoload()`。
  - 在 `dm_auto` 状态机 `forward` 分支开头插入：若 `g_dm_autoload_stop_req[CHx]` 置位，则清 PID、复位 `dm_auto_state=DM_AUTO_IDLE`、`dm_autoload_active=false`、`dm_autoload_gate=0`、电机 PWM=0，并 `return` 直接停机（原误用的 `goto run_end` 标签不存在，已改为 `return`）。

**效果**：打印机一发暂停/停止（或通道掉线），BMCU 若在 `send_out` 或 `dm_auto` 送料，立即退出并停机；正常 `on_use`/`before_on_use` 的停止逻辑保持不变。

#### 13.3.2 Bug2：进料电机缓冲顶满误报堵料 → 黄灯三步法

**现象**：料过五通（4 管汇 1）或到挤出机入口时，缓冲头被顶到"满"位。原版此时一直用大力死命硬推（PWM cap 最高 950），要么空转啃料、要么误报"堵料"红灯停机；真堵了又可能判断不及时。

**根因**：BMCU 与打印机之间**没有"料到哪儿了"的通讯**，无法靠电机转速/位置区分"已过五通正常送料"与"真卡死"。原版用 `retrig=55.0` 的 `pct` 二次放大推力（最高 950）去"硬闯"，在过五通的正常阻力下极易误触发堵料红灯。

**改动清单**（`src/Motion_control.cpp`，`pressure_ctrl_on_use` 顶满分支）：
- 新增静态计时器 `g_on_use_full_ms[4]`（缓冲顶满持续毫秒）。
- 正常带内（回落）分支新增 `g_on_use_full_ms[CHx] = 0u;` —— 缓冲头回落即清零计时、恢复绿灯正常送料。
- 顶满（`pct` 高于目标带）分支改为**时间配合三步法**：
  1. 累计 `g_on_use_full_ms[CHx] += time_E*1000`；
  2. `t_full >= 5000`：**判定真堵** → 置 `g_on_use_jam_latch[CHx]=1`、上报 `0xF06F`、红灯、`Motion_control_set_PWM(CHx,0)`、`return` 停机（与原堵料红灯逻辑一致）；
  3. `<5s`：**亮黄灯**（0xFF,0xFF,0x00，保护避让、非故障）；按时间分两段推力：
     - `t_full < 2000`：`lim_f = 600`（中力推一把，协助顶过五通/送进挤出机）；
     - `t_full >= 2000`：`lim_f = 180`（轻压保持，只防后退，等打印机把料拉走）；
  4. 删除原 `retrig` 二次放大段（原 `mul` 最高 3×、cap 950 的逻辑）。
- 原理：绝大多数"过五通/正常送料"的顶满会在 2-5s 内随打印机拉料而回落 → 自动恢复绿灯；只有**超过 5s 仍顶满**才确认真堵报红灯，误报率大幅下降。

**效果对照**：送料遇五通阻力、缓冲头顶满时亮**黄灯**（保护避让）；仅真堵亮**红灯**。LED 语义见 `docs/rgb_led_meaning.md`。

#### 13.3.3 上报版本名回退（AMS08）
v3.2 将 `bambu_bus_ams.cpp` 中 `long_packge_version_version_and_name_AMS08[]` 的型号名字段由 v3.1 的 `N3F05`（0x4E,0x33,0x46,0x30,0x35）**改回 `AMS08`**（0x41,0x4D,0x53,0x30,0x38）。原因：`N3F05` 虽能让 Studio 显示温湿度数值，但实测运行两次即被打印机拉黑；`AMS08` 不显示具体数值但稳定不被拉黑。上报号仍保持 `10.50`。如需 `N3F05` 显示数值方案，需另行解决被拉黑问题（不在本版本范围）。

#### 13.3.4 编译/验证
- 三种型号实编通过：双开关 DM=1（Flash ≈84.9%）、单开关 DM=0（≈80.2%）、双开关固定长度。
- `build_one.sh` 在 v3.2 修复了单开关 `AUTOLOAD=0` 时 `RETRACT` 被误覆盖为 `2.00_auto` 的脚本 bug（现单开关保留用户指定回抽长度）。

---

> 文档基于源码静态分析整理，覆盖功能、实现逻辑与通讯协议主干。具体字节级帧样例可参考 `bambu_bus_ams.cpp` 中 `// 3D C5 ...` 形式的抓包注释。

---

## 14. v4.0-tpu 开发说明（TPU 软料送料）

> 本章记录 `dev/v4.0-tpu` 分支针对 TPU 软料送料所做的开发细节。用户视角见
> [`docs_release/v4.0-tpu发布说明.md`](./docs_release/v4.0-tpu发布说明.md)。

### 14.1 背景与设计动机

BMCU 的 on_use 送料闭环（见第 6 章）原本假设料是「刚性、低摩擦、可压缩性小」的（PLA/PETG 接近）。
对 TPU 这类**高弹性、高摩擦、易堆料**的软料，原闭环会出问题：

- 缓冲头压力判定失真（弹性料顶不进挤出机却显示「顶满」）→ 误报堵料红灯；
- 推力过大啃料/堆料，过小送不动；
- 固定长度回抽后 TPU 回弹，实际送料量不足。

**核心决策：单一通用固件 + 编译期 4 通道写死型号表**

所有固件都**无条件内置 TPU 逻辑**（参数表与查表函数不再用 `#ifdef` 隔离），没有"通用/专用"双轨之分。TPU 优化只在某通道被设为 TPU 时触发，其它通道零差异。

1. **TPU 逻辑何时触发**
   - 装料后打印机会把**当前通道材料型号**下发给 BMCU；BMCU 读到某通道 `filament_type == tpu`（即打印机允许设置的 TPU for AMS `GFU98`）后，切换成 TPU 软料推力参数，其他通道（PLA/PETG…）仍走原 v3.2 刚性推力。
   - 打印过程不改材料，直到更换/重设材料才更新该通道参数。
   - 任何通道都不设 TPU 时，固件行为 = 原版 v3.2，**零差异**（参数表虽编入固件，但未被任何通道触发）。

2. **写死型号表解决「打印机只能设 GFU98」**
   - Bambu 打印机只允许设置 TPU for AMS（`GFU98`），其它 TPU 型号（95A/90A/85A 等）被拒，BMCU 收不到对应 `filament_id`，TPU 参数不生效。
   - 编译期用 4 个宏 `BMCU_TPU_FIX0~3` 给槽 0~3 **分别写死一个型号**（如 `SLOT0=GFU98 SLOT1=GFU90 SLOT2=GFU95 SLOT3=GFU85`），不指定时保底全写 `GFU85`（最软最稳）。
   - 运行时：某通道被设为 `GFU98` 时，固件**忽略下发的 `GFU98`，内部用该通道写死型号跑软料参数**；回传仍是 `GFU98`，不骗打印机，循环不触发（本方案核心优点）。
   - 非 TPU（PLA/PETG/ABS/PA/other）走刚性逻辑，**写死表不参与**。

最常用场景：**一个通道放 TPU、其他通道放 PLA 做支撑**（如 TPU 壳体 + PLA 支撑）。
一块固件通吃，哪个槽放 TPU 哪个槽自动优化；想用非 GFU98 型号时靠编译写死表绕开打印机限制。

### 14.2 送料逻辑详解：不同耗材各自怎么推（重点）

**核心结论：不是两套算法，是同一套 on_use 闭环，按通道加载不同参数集。**

BMCU 在 `Motion_control::run(CHx)` 里**逐通道独立**处理。每个通道在 on_use 送料时，
先决定该通道用哪一套参数（`tpu_p` 指针）：

- **PLA / PETG / ABS / PA 等刚性料**（非 TPU）：`tpu_p == nullptr`，全部走原 v3.2 常量
  （目标压力带 `MC_ON_USE_TARGET_PCT` 约52-65%、PWM 上限 `MC_LOAD_S2_PWM_HI` 约480-550、
  `MC_LOAD_S2_PWM_LO=1000`、三段时间窗避让 `jam_ms=5000`）。
- **TPU 通道**（运行时 `filament[CHx].filament_type == tpu`，即打印机设为 `GFU98`）：`tpu_p = tpu_param_fixed(CHx)`
  按**该通道写死型号** `TPU_FIXED_ID[CHx]` 加载软料参数（如槽 1 写死 GFU90：目标带降到 `40-50%`、PWM 上限降到 `360/850`、
  三段式避让 `phase1=3000 / phase2=5000 / jam=8000`、回抽多退 `0.03m`）。下发的 `GFU98` 被忽略，内部用写死型号跑。
- **非 TPU 通道**（PLA/PETG/ABS/PA/other）：`tpu_p == nullptr`，走原 v3.2 刚性常量，写死表不参与。

**同一时刻多通道举例**（A 槽 TPU + B 槽 PLA 做支撑）：
BMCU 在 `run(A)` 时 A 通道加载软料参数（降推力、宽避让、多退料），
在 `run(B)` 时 B 通道加载刚性参数（原 v3.2 行为），二者互不干扰、各自独立闭环。

闭环本身（压力目标带 PID、缓冲顶满判定、黄灯三步法避让、堵料红灯）**结构不变**，
v4.0 只把"推力/时间窗/避让力度/回抽长度"这些**数值**按通道替换为对应料的参数集。
详见 13.4（三段式避让）、13.5（参数表）、13.6（数据流）。

**常态送料推力 vs 顶满避让（两类逻辑要分清）**

**表 A：常态送料推力（缓冲头在目标带内 / 低于目标带，占 99% 时间，走 PID 压力闭环）**

| 料（型号） | 类型 | 目标带 target/band_hi | PWM 上限 feed_pwm_hi（推一把） | PWM 上限 feed_pwm_lo（持续） | 相对 PLA 推力 |
|---|---|---|---|---|---|
| PLA / PETG / ABS / PA | 刚性 | 52-65% | 900（原常量） | 380（原常量） | 基准 |
| GFU98（68D） | TPU | 50/58 | 440 | 950 | 略降 |
| GFU02（约68D） | TPU | 50/58 | 440 | 950 | 略降 |
| GFU00 / GFU95（95A） | TPU | 45/54 | 400 | 900 | 降约 10-15% |
| GFU90（90A） | TPU | 40/50 | 360 | 850 | 降约 20-25% |
| GFU85（85A） | TPU | 35/46 | 320 | 800 | 降约 30-40% |

> 越软 → 目标带越低（避免硬顶压缩而非前进）、PWM 上限越小（防过推啃料）。**常态推力直接降级，不靠三段式。**

**表 B：顶满避让（缓冲头被顶到 band_hi 以上才触发，兜底逻辑，见 13.5 三段式）**

| 料（型号） | 第1段力度 phase1_lim（0-phase1_ms） | 第2段力度 phase2_lim（phase1-+phase2） | 第3段力度 phase2_lim×0.5（>+phase2） | 真堵阈值 jam_ms | 窗口趋势 |
|---|---|---|---|---|---|
| PLA / PETG（刚性） | 600 | 180 | 90 | 5000 | v3.2 原值 |
| GFU98（68D） | 520 | 160 | 80 | 6000 | 略小/略宽 |
| GFU02（约68D） | 520 | 160 | 80 | 6000 | 略小/略宽 |
| GFU00 / GFU95（95A） | 420 | 120 | 60 | 7000 | 更小/更宽 |
| GFU90（90A） | 360 | 100 | 50 | 8000 | 更保守 |
| GFU85（85A） | 300 | 80 | 40 | 9000 | 最保守/最长 |

> 越软 → 避让力度越小（防啃料）、`jam_ms` 越长（软料弹性大、易"假顶满"，给更长容忍窗口才报真堵）。

**两类逻辑的关系**
- **常态送料（表 A）**：PID 闭环，推力由 `feed_pwm_hi/lo` 封顶，决定"平时怎么推"，占绝大多数时间。
- **顶满避让（表 B）**：只在料卡五通/进挤出机、缓冲头被持续顶满时接管，决定"卡住了怎么温柔退避"。
- 两者都按 `tpu_p` 指针选参数集；非 TPU 走 v3.2 原常量，互不影响。

**为什么软料（如 TPU85）不会"顶不满就出问题"**
TPU 软、弹性大，缓冲头确实比硬料更难被硬顶满；但这不意味着不需要降推力——软料高摩擦、易堆料，
若仍用 PLA 的 900 上限去推会过推啃料。所以常态闭环 PWM 上限被参数表调小（GFU85: 320/800），
温和推进不暴冲（见表 A）。另一方面，软料弹性会让缓冲头**轻微波动式顶到带上限又回落**，
易被误判堵，故 `jam_ms` 放宽到 9000（PLA 5000，见表 B）给更长容忍窗口。即：软料"顶不满"
由常态降级推力解决，"假顶满"由放宽 jam 阈值解决，两者都不依赖三段式硬推。

### 14.3 分支与约束（实现前提）

| 项 | 约束 |
|---|---|
| `main` | 保持与原作者镜像一致，不动 |
| `dev/grid` | 二次开发主分支 |
| `dev/v4.0-tpu` | 本次 TPU 开发分支 |
| `version` 文件 | 保持 `10.50.00.00`，不动（版本靠 git 标签区分） |
| `platformio.ini` | 原版脚本，**绝对不动**；所有变体通过构建脚本注入宏实现 |

### 14.4 改动清单

**新增 `src/tpu_params.h`（TPU 送料参数表）**
- `_tpu_model` 枚举：按 Bambu filament_id 前缀/型号对应（GFU98/GFU00/GFU02/GFU95/GFU90/GFU85）。

**TPU 型号 RGB 识别色（宏 `BMCU_ONLINE_LED_FILAMENT_RGB` 关闭时显示，用于验证识别）**

| filament_id | 型号 | Shore | RGB（r,g,b） | 颜色 |
|---|---|---|---|---|
| GFU98 | TPU for AMS | 68D | 0x00,0x20,0x20 | 青 |
| GFU00 | TPU 95A HF | 95A | 0x00,0x20,0x00 | 绿 |
| GFU02 | Generic TPU | 约68D | 0x18,0x00,0x20 | 紫 |
| GFU95 | TPU 95A | 95A | 0x20,0x18,0x00 | 黄 |
| GFU90 | TPU 90A | 90A | 0x20,0x0A,0x00 | 橙 |
| GFU85 | TPU 85A | 85A | 0x20,0x00,0x00 | 红 |
| 非 TPU（PLA/PETG/ABS/PA/未知/other） | — | — | 0x08,0x08,0x10 | 白偏蓝（统一色） |

> 宏开启时，上述颜色不参与，LED 仍显示打印机下发的真实耗材色（原有功能不变）。
- `_tpu_param` 结构体：每个型号的完整送料参数。
- `TPU_PARAMS[]`：参数表（按硬度分级，初值待实测校准）。每个型号额外带一组 `rgb_r/g/b` 识别纯色
  （见下表，亮度与普通状态色同档，不刺眼）。
- `tpu_param_lookup(const char *filament_id)`：按 filament_id 前 4 字符查表；
  找不到返回最软项（TPU_85A），保证「未知 TPU 也走最保守参数」。
- `tpu_param_fixed(uint8_t ch)`：**按通道写死型号查表**，返回 `TPU_FIXED_ID[ch]` 对应的参数指针（供 TPU 通道调用）。
- `tpu_model_rgb(_tpu_model, r, g, b)`：查表返回某型号的 RGB 识别色（供 RGB 模块调用，显示**写死的真实型号色**）。
- `TPU_NON_TPU_RGB_R/G/B`：宏关闭时**非 TPU** 材质（PLA/PETG/ABS/PA/未知/other）的统一显示色（白偏蓝）。
- `TPU_FIXED_ID[4]`：编译期 4 通道写死型号表，由 `BMCU_TPU_FIX0~3` 宏注入（未定义时默认全 `GFU85`）。
- 关键改动：参数表与 `tpu_param_lookup()` **无条件编译进固件**（不再用 `#ifdef BMCU_TPU_MODEL` 隔离），
  使每个固件都内置 TPU 逻辑；运行时 TPU 通道改用 `tpu_param_fixed(CHx)` 走写死型号。

**修改 `src/ams.h`**
- 新增 `_filament_type` 枚举：`unknown / pla / petg / abs / pa / tpu / other`。
- `_filament` 结构体新增 `filament_type` 字段（uint8_t，默认 `unknown`）。
- `_filament` 结构体新增 `tpu_model` 字段（`_tpu_model`，默认 `UNKNOWN`）：识别到 TPU 时记录具体型号，供 RGB 识别色使用。

**修改 `src/bambu_bus_ams.cpp`**
- 新增 `bambubus_filament_id_to_type()`：把 Bambu filament_id（如 `GFU98`）映射到 `_filament_type::tpu`。
- 在两处材料下发回调（`set_filament`、`set_filament_type2`）写入 `filament[ch].filament_type`；
  若为 TPU，则把 `filament[ch].tpu_model` 设为**该通道写死型号** `tpu_param_fixed(ch)->model`（忽略下发的 `GFU98`）。
  这正是「装料下发 → 识别 TPU → 走写死型号参数」的触发点，也为 RGB 识别色提供**真实内部型号**来源。

**修改 `src/Motion_control.cpp`（on_use 闭环接入 TPU 参数）**
- `MC_PULL_ONLINE_RGB_set` 调用处（`run()` 内 RGB 刷新分支）新增 v4.0-tpu 识别色逻辑：
  - 宏 `BMCU_ONLINE_LED_FILAMENT_RGB` **开启**：行为不变，按打印机下发真实耗材色显示。
  - 宏 **关闭**：不再显示微弱橙状态色，而是按通道**真实内部型号**点亮，用于**肉眼验证「程序实际用哪个型号送料」**：
    - `filament_type == tpu` → 显示该通道**写死型号** `tpu_model` 对应的专属纯色（一眼区分是哪种 TPU）；
    - 非 TPU（PLA/PETG/ABS/PA/未知/other）→ 统一显示 `TPU_NON_TPU_RGB_*` 一种颜色，便于与 TPU 区分。
  - 识别色走与普通状态色同级的亮度档（不经过下发色 gamma 通道），不刺眼。
- `run()` 的 on_use 段新增按通道决策（见 13.2）：
  - `if (filament[CHx].filament_type == tpu) tpu_p = tpu_param_fixed(CHx);` 用该通道写死型号；
  - 否则 `tpu_p == nullptr`，走原 v3.2 常量。
- 接入的全部参数字段：
  - `on_use_target_pct` / `on_use_band_hi`：目标压力带。
  - `feed_pwm_hi` / `feed_pwm_lo`：on_use 主路 PWM 推力上限，TPU 降级防过推/啃料。
  - `phase1_ms` / `phase2_ms` / `jam_ms`：三段式堵料避让时间窗（见 13.5）。
  - `phase1_lim` / `phase2_lim`：避让段力度上限，TPU 减小防啃料。
  - `pull_comp_m`：固定长度回抽时 TPU 通道额外多退的补偿长度（解决回弹送料不足）。

**修改 `build_one.sh`（单编脚本）**
- 第 7~10 参数 `TPU0~TPU3`：分别指定槽 0~3 的写死型号（如 `bash build_one.sh ... GFU98 GFU90 GFU95 GFU85`），缺省 `GFU85`。
- 通过 `PLATFORMIO_BUILD_FLAGS="-DBMCU_TPU_FIX0=.. -DBMCU_TPU_FIX1=.. -DBMCU_TPU_FIX2=.. -DBMCU_TPU_FIX3=.."` 注入（不改 `platformio.ini`）。
- 写死型号表并入现有变体编译，**不新增变体维度**（固件数量不变），只是每个固件内置 4 通道写死表。

**修改 `build_all_firmwares_fast.py`（全量快速编译）**
- 顶部读取环境变量 `BMCU_TPU_FIX0~3`（缺省 `GFU85`）。
- 写死型号宏 `BMCU_TPU_FIX0~3` **无条件追加**进所有变体的 `defs`，不读 `BMCU_TPU_MODEL`、不再有 TPU 隔离分支、不再有 `firmwares-tpu` 目录。
- 全量仍为 972 / 1884 个（固件数不增），每个固件都带 4 通道写死表。
- 基础链接的 worker 加**失败重试（最多 2 次）**，消除并发链接偶发竞争导致的整批失败。

**修改 `clean_build.sh` / `.gitignore`**
- `clean_build.sh` 的 `DIRS` 含 `firmwares` `firmwares_Release`（写死型号不新增目录，无需单独清理）。
- `.gitignore` 忽略 `firmwares/`（固件产出与编译产物不入库，发布页自行发布）。

### 14.5 三段式堵料避让逻辑（v4.0 参数化）

**澄清：三段时间窗本身不是 v4.0 原创，v3.2 的 `pressure_ctrl_on_use` 黄灯三步法已经是三段
（0-2s 中力推、2-5s 轻压保持、≥5s 真堵）。** v4.0 的改动是：把这三段各自的**时间长度与力度上限**
从写死的常量，改成**可按 TPU 型号缩放的参数**（见代码 `Motion_control.cpp:1658-1672`），
使软料能拉长窗口、调小力度，避免弹性料被硬顶误报堵。刚性料（PLA/PETG）仍走 v3.2 原值（jam=5000），不受影响。

| 阶段 | 时间窗 | 力度上限 | 目的 |
|---|---|---|---|
| 第 1 段 | `0 - phase1_ms` | `phase1_lim` | 中力推一把，协助顶过五通/送进挤出机 |
| 第 2 段 | `phase1_ms - phase1_ms+phase2_ms` | `phase2_lim` | 轻压保持，等打印机把料拉走 |
| 第 3 段 | `> phase1_ms+phase2_ms`（仍 `< jam_ms`） | `phase2_lim × 0.5` | 超长顶满时更保守，进一步防误报 |
| 真堵 | 累计 `> jam_ms` | — | 报堵料红灯（与原逻辑一致） |

所有时间窗/力度按型号在 `tpu_params.h` 中分级，越软的料窗口越长、力度越小。
（对比：刚性料沿用 v3.2 的三段 `phase1=2000/phase2=3000/jam=5000`；TPU GFU90 放宽到 `phase1=3000/phase2=5000/jam=8000`。）

### 14.5.1 每个 TPU 型号相对 PLA/PETG 的优化对比

TPU 与刚性料（PLA/PETG）的本质差异：高弹性、高摩擦、易堆料。直接套用刚性参数会导致
缓冲头（pressure）被弹性顶满、误判堵料、甚至硬推啃料。v4.0 对每个型号按**硬度分级**缩放 5 类参数。
下面以「相对原 v3.2 刚性参数的变化」逐型号说明推力如何改变：

**基准（PLA/PETG 等刚性料，v3.2 原值）**
- 目标压力带 52-65%，PWM 上限 HI≈480-550 / LO=1000，三段 `phase1=2000/phase2=3000/jam=5000`，
  避让力度 `phase1_lim=600 / phase2_lim=180`，回抽补偿 0。

**GFU98 — TPU for AMS（68D，最硬，接近刚性）**
- 最"接近 PLA"的 TPU。目标带 50/58（略降）、PWM 440/950（略降）、`phase1=2000/phase2=3000/jam=6000`、
  避让 `520/160`、回抽补偿 0.01m。改动最小，只在弹性余量上稍放宽。
- 适用：AMS 常规供料、追求接近刚性料手感。

**GFU02 — Generic TPU for AMS（约68D）**：参数与 GFU98 完全相同（同档硬度，通用料保守对齐）。

**GFU00 — TPU 95A HF（95A，仅 AMS HT 手动）** 与 **GFU95 — TPU 95A（95A）**
- 软一档。目标带 45/54、PWM 400/900、避让 `420/120`、窗口 `2500/4000/7000`、回抽 0.02m。
  推力比 GFU98 再降约 15-20%，窗口拉长，给 95A 弹性更多恢复时间。
- 适用：95A 软料，需 AMS HT 手动进料的场景。

**GFU90 — TPU 90A（90A，仅 AMS HT 手动）**
- 再软一档。目标带 40/50、PWM 360/850、避让 `360/100`、窗口 `3000/5000/8000`、回抽 0.03m。
  推力相对 PLA 降幅约 25-30%，窗口最长档之一。
- 适用：90A 软料、易堆料，最典型的「降推力 + 宽避让」示例。

**GFU85 — TPU 85A（85A，最软，禁用 PTFE 管）**
- 最保守档。目标带 35/46、PWM 320/800、避让 `300/80`、窗口 `3500/6000/9000`、回抽 0.04m。
  推力相对 PLA 降幅约 35-40%，窗口最长、回抽补偿最大（弹性回弹最多）。
- 适用：85A 极软料，禁用 PTFE 管，全程最保守防啃防误报。

**规律性总结（硬度硬→软：68D > 95A > 90A > 85A）**
- target_pct：50 → 45 → 40 → 35（越低，避免硬顶压缩而非前进）
- band_hi：58 → 54 → 50 → 46
- PWM HI/LO：440/950 → 400/900 → 360/850 → 320/800（持续推力上限逐档下调防啃料）
- 避让力度 phase1_lim/phase2_lim：520/160 → 420/120 → 360/100 → 300/80
- 时间窗 phase1/phase2/jam：随软度 2000/3000/6000 → 3500/6000/9000 逐级拉长（给弹性恢复时间）
- 回抽补偿 pull_comp_m：0.01 → 0.02 → 0.03 → 0.04（越软回弹越多，固定长度回抽额外多退）

> 注意：上表数值均为**基于硬度分级的初值占位**，并非最终校准值。每个型号的最终参数需
> 在成品板上实测迭代后填定（无串口，只能看动作 + RGB 灯判断）。标 `[待实测]` 的项即为需校准项。

### 14.6 参数表（`src/tpu_params.h` 字段含义）

| 字段 | 含义 | 趋势（越软越小/越长） |
|---|---|---|
| `on_use_target_pct` | 常态送料目标压力带中心 | 越低 |
| `on_use_band_hi` | 目标压力带上限 | 越低 |
| `phase1_ms` | 第 1 段中力推时长 | 越长 |
| `phase2_ms` | 第 2 段轻压时长 | 越长 |
| `jam_ms` | 真堵阈值（累计顶满） | 越长 |
| `phase1_lim` | 第 1 段 PWM 上限 | 越小 |
| `phase2_lim` | 第 2 段 PWM 上限 | 越小 |
| `feed_pwm_hi` | on_use 主路 PWM 上限（推一把） | 越小 |
| `feed_pwm_lo` | on_use 主路 PWM 下限（持续推力） | 越小 |
| `pull_comp_m` | 固定回抽弹性补偿（米） | 越大 |

当前表内 6 档型号（硬度硬→软）：GFU98(68D) > GFU00/GFU95(95A) > GFU90(90A) > GFU85(85A)。
**表中数值为初值，标注 `[待实测]` 的需上机校准后再固化。**

### 14.7 运行时数据流

```
打印机下发材料型号
   └─> bambu_bus_ams.cpp: set_filament / set_filament_type2
         └─> filament[ch].filament_type = tpu (经 bambubus_filament_id_to_type)
               └─> Motion_control::run(CHx) on_use 段
                     ├─ 某通道: filament_type==tpu ? (打印机设为 GFU98)
                     │     tpu_p = tpu_param_fixed(CHx)  // 用该通道写死型号 TPU_FIXED_ID[CHx]，忽略下发 GFU98
                     │     否则 tpu_p = nullptr (走 v3.2 常量)
                     └─> 用 tpu_p->* 覆盖 on_use 目标带/PWM 上限/避让/回抽补偿
```

### 14.8 全量快速编译算法详解（改进点 + TPU 链接稳定性修复）

**为什么原版慢、我们快**：原版 `build_all_firmwares_softload.sh` 走 `pio run` 逐个固件全量重编，
972 个固件要几小时。我们的 `build_all_firmwares_fast.py` 用**预编译 + 二进制修补**算法，约 1 分钟：

1. **提取工具链参数（带缓存）**：首次跑一次 `pio run -e moj -v` 抓出 C++/C 编译命令与链接命令，
   存 `.pio_parallel/verbose_cache.*`；之后源文件未变则直接复用，跳过这次 pio 编译（省最多时间）。
2. **按变体宏分类源文件**：扫描每个用户源引用了哪些变体宏（`BAMBU_BUS_AMS_NUM`、`AMS_RETRACT_LEN`、
   `BMCU_TPU_FIX0~3` 等），分 `INVARIANT`（不变）/ `OTHER`（模式相关）/ `RETRACT`（含回抽长度占位符）。
3. **每类只预编译一次 `.o`**：同样分类的源只编一次，得到少量 `.o`（约 392 个），
   **绝不每固件重编**（这是比原版快几个数量级的关键）。
4. **链接基础 elf**：把 `.o` 链接成 16 个「基础固件」elf（每个模式组合一个，回抽长度用占位符浮点）。
5. **二进制修补生成全部固件**：把占位符浮点在 elf 的 `.bin` 里替换为各档真实回抽长度，
   复制出 972 / 1884 个最终固件。**修补是纯字节替换，极快**。

**参数传递（写死型号并入）**：`BMCU_TPU_FIX0~3` 作为变体宏无条件追加进所有变体的 `defs/flags`，
不隔离、不新增命名空间、不新增产物目录。故常规变体的宏传参、输出 `firmwares/` 相对路径与原版完全一致。

**全量稳定性**：并发链接（max_links）对共享 `.o` / 框架 `.a` 偶发竞争，已在基础链接 worker 加**失败重试（最多 2 次）**。
实测：常规全量 972 个 **1分29秒**，均零失败。

### 14.9 构建与验证

- **默认（4 通道全 GFU85）**：`python build_all_firmwares_fast.py` → 972 个到 `firmwares/`。
- **写死 4 通道型号**：`BMCU_TPU_FIX0=GFU98 BMCU_TPU_FIX1=GFU90 BMCU_TPU_FIX2=GFU95 BMCU_TPU_FIX3=GFU85 python build_all_firmwares_fast.py` → 仍为 972 个（每固件带 4 通道写死表）。
- **单编调试**：`bash build_one.sh standard 1 1 SOLO 0.30 1`（默认 GFU85） / `bash build_one.sh standard 1 1 SOLO 0.30 1 GFU98 GFU90 GFU95 GFU85`（写死 4 通道）。
- 验证要点：固件二进制含完整参数表，无 TPU 通道时行为与 v3.2 一致；某通道设为 GFU98 时内部走写死型号软料参数、回传仍 GFU98。
- 实测耗时：常规全量 1分29秒（972），均零失败。

### 14.10 AHT20 状态灯与读取重试（SYS_RGB 状态机）

**设计目标**：AHT20 是辅助传感器，绝不能因它异常导致 BMCU 掉线/卡死/误报通讯故障。
同时要让用户能通过**主板系统灯（SYS_RGB）**直观判断 AHT20 是否在正常上报。

**核心原则（不故障）**
- `aht20.init()` 在开机只握手一次，`is_online()` 永久记录结果；
- `is_online()==false`（没接/坏了）→ `main.cpp` **完全不进入采样分支**，温湿度保持 `ams.h` 默认 22℃/20%，BMCU 核心送料逻辑零改动；
- `is_online()==true` 才周期采样；单次读失败**在本轮窗口内重试最多 3 次**；连续 3 次失败仅"本次采样失败"，温湿度**沿用上一次成功值**（从未成功过则保持默认 22/20），绝不清零、绝不回落到特殊值、绝不影响送料。

**SYS_RGB 系统灯（仅显示通讯心跳，不显示 AHT20 事件）**

系统灯只负责 BMCU 与打印机的通讯状态，由 `comm_ok` 状态机驱动，与 AHT20 无关：
- 启动瞬间 / 首心跳前：红（短亮）；
- 通讯正常 + 无 AHT20：浅灰白常亮 `0x38,0x35,0x32`；
- 通讯正常 + 有 AHT20：浅灰白 **每 3 秒闪一下（约 150ms）**——为避免灯珠靠近 AHT20 发热烤高读数，间歇闪烁而非常亮；
- 通讯失败：红 `0x10,0,0` 常亮。

> AHT20 **不影响任何 RGB 灯**：没接/坏了/读失败都不在系统灯或通道灯上体现，温湿度走默认 22℃/20%。详见 `docs/rgb_led_meaning.md` 第 1 节。

**采样状态机（`main.cpp` 主循环内，非阻塞）**

```
aht20_next_ms    本轮周期计时（AHT20_PERIOD_MS，约 2s）
aht20_deadline   单次测量等待（start_measure 后 +90ms）
aht20_waiting    是否处于"已触发测量、等结果"状态
aht20_retries    本轮已尝试次数（上限 AHT20_RETRY_MAX=3）

每帧：
  if !is_online():            aht20_waiting=false            // 没传感器就不采样
  elif 到周期且!waiting:       start_measure(); waiting=true; deadline=now+90
  if waiting && now>=deadline:
      if get_measure(t,h) 成功: 写温湿度; waiting=false; next=now
      else:
          retries++
          if retries<3:   start_measure(); deadline=now+90   // 重试
          else:           waiting=false; next=now            // 3次失败,沿用上次值
```

**关键实现细节**
- 重试发生在**同一轮采样窗口内**（`waiting` 期间反复 `start_measure`+等待 90ms），
  不会跨越周期，避免把重试拖成连续高频读。
- 读失败只表示"本次读取失败"，不表示"AHT20 消失"——`is_online()` 仍为真，
  下一轮仍正常尝试；只有开机握手失败才彻底不采样。
- 采样周期与系统灯心跳互不耦合，采样状态机无阻塞、不写灯。

**验证**
- 双开关 RGB_ON（SOLO 0.30）+ 单开关 RGB_OFF（AMS_D 0.80）均编译通过；
- 行为验证（需上板）：无 AHT20 → 系统灯浅灰白常亮、温湿度上报 22/20；
  有 AHT20 且通讯正常 → 系统灯每 3 秒闪一下；制造读取失败 → 温湿度保持上次值、系统灯无变化。

### 14.11 TPU 间歇送料 + Stage2 装填降级（解决软料被挤出缓冲头间隙）

> 本节记录 `dev/v4.0-tpu` 分支在 13.2/13.4 基础上，针对实测问题的二次增强。
> 背景：实测 `GFU95`（TPU 95A，RGB 识别色确认已识别为黄色 TPU）时，料不从管道正常进入，
> 而是被连续推力从缓冲头（buffer）间隙挤出来。根因是此前 TPU 降级**只覆盖了 on_use 常态闭环**
> （表 A 的 `feed_pwm_hi/lo`），而**Stage2 装填阶段与"持续转"行为未处理**，软料被持续猛推导致挤出。

#### 14.11.1 问题根因

- **Stage2 装填阶段未接 TPU 降级**：`hold_load()`（装填保持逻辑）原用 `MC_LOAD_S2_PWM_HI/LO` 硬编码常量
  （`_S2_PWM_LO = 1000` 全速），**完全没接 `tpu_p->feed_pwm_hi/lo`**。即 TPU 在装填那一脚仍走刚性料 1000 全速，
  软料被猛推、堆积挤出缓冲头间隙——正是实测现象。
- **on_use 持续转动、无间歇**：原 on_use 闭环是连续 PWM 闭环，只要缓冲头低于目标就持续转。对刚性料没问题，
  对软料则一直被顶着压缩，料不前进反而从间隙溢出。TPU 需要「推一下、停一下」的**间歇脉冲式送料**，
  让料在停窗口里被打印机拉走/自身松弛，避免堆积。
- **`pct_fast_onuse` 硬编码 50% 对软料偏高**：95A 的 `target=45`，原 `pct_fast_onuse=50` 导致缓冲头稍回落就给大推力，
  加剧过推。

#### 14.11.2 改动清单

**`src/tpu_params.h`**：`_tpu_param` 结构新增两个字段，参数表逐型号补初值（越软停越久）：

| filament_id | 型号 | `push_cycle_ms`（周期） | `push_on_ms`（推窗口） |
|---|---|---|---|
| GFU98 / GFU02 | 68D | 800 | 500 |
| GFU00 / GFU95 | 95A | 900 | 420~450 |
| GFU90 | 90A | 1000 | 400 |
| GFU85 | 85A | 1200 | 400 |

> 周期内分「推窗口（`push_on_ms`，正常给 PWM）」与「停窗口（剩余时间，PWM 强制为 0）」。
> 越软 → 周期越长、推窗口占比越小（85A 停窗口最久）。

**`src/Motion_control.cpp`**：

1. **Stage2 装填接 TPU 降级**：`hold_load()` 新增参数 `const _tpu_param *tpu_p = nullptr`；
   内部 `pwm_hi/pwm_lo` 改为 `tpu_p ? tpu_p->feed_pwm_hi/lo : MC_LOAD_S2_PWM_HI/LO`。
   两处 `hold_load()` 调用（`send_out` 装填段、`on_use` 前段）传入 `tpu_p_run`（见下）。
   装填阶段 TPU（`feed_pwm_lo≈800~900`）不再用刚性料 1000 全速夯。

2. **`pct_fast_onuse` 跟随 TPU target**：`tpu_p ? (target_pct + 5.0f) : 50.0f`，
   避免软料小幅回落即触发大推力。

3. **核心新增——间歇送料统一门控**：把 `tpu_p` 提升到 `run()` 函数级（`tpu_p_run`，
   专用/通用两分支均赋值）。在 PWM 统一出口（`Motion_control_set_PWM` 之前）插入：
   ```cpp
   if (tpu_p_run != nullptr)
   {
       const uint16_t cyc = tpu_p_run->push_cycle_ms;
       const uint16_t on  = tpu_p_run->push_on_ms;
       if (cyc > 0u && on < cyc)
       {
           const uint32_t phase = (uint32_t)(now_ms % (uint64_t)cyc);
           const bool pushing_fwd = (dir != 0.0f) && (((float)pwm_out0) * dir > 0.0f);
           if (pushing_fwd && phase >= (uint32_t)on)
               pwm_out0 = 0;   // 停窗口: 电机停转, 让软料松弛/被打印机拉走
       }
   }
   ```
   - 仅对 TPU 通道、**正向往前送料**（`x` 与 `dir` 同向）生效；回退/拉料（`x*dir ≤ 0`）不受影响，保证回抽正常。
   - 覆盖 Stage2 装填 / on_use 常态 / 顶满避让**所有正向送料分支**，刚性料（`tpu_p_run==nullptr`）零差异。
   - `pwm_out0` 由 `const int` 改为 `int` 以支持停窗口清零。

#### 14.11.3 效果与验证

- TPU 通道不再是「持续猛推」，而是「推一段（周期 800~1200ms，推窗口约 400~500ms）→ 停一段（电机停转）→ 再推」的
  间歇脉冲式送料；缓冲头不被持续顶着，软料靠停窗口松弛/被打印机拉走，正常进入管道而非从间隙挤出。
- 刚性料（PLA/PETG…）完全不受影响（门控条件 `tpu_p_run==nullptr` 直接跳过）。
- 验证：编译 `build_one.sh standard 1 1 SOLO 0.30 0`（含通用 TPU 运行时识别）通过；
  上板实测 95A 应见电机间歇转动、料正常入管。

> 注意：本改动在 `dev/v4.0-tpu` 分支，**本地未提交未推送**。间歇周期初值（`push_cycle_ms`/`push_on_ms`）
> 为基于硬度的经验占位，需实测后按机型校准（越软停越久）。

---

## 15. SSD1306 OLED 状态屏（`BMCU_OLED`，v4.0-tpu 新增）

> 本节记录 `dev/v4.0-tpu` 分支新增的 SSD1306 OLED 显示支持。它通过**复用 AHT20 的软件 I2C 总线**（PB10/PB11）显示 BMCU 当前状态（温湿度、通讯状态、四通道料况），不上机也能肉眼看到 BMCU 状态。

### 15.1 硬件与接线

- **屏幕**：SSD1306 驱动，0.96" I2C OLED（128×64，单色）。
- **总线复用**：与 AHT20 **共用同一软件 I2C 通道**（PB10=SCL、PB11=SDA，开漏 + 外部 4.7k 上拉）。不新增任何引脚——这是把 OLED 加上去零硬件成本的关键。
- **I2C 地址**：`0x3C`（7 位，写地址 `0x78`）。
- **工作频率**：400 kHz（软件 I2C `i2c_speed` 取 `FAST`），与 AHT20 同频。
- **外部上拉**：PB10/PB11 需外接 4.7kΩ 上拉到 3.3V（AHT20 模块通常自带；裸片/OLED 模块需各自补上拉，总线挂两个设备时确保上拉到位）。

> 复用同一条软件 I2C 总线意味着 OLED 与 AHT20 **分时复用**总线（不会同时传），刷新率需克制（当前 1Hz 整屏重绘），否则会与 AHT20 采样抢总线、拉长阻塞窗口。后续可提高到 200~300ms，并改局部刷新降开销。

### 15.2 软件结构

- **驱动实现**：`src/oled/ssd1306_oled.cpp` / `ssd1306_oled.h`（自写实现，不依赖原参考库 `OLED-0.96-4P-I2C/`，该参考库已删除）。
- **总线转发接口**：AHT20 头文件 `src/aht20/aht20.h` 新增三个转发接口，供 OLED 在**完全不 include AHT20 私有对象**的情况下复用同一条软件 I2C：
  - `void bus_start(uint8_t addr)`：发起始条件 + 写地址（addr 为 7 位地址）。
  - `bool bus_write(uint8_t b)`：写一字节 + ACK，返回是否收到 ACK。
  - `void bus_stop()`：发停止条件。
  - 这三个函数由 `main.cpp` 以 `g_aht20.bus_start / bus_write / bus_stop` 形式调用（`g_aht20` 是 AHT20 单例），把底层软件 I2C 暴露给 OLED。
- **屏幕对象**：`SSD1306_OLED` 单例（全局 `g_oled`），封装显存、页状态机、热插拔探测。
- **绘制接口**：
  - `init()`：发 SSD1306 上电序列；先 `probe_ack()` 探测屏是否存在，成功才置 `s_ready=true`。
  - `tick(bool comm_ok)`：主循环每 1s 调用一次。内部先 `probe_ack()` 重探——屏掉线则 `s_ready=false` 并跳过绘制（整屏不亮）；屏重新插上后 `probe_ack()` 成功自动恢复 `s_ready=true` 继续显示（**热插拔无需重启**）。随后按当前页重绘并 `oled_update()`。
  - `probe_ack()`：仅发一次地址写探测、不修改 `s_ready` 也不清显存，用于不破坏显示的前提下判断屏是否在线。
  - `draw_aht20(bool comm_ok, float temp, float humi, ...)`：画 AHT20 页（详见 15.4）。
  - `draw_channels()`：画四通道概览页（详见 15.4）。
  - `draw_action(...)`：动作覆盖页（见 15.4 动作显示）。
- **多页轮询**：`oled_page` 枚举（`page_aht20` / `page_channels` / `page_count`），由 `tick()` 内部计时器每数秒翻一页（AHT20 页、四通道概览页循环切换），无需用户干预。

### 15.3 主循环集成（`src/main.cpp`）

- OLED 相关代码全部包在 `#ifdef BMCU_OLED` 内。**不定义 `BMCU_OLED` 时，OLED 代码完全不编译、不占用 Flash/RAM**，即 OLED 是纯可选功能。
- 初始化：`bambubus_init()` 在初始化阶段**标记本机 AMS 槽位在线**（`ams[slot].online = true`），使打印机后续下发的 `filament_type` / 写死型号逻辑能正确生效（否则 `set_filament` 因槽位离线而不处理材质，TPU 识别与写死表均不触发）。
- 刷新：`g_oled.tick(comm_ok)` 每 1s 调用一次（不阻塞主循环），内部自行处理热插拔重探 + 多页轮询 + 重绘。
- 温湿度数据取自 AHT20 主循环采样结果（`g_aht20` 的 compartment 温度/湿度），与上报给打印机的数据同源。
- 动作覆盖：送料状态机在 `send_out` / `pulling_back` / `before_on_use` 等动作进入时，调用 `g_oled.show_action(ch, action)` 立即插队显示对应通道动作（优先级高于常规页轮询），动作结束后回到常规页轮询。

### 15.4 状态显示设计（与 RGB 联动）

OLED 与 RGB 灯是**同一套状态信息的两种呈现**：不上机 / 通讯失败时 OLED 也能像 RGB 一样提示用户，不依赖打印机下发。

**第 0 页 · AHT20 状态页（每轮询周期先显示）**
| 行 | 内容 | 含义 |
|---|---|---|
| 行 0 | `T:23.5C H:45%`（合并一行） | AHT20 实时温湿度（原两行合并以腾出行）；未焊 AHT20 显示 `AHT20 OFFLINE` |
| 行 1 | `COMM: OK` / `COMM: ERR` | BMCU 与打印机通讯状态（取 `comm_ok`）；未连上显示 `ERR`，提示检查 RS485，与 SYS_RGB 系统灯红=掉线逻辑一致 |
| 行 2 | `LOCK: 98/90/95/85`（取写死型号末两位） | 编译写死的 4 通道 TPU 型号摘要；与 `BMCU_TPU_FIX0~3` 一致，肉眼确认写死表已编入。非 TPU 槽位不在此摘要强调 |

**第 1 页 · 四通道概览页**
| 行 | 内容 | 含义 |
|---|---|---|
| 行 0~3 | `CH0:状态/料型 颜色` 等四行 | 每通道显示「空 / 有料 / 进料中 / 推料中」+ 当前料类型（GFUxx 或 PLA/PETG），与通道 RGB 灯含义对齐 |

**动作覆盖页（临时插队）**
当某通道进入送料动作（进料 `send_out` / 退料 `pulling_back` / 准备上料 `before_on_use`）时，OLED 立即切到该通道动作页（显示 `CHx: 进料/退料/上料`），动作结束后自动回到常规页轮询。这与 RGB 通道灯的动作/故障状态色（绿/紫闪/黄）信息同源。

> 写死型号摘要（LOCK 行）的价值：配合 `FILAMENT_RGB_OFF` 看 RGB 灯自检内部型号时，OLED 的 LOCK 行可双重确认"编译写死型号确实编进了固件"，排查写死表不生效问题时尤其有用。

### 15.5 热插拔说明（重要）

早期版本 OLED 屏若开机时未插（或开机后带电拔插），因 `s_ready` 在 `init()` 发序列前定死、掉线后无代码复位重探，导致重插屏不亮。现修复为：
- `tick()` 每 1s 调 `probe_ack()` 探测；屏掉线（`probe_ack()` 失败）即 `s_ready=false`、跳过绘制（屏熄灭）。
- 屏重新插上后 `probe_ack()` 成功自动恢复 `s_ready=true` 继续显示；`main.cpp` 每 10s 触发 `g_oled.reinit()` 重新走一遍上电序列，确保热插拔后的屏稳定初始化。
- **结论**：开机有没有屏都行；开机后带电拔插 OLED，拔掉会熄灭、重新插上会自动点亮，无需重启 BMCU。

### 15.6 编译开关与 OLED 默认行为

- **OLED 默认编入固件，无脚本开关**。`src/oled/ssd1306_oled.h` 顶部用 `#ifndef BMCU_OLED / #define BMCU_OLED / #endif` 默认定义 `BMCU_OLED`，驱动代码无条件编译进固件；运行时 `init()` 自动 `probe_ack()` 探测屏是否存在，无屏则跳过显示、零影响（详见 [`编译指南.md`](./编译指南.md) 的 `BMCU_OLED` 说明）。
- **要彻底关闭 OLED**（剥离驱动代码、省 Flash/RAM）：直接注释掉 `ssd1306_oled.h` 顶部那 3 行宏定义即可，**不改动 `build_one.sh`、`build_all_firmwares_fast.py` 或 `platformio.ini`**。
- 历史上 `build_one.sh` 曾用第 11 参数 `OLED=1` 注入 `-DBMCU_OLED`，现已移除：头文件侧默认开使该脚本注入无额外作用，反而容易让人误以为"默认关"。


