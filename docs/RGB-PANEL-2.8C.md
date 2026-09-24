# 2.8C 圆屏（480×480 / ST7701 / RGB 并口）—— **实际生效的参数记录**

这一页记的是 **2026-09-24 真正点起来的那块板**：微雪 **ESP32-S3-LCD-2.8C（非触控，最终板）**，
以及当时**实际生效**在 `src/dash_display_rgb.cpp` 里的每一个数。

**为什么要单独记一页**：RGB 并口屏的引脚/初始化/时序**抄错一项就是黑屏且不报错**
（本项目在 2.1" 那族面板上就取到过另一套 `0xC1/0xC2` 与 16MHz 时序）。
把这些数、以及每个数的出处固定在**一处**，下次换板/换屏时才有对照物。

> ★ 权威仍是 **驱动源码**（`src/dash_display_rgb.cpp` 顶部第 1/2/3 块）；
> 本页是**快照 + 出处 + 复核方法**。两者对不上时以源码为准，并重跑下面那条复核脚本。

---

## 1. 板子与出处

| 项 | 值 |
|---|---|
| 板 | 微雪 **ESP32-S3-LCD-2.8C**（非触控版，SKU 30254） |
| 屏 | 2.8" 圆形 IPS **480×480**，控制器 **ST7701**，**只支持 RGB565**（`data_width=16`） |
| 主控 | ESP32-S3R8，16MB Flash + 8MB OPI PSRAM（实测 `psram: 8189 KB`） |
| 官方例程 | `https://files.waveshare.com/wiki/ESP32-S3-LCD-2.8C/ESP32-S3-LCD-2.8C-Demo.zip` → `Arduino/examples/LVGL_Arduino/Display_ST7701.{h,cpp}` |
| wiki | `https://docs.waveshare.com/ESP32-S3-LCD-2.8C` |

**触控版与非触控版的差别**：只差触摸（CST820）那一块。
`ESP32-S3-LCD-2.8C-Demo.zip`（非触控）与 `ESP32-S3-Touch-LCD-2.8C-Demo.zip`（触控）里的
`Display_ST7701.cpp` 的 ST7701 上电序列**逐条相同**（两份 diff 过：IDENTICAL）
⇒ **屏这一段按面板走、不按板子走**。
（非触控例程的 `ESP_PANEL_LCD_RGB_FRAME_BUF_NUM` 是 1、触控版是 2 —— 那是例程里 S3 侧帧缓冲
数量的差别。★ 2026-09-24 换栈之后本项目也走上了同一条路：`num_fbs = 2`，见第 9 节；
而「旧 esp_lcd 里 `num_fbs` 根本不存在」这件事记在第 5 节 —— **那一节写的是换栈前的旧口径**。）

---

## 2. 实际生效的引脚（`src/dash_display_rgb.cpp` 第 1 块）

出处：例程 `Display_ST7701.h` 的 `ESP_PANEL_LCD_PIN_NUM_RGB_*` 与 `LCD_{CLK,MOSI,Backlight}_PIN`。

| 功能 | GPIO | 备注 |
|---|---|---|
| PCLK | 41 | |
| DE | 40 | |
| VSYNC | 39 | |
| HSYNC | 38 | |
| DATA0..DATA15 | 5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17 | 顺序**照抄**，见下 |
| 3 线 SPI：SDA(MOSI) | 1 | 只用于写初始化命令 |
| 3 线 SPI：SCLK | 2 | |
| 背光 LCD_BL | 6 | LEDC：20kHz / 10 位 / 50% |
| I2C SDA / SCL | 15 / 7 | 板上共用（TCA9554 / QMI8658 / PCF85063） |
| LCD_RST | **EXIO1**（TCA9554，非 GPIO） | 低 10ms → 高 → 等 50ms |
| LCD_CS | **EXIO3**（TCA9554，非 GPIO） | 写命令期间拉低；写完拉高 |
| 蜂鸣器 | **EXIO8**（TCA9554） | 上电拉低（关） |

★ **数据线的接线形状**：`data_gpio_nums[i]` 是 RGB565 的 **bit i**，
面板那头接的是 **B1..B5 / G0..G5 / R1..R5** —— 即面板的 **B0 与 R0 不接（NC）**。
**这不是错位，是这块板的实际走线**：照抄例程的顺序，别去"补齐"那两根。

★ TCA9554：地址 `0x20`，寄存器 `0x01`=输出 / `0x03`=方向（0=输出）；
EXIO 编号按**位**算（EXIO1=bit0、EXIO3=bit2、EXIO8=bit7）。
`RST`/`CS` 在扩展芯片上而不是 GPIO 上，是这块板最容易漏的一步 ——
漏了的症状与"初始化序列抄错"**完全一样**（黑屏、串口一切正常）。

---

## 3. 实际生效的时序（第 3 块）

出处：例程 `Display_ST7701.h` 的 `ESP_PANEL_LCD_RGB_TIMING_*` 与 `Display_ST7701.cpp` 的 `rgb_config`。

| 参数 | 值 | 例程宏 |
|---|---|---|
| `pclk_hz` | **18 MHz**（★ 见下，不是例程的 30MHz） | `ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ`(Arduino) / `EXAMPLE_LCD_PIXEL_CLOCK_HZ`(**ESP-IDF**) |
| `hsync_pulse_width` (HPW) | 8 | `..._HPW` |
| `hsync_back_porch` (HBP) | 10 | `..._HBP` |
| `hsync_front_porch` (HFP) | 50 | `..._HFP` |
| `vsync_pulse_width` (VPW) | **2** | `..._VPW` |
| `vsync_back_porch` (VBP) | **18** | `..._VBP` |
| `vsync_front_porch` (VFP) | 8 | `..._VFP` |
| `flags.hsync_idle_low` | 0 | 例程 `.flags` |
| `flags.vsync_idle_low` | 0 | 例程 `.flags` |
| `flags.pclk_active_neg` | 0(false) | 例程 `.flags` |
| `clk_src` | **`LCD_CLK_SRC_PLL160M`** | 18MHz 走 PLL160M（160/18≈8.89，分频器带小数部分） |

**理论帧率**：`18e6 / ((480+8+10+50) × (480+2+18+8)) = 18e6/548/508 ≈ 64.7 Hz`
⇒ 串口上 `rgb: frames=…(+N/s)` 的 N 应当是这个量级。实测 **+64~+67/s**（第 6 节）。

**★ PCLK 为什么是 18MHz 而不是 30MHz**（这一档在**换栈前后都实测过**：换栈前的理由见第 5 节，
换栈后的两次实测数字与最终取值见 **9.4**）：

---

## 4. 实际生效的初始化序列（第 2 块，41 步）

出处：例程 `Display_ST7701.cpp` 的 `ST7701_Init()`，**逐条照抄，顺序与延时都没动**。
下面只是便于人眼核对的快照；**权威在源码里**。

```
页: FF 77 01 00 00 13
EF = 08
页: FF 77 01 00 00 10
C0 = 3B 00                       ; Scan line
C1 = 10 0C                       ; VBP
C2 = 07 0A                       ; VFP
C7 = 00
CC = 10
CD = 08                          ; RGB format
B0 = 05 12 98 0E 0F 07 07 09 09 23 05 52 0F 67 2C 11     ; IPS
B1 = 0B 11 97 0C 12 06 06 08 08 22 03 51 11 66 2B 0F     ; IPS
页: FF 77 01 00 00 11
B0 = 5D                          ; VOP
B1 = 3E                          ; VCOM amplitude
B2 = 81                          ; VGH 12V
B3 = 80
B5 = 4E                          ; VGL
B7 = 85
B8 = 20
C1 = 78
C2 = 78
D0 = 88
E0 = 00 00 02
E1 = 06 30 08 30 05 30 07 30 00 33 33
E2 = 11 11 33 33 F4 00 00 00 F4 00 00 00
E3 = 00 00 11 11
E4 = 44 44
E5 = 0D F5 30 F0 0F F7 30 F0 09 F1 30 F0 0B F3 30 F0
E6 = 00 00 11 11
E7 = 44 44
E8 = 0C F4 30 F0 0E F6 30 F0 08 F0 30 F0 0A F2 30 F0
E9 = 36 01
EB = 00 01 E4 E4 44 88 40
ED = FF 10 AF 76 54 2B CF FF FF FC B2 45 67 FA 01 FF
EF = 08 08 08 45 3F 54
页: FF 77 01 00 00 00
11                               ; SLPOUT —— 之后 **delay 120ms**
3A = 66                          ; COLMOD
36 = 00                          ; MADCTL
35 = 00                          ; TEON
29                               ; DISPON
```

三个最容易抄串的地方（都与 2.1" 那族面板**不同**）：

1. **入口页顺序**：开头先 `页0x13` → `EF=08` → 再回 `页0x10` 写电源/伽马。
   顺序错了后面整段都落错页 ⇒ 屏黑，而串口一切正常。
2. **`C1=10 0C` / `C2=07 0A`** 是这块面板的 VBP/VFP 组
   （2.1" 是 `0B 02` / `07 02`）—— 两套数别混用。
3. **结尾是 `3A=66 → 36=00 → 35=00 → 29`**：**没有** `0x20`(INVOFF) 那一步（2.1" 才有），
   而多一步 **`0x35`(TEON)**；`0x11` 之后是 **120ms**（2.1" 是 480ms）。

### 复核脚本（把驱动表与官方例程逐条对账）

```powershell
# 解出驱动里的 kPanelInit 与例程里的 ST7701_WriteCommand/WriteData 序列，逐条比对
# 2026-09-24 跑出来：41 步，命令/数据/顺序/延时全同。
```
（脚本见本节 PR 的回报；要点是：驱动表用
`\{\s*(0x..)\s*,\s*\{([^}]*)\}\s*,\s*(\d+)\s*,\s*(\d+)\s*\}` 抽，
例程按 `WriteCommand/WriteData/delay` 抽，然后逐行 `-ne` 比对。
顺带会校验 `len` 与花括号里的字节数一致 —— 这两者不一致时驱动会**少发字节**而不会报错。）
时序那一组也一起对：`RGB_*` 宏 vs `ESP_PANEL_LCD_RGB_TIMING_*`（7 项）+ 数据线顺序（16 项）。

---

## 5. 这一版 esp_lcd 的两个"静默失败"（都踩过，别再踩）

> ★ **本节记的是换栈前的旧口径**（arduino-esp32 2.0.17 / IDF 4.4 系）。2026-09-24 第二轮
> 已把 `[env:esp32s3-rgb]` 换到 IDF 5.5（见第 9 节）⇒ 这两个坑本身**仍然成立**
> （少 `esp_lcd_panel_init()` 就没帧、绘制缓冲没对齐就静默不装），但「头文件只有 129 行」
> 那句只对旧栈成立。

这一版框架是 **arduino-esp32 2.0.17（ESP-IDF 4.4 系）**，
`esp_lcd_panel_rgb.h` 只有 129 行：**没有** `num_fbs` / `bounce_buffer_size_px` /
`esp_lcd_rgb_panel_get_frame_buffer()` / `register_event_callbacks()`。
两个坑的共同点是 **不报错、只留一个数字**：

### ① 少了 `esp_lcd_panel_init()` ⇒ `frames=0`、屏全黑

`esp_lcd_new_rgb_panel()` 只是把面板对象建起来，**真正开 DMA / 启动 LCD_CAM 连续扫描的是**
`esp_lcd_panel_init()`（`esp_lcd_panel_reset()` 对 RGB 面板是空操作）。
漏了它：串口一切正常、"已就绪"照打、`draw_bitmap()` 也照抄进 fb，但 **`rgb: frames=0(+0/s)` 永远是 0**。
⇒ 那行每秒一次的 `frames=` 就是为这个留的判据。

### ② 绘制缓冲没按 `LV_DRAW_BUF_ALIGN` 对齐 ⇒ `flush=0`、屏全黑

LVGL 9.3 的 `lv_display_set_buffers()` 会先校验 `buf1 == lv_draw_buf_align(buf1)`
（即按 `LV_DRAW_BUF_ALIGN = 4` 对齐），**不满足就静默 `return`**（`LV_USE_LOG=0`，一行警告都没有）
⇒ 这条缓冲根本没装上。症状是四个"看起来都对"的现象同时成立：

* 面板在扫描（`frames` +108/s，和理论值分毫不差）；
* UI 树齐全、LVGL 堆还剩 26KB；
* 主循环 / `lv_tick_inc` / `lv_timer_handler()` 全都在跑；
* 但 **`rgb: flush=` 永远不涨**、屏全黑。

`static lv_color_t buf[...]` 只保证 **2 字节**对齐（`lv_color_t` = `uint16_t`），实测 `misalign=1`。
⇒ 驱动里那句 `__attribute__((aligned(LV_DRAW_BUF_ALIGN)))` 是**必须**的。
（同一类坑在桩驱动 `src/dash_display.cpp` 的 `buf_left/buf_right` 上同样成立 ——
那份是 esp32dev / 抓帧盒在用的构建，**本轮没动**，见 ACCEPTANCE 里记的"未做的项"。）

---

## 6. 2026-09-24 实测数字（`[env:esp32s3-rgb]` + COM6）

> ★ **本节是"换栈之前"那一轮（单 framebuffer）的数字**，`rgb: frames=` / `sync=` / `chunk=`
> 那几行日志**在新驱动里已经没有了**（换成 `vsync=` / `swap=` / `timeout=`）。
> 要看**当前**口径的实测数字，直接跳 **第 9 节**（9.3 是数字表、9.6 是复核方法）。

开机横幅与自检（**原始串口输出，未改写**）：

```
206 dash ok
--- 自检(上电)---
chip  : ESP32-S3 rev0, 2 核 @ 240 MHz
flash : 16 MB
psram : 8189 KB 可用 / 8189 KB 总
heap  : 223 KB
image : 分区 8192 KB @ 0x254000(编译期口径 8192 KB)
obd: 未启用(-DOBD_SERIAL=0),只跑 Sim 假数据
rgb: RGB565 480x480 pclk=30000000Hz 数据位=16 已就绪(第二块屏待接)
rgb: 板=微雪 ESP32-S3-LCD-2.8C(非触控) ST7701 RST=EXIO1 CS=EXIO3 BL=GPIO6/PWM20000 @50%
```

稳定后每秒两行（节选）：

```
BEACON  5  step=6(loop: 刚开始一轮)  uptime=5s heap=219KB psram=8189KB flash=16MB
rgb: frames=574(+108/s) flush=187
```

| 指标 | 实测 | 说明 |
|---|---|---|
| 面板扫描帧率 | **+64~+67 /s** | 与理论 64.7 Hz（18MHz）相符 ⇒ 面板确实在收帧 |
| LVGL flush 速率 | ~20 /s 稳态 | 局部刷新：UI 每 200ms 重画一次 + 每秒读数 |
| 写 fb 的单块耗时 | `copy_avg ≈ 200~240µs`、`copy_max = 561µs` | **全部 < 消隐期 0.85ms** ⇒ 不越界 |
| 写 fb 的同步 | `sync == chunk`、`timeout = 0` | **每一次**写 fb 都等到了"帧结束"，没有一次靠超时放行 |
| heap（setup 后 / 稳态） | **223 KB / 219 KB** | ⚠ 当前日志是 **KB 截断**（`/1024`），不是整 KB 精度 |
| PSRAM | 8189 KB 可用 / 8189 KB 总 | 双 480×480 全缓冲（2×450KB）在这里**绰绰有余** |
| LVGL 自己的堆 | 26,264 / 45,860 字节可用 | `LV_MEM_SIZE` 默认 48KB |
| CPU | 240 MHz，2 核 | |

### 「留白」观感（**这条还没做**）

选型当初的判据是"塞进仪表盘 Ø89 的孔之后每边留多少"：
**2.8\" 可视 Ø70.1 ⇒ 约 9mm/边**（`PURCHASE.md` 第五节那一栏）。
本轮**没有**对着 Ø89 的表壳实际比过 ⇒ **观感这一条仍是未测**，别把它当结论用。
（机械尺寸另有一条 2026-09-24 的更正，见 `PURCHASE.md`。）

---

## 7. 构建开关：为什么这个 env 必须带这两条

```
[env:esp32s3-rgb]  (extends env:esp32s3)
  -UDASH_DISPLAY_STUB   把桩驱动关掉
  -DDASH_DISPLAY_RGB=1  打开真驱动
  -ULINK_PHY_UART       ← 见下
  -DOBD_SERIAL=0        ← 见下
```

* **`-ULINK_PHY_UART`**：基环境带着 `-DLINK_PHY_UART=1`，而 `dash_log.h` 见到它就把
  `DASH_LOG_UART0` 定为 0（日志只走原生 USB-CDC）。可是 2.8C 上 **UART0(43/44)= 板载 CH343P
  = 唯一的 Type-C 口**，原生 USB(GPIO19/20)只引到 12PIN 排针上 —— **没有杜邦线时够不着**。
  于是"日志只走 USB-CDC"= 串口一片空白，而板子跑得好好的。
  去掉它之后，COM 口上同时有：开机自检 / 每秒 `frames=` / **以及回放输入**
  （`van_replay_poll()` 读的正是 `Serial0`）。
  ★ 代价：这份构建里 **43/44 是"日志+回放"口，不是链路**。真要在这种板子上跑双板链路，
  得先把日志挪走（原生 USB 从 12PIN 引出来，或换 `-DLINK_UART_PORT`）。
* **`-DOBD_SERIAL=0`**：OBD 默认脚是 **RX=17 / TX=18**，而这两根正是 RGB 的
  **DATA15/DATA14**。`attachObdSerial()` 在 `dash_display_init()` **之前**跑，
  于是 Serial1 先抓走 17/18，LCD 矩阵再抢回去（最后写的赢）—— 屏能亮，但 OBD 那一路
  静默失效。`main.cpp` 里本来就有这条开关（"没有 OBD 硬件时编译加 `-DOBD_SERIAL=0`"）。

---

## 8. 没有 VAN 收发器时怎么把数据喂上屏（PC 中继）

2.8C 的数据层支持**从串口贴回放行**（`van_replay_poll()`，见 `lib/dashcore/van_replay.h`），
于是"没有收发器/没有抓帧盒"也能把整条链跑通：

```powershell
# ① 文件源（今天可用）：仓库现成的回放样本 → 2.8C 的回放口
.\tools\serial-capture\relay.ps1 -From .\tools\serial-capture\sample-log.txt -To COM6 -ReplayLinesOnly -Loop -LineDelayMs 1500 -Seconds 45

# ② 串口源（裸 S3 抓帧盒在场时）：A 口 → B 口
.\tools\serial-capture\relay.ps1 -From COM7 -To COM6 -ReplayLinesOnly
```

★★ **一个必须知道的格式坑**：设备和抓帧盒自己打出来的帧行**不能直接回放** ——
它长这样：`VAN 824 18 F8 27 10 00 00 00   # cmd=1 ack=0`，
而 `parseVanReplayLine()` 扫到 `#` 就判错、设备回显 `VAN? …`、数据层一帧都收不到。
`relay.ps1` 的 `-ReplayLinesOnly` 就是干这个的：只挑 `VAN <hex>` 开头的行并砍掉 `#` 之后那段。
（**"抓帧日志"与"回放输入"差一个后缀** —— 2026-09-24 第一次中继实测 30 行全被判错。）

回放行格式与字段口径见 `lib/dashcore/van_replay.h` / `van_source.h`
（`IDEN 0x824`：`data[0..1]` 大端 = 转速×8；`data[2]` **单字节** = 车速计数，`×2.56 km/h`）。
---

## 9. 缓冲与**扫描同步**策略（2026-09-24 第二轮：**已实施**双 framebuffer + vsync 换帧 ⇒ 撕裂根治）

> **本节的历史**：同日早先这一节记的是"**方案**" —— 那时这份 esp_lcd 是旧的
> （官方 espressif32 7.1.3 = arduino-esp32 **2.0.17** / IDF 4.4 系）：**没有** `num_fbs`、
> **没有** vsync 回调、**没有** bounce buffer ⇒ 只有一块 framebuffer，只能"一边扫描一边往里写"
> ⇒ **撕裂是结构性的**（当时的实测与修法见 `ACCEPTANCE.md`「2.8C 圆屏：点亮、"整屏花屏"与"撕裂"的
> 根因与修法」）。同日**晚些时候已按方案换栈实施并上板实测** ⇒ 下面是**实施后的实际口径**；
> 旧的"单 fb + 8KB 定额分块 + 每块等消隐期"那套代码已从 `src/dash_display_rgb.cpp` 里**删掉**。

### 9.1 换了什么（**只动一条 env**）

| 项 | 旧（同日 早先） | 现在 |
|---|---|---|
| 平台 | 官方 `espressif32@7.1.3` = arduino-esp32 **2.0.17** / IDF 4.4 系 | **pioarduino 的 espressif32 55.03.39** = arduino-esp32 **3.3.9** / **ESP-IDF 5.5.4** |
| 换到哪条 env | —— | **只有 `[env:esp32s3-rgb]`**：`platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip` |
| 抓帧盒 env | `[env:esp32s3]`：`platform = espressif32` | **平台与行为一个字没变**，只把那一行钉成 `espressif32@7.1.3`（**就是它一直在用的那一版**）。★ 必须钉：PlatformIO 对不带版本的 `espressif32` 取"已安装里**版本号最高**的那个"（`package/manager/base.py`:`get_package`）⇒ 不钉就会被**静默换成 3.x** |
| framebuffer | **1** 块（450KB，PSRAM） | **2** 块（`cfg.num_fbs = 2`，各 450KB，PSRAM） |
| 扫描同步信号 | `cfg.on_frame_trans_done`（单回调，"帧结束"） | **`esp_lcd_rgb_panel_register_event_callbacks()` 的 `on_vsync`**（每帧一次中断） |
| flush 落点 | `esp_lcd_panel_draw_bitmap()` → memcpy 进**正在扫描**的那块 fb | memcpy 进 **back**（不在扫描的那块）；一次刷新的**最后一块**再换帧 |
| 换帧 | 没有（⇒ 结构性撕裂） | `esp_lcd_panel_draw_bitmap(panel, 0,0,W,1, back)`：驱动只改 `cur_fb_index` + 重串 DMA 链表 ⇒ **下一个帧边界整块换过去** |
| bounce buffer | 没有（头文件里没这个字段） | 仍然**不用**（见 9.5 的取舍：双 fb + bounce buffer 要每帧全屏 CPU 拷贝，得不偿失） |
| 整屏刷新 | 8KB 一块 × 每块等一个消隐期 ≈ **0.85~1.0 秒** | **一次拷 450KB** ⇒ 实测 **56.6ms** |
| 局部刷新 | 每次 flush 等一帧 + 分块 | 只拷脏区、不等（稳态 `copy_avg ≈ 0.9ms`） |

### 9.2 为什么这样就无撕裂（代码级，可复核）

全部在 `src/dash_display_rgb.cpp`：

1. 建面板：`cfg.num_fbs = 2`、`cfg.flags.fb_in_psram = 1`（**旧栈里这两个字段根本不存在**）；
2. `esp_lcd_panel_init()` 之后 `esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, &fb0, &fb1)`
   取两块 fb 的地址（实测 `fb0=0x3c0d1b20` / `fb1=0x3c142340`，正好差 450KB）；
3. `rgb_flush_cb()` **只往 back 写**：`blit_area()` = memcpy 脏区（整宽一次拷、窄区按行拷）
   + `esp_cache_msync(..., C2M | UNALIGNED)`。★ 那记 cache 回写**不能省**：fb 在 PSRAM、
   在 cache 后面（驱动自己拷贝完也做同一件事）；
4. 一次刷新的最后一块（`lv_display_flush_is_last()`）调 `request_swap()`：
   把 **back 的地址**交给 `esp_lcd_panel_draw_bitmap(panel, 0, 0, W, 1, fb_back)`。
   IDF 5.5 的 `rgb_panel_draw_bitmap()` 见到"传进来的指针落在某块 fb 范围内"就走
   `draw_buf_copy_to_fb = false` 那一支：**它不拷贝**，只把 `cur_fb_index` 指过去，
   并在 stream_mode 下把 DMA 的帧缓冲链表重串到新 fb ⇒ **在下一个帧边界（消隐期）
   整块换过去**，屏幕上任何时刻都只有"上一幅"或"下一幅"，不存在半新半旧。
   （坐标给 `(0,0,W,1)` 而不是整屏，是为了让那一支里驱动自己做的那次 cache 回写
   只覆盖一行 —— 真正写过的区域我们在第 3 步已经回写过了。）
5. **两块 fb 的一致性**（否则换过去会看到上一轮的残影）：`g_fb_complete[i]` 记"这块 fb 里
   是不是一整幅完整画面"；往一块还不完整的 fb 上画之前，先把完整的那块**整块拷过来**
   （450KB，**开机后只发生一次** —— 串口上的判据是 `msync=1` 且不再涨）。之后每块 fb 都等于
   "上一幅完整画面"，局部刷新叠上去自然就是新的完整画面。稳态下**不做任何整块拷贝**。
6. `on_vsync` 里**只做一件事：计数**（`vsync=`）。它同时是两件事的判据 ——
   "面板确实在收帧"（≈65/s @18MHz）与"换帧那个边界过去了没有"。

★ **换帧之后为什么还要等一下**：`request_swap()` 是**立刻**改 `cur_fb_index` 的，但 DMA 要到
**下一个帧边界**才真的换过去 —— 那之前旧的**还在被扫**；这时若往"新的 back（= 刚被换下去的那块）"
里写，那一笔就落在正在扫描的显存上 ⇒ 又会出现一条缝。所以每次 flush 动手前先过
`wait_swap_settled()`：等 `on_vsync` 计数越过换帧时的计数（**60ms 兜底 + `timeout` 计数**，
绝不死等 —— 死在这里的后果是整屏再也不更新）。稳态下这个门**一次都不阻塞**（一次比较就过；
实测 `swap_wait_max = 8.9ms < 一帧 15.5ms`，只有开机动画那种 50Hz 刷新才可能等到）。

### 9.3 实测数字（`[env:esp32s3-rgb]` + COM6；下面都是**原始串口行**）

开机自检与面板就绪：

```
psram : 8187 KB 可用 / 8192 KB 总
heap  : 205 KB
rgb: RGB565 480x480 pclk=18000000Hz 数据位=16 已就绪(第二块屏待接)
rgb: 双framebuffer num_fbs=2 fb0=0x3c0d1b20 fb1=0x3c142340(各 450KB PSRAM) on_vsync=已注册 ⇒ vsync 边界换帧(无撕裂)
rgb: 双fb 之后 空闲 PSRAM=7285KB heap=200KB(内部)
rgb: 整屏刷新 56.6ms(块=8 拷贝20.0ms 数据450KB) 换帧在下一个vsync
boot anim done (收尾补一次 boot_apply: faceStage=1 → 表情 opa=COVER)
```

稳定后每秒那行（节选，`fb=` 在 `1/0`↔`0/1` 之间翻 = **真的在换帧**）：

```
rgb: vsync=292(+65/s) swap=51(+5/s) flush=172 copy_max=3012us copy_avg=881us swap_wait_max=8875us timeout=0 fb=1/0 msync=1
rgb: vsync=357(+65/s) swap=56(+5/s) flush=194 copy_max=3012us copy_avg=897us swap_wait_max=8875us timeout=0 fb=0/1 msync=1
rgb: vsync=422(+65/s) swap=61(+5/s) flush=214 copy_max=3012us copy_avg=887us swap_wait_max=8875us timeout=0 fb=1/0 msync=1
```

| 指标 | 实测 | 说明 / 判据 |
|---|---|---|
| 面板扫描帧率 | **+64~+65 /s** | 18e6/(548×508) = **64.7Hz** ⇒ PCLK 真是 18MHz（这行同时是"PCLK 跑成多少"的判据：30MHz 时它是 +108/s） |
| 换帧次数 | 稳态 **+5 /s** | 与 UI 每 200ms 重画一次对得上；**每一次刷新都换了一次 fb** |
| **`timeout`** | **0**（全程） | 等"换帧边界过去"从来没有靠超时放行 ⇒ 换帧的同步是**真的**在起作用 |
| `msync` | **1**（不再涨） | 整块补拷只发生在开机后第一次换帧之后 —— 正是设计里那一处 |
| 写 fb 的耗时 | `copy_max = 3012µs`、`copy_avg ≈ 0.9ms` | 单次 flush 的脏区拷贝（含 cache 回写）；`copy_max` 那 3ms 是开机那次整块补拷 |
| 换帧等待 | `swap_wait_max = 8875µs` | < 一帧（15.5ms）⇒ 与设计一致 |
| **开机整屏刷新** | **56.6ms**（块=8，拷贝 20.0ms） | 旧的"8KB 分块 + 每块等消隐期"是 **≈0.85~1.0 秒** ⇒ 快 **~15 倍**，而且**不再需要**那套节流 |
| 双 fb 的代价 | 空闲 PSRAM 从 8187KB → **7285KB** | 2×450KB = 900KB，与账算得一分不差 |
| heap（内部） | 205KB（自检）/ 200KB（稳态） | 旧栈是 223/219KB ⇒ 新栈多占约 20KB（IDF 5.5 的驱动更大），仍有 200KB 余量 |
| 表情 | `boot anim done (… faceStage=1 → 表情 opa=COVER)` + 每秒行里 5 档都在转（`idle/cruise/sport/high/redline`） | 那个"收尾补一次 `boot_apply`"的兜底**照旧保留**（它是"动画状态机不能被主循环卡顿跳过"的正确兜底） |

### 9.4 PCLK：最终定 **18MHz**（30MHz 也实测了，数字在下面）

| PCLK | 面板帧率（实测） | 开机整屏刷新 | 单次脏区拷贝 | `timeout` |
|---|---|---|---|---|
| **18MHz（定案）** | +64~65/s | **56.6ms**（拷贝 20.0ms） | `copy_avg ≈ 0.9ms`、`copy_max 3012µs` | 0 |
| 30MHz（试过） | **+107~108/s**（30e6/548/508 = 107.8 ⇒ 分频器真的凑出了 30MHz） | 64.8ms（拷贝 **27.2ms**） | `copy_avg ≈ 1.1ms`、`copy_max 3744µs` | 0 |

⇒ **定 18MHz**，理由（两条，都是实测）：
1. 30MHz 下**同一块 fb 的拷贝耗时反而涨了 35%**（19.9→27.2ms）—— 那正是 PSRAM 带宽争用的信号：
   DMA 每帧要读 60MB/s，CPU 再往里写就更挤；而本栈**没开 bounce buffer**（见 9.5），
   欠载（FIFO underrun ⇒ 那帧花屏）正是上一轮在 30MHz 上踩过的坑。
2. 18MHz 这一档的收益（整屏刷新 56.6ms）已经满足"几十毫秒"的目标，而且 `timeout=0`、
   帧率与理论值分毫不差 ⇒ **没有理由为了 +43Hz 的扫描率去冒欠载的风险**。
   ★ 要试 30MHz 只改一个数：`src/dash_display_rgb.cpp` 的 `RGB_PIXEL_CLOCK_HZ`
   （`clk_src` 不用动：`LCD_CLK_SRC_PLL160M` 带小数分频，160/30 也凑得出 30.0MHz ——
   旧注释里"30MHz 必须换 PLL240M"那句是**旧驱动**的口径，已作废）。

### 9.5 为什么不干脆开 bounce buffer（把 30MHz 也吃下来）

`bounce_buffer_size_px` 确实能让 DMA 从**内部 SRAM** 取像素、彻底不争 PSRAM —— 但它与
"双 fb + 局部刷新"是**互相排斥**的两条路：开了 bounce buffer，驱动要在每帧的有效像素期间
由 CPU 把 fb 的内容**一块一块搬进 bounce buffer**（`on_bounce_empty` 那条路）——
以 30MHz、480×480×2B 算就是每帧 450KB 的 CPU 拷贝（≈60MB/s），CPU 直接被吃光。
⇒ 本项目的取舍是：**要"无撕裂"就上双 fb（本方案），要"高 PCLK"才需要 bounce buffer，
两者不必兼得**（18MHz 已经够用）。

### 9.6 怎么复核（换板/换屏/下次动这个驱动时照这条走）

```powershell
# ① 新平台的工具链（首次会下载 arduino-esp32 3.3.9 + IDF 5.5.4，约 1GB；只在第一次）
& 'C:\.platformio\penv\Scripts\platformio.exe' run -e esp32s3-rgb

# ② 只烧 COM6（★ 绝不烧 COM7：那是抓帧盒）
& 'C:\.platformio\penv\Scripts\platformio.exe' run -e esp32s3-rgb -t upload --upload-port COM6

# ③ 从复位那一刻抓 20 秒串口（零依赖，只用 .NET 的 System.IO.Ports）
powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-boot-nopy.ps1 -Port COM6 -Seconds 20
```

看这 6 个数就够判"撕裂有没有被根治"：

| 看什么 | 期望 | 不对时说明什么 |
|---|---|---|
| `双framebuffer num_fbs=2 fb0=… fb1=…` | 两行都有地址、差 450KB | 只有一块 ⇒ `num_fbs` 没生效（回到单 fb，撕裂必然回来） |
| `pclk=…` 与 `vsync=…(+N/s)` | 18MHz + **+64~65/s** | PCLK 与帧率对不上 ⇒ 时序/分频没配好 |
| `timeout=` | **恒为 0** | 涨 ⇒ 换帧的边界没等到（面板被停 / 中断没来），画面会重新出现缝 |
| `fb=` | 在 `1/0`↔`0/1` **来回翻** | 一直同一个值 ⇒ 换帧**没生效**（屏上会是静止/不更新） |
| `整屏刷新 …ms` | **几十毫秒** | 回到 ~1 秒 ⇒ 又走回"分块 + 等消隐期"那条老路 |
| `msync=` | 停在 **1** | 一直涨 ⇒ 每帧都在整块补拷（一致性逻辑被打破了，性能会塌） |

### 9.7 这套做法的边界（写清楚，别指望它包打天下）

* **双 fb 在 PSRAM ≠ 不占带宽**：DMA 每帧从 PSRAM 读 480×480×2B×64.7Hz ≈ **30MB/s**（18MHz；
  30MHz 时 60MB/s），CPU 往 back 写再占一份 ⇒ 这也是为什么 30MHz 那一档的拷贝耗时反而更高。
  真要把 PCLK 拉到 30MHz 以上，就得走 9.5 里说的 bounce buffer，而那样就不能要双 fb。
* **换帧延迟最坏 1 帧**（18MHz 15.5ms / 30MHz 9.3ms）：肉眼不可见，但"按键到画面变"那条链上
  要把它算进去（我们的 UI 是 200ms 档，无影响）。
* **两屏共用一条总线**那件事**仍未做**：本文件现在还是单屏版本（`g_left = g_right = d0`），
  第二块屏的两条出路见文件头。★ 但换栈之后**那条硬阻塞没了**：`num_fbs` / `get_frame_buffer` /
  帧切换回调三样现在都有了 ⇒ 将来做双屏"轮流发帧"时，本文件这套 back/换帧结构可以直接复用。
