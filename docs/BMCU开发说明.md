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

> 本章集中记录 3.0/3.1/3.2 三个标签的**改动动机、根因分析、具体代码改动点**，供回溯。代码行号/符号以各标签对应提交为准；功能面描述见 README「版本说明」。当前最新标签为 `v3.2.1-fix105`（AHT20 驱动修复，见第 12 章）。
> 本章集中记录 3.0/3.1/3.2 三个标签的**改动动机、根因分析、具体代码改动点**，供回溯。代码行号/符号以各标签对应提交为准；功能面描述见 README「版本说明」。

### 13.1 v3.0-autoretract：双开关自动回抽（S2 判定料根）

- **新增能力**：双微动开关板（S1=进料口、S2=挤出机后）在回抽时不再依赖编译期固定的 `AMS_RETRACT_LEN`，改用 **S2 释放** 自动判定料根位置，自动决定回抽距离。
- **关键宏**：`BMCU_DM_AUTO_RETRACT`（默认随 `BMCU_DM_TWO_MICROSWITCH` 派生；双开关=1、单开关=0）。关闭则双开关也走固定回抽长度（需编译全长度矩阵），见第 6.3 节说明。
- **代码位置**：`Motion_control.cpp` 中 `dm_autoretract_phase_enum`、`dm_ar_phase[]`、`dm_ar_finish_pullback` lambda；自检用 `dm_key_raw[]`（原始开关状态，不含手势覆盖）、`dm_ar_freeze_report[]`（冻结对打印机上报）。
- **原理**：退料期间冻结 MC_ONLINE 上报避免 S2 跳变误触发打印机；以 `ks`（由 `dm_key_to_state()` 解码）判料根；`AMS_RETRACT_LEN(=2.00f)` 退化为安全上限兜底。

### 13.2 v3.1-autoretract：修复双开关自动回抽退料后不自动送料

#### 现象
v3.0 双开关自动回抽退完料后，不会自动向前送料就位，需用户手动干预。

#### 根因（读代码确认）
BMCU 370C 机构中 **S1（进料口微动开关）永远被 BMG 滚轮压住**，`ks` 的物理真实取值只能是：
- `1` = 两开关都按（both，S1+S2，≈1.8V）
- `2` = 仅 S1 按下（≈1.5V，即 S2 已释放）
- `3` = 仅 S2 内侧（≈1.0V，异常/进料中）
- **永远到不了 `0`（none）**

v3.0 的回抽状态机有两段：
1. `AR_RETRACT_WAIT_S2`：退料，等到 `ks==0` 才算完全退出；
2. `AR_RESEAT_WAIT_S1S2`：再正推 `AR_RESEAT_MAX_M≈5cm`，直到 S2 重新按下（`ks==1`）或超时 200 周期。

由于 `ks` 到不了 `0`，第 1 段靠 `d >= safety_max` 兜底退出，第 2 段则靠 200 周期超时退出；退出后 `dm_autoload_gate` 仍保持置位（`dm_auto` 的 Stage1 `S1_DEBOUNCE` 仅在 `ks==0 + idle` 时复位），导致 `dm_auto` 自动装载流程被永久挡住 → **退完不自动送料**。

#### 改动清单（`Motion_control.cpp` + `bambu_bus_ams.cpp`）
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

#### 验证点
- 双开关板退料后无需手动干预即自动送料就位；
- 单开关板（`BMCU_DM_AUTO_RETRACT=0`）走固定长度，不受影响；
- `AMS_RETRACT_LEN` 仍作为安全上限兜底防卡死。

### 13.3 v3.2-fix105：仅修 bug（不丢 3.1 功能）

> 分支 `bugfix/v3.2-fix105`，基于 `v3.1-autoretract`。**仅修两个遗留 bug，功能/上报号 `10.50` 完全不变**。具体功能面说明见 README。

#### Bug1：手动暂停 / 停止时 BMCU 不停

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

#### Bug2：进料电机缓冲顶满误报堵料 → 黄灯三步法

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
- 原理：绝大多数"过五通/正常送料"的顶满会在 2~5s 内随打印机拉料而回落 → 自动恢复绿灯；只有**超过 5s 仍顶满**才确认真堵报红灯，误报率大幅下降。

**效果对照**：送料遇五通阻力、缓冲头顶满时亮**黄灯**（保护避让）；仅真堵亮**红灯**。LED 语义见 `docs/rgb_led_meaning.md`。

#### 上报版本名回退（AMS08）
v3.2 将 `bambu_bus_ams.cpp` 中 `long_packge_version_version_and_name_AMS08[]` 的型号名字段由 v3.1 的 `N3F05`（0x4E,0x33,0x46,0x30,0x35）**改回 `AMS08`**（0x41,0x4D,0x53,0x30,0x38）。原因：`N3F05` 虽能让 Studio 显示温湿度数值，但实测运行两次即被打印机拉黑；`AMS08` 不显示具体数值但稳定不被拉黑。上报号仍保持 `10.50`。如需 `N3F05` 显示数值方案，需另行解决被拉黑问题（不在本版本范围）。

#### 编译/验证
- 三种型号实编通过：双开关 DM=1（Flash ≈84.9%）、单开关 DM=0（≈80.2%）、双开关固定长度。
- `build_one.sh` 在 v3.2 修复了单开关 `AUTOLOAD=0` 时 `RETRACT` 被误覆盖为 `2.00_auto` 的脚本 bug（现单开关保留用户指定回抽长度）。

---

> 文档基于源码静态分析整理，覆盖功能、实现逻辑与通讯协议主干。具体字节级帧样例可参考 `bambu_bus_ams.cpp` 中 `// 3D C5 ...` 形式的抓包注释。
