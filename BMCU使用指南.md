# BMCU 使用指南

> 面向使用者：如何为你的硬件与打印机选对固件、刷写、校准，以及使用中的注意事项。
> 开发细节见 [`BMCU开发说明.md`](./BMCU开发说明.md)，编译方法见 [`编译指南.md`](./编译指南.md)。

---

## 1. 这个固件是做什么的

BMCU-C 是 **Bambu Lab AMS（自动多色换料系统）的开源替代固件**，运行在第三方 BMCU 370C 硬件上，使 4 通道 filament 切料/换料器像原厂 AMS 一样被打印机识别与控制：多色自动换料、filament 装入/回抽/卸载、空通道校准、堵塞与防卡保护、AUTOLOAD 自动装载、filament 元数据掉电持久化、RGB 状态指示。

固件通过半双工 RS485 与打印机通讯，支持两套协议：
- **BambuBus**（`0x3D`）：BMCU 直接挂在打印机 AMS 总线上时
- **AHUB**（`0x33`）：BMCU 通过 AMS HUB 串联时

---

## 2. 固件选型（这么多固件是干嘛的）

仓库提供大量固件变体，是因为装入力、自动装载、LED、槽位、回抽长度都需要按你的硬件与打印机组合来定。变体按下面四个维度组合，对应 `firmwares/` 目录树（由 `build_all_firmwares_softload.sh` 生成，或从 **GitHub Releases** 下载）。

> 每个维度的详细选型理由见仓库根目录 `which_to_choose_*.txt`（编译时会复制到对应目录层）。下面只给结论速查。

### 2.1 装入力模式（顶层目录）

| 模式 | 适用场景 |
|------|----------|
| `standard(A1)` | 默认。A1 等 PTFE 管较短且较直（摩擦小）的配置 |
| `high_force_load(P1S)` | P1S 或 PTFE 路径长/多弯折，装入力更大；装入被拒绝/失败时优先切这档 |
| `soft_load(A1)` | 仅 A1/A1 Mini。装入时听到打磨/咔哒声、想降低 BMCU 磨损时试用；可能装入被拒，优先方案是换更强的压力弹簧 |

**不确定就从 `standard(A1)` 开始。**

### 2.2 AUTOLOAD / NO_AUTOLOAD

- **AUTOLOAD**：为 DM 双微动开关板设计，自动把 filament 送过挤出机；单开关板第一阶段需手动推入。
- **NO_AUTOLOAD**：手动把 filament 推到 PTFE 管里可见为止，逻辑更简单安全，但装入时人工操作更多。

### 2.3 FILAMENT_RGB_ON / FILAMENT_RGB_OFF

仅影响前 LED 是否在装入后显示 filament 颜色，**无其他行为变化**。想要安静/无灯环境选 `FILAMENT_RGB_OFF`。

### 2.4 槽位与回抽长度（SOLO / AMS_A~D）

- **SOLO**：单 BMCU，回抽 9.5 cm（`solo_0.095f.bin`）。
- **AMS_A~D**：多 BMCU 配置（2–4 台，每台必须用不同槽位），或单台但需要更长回抽。
- 回抽长度从打印机内部 **AMS 分线器末端** 起算，加一点安全余量使 filament 清过分线器。例如到分线器约 9 cm，则选 9.5 cm 档。P1 配置常因 PTFE 更长而需更长的回抽。

---

## 3. 刷写固件

1. 从 Releases 下载对应变体的 `firmware.bin`，或用 [`编译指南.md`](./编译指南.md) 自行编译。
2. 使用 **BMCU Flasher** 在线或本地刷写，**无需 wchisptool**。
3. 若用 PlatformIO + WCH-Link 调试器烧录：`pio run -e <env> -t upload`。

普通使用推荐 BMCU Flasher，无需调试器。

---

## 4. 打印机配置与兼容性

- 打印机必须配置为 **AMS**，而不是 **AMS Lite**；使用 AMS Lite 会导致兼容性问题。
- Bambu Lab 正通过固件更新限制本地 BMCU 的互通性，详见 [`bmcu-vs-firmware-locks.md`](./bmcu-vs-firmware-locks.md)。
- 打印机启动会报 **HMS 警告**（来自心跳握手），属已知可接受行为，**不阻断打印**。

---

## 5. 校准

- **首次刷写必须所有通道为空**；否则取出 filament 后**按住 buffer 约 5 秒**重新校准。
- 校准会记录每通道“无 filament”检测点、霍尔极性、DM 开关阈值，并存入 Flash。
- filament 元数据与已装入状态掉电持久化，断电后可续打。

---

## 6. 使用注意事项

- **二代打印机不被识别**：多为 RS485 信号 A/B 接反，可尝试对调（需明确自己在做什么）。
- **系统灯含义**：心跳时浅灰为正常；总线错误时红色。
- **AHT20 版本（v2.0-aht20）额外注意**：
  - `PB10`/`PB11`（AHT20 的 SCL/SDA）需外接 **4.7kΩ 上拉到 3.3V**（模块通常自带，裸片需补）。
  - 这两脚原被 USART3 调试串口占位；未开启调试输出时安全，若开启调试日志需另选引脚或关闭 AHT20。
  - 温湿度由现有协议自动上报给打印机，无需额外设置。
- **多 BMCU 配置**：每台必须使用不同槽位固件（AMS_A~D），并各自校准。

---

> 文档基于仓库源码与选型说明整理。选型细节以 `which_to_choose_*.txt` 与 [`编译指南.md`](./编译指南.md) 为准。
