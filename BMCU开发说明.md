# BMCU-C 固件开发说明

> 面向开发者的架构、实现逻辑与通讯协议说明。无需逐行读源码即可理解整体设计。
> 源码注释已翻译为中文（见 `README.md` 与各 `.cpp/.h`）。

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

## 2. 硬件平台

| 资源 | 用途 |
|---|---|
| USART1 (PA9=TX, PA10=RX, PA12=DE) | 与打印机通讯的半双工 RS485 总线 |
| TIMER / PWM | 4 路电机驱动（每通道一路） |
| ADC + DMA | 采集 buffer 位置电压（空通道检测、DM 开关） |
| 软 I2C（GPIO 模拟开漏） | 4 路 AS5600 磁编码器（测 buffer 轮角度→距离/速度） |
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
- **pull_back / before_pull_back**：回抽，末端 **线性减速**（`PULL_RAMP_M=15mm` 区），速度从 `PULL_V_FAST=60mm/s` 降到 `PULL_V_END=12mm/s`。
- **idle / send_out / redetect**：按 buffer 位置与有无 filament 决定动作；空闲 10s 后仅在末端动作。
- **AUTOLOAD**（DM 双微动开关板）：触碰第一个开关触发装入，第二个开关（挤出机后）确认完全插入，再送约 120mm；防卡保护：buffer 卡住则回抽重试（最多 3 次）。

---

## 7. 运动控制与传感器

- **AS5600 磁编码器**（`many_soft_AS5600.cpp`）：软 I2C（开漏 50MHz，正确 ACK/NACK/START/STOP），4 通道并行轮询，限速约 1ms/次。输出角度→距离：`kAS5600_MM_PER_CNT = -(π*7.5)/4096`（7.5mm 轮半径）。`updata_stu` 判磁铁在线/强弱，`updata_angle` 算速度。
- **AS5600 健康门控**：连续失败 `kAS5600_FAIL_TRIP=3` 次判离线并隔离该通道；恢复需 `kAS5600_OK_RECOVER=2` 次连续正常，防止失控。
- **ADC_DMA**（`ADC_DMA.cpp`）：并行扫描 ADC1+ADC2，DMA 半满/全满后台滤波，约 5ms 更新一次；用于空通道检测电压、DM 微动开关电压。
- **校准**（`MC_PULL_calibration.cpp`）：首次空通道启动记录每通道“无 filament”检测点（`MC_PULL_V_OFFSET/MIN/MAX`）、霍尔极性（`MC_PULL_POLARITY`）、DM 开关阈值（`MC_DM_KEY_NONE_THRESH`）；按住 buffer 约 5s 可重新校准。

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
| `AMS_RETRACT_LEN` (米) | filament 回抽长度（从 AMS 分线器末端起算），如 SOLO=0.095 |
| `BMCU_DM_TWO_MICROSWITCH` | DM 双微动开关板 + AUTOLOAD 辅助 |
| `BMCU_P1S` | P1/P1S/X1 打印机适配（更长 PTFE 路径） |
| `BMCU_SOFT_LOAD` | soft_load(A1)：更低装入力（弱弹簧单元） |
| `BMCU_ONLINE_LED_FILAMENT_RGB` | 装入时 ONLINE LED 显示 filament RGB 颜色 |

`env:fw` 通过环境变量注入上述宏；`env:moj` 为开发者默认（AMS_A，0.095m）。

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

> 文档基于源码静态分析整理，覆盖功能、实现逻辑与通讯协议主干。具体字节级帧样例可参考 `bambu_bus_ams.cpp` 中 `// 3D C5 ...` 形式的抓包注释。
