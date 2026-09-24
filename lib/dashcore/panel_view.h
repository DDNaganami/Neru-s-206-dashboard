#pragma once
#include <stdint.h>
#include <stdio.h>    // snprintf（panelDescribe）
#include <math.h>     // sqrt（sqrtf 走 math.h；设备端 newlib 也有）
#include <string.h>
#include "lamp_view.h"   // kLampBaseRes（屏基准分辨率，480）

// ============================================================
// 圆屏可视化区 —— 「这块屏是圆的」这件事的**唯一一份几何**
// （2026-09-24 新增；任务：pcpreview 出「2.8C（最终板）」档）
//
// ★ 为什么单独一个头（而不是写进 src/dash_display.cpp 的预览那一段）：
//   ① **native 能测**：`lib_archive = no` 的 native 构建只编 `lib/`，
//      所以"想测谁就把它放进 lib/"（与 lamp_view.h / preview_input.h 同一条
//      结构约束）。下面这几条不变量算错**不会报错**，只会让遮罩圈画歪一格 ——
//      那正是最需要机器钉住的一类东西。
//   ② **PC 预览与主题编辑器必须引用同一份数字**：网页那边（JS）按同样口径给出
//      "内切正方形边长 336 / 168"。这边是固件/预览侧的权威复述，改动时
//      两边一起看（网页侧在 tools/theme-editor/asset-spec.js 的 circleSafeSide）。
//
// ★ 这块板是谁（口径来自 PURCHASE.md 第六节的表，**别自己发明数字**）：
//     微雪 ESP32-S3-LCD-2.8C，圆屏，480×480，ST7701S，RGB 并口；
//     **有效区（可视圆）Ø70.13 mm** —— PURCHASE.md 第六节表格里
//     Dwin / 微雪 2.8C / 鑫洪泰 / Wisecoco 四家都是这个数（70.128~70.13）。
//   它**不是** 240 档那块：240 档是微雪 ESP32-S3-DualEye-Touch-LCD-1.28
//   （两块 240×240 GC9A01A，2026-09-22 已退货，见 README.md / platformio.ini）。
//
// ★ 为什么要"遮罩圈"：屏是方的（480×480 像素矩阵），外面是圆的（Ø70.13）。
//   画布四角**物理上不可见**。而素材/几何算错时最常见的表现恰恰是"内容伸到
//   四角去了，真机上被圆边吃掉，PC 上却看得见" —— 于是 PC 预览必须把
//   这个圆**画出来**：圈内 = 真机能看见的，圈外 = 真机上不存在的。
//
// ★ 为什么不做成"圆外一律涂黑"：预览落的是**同一个 LVGL 渲染缓冲**，
//   涂黑会让人分不清"这是遮罩"还是"固件真的画黑了"。所以圆外是**压暗**
//   （不是涂黑）+ 斑点填充（一眼看出是标注层，不是画面内容）。
//   这个压暗**只发生在落盘的 BMP 上**（preview 侧），LVGL 缓冲与设备端
//   一个像素都不动 —— 见 src/dash_display.cpp 的 `preview_panel_overlay()`。
// ============================================================

// 这块板的档位 id（给网页/文档/日志复述用；纯字符串，不参与几何）
#define PANEL_28C_TIER_ID    "2.8C"
#define PANEL_28C_TIER_LABEL "2.8C (final, 480x480 round ST7701S)"
#define PANEL_DUALEYE_TIER_ID    "DualEye"
#define PANEL_DUALEYE_TIER_LABEL "DualEye (historical, returned 2026-09-22)"

// 480 基准的方屏边长（= 像素矩阵边长 = THEME_BASE_RES = kLampBaseRes）。
// ★ 这三个数本来就是同一个数，这里**复述并 static_assert 钉住**：
//   万一哪天有人只改了其中一处，编译期就报出来（而不是屏上慢慢歪）。
static const int32_t kPanelBaseSide = 480;
static_assert(kPanelBaseSide == kLampBaseRes,
              "panel_view 的基准边长与 lamp_view 的 kLampBaseRes 对不上");

// 480×480 像素矩阵里，圆的直径就是整块屏的边长（有效区就是内切圆）。
// 之所以还要写出来：下面的 mm 换算要用它，而"用哪个数当直径"正是最容易
// 写错成"内切正方形边长"的地方。
static inline int32_t panelRoundDiameterPx(int32_t res) { return res; }

// ---- 物理尺寸（**单位：0.1 mm**，整数，避免浮点误差进几何）----
// 70.13 mm ⇒ 7013。见文件头：PURCHASE.md 第六节的「Ø 有效区」那一列。
static const int32_t kPanelActiveAreaMm10 = 7013;   // = 70.13 mm

// 圆心（像素，按屏分辨率算）：320 档不会用到（那是矩形屏的口径），
// 480 档就是 (239.5, 239.5)。这里用**整数像素索引**的口径：
// 像素 (x,y) 的几何中心在 (x+0.5, y+0.5) ⇒ 圆心的连续坐标是 res/2。
// 于是判断"这个像素可不可见"用的是**像素中心到圆心的距离**与半径比较 ——
// 这是唯一与"半像素偏移"无关的写法（`<= R` 而不是 `< R`：
// 边界那一圈像素真机上处在圆边上，宁可算作"可见"）。
static inline double panelRadiusPx(int32_t res) {
  return (double)panelRoundDiameterPx(res) / 2.0;
}

// 像素 (x,y) 的**中心**到屏心的距离（像素）。
static inline double panelDistPx(int32_t x, int32_t y, int32_t res) {
  const double c = (double)res / 2.0;
  const double dx = ((double)x + 0.5) - c;
  const double dy = ((double)y + 0.5) - c;
  return sqrt(dx * dx + dy * dy);
}

// 这个像素在真机上**看得见吗**（圆内 = 可见）。
static inline bool panelPixelVisible(int32_t x, int32_t y, int32_t res) {
  return panelDistPx(x, y, res) <= panelRadiusPx(res);
}

// 内切正方形边长（**向下取到 4 的倍数**）—— 与 tools/theme-editor/asset-spec.js
// 的 circleSafeSide() 逐位同口径：480 档 = 336、240 档 = 168。
// 意义：任何**矩形**素材（背景之外的贴图）要想整块都落在圆内，边长不能超过它。
static inline int32_t panelInscribedSquareSide(int32_t res) {
  // floor(res / sqrt(2)) 的整数算法：找最大的 s 使 2*s*s <= res*res。
  // ★ 用整数而不是 floor(res/1.41421356)：后者在 res=480 上给出 339
  //   （与网页一致），但那是浮点的巧合 —— 整数版在任何 res 上都不会差 1。
  int32_t s = (int32_t)((double)res / 1.4142135623730951);
  while ((int64_t)2 * s * s > (int64_t)res * res) --s;
  while ((int64_t)2 * (s + 1) * (s + 1) <= (int64_t)res * res) ++s;
  return (s / 4) * 4;   // 向下对齐到 4 的倍数（网页同一条）
}

// ---- mm 换算（给日志 / 文档 / 网页复述用；几何本身不依赖它）----
//
// ★★ 两个函数的单位**不一样**，名字里的数字说的就是精度，别照字面乘除：
//     `panelMmPerPx10000` → **每像素多少 mm**，单位 1e-4 mm；
//                           480 档 = 0.1461 mm/px ⇒ 返回 **1461**
//     `panelPxToMm100`    → **一段长度多少 mm**，单位 1e-2 mm；
//                           480 px = 70.13 mm ⇒ 返回 **7013**
//   （这一段我连着算错三次 —— 一度把"每像素 0.1461mm"写成 14610，
//     于是屏被算成 701 mm。这种错**不会让任何东西崩**，只会在串口/文档里
//     写错一个物理尺寸，所以 test_ui_lamps.cpp 里逐条钉着它。）
static inline int32_t panelMmPerPx10000(int32_t res) {
  if (res <= 0) return 0;
  // kPanelActiveAreaMm10 的单位是 1e-2 mm（7013 = 70.13 mm）；
  // 要落到 1e-4 mm，**乘 100** 得 701300，再除以像素数：
  //   701300 / 480 = 1461.04 ⇒ 1461（= 0.1461 mm/px）
  return (int32_t)(((int64_t)kPanelActiveAreaMm10 * 100 + res / 2) / res);
}

// 像素长度 → 1e-2 mm（展示值：480 px = **7013** = 70.13 mm）。
static inline int32_t panelPxToMm100(int32_t px, int32_t res) {
  // px × 每像素(1e-4 mm) = 单位 1e-4 mm；/100 ⇒ 单位 1e-2 mm。
  // 用 int64 是必须的：res 调到 1920 那一档时这个乘积会翻四倍，别留溢出的坑。
  const int64_t mm10000 = (int64_t)px * (int64_t)panelMmPerPx10000(res);
  return (int32_t)((mm10000 + 50) / 100);
}

// 一行"这块屏 + 可视圆"的说明（纯 ASCII —— README 那条纪律：预览/测试输出里的
// 中文在 GBK 控制台上会抛 UnicodeEncodeError，把统计打乱）。
// 写成"往里塞"的形式（buf 由调用方给），免得 preview 那边依赖 printf 的堆。
inline void panelDescribe(char* buf, size_t cap, int32_t res) {
  if (!buf || cap == 0) return;
  const int32_t d_mm100 = panelPxToMm100(panelRoundDiameterPx(res), res);
  const int32_t sq = panelInscribedSquareSide(res);
  snprintf(buf, cap,
           "panel %s: %dx%d px = %d.%02d mm active dia | "
           "inscribed square %d px | corners outside the circle are NOT visible",
           PANEL_28C_TIER_ID, (int)res, (int)res,
           (int)(d_mm100 / 100), (int)(d_mm100 % 100), (int)sq);
}

// ============================================================
// 遮罩的**着色规则**（把"圆外压暗"这件事讲成一条可测的规则）
//
// 返回值是"这个像素该乘多少亮度"（0..256 的定点数，256 = 原样）。
// ★ 三段，各有各的用处，**顺序也是口径**：
//   ① 圆内                 → 256（一个像素都不碰：圈内就是真机画面）
//   ② 圆外的参考圈带       → 128（半边暗：把"可视圆"这条边界描出来）
//   ③ 圈带以外的四角       → 32 / 64 交替（斑点：一眼看出是标注，不是画面）
// ★ 为什么圈带在**圆外**而不是压在圆上：压在圆上会把真机可见的边沿像素
//   改掉，那正是"素材有没有被切掉"要看的地方。圈外描边既标出边界，又不
//   遮挡任何可见内容。
// ============================================================
static const uint16_t kPanelShadeInside   = 256;   // 全亮（不碰）
static const uint16_t kPanelShadeRing     = 128;   // 参考圈带（半边暗）
static const uint16_t kPanelShadeCornerA  = 32;    // 四角斑点（暗）
static const uint16_t kPanelShadeCornerB  = 64;    // 四角斑点（稍亮）

// 参考圈带的宽度（像素，按屏分辨率缩放：480 档 = 3）。
// ★ 用 res/160 而不是写死 3：240 档（历史 DualEye）也能得到 1~2 px 的圈带，
//   而且这条与 theme_scale() 是同一套"按分辨率缩"的口径（不新造一套放大率）。
static inline int32_t panelRingWidthPx(int32_t res) {
  const int32_t w = res / 160;
  return w < 2 ? 2 : w;
}

// 像素 (x,y) 的亮度系数（0..256）。见上面三段说明。
static inline uint16_t panelShadeAt(int32_t x, int32_t y, int32_t res) {
  const double d = panelDistPx(x, y, res);
  const double r = panelRadiusPx(res);
  if (d <= r) return kPanelShadeInside;
  if (d <= r + (double)panelRingWidthPx(res)) return kPanelShadeRing;
  // 四角斑点：棋盘格。★ 用 (x/2 + y/2) 而不是 (x + y)：单像素棋盘在 480 上
  // 太密，缩到网页上看就是一片灰（那样"这块被切掉了"反而看不出来）。
  return (((x / 2) + (y / 2)) & 1) ? kPanelShadeCornerB : kPanelShadeCornerA;
}

// ============================================================
// 把遮罩**落到一个像素缓冲上**
//
// ★ 几何这一段**不认识任何颜色格式**：下面给两条路
//   ① `panelApplyOverlayRgb565()` —— 16 位 RGB565（本仓库预览的像素格式，
//      见 include/lv_conf.h 的 LV_COLOR_DEPTH；也是真机的线上格式）。
//      亮度的乘法**按通道做**（不是整字节乘），否则"半边暗"会串色。
//   ② `panelApplyOverlay()` 模板 —— 颜色格式由调用方给两个钩子。
//      留着它是因为"遮罩逻辑在别的深度上也要能跑"，而模板不引入任何依赖。
//
// 返回**被改过的像素数**（0 = 一个都没碰）。这个返回值是有用的：
//   ① 用例拿它断言"圆内一个像素都没动"；
//   ② preview 那边把它打进日志 —— "遮罩生效了吗"一眼可见，
//      而不是"看着好像有圈"。
// ============================================================
template <typename Pixel, typename ShadeFn, typename PutFn>
inline int32_t panelApplyOverlay(Pixel* buf, int32_t res,
                                 ShadeFn shade, PutFn put) {
  if (!buf || res <= 0) return 0;
  int32_t changed = 0;
  for (int32_t y = 0; y < res; ++y) {
    for (int32_t x = 0; x < res; ++x) {
      const uint16_t k = panelShadeAt(x, y, res);
      if (k >= kPanelShadeInside) continue;         // 圆内：一个字节都不碰
      const int32_t i = y * res + x;
      put(buf, i, shade(buf[i], k));
      ++changed;
    }
  }
  return changed;
}

// RGB565 的单通道缩放：`v` 是通道原始值（0..31 或 0..63），
// 结果仍在**同一个量程**里（乘 k/256 后四舍五入）。
// ★ 三个乘数都要先转大整数：480 档上 v*k 只有 31*256 = 7936，看着不会溢出，
//   但这行代码将来会被抄到"v 是 8 位"的地方 —— 那时 255*256*255 直接爆 int32。
//   显式用 uint32_t 之后，两种量程都对。
static inline uint16_t panelScaleRgb565Channel(uint16_t v, uint16_t k)
{
  return (uint16_t)(((uint32_t)v * (uint32_t)k + kPanelShadeInside / 2u) /
                    (uint32_t)kPanelShadeInside);
}

// 16 位 RGB565 缓冲上的遮罩（预览的实际路径；`res` 是方屏边长）。
inline int32_t panelApplyOverlayRgb565(uint16_t* buf, int32_t res) {
  return panelApplyOverlay<uint16_t>(
      buf, res,
      [](uint16_t px, uint16_t k) -> uint16_t {
        const uint16_t r = (uint16_t)panelScaleRgb565Channel((uint16_t)((px >> 11) & 0x1Fu), k);
        const uint16_t g = (uint16_t)panelScaleRgb565Channel((uint16_t)((px >> 5) & 0x3Fu), k);
        const uint16_t b = (uint16_t)panelScaleRgb565Channel((uint16_t)(px & 0x1Fu), k);
        return (uint16_t)((r << 11) | (g << 5) | b);
      },
      [](uint16_t* p, int32_t i, uint16_t v) { p[i] = v; });
}

// 圆外像素的总数（= panelApplyOverlay 的期望返回值）。
// ★ 单独立一个纯函数是为了让用例能**先算期望、再看实际**：
//   遮罩少画一格和多画一格都看不出，但这里能精确断言。
static inline int32_t panelOutsidePixelCount(int32_t res) {
  int32_t n = 0;
  for (int32_t y = 0; y < res; ++y) {
    for (int32_t x = 0; x < res; ++x) {
      if (!panelPixelVisible(x, y, res)) ++n;
    }
  }
  return n;
}
