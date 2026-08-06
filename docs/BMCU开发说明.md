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
- 本仓库自身的迭代使用 git 标签管理（如 `v1.0-baseline`、`v2.0-aht20`、`v2.1-aht20`、`v3.0-autoretract`、`v3.1-autoretract`、`v3.2-fix105`、`v3.2.1-fix105`），后续版本递增。

其中 `v1.0-baseline` 是基于原作者 V10.5 整理出的可编译、有中文文档的干净基线（修复编译问题、新增编译脚本与说明文档），作为后续开发的起点；`v2.0-aht20` 在其基础上新增 AHT20 温湿度传感器支持（见第 7 章），最新 `v2.1-aht20` 在此基础上新增温湿度探测模式（见第 6 章使用指南）。`v3.0-autoretract` 起引入双开关自动回抽（用 S2 自动判定料根，见第 6.3 节）；`v3.1-autoretract` 修复双开关自动回抽退料后不自动送料；`v3.2-fix105` 修复「暂停/停止不停机」与「进料电机黄灯三步法避让」（见第 6.2 节）；**`v3.2.1-fix105` 为当前最新，专修 AHT20 温湿度传感器驱动缺陷**（见第 12 章）——3.2 核心功能代码未改动，仅 `src/aht20/`、`src/main.cpp` 自检逻辑有更新。回退到基线：`git checkout v1.0-baseline`。使用与固件选型见 [`BMCU使用指南.md`](./BMCU使用指南.md)。

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
  - **进料缓冲避让（v3.2-fix105 改进）**：料过五通/到挤出机入口把缓冲头顶满时，原版会一直死命硬推（空转啃料、易误报堵）。因 BMCU 与打印机无「料到哪儿」的通讯，无法靠电机转速区分「已过五通正常送料」与「真堵」，故改**时间配合法**（`pressure_ctrl_on_use`，见 `Motion_control.cpp`）：顶满累计计时 `g_on_use_full_ms[]`，① 0~2s 中力推一把（PWM cap ≈600）帮过五通；② 2~5s 减到轻压（cap ≈180）只保持不后退、等打印机拉走，此间缓冲头回落到正常带内即清零计时、恢复正常送料（绿灯）；③ ≥5s 仍顶满才算真堵，置 `g_on_use_jam_latch` 报红灯停机。**顶满但 <5s 亮黄灯**（保护避让、非故障），仅真堵亮红灯。
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

多重兜底：`AMS_RETRACT_LEN`（编译为 `2.00f`）作为安全上限，若退料超过该距离仍未检测到 S2 释放，自动停电机并进入 `filament_redetect`，绝不卡死。状态机顶部有保护：若因中断等离开回抽状态，立即解冻上报、复位自检相位，防止冻结卡死。

> 单开关板只有 1 个开关，无法用 S2 判定料根，**自动回抽无效**，仍走固定长度逻辑（需按 PTFE 长度编译对应固件）。

**暂停/停止即时响应（v3.2-fix105 改进）**：打印机发来暂停/停止时，若 BMCU 正在 `send_out` 送料（此时 `loaded=0xFF`，原版因 `allow_stop=(loaded==ch)` 而忽略指令、继续转），或正在 DM 自动装载送料，都会**立即退出送料并停机**。`send_out` 场景由 `bambu_bus_ams.cpp` 的 `is_stop_on_use` 放宽 `allow_stop` 限制处理；DM 自动装载送料场景由 `Motion_control_request_stop_dm_autoload()`（`Motion_control.cpp`）经 `g_dm_autoload_stop_req[]` latch 在 DM 状态机开头中止。

---

## 7. 运动控制与传感器

- **AS5600 磁编码器**（`many_soft_AS5600.cpp`）：软 I2C（开漏 50MHz，正确 ACK/NACK/START/STOP），4 通道并行轮询，限速约 1ms/次。输出角度→距离：`kAS5600_MM_PER_CNT = -(π*7.5)/4096`（7.5mm 轮半径）。`updata_stu` 判磁铁在线/强弱，`updata_angle` 算速度。
- **AS5600 健康门控**：连续失败 `kAS5600_FAIL_TRIP=3` 次判离线并隔离该通道；恢复需 `kAS5600_OK_RECOVER=2` 次连续正常，防止失控。
- **ADC_DMA**（`ADC_DMA.cpp`）：并行扫描 ADC1+ADC2，DMA 半满/全满后台滤波，约 5ms 更新一次；用于空通道检测电压、DM 微动开关电压。
- **校准**（`MC_PULL_calibration.cpp`）：首次空通道启动记录每通道“无 filament”检测点（`MC_PULL_V_OFFSET/MIN/MAX`）、霍尔极性（`MC_PULL_POLARITY`）、DM 开关阈值（`MC_DM_KEY_NONE_THRESH`）；按住 buffer 约 5s 可重新校准。
- **AHT20 温湿度传感器**（`aht20.cpp`）：独立软件 I2C 通道（PB10=SCL / PB11=SDA，两线均开漏 `GPIO_Mode_Out_OD`，符合标准 I2C 规范），**不与** 4 路 AS5600 共用总线。写时拉低=低、释放=靠外部上漏拉高，读 SDA 时也保持开漏、释放后靠外部 4.7k 上拉读 IDR（**绝不切输入模式**——CH32V203 上动态改 CNF/MODE 不可靠，曾是该传感器读不到的根因）。严格按官方手册流程：上电等 5ms（`init()` 内 `delay(200)` 稳压）→ 发写测量命令 `0x70 0xAC 0x33 0x00`（`start_measure`）→ 等 ≥80ms（`read_blocking` 内先 `delay(80)` 再轮询状态字 bit7=0 就绪）→ 发 `0x71` 读 7 字节（`get_measure`）：状态字 + SRH[19:0] + ST[19:0] + CRC，换算 `H=rawH/1048576*100%`、`T=rawT/1048576*200-50℃`。**上电自检**：`main.cpp` 在 `MC_PULL_calibration_boot()` 之前调用 `g_aht20.init()` + `read_blocking()`，成功则亮蓝灯→绿灯，失败不阻塞、直接进主程序。之后每 2 秒由主循环非阻塞采样（start_measure → 90ms 后 get_measure），结果写入 `ams[].filament[].compartment_temperature`（℃）/ `compartment_humidity`（%），由现有 ahub / bambu 协议自动上报打印机；未焊接 AHT20 时维持默认值 22℃/20%。

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
| `AMS_RETRACT_LEN` (米) | filament 回抽长度（从 AMS 分线器末端起算）；SOLO 固定 0.095，AMS_A~D **支持 0.10~2.00 米（步长 5cm，共 39 档）**，可由编译参数/环境变量自定义 |
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

- 系统灯：正常心跳时浅灰（`0x38,0x35,0x32`）；总线错误时红色（`0x10,0,0`）。
- 打印机启动会报 **HMS 警告**（来自 `0x20` 心跳握手），属已知可接受行为，不阻断打印。
- 首次刷写必须**所有通道为空**；否则取出 filament 后按住 buffer 约 5s 重新校准。
- 二代打印机若不被识别，多为信号 A/B 接反，可尝试对调（需明确自己在做什么）。
- 调试串口日志见 `Debug_log.cpp`（`DEBUG(...)` 宏）。

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
