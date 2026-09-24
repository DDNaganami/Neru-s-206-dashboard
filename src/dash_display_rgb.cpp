// ============================================================
// 真实 RGB 并口屏驱动(480×480,ST7701)—— **双 framebuffer + vsync 边界换帧**
//
// 编译开关:`-DDASH_DISPLAY_RGB=1`(见 platformio.ini 的 [env:esp32s3-rgb])。
// 与桩驱动/预览驱动共用同一个接口(dash_display.h 的三个函数),所以
// dash_ui.cpp 一行都不用改 —— 这正是当初把它抽成接口的目的。
//
// ------------------------------------------------------------
// ★★ 2026-09-24:这一版是**换栈之后**的驱动(撕裂的根治)
//
// 上一轮把撕裂的**结构性根因**查清了(记录在 docs/RGB-PANEL-2.8C.md 第 5/9 节):
//   旧栈(官方 espressif32 7.1.3 = arduino-esp32 **2.0.17** / IDF 4.4 系)自带的
//   `esp_lcd_panel_rgb.h` 是**旧的精简版**(129 行),只有
//     · 单个 `cfg.on_frame_trans_done` 回调
//     · 一块由驱动分配的 framebuffer(`esp_lcd_panel_draw_bitmap` 往它里面 memcpy)
//   **没有** `num_fbs` / `esp_lcd_rgb_panel_get_frame_buffer()` /
//   `esp_lcd_rgb_panel_register_event_callbacks()` / bounce buffer
//   ⇒ 只能"一边扫描一边往同一块 fb 里写",**撕裂是结构性的**,再怎么调时机都是
//     "把缝挪到别处"(上一轮实测:8KB 定额 + 每块等消隐期 ⇒ 从花屏变成
//     "一条横扫的缝",整屏刷新还要 0.85 秒)。
//
// 现在这份平台是 **pioarduino espressif32 55.03.39 = arduino-esp32 3.3.9 +
// ESP-IDF 5.5.4**(只换 [env:esp32s3-rgb] 这一条 env,见 platformio.ini 那段),
// IDF 5.5 的 `esp_lcd_panel_rgb.h` 有那三样 ⇒ 换成**双缓冲 + vsync 换帧**:
//
//   · `num_fbs = 2`,`flags.fb_in_psram = 1` ⇒ 驱动在 PSRAM 里分配**两块**
//     480×480×2B = 450KB 的整屏 fb(`esp_lcd_rgb_panel_get_frame_buffer()` 取地址);
//   · 驱动只把 **cur_fb_index 那一块**交给 LCD_CAM 的 DMA 连续扫描;
//   · 我们的 flush 永远往**不在扫的那一块**(back)里画;
//   · 一次 LVGL 刷新画完之后,用 `esp_lcd_panel_draw_bitmap(panel, 0,0,W,1, back)`
//     把驱动的 cur_fb_index 指到 back —— 传的指针落在 fb 范围内时,驱动走的是
//     "draw buffer 就是帧缓冲"那一支(`esp_lcd_panel_rgb.c`:`draw_buf_copy_to_fb
//     = false`):**它不拷贝**,只改 cur_fb_index,并在 stream_mode 下把 DMA 的
//     帧缓冲链表重新串到新 fb 上 ⇒ **在下一个帧边界(消隐期)整块换过去**,
//     换帧那一刻屏幕上只有"上一幅"或"下一幅",不存在半新半旧 ⇒ **无撕裂**。
//
// ★ 为什么坐标给 (0, 0, W, 1) 而不是整屏:那一支里驱动还会对"这次窗口"做一次
//   cache 回写(`esp_cache_msync`),给整屏就是每次换帧都回写 450KB;我们自己
//   已经对**真正写过的区域**做过回写(见 blit_area),所以这里只要一行,
//   把驱动那次回写压到最小 —— 换帧因此是**纯指针操作**,几十微秒。
//
// ★★ "写 back 之前"的那道门(wait_swap_settled,看 `swap_wait`/`timeout` 两个计数):
//   换帧请求是**立刻**改 cur_fb_index 的,但 DMA 要到**下一个帧边界**才真的换过去
//   —— 也就是说,换帧后的一小段(≤1 帧 = 18MHz 下 15.5ms)里,旧的那块**还在被扫**。
//   所以下一次 flush 动手之前必须确认那一个边界已经过去,否则那一笔就会落在
//   正在扫描的块上(又是撕裂)。做法是等 on_vsync 计数越过换帧时的计数:
//   稳态下 UI 每 200ms 才画一次,这个门**从来不阻塞**(一次比较就过);
//   开机动画 50Hz 档最坏等一帧。等不到(60ms)就放行并 ++timeout —— 绝不死等。
//
// ★ 两块 fb 的"内容一致"是怎么保证的(否则换过去会看到上一轮的残影/跳回旧内容):
//   **把上一次刷新的脏区补拷进另一块 fb**(`pend_step()`/`pend_finish()`,
//   只补真正改过的那些矩形)。⇒ 两块 fb 逐帧收敛到同一幅画面。
//   ★ 最初的写法是"开机后整块补拷贝一次(450KB,一次性)",那是**不够**的:一次改动只喂了
//     当次的后台那块,两块 fb 的内容从此就不一样 —— 实机症状正是"表情切到下一段了,
//     可每秒刷新时又回到上一段的那个表情;进度条也有残留"(见第三轮那段)。
//   ⇒ 稳态下每次补拷的量 = 上一次改动的面积(几百 µs);只有整屏档才补 450KB。
//
// 代价与边界(写清楚,别指望它包打天下):
//   · 换帧延迟 = 最坏 1 帧(18MHz 15.5ms、30MHz 9.3ms),肉眼不可见;
//   · 整屏刷新 = 450KB 的 CPU→PSRAM 拷贝(实测几十毫秒量级,见 ACCEPTANCE),
//     而且**不再需要**"按 8KB 分块 + 每块等消隐期"那套节流;
//   · 双 fb 各 450KB ⇒ 900KB PSRAM 常驻(板上有 8189KB,见自检那行)。
// ============================================================
//   ★★ 两类失效模式的区分（写在代码里，下次别再混）：
//     ① **只撕不残** ⇒ 换帧没落在帧边界（或写在了正在扫描的 fb 上）。
//        治法：帧边界换帧（见下一段的 bounce + bb_fb_index 锁存）+ 别写正在显示的 fb。
//     ② **撕 + 残 / 或者只有残** ⇒ 两块 fb **内容**不一致（局部刷新只喂了其中一块）。
//        治法：让两块逐帧收敛 —— 要么每帧全量重绘，要么把脏区补到另一块（**本实现**，
//        见下面 pend_step()）。
//
// ------------------------------------------------------------
// ★★ 下面这一段：真根因之一 —— 上面这套"双 fb + 换帧"**在实机上根本没生效**
//    残留与撕裂的真根因是**少了 bounce buffer**。证据全部来自 IDF 5.5.4 源码
//    （`components/esp_lcd/rgb/esp_lcd_panel_rgb.c`；版本对应关系见
//      framework-arduinoespressif32-libs/esp32s3/versions.txt 的 `esp-idf: v5.5.4`）：
//
//  ① ESP32-S3 上 `RGB_LCD_NEEDS_SEPARATE_RESTART_LINK = 1`（硬件规避），驱动因此
//     建了一条**专用 restart link**，它的第 0 个节点**固定挂在 `fbs[0]`** 上：
//         .buffer = rgb_panel->fbs[0] + restart_skip_bytes           // init 里挂一次
//         gdma_link_concat(rgb_panel->dma_restart_link, 0, rgb_panel->dma_fb_links[0], 1);
//     而 `cur_fb_index` 变化之后，**没有任何代码去重挂它**。
//  ② 这版 arduino-esp32 3.3.9(pioarduino) 给 esp32s3 的 sdkconfig 里
//     `CONFIG_LCD_RGB_RESTART_IN_VSYNC=1`（直接读
//      framework-arduinoespressif32-libs/esp32s3/qio_opi/include/sdkconfig.h 即可确认），
//     于是**每个 VSYNC 中断**都走这一支：
//         do_restart = true;  lcd_ll_fifo_reset();  gdma_reset();
//         gdma_start(..., gdma_link_get_head_addr(panel->dma_restart_link));  // ← 又回 fbs[0]
//  ③ ⇒ **没有 bounce buffer 时，屏上永远是 `fbs[0]`**：那次
//     `esp_lcd_panel_draw_bitmap(..., g_fb[g_back])`（= 换帧）**对显示内容毫无影响**。
//     后果正是实机看到的两个症状：
//       · "往 back 画"的那些刷新**根本没上屏**，而 LVGL 也不会再重画那一块
//         ⇒ **图像残留**；
//       · back 恰好等于 `fbs[0]` 的那些轮，**写在了正在被扫描的显存上** ⇒ **撕裂**。
//     两者同一个根因（不是"后台缓冲半新半旧"）。
//
// ★ 治法就是**照微雪官方 2.8C 例程抄一行**：开 **bounce buffer**
//   （出处：ESP32-S3-LCD-2.8C-Demo.zip → ESP-IDF/ESP32-S3-LCD-2.8C-Test/
//     main/LCD_Driver/ST7701S.c，其 `panel_config` 里
//       `.bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES,` 且 Kconfig 默认 y）。
//   为什么这一行治本（同一个 .c）：
//     // bounce 模式下 DMA 读的是**内部 SRAM 的 bounce buffer**，不是 PSRAM 里的 fb
//     memcpy(buffer, &panel->fbs[panel->bb_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);
//     ...
//     if (panel->bounce_pos_px >= panel->fb_size / bytes_per_pixel) {
//         panel->bounce_pos_px = 0;
//         panel->bb_fb_index = panel->cur_fb_index;   // ← 换帧在这一刻生效（= 帧边界）
//     }
//   ⇒ ① restart link 挂的是 `bounce_buffer[0]`，而它**装什么**才是我们选的那块 fb
//        ⇒ "永远 fbs[0]"这条死路没了；
//      ② 换帧被驱动**锁在帧边界**（`bb_fb_index` 只在整帧走完那一刻更新）⇒ 不半新半旧；
//      ③ LCD 的 DMA 从此**完全不碰 PSRAM** ⇒ 不再与 CPU 抢 PSRAM 带宽（IDF 文档把
//        bounce 模式正是写成"对抗带宽尖峰"的手段）。
//   代价（写清楚）：CPU 每帧要把整幅 450KB 从 PSRAM 拷进内部 SRAM（18MHz/64.7Hz 下
//   约 30MB/s；在 DMA EOF 中断里分成 48 次、每次 9600B）—— 这就是官方例程默认档的
//   代价，实测数字见 docs/RGB-PANEL-2.8C.md 第 10 节。
//   `-DRGB_BOUNCE_LINES=0` 可退回旧行为（屏上只有 fbs[0]）用于复现/对照。
//
// ★ 本轮另加两个**可测开关**（都明确打在日志里，见 dash_display_poll）：
//   · `模式=全屏重绘|局部刷新`：可编译期定档，也可运行期自动交替（默认每 10s）；
//     "全屏重绘"档 = 每 30ms 让当前屏整体失效一次 ⇒ LVGL 每轮重画整屏。
//   · `wrap=`：驱动"走完一整帧"的次数（= 换帧真正生效的次数），与 `vsync` 对照着看。
// ------------------------------------------------------------

#include "dash_display.h"
#include "ui_theme.h"
#include "dash_log.h"     // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

#if defined(DASH_DISPLAY_RGB)

#include <Arduino.h>
#include <string.h>              // memcpy(往 back fb 里搬像素)
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_rgb.h>   // ★ IDF 5.5 的版本:num_fbs / register_event_callbacks
#include <esp_lcd_panel_ops.h>
#include <esp_cache.h>           // esp_cache_msync():CPU 写过的 PSRAM 要回写给 DMA 看
#include <driver/spi_common.h>   // SPI2_HOST(初始化命令那条 3 线 SPI 用)
#include <driver/spi_master.h>   // 裸 spi_device_transmit(见下面第 4 块的说明)
#include <esp_heap_caps.h>

// ------------------------------------------------------------
// ★ 屏到手后**只改这个文件顶部的数字**,别处的代码不用动。
//   下面每一项都标了"从哪来",因为 ST7701 的初始化时序/上电顺序各家不同,
//   抄错一项就是黑屏或者花屏(而且不报错)。
//
//   ★★ 2026-09-24 这块板已经点起来了:**微雪 ESP32-S3-LCD-2.8C(非触控,最终板)**。
//   实际生效的引脚/时序/41 步初始化、出处、以及两个"静默失败"的坑,
//   都记在 **docs/RGB-PANEL-2.8C.md** 里 —— 换板/换屏之前先看那一页。
// ------------------------------------------------------------

// ---- 1) 引脚:**微雪 ESP32-S3-LCD-2.8C(非触控,最终板)** ----
//   出处(可复核):官方例程包 ESP32-S3-LCD-2.8C-Demo.zip 里
//     Arduino/examples/LVGL_Arduino/Display_ST7701.h —— 与 wiki 的
//     2.8C 引脚表逐脚一致(LCD_BL=GPIO6 / PCLK=41 / DE=40 / VSYNC=39 / HSYNC=38)。
//   ★ 这块板的**屏接口引脚表与 2.1" 那族逐脚相同**(3 份官方例程逐行比过:
//     ESP32-S3-LCD-2.8C / ESP32-S3-Touch-LCD-2.8C / ESP32-S3-Touch-LCD-2.1)
//     ⇒ 引脚这块**不需要按板子分支**;按板子分的只有初始化和时序(第 2/3 块)。
//   ★ 数据线 bit0 接的是面板 **B1**(B0=NC)、bit5=G0、bit11=**R1**(R0=NC):
//     即面板 B0/R0 两根最低位不接,其余 16 根按 RGB565 顺序连号。**照抄,别重排** ——
//     换序的症状是"颜色整体偏色/红蓝互换",而屏是亮的,很容易误判成时序问题。
#define RGB_PIN_PCLK   41
#define RGB_PIN_DE     40
#define RGB_PIN_VSYNC  39
#define RGB_PIN_HSYNC  38
#define RGB_DATA_GPIOS { 5, 45, 48, 47, 21, 14, 13, 12, \
                         11, 10, 9, 46, 3, 8, 18, 17 }

// ---- 2) 初始化命令:**照抄 2.8C 官方例程的 ST7701 上电序列**(41 步) ----
//   来源:ESP32-S3-LCD-2.8C-Demo.zip → Display_ST7701.cpp 的 ST7701_Init()。
//   ★ 触控版(Touch-2.8C)与非触控版这一段的**逐条相同**(两份例程 diff 过:
//     IDENTICAL)⇒ 这一段按**面板**走、不按板子走。
//   ★ 三类细节别自作聪明改:
//     · 开头那三行 `0xFF 77 01 00 00 13` → `0xEF 08` → `0xFF …10` 是 2.8C 的
//       **入口页顺序**:先页 0x13 把 0xEF 置 8,再回页 0x10 写电源/伽马。顺序错了
//       后面整段都落错页(屏黑,而串口一切正常)。
//     · `0xC1=0x10 0x0C` / `0xC2=0x07 0x0A` 是 VBP/VFP 那一组,**别与 2.1" 的
//       `0x0B 0x02`/`0x07 0x02` 混用** —— 那是另一块面板的数。
//     · 0x11(SLPOUT)后 **120ms**,然后 0x3A=0x66 → 0x36=0x00 → 0x35=0x00(TEON)
//       → 0x29(DISPON)。★ 2.8C **没有** 0x20(INVOFF)那一步(2.1" 才有),
//       而多一步 0x35;这两条是两块面板最容易抄串的地方。
//   走 3 线 SPI(SCLK/SDA)写命令;**CS 不在 GPIO 上**,在 TCA9554 的 EXIO3 上,
//   所以下面 io_cfg.cs_gpio_num = -1,由 tca9554_* 手动拉(见第 4 块)。
#define RGB_PIN_INIT_SDA   1
#define RGB_PIN_INIT_SCLK  2
struct RgbInitCmd { uint8_t cmd; uint8_t data[16]; uint8_t len; uint16_t delay_ms; };
static const RgbInitCmd kPanelInit[] = {
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0 },   // 入口页 0x13
  { 0xEF, {0x08}, 1, 0 },
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0 },   // 回页 0x10
  { 0xC0, {0x3B, 0x00}, 2, 0 },                     // Scan line
  { 0xC1, {0x10, 0x0C}, 2, 0 },                     // VBP(2.8C)
  { 0xC2, {0x07, 0x0A}, 2, 0 },                     // VFP(2.8C)
  { 0xC7, {0x00}, 1, 0 },
  { 0xCC, {0x10}, 1, 0 },
  { 0xCD, {0x08}, 1, 0 },                           // RGB format
  { 0xB0, {0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09,
           0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11}, 16, 0 },   // IPS
  { 0xB1, {0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08,
           0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F}, 16, 0 },   // IPS
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0 },   // 页 0x11
  { 0xB0, {0x5D}, 1, 0 },                           // VOP
  { 0xB1, {0x3E}, 1, 0 },                           // VCOM amplitude
  { 0xB2, {0x81}, 1, 0 },                           // VGH 12V
  { 0xB3, {0x80}, 1, 0 },
  { 0xB5, {0x4E}, 1, 0 },                           // VGL
  { 0xB7, {0x85}, 1, 0 },
  { 0xB8, {0x20}, 1, 0 },
  { 0xC1, {0x78}, 1, 0 },
  { 0xC2, {0x78}, 1, 0 },
  { 0xD0, {0x88}, 1, 0 },
  { 0xE0, {0x00, 0x00, 0x02}, 3, 0 },
  { 0xE1, {0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07,
           0x30, 0x00, 0x33, 0x33}, 11, 0 },
  { 0xE2, {0x11, 0x11, 0x33, 0x33, 0xF4, 0x00,
           0x00, 0x00, 0xF4, 0x00, 0x00, 0x00}, 12, 0 },
  { 0xE3, {0x00, 0x00, 0x11, 0x11}, 4, 0 },
  { 0xE4, {0x44, 0x44}, 2, 0 },
  { 0xE5, {0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0,
           0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0}, 16, 0 },
  { 0xE6, {0x00, 0x00, 0x11, 0x11}, 4, 0 },
  { 0xE7, {0x44, 0x44}, 2, 0 },
  { 0xE8, {0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0,
           0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0}, 16, 0 },
  { 0xE9, {0x36, 0x01}, 2, 0 },
  { 0xEB, {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0 },
  { 0xED, {0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF,
           0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF}, 16, 0 },
  { 0xEF, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0 },
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0 },  // 回页 0
  { 0x11, {}, 0, 120 },                            // SLPOUT(2.8C 是 120ms)
  { 0x3A, {0x66}, 1, 0 },                          // COLMOD(见上面 ★)
  { 0x36, {0x00}, 1, 0 },                          // MADCTL:扫描方向
  { 0x35, {0x00}, 1, 0 },                          // TEON(2.8C 有,2.1" 没有)
  { 0x29, {}, 0, 0 },                              // DISPON(2.8C 无 0x20 那步)
};
static const size_t kPanelInitCount = sizeof(kPanelInit) / sizeof(kPanelInit[0]);

// ---- 3) 时序:porch **照抄 2.8C 官方例程**;PCLK 见下 ----
//   像素时钟 = (h_res + 前后沿/脉宽) × (v_res + 前后沿/脉宽) × 刷新率。
//     · 30MHz:官方 **Arduino** 例程的值(porch 与这里逐项相同);
//     · 18MHz:官方 **ESP-IDF** 例程的值(`EXAMPLE_LCD_PIXEL_CLOCK_HZ`)。
//   ★ 2026-09-24(旧栈、单 fb):30MHz 下**每次画面更新整屏花** —— 那时候没有
//     bounce buffer、CPU 和 DMA 抢同一块 PSRAM,写一下就 FIFO 欠载;降到 18MHz
//     并把写 fb 挪到消隐期之后才干净。
//   ★ 2026-09-24(本栈、双 fb):**先按 18MHz 实测,再单独试 30MHz**,以实测为准,
//     不稳就退回 18MHz —— 见 docs/RGB-PANEL-2.8C.md 第 9 节的实测表。
//   ★ 2026-09-24（第四轮）:**降到 15MHz** —— 车主看到"横纹**随刷新移动**"（⇒ 欠载/带宽类，
//     不是固定干扰、也不是背光 PWM 那条）。账是这么算的（为什么 bounce 档怕"额外流量"）:
//       · 面板消耗速率 = pclk × 2B:18MHz ⇒ **36MB/s**、15MHz ⇒ 30MB/s、12MHz ⇒ 24MB/s;
//       · bounce 档的搬运全在 CPU 手上（DMA 只读内部 SRAM），而 CPU 从 PSRAM 搬的
//         实测速率只有 ~30MB/s 量级，还要和"往 fb 里写像素 / LVGL 重绘"抢带宽
//         （实测一次整屏补拷能慢到 **37ms** ⇒ 12MB/s）。
//       ⇒ 18MHz 时**填充速率 ≈ 消耗速率**，只要有一笔突发（整屏 blit、补拷）就会
//         让某一块 bounce buffer 来不及填 ⇒ 那一帧的某几行吐旧数据 ⇒ **会移动的横纹**。
//       ⇒ 降到 15MHz 就是把这个比值拉开 17%（53.9Hz 面板刷新率，肉眼无感）;
//         若还不够，下一步只改这一个数到 12MHz（24MB/s，43.2Hz）。
//   ⇒ 本机当前:**15MHz** / ((480+8+10+50) × (480+2+18+8)) = 15e6/548/508 ≈ **53.9 Hz**。
//     (串口上 `rgb: vsync=…(+N/s)` 的 N 就是这个量级 —— 它同时是"PCLK 到底跑成
//      多少"的**第一手判据**:15MHz→约 54、18MHz→约 65、30MHz→约 108。)
#define RGB_PIXEL_CLOCK_HZ  (15 * 1000 * 1000)   // ← 定案历史:30MHz 也实测过(帧率 107.8Hz 对得上),
                                                   //   但同一帧里往 fb 里搬像素的耗时从 19.9ms 涨到 27.2ms
                                                   //   —— 那正是 PSRAM 争用的信号。18MHz 是微雪官方例程值,
                                                   //   本轮因为"会移动的横纹"再降到 15MHz(见 docs 第 11 节)。
#define RGB_HSYNC_PULSE     8                    // HPW
#define RGB_HSYNC_BACK      10                   // HBP
#define RGB_HSYNC_FRONT     50                   // HFP
#define RGB_VSYNC_PULSE     2                    // VPW(2.8C;2.1" 是 3)
#define RGB_VSYNC_BACK      18                   // VBP(2.8C;2.1" 是 8)
#define RGB_VSYNC_FRONT     8                    // VFP
// 例程里这三个都是 0:hsync_idle_low=0 / vsync_idle_low=0 / pclk_active_neg=false
// (占位版写的是 idle_low=1 —— 那是另一族的常见值,在这块屏上会让画面整行错位)
#define RGB_PCLK_ACTIVE_NEG 0
#define RGB_HSYNC_IDLE_LOW  0
#define RGB_VSYNC_IDLE_LOW  0

// 一屏的字节数(480×480×RGB565)。两块 fb 各这么大,都在 PSRAM。
#define RGB_FB_BYTES  ((uint32_t)THEME_DISPLAY_RES * (uint32_t)THEME_DISPLAY_RES * 2u)

// ------------------------------------------------------------
// ★★ 本轮的两个开关(默认值就是"要烧上板的那一档")
// ------------------------------------------------------------
// bounce buffer 的行数:**10 行 = 微雪官方 2.8C 例程的取值**(10 * 480 px = 9600B/块,
// 两块共 19.2KB 内部 SRAM)。`0` = 关掉(退回旧行为:屏上只有 fbs[0])。
//   ★ 为什么必须能整除:`esp_lcd_new_rgb_panel` 会校验 `fb_size % bb_size == 0`
//     (480 行 / 10 行 = 48 次/帧,正好整除)。填不能整除的值会**创建失败**并打日志。
#ifndef RGB_BOUNCE_LINES
#define RGB_BOUNCE_LINES 10
#endif
// "全屏重绘 / 局部刷新"对比档:编译期默认 + 运行期自动交替周期(0 = 不自动交替)。
//   ★ 局部刷新(0)是**正路**;全屏重绘(1)只用来做"残留/撕裂"的对照实验。
#ifndef RGB_FULL_REFRESH_DEFAULT
#define RGB_FULL_REFRESH_DEFAULT 0
#endif
#ifndef RGB_FULL_REFRESH_ALTERNATE_MS
#define RGB_FULL_REFRESH_ALTERNATE_MS 10000
#endif
// 全屏重绘档:每隔这么久让"当前屏"整体失效一次(30ms ≈ 33 次/秒的整屏重画请求;
// 实际能画多少取决于 CPU/PSRAM,串口上 `swap=+N/s` 就是那个实测值)。
#ifndef RGB_FULL_REFRESH_PERIOD_MS
#define RGB_FULL_REFRESH_PERIOD_MS 30
#endif

// ------------------------------------------------------------
// 双缓冲状态
// ------------------------------------------------------------
static esp_lcd_panel_handle_t g_panel = nullptr;
static lv_display_t* g_left = nullptr;
static lv_display_t* g_right = nullptr;
static uint8_t* g_fb[2] = {nullptr, nullptr};   // 驱动分配的**两块**整屏 fb(PSRAM)

// 哪一块正在被 DMA 扫描(g_front)、我们往哪一块画(g_back)。
//   ★ 只在 flush 里改(LVGL 的刷新跑在主循环),ISR 只读不写。
static uint8_t g_front = 0;
static uint8_t g_back = 1;
// ★★ 第三轮之二:**两块 fb 的"收敛"靠"补拷上一次的脏区"**(见 pend_step/pend_finish)。
//   ★ 第五轮(车主"扫表时正常、稳态才出现横纹"那条观察)把**补拷的粒度**改了:
//     原来是在刷新开头**一笔**拷完(稳态实测一笔 ~90KB、要 7ms 量级)⇒ 那一笔会把
//     PSRAM 带宽全占住 ⇒ 同一帧里 bounce 的填充来不及 ⇒ **一条随刷新移动的横纹** ✗。
//     现在改成**在两次刷新之间的空闲时间里一小段一小段地拷**(每次 8 行 ≈ 7.7KB),
//     总字节数一模一样(稳态 ~450KB/s ✓),但**没有大突发** ⇒ 填充不再被挤掉 ✓。
//     刷新真要开始了还没拷完 ⇒ 在刷新开头把剩余的一次性补完(正确性兜底)。
#define RGB_DMG_MAX 12
#define RGB_CATCHUP_STEP_ROWS 8      // 每次"小碎步"最多拷几行(8 行 = 7.68KB)
struct RgbRect { int16_t x1, y1, x2, y2; };
static RgbRect  g_dmg[RGB_DMG_MAX];        // 本次刷新的脏区
static uint8_t  g_dmg_n = 0;
static bool     g_dmg_overflow = false;    // 超过上限 ⇒ 本次按"整屏脏"处理(保守但正确)
// 待补拷清单(上一次刷新改过的区域)+ 进度游标
static RgbRect  g_pend[RGB_DMG_MAX];
static uint8_t  g_pend_n = 0;
static bool     g_pend_full = false;       // 待补的是整屏(上一次刷新就是整屏)
static bool     g_pend_active = false;     // 还有没补完的
static uint8_t  g_pend_rect = 0;           // 补到第几个矩形
static int16_t  g_pend_y = 0;              // 当前矩形补到哪一行
static uint32_t g_catchup_n = 0;           // 补拷完成次数
static uint32_t g_catchup_kb = 0;          // 补拷总量(KB)

static volatile uint32_t g_vsync = 0;        // on_vsync 回调计数(= 面板扫描帧数)
static uint32_t g_swap = 0;                  // 换帧次数(一次 LVGL 刷新 = 一次)
static uint32_t g_flush_count = 0;           // flush 回调次数

// ★ 本轮新增:驱动"走完一整帧"的次数(on_frame_buf_complete 回调)。
//   bounce 模式下,**换帧真正生效**就发生在这一刻(同一个函数里 `bb_fb_index = cur_fb_index`)
//   ⇒ 它既是"换帧落在帧边界"的证据,也是"官方那套 bounce 有没有真的跑起来"的判据
//   (`vsync` 在涨而 `wrap` 不涨 = bounce 没生效,屏上就还是老样子)。
static volatile uint32_t g_frame_wrap = 0;
static uint32_t g_swap_barrier = 0;          // 最近一次换帧请求时的屏障计数(等它变 = 生效了)

// ★ 第四轮诊断:LVGL 刷新节奏(两次换帧的间隔)与"整屏刷新"的频次。
//   用途:把"抖动/横纹"归因到带宽上 ——
//     · 间隔 jitter 大 ⇒ 主循环被 PSRAM 搬运(LVGL 重绘/blit/补拷)拖住了;
//     · `fullrb/s` 大 ⇒ 屏上正在反复整屏重画(整屏 blit 是最大的一笔突发)。
static uint32_t g_swap_last_us = 0;
static uint32_t g_swap_int_min_us = 0xFFFFFFFFu;
static uint32_t g_swap_int_max_us = 0;
static uint64_t g_swap_int_sum_us = 0;
static uint32_t g_swap_int_n = 0;
static uint32_t g_fullrb_n = 0;              // 本秒内"整屏刷新"的次数

// 换帧请求落在"帧内哪个相位"(相对上一个 VSYNC 过去了多少 µs)。稳态下换帧请求是
// 异步来的(200ms 一次),所以这个数应当**散布在 0..一帧**之间;它本身不是判据,
// 判据是上面那道门 + 驱动那边的帧边界锁存(见文件头)。这里只把它记下来当证据。
static volatile uint32_t g_vsync_us = 0;     // 最近一次 VSYNC 中断的时刻(ISR 里写)
static uint32_t g_swap_phase_max_us = 0;

// ★ 本轮新增:"全屏重绘 / 局部刷新"对比档的运行期状态(默认值来自上面那两个宏)。
//   g_full_refresh=true 时,每隔 RGB_FULL_REFRESH_PERIOD_MS 让当前屏整体失效一次。
static bool g_full_refresh = (RGB_FULL_REFRESH_DEFAULT != 0);
static uint32_t g_full_refr_last_ms = 0;     // 上一次"让整屏失效"的时刻
static uint32_t g_mode_switch_ms = 0;        // 上一次自动交替的时刻

// 诊断:等"换帧那个边界过去"的统计(判据是 timeout 恒为 0)
static uint32_t g_swap_wait_max_us = 0;
static uint32_t g_swap_timeout = 0;
static uint32_t g_skip_count = 0;            // 面板没建起来时直接放行的次数

// 诊断:往 fb 里搬像素(含 cache 回写)的耗时
static uint32_t g_copy_us_max = 0;
static uint32_t g_copy_us_sum = 0;
static uint32_t g_copy_n = 0;
// ★ 第五轮:把"单笔 PSRAM 搬运"按**来源**分开计 —— 车主要的诊断是
//   "**每一笔**搬运的最大耗时"(要能看出"当次脏区"那一笔有多大):
//     · `blit_max` = 当次脏区的 memcpy(受 LVGL 绘制缓冲大小限制,见 RGB_DRAW_BUF_LINES)
//     · `step_max` = 小碎步补拷**一步**的耗时(≤ RGB_CATCHUP_STEP_ROWS 行)
//     · `forced_kb` = 被"刷新提前开始"逼出来的一次性补完量(pend_finish 走的那条兜底路)
static uint32_t g_blit_max_us = 0;
static uint32_t g_step_max_us = 0;
static uint32_t g_forced_kb = 0;
static bool     g_in_forced = false;

// 本次刷新覆盖了哪些**整行**(只统计"整行都写了"的,用位图记 —— 480 行 = 15 个 u32)。
//   它的唯一用途:判断这一次刷新是不是"整屏"(是的话,目标 fb 从此算完整)。
//   ★ THEME_DISPLAY_RES=480 是 32 的整数倍,所以"全 1"就是"所有行都覆盖"。
#define RGB_ROW_WORDS  (((int)THEME_DISPLAY_RES + 31) / 32)
static uint32_t g_rows_full[RGB_ROW_WORDS];

static void rows_reset() {
  for (int i = 0; i < RGB_ROW_WORDS; ++i) g_rows_full[i] = 0u;
}
static void rows_mark(int32_t y1, int32_t y2) {
  for (int32_t y = y1; y <= y2; ++y) {
    const int w = (int)(y >> 5);
    if (w >= 0 && w < RGB_ROW_WORDS) g_rows_full[w] |= (1u << (uint32_t)(y & 31));
  }
}
static bool rows_all() {
  const uint32_t last_mask = (((uint32_t)THEME_DISPLAY_RES % 32u) != 0u)
      ? ((1u << ((uint32_t)THEME_DISPLAY_RES % 32u)) - 1u) : 0xFFFFFFFFu;
  for (int i = 0; i < RGB_ROW_WORDS; ++i) {
    const uint32_t want = (i == RGB_ROW_WORDS - 1) ? last_mask : 0xFFFFFFFFu;
    if (g_rows_full[i] != want) return false;
  }
  return true;
}

// 一次刷新的计时(给"开机整屏刷新耗时"那条日志用)
static uint32_t g_refr_t0_us = 0;
static uint32_t g_refr_copy_us = 0;
static uint32_t g_refr_flush_n = 0;

// ------------------------------------------------------------
// on_vsync:**每帧一次的中断**(IDF 5.5 的 RGB 面板回调表里的一项)
//
//   ★ 它是这一版**唯一的**扫描同步信号,只做一件事:计数。
//   · 计数 g_vsync 就是"面板确实在收帧"的判据(和旧栈的 `frames=` 同一个用途),
//     同时也是"上一次换帧那个边界过去了没有"的判据(见 wait_swap_settled);
//   · **绝不在中断里碰显存**(那是 flush 的事),也不能在这里调任何阻塞函数。
//   ★ 这个回调在**中断上下文**里跑,所以标 IRAM_ATTR(IDF 在
//     CONFIG_LCD_RGB_ISR_IRAM_SAFE 下会直接拒收不在 IRAM 里的回调)。
// ------------------------------------------------------------
static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t* edata,
                               void* user_ctx) {
  (void)panel; (void)edata; (void)user_ctx;
  // 不用 `++g_vsync`：C++20 起对 volatile 的自增/复合赋值已弃用（GCC 报 -Wvolatile），
  // 写开是同一件事，而且只有一个写者（这个 ISR）。
  g_vsync = g_vsync + 1u;
  g_vsync_us = micros();   // 记时刻:给"换帧请求落在帧内什么相位"当分母(只写一个 u32,ISR 里安全)
  return false;   // 没唤醒高优先级任务
}

// ------------------------------------------------------------
// on_frame_buf_complete:**一整帧的 bounce 数据都送出去了**(仍然在中断上下文)
//
//   ★ 这个回调只在 bounce 模式下有意义,而且它是本轮"换帧到底有没有生效"的**唯一判据**:
//     同一个函数(`lcd_rgb_panel_fill_bounce_buffer`)里,位置绕回 0 的那一刻会做
//         panel->bb_fb_index = panel->cur_fb_index;   // ← 换帧在帧边界生效
//     然后回调我们。所以 `wrap` 每 +1,就代表**驱动刚刚把一整帧走完并锁存了新 fb**。
//   · 与 `vsync` 对照:`vsync` 是面板扫描帧数(只要在扫就涨),`wrap` 是**驱动换帧**次数;
//     两者应当基本同步(+64/s 量级)。若 `vsync` 涨而 `wrap` 不涨 ⇒ bounce 没成立。
//   · 绝不在中断里碰显存/调阻塞函数,只加计数。
// ------------------------------------------------------------
static bool IRAM_ATTR on_frame_complete(esp_lcd_panel_handle_t panel,
                                        const esp_lcd_rgb_panel_event_data_t* edata,
                                        void* user_ctx) {
  (void)panel; (void)edata; (void)user_ctx;
  g_frame_wrap = g_frame_wrap + 1u;
  return false;
}

// ------------------------------------------------------------
// 换帧的那道门:等"上一次换帧真的生效"之后,再动那块刚被换下去的 fb
//
//   为什么要等:bounce 模式下,换帧请求(`draw_bitmap` 指到 back)只是**登记**了
//   `cur_fb_index`;驱动要到**整帧走完那一刻**才把它锁进 `bb_fb_index`。
//   在那之前,屏上还在按旧 fb 出数据 —— 这时候往"新 back(= 刚被换下去的那块)"里写,
//   就写在了还在被当数据源读的显存上 ⇒ 又是一条缝。
//
//   ★ 判据用哪个计数器(本轮改过,这里是关键):
//     · bounce 档(默认)用 `g_frame_wrap` —— 它**就是**驱动锁存新 fb 的那一刻,
//       所以这道门等的是"换帧已生效",不是"大概过去了"。
//     · 退回档(无 bounce)只能用 `g_vsync` 近似(那时换帧其实不生效,见文件头 ③)。
//   ★ 稳态(UI 每 200ms 画一次)下这个门**一次都不阻塞**:一次比较就过。
//   ★ 兜底:60ms 还没等到(面板被停/中断没来)就放行并 ++timeout,**绝不死等**
//     —— 死在这里的后果是整屏再也不更新(上一轮踩过)。
// ------------------------------------------------------------
static inline uint32_t swap_barrier() {
#if RGB_BOUNCE_LINES > 0
  return g_frame_wrap;    // 帧边界上的"换帧已锁存"计数
#else
  return g_vsync;         // 无 bounce:只能拿扫描帧数近似
#endif
}

static void wait_swap_settled() {
  if (swap_barrier() != g_swap_barrier) return;   // 绝大多数情况走这一行
  const uint32_t t0 = micros();
  while (swap_barrier() == g_swap_barrier) {
    if ((uint32_t)(micros() - t0) >= 60000u) { ++g_swap_timeout; return; }
  }
  const uint32_t waited = (uint32_t)(micros() - t0);
  if (waited > g_swap_wait_max_us) g_swap_wait_max_us = waited;
}

// 把一块区域从 LVGL 的绘制缓冲搬进某块 fb,并把这一段回写进 PSRAM。
//   · LVGL v9 的 px 指向绘制缓冲,区域内容按**区域自己的行距**紧排,而 fb 的行距
//     是整屏宽(960B)⇒ 区域不是整宽时按行拷;
//   · ★ cache 回写**不能省**:fb 在 PSRAM、在 cache 后面(驱动自己也这么干:
//     `esp_lcd_panel_rgb.c` 里拷完就 `esp_cache_msync`),不回写的话 DMA
//     读到的还是旧内容(症状是"画面里混着上一帧的碎片")。
static void blit_area(uint8_t* dst_fb, const uint8_t* src,
                      int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
  const uint32_t row_bytes = (uint32_t)(x2 - x1 + 1) * 2u;      // 区域一行多少字节
  const uint32_t fb_stride = (uint32_t)THEME_DISPLAY_RES * 2u;  // fb 一行多少字节
  const uint32_t off = (uint32_t)y1 * fb_stride + (uint32_t)x1 * 2u;
  const uint32_t nbytes = (uint32_t)(y2 - y1 + 1) * row_bytes;
  if (nbytes == 0u) return;
  uint8_t* dst = dst_fb + off;
  if (row_bytes == fb_stride) {
    memcpy(dst, src, nbytes);                 // 整宽区域:一次拷完
  } else {
    for (int32_t y = y1; y <= y2; ++y) {      // 窄区域:按行拷
      memcpy(dst, src, row_bytes);
      dst += fb_stride;
      src += row_bytes;
    }
  }
  esp_cache_msync((void*)(dst_fb + off), nbytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// 把"这一笔改动"记进本次刷新的脏区清单(下一次刷新开头会拿它去补另一块 fb)。
//   · 上限 RGB_DMG_MAX 条;超了就置 overflow ⇒ 下一次按"整屏"补(慢一点但一定对);
//   · 不相邻的矩形不去合并:补拷时重叠几次只是多几行 memcpy,不影响正确性。
static void dmg_add(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
  if (g_dmg_overflow) return;
  if (g_dmg_n >= RGB_DMG_MAX) { g_dmg_overflow = true; return; }
  RgbRect& r = g_dmg[g_dmg_n++];
  r.x1 = (int16_t)x1; r.y1 = (int16_t)y1;
  r.x2 = (int16_t)x2; r.y2 = (int16_t)y2;
}

// fb → fb 的区域拷贝(+cache 回写)。**只用来把脏区从 front 补进 back**:
//   ★ 读的那块(front)此刻正被驱动当"这一帧的数据源"读 —— 只读,不冲突;
//   ★ 写的那块(back)既不在显示、也不在被读(换帧已在帧边界锁存)⇒ 不会撕。
//   ★ 行数可限:小碎步补拷就靠这个参数(见 pend_step)。
static void copy_rect_rows(uint8_t dst_idx, uint8_t src_idx,
                           int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
  if (x2 < x1 || y2 < y1) return;
  const uint32_t fb_stride = (uint32_t)THEME_DISPLAY_RES * 2u;
  const uint32_t row_bytes = (uint32_t)(x2 - x1 + 1) * 2u;
  const uint32_t off = (uint32_t)y1 * fb_stride + (uint32_t)x1 * 2u;
  const uint32_t nbytes = (uint32_t)(y2 - y1 + 1) * row_bytes;
  if (nbytes == 0u) return;
  uint8_t* dst = g_fb[dst_idx] + off;
  const uint8_t* src = g_fb[src_idx] + off;
  if (row_bytes == fb_stride) {
    memcpy(dst, src, nbytes);
  } else {
    for (int32_t y = y1; y <= y2; ++y) {
      memcpy(dst, src, row_bytes);
      dst += fb_stride; src += fb_stride;
    }
  }
  esp_cache_msync((void*)(g_fb[dst_idx] + off), nbytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  g_catchup_kb += (nbytes + 1023u) / 1024u;
}

// ★★ 两块 fb 的"收敛":把**上一次刷新改过的区域**从 front 补拷进 back。
//
//   不做这一步的后果(实机症状原文):"表情已经切到下一段了,但每秒刷新时又回到上一段的
//   那个表情;进度条也有点残留" —— 因为一次改动只喂了当次的后台那一块,两块 fb 的内容
//   从此不一样,换帧就是在两块之间来回翻。
//
//   ★ 粒度是第五轮改的关键:**一小段一小段地补**(每次 ≤ RGB_CATCHUP_STEP_ROWS 行)。
//     这个函数被 `dash_display_poll()` 在两次刷新之间的空闲时间里反复调用 ——
//     稳态一次刷新要补 ~90KB,分成 ~12 小步(每步 7.7KB)就搬完了,
//     总字节不变、但**没有"一笔 90KB"那种会把 bounce 填充挤掉的大突发** ✓。
//   返回值:还有没有没补完的。
static bool pend_step() {
  if (!g_pend_active) return false;
  if (g_fb[0] == nullptr || g_fb[1] == nullptr) { g_pend_active = false; return false; }
  // ★ 换帧还没生效(屏障没动)时**不能写**:那一刻 back 还是"正在显示/正在被读"的那块
  if (swap_barrier() == g_swap_barrier) return true;   // 留着,等下一圈

  const uint32_t t0 = micros();
  const uint32_t kb0 = g_catchup_kb;
  int budget = RGB_CATCHUP_STEP_ROWS;
  while (budget > 0 && g_pend_active) {
    RgbRect r;
    if (g_pend_full) {
      r.x1 = 0; r.y1 = g_pend_y;
      r.x2 = (int16_t)(THEME_DISPLAY_RES - 1); r.y2 = (int16_t)(THEME_DISPLAY_RES - 1);
      if (g_pend_y > r.y2) { g_pend_active = false; break; }
    } else {
      if (g_pend_rect >= g_pend_n) { g_pend_active = false; break; }
      r = g_pend[g_pend_rect];
      if (g_pend_y < r.y1) g_pend_y = r.y1;
      if (g_pend_y > r.y2) {            // 这个矩形补完了 ⇒ 换下一个
        ++g_pend_rect;
        g_pend_y = (g_pend_rect < g_pend_n) ? g_pend[g_pend_rect].y1 : 0;
        continue;
      }
    }
    const int32_t y2 = (int32_t)r.y2 < (int32_t)g_pend_y + budget - 1
                     ? (int32_t)r.y2 : (int32_t)g_pend_y + budget - 1;
    copy_rect_rows(g_back, g_front, r.x1, g_pend_y, r.x2, y2);
    budget -= (int)(y2 - g_pend_y + 1);
    g_pend_y = (int16_t)(y2 + 1);
    if (g_pend_full) {
      if (g_pend_y > (int16_t)(THEME_DISPLAY_RES - 1)) g_pend_active = false;
    } else if (g_pend_y > r.y2) {
      ++g_pend_rect;
      g_pend_y = (g_pend_rect < g_pend_n) ? g_pend[g_pend_rect].y1 : 0;
      if (g_pend_rect >= g_pend_n) g_pend_active = false;
    }
  }
  if (!g_pend_active) ++g_catchup_n;
  const uint32_t dt = (uint32_t)(micros() - t0);
  if (g_in_forced) g_forced_kb += (g_catchup_kb - kb0);   // 兜底那一次的量单独记
  else if (dt > g_step_max_us) g_step_max_us = dt;        // 正常小碎步:记单步最大耗时
  return g_pend_active;
}

// 刷新真要动手了还没补完 ⇒ 把剩下的**一次补完**(正确性兜底:back 必须完整才能换上去)。
//   调用前必须已经过了 wait_swap_settled()(那道门),否则这里会白等。
//   ★ 这条路上会出现"一大笔"搬运(稳态几乎不会走到:两次刷新之间有 ~200ms 给小碎步),
//     所以它的量记进 `forced_kb`,用来盯"兜底有没有被频繁触发"。
static void pend_finish() {
  uint32_t guard = 0;
  g_in_forced = true;
  while (pend_step()) {
    if (++guard > 4096u) { g_pend_active = false; break; }   // 兜底:绝不死循环
  }
  g_in_forced = false;
}

// 换帧:把驱动的 cur_fb_index 指到 back —— DMA 会在**下一个帧边界**整块换过去。
//
//   ★ 传的指针落在 fb 范围内 ⇒ 驱动走 `draw_buf_copy_to_fb = false` 那一支:
//     **不拷贝**,只改 cur_fb_index + 在 stream_mode 下重串 DMA 的帧缓冲链表。
//   ★ 窗口给 (0,0,W,1):那一支里驱动会对"这次窗口"做一次 cache 回写,给整屏
//     就是每次换帧回写 450KB —— 我们自己已经回写过真正写过的区域了,所以这里
//     只要一行,让驱动那次回写退化成 960B。换帧本身因此是**几十微秒**的事。
static void request_swap() {
  if (g_panel == nullptr || g_fb[0] == nullptr) return;

  // 这一幅画完之后:① 这次刷新算不算"整屏"?② 把这次的脏区登记成"待补拷清单"
  //   (下一次换帧生效之后,由 pend_step() 在空闲时间里一小段一小段补到另一块 fb)
  const bool full = rows_all();
  g_pend_n = g_dmg_n;
  for (uint8_t i = 0; i < g_dmg_n; ++i) g_pend[i] = g_dmg[i];
  g_pend_full = full || g_dmg_overflow;   // 整屏/脏区太多 ⇒ 按整屏补(保守但一定对)
  g_pend_rect = 0;
  g_pend_y = 0;
  g_pend_active = (g_pend_full || g_pend_n > 0) && g_fb[0] != nullptr;
  g_dmg_n = 0;
  g_dmg_overflow = false;

  const uint32_t t0 = micros();
  // ★ 先记屏障(换帧前),再登记新 fb:于是"等屏障变过"= 等**这次登记之后**的那个帧边界
  //   (顺序反过来的话,极端情况下会把"登记前刚好过去的那次 wrap"当成已经生效)。
  g_swap_barrier = swap_barrier();
  esp_lcd_panel_draw_bitmap(g_panel, 0, 0, (int)THEME_DISPLAY_RES, 1, g_fb[g_back]);
  const uint32_t dt = (uint32_t)(micros() - t0);
  const uint32_t phase = (uint32_t)(micros() - g_vsync_us);   // 帧内相位(证据用)
  if (phase > g_swap_phase_max_us) g_swap_phase_max_us = phase;

  g_front = g_back;
  g_back = (uint8_t)(1u - g_front);
  ++g_swap;
  if (dt > g_copy_us_max) g_copy_us_max = dt;   // 换帧本身也记进最大值(它极小)

  // 刷新节奏诊断(见 g_swap_int_*):两次换帧的间隔 = 一次 LVGL 刷新的周期
  {
    const uint32_t now_us = micros();
    if (g_swap_last_us != 0) {
      const uint32_t iv = now_us - g_swap_last_us;
      if (iv < g_swap_int_min_us) g_swap_int_min_us = iv;
      if (iv > g_swap_int_max_us) g_swap_int_max_us = iv;
      g_swap_int_sum_us += iv;
      ++g_swap_int_n;
    }
    g_swap_last_us = now_us;
  }
  if (full) ++g_fullrb_n;

  if (full) {
    // 整屏刷新:把"从第一块 flush 到换帧"这段耗时打出来 —— 这就是
    // "开机整屏刷新耗时"那个数(见 ACCEPTANCE / docs 第 9 节)。
    dash_logf("rgb: 整屏刷新 %.1fms(块=%u 拷贝%.1fms 数据%.0fKB) "
              "换帧在下一个帧边界由驱动锁存(bb_fb_index)\n",
                  (double)(micros() - g_refr_t0_us) / 1000.0,
                  (unsigned)g_refr_flush_n, (double)g_refr_copy_us / 1000.0,
                  (double)RGB_FB_BYTES / 1024.0);
  }
  rows_reset();
  g_refr_copy_us = 0;
  g_refr_flush_n = 0;
}

// LVGL → 面板:把这一块搬进 **back** fb;一次刷新的最后一块再请求换帧。
static void rgb_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  ++g_flush_count;
  if (g_panel == nullptr || g_fb[0] == nullptr) {   // 没面板:直接放行,别把 LVGL 卡死
    ++g_skip_count;
    lv_display_flush_ready(disp);
    return;
  }
  const bool first_of_refresh = (g_refr_flush_n == 0);
  if (first_of_refresh) g_refr_t0_us = micros();   // 本次刷新的第一块:起表
  ++g_refr_flush_n;

  // ① 裁剪(LVGL 不该给越界的区域,这里只是不信任输入)
  int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > (int32_t)THEME_DISPLAY_RES - 1) x2 = (int32_t)THEME_DISPLAY_RES - 1;
  if (y2 > (int32_t)THEME_DISPLAY_RES - 1) y2 = (int32_t)THEME_DISPLAY_RES - 1;
  if (x2 < x1 || y2 < y1) { lv_display_flush_ready(disp); return; }

  // ② 动手之前:确认上一次换帧的那个边界已经过去(见 wait_swap_settled)
  wait_swap_settled();

  // ③ ★★ 本次刷新的第一块:确认"上一次的脏区"已经全部补进这块 back ——
  //    正常情况下 pend_step() 已经在空闲时间里补完了(这里只是走个空循环);
  //    万一刷新来得早,这里把剩余的一次补完。不补完就换帧 ⇒ 两块 fb 内容分叉
  //    ⇒ 屏上"表情/进度条来回跳"(车主实测过的那条)。
  if (first_of_refresh) pend_finish();

  // ④ 搬像素(只往 back 写 ⇒ 不碰正在显示/被读的那块 ⇒ 无撕裂)
  const uint32_t c0 = micros();
  blit_area(g_fb[g_back], px, x1, y1, x2, y2);
  const uint32_t dt = (uint32_t)(micros() - c0);
  g_copy_us_sum += dt;
  g_refr_copy_us += dt;
  ++g_copy_n;
  if (dt > g_copy_us_max) g_copy_us_max = dt;
  if (dt > g_blit_max_us) g_blit_max_us = dt;   // ★"当次脏区那一笔"单独记(车主要的诊断)

  // ⑤ 记账:① 这一笔进"本次脏区";② 整行都写了就标进整屏判据
  dmg_add(x1, y1, x2, y2);
  if (x1 == 0 && x2 == (int32_t)THEME_DISPLAY_RES - 1) rows_mark(y1, y2);

  // ⑥ 一次刷新的最后一块:请求换帧(vsync 边界整块换过去)
  if (lv_display_flush_is_last(disp)) request_swap();

  lv_display_flush_ready(disp);
}

// ------------------------------------------------------------
// 4) 板载 TCA9554PWR(I2C 扩展):**RESET 与 CS 都不在 GPIO 上**
//
//   这块板把 LCD_RST 放在 EXIO1、LCD_CS 放在 EXIO3(wiki 引脚表的 "EXIO1/EXIO3"
//   就是这里),所以"点屏"除了 SPI/RGB 那二十来根线,还必须先打通 I2C:
//       · I2C:SCL=GPIO7 / SDA=GPIO15(12PIN 上那两根,板上共用;见 wiki 接口表)
//       · TCA9554 地址 0x20;寄存器 0x01=输出、0x03=方向(0=输出)
//       · EXIO 编号按**位**算:EXIO1=bit0、EXIO3=bit2、EXIO8=bit7(蜂鸣器)
//   ★ 顺序是硬的:`Wire.begin` → 方向全设输出 → 拉 RST 低→高 → **再**拉 CS 低,
//     然后才开始写初始化命令。少任何一步的症状都是"屏全黑,而串口一切正常"。
//   ★ 例程还把 EXIO8 拉低(蜂鸣器关)。这里也拉一下:输出寄存器复位值本来就是 0,
//     显式写一次是为了"以后谁想在 EXIO 上加点什么"时不会先被蜂鸣器吓一跳。
// ------------------------------------------------------------
#include <Wire.h>
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#define LCD_RST_EXIO_BIT    0    // EXIO1
#define LCD_CS_EXIO_BIT     2    // EXIO3
#define BUZZER_EXIO_BIT     7    // EXIO8
#define RGB_PIN_BL          6    // LCD_BL:背光(高有效,板上经 MOS 管)

static uint8_t g_exio_out = 0x00;   // 影子寄存器:Set_EXIO 是"读-改-写",别丢别的位

static void tca9554_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void tca9554_begin() {
  Wire.begin(15, 7);                  // SDA=GPIO15, SCL=GPIO7
  tca9554_write(TCA9554_REG_CONFIG, 0x00);   // 8 个口全设成输出(例程 TCA9554PWR_Init(0x00))
  tca9554_write(TCA9554_REG_OUTPUT, g_exio_out);
}

static void tca9554_set(uint8_t bit, bool high) {
  if (high) g_exio_out |= (uint8_t)(1u << bit);
  else      g_exio_out &= (uint8_t)~(1u << bit);
  tca9554_write(TCA9554_REG_OUTPUT, g_exio_out);
}

// 背光:PWM 走 LEDC。★ 这块框架是 arduino-esp32 **3.3.9**,LEDC 的 API 在 3.x
// 换过一次:2.x 是 `ledcSetup(通道,频率,位数)` + `ledcAttachPin(脚,通道)` 两步,
// 3.x 合成一步 **`ledcAttach(脚, 频率, 位数)`**,之后 `ledcWrite(脚, 占空比)`
// (按**脚**寻址,不再是我们自己挑通道)。★ 频率/位数/占空比一个字没变:
// 例程用 20kHz / 10 位 / 50%,这里照抄(频率落在人耳外,不会听见啸叫)。
#define RGB_BL_LEDC_HZ   20000
#define RGB_BL_LEDC_BITS 10
#define RGB_BL_DUTY      512    // 10 位的一半 ≈ 50%

// 写一条初始化命令:**裸 spi_master**(和官方例程 Display_ST7701.cpp 一模一样)
//
// ★ 为什么不用 esp_lcd_panel_io_spi(骨架原稿的写法):
//   ST7701 的 3 线 SPI 是"**9 位**"帧 —— 第 1 位 0=命令、1=数据,后面 8 位是内容,
//   例程把它表达成 spi_device 的 `command_bits=1 + address_bits=8`(这是 esp_lcd
//   那套 IO 里没有的形状;而且 CS 还不在 GPIO 上,-1 之后总线谁初始化的也不确定)。
//   例程那段是**在这块板上验过**的,直接照抄最省事:总线自己 init、设备自己 add。
static spi_device_handle_t g_spi = nullptr;

static void st7701_tx(uint8_t is_data, uint8_t v) {
  if (g_spi == nullptr) return;
  spi_transaction_t t = {};
  t.cmd = is_data ? 1 : 0;    // command_bits=1:0=命令,1=数据
  t.addr = v;                 // address_bits=8:内容
  t.length = 0;               // 没有数据阶段
  spi_device_transmit(g_spi, &t);
}

static void panel_init_sequence() {
  if (g_spi == nullptr) return;
  for (size_t i = 0; i < kPanelInitCount; ++i) {
    const RgbInitCmd& c = kPanelInit[i];
    st7701_tx(0, c.cmd);
    for (uint8_t k = 0; k < c.len; ++k) st7701_tx(1, c.data[k]);
    if (c.delay_ms) delay(c.delay_ms);
  }
}

void dash_display_init() {
  // ---- ① 先把 RST/CS 那两颗扩展口的片子叫醒(TCA9554)----
  tca9554_begin();
  tca9554_set(BUZZER_EXIO_BIT, false);
  // 复位脉冲:低 10ms → 高 → 等 50ms(例程 ST7701_Reset() 的时序)
  tca9554_set(LCD_RST_EXIO_BIT, false); delay(10);
  tca9554_set(LCD_RST_EXIO_BIT, true);  delay(50);

  // ---- ② 3 线 SPI:只用来写初始化命令,不进画 ----
  //   SCLK=GPIO2 / MOSI(SDA)=GPIO1,SPI2_HOST、模式 0、40MHz(例程的取值)。
  //   ★ CS 由 EXIO3 手动拉(下面 tca9554_set),所以这里 spics_io_num = -1:
  //     填成某个 GPIO 的话,驱动会去动一根**没接屏**的脚,而真正的 CS 一直浮着
  //     —— 症状同样是全黑。
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = RGB_PIN_INIT_SDA;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = RGB_PIN_INIT_SCLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 64;
  esp_err_t berr = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (berr != ESP_OK) {
    dash_logf("rgb: 3线SPI 总线初始化失败 err=%d\n", (int)berr);
  }
  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits = 1;      // ← 第 9 位:0=命令 / 1=数据
  devcfg.address_bits = 8;      // ← 后 8 位:内容
  devcfg.mode = 0;
  devcfg.clock_speed_hz = 40 * 1000 * 1000;
  devcfg.spics_io_num = -1;     // CS 在 EXIO3 上(见上)
  devcfg.queue_size = 1;
  esp_err_t derr = spi_bus_add_device(SPI2_HOST, &devcfg, &g_spi);
  if (derr != ESP_OK) {
    dash_logf("rgb: 3线SPI 设备注册失败 err=%d\n", (int)derr);
  }
  tca9554_set(LCD_CS_EXIO_BIT, false);   // CS 低:开始收命令
  delay(10);
  panel_init_sequence();
  tca9554_set(LCD_CS_EXIO_BIT, true);    // CS 高:命令写完就不再用 SPI 了
  delay(10);

  // ---- RGB 并口 ----
  esp_lcd_rgb_panel_config_t cfg = {};
  // ★ 18MHz 走 PLL160M(160/18≈8.89,分频器带小数部分,能凑准)。
  //   换了 pclk 记得一起看这一行:30MHz 也在这个源上试过(见文件头第 3 块)。
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = RGB_PIXEL_CLOCK_HZ;
  cfg.timings.h_res = THEME_DISPLAY_RES;
  cfg.timings.v_res = THEME_DISPLAY_RES;
  cfg.timings.hsync_pulse_width = RGB_HSYNC_PULSE;
  cfg.timings.hsync_back_porch = RGB_HSYNC_BACK;
  cfg.timings.hsync_front_porch = RGB_HSYNC_FRONT;
  cfg.timings.vsync_pulse_width = RGB_VSYNC_PULSE;
  cfg.timings.vsync_back_porch = RGB_VSYNC_BACK;
  cfg.timings.vsync_front_porch = RGB_VSYNC_FRONT;
  cfg.timings.flags.pclk_active_neg = RGB_PCLK_ACTIVE_NEG;
  cfg.timings.flags.hsync_idle_low = RGB_HSYNC_IDLE_LOW;
  cfg.timings.flags.vsync_idle_low = RGB_VSYNC_IDLE_LOW;
  cfg.data_width = 16;                    // RGB565
  cfg.bits_per_pixel = 16;
  // ★★ 就是这两行把撕裂根治掉的(旧栈里这两个字段**根本不存在**,见文件头):
  cfg.num_fbs = 2;                        // 两块整屏 fb(各 450KB,在 PSRAM)
  cfg.flags.fb_in_psram = 1;
  // ★★★ 2026-09-24 第三轮:**第三行才是关键** —— bounce buffer(照微雪官方 2.8C 例程抄)。
  //   没有它:每个 VSYNC 都把 DMA 重置回**固定在 fbs[0]** 的 restart link ⇒ 换帧
  //   等于没换(残留 + 撕裂,见文件头)。有了它:DMA 只读内部 SRAM 的 bounce buffer,
  //   而"装哪块 fb 的数据"由驱动在**帧边界**锁进 `bb_fb_index` ⇒ 换帧真的生效。
#if RGB_BOUNCE_LINES > 0
  cfg.bounce_buffer_size_px = (size_t)RGB_BOUNCE_LINES * (size_t)THEME_DISPLAY_RES;
#endif
  // ★ IDF 5.5 里 `psram_trans_align`/`sram_trans_align` 已经 deprecated(同一个
  //   union 的 `dma_burst_size`);不写就是驱动默认值,别再去写那两个旧名字。
  cfg.hsync_gpio_num = RGB_PIN_HSYNC;
  cfg.vsync_gpio_num = RGB_PIN_VSYNC;
  cfg.de_gpio_num = RGB_PIN_DE;
  cfg.pclk_gpio_num = RGB_PIN_PCLK;
  cfg.disp_gpio_num = -1;                 // 背光/显示使能另接 MOSFET(见 PINOUT)
  const int data_pins[16] = RGB_DATA_GPIOS;
  for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = data_pins[i];

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
  if (err != ESP_OK || g_panel == nullptr) {
    // 不静默:黑屏时这一行是唯一线索
    dash_logf("rgb: 面板创建失败 err=%d(检查引脚/时序/PSRAM)\n", (int)err);
    g_panel = nullptr;
    return;
  }

  // ★ on_vsync 必须在 `esp_lcd_panel_init()` **之前**注册:`init` 里就开 DMA/开扫描,
  //   注册晚了几帧也无所谓,但"先注册、后起扫"顺序更干净。
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = on_vsync;                // 每帧一次(诊断 + 换帧边界的判据)
  // ★ 本轮新增:整帧走完那一刻的回调(= 驱动锁存 `bb_fb_index` 的那一刻)。
  //   它在中断上下文里跑,只加计数(见 on_frame_complete)。
  cbs.on_frame_buf_complete = on_frame_complete;
  err = esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr);
  if (err != ESP_OK) {
    dash_logf("rgb: on_vsync 回调注册失败 err=%d(换帧的边界判据就没有了)\n", (int)err);
  }

  // ★★★ 这两行**不能省**:`esp_lcd_new_rgb_panel()` 只是把面板对象建起来,
  //   真正**开 DMA / 启动 LCD_CAM 连续扫描**的是 `esp_lcd_panel_init()`。
  //   少了它,面板一个像素都不发 —— 而症状极具误导性:
  //     · 串口一切正常、"rgb: 已就绪"照打、`draw_bitmap()` 也照抄进 fb;
  //     · 屏是**纯黑**;
  //     · 唯一能看出来的数字是每秒那行 `rgb: vsync=0(+0/s)`
  //       (vsync 由 VSYNC 中断里的回调累加,没扫描就永远是 0)。
  //   2026-09-24 第一次烧上 2.1 板时踩的正是这一条(骨架原稿漏了这两行,
  //   它是照 IDF 5.x 的习惯写的;官方例程 Display_ST7701.cpp 结尾有这两句)。
  //   `esp_lcd_panel_reset()` 对 RGB 面板是空操作(没有独立复位脚,
  //   复位走的是 EXIO1,前面已经拉过了),留着是为了与例程/IDF 文档一致。
  esp_lcd_panel_reset(g_panel);
  esp_lcd_panel_init(g_panel);

  // ---- 拿到**两块** framebuffer 的地址(旧栈没有这个 API,见文件头)----
  uint32_t fb_count = 0;
  err = esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, (void**)&g_fb[0], (void**)&g_fb[1]);
  if (err != ESP_OK || g_fb[0] == nullptr || g_fb[1] == nullptr) {
    dash_logf("rgb: 取 framebuffer 失败 err=%d(拿不到双缓冲就没法无撕裂)\n", (int)err);
    g_fb[0] = g_fb[1] = nullptr;
  } else {
    fb_count = 2;
    // 驱动用 `heap_caps_aligned_calloc` 分配 ⇒ 两块都是**全 0(黑)**。
    // 两块 fb 的收敛不靠"是不是完整"这个标记,而靠"每次刷新补拷上一次的脏区"
    // (见 pend_step):第一次整屏刷新之后,空闲时间里就会把整屏一小段一小段补进另一块。
    g_front = 0;
    g_back = 1;
    g_pend_n = 0;
    g_pend_full = false;
    g_pend_active = false;
    g_pend_rect = 0;
    g_pend_y = 0;
    g_dmg_n = 0;
    g_dmg_overflow = false;
    rows_reset();
  }

  // ---- 两个 lv_display:单屏版本先都画到同一块屏上 ----
  //   ★ 第二块屏到货后,把 g_right 换成它自己的 flush(见文件头"双屏出路");
  //     现在这样至少能把"两块屏各自要显示什么"的 UI 逻辑先跑通。
  lv_display_t* d0 = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_flush_cb(d0, rgb_flush_cb);
  // LVGL 的绘制缓冲:**内部 SRAM 的小 PARTIAL 缓冲**(480 行里的一小段),
  // 不是整屏缓冲 —— 渲染完一段就由 flush 搬进 back fb。整屏缓冲没必要:
  // 450KB×2 已经在 PSRAM 里当 framebuffer 了(见 cfg.num_fbs)。
  // ★★ 这一行同时决定**"当次脏区"那一笔 memcpy 有多大**(= 一次 flush 的最坏情况):
  //   40 行 ⇒ 38.4KB ⇒ 实测 ~3ms 的 PSRAM 突发 ✗ —— 它和 bounce 的填充撞上就是
  //   "那一帧吐旧行"(车主看到的"**时有时无**的横纹" ✓)。本轮缩到 **16 行(15.4KB ≈1.2ms)**,
  //   而两块 bounce buffer 合计能吸收 ~19KB 的赤字 ⇒ 这一笔**能被吸收** ✓。
  //   代价:整屏刷新从 12 块变 30 块(每块固定开销变大),稳态 UI 是局部刷新,几乎无感 ✓。
  // ★★ `aligned(LV_DRAW_BUF_ALIGN)` **一个字都不能少**(2026-09-24 实机踩的坑):
  //   LVGL 9.3 的 `lv_display_set_buffers()` 会先校验
  //       buf1 == lv_draw_buf_align(buf1)      // 即 buf1 必须按 LV_DRAW_BUF_ALIGN(=4) 对齐
  //   不满足就**静默 return**(LV_USE_LOG=0 时连一行警告都没有)⇒ 这条缓冲**根本没装上**。
  //   症状:面板在扫(`vsync` 每秒 +64.7,分毫不差)、UI 树齐全,但 `flush=0`、屏全黑。
  //   而 `lv_color_t` 是 24 位(3 字节)⇒ 这个数组只保证 2 字节对齐,实测 misalign=1。
#ifndef RGB_DRAW_BUF_LINES
#define RGB_DRAW_BUF_LINES 16
#endif
  static lv_color_t draw_buf[THEME_DISPLAY_RES * RGB_DRAW_BUF_LINES]
      __attribute__((aligned(LV_DRAW_BUF_ALIGN)));
  lv_display_set_buffers(d0, draw_buf, nullptr, sizeof(draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  g_left = d0;
  g_right = d0;

  // ---- ③ 背光最后开 ----
  //   放在这里而不是开头,是为了"先有画面、再点亮":反过来的话,初始化那 600ms
  //   里屏是亮的但没内容(白/雪花),看起来像花屏,容易误判。
  ledcAttach((uint8_t)RGB_PIN_BL, (uint32_t)RGB_BL_LEDC_HZ, (uint8_t)RGB_BL_LEDC_BITS);
  ledcWrite((uint8_t)RGB_PIN_BL, (uint32_t)RGB_BL_DUTY);

  dash_logf("rgb: RGB565 %dx%d pclk=%uHz 数据位=%d 已就绪(第二块屏待接)\n",
                (int)THEME_DISPLAY_RES, (int)THEME_DISPLAY_RES,
                (unsigned)RGB_PIXEL_CLOCK_HZ, 16);
  dash_logf("rgb: 双framebuffer num_fbs=%u fb0=%p fb1=%p(各 %uKB PSRAM) "
            "on_vsync=已注册\n",
                (unsigned)fb_count, (void*)g_fb[0], (void*)g_fb[1],
                (unsigned)(RGB_FB_BYTES / 1024u));
  // ★★ 本轮最关键的一行日志:换帧到底靠什么生效(有 bounce / 没 bounce 是天壤之别)
#if RGB_BOUNCE_LINES > 0
  dash_logf("rgb: bounce=%d行/块(2块共%uKB内部SRAM) on_frame_buf_complete=已注册 "
            "⇒ 换帧在**帧边界**由驱动锁存(bb_fb_index=cur_fb_index);DMA 不读 PSRAM\n",
                (int)RGB_BOUNCE_LINES,
                (unsigned)((size_t)RGB_BOUNCE_LINES * THEME_DISPLAY_RES * 2u * 2u / 1024u));
#else
  dash_logf("rgb: bounce=关(退回档!) ⇒ 每个 VSYNC 都把 DMA 重置回固定的 fbs[0],"
            "换帧不生效(残留+撕裂会回来;仅用于复现/对照)\n");
#endif
  dash_logf("rgb: 刷新档=%s(每%ums整屏失效一次;自动交替=%ums)——本行是"
            "\"当前是全屏重绘还是局部刷新\"的判据\n",
                g_full_refresh ? "全屏重绘" : "局部刷新",
                (unsigned)RGB_FULL_REFRESH_PERIOD_MS,
                (unsigned)RGB_FULL_REFRESH_ALTERNATE_MS);
  // ★ 两块 fb 靠什么"内容一致":把上一次的脏区在空闲时间里小碎步补到另一块(见 pend_step)。
  //   少了它,局部刷新会让两块 fb 内容分叉 ⇒ 屏上"表情/进度条来回跳"。
  dash_logf("rgb: 两块fb收敛=空闲时间小碎步补拷上一次的脏区(每步%d行,pend_step)"
            " ⇒ 局部刷新也不会跳回旧内容;换帧=帧边界锁存\n", (int)RGB_CATCHUP_STEP_ROWS);
  // ★ "当次脏区那一笔"有多大 = LVGL 绘制缓冲的大小(见 RGB_DRAW_BUF_LINES):
  //   它是**仅剩的、没法再摊平**的一笔(必须等 LVGL 回用缓冲),所以压到 bounce 能吸收的量级。
  dash_logf("rgb: 脏区单笔上限=%d行(%uB) —— bounce 两块共%uKB,能吸收 ~19KB 赤字\n",
            (int)RGB_DRAW_BUF_LINES,
            (unsigned)((uint32_t)RGB_DRAW_BUF_LINES * THEME_DISPLAY_RES * 2u),
            (unsigned)((size_t)RGB_BOUNCE_LINES * THEME_DISPLAY_RES * 2u * 2u / 1024u));
  dash_logf("rgb: 板=微雪 ESP32-S3-LCD-2.8C(非触控) ST7701 RST=EXIO1 CS=EXIO3 "
            "BL=GPIO%d/PWM%d @%u%%\n",
                (int)RGB_PIN_BL, (int)RGB_BL_LEDC_HZ,
                (unsigned)(RGB_BL_DUTY * 100u / (1u << RGB_BL_LEDC_BITS)));

  // 诊断：双 fb 拿到手之后再报一次空闲内存 —— framebuffer 是 2×450KB，
  // 这一步才看得出“双缓冲到底吃掉多少 PSRAM”（自检那行跑在显示初始化之前）。
  dash_logf("rgb: 双fb 之后 空闲 PSRAM=%uKB heap=%uKB(内部)\n",
                (unsigned)(ESP.getFreePsram() / 1024u),
                (unsigned)(ESP.getFreeHeap() / 1024u));
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

// 每秒报一次帧率 —— 实屏调试时这是判断"面板到底在不在收帧"的第一手信息
// (和 VAN 那条 edges/frames 的诊断是同一个思路),同时也是**换帧健不健康**的判据:
//   · `vsync` 每秒 +N:N≈65 ⇒ PCLK 真跑在 18MHz(N≈108 ⇒ 30MHz);
//   · `wrap` 每秒 +N:**驱动走完整帧的次数**。bounce 模式下换帧就在这一刻锁存
//     ⇒ `vsync` 在涨而 `wrap` 不涨 = bounce 没成立(屏上会退回"只有 fbs[0]")。
//   · `swap` 跟着 flush 涨:每次 LVGL 刷新都登记了一次换帧;
//   · **`timeout=0`**:等"换帧已生效"这道门从来没有靠超时放行;
//   · `copy_max/copy_avg`:一次往 back fb 搬像素(含 cache 回写)要多久;
//   · `catchup=N(+n/s xKB/s)`:**补拷上次脏区到另一块 fb** 的次数与总量 ——
//     它才是"两块 fb 内容一致"的判据(稳态 n ≈ 刷新次数、xKB 很小;整屏档才会大);
//   · `phase_max`:换帧请求落在帧内最靠后的那个相位(相对上一个 VSYNC,µs);
//   · `bounce=N MB/s`:bounce 模式每帧从 PSRAM 搬进内部 SRAM 的量(= 那一行的代价);
//   · `fb=front/back`:当前哪块在显示、往哪块画;
//   · `模式=全屏重绘|局部刷新`:当前对比档(**这一行就是"现在是哪一档"的判据**)。
void dash_display_poll() {
  static uint32_t last_ms = 0;
  static uint32_t last_vsync = 0;
  static uint32_t last_wrap = 0;
  static uint32_t last_swap = 0;
  static uint32_t last_catchup = 0;
  static uint32_t last_catchup_kb = 0;
  static uint32_t last_forced_kb = 0;
  static uint32_t last_copy_sum = 0;
  static uint32_t last_copy_n = 0;
  const uint32_t now = millis();

  // ---- ① 对比档:全屏重绘 / 局部刷新 ----
  //   ★ 这一段**必须放在下面那个 "1 秒才打一行" 的早退之前**(否则开关只在打日志时生效)。
  // ---- ② 空闲时间里的"小碎步补拷"(第五轮:把大突发摊平,见 pend_step)----
  //   也必须在早退之前 —— 它每圈只搬 8 行,靠"跑很多圈"把 ~90KB 摊开搬完。
  pend_step();
#if RGB_FULL_REFRESH_ALTERNATE_MS > 0
  if (g_mode_switch_ms == 0) g_mode_switch_ms = now + RGB_FULL_REFRESH_ALTERNATE_MS;
  if ((int32_t)(now - g_mode_switch_ms) >= 0) {
    g_mode_switch_ms = now + RGB_FULL_REFRESH_ALTERNATE_MS;
    g_full_refresh = !g_full_refresh;
    dash_logf("rgb: ===== 对比档自动切换 ⇒ %s =====\n"
              "     全屏重绘档:每 %ums 让当前屏整体失效一次(swap/s 就是整屏能画多少张/秒);\n"
              "     局部刷新档:只重画 LVGL 标脏的区域(正式档)。\n",
                  g_full_refresh ? "全屏重绘" : "局部刷新",
                  (unsigned)RGB_FULL_REFRESH_PERIOD_MS);
  }
#endif
  if (g_full_refresh) {
    // 让"当前屏"整体失效 ⇒ LVGL 下一轮把 480×480 全部重画一遍(可测开关的"开"档)。
    // ★ 必须在这里调(循环任务里、`lv_timer_handler()` 之外):LVGL 9 明令
    //   "Invalidate area is not allowed during rendering"(lv_refr.c 里那条断言),
    //   所以在 flush 回调里标脏是**不行**的。
    if ((uint32_t)(now - g_full_refr_last_ms) >= RGB_FULL_REFRESH_PERIOD_MS) {
      g_full_refr_last_ms = now;
      lv_obj_invalidate(lv_screen_active());
    }
  }

  if (now - last_ms < 1000) return;
  const uint32_t v = g_vsync;
  const uint32_t wp = g_frame_wrap;
  const uint32_t sw = g_swap;
  const uint32_t dn = g_copy_n - last_copy_n;
  const uint32_t dsum = g_copy_us_sum - last_copy_sum;
  // bounce 模式下驱动每帧要从 PSRAM 搬多少:wrap/s × 一帧的字节数(这就是那一行的代价)
  const uint32_t fill_mb = (uint32_t)(((uint64_t)(wp - last_wrap) * RGB_FB_BYTES) / (1024ull * 1024ull));
  const uint32_t dcatch = g_catchup_n - last_catchup;
  const uint32_t dcatch_kb = g_catchup_kb - last_catchup_kb;
  const uint32_t dforced = g_forced_kb - last_forced_kb;
  const uint32_t iv_avg = g_swap_int_n ? (uint32_t)(g_swap_int_sum_us / g_swap_int_n) : 0u;
  const uint32_t iv_min = (g_swap_int_n && g_swap_int_min_us != 0xFFFFFFFFu) ? g_swap_int_min_us : 0u;
  // ★ 车主要的诊断:**每一笔 PSRAM 搬运的最大耗时,按来源分开**
  //   blit_max = 当次脏区那一笔(受 RGB_DRAW_BUF_LINES 限制) / step_max = 小碎步一步
  dash_logf("rgb: vsync=%u(+%u/s) wrap=%u(+%u/s) swap=%u(+%u/s) flush=%u "
            "blit_max=%uus step_max=%uus copy_max=%uus copy_avg=%uus swap_wait_max=%uus "
            "phase_max=%uus timeout=%u fb=%u/%u "
            "catchup=%u(+%u/s %uKB/s forced%uKB) refresh=%u/%u/%uus fullrb=%u/s bounce=%uMB/s "
            "模式=%s psram=%uKB heap=%uKB\n",
                (unsigned)v, (unsigned)(v - last_vsync),
                (unsigned)wp, (unsigned)(wp - last_wrap),
                (unsigned)sw, (unsigned)(sw - last_swap),
                (unsigned)g_flush_count,
                (unsigned)g_blit_max_us, (unsigned)g_step_max_us,
                (unsigned)g_copy_us_max, (unsigned)(dn ? (dsum / dn) : 0u),
                (unsigned)g_swap_wait_max_us,
                (unsigned)g_swap_phase_max_us,
                (unsigned)g_swap_timeout,
                (unsigned)g_front, (unsigned)g_back,
                (unsigned)g_catchup_n, (unsigned)dcatch, (unsigned)dcatch_kb, (unsigned)dforced,
                (unsigned)iv_avg, (unsigned)iv_min, (unsigned)g_swap_int_max_us,
                (unsigned)g_fullrb_n,
                (unsigned)fill_mb,
                g_full_refresh ? "全屏重绘" : "局部刷新",
                (unsigned)(ESP.getFreePsram() / 1024u),
                (unsigned)(ESP.getFreeHeap() / 1024u));
  last_forced_kb = g_forced_kb;
  // 刷新节奏统计:每秒清零重来(min 用 0xFFFFFFFF 当"还没测到")
  g_swap_int_n = 0;
  g_swap_int_sum_us = 0;
  g_swap_int_min_us = 0xFFFFFFFFu;
  g_swap_int_max_us = 0;
  g_fullrb_n = 0;
  last_vsync = v;
  last_wrap = wp;
  last_swap = sw;
  last_catchup = g_catchup_n;
  last_catchup_kb = g_catchup_kb;
  last_copy_sum = g_copy_us_sum;
  last_copy_n = g_copy_n;
  last_ms = now;
}

#endif  // DASH_DISPLAY_RGB
