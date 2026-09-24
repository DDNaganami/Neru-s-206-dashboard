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
| `msync` | **1**（不再涨） | 整块补拷只发生在开机后第一次换帧之后 —— 正是设计里那一处（★ 第四轮起这一列改名 `catchup=`，见 11.4） |
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

> ★★ **2026-09-24 第三轮更正：这一节当时的结论是错的，别照它做**（原文留在下面当记录）：
> ① `bounce_buffer_size_px` 与"双 fb"**不互斥**。`num_fbs > 0` 时搬数据的是**驱动自己** ——
>    `esp_lcd_panel_rgb.c` 的 `lcd_rgb_panel_fill_bounce_buffer()` 里那句
>    `memcpy(buffer, &panel->fbs[panel->bb_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size)`；
>    需要用户 `on_bounce_empty` 的只有 `no_fb`（Bounce Buffer Only）那条路。
>    微雪官方例程的 `panel_config` 里 `.num_fbs` 与 `.bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES`
>    就是**同时**写着的（见 10.9）。
> ② **恰恰相反：不带 bounce 的"双 fb"在这个栈里换帧根本不生效**（逐行原因见 10.2）——
>    bounce 不是"另一条路"，而是**这条路能走通的前提**。
> ③ 代价那一半当年算对了：CPU 每帧要搬 450KB（实测 `bounce=28~31MB/s`，见 10.5）。
>    这笔代价**已经付了**，换来的是"换帧真的落在帧边界"。
> ④ 当年把 30MHz 的账也算在这里 —— 现在 DMA 不再抢 PSRAM，30MHz 反而更可试了；
>    但本轮**没动 PCLK**（先跟官方 Demo 走：18MHz）。

（以下是 2026-09-24 第二轮的原文，仅作记录）

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

---

## 10. 2026-09-24 第三轮：**"图像残留 + 撕裂"的真根因 = 少了 bounce buffer**（已修 + 已上板实测）

### 10.1 车主反馈与第一假设（记录在案：它解释得通，但不是根因）

上一轮（第 9 节）上了"双 framebuffer + `on_vsync` 换帧"之后，实机是：

* **撕裂还在** ✗；
* **新出现"图像残留"** ✗✗（旧画面留在屏上，区域边界看着像撕裂）。

当时的第一假设是"双缓冲 + LVGL 局部刷新 ⇒ 后台缓冲半新半旧"（`msync=1` + `swap +5/s` 像是它的签名）。
**这条假设对现象的描述是对的，但根因不是它** —— 见下一条。

### 10.2 真根因：这个栈里"换帧"**根本没生效**（IDF 5.5.4 逐行证据）

三处代码，缺一不可（都在 `components/esp_lcd/rgb/esp_lcd_panel_rgb.c`；
本栈的 IDF 版本由 `framework-arduinoespressif32-libs/esp32s3/versions.txt` 钉死为 **v5.5.4**，
我按 `v5.5` 与 `release/v5.5` 两个 tag 都核过，这一段两版相同）：

1. **ESP32-S3 上 `RGB_LCD_NEEDS_SEPARATE_RESTART_LINK = 1`**（文件开头的硬件规避），驱动因此建一条
   **专用 restart link**，它的第 0 个节点**固定挂在 `fbs[0]`** 上：

   ```c
   // lcd_rgb_panel_init_trans_link()（只在 init 里挂这一次）
   gdma_buffer_mount_config_t restart_buffer_mount_cfg = {
       .buffer = rgb_panel->fbs[0] + restart_skip_bytes,   // ★ 固定 fbs[0]
       ...
   };
   gdma_link_concat(rgb_panel->dma_restart_link, 0, rgb_panel->dma_fb_links[0], 1);
   ```

   ★ 而 `cur_fb_index` 变了之后，**没有任何代码去重挂它**（`rgb_panel_draw_bitmap()` 里只对
   `dma_fb_links[]` 做 `gdma_link_concat`，碰不到 restart link）。

2. **这版 arduino-esp32 给 esp32s3 的 sdkconfig 里 `CONFIG_LCD_RGB_RESTART_IN_VSYNC=1`**
   （可直接读 `framework-arduinoespressif32-libs/esp32s3/qio_opi/include/sdkconfig.h` ——
   我读到的就是 `#define CONFIG_LCD_RGB_RESTART_IN_VSYNC 1`），于是**每个 VSYNC 中断**都走这一支：

   ```c
   // lcd_rgb_panel_try_restart_transmission()
   #if CONFIG_LCD_RGB_RESTART_IN_VSYNC
       do_restart = true;                       // ← 每帧无条件重启
   #else
       ...（need_restart / bb_eof_count 那套）
   #endif
       ...
       lcd_ll_fifo_reset(rgb_panel->hal.dev);
       gdma_reset(rgb_panel->dma_chan);
   #if RGB_LCD_NEEDS_SEPARATE_RESTART_LINK
       gdma_start(rgb_panel->dma_chan, gdma_link_get_head_addr(rgb_panel->dma_restart_link));  // ★ 又回 fbs[0]
   #else
       gdma_start(..., gdma_link_get_head_addr(rgb_panel->dma_fb_links[rgb_panel->cur_fb_index]));
   #endif
   ```

3. ⇒ **没有 bounce buffer 时，每一帧的 DMA 都从 `fbs[0]` 开始吐数据 ⇒ 屏上永远是 `fbs[0]`。**
   我们那次"换帧"（`esp_lcd_panel_draw_bitmap(panel, 0,0,W,1, g_fb[g_back])`）只改了
   `cur_fb_index` 这个变量，**对显示内容毫无影响**。两个症状就是这么来的：

   | 症状 | 机制 |
   |---|---|
   | **图像残留** ✗✗ | 我们"往 back 画"的那些刷新全画进了**不上屏的那块 fb**；LVGL 不会重画同一块区域 ⇒ 那块内容在屏上**永远是旧的** |
   | **撕裂** ✗ | `back` 恰好等于 `fbs[0]` 的那些轮，**写在了正在被扫描的显存上**（就是老的单 fb 行为） |

   ★ 这也解释了"为什么引入双缓冲之后**才**出现残留"：上一版单 fb 时所有写入都落在唯一那块（= 上屏那块），
   所以没有残留、只有撕裂。

### 10.3 修法：照官方例程抄一行，再挂一个"当真了"的判据

**核心那一行**（`[env:esp32s3-rgb]` 里 `-DRGB_BOUNCE_LINES=10`）：

```c
cfg.bounce_buffer_size_px = (size_t)RGB_BOUNCE_LINES * (size_t)THEME_DISPLAY_RES;   // 10 × 480 px = 9600B/块
```

它为什么治本（同一个文件里的 `lcd_rgb_panel_fill_bounce_buffer()`）：

```c
memcpy(buffer, &panel->fbs[panel->bb_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);
...
panel->bounce_pos_px += panel->bb_size / bytes_per_pixel;
if (panel->bounce_pos_px >= panel->fb_size / bytes_per_pixel) {   // 一整帧走完
    panel->bounce_pos_px = 0;
    panel->bb_fb_index = panel->cur_fb_index;                     // ★★ 换帧在这一刻生效（= 帧边界）
    if (cb) cb(...);                                              // on_frame_buf_complete
}
```

① restart link 挂的是 **`bounce_buffer[0]`**，而它**装哪块 fb 的数据**由 `bb_fb_index` 决定
⇒ "永远 `fbs[0]`"这条死路没了（`cur_fb_index` 每帧被锁存一次）；
② 换帧只在**整帧走完那一刻**生效 ⇒ 不会半新半旧；
③ **LCD 的 DMA 从此完全不碰 PSRAM**（只读内部 SRAM 的 bounce buffer）⇒ 也不再和 CPU 抢带宽。

**另外两处配套改动**（都在 `src/dash_display_rgb.cpp`）：

* ★★ **两块 fb 的"内容收敛"：每次刷新开头，把上一次刷新的脏区从 front 补拷进 back**
  （`catch_up_back()`）。**不做这一步，局部刷新会让两块 fb 的内容分叉** ⇒ 屏上表现就是
  "表情切到下一段了、每秒刷新又回到上一段；进度条也有残留" ✗✗（车主实测）。
  ⇒ 完整的机制、数字与分诊见 **11.1 / 10.3 的两类失效模式**。
* **`on_frame_buf_complete` 计数器 `wrap=`**：上面 `bb_fb_index = cur_fb_index` 的同一刻会回调我们，
  所以 `wrap` 是"换帧真的落在帧边界"的**唯一判据**（`vsync` 在涨而 `wrap` 不涨 ⇒ bounce 没成立）。
* **换帧那道门改用 `wrap` 做屏障**（`wait_swap_settled()`）：等的是"驱动已经把新 fb 锁进
  `bb_fb_index`"，而不是"大概过了一个 vsync"—— 这是"往刚换下去那块写"会不会撕裂的分界。

**两个可测开关**（任务书要的那条；都在日志里明确打出来）：

| 开关 | 位置 | 日志判据 |
|---|---|---|
| **全屏重绘 / 局部刷新** | 编译期默认 `-DRGB_FULL_REFRESH_DEFAULT`（0=局部）+ 运行期自动交替 `-DRGB_FULL_REFRESH_ALTERNATE_MS`（默认 10000ms） | 开机一行 `rgb: 刷新档=…`；每秒行里 `模式=全屏重绘\|局部刷新`；每次自动切换再打一条 `===== 对比档自动切换 ⇒ … =====` |
| **bounce 开关** | `-DRGB_BOUNCE_LINES=10`（0 = 退回旧行为，用于复现/对照） | 开机一行 `rgb: bounce=10行/块(…) on_frame_buf_complete=已注册 ⇒ 换帧在**帧边界**由驱动锁存…` |

### 10.4 上板实测（`[env:esp32s3-rgb]` + COM6；下面都是**原始串口行**）

开机（`rgb:` 那几行就是"现在是什么档"的判据）：

```
rgb: RGB565 480x480 pclk=18000000Hz 数据位=16 已就绪(第二块屏待接)
rgb: 双framebuffer num_fbs=2 fb0=0x3c0d1b20 fb1=0x3c142340(各 450KB PSRAM) on_vsync=已注册
rgb: bounce=10行/块(2块共18KB内部SRAM) on_frame_buf_complete=已注册 ⇒ 换帧在**帧边界**由驱动锁存(bb_fb_index=cur_fb_index);DMA 不读 PSRAM
rgb: 刷新档=局部刷新(每30ms整屏失效一次;自动交替=10000ms)——本行是"当前是全屏重绘还是局部刷新"的判据
rgb: 双fb 之后 空闲 PSRAM=7285KB heap=183KB(内部)
rgb: 整屏刷新 66.2ms(块=8 拷贝25.9ms 数据450KB) 换帧在下一个帧边界由驱动锁存(bb_fb_index)
```

稳态两种模式各来一行（原样，没改一个数字）：

```
rgb: vsync=294(+66/s) wrap=294(+66/s) swap=51(+5/s) flush=170 copy_max=3478us copy_avg=1090us swap_wait_max=7374us phase_max=15292us timeout=0 fb=1/0 msync=1 bounce=29MB/s 模式=局部刷新 psram=7285KB heap=183KB
rgb: vsync=3535(+71/s) wrap=3535(+71/s) swap=429(+11/s) flush=2651 copy_max=3947us copy_avg=3254us swap_wait_max=9570us phase_max=15456us timeout=0 fb=1/0 msync=1 bounce=31MB/s 模式=全屏重绘 psram=7285KB heap=183KB
```

| 指标 | **局部刷新（正式档）** | **全屏重绘（对照档）** | 说明 |
|---|---|---|---|
| `wrap` | **= `vsync`（1:1）** | **= `vsync`（1:1）** | ★ 每扫一帧驱动就走完一整帧并锁存一次 `bb_fb_index` ⇒ **bounce 真的在跑**、换帧真的在帧边界 |
| `vsync` | +64~+66/s | +65~+71/s | 理论值 18e6/(548×508)=64.7Hz。★ 忙时偏高 5~9%（见 10.6 的边界说明） |
| `swap` | +5/s（UI 200ms 档） | **+10~11 整屏/秒** | 全屏重绘档的 `swap/s` 就是"整屏能画多少张/秒" |
| `flush` | ≈20/s | ≈88/s（12 块 × 7.3 整屏） | 整屏 = 12 个 40 行块 |
| `copy_avg` | 1.0~1.5ms | 3.2ms | 一次脏区拷贝 + cache 回写 |
| 整屏刷新 | —（用不到） | **79~87ms/整屏** | 开机那次是 66.2ms |
| `swap_wait_max` | ≤7.4ms | ≤9.6ms | **都 < 一帧 15.5ms** ⇒ 那道门从没等到超出一帧 |
| **`timeout`** | **0** | **0** | 换帧屏障从没靠超时放行 |
| `msync` | **1**（不涨） | **1**（不涨） | "整块补拷"仍然只发生**一次**（第 9 节那套一致性逻辑不动） |
| `bounce` | 28~29MB/s | 28~31MB/s | = 450KB × 64.7Hz，**bounce 的 CPU 代价**（见 10.6） |
| PSRAM / heap | 7285KB / 183KB | 7285KB / 183KB | 不随模式变；PSRAM 与上一轮**逐 KB 相同**（bounce 在内部 SRAM） |
| 应用层 | `206 dash ok spd=… face=…` 照常 1Hz、`BEACON` / `SRC` / `link` 行照常 | 同左 | bounce 那 30MB/s 没有把主循环/VAN 挤坏 |

★ **heap 的差**：上一轮稳态 200KB → 本轮 **183KB**，差的 17KB ≈ bounce 那 18KB（10 行×480×2B×2 块）
—— 与设计账对得上（PSRAM 一分没动）。

### 10.5 换帧时序证据（任务书第 3 条："`vsync` 在涨 ≠ 换帧发生在 vsync 边界"）

这一条上一轮的判断**不成立**，本轮给了能收口的证据，分三层：

1. **源码层（最强）**：换帧生效的唯一时刻就是 `lcd_rgb_panel_fill_bounce_buffer()` 里
   `bb_fb_index = cur_fb_index` 那一行 —— 它在一个 `if (bounce_pos_px >= fb_size/2)` 里，
   也就是**一整帧的数据都送出去之后**（= 帧边界）。这不是"我们的推断"，是驱动的那一行。
2. **计数层**：那一刻驱动回调 `on_frame_buf_complete`，我们记成 `wrap`。
   实测 `wrap == vsync`（逐秒 1:1，见 10.4 两行原文）⇒ **每帧恰好锁存一次**。
   （旧版没有这个回调，所以旧版的 `fb=` 翻转**只是我们自己的变量在翻**，跟屏上无关。）
3. **相位层**：`phase_max = 15292~15456µs` ≈ **一整帧**。也就是说，换帧**请求**是异步来的
   （LVGL 任务在帧内任意相位调用，最晚的那次几乎贴着下一个 VSYNC）。
   ⇒ **安全性不来自"请求时机"，而来自"驱动在帧边界锁存" + 我们那道等 `wrap` 的门**
   （`swap_wait_max ≤ 9.6ms < 15.5ms`、`timeout=0`）。

**结论**：旧版不是"换帧没对齐帧边界"，而是**换帧根本没发生**（屏上只有 `fbs[0]`）；
新版换帧**确实发生在帧边界**，有源码 + 计数 + 相位三层证据。

### 10.6 取舍：三条路（任务书 a / b / c）的账

| 方案 | 实测/推算代价 | 结论 |
|---|---|---|
| **(a) 保持全屏重绘** | 实测 **+10~11 整屏/秒**、`copy_avg` 从 1.1ms 涨到 3.2ms、整屏 79~87ms；即同样的 UI，CPU 花在重画整屏上 | **不采用**（只做对照档）。它能让"残留"看不见（每次都整屏覆盖），但**治不了撕裂**（写的那块照样可能是正在扫的那块），而且贵 7 倍 |
| **(b) 换帧后把 front 拷进 back（450KB/次）** | 450KB memcpy+msync 实测 **≈3.5ms**（`copy_max=3478µs` 那次就是它）；若每次换帧都做 ⇒ 3.5ms×64.7 ≈ **226ms/s（23% CPU）** | **不需要**：`g_fb_complete[]` + 局部 blit 让两块 fb **一直保持一致**，整块补拷只在开机后发生 **1 次**（`msync=1`）⇒ 比"每帧拷"省 **~63 倍** |
| **(c) LVGL DIRECT mode / 双 draw buffer 对齐 RGB 双 fb** | 要把 450KB 的 PSRAM fb 直接当 LVGL 绘制缓冲（渲染写 PSRAM，比我们现在的 38KB **内部 SRAM** 40 行缓冲慢）；而且它**同样绕不开 10.2 的 restart link**（2 fb 不带 bounce = 我们上一轮踩的那个坑） | **不采用**（可行但更慢；真要走的配方在 10.7 的官方 DOUBLE_FB 档里）。★ 官方材料点名的那条 `lv_display_set_flush_wait_cb` 属于 **DIRECT/FULL 模式**才需要：**我们是 PARTIAL（小块内部缓冲）+ 驱动帧边界锁存 + 自己的 `wrap` 门**，缓冲"正在被显示"这件事由那道门解决，所以**不需要**它 |

**最终定案**：**局部刷新（PARTIAL）+ 双 fb（PSRAM）+ bounce 10 行/块 + 驱动帧边界锁存 + 我们那道
`wrap` 门**。全屏重绘档作为运行期对照/兜底保留。

### 10.7 官方 2.8C 例程逐项对照（本机已下载并解开：69MB 资料包）

来源：`https://files.waveshare.com/wiki/ESP32-S3-LCD-2.8C/ESP32-S3-LCD-2.8C-Demo.zip`
（本机可直连；`github.com` 被污染，所以 IDF 源码是从 Gitee 镜像取的）。包内两份官方工程：

* `ESP-IDF/ESP32-S3-LCD-2.8C-Test/`（IDF + LVGL v8）
* `Arduino/examples/LVGL_Arduino/`（Arduino + LVGL v8）

| 项 | 我们 | **官方 IDF 例程** | **官方 Arduino 例程** | 结论 |
|---|---|---|---|---|
| PCLK | **18MHz** | `EXAMPLE_LCD_PIXEL_CLOCK_HZ = 18MHz` | `ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ` | ✓ **我们的 18MHz 就是官方值**（不是折中） |
| porch | hbp=10 / hfp=50 / hpw=8；vbp=18 / vfp=8 / vpw=2 | 逐项相同 | 走 `ESP_PANEL_LCD_RGB_TIMING_*` | ✓ 逐项相同 |
| 极性 | `pclk_active_neg=false`（另两个 idle 也都 0） | 相同 | 同 | ✓ |
| 引脚 | PCLK41/DE40/VSYNC39/HSYNC38 + DATA0..15 = 5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17 | **逐脚相同** | 同 | ✓ |
| 帧缓冲 | `num_fbs=2` + `fb_in_psram=1` | `num_fbs=EXAMPLE_LCD_NUM_FB`（`DOUBLE_FB` 时 2，**默认 1**）+ `fb_in_psram=true` | `FRAME_BUF_NUM (1)` + `fb_in_psram=true` | 我们**多一块 fb**（换来"写的那块永不被读"） |
| **bounce** | **10 行 × 480 = 9600B/块（本轮加）** | `.bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES`（`USE_BOUNCE_BUFFER` **默认 y**） | `BOUNCE_BUF_SIZE (10 * HEIGHT)`，原注释：*"used to avoid screen drift"* | ✓✓ **两边官方例程都开 bounce —— 这就是本轮抄的那一条** |
| 换帧同步 | 双 fb + **驱动帧边界锁存** + `wrap` 门 | 默认档：**单 fb** + bounce + **一对信号量（flush 等 VSYNC 再写）**；`DOUBLE_FB` 档：双 fb 当 LVGL 绘制缓冲 + `full_refresh=true` | 单 fb + bounce，flush 直接写 | 路线不同、目标同一个；我们的方案对**整屏刷新**（开机动画）也安全 |
| LVGL 绘制缓冲 | **PARTIAL**，内部 SRAM 480×40 | 两块整屏 **PSRAM**（或直接用驱动 fb） | 两块整屏 PSRAM（`LVGL_BUF_LEN`） | 我们更省 PSRAM 带宽（渲染写内部 SRAM） |

**关于"要不要先烧官方 Demo 做对照"**：我**没有**先烧它，理由三条（都摆在这儿，随时可跑）：

1. 官方资料包的**源码**已经把答案给全了（上表：porch/PCLK 我们逐项相同；差别只有 buffer 策略），
   而**差别的那一条正是 10.2 的根因**；
2. 车主"看屏"的机会有限，我宁愿花在**修好的固件**上，而不是花在官方 Demo 上；
3. 它是 **16MB 整片镜像**（`Firmware/ESP32-S3-LCD-2.8C.bin`，要写 `0x0`）——
   会覆盖整片 flash（含 `theme` / `image` / NVS 分区）。★ 本机板子上这两个分区**本来就是空的**
   （开机日志原话：`theme: 分区为空,用默认主题` / `image: 镜像无效或未刷入,不用图片资源`），
   所以烧它不会丢车上的主题数据；但烧完**必须再烧回我们的固件**。

真要跑这个对照（一刀切开"硬件 vs 软件"），命令是：

```powershell
# ① 备份整片 flash（万一要原样恢复）：
& 'C:\.platformio\penv\Scripts\python.exe' -m esptool --chip esp32s3 --port COM6 read_flash 0 0x1000000 C:\206dash-scratch\flash-backup.bin

# ② 烧官方 Demo（16MB 整片，约 4~6 分钟；★ 只烧 COM6）：
& 'C:\.platformio\penv\Scripts\python.exe' -m esptool --chip esp32s3 --port COM6 --baud 460800 write_flash 0x0 C:\206dash-scratch\ws-demo\Firmware\ESP32-S3-LCD-2.8C.bin

# ③ 烧回我们自己的固件（app 分区，不动 theme/image）：
cd C:\206dash-scratch\Neru
& 'C:\.platformio\penv\Scripts\platformio.exe' run -e esp32s3-rgb -t upload --upload-port COM6
```

判据：**官方不撕、我们撕 ⇒ 缓冲/时序层**（那就继续按 10.7 抄参数）；**两边都撕 ⇒ 供电/排线/PCLK 层面**。

### 10.8 复核清单（下次动这个驱动时照这条走）

```powershell
# ① 构建（ASCII 副本里；★ 首次会重装 pioarduino 的框架包，见 platformio.ini 那段）
cd C:\206dash-scratch\Neru
& 'C:\.platformio\penv\Scripts\platformio.exe' run -e esp32s3-rgb

# ② 烧写与抓串口：★ 必须先切 UTF-8 代码页，否则 PlatformIO 的日志线程会在进度条那个
#    '░' 字符上抛 UnicodeEncodeError('gbk' codec can't encode …) ——
#    然后 esptool 写满管道**直接卡死**（本轮实测踩了两次，各卡 10 分钟）。
#    症状：终端里刷到 "Writing at 0x00000000 [░░░…]" 就不动了。
chcp 65001 | Out-Null
$env:PYTHONIOENCODING = 'utf-8'; $env:PYTHONUTF8 = '1'
& 'C:\.platformio\penv\Scripts\platformio.exe' run -e esp32s3-rgb -t upload --upload-port COM6

# ③ 抓 60 秒串口（含 5 次"对比档"自动切换）
powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-boot-nopy.ps1 -Port COM6 -Seconds 60 -Out C:\206dash-scratch\rgb-bounce-boot.txt
```

看这 5 个数就够判"这一版对不对"：

| 看什么 | 期望 | 不对时说明什么 |
|---|---|---|
| `rgb: bounce=… on_frame_buf_complete=已注册` | **有这一行**且写着 10 行/块 | 没有 ⇒ `-DRGB_BOUNCE_LINES` 没生效 ⇒ 换帧必然不生效（10.2） |
| `wrap=` 与 `vsync=` | **两者同步（1:1）** | `vsync` 涨 `wrap` 不涨 ⇒ bounce 没跑起来（`bb_fb_index` 不更新） |
| `timeout=` | **恒为 0** | 涨 ⇒ 换帧屏障没等到（面板被停 / 中断没来） |
| `msync=` | 停在 **1** | 一直涨 ⇒ 每帧都在整块补拷（一致性逻辑被打破） |
| `模式=` | 在 `局部刷新`↔`全屏重绘` 之间按 10s 交替 | 一直同一档 ⇒ 自动交替没生效（`-DRGB_FULL_REFRESH_ALTERNATE_MS`） |

**这一版的边界（写清楚，别指望它包打天下）**：

* **bounce 的 CPU 代价是硬的**：每帧 450KB 从 PSRAM 搬进内部 SRAM = 实测 **28~31MB/s**，
  在 DMA EOF 中断里分 48 次做（每次 9600B）。本机实测没把主循环/VAN/UI 挤坏
  （heap 183KB、`BEACON`/`SRC`/`link` 行都正常），但**它确实占了可观的一块 CPU**；
  要降这块开销只有两条路：降 PCLK/刷新率，或改回"直读 PSRAM"——而后者就是 10.2 的死路。
* **`vsync` 计数在 CPU 忙时会偏高 5~9%**（实测 +71/s vs 理论 64.7/s），怀疑是 VSYNC 脉冲宽度内
  ISR 被 bounce 的 EOF 中断延迟、同一脉冲被重复计入。它**不影响换帧**（重复的重启仍落在消隐期，
  而且 `wrap` 与它同步），但**别拿这个数当 PCLK 的精确判据** —— 空闲时它仍然是 +64~65/s。
* **`35 秒` 附近那几行 `swap_wait_max` 涨到 9.6ms** 是正常的：稳态 UI 200ms 一次刷新，
  但开机动画/告警闪烁那几拍会连着换算，等的就是"下一个帧边界"，上限一帧（15.5ms）。
* **两屏共用一条总线**仍未做（单屏版本），见 9.7。

---

## 11. 2026-09-24 第四轮：**残留修好之后剩的"抖动 + 横纹"**（分诊表 + 两个改动 + 数字）

### 11.1 两轮症状的机制收口表（每一轮到底修的是哪一类）

| 轮次 | 车主看到的现象 | 归到哪一类 | 机制（可复核的位置） | 治法 | 结果 |
|---|---|---|---|---|---|
| 第二版 | 撕裂 + **残留** | **换帧根本没生效** | 每个 VSYNC 都把 DMA 重置回**固定挂在 `fbs[0]`** 的 restart link ⇒ 屏上只有 fbs[0]（10.2） | 开 bounce（官方例程的 10 行）⇒ `bb_fb_index` 在帧边界锁存 | 撕裂基本没了 ✓ |
| 第三版 | **残留**（"表情切到下一段了，每秒刷新又回到上一段"） | **两块 fb 内容不一致** | 局部刷新只喂了"当次的后台"那一块 ⇒ 两块 fb 分叉，换帧就是在两块之间来回翻（10.3） | **每次刷新把上一次的脏区补拷进另一块**（`catch_up_back()`） | **残留修好 ✓✓** |
| 第四版 | **抖动 + 横纹（随刷新移动）** | **欠载 / 带宽** | 补拷/全屏刷新把 PSRAM 占满的那几毫秒里，bounce 的填充来不及 ⇒ 那一帧吐旧行 | 见 11.3（去掉对照档 + 降 PCLK） | 横纹仍在 ✗ |
| 第五版 | 同上，但车主补了一条**决定性观察**：**扫表阶段正常、稳态才出现** | **补拷的"大突发"**（不是"拷贝没完成"） | 稳态每次刷新是一笔 **~90KB 的 PSRAM 拷贝**；实测一笔 450KB 要 **34ms（13MB/s）** ⇒ 那一笔持续几毫秒，期间 bounce 填充被挤掉 ⇒ 一条随刷新移动的横带 | **把补拷改成"空闲时间小碎步"（每步 8 行 ≈7.7KB）** | 见 11.4/11.5 |

### 11.2 抖动 / 横纹的分诊表（现象 → 判据 → 方子）

| 现象 | 判据（怎么分辨） | 归到哪一类 | 方子 |
|---|---|---|---|
| **撕裂**（一条横缝，缝两侧是**两幅完整画面**） | 缝随每次刷新换位置；`wrap == vsync`、`timeout=0` | 换帧与扫描赛跑 | 帧边界换帧（bounce 的 `bb_fb_index` 锁存）+ 只写不显示的那块 fb（10.2/10.3） |
| **残留**（旧内容**留在**某个区域，可能来回跳） | 同一区域在两块 fb 之间来回翻；`catchup` 不涨或很小 | 两块 fb 内容不一致 | 让两块逐帧收敛：**补拷脏区**（10.3）或每帧全量重绘 |
| **横纹（固定不动）** | 位置不随刷新变；关掉背光 PWM（改恒亮）后消失 ⇒ 拍频/干扰 | 背光 PWM / 干扰 | 改 PWM 频率或恒亮 + 外调光 |
| **横纹（随刷新移动）** | 位置随刷新变；`bounce=MB/s` 越大越明显；有整屏刷新/大批拷贝时更明显 | **欠载 / 带宽** ★ | 见 11.5：先减突发（关掉全屏重绘档），再降 PCLK，必要时加大 bounce |
| **横纹（扫表正常、稳态才出现）** ★★ | 画面**持续变化**时看不出来、**稀疏刷新**时才暴露；同一行日志里 `copy_max` 是几毫秒~几十毫秒的大值 | **"大突发"与 bounce 填充抢 PSRAM** | **把拷贝摊成小碎步**（本轮的做法，见 11.3）；或降 PCLK；或加大 bounce |
| **整幅平移 / 游走** | 整幅画面偏几个像素，慢慢漂 | PCLK 过高 / 关 cache / 写 Flash 抢带宽 | `CONFIG_LCD_RGB_RESTART_IN_VSYNC`（**本栈本来就是 y**，见 10.2 ②）/ `esp_lcd_rgb_panel_restart()` |
| **抖动（局部/周期性）** | 一时一时的卡顿；`refresh=avg/min/max` 的 max 远大于 avg | 主循环被 PSRAM 搬运拖住 | 同上：先减突发 |

★ 本轮的现象归到 **"横纹（随刷新移动）"** 这一行（车主原话：横纹是随刷新移动的）。

### 11.3 逐条回答"拷贝 → msync → 交换"那几个问题（**顺序本来就是对的**，问题在"拷贝的代价"）

1. **顺序**：`blit_area()` 里 `memcpy` 与 `esp_cache_msync(C2M|UNALIGNED)` 都是**同步**完成的；
   `request_swap()` **只在本次刷新的最后一块 flush**（`lv_display_flush_is_last()`）里被调用
   ⇒ **不存在"最后一块还没拷完/没 msync 就换帧"的路径** ✗（代码顺序可复核）。
2. **`wait_swap_settled()` 等的是谁**：等的是**"换帧已生效"** —— 判据是驱动的
   `bb_fb_index = cur_fb_index` 在**帧边界**锁存的那一刻（我们用 `on_frame_buf_complete` 记成
   `wrap`）；**不是**等拷贝（拷贝在它之前早就同步做完了）。两者没有被混用 ✓。
3. ⇒ 所以"**半写的 back fb 被换上去**"这条**不成立** ✗；真正成立的是"**拷贝本身把 PSRAM 占满**"
   ⇒ 见 11.5 的带宽账 ✓（这一条由车主"扫表正常/稳态异常"那条观察直接指出来 ✓）。
4. 时间戳：`phase_max`（换帧请求落在帧内哪个相位）与 `refresh=avg/min/max`（刷新间隔）
   已经打在**同一行**日志里 ✓；改动前后的对照见 11.4 ✓。
5. 周期性大写入：1Hz 日志那点 UART 输出（约 1KB/s）不是问题；**真正的周期性大写入就是补拷** ——
   本轮把它摊平了 ✓（`copy_max` 33.7ms → 3.06ms）。

### 11.4 数字（同一块板 COM6；四个版本的原始串口行）

| 指标 | 18MHz + 自动交替(10s) | 18MHz + 交替关 | 15MHz + 交替关 + **整笔**补拷 | **15MHz + 交替关 + 小碎步补拷（交付档）** |
|---|---|---|---|---|
| `pclk` / `vsync` | 18MHz / +64~66/s | 18MHz / +64~66/s | 15MHz / +53~54/s | **15MHz / +53~54/s** |
| **`copy_max`**（单笔最大 PSRAM 搬运） | 36.7ms | 33.7ms ✗ | **33.7ms** ✗ | **3.06ms** ✓✓（**降 11 倍**） |
| `refresh=avg/min/max`（刷新间隔） | — | — | 199/192/211ms | **200/199/201ms** ✓✓（抖动消失） |
| `fullrb`（整屏刷新频次） | **~11/s** ✗ | 0/s ✓ | 0/s ✓ | **0/s** ✓ |
| `catchup` | — | — | +5/s、~450KB/s | **+5/s、429~466KB/s**（总字节一样 ✓，只是摊开了） |
| `bounce`（填充负载） | 28~31MB/s | 28~29MB/s | 23MB/s | **23MB/s** ✓ |
| `wrap` vs `vsync` | 1:1 | 1:1 | 1:1 | **1:1** ✓ |
| `timeout` / heap / PSRAM | 0 / 183KB / 7285KB | 同 | 同 | **0 / 183KB / 7285KB** ✓ |

### 11.5 为什么"扫表正常、稳态异常"，以及账怎么算

* **面板消耗速率** = `pclk × 2B`：18MHz ⇒ 36MB/s、**15MHz ⇒ 30MB/s**、12MHz ⇒ 24MB/s。
* **CPU 填充速率**：稳态实测 23MB/s（够用 ✓），但**一笔大拷贝时掉到 13MB/s**：
  实测 `copy_max = 33739µs`（450KB ⇒ 13MB/s）✗。
* ⇒ bounce 档真正的判据不是"平均够不够"，而是**"那几毫秒里填充有没有掉到消耗速率以下"**：
  掉下去的那几毫秒，LCD 就把**bounce buffer 里的旧内容**当这一帧发出去 ⇒ **若干行是旧的** ⇒
  一条**横带**；下一次刷新又发生在别的位置 ⇒ "**随刷新移动**" ✓✓。
* **为什么扫表阶段看不出来**：扫表时整屏在**持续变化**，旧的横带立刻被下一帧的新内容盖掉（而且
  那时每帧都在重绘，横带位置一直在动，眼睛把它当成运动的一部分）✓；稳态下画面**几百毫秒才动一次**，
  横带就停在那儿，一眼就看见 ✓✓ —— 车主的这条观察与机制**完全吻合** ✓。
* ⇒ 结论：**别在 bounce 填充旁边放"几毫秒级的 PSRAM 大突发"** ✓。治法就是本轮做的：
  把补拷拆成**每步 8 行（7.68KB）**的小碎步，放在两次刷新之间的空闲时间里 ✓。
  （对比：一笔 90KB 至少要 7ms ✗；一步 7.7KB ≈ 0.6ms ✓，而两块 bounce buffer 合计能吸收
   ~19KB 的赤字 ⇒ **这一小步完全被吸收** ✓✓。）
* 顺带：这一改动同时把**主循环的卡顿**也去掉了 —— `refresh` 从 `192~211ms`（±16ms）变成
  `199~201ms`（±1ms）✓✓，车主说的"抖动"应当就是同一件事。

### 11.6 如果 15MHz 只是"好转、没消失"，下一步（一次只动一个）

1. `RGB_PIXEL_CLOCK_HZ` → **12MHz**（24MB/s、43.2Hz）：只改一个数；
2. 干掉**补拷的 450KB 突发**：`g_prev_full` 那一条改成"让 LVGL 重画整屏"（少 2/3 的 PSRAM 流量）；
3. `-DRGB_BOUNCE_LINES=40`；
4. （治本、但要单独一轮）把 UI 里"整屏失效"的告警闪改成只失效真正变的那几块 —— 但**本轮实测
   `fullrb=0/s`：告警闪并没有整屏失效**，所以这条**不是**当前横纹的原因 ✓（别再往这边查）。

### 11.7 官方 Demo 对照（**尚未烧**，命令与判据在这儿）

已经做到的一步（**源码级对照**，见 10.7 的表）：两份官方例程**都开 bounce**（`10 * H_RES`），
**默认都是单 fb**（单 fb 不可能出现"两块 fb 内容不一致"，也不会把 CPU 拖进 450KB 突发），
IDF 那份还额外用"flush 等 VSYNC"的信号量把手写像素锁在帧边界 ✓。

**还没做的那一步**：把官方整片镜像烧上去、让车主在**同一块板/同一根线/同一电源**下看它撕不撕/抖不抖
（这是排除供电与线材的唯一干净做法）。我没烧的原因有两条，摆在这儿：

1. 我这边只能读串口、**看不到屏** —— 烧完还是得车主看，等于要占掉一次"看屏"的机会，
   而当前更值钱的一次看屏是**这一版修好的固件**（残留 ✓ 已确认、横纹待确认）；
2. 它是 **16MB 整片镜像**（`Firmware/ESP32-S3-LCD-2.8C.bin`，写到 `0x0`），会覆盖整片 flash。
   ★ 本机实测 `theme` / `image` 分区**本来就是空的**（`theme: 分区为空,用默认主题`），
   所以烧它**不会丢车上的数据** ✓，但烧完必须再烧回我们的固件。

要跑就照这三条（**只烧 COM6**）：

```powershell
# ① 备份整片 flash：
& 'C:\.platformio\penv\Scripts\python.exe' -m esptool --chip esp32s3 --port COM6 read_flash 0 0x1000000 C:\206dash-scratch\flash-backup.bin
# ② 烧官方 Demo（16MB 整片，4~6 分钟）：
& 'C:\.platformio\penv\Scripts\python.exe' -m esptool --chip esp32s3 --port COM6 --baud 460800 write_flash 0x0 C:\206dash-scratch\ws-demo\Firmware\ESP32-S3-LCD-2.8C.bin
# ③ 烧回我们的固件：
cd C:\206dash-scratch\Neru; & 'C:\.platformio\penv\Scripts\platformio.exe' run -e esp32s3-rgb -t upload --upload-port COM6
```

判据：**官方也撕/也横纹 ⇒ 供电/线材层面**（同一块板同一根线）；**官方干净、我们还有 ⇒ 继续在驱动侧收**。

---

## 12. 2026-09-24 第六轮：**"时有时无" ⇒ 带宽争用**（把"仅剩的那一笔"也压下去）

### 12.1 车主的判据：**"有时消失、有时继续抖动+横纹" = 带宽争用的指纹**

余量够就没事、一有别的 PSRAM 活动就被吃掉 ⇒ 说明**还有一笔大搬运**。查下来是
**"当次脏区"那一笔**（不是补拷 —— 补拷上一轮已经改成小碎步了）：

* 那一笔的大小 = **一次 LVGL flush 的数据量** = 绘制缓冲的高 × 屏宽 × 2B；
* 上一版绘制缓冲是 **40 行 ⇒ 38.4KB ⇒ 实测 `blit_max ≈ 3ms`** ✗（按 13MB/s 算）；
* 它**没法再摊平**（必须等 LVGL 把这块缓冲收回去才能 `flush_ready` ⇒ 不能跨圈拖着拷 ✗），
  所以只能**把它变小**：本轮把绘制缓冲改成 **16 行 ⇒ 15.36KB** ✓。

### 12.2 本轮的改动（一次一个变量：**只动绘制缓冲的行数**）

```c
#ifndef RGB_DRAW_BUF_LINES
#define RGB_DRAW_BUF_LINES 16          // 原 40 行
#endif
static lv_color_t draw_buf[THEME_DISPLAY_RES * RGB_DRAW_BUF_LINES] __attribute__((aligned(LV_DRAW_BUF_ALIGN)));
```

为什么是 16 行：两块 bounce buffer 合计 **19.2KB**，它能吸收的"赤字"上限就是这个量级；
**15.36KB < 19.2KB ⇒ 这一笔能被吸收** ✓；40 行的 38.4KB 则吸收不了 ✗。
代价：整屏刷新从 12 块变 30 块（每块固定开销变大）；稳态是局部刷新 ⇒ 几乎无感 ✓，
而且**内部 SRAM 反而省了 34.5KB**（RAM 48.1% → **37.6%**，`heap` 183KB → **217KB** ✓）。

另外把"单笔搬运"按来源拆开打点（车主要的诊断）：`blit_max`（当次脏区）/ `step_max`（补拷一步）/
`copy_max`（全系统最大单笔）/ `catchup=…forced?KB`（被"刷新提前开始"逼出来的兜底量）。

### 12.3 数字（同一块板 COM6，原始串口行）

```
rgb: 脏区单笔上限=16行(15360B) —— bounce 两块共18KB,能吸收 ~19KB 赤字
rgb: vsync=242(+54/s) wrap=242(+54/s) swap=48(+5/s) flush=211 blit_max=1325us step_max=950us copy_max=1325us copy_avg=695us swap_wait_max=10675us phase_max=17106us timeout=0 fb=0/1 catchup=48(+5/s 436KB/s forced0KB) refresh=200898/199362/205120us fullrb=0/s bounce=23MB/s 模式=局部刷新 psram=7285KB heap=217KB
```

| 指标 | 40 行绘制缓冲（上一版） | **16 行（交付档）** |
|---|---|---|
| **`blit_max`**（当次脏区那一笔） | ~3011~33739µs ✗ | **1325~1332µs** ✓（≈1.33ms） |
| `step_max`（补拷一步，8 行） | — | **950~966µs** ✓ |
| `copy_max`（全系统最大单笔） | **33739µs** ✗ | **1332µs** ✓（**降 25 倍**） |
| `catchup` / `forced` | ~450KB/s | **+5/s、429~461KB/s、`forced0KB`** ✓（兜底一次都没触发） |
| `refresh=avg/min/max` | 199/192/211ms | **199~201 / 186~199 / 205~214ms** ✓ |
| `wrap`vs`vsync` / `timeout` / `fullrb` / `bounce` | 1:1 / 0 / 0/s / 23MB/s | **1:1 / 0 / 0/s / 23MB/s** ✓ |
| **RAM / heap（内部）** | 48.1% / 183KB | **37.6% / 217KB** ✓（省 34.5KB） |
| Flash | 84.8% | 84.8% |

⇒ **全系统最大的单笔 PSRAM 搬运从 33.7ms 降到 1.33ms**（两轮共降 **25 倍**），
而 bounce 的填充预算只有 ~0.6ms/ 块的量级 ⇒ 现在任何一笔都**落在能被吸收的范围内** ✓。
**"时有时无"应当就此消失** —— 这一句只能由车主那一眼收口。

### 12.4 如果还"时有时无" ⇒ 按这个顺序（各一次、只改一个）

1. `RGB_PIXEL_CLOCK_HZ` **15 → 12MHz**（24MB/s、43.2Hz；只改一个数）；
2. `-DRGB_BOUNCE_LINES=10 → 40`（绝对余量 ×4；★ 代价 76.8KB 内部 SRAM，当前 heap 217KB ⇒ 装得下，
   但要实测 heap 与 RAM 占比，别把别的路径饿死）；
3. 便宜的排除法（只作诊断、别长期改）：把 1Hz 的 `BEACON`/`SRC*`/每秒行**静音 30 秒**看横纹是否变稀
   （⇒ 周期活动参与其中）；以及**对齐时间戳**看横纹是否与"UI 每秒刷新"严格同拍（同拍 ⇒ 就是 12.1 那条）。

### 12.5 本轮补跑的两套回归（上一轮因为没挂 gcc 桩而没跑成）

* **native**：把 `C:\206dash-scratch\zigbin`（zig 转发桩：`gcc.cmd` → `zig cc`）挂上 PATH 后
  `pio test -e native` ⇒ **234 test cases: 2 skipped, 232 succeeded**（与基线逐位相同 ✓）。
  ★ 坑记在这儿：**没挂这个桩就会报 `'gcc' is not recognized`**，那是环境问题、不是回归。
* **JS 五套**：`node tools/theme-editor/test-*.js` ⇒ **71 / 429 / 259 / 155 / 369** 全绿 ✓
  （外加 `syntax-check-pages.js` **12** ✓）。

---

## 13. 2026-09-24 板载蜂鸣器：**有源 / 无源**（临时自检路径 + 实测）

### 13.1 要回答的那半问（`ARCHITECTURE.md` §8 的 **L14**）

已知的三条（都可复核）：

* 微雪 wiki 的 2.8C **器件清单**写的是 **`Buzzer`（蜂鸣器）**，不是扬声器；
* 它的控制脚是 **TCA9554 的 `EXIO8`（bit7）** —— **零额外引脚**（同一颗扩展芯片本来就在驱动
  `LCD_RST=EXIO1` / `LCD_CS=EXIO3`，见本文件第 4 块）；
* 我们的固件**已经在驱动这颗 TCA9554**（点屏就是它通的证据）⇒ "加一路 EXIO8 输出"是小改动。

**不知道的**是：它**有源**（自带振荡电路，给直流就响）还是**无源**（要外部方波，频率=音高）。
wiki 与官方例程**都没写** —— 而这两条在固件里是**两条不同的驱动路径**：

| | 有源 | 无源 |
|---|---|---|
| 固件要做什么 | 一个**开关**（EXIO8 置位/清零） | 必须给**方波**（频率=音高、占空比≈音量） |
| 在这块板上代价 | 一次 I2C 事务（~0.3ms @100kHz） | 每个边沿一次 I2C 事务 ⇒ 实测 **141~178µs/次**、要主循环忙等 |
| 能表达什么 | 只表达"响/不响"的节奏 | 能表达音调，但**很粗糙**（见 13.4） |

### 13.2 自检的四段，以及**为什么 ①② 单独不足以定性**

| 段 | 波形 | 时长 | 只看这一段的判据 |
|---|---|---|---|
| ① 直流开关 | EXIO8 高 200ms → 低 200ms，重复 5 轮 | 2.0s | 有源 ⇒ "哔"5 声；无源 ⇒ 每轮起止各一声"咔哒" |
| ② 方波 | 200 / 1000 / 2000 / 4000 Hz，各 1 秒（段间静音 300ms） | 4×1.3s | 音高随频率变 ⇒ 像无源；只"咔哒" ⇒ 像有源 |
| **③ 持续拉高** | **持续 2 秒、中间一次都不翻转** | 2.0s | **一整段不间断的稳定音 ⇒ 有源**；只有起止两声"咔哒"、中间静音 ⇒ 无源 |
| ④ 持续拉低（对照） | 持续 2 秒 | 2.0s | 应当**完全静音**（排除"响的是别的东西"或极性反了） |

★★ **③ 才是唯一判据**，这一条是本轮补上的，理由是：

* **有源**蜂鸣器被高频通断时，**听起来也像"音高在变"** —— 那是在**斩波它自己的输出**
  （200Hz 斩波 ≈ 低音嗡嗡、更高频斩波 ≈ 高音嗡嗡）；
* **无源**蜂鸣器在直流段只会"咔哒"，但被车主的耳朵听成"哔"也不奇怪（短促的咔哒就是一"声"）。

⇒ 车主第一轮**两步都听到了**，而这两种解释**都与它相容** ✗ ⇒ 必须补一个**只对其中一种成立**的测试：
**给一个不翻转的稳定直流**。有源会"自己响满 2 秒"，无源只会顶一下振膜（起止各一声咔哒）。

### 13.3 怎么复现（临时开关，**测完必须删掉**）

> ★★ **先看 13.8**：这一节与 13.4 的**数字**出自"第一版"（自检跑在显示主循环里、方波段临时把
> I2C 提到 400kHz）；当晚因为车主报"屏幕突然黑掉"已**改版**（自检搬进 core 0 的独立任务、
> 不再改总线时钟），**改版后的数字与实现以 13.8 为准**。下面这套命令与"四段判据"没变。

```powershell
# ① 唯一的打开方式:在 platformio.ini 的 [env:esp32s3-rgb] 的 build_flags 里**临时**加一行
#      -DBUZZER_SELFTEST=1
#    然后构建+烧 COM6(★ 只烧 COM6;构建与烧写都在 ASCII 副本 C:\206dash-scratch\Neru 里):
chcp 65001; $env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'
python -m platformio run -e esp32s3-rgb -t upload --upload-port COM6

# ② 从复位那一刻抓 45 秒串口(DTR/RTS 全程 false,只用 RTS 做一次复位)
powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-boot-nopy.ps1 -Port COM6 -Seconds 42

# ③ 测完把那一行删掉、重建、回烧 —— 默认构建的 RAM/Flash 应当**回到基线**(见 13.5)
```

* 开局留 **6 秒**给车主开监视器 / 注意听；**复位一次就重跑一遍**（自检每次上电都跑，跑完就永久闭嘴）。
* 代码在 `src/dash_display_rgb.cpp` 的 `#if BUZZER_SELFTEST`（**默认 0**）里，由 `dash_display_poll()`
  驱动 ⇒ **`main.cpp` 一行都不用改**，`lib/dashcore/buzzer.h` 的抽象**一个字没动**（测完再按结论接真机）。
* **不阻塞主循环**：每次调用最多干 2~3.5ms（方波段的自旋预算，见 `kBsSqBudgetMin/Max`），做完立刻返回；
  没有一处 `delay()`、没有一处死等。

### 13.4 实测（COM6，原始串口行；自检档 `-DBUZZER_SELFTEST=1`）

四段方波（**车主应当拿"实测平均"去对听到的音高，不是请求值**）：

| 段 | 请求 | 翻转次数 | 段时长 | **实测平均** | I2C 事务 次数/平均 | 音高上限 | 两次翻转最大间隔 | 跳过格点 | 自检占用 |
|---|---|---|---|---|---|---|---|---|---|
| 200Hz | 200Hz | 332 | 1002.477ms | **≈165.5Hz** | 334 / 178µs | ≈2808Hz | 41801µs | 68 | 765.82ms（76.3%） |
| 1kHz | 1000Hz | 1760 | 1000.136ms | **≈879.8Hz** | 1762 / 147µs | ≈3401Hz | 40192µs | 240 | 839.60ms（83.9%） |
| 2kHz | 2000Hz | 3070 | 1011.746ms | **≈1517.1Hz** | 3072 / 141µs | ≈3546Hz | 41693µs | 976 | 753.65ms（74.4%） |
| 4kHz | 4000Hz | 2910 | 1000.009ms | **≈1454.9Hz** | 2912 / 141µs | ≈3546Hz | 36973µs | 5090 | 708.97ms（70.8%） |

**这张表怎么读**（都是"预期之内"的粗糙，不是故障）：

* EXIO8 在 **I2C 扩展器**上 ⇒ **每次翻转一次完整事务**（START+地址+寄存器+数据+STOP）：
  方波段临时把 I2C 提到 **400kHz**（TCA9554PWR 是 Fast-mode 器件）后实测 **141~178µs/次** ⇒
  **纯翻转率上限 ~2800~3500Hz**（= 表中"音高上限"）；4kHz 那一段**物理上就到不了**，
  请求 4000Hz 只跑出 ≈1455Hz —— 这是"经 I2C 扩展器驱动"的**硬顶**，换固件写法也变不了多少。
* **200Hz 只跑出 ≈165.5Hz**：主循环一圈在自检期间平均 **2.4~3.7ms**（自检自己忙 0.7~0.85ms +
  显示驱动的补拷小碎步 ~0.9ms + LVGL/VAN/日志），而 200Hz 的半周期是 2.5ms ⇒ 有 68 个格点
  （/400）整个落在"两次调用之间"被跳过。**平均"贴上请求值"和"一格都不丢"在单任务主循环里
  不能同时成立** —— 这里选的是前者（格点只往前走，不因为"晚了"就往后推）。
* **两次翻转最大间隔 37~42ms**：那一下是 **1Hz 的日志行写串口**（`rgb:` 那行 ~300 字节 @115200）
  把主循环按住了一会儿；每秒一次，对"平均频率"影响很小（跳过格点数已经把它的账记进去了）。
* ⇒ **③ 持续拉高那一段完全不受这些影响**（整段只写一次 I2C），这就是为什么定性判据放在那一段。

自检本身的自证（原始行）：

```
buzzer: 回读 TCA9554 输出寄存器=0x05(本机影子=0x05)
buzzer: 期望 0x05 = LCD_RST(EXIO1)高 + LCD_CS(EXIO3)高 + 蜂鸣器(EXIO8)低 ⇒ 屏没被扰动、蜂鸣器已静音
```

即：自检**没有**把扩展器写坏（影子寄存器与芯片读数一致）、**`LCD_RST`/`LCD_CS` 全程为高**
（屏没有被复位/片选扰动）、收尾 **EXIO8=0（静音）**。

### 13.5 自检期间的 UI / 内存（**"车主显然听得到"之外的那些客观项**）

自检 ①~④ 全跑在那 17 秒里，同期串口上（都是原始行）：

| 项 | 实测 | 说明 |
|---|---|---|
| `206 dash ok spd=… face=…` | **每秒一行，一行没断** | UI 照常渲染（自检 ② 段自检自己占了 71~84% 的 CPU，仍没把 UI 挤掉） |
| `BEACON n step=…` | **1~40 全在**（step=6/7/8 轮转） | 主循环没有卡死在任何一步 |
| `heap=217KB` | 自检**前中后一模一样的 217KB** | 自检不分配任何堆内存（几个静态计数器） |
| `psram=7285KB` | 同上，**不变** | 没有新的 PSRAM 占用 |
| `rgb: vsync=…(+54/s) wrap=+54/s timeout=0` | 面板扫描/换帧**一路正常**，`timeout` 全程 0 | 显示路径没被自检影响 |
| `blit_max 1.28~1.34ms / catchup +5/s 43xKB/s` | 与自检前**同一量级** | 自检没有恶化上一轮收口的 PSRAM 搬运 |
| 主循环间隔（自检自测） | 平均 **2.4~3.7ms** / 最大 **38.6~43.1ms**（= 1Hz 日志行那一下） | 见 13.4 |

构建体积（同一条 env）：

| 档 | RAM | Flash |
|---|---|---|
| **自检档**（`-DBUZZER_SELFTEST=1`） | 37.6% / 123,136B | 85.3% / 894,027B |
| **默认档**（开关删掉后重建、并已回烧 COM6） | **37.6% / 123,080B** | **84.8% / 889,247B** |
| ★ 改动前的基线（`3d438a3` 记在 ACCEPTANCE 第六轮） | 37.6% / 123,080B | 84.8% / 889,247B |

⇒ **默认构建回到基线的数字逐位相同**：`#if BUZZER_SELFTEST` 默认 0，那段代码在常规构建里
**一个字节都不参与**（RAM 那 +56B 与 Flash +4.8KB 只出现在自检档里）。

### 13.6 车主听到的 = **结论**（2026-09-24 晚）

> ★★ **结论：这颗蜂鸣器是【有源】蜂鸣器（自带振荡电路）—— 只会"响 / 不响"，没有音调，
> 也没有 PWM 通路，音量不可调。**

**车主的原话（三问的回答）**：

| 问 | 车主听到的 | 说明 |
|---|---|---|
| ① 直流 5 轮 | **"哔"5 声** | 有源的签名（无源只会"咔哒"） |
| ② 方波 200/1k/2k/4k | **换驱动频率音调不变** | ★ **判别性的一条**：无源的音高必然随频率变 |
| ③ 持续拉高 2 秒 | **持续长鸣**（一整段，不是两声咔哒） | 唯一定性判据 ⇒ 有源 |
| ④ 持续拉低 2 秒 | （对照）静音 | 排除"响的是别的东西"/极性反了 |
| 主观音量/音色 | **较响、偏尖** | 决定了告警音要短、且**必须能关** |

**为什么"音调不变"不是偶然、而是设计使然（电路事实）**：
蜂鸣器挂在 **TCA9554 的 EXIO8**（一颗 **I2C 扩展器**的输出脚）上 —— 那条路上
**根本没有 LEDC/PWM 通路**（PWM 要的是 GPIO 的 LEDC 通道或定时器），扩展器的输出只能是
**静态高/低** ⇒ 原理上就**不可能**产生不同频率的音调。所以：

* 之前为了"也许是无源"而搭的方波段（②）**没有意义**（第一版还用软件翻转去凑 200Hz~1kHz，
  只是把有源蜂鸣器的输出**斩波**了 —— 听起来像音高在变，其实是通断节奏）；
* 结论与 13.2 的判据表完全一致：**只有 ③ 那一条是定性的**，①②都只是佐证。

★ 这条结论**同时回答了任务书里"若有源/若无源"的两条分支**：走"有源"那一支（13.7）。

### 13.7 接真机那一档：★ **2026-09-24 已实现并上板**（原"只写方案"已作废）

**结论是有源（13.6）⇒ 需要的代码量极小，且 `alerts` 一行都不用改。**

★ 下面 1~3 条**已经落地**（不是方案了），实际实现与本方案的两处**有意偏离**写在
"实际实现"那一段里；4 条（L14 本身）**已经关闭**，结论见下。

1. 给 `TCA9554` 的 EXIO8 暴露一个**最小接口**：在 `src/dash_display_rgb.cpp` 里加
   `dash_buzzer_set(bool on, void* ctx)`（内部就是现成的 `tca9554_set(BUZZER_EXIO_BIT, on)`），
   声明放进 `src/dash_display.h`（`#if defined(DASH_DISPLAY_RGB)`）。
   ★ **不要**在第二个文件里另写一遍 I2C 时序：影子寄存器 `g_exio_out` 是"读-改-写"的，
   两处各持一份会**互相覆盖丢位**（丢到 `LCD_RST`/`LCD_CS` 上就是黑屏）。
2. 加一个 `Buzzer` 子类（实现"开/关"就够）：

   ```cpp
   class BuzzerExio : public Buzzer {
     void begin() override;                            // 上电静音
     void beep(BeepPattern p, uint32_t ms) override;   // 起一拍（非阻塞）
     void tick() override;                             // ★ 主循环每轮推进时序
     void off() override;                              // 取消 + 立刻静默
     const char* name() const override { return "exio8"; }
   };
   ```

   ★ `beep()` **不需要**频率/占空比参数 —— 有源蜂鸣器只有"响/不响"，节拍全由
   `Alerts::beeping()` + `beep_ms` + 调用方 `off()` 给（这正是 `buzzer.h` 当初把
   "谁发声"与"什么时候该响"分开的原因）。
3. `main.cpp` 里挂上（**新增 `-D` = 0**：门用的是既有的 `DASH_DISPLAY_RGB`），
   在 `#if defined(DASH_DISPLAY_RGB)` 里那一行 `g_buzzer = &g_buzzer_exio;`。
4. ★ **L14 已关闭（2026-09-24）**：发声的是 **2.8C 这一块 = 右板（主机板）**。
   自检（13.6）只证明"这块板上的蜂鸣器会响"，而"**由哪块板发**"这一问在**本单**里由
   "手上只有这一块 2.8C + 它板载就能发声（零额外引脚）"定下来：
   走 2.8C 本地发声就**用不上** `EVENT 0x01` 那条还没定的上行。
   完整答复（含"另一块板有没有蜂鸣器 = 未实测"那一条）见
   `ARCHITECTURE.md`「显示约定」§4.2 与 §8 的 L14 行。

#### 13.7.1 实际实现（和上面那份方案的两处**有意偏离**，都写清理由）

| 方案里写的 | 实际做的 | 为什么改 |
|---|---|---|
| `bool dash_buzzer_set(bool on)` | `void dash_buzzer_set(bool on, void* ctx)` | 驱动与显示侧靠**函数指针**解耦（`BuzzerExioSetFn`），签名要能当回调用；`lib/dashcore` **不 include `src/` 的头**（口径与 `buzzer_host_printf()` 那条一致）。返回值没人用 ⇒ 去掉（"写没写对"由那行 `buzz: exio` 自证 + 开关那一刻的回读兜着） |
| `beep()` 里直接 `set(true)` | `beep()` 只**起序列**，真正的高低电平由 `tick()` 推进 | ① `Alerts::beeping()` 只在**一拍**上为真，而 `Triple`/`Urgent`/`Long⇒3 短哔` 要跨好几拍 ⇒ 必须有人在这些拍之间推进，否则**本该 3 声只响 1 声**（这一条是**上板前就被 native 用例逼出来的**）；② `off()` 必须是"取消"（静音那一跳靠它立刻掐断）⇒ 序列不能靠调用方的时序收尾。`Buzzer::tick()` 是**默认空实现**的新虚函数 ⇒ `BuzzerNull`/`BuzzerHost` 与既有调用点**零影响** |

文件清单（**只有这些**）：

| 文件 | 改动 |
|---|---|
| `lib/dashcore/buzzer_exio.h` / `.cpp` | **新增**：真机驱动（非阻塞多相序列 + 注入的时钟/输出回调） |
| `lib/dashcore/buzzer.h` | 加一个**默认空实现**的 `virtual void tick()`（唯一的接口改动） |
| `src/dash_display_rgb.cpp` | 新增 `dash_buzzer_set()`（**唯一**写 TCA9554 的地方，走既有 `tca9554_set()`） |
| `src/dash_display.h` | 那句声明（`#if defined(DASH_DISPLAY_RGB)`） |
| `src/main.cpp` | 实例/绑定/`g_buzzer->tick()`/串口命令 `b`（全部在 `DASH_DISPLAY_RGB` 里） |
| `test/test_dashcore/test_buzzer_exio.cpp` + `test_main.cpp` | **新增 9 条** native 用例（掩码/降级/夹时长/静音/到点关/幂等/取消/begin） |
| `platformio.ini` | ★ **一个字节都没改**（门用的是既有的 `DASH_DISPLAY_RGB`） |

#### 13.7.2 设计影响（有源 ⇒ 提示音只能靠**节奏与次数**；**音量不可调**）

#### 13.7.3 本单的上板实测（COM6，原始串口行）

**判据与命令**（详见 `ARCHITECTURE.md`「显示约定」§4.3）：

* 人耳那条**只能由车主给**（机上没有麦克风）；
* 串口命令 **`b`** ⇒ 响一声短的（走 `Long` ⇒ **3 短哔**那条降级）+ 一行回执；
* 每一次写到扩展器都落一行 `buzz: exio 0xXX -> 0xXX (mask 0x01…)`
  ⇒ "有没有打到 `LCD_RST`/`LCD_CS`"从串口一眼可见；
* **显示健康四行**（`vsync +54/s` / `copy_max≈1.3ms` / `timeout=0` / `fullrb=0/s`）
  证明这一轮**没有把屏搞坏**。

原始日志见 `ACCEPTANCE.md` 2026-09-24 那一条（本单追加的那一节）。

* **可用的自由度只剩三个**：**响多久**（`beep_ms`）、**响几次**（`BeepPattern`）、
  **隔多久**（告警的最短重复间隔）。**没有频率、没有音色、没有音量**（13.6 的电路事实）。
  ⇒ 提示音语言只能设计成**节奏区分**。★ **词表已经定稿**（2026-09-24）：
  **1 短哔** / **2 短哔** / **3 短哔** / **4 短哔**（超速 `Urgent`；**"长鸣"在这块板上被禁止**，
  `Long` 降级成 3 短哔 —— 理由见 `ARCHITECTURE.md`「显示约定」§4.1 那张表与第 2 条）。
  ★ 2026-09-24 更正：这一行原先只列到 **3 短哔**，而 `pulsesFor(Urgent)` **一直是 4 声**
  （`lib/dashcore/buzzer_exio.cpp`）⇒ 词表按**代码**补齐成四档。
  本节原先写的"红区=长鸣"那条**已被否掉**，别照它实现。
* ★★ **静音开关是必须功能**（不是"最好有"）：有源蜂鸣器**较响、偏尖**（车主主观），
  而**音量不可调** ⇒ 唯一的"调轻"手段就是**少响 + 能一键关**。
  仓库里已经有这一层：`Alerts::setMuted()` + `AlertsConfig`（预览的注入里
  已经能按 `mute` 开关，见 `buzzer.h` 的文件头）⇒ 真机上的入口是**串口命令 `m`**
  （取反 + 写 NVS 掉电保存；见 §15 与 `ARCHITECTURE.md`「显示约定」§4 第 4/5 条）。
* **不要**为了"音调"再去动 EXIO8 之外的硬件（本轮没有飞线、没有改板）：除非将来**看原理图**
  确认蜂鸣器的驱动级还有别的控制脚，且那个脚是**普通 GPIO**（能走 LEDC 硬件 PWM）——
  那才有"音调"这条路，而它属于**换硬件方案**，不在本轮范围。
* `alerts` 的现有形状**够用、不要动**：`Alerts` 决定"什么时候响、什么模式"，
  `Buzzer` 决定"怎么落" —— 有源蜂鸣器只用到 `beep()/off()`/`tick()` 三个动作
  （13.7.2 那张表里说明了两处与初版方案的有意偏离）。

---

### 13.8 当晚改版（起因：车主报"**屏幕突然黑掉**"）—— 自检不再碰显示主循环、也不再碰 I2C 时钟

**症状**：车主报"屏幕突然黑掉"；同一时刻串口上**驱动侧一切正常**：

```
rgb: vsync=5900(+54/s) wrap=5900 swap=573 blit_max=1340us step_max=965us copy_max=1340 ... timeout=0 ...
alert=none beeps=27
206 dash ok spd= 14% rpm= 32% coolant=79C face=cruise/city
```

#### 13.8.1 先排除一条（**重要**，写下来免得下次认错）

**"主循环被自检阻塞"解释不了这个症状**，而且与当时的日志不符：

* **黑屏 ≠ 卡死**：主循环被卡住时，面板**还在扫那一块 framebuffer** ⇒ 屏上是**定格**
  （停在最后一帧），**不会变黑**。变黑只有两种可能：**面板丢了初始化**（ST7701 要重跑 41 步）
  或**背光没了**。两者在串口上的区别正是：**卡死 ⇒ `flush` 不涨；黑屏 ⇒ 一切都正常** ✓
  —— 当时的读数正好是后者。
* 自检进行中的日志（改版前那一版，原始行）：

  ```
  buzzer: ② 方波 2000Hz 段结束:翻转 3070 次 / 实际 1011.746ms ⇒ **实测平均 ≈1517.1Hz**(请求 2000Hz)
          事务 3072 次、平均 141us/次(= 音高上限 ≈3546Hz);跳过格点 976 个
          自检占用 753.646ms(74.4% 的段时长);主循环间隔 平均2756us/最大43401us
  rgb: vsync=620(+54/s) wrap=620(+54/s) swap=81(+5/s) flush=391 blit_max=1323us ... timeout=0 ...
  206 dash ok  spd= 99% rpm= 46% coolant=90C face=cruise/overspeed
  BEACON 12  step=7(loop: 数据已更新)  uptime=12s heap=217KB psram=8192KB flash=16MB
  ```

  ⇒ 自检 ①~④ 那 17 秒里 **`206 dash ok` 每秒一行没断**、`vsync +54/s`、`swap +5/s`、
  `timeout=0`、`blit_max ≈1.3ms` 与自检前同量级 —— **显示链当时是活的** ✓。
* **但**：那一版**确实**在主循环里按 2~3.5ms 的预算自旋（"自检占用 71~84% 的段时长"就是它），
  这**不该做**（任务书的原话是"不许忙等、自检期间 UI 必须照常"）⇒ 下面 13.8.2 第 ① 条照改。

#### 13.8.2 两条**仍然成立**的候选机制（本轮**没有**仪表能分辨，如实并列，不猜一个当结论）

| # | 机制 | 支持它的观察 | 本轮怎么处置 |
|---|---|---|---|
| **A** | **方波段把 I2C 时钟从 100kHz 提到 400kHz**（这是本次自检**新引入**的变量）⇒ 万一有一个字节被打错，而那颗 TCA9554 的输出寄存器里**同时挂着 LCD_RST(EXIO1)/LCD_CS(EXIO3)** ⇒ 一次**面板复位**，而面板复位**不会自己回来** ⇒ **黑屏 + 驱动计数全正常**（与现场完全对得上） | 三次自检里前两次都没黑屏、第三次才出现 ⇒ 与"概率性"相容；但同一段代码已跑过约 3 万次事务，**没有直接证据** | **整个去掉**：方波段也用 **100kHz**（与点屏时同一个档）—— 代价是音高上限掉到 ~1.2kHz（13.8.3） |
| **B** | **③ 持续拉高 2 秒**：蜂鸣器**连续**通电 2 秒（① 是 200ms 级短脉冲）⇒ 3.3V 轨被拉低一下 ⇒ **面板**（ST7701 内部寄存器/电荷泵）掉状态，而 **ESP32 活着** | 黑屏第一次出现，正好是 ③④ 这两段**第一次**上板（之前两版固件只有 ①②） | **保留**（它就是要问的那一问，不能为了好看砍掉）—— 但**记在案**：若再出现黑屏，**务必记下发生在哪一段**，那一条信息就能把 A 与 B 分开 |

★ **恢复办法（已实测过一次）**：**复位一次**（或重新烧写默认固件）—— 面板会重跑 41 步初始化，
黑屏即恢复。当晚回烧默认固件 + 复位后的原始行：`rgb: RGB565 480x480 pclk=15000000Hz …已就绪`、
`rgb: 双framebuffer num_fbs=2 …`、`rgb: vsync=27(+27/s) … timeout=0`、`206 dash ok` ✓。

★ 注意：13.6 的结论（**有源**）**不能**用来在 A 与 B 之间选一个 —— 两者都与"有源蜂鸣器 + 黑屏"
相容。要分开它们，需要的是一条**当时没记下来**的信息：**黑屏发生在哪一段**（② 方波段 ⇒ A；
③ 持续拉高那 2 秒 ⇒ B）。下次若要复跑，**先记这一条**。

#### 13.8.3 改版做了什么（逐条）

1. **自检整个搬进独立任务**：`xTaskCreatePinnedToCore(bs_task, "bselftest", 8192, nullptr,
   优先级 **1**, nullptr, **core 0**)`。Arduino 的 `loopTask` 在 **core 1**，LVGL/flush、
   RGB 的 bounce 填充也都在 core 1 的时间线上（第 10 节）⇒ 自检**一个微秒都不占显示主循环**；
   方波那几段就算自旋，被占的也是核 0 的空闲时间。
   （"每圈只翻一次、状态机推进"也能让 UI 活着，但那样方波会被主循环周期压到 ~166Hz —— 独立任务
   两样都要：**既不动主循环，又能把频率排准**。）
2. **运行期不再改 I2C 时钟**：`Wire.setClock()` 那一行**删掉**（源码里现在搜不到 `Wire.setClock`）。
   代价写在 13.8.4 的表里。
3. **所有等待仍是"截止时刻驱动"**：直流/持续段走 `bs_hold_until_ms()`（每 10ms `vTaskDelay` 一次），
   方波段按格点排 —— **没有一条长 `delay()`**（③ 的 2 秒也是"记下截止时刻、一小段一小段让出"）。
4. **栈给 8KB**（不是 4KB）：这个任务万一崩了就会重启 → 重启又建这个任务 = **引导环**，
   宁可多给 4KB（它只在自检那 17 秒里存在）。
5. 收尾日志加一句恢复提示：`若屏变黑,按板上 RST 复位就会重新初始化面板`。
6. 触发方式**不变**（编译开关 `-DBUZZER_SELFTEST=1`，每次上电跑一遍、跑完任务自己
   `vTaskDelete`）—— 也就是说**板上跑这一版时，前 17 秒是自检，之后就是一份普通固件**；
   但按任务书要求，**测完仍然把开关关掉、回烧默认固件**。

#### 13.8.4 改版后的实测（COM6 原始串口行）

```
buzzer: ===== 板载蜂鸣器自检开始(BUZZER_SELFTEST;任务在 core 0,与显示主循环 core 1 分开)=====
buzzer: ② 方波 200Hz 段结束:翻转 399 次 / 实际 1000.388ms ⇒ **实测平均 ≈199.4Hz**(请求 200Hz)
        事务 401 次、平均 429us/次(= 音高上限 ≈1165Hz);两次翻转最大间隔 2871us;跳过格点 0 个
buzzer: ② 方波 1000Hz 段结束:翻转 1880 次 / 实际 1001.053ms ⇒ **实测平均 ≈939.0Hz**(请求 1000Hz)
        事务 1882 次、平均 392us/次(= 音高上限 ≈1275Hz);跳过格点 121 个
buzzer: ② 方波 2000Hz 段结束:翻转 1891 次 / 实际 1000.747ms ⇒ **实测平均 ≈944.7Hz**(请求 2000Hz)
        事务 1893 次、平均 402us/次;跳过格点 2110 个
buzzer: ② 方波 4000Hz 段结束:翻转 1994 次 / 实际 1000.381ms ⇒ **实测平均 ≈996.6Hz**(请求 4000Hz)
        事务 1996 次、平均 404us/次;跳过格点 6006 个
buzzer: ③④ 持续段结束 —— EXIO8 已回低(**静音**),UI 全程没被自检占用
buzzer: 回读 TCA9554 输出寄存器=0x05(本机影子=0x05)
buzzer: 自检任务结束(要再听一遍:复位/重新上电一次即可;若屏变黑,按板上 RST 复位就会重新初始化面板)
```

| 判据 | 改版前（主循环自旋 + 400kHz） | **改版后（core 0 任务 + 100kHz）** |
|---|---|---|
| 自检占用**显示主循环** | 段时长的 **71~84%** ✗ | **0**（自检在 core 0 的任务里）✓ |
| 主循环（自检期间） | 间隔 平均 2.4~3.7ms / 最大 43ms | `vsync +54/s`、`wrap +54/s`、`swap +5/s`、`timeout=0`、`blit_max 1.52ms` —— **与自检前同量级** ✓ |
| `206 dash ok` / `BEACON` | 每秒不断 | **每秒不断**（BEACON 1~40 全在）✓ |
| heap（内部） | 217KB（自检中不变） | **自检中 208KB（任务 8KB 栈）→ 跑完回到 217KB** ✓（证明任务确实自己 `vTaskDelete` 了） |
| 方波四段实测 | 165.5 / 879.8 / 1517.1 / 1454.9 Hz | **199.4 / 939.0 / 944.7 / 996.6 Hz** |
| I2C 事务 | 141~178µs/次（400kHz） | **392~429µs/次**（100kHz） |
| 改总线时钟 | 是（400kHz）✗ | **否** ✓（源码里 `Wire.setClock` 一个都没有） |

★ **改版带来的代价要说清楚**：I2C 回到 100kHz 后事务耗时从 ~150µs 涨到 ~400µs ⇒ 2k/4k 两段
**都被封在同一档**（944.7 / 996.6 Hz），于是 ② 这段"音高阶梯"只剩 **200Hz → ~1kHz** 这一跳是
明显可听的。**这没关系**：② 本来就不定性（13.2 那张表），**定性只看 ③**（持续拉高，与 I2C 速率
无关，整段只写一次寄存器）。

#### 13.8.5 教训（**以后还会用到，放这儿最显眼**）

1. **自检 / 调试代码不得占用显示主循环的时间线** —— 本轮的做法：整个搬进
   `xTaskCreatePinnedToCore(..., core 0)` 的独立任务。**判断标准**：自检跑起来时，
   `rgb: vsync/wrap/swap/flush/timeout` 与 `206 dash ok` 必须与自检前**同量级**。
2. **不要在运行期改共享 I2C 总线的时钟** —— 那条总线上挂着**面板复位的那个位**：
   任何一次被打错的事务都可能是一次"面板复位"，而面板复位**不会自己回来**（要重跑 41 步）。
3. **"黑屏"与"卡死"是两种病**：卡死 ⇒ 屏上**定格**（`flush` 不涨）；黑屏 ⇒ 面板丢初始化
   （或背光没了）而**所有驱动计数都正常**。别拿"黑屏"当"主循环被阻塞"的证据。
4. **一次只引入一个新变量**：本轮同时引入了"400kHz"和"2 秒连续通电"两个新东西，
   出事后**没法一眼归因** —— 这是本轮真正的代价，也是上面第 1/2 条要固化下来的原因。
5. 任何**会写共享外设**的自检，都要有一条**一键恢复**的路（本轮：`pio run -e esp32s3-rgb
   -t upload --upload-port COM6` 用默认固件覆盖 + 复位 ⇒ 面板重跑初始化）。

---

## 14. 2026-09-24 构建环境：这台机器上的**四个 PlatformIO core**（"怎么才能编出真目标"）

**先看症状**（两条都是环境问题，**不是**仓库回归 ✗）：

- `SSL: CERTIFICATE_VERIFY_FAILED`，URL 是
  `…/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip`
  ⇒ 那个 core 里**没装** pioarduino 平台，PlatformIO 去 github 拿 —— 而本机对 `github.com` 的
  DNS/SSL 是坏的（见 `PINOUT`/同步那几节的同一条已知问题）。
- `Failed to install Python dependencies into penv` / `uv installation via pip failed with exit code 106`
  ⇒ 那个 core 里有一个**残缺的 `penv`**，PlatformIO 于是想"就地重装依赖"，而重装要联网。

**实测的四个 core 分工**（2026-09-24 逐个 `platforms/` 列表 + `platform.json` 版本）：

| core | `platforms/` 里有什么 | 能不能用 |
|---|---|---|
| `C:\.platformio` | `espressif32` = **55.03.39**（pioarduino）、`espressif32@7.1.3`、`native` | 平台与包**都在这里** ✓，但 `penv` 残缺 ⇒ **直接用会触发联网重装** ✗ |
| `C:\Users\Public\206dash\.pio-core` | `espressif32` = 7.1.3、`native` | 没有 pioarduino 平台 ✗；**没有 `penv`** ⇒ 用当前解释器 ✓（native 回归一直用它 ✓） |
| `C:\Users\张九思\206Dash\.pio-core` | `native` | 只够 native 回归 ✓ |
| `C:\Users\张九思\.platformio` | （没有 `platforms/`） | 空壳 ✗ |

**可用做法：用 junction 拼一个"只读组合 core"** —— **不改任何既有 core 的一个字节** ✓：

```powershell
$core='C:\206dash-scratch\pio-core-mix'
New-Item -ItemType Directory -Force "$core\platforms" | Out-Null
New-Item -ItemType Junction -Path "$core\platforms\espressif32"       -Target 'C:\.platformio\platforms\espressif32'       | Out-Null
New-Item -ItemType Junction -Path "$core\platforms\espressif32@7.1.3" -Target 'C:\.platformio\platforms\espressif32@7.1.3' | Out-Null
New-Item -ItemType Junction -Path "$core\platforms\native"            -Target 'C:\Users\Public\206dash\.pio-core\platforms\native' | Out-Null
New-Item -ItemType Junction -Path "$core\packages"                    -Target 'C:\.platformio\packages'                    | Out-Null
# ★ 故意**不**挂 penv:挂了就又变成"就地重装依赖" ⇒ 用当前解释器才对
```

```powershell
$env:PLATFORMIO_CORE_DIR='C:\206dash-scratch\pio-core-mix'
$env:PYTHONPATH='C:\Users\张九思\206Dash\.pio-pylibs'   # ★ `platformio` 这个包在这里(机器上的 python 里没有它)
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
cd C:\206dash-scratch\Neru-sys                        # HEAD 的 ASCII 副本
python -m platformio run -e esp32s3-rgb
```

**实测数字**（2026-09-24，`Neru-sys` = 提交 `ca08467` 的 ASCII 副本，203 s）：`SUCCESS`、
**RAM 38.3% / 125,416 B**、**Flash 86.0% / 901,699 B**（Flash 还剩 14% ✓）。

**两条判据纪律**：

1. PlatformIO 在这台机器上**成功也返回非 0 退出码** ⇒ **看 `SUCCESS` 行，不看退出码**
   （`robocopy` 的 1/3 同理：1 和 3 都是成功 ✓）。
2. 忘了 `PYTHONPATH` 会报 `No module named platformio` —— **不是**仓库问题 ✗。

### 14.1 另外两条环境坑（2026-09-24 实测补记）

**① 本机是 Windows PowerShell 5.1，`pwsh` 不存在** ⇒ 脚本/命令有两处后果：

* `pwsh -Command` 那条路走不通；**所有命令都得能在 5.1 上跑**
  （`$PSVersionTable.PSVersion` 实测 = `5.1.26100.9444`，`Get-Command pwsh` 无结果）。
* ★★ **无 BOM 的 `.ps1` 会被按 GBK 解码** ⇒ 只要脚本里出现**中文**（注释、
  字符串、日志），解析就会乱码甚至报语法错。规矩：**仓库里的 `.ps1` 一律纯 ASCII**，
  或者存成**带 BOM 的 UTF-8**。
  ★ 同一条坑在**别处**也咬过：`partitions*.csv` 被 PlatformIO 用系统代码页（GBK）
  解码（见那两张表的文件头）—— 所以**给工具吃的文本文件，先问一句"它按什么编码读"**。
* 顺带：**`Get-Content` 读 UTF-8 文档会显示成乱码**（它按 GBK 解）⇒
  要读 `README.md`/`ACCEPTANCE.md` 这类中文文档，用能指定编码的方式
  （`Get-Content -Encoding UTF8`），**不要**用它来核对文字内容。

**② `.pio/libdeps` 里没有 `pcpreview` ⇒ 直接编会去联网装 lvgl 然后卡死**：

* 症状：`platformio run -e pcpreview` 停在依赖解析那一步**很久不动**
  （本机对 `github.com` / PlatformIO registry 的网络是坏的，见 §14 开头两条症状）。
* 为什么：`lib_deps = lvgl/lvgl@^9.3.0` 是**按 env 分别装**到
  `.pio/libdeps/<env>/` 的；**新开的 ASCII 副本**（或刚 `pio clean` 过的）
  那一格是空的 ⇒ PlatformIO 必须联网把它装下来。
* **修法 = 先"播种"**：从一个**已经编过 pcpreview 的副本**把 `.pio` 拷过来
  （`.pio` 里有 `libdeps/pcpreview/lvgl` 与 `build/pcpreview/` 的中间产物）：

  ```powershell
  robocopy C:\206dash-scratch\Neru-pcp2\.pio C:\206dash-scratch\<新副本>\.pio /MIR /NFL /NDL /NJH /NJS /NP
  # robocopy 的 1 / 3 都是成功
  ```

  （2026-09-24 实测：播种后 `pcpreview` 增量构建 **6~7 秒**；
  不播种则卡在联网那一步。）
* ★ 这只对 **native 平台**的 env（`pcpreview` / `native`）有效；
  `esp32s3-rgb` 的框架包在 `pio-core-mix` 的 `packages` 里（§14 的 junction 组合 core）。

**③ `core.autocrlf = true` ⇒ 新文件在检出/复制时是 CRLF，"数行数"要用字节**（2026-09-24 补）：

* 本仓库 `git config core.autocrlf` 实测 = **`true`**，而工作区里既有的文件
  大多是 LF（历史遗留）⇒ **同一棵树里两种行尾并存**，`wc -l` / `Measure-Object -Line`
  给出的"行数"会随文件不同而差一截。
* ⇒ 报告/文档里凡是要给"**这个文件多少行**"这类数字，一律用
  **`(Get-Item <f>).Length` 的字节数**（或 `git show :<f> | wc -c` 那种按索引取的字节数），
  不要数行 —— 数行会把 CRLF 与 LF 的差别算进去。
* 顺带一条同源的经验：**判断"这个文件被改过没有"用 `git status` / `git diff --stat`**，
  别用"行数变了我没变"去推（行尾归一化会让没改的文件看起来在 diff 里）。

**④ 跑 native 用例时 `PATH` 上必须挂 zig 转发桩**（这一条踩过两次）：

* 症状：`'gcc' is not recognized as an internal or external command` ⇒ 构建直接
  `[ERRORED]`，而**不是**仓库的问题。
* 修法：`$env:PATH='C:\206dash-scratch\zigbin;' + $env:PATH`
  （那一组 `gcc.cmd`/`g++.cmd`/`cc.cmd`/`c++.cmd` → `zig cc`；zig 本体在 `C:\ziglang\zig.exe`，
  全局缓存在 `C:\206dash-scratch\zigcache`）。

## 15. 2026-09-24 诊断页 / 静音的真机入口 = **串口命令 `d` / `m`**（不接按键）

**为什么是串口，不是按键**：**这块板上没有可用的按键** —— 能当输入用的只剩 12PIN 的
`GPIO0`（= BOOT strap，**不建议**）与排针上剩下的 `GPIO7`（I2C 的 SCL）。
而 Type-C 口（板载 CH343P = `Serial0`/UART0 = `COMx`）**本来就是看日志要接的那一根线**
⇒ 用串口单字符命令当下入口：**零额外引脚、零额外接线**，与"插着 USB-C 看串口"
是同一类行为（诊断页的 L11 合规说明见 `ARCHITECTURE.md`「显示约定」§3）。

| 命令 | 作用 | 回执（原始串口行） |
|---|---|---|
| **`d`** | 诊断页：与预览的 `K` **同一个循环** —— 关着 ⇒ 打开第 1 页；还有下一页 ⇒ 翻页；在最后一页 ⇒ 关闭 | `diag: open page=1/2` / `diag: closed page=2/2` |
| **`m`** | **静音开关取反 + 写 NVS**（掉电保存）⇒ 车主**不用按键**就能静音 | `mute: 1 (saved)` |

**三条边界（都写进代码注释了，这里留一份）**：

1. **只编进 `[env:esp32s3-rgb]`**：门用的是**既有的** `DASH_DISPLAY_RGB` 宏 ——
   `platformio.ini` 里**早就有**、且**只有**那一条 env 定义它 ⇒ `[env:esp32s3]` /
   `[env:esp32dev]`（**VAN 抓帧盒**）以及 `extends` 它们的 `-vaninv` / `-vansniff`
   的编译单元里**这段代码一行都不存在**，行为逐字节不变 ✓。
   ★ **`platformio.ini` 没有新增任何 `-D`**（能不新增就不新增）。
2. **命令只认"行首"**（该口当前没有未完成的一行）：回放行里的十六进制**本来就可能
   含 `d`**（例 `VAN 824 18F8271D000000`）⇒ 不这么判会悄悄吃掉回放数据。
   认不出来的字节**原样**交给既有的 `van_replay_feed()` 路径，一个字都不多吃。
   **唯一的例外**：缓冲里那串字节**不可能是回放行**（首字节不是 `V`/`v` —— 回放行的
   头三个字符必须是 `VAN`，见 `van_replay.cpp`）⇒ 那多半是串口线上的一颗**杂散字节**，
   这时也认命令并把它清掉；否则那颗字节会把命令通道**堵到下一个换行为止**
   （"按了没反应"是最难查的一类现象）。2026-09-24 上板用一颗 `x` 验过这条路径：
   先发 `x` 再发 `m` ⇒ `mute: 1 (saved)` 照常出来，且**没有** `VAN? x` 回显。
3. **两个口共用同一个判据**：`Serial`（原生 USB-CDC，要 12PIN 引线才够得着）与
   `Serial0`（板载 CH343P = 唯一的 Type-C = `COMx`）都认这两个命令，各自维护行缓冲。

★ **掉电保存的验证方法**（不用按键，两条命令就够）：发 `m` ⇒ 看到 `mute: 1 (saved)`；
**复位或断电重上** ⇒ 开机后诊断页第 1 页最后一行是 `mute=1`（`setup()` 里读回 NVS）。
再发一次 `m` ⇒ `mute: 0 (saved)`，恢复有声。

★ 当天的**上板实测原始串口行**见 `ACCEPTANCE.md` 2026-09-24 那一条（角标 / 诊断页 `/`
静音持久化 / `rgb:` 四行 / `206 dash ok`）。

★ **诊断页的可读性**（2026-09-24 晚，起因是车主报"屏幕为什么黑了"）：底色、描边、
标题字号、ASCII-only、几何自证日志、以及"打开即第 1 页"这几条**都写在
`ARCHITECTURE.md`「显示约定」§3 的「诊断页的可读性约定」**里（那是显示约定的家）。
这里只留一句结论：诊断页的文本**必须 ASCII**（本构建只使能 Montserrat，
没有 CJK 字形 ⇒ 中文是"一个字形都画不出来"，不是"看不清"）。

---

## 16. 2026-09-24 把**主题 / 图片**刷进数据分区（不重编固件就能换外观）

**为什么需要这一节**：换一套配色或一套表情**不需要动 app** —— 它们住在两个
**数据分区**里（`theme` 与 `image`），刷这两块比重编重刷固件快得多，而且
**不会碰**正在跑的固件。

### 16.1 偏移与大小**只能从 `partitions-s3.csv` 读**（不许猜、不许用 4 MB 那张表）

```powershell
# 本机（S3 N16R8 = 16MB）真正生效的是 partitions-s3.csv ——
#   [env:esp32s3-rgb] extends [env:esp32s3]，而那里写着 board_build.partitions = partitions-s3.csv
Select-String -Path partitions-s3.csv -Pattern '^theme|^image'
```

| 分区 | Offset | Size | 谁读它 |
|---|---|---|---|
| **`theme`** | **`0x210000`** | `0x4000` = 16 KB | `esp_partition_find_first(DATA, 0x40, "theme")`（`src/theme_load.cpp`） |
| **`image`** | **`0x254000`** | **`0x800000`** = 8 MB | `esp_partition_find_first(DATA, 0x41, "image")` + `esp_partition_mmap`（`lib/themetool/image_blob.cpp`） |

★★ **绝对不要用 `partitions.csv`（4 MB 那张）的数字**：它的 `image` 只有
**1 MB**（`0x254000` / `0x100000`）⇒ 一份 1.8 MB 的镜像在那里"装不下"，
照着它算会得出错误的结论。两张表的 `theme`/`image` **偏移故意保持一致**
（0x210000 / 0x254000），差的只有 `image` 的**大小**。
（这条写在 `partitions-s3.csv` 的文件头注释里。）

### 16.2 步骤（**先备份，再刷**）

```powershell
# ① ★ 先 dump 板上现有的两份 —— 车主手写的主题/表情**没有别的副本**
python -m esptool --chip esp32s3 --port COM6 --baud 921600 read_flash 0x210000 0x4000   C:\206dash-scratch\backup-theme.bin
python -m esptool --chip esp32s3 --port COM6 --baud 921600 read_flash 0x254000 0x800000 C:\206dash-scratch\backup-image.bin
Get-FileHash C:\206dash-scratch\backup-theme.bin, C:\206dash-scratch\backup-image.bin -Algorithm SHA256

# ② 核对大小（文件必须小于等于分区大小；大了 esptool 会写出去、压掉隔壁分区）
Get-Item .\theme.json, .\image.bin | Select-Object Name, Length

# ③ 写（两个分区一次写完最省事），结尾 hard_reset 让固件重新读分区
python -m esptool --chip esp32s3 --port COM6 --baud 921600 --after hard_reset write_flash `
    0x210000 .\theme.json `
    0x254000 .\image.bin
```

★ `esptool` 用 `C:\206dash-scratch\pio-core-mix\penv` 那一套（5.3.0）；
本机 `PATH` 上的 python 里没有它，用 `python -m esptool` 之前先按 §14 设好
`PLATFORMIO_CORE_DIR` / `PYTHONPATH`。

### 16.3 怎么确认真的生效了（看**开机那几行**，不用问人）

固件在 `setup()` 里加载这两块，各打一行（`grep` 这两句就能确认）：

| 日志行 | 出处 | 说明 |
|---|---|---|
| `theme: 已加载 <N> 字节` 或 `theme: 用默认主题` | `src/theme_load.cpp` | 主题分区读到了/没读到 |
| `image ok: <N> 张,数据 <B> 字节,镜像 <S> 字节` | `lib/themetool/image_blob.cpp` | 图片分区解析成功；`<S>` 是**分区大小**（8 MB），不是文件大小 |
| `image: 镜像无效或未刷入,不用图片资源` | 同上 | 分区是空的（全 `0xFF`）⇒ **正常情况，不是错误**，界面走降级路径（程序化表情） |

★ 这几行都在 `dash_ui_init()` 之后、`206 dash ok … face=…` 那行之前 —— 它们是
"这块分区到底被认成什么"的**唯一**权威出口，而 `face=` 那一格证明
**表情状态机开始按新资源走**。

### 16.4 出错时怎么回退

* **只有数据分区坏了** ⇒ 把 16.2 ① 的备份写回去（同一条 `write_flash` 命令）。
* **固件也一起坏了/黑屏花屏** ⇒ 先把 app 分区写回上一份已知可用的镜像
  （`write_flash 0x10000 <那个 .bin>`），**保留现场日志**再排查。

---

## 17. 2026-09-24 第七轮："**刷新发肉**"先量化，再动刀（每秒脏区 + 读数下移 16px）

**这一单的硬约束（照办，一条没破）**：**先量后改** —— 在没有"每秒脏了多少"两行
数字之前，**不许**碰 bounce、PCLK、`num_fbs`（撕裂/残留那三轮已经收口的东西）；
不许重写 RGB 驱动、不许新渲染引擎、不许为读数再链一套完整 LVGL 字体；
`theme.json`（车主资产）不碰；**只烧 COM6**，COM7 一次都没打开。

### 17.1 第 1 步：把"脏了多少"打成两行（**诊断，不改行为**）

口径写在 `lib/dashcore/flush_stats.h`（新增，真机与预览**共用同一份**）：
LVGL 只把"它认为脏了的"区域交给 flush ⇒ **把 flush 区域的面积加起来**就是
"这一秒 LVGL 脏了多少像素"，不必去 hook LVGL 内部。

```
rgb: 脏区/s inv=<面积和>px2(=一屏的 x%) flush=<次数>/s(累计<总数>) fmax=<w>x<h>(一屏的 x%) 单屏=230400px2
```

* ★ 上面那一行 `rgb: vsync=…` 的格式**一个字节都没动**（它被多处引用），
  这一行是**另起**的第二行；预览端同格式、前缀是 `preview:`。
* ★ 真机上 `fmax` **永远到不了 480×480** —— 它受"绘制缓冲一行多大"限制（见 17.3 的
  实测发现）。真正能暴露"整屏失效"的是**预览端**（它的绘制缓冲是整屏大小）。

**（a）静画（pcpreview 2.8C 档；注入把速度钉在 100↔101、转速钉在 8000，
只有末位数字在跳，表盘/表情不动）** —— 原始行：

```
（前，digit_cy=72 / unit_cy=107）
preview: 脏区/s(左右两屏合计) inv=96867px2(=单屏的 42.0%) flush=23/s(累计172) fmax=480x78(单屏的 16.2%)
preview: 脏区/s(左右两屏合计) inv=97618px2(=单屏的 42.3%) flush=23/s(累计195) fmax=480x78(单屏的 16.2%)
preview: 脏区/s(左右两屏合计) inv=82715px2(=单屏的 35.9%) flush=22/s(累计217) fmax=480x78(单屏的 16.2%)
（后，digit_cy=88 / unit_cy=110 —— 只有几何变了，脏区**一模一样**）
preview: 脏区/s(左右两屏合计) inv=98117px2(=单屏的 42.5%) flush=25/s(累计174) fmax=480x78(单屏的 16.2%)
```

**（b）扫表（同一次运行的头两秒：整屏首帧 + 开机扫表）**：

```
preview: 脏区/s(左右两屏合计) inv=884259px2(=单屏的 383.7%) flush=104/s(累计104) fmax=480x480(单屏的 100.0%)
preview: 脏区/s(左右两屏合计) inv=576098px2(=单屏的 250.0%) flush=45/s(累计149) fmax=147x332(单屏的 21.1%)
preview: 脏区/s(左右两屏合计) inv=96867px2(=单屏的 42.0%) flush=23/s(累计172) fmax=480x78(单屏的 16.2%)   ← 稳态
```

**（a'）静画（真机 COM6 —— 板上没有注入通道，这一档是"稳态"：Sim 慢波驱动，
数字每拍都在跳、表盘缓慢移动）**：

```
rgb: 脏区/s inv=199663px2(=一屏的 86.6%) flush=31/s(累计214) fmax=480x24(一屏的 5.0%) 单屏=230400px2
rgb: 脏区/s inv=210524px2(=一屏的 91.3%) flush=32/s(累计246) fmax=480x24(一屏的 5.0%) 单屏=230400px2
rgb: 脏区/s inv=197296px2(=一屏的 85.6%) flush=31/s(累计277) fmax=480x24(一屏的 5.0%) 单屏=230400px2
```

**（b'）扫表（真机开机那一秒 + 第二秒）**：

```
rgb: 脏区/s inv=234876px2(=一屏的 101.9%) flush=32/s(累计32)  fmax=480x24(一屏的 5.0%) 单屏=230400px2
rgb: 脏区/s inv=468524px2(=一屏的 203.3%) flush=89/s(累计121) fmax=240x48(一屏的 5.0%) 单屏=230400px2
（同一秒的 `rgb: vsync` 那行里 `fullrb=1/s` ⇒ 那一秒确实有**一次整屏刷新** = 开机首帧）
```

### 17.2 第 2 步：谁在全屏重画（**诊断**）—— 评审的假设**被推翻**

| 查什么 | 结论 | 证据（可复核） |
|---|---|---|
| 有没有 `full_refresh` / `LV_DISPLAY_RENDER_MODE_FULL` | **没有**（默认档） | `src/dash_display_rgb.cpp` 的 `lv_display_set_buffers(…, LV_DISPLAY_RENDER_MODE_PARTIAL)`；`g_full_refresh = (RGB_FULL_REFRESH_DEFAULT != 0)`，而 `platformio.ini` 写着 `-DRGB_FULL_REFRESH_DEFAULT=0` 与 `-DRGB_FULL_REFRESH_ALTERNATE_MS=0`；实测 `fullrb=0/s`（除开机那一秒 `1/s`） |
| 有没有每拍 `lv_obj_invalidate(scr)` | **没有**（那一句在 `if (g_full_refresh)` 里，默认档不执行） | `src/dash_display_rgb.cpp` 里 `lv_obj_invalidate(lv_screen_active())` 只有一处，就在那个 `if` 里面 |
| 底图 / 弧的**轨道**是不是每帧重画 | **不是**：底图与轨道只在建屏时画一次（轨道角度此后一次都不改） | `build_arcs()` 只在 `dash_ui_init()` 里调；此后每拍只动 `lv_arc_set_start/end_angle`（INDICATOR 那一段） |
| 四条弧的 `arc_set_progress()` 是不是每拍都调 | **是**（每拍、每条弧都调） | `src/dash_ui.cpp` 的 `dash_ui_render()` → `arc_set_progress(ui.arcs[i], a, ui.arc_cur[i])` |
| ★ **评审的假设**："`lv_arc_set_*` 会整块失效该 arc 的包围盒（≈410×410）⇒ 四条弧每拍各脏 1/4 屏" | **推翻** ✗ | LVGL 9.3 的 `lv_arc_set_angles()`（`.pio/libdeps/*/lvgl/src/widgets/arc/lv_arc.c`）**只失效"变化的那一段角度"**：它取新旧角度的差值区间，交给 `inv_arc_area()` → `lv_draw_arc_get_area()` 算**扇形外接矩形**。退回"整块"只有两条路：① 单步角变化 **>180°**；② 扇形**跨 2 个以上象限**（`lv_draw_arc.c` 的最后一个 `else` 才是整个包围盒）。稳态实测也印证：`fmax` 里**从来没有出现过 410×410**（静画 `480×78`、开机 `147×332`），而 `inv` 只有 ~86%/s ⇒ **不存在"四条弧加起来接近整屏"这回事** |

**真正的元凶（同一批数字指着它）**：**读数标签是"整屏宽"的**。

* `src/dash_ui.cpp` 的 `make_readout_label()`：`lv_obj_set_width(l, LV_PCT(100))`
  ⇒ 标签对象宽 **480**（= 整屏宽），文字只是居中画在里面；
* `lv_label_set_text*()` 会失效**整个标签** ⇒ 数字一变就脏一条 **480×52** 的带；
  单位标签同理（480×20）⇒ 两条带合并成 **480×78 = 一屏的 16.2%**
  （实测值，静画/稳态**每一秒都是这个数**）；
* 而 UI 是 **200 ms 一拍**（5 次/秒）⇒ `5 × 16.2% ≈ 81%`，
  与真机实测的 `inv ≈ 86%` **对得上** ✓。弧的贡献只剩几个百分点。
* ⇒ "整表闪一刀"的**正确解释**是：**每个刷新都重画一条横跨整屏宽度的读数带**
  （真机上再被切成 5 块 480×24 依次搬过去），不是"整屏重画"、也不是"弧的包围盒"。

★ **顺带量出来的一条（写下来，别当成故障）**：真机单次 flush 是 **480×24**，不是
文档里写的"16 行"。原因：`lv_display_set_buffers()` 的 `buf_size` 收的是**字节**，
而 `draw_buf[]` 的元素是 `lv_color_t`（LVGL 9 里 **3 字节/个**）⇒
`480×16×3 = 23,040 B`，而 PARTIAL 模式按 `h = buf_size / stride`（stride = 480×2）
算出 **24 行** ⇒ 单笔数据 **23,040 B**（不是 15,360 B）。
⇒ §12 那句"15.36KB < bounce 19.2KB ⇒ 这一笔能被吸收"的**前提要按 23.04KB 重算**
（这一条**只作诊断记录**：绘制缓冲的行数是**已收口那一单**的交付参数，本轮**没有**动它 ✗）。

### 17.3 第 3 步：读数下移 16px（**先量后改**，判据是像素）

| 指标（480 基准；`tools/theme-editor/check-readout-clearance.js`，逐像素扫全屏） | 改前 | 改后 |
|---|---|---|
| **被数字压住的弧像素**（左屏，rpm=8000，4 位数字） | **358** | **0** |
| **被数字压住的弧像素**（右屏，speed=100，3 位数字） | **197** | **0** |
| 大数字墨迹 y 区间 | `[55..88]` | `[71..104]` |
| 弧带上"看得见的弧"像素（左屏，对照） | 8142 | 8137 |

* 新坐标：`digit_cy 72 → 88`、`unit_cy 107 → 110`（480 基准，经 `theme_scale()` 落 240）。
  两个数是**解不等式**解出来的：数字墨迹顶(`cy-17`) 必须低于弧带内沿在**同样 x**
  上的高度(70) ⇒ `cy ≥ 87`；单位墨迹下沿(`cy+10`) ≤ 表情图名义顶边(120) ⇒
  `unit_cy ≤ 110`，而它又要 `> digit_cy+19`（数字墨迹下沿必须在单位墨迹上沿之上）
  ⇒ **110 是唯一同时满足两头的值**。
* **弧半径、251° 扫角、字号一个都没动** ✗（那是另一单）。
* 同步的镜像（少一处"预览代表真机"就不成立）：`ui_theme.h`（唯一事实来源）、
  `theme-default.json`、`image-editor.html` 的 `READOUT_LAYOUT`、`index.html` 的默认主题、
  `test-theme-json.js` 的硬编码期望、`check-preview-frame.js` 的两条读数带
  （★ 扫描窗口边界是**量出来的**：数字墨迹+抗锯齿到 104 为止、单位从 107 起 ⇒
  窗口在 105 分开；下边界停在 136 —— 表情(白)从 y=140 起，扫过去会把脸当数字）。

### 17.4 第 4 步：数字图集 —— ★ **没有做，按任务书"停下来报车主"** ✗

任务书写明"flash 目标持平或下降；**若砍不掉对应的 LVGL 字体导致 flash 上涨 ⇒
停下来报我，不要硬上**"。查清楚了，**砍不掉**：

| 谁在用那两档字 | 出处 | 能不能砍 |
|---|---|---|
| `m48`（大数字档） | 读数大数字 **+ 诊断页标题/页码**（`ARCHITECTURE.md`「显示约定」§3 的可读性约定 ③ 明确要求"标题与页码用大数字那一档 48 号"） | ✗ 砍了诊断页就违反另一条已定稿的约定 |
| `m18`（单位/副表档） | 读数单位/水温/进气 **+ 诊断页正文**（两页都是它排的） | ✗ 同理 |
| `m24` / `m10` | `dash_ui.cpp` 的 `diagFontTag()` 做**指针比较**（`240` 档那套） | ✗ 参与编译 |

⇒ 新增一份 A8 数字图集（`0-9`+`.`+空格，48px）**只会让 flash 上涨**（量级 ~19KB），
而"持平或下降"做不到 ⇒ **本轮不实现**，如实报上去等裁决。

★ **但同一件事有一个"零 flash"的做法，本轮已经量过（只在副本里试，未提交）**：
把读数标签从"整屏宽"改成"**内容宽**"（删掉 `lv_pct(100)` 那一行，其余不动）：

```
（静画，pcpreview；两屏合计）
改前： fmax=480x78(单屏的 16.2%)  inv=82715~98117px2(=单屏的 35.9~42.5%)
改后： fmax=115x78(单屏的  3.8%)  inv=25751~27448px2(=单屏的 11.1~11.9%)
```

—— **一行都不新增、flash 一个字节不涨**（是**删**一行），而"横跨整屏的读数带"
这件事就没了（16.2% → 3.8%）。它**不是**数字图集（LVGL 仍然重画整个数字串），
但把"整表闪一刀"的**形状**修掉了。**要不要上，等车主/评审一句话**。

### 17.5 第 5 步：`Urgent` 词表按**代码**对齐（文档原先写 3 声）

* 事实来源：`lib/dashcore/buzzer_exio.cpp` 的 `pulsesFor()` —— `Urgent` **push ×4**；
  native 用例 `test_buzzer_exio.cpp` 用 `ASSERT_EQUAL_UINT8(4, …)` 钉着；
  `alerts.h` 的枚举注释也写着"四短（超速）"。
* ⇒ **代码对、文档错**，改的是三处文档：`ARCHITECTURE.md` §4.1 的词表
  （`Urgent` 单列成"**4 短哔**"一行，并把"听起来一样"改成"只差一声"）、
  `buzzer_exio.h` 文件头与 `pulsesFor()` 上方那两句注释、
  `docs/RGB-PANEL-2.8C.md` §13.7.1 的词表（补第 4 档）。
* `alerts.*` / `buzzer.*` / 用例 / `platformio.ini` **一个字节没动**。

### 17.6 回归与红线复核

| 项 | 数字 |
|---|---|
| native（挂 zig 桩） | **266 test cases: 2 skipped, 264 succeeded**（0 失败，与基线逐位相同） |
| JS 八套 | **95 / 105 / 261 / 429 / 71 / 372 / 259** + `syntax 42`（全部与基线相同） |
| `check-preview-frame.js`（左屏改后帧） | 5 FAIL / 17（**调色板不匹配**那几条，与本轮改动无关；读数两条现在都是 OK） |
| `check-layer-order.js` | `LAYERS-OK`（弧可见 8/8、读数在最上层） |
| Flash | 基线 **904,783 B**（86.3%）→ 诊断 **905,059 B**（+276 B）→ 读数下移 **905,059 B**（同一字节数） |
| RAM | 125,496 B → **125,512 B**（+16 B） |
| 串口 `b` 的自证 | `buzz: exio 0x05 -> 0x85 (mask 0x80)` / `0x85 -> 0x05` —— **只有 bit7 在动** ✓ |

* `platformio.ini` **一个字节没动** ✓；`tools/theme-editor/theme.json`（车主资产）**未碰** ✓；
  **只开 COM6**、COM7 一次都没打开 ✓；没跑 `pio clean`、没删 `.pio` ✓；
  没新开 L 号 ✓；温度筛查 / BLE / 第二块板链路 / `gpio17↔18` 回环 **都没动** ✓。
* 收尾：板子停在**表盘**（复位后不再发 `d`），`mute=0`（有声，与开工一致）。

### 17.7 还没做 / 拿不准的（如实列）

1. **数字图集（任务书第 4 步）：没做** —— 理由是 flash 会涨且砍不掉对应字体（见 17.4），
   按任务书"停下来报"处理；零 flash 的替代做法已经**量过**（16.2% → 3.8%），等裁决。
2. **"整表闪一刀"消没消、外弧还挡不挡数字：只有车主/朋友能判**（机上没有相机）。
   客观侧已经给到：静画时最大脏矩形从"横跨整屏"变成"内容宽"这件事**还没上板**
   （17.4 那个改动未提交 ⇒ 板上现在仍是 480 宽的读数带）。
3. **真机的"静画"是稳态（Sim 慢波）而不是"钉住的静画"**：板上没有注入通道
   （注入只在 pcpreview 里编），要做到"真机上把速度钉住"得走 VAN 回放喂帧 ——
   本轮**没有**做那一条，如实记。
4. **`fullrb=1/s` 只出现在开机那一秒**（= 首帧整屏），第 2 秒起 `fullrb=0/s`
   ⇒ "告警闪会整屏失效"这条**仍然不成立**（与 §11.6 的结论一致）。
