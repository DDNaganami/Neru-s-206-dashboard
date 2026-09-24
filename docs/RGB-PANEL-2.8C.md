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
（非触控例程的 `ESP_PANEL_LCD_RGB_FRAME_BUF_NUM` 是 1、触控版是 2 —— 那是 S3 侧帧缓冲数量的差别，
与本项目无关：这一版 esp_lcd 是旧 API，`num_fbs` 这个字段**根本不存在**，见第 5 节。）

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

**★ PCLK 为什么是 18MHz 而不是 30MHz**（实测换来的，见第 5 节）：

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

## 9. 缓冲与**扫描同步**策略（2026-09-24 实测收口）

这一节回答"framebuffer 在哪、几个、以及**什么时候**往里写"——撕裂/花屏那两条都出在这里。

### 只有一块 framebuffer，且在 PSRAM

| 项 | 实际 | 说明 |
|---|---|---|
| framebuffer 块数 | **1** | 这份 esp_lcd 的 `esp_lcd_rgb_panel_config_t` **没有** `num_fbs` 字段 ⇒ 只能一块 |
| framebuffer 在哪 | **PSRAM**（`flags.fb_in_psram = 1`） | 480×480×2B = **450KB**，内部 SRAM 塞不下 |
| bounce buffer | **没有** | 头文件里**没有** `bounce_buffer_size_px` ⇒ DMA 只能直接从 PSRAM 读 |
| vsync 回调 | **没有** | `nm` 全库只有 `esp_lcd_panel_io_register_event_callbacks`（那是 panel IO 的，不是 RGB panel 的） |
| 唯一的同步信号 | `on_frame_trans_done`（每帧一次，64.7Hz） | 也就是"帧结束"那一下 |
| LVGL 绘制缓冲 | **内部 SRAM**，480×40（38.4KB，`aligned(LV_DRAW_BUF_ALIGN)`） | 只做渲染工作区 |
| flush 落点 | `esp_lcd_panel_draw_bitmap()` → 直接 memcpy 进那块 PSRAM fb | 另有 `Cache_WriteBack_Addr`（反汇编确认，见第 5 节） |

### 写入时机：等"帧结束"，再**按块**写

`rgb_flush_cb()` 里做两件事：

1. **等帧结束**（`wait_frame_boundary()`）：微秒级自旋等 `g_frame_done`（20ms 兜底）。
   ★ 必须**微秒级** —— 消隐期只有 0.85ms，`delay(1)` 那种毫秒级轮询最坏晚 1ms 才醒，
   那已经扎进有效像素里了（2026-09-24 实机：**花屏没了、但撕裂还在**，就是这一步的粒度问题）。
2. **按块写**：每次最多 `RGB_FLUSH_MAX_BYTES = 8192` 字节（按区域宽度折算成行数），
   **一块一块地等各自的消隐期**。8KB 这个数是实测倒推的：实测 19200B 要 1309µs
   （≈14.7MB/s，一边写 PSRAM 一边被 DMA 读），已经超过 0.85ms 的窗口；
   8KB 约 560µs，占窗口 66%，留了三分之一余量。

**实测判据（每秒那行）**：`sync == chunk` 且 `timeout = 0` ⇒ 每一次写 fb 都真的等到了帧结束；
`copy_max = 561µs < 850µs` ⇒ 每一次写都落在消隐期之内。

### 这套做法的边界（写清楚，别指望它包打天下）

* **局部刷新**（UI 常态：弧段、读数）：完全干净 ✓。
* **整屏更新**（开机动画、换背景图）：450KB ÷ 8KB = **55 块 ≈ 0.85 秒**。
  慢，但**不花不撕** —— 这是"单 framebuffer + 无 vsync 翻转"下换来的必然代价。
* ⇒ 想又快又干净，只剩一条路：**换到带 `num_fbs` / `on_vsync` 回调的 esp_lcd**
  （pioarduino 新平台或直接用 ESP-IDF），那样才能"画在另一块 fb 上、vsync 时翻转"。
  影响面：`lib/link` 的 UART PHY、`dash_log.h` 的 `DASH_LOG_UART0`、本驱动的
  `esp_lcd_panel_io_spi` 那段（新平台里 3 线 SPI 的写法也要一起换）——
  **本轮没做**，留作下一步（属于"换构建"级别的改动）。