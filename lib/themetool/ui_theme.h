#pragma once
#include <lvgl.h>
#include <stdint.h>

// ============================================================
// 主题 —— 换皮只改这个文件（编译期默认值）
//
// 设计定案:无指针;外圈=圆弧进度条(车速/转速/水温);中间=角色表情
//
// ★ 运行时主题（2026-09-15 新增）
//   原来所有值都是 `static const`,改一个颜色就得重编译重刷。现在改成:
//     1. ui_theme.h 只提供**默认值**（Theme 结构体 + kDefaultTheme）
//     2. 启动时 theme_load() 尝试从 flash 的 theme 分区读主题文件
//        （见 theme_store.h）。读不到就用默认值 —— 不刷主题也能跑
//     3. 代码里仍写 THEME_BG_COLOR 这样的名字不变,它们是**转发宏**,
//        展开成 "从全局主题里取这个字段"
//   这样改配色/几何只需重新生成主题文件并刷 theme 分区,**固件不用重编译**。
//
// 编辑工具:tools/theme-editor/（本地网页,可视化改配色与几何、实时预览、导出主题文件）
//
// 注意:THEME_DISPLAY_RES 是唯一保留为**编译期常量**的项 —— 它决定显示缓冲
//      尺寸,必须编译期确定（换屏才改它）。
// ============================================================

// ---- 分辨率（编译期） ----
// 所有尺寸/位置按 480×480 基准设计;渲染时统一乘 theme_scale()。
// 桩驱动直接读这个常量注册显示宽高,两处永远一致。
// 注意:480×480 双屏真驱动必须 ESP32-S3 + PSRAM(见 ARCHITECTURE.md)。
#define THEME_BASE_RES     480
#ifndef THEME_DISPLAY_RES
#define THEME_DISPLAY_RES  480
#endif
static inline float theme_scale() {
  return (float)THEME_DISPLAY_RES / (float)THEME_BASE_RES;
}

// ---- 圆弧定义 ----
// ★ 枚举值就是主题 JSON 里 arcs[].kind 的数字,**只能往后加、不能插队**
//   (插队会让已经导出的 theme.json 里那几条弧换意思,而且不报错)。
//   0/1/2 是历史值,3 = 进气温度(2026-09 新增)。
enum class ArcKind : uint8_t { Speed, Rpm, Coolant, Intake };
static const uint8_t kMaxArcs = 3;   // 每屏最多几条(不是总共)
// 屏数 = `Theme::screens[]` 的长度 = UI 里"每屏一份"那些数组的长度。
// ★★ 为什么要有这个名字（2026-10-16）：解析器（`theme_store.cpp` 的 screens 数组）
//   与 UI（`src/dash_ui.cpp` 的 `g_ui[]`/`g_screens[]`/… 以及十几处逐屏循环）原先
//   各自写死字面量 **2**。两边一旦不同步就是**静默越界**（写这一屏、读那一屏，
//   不报错、只是画面串了）—— 与 `arc_count` 那类"魔法数当数组上界"的 bug 同型。
//   ⇒ 现在两处用同一个名字；`dash_ui.cpp` 里另有一条 static_assert 把它与
//     `dashlayout::kScreenCount`（角色映射那一层，本文件不能依赖它）钉在一起。
static const uint8_t kScreenCount = 2u;

// 一条弧的样式。
//
// ★ `reverse` —— 涨幅从**哪一端**开始涨(用户说的"镜像"):
//     0(默认):start 端固定,值从 start 往 end 涨。外圈转速弧就是这样:
//              起点 7:30,顺时针经过左、上、右,涨到 4:30。
//     1:end 端固定,值从 end 往回涨 —— 几何上等于把这条弧**水平镜像**一遍。
//   为什么需要它:水温弧是"下方半圆"(0°→180°,开口朝上),而 LVGL 的弧只能从
//   start 顺时针画到 end,所以只会从**右边**(3 点钟)开始亮。
//   水温表按惯例该从**左边**开始涨,所以给它 reverse=1:
//   点亮区从 180°(9 点钟)往 0°(3 点钟)长,读起来就是"从左往右涨"。
struct ArcStyle {
  ArcKind  kind;
  int32_t  start_deg;  // 0°=3点钟,顺时针;见 README 的角度约定
  int32_t  end_deg;
  int32_t  radius;     // 弧的**外沿**半径(width 往里长;480 基准,渲染时乘 theme_scale())
  int32_t  width;      // 弧线宽(480 基准)
  lv_color_t track_color;  // 未点亮轨道
  uint8_t  track_opa;      // 0..255
  lv_color_t value_color;  // 已点亮部分
  uint8_t  reverse;        // 1 = 从 end 端起涨(镜像);见上面的说明
};

struct ScreenTheme {
  ArcStyle arcs[kMaxArcs];
  uint8_t  arc_count;
  uint8_t  show_face;
};

// ============================================================
// 数字读数(转速/速度/水温)的样式与位置
//
// 为什么单独一个结构体而不是塞进 ScreenTheme:
//   两个表的读数**布局一样**(都在正上方),只是数据源和单位不同;
//   水温只有转速表有。放全局一份比每屏复制一遍更不容易写歪。
//
// 位置都是 **480 基准**坐标,乘 theme_scale() 后使用;圆心在 (240,240)。
// 字号也随分辨率走,但不是乘小数而是查表(见文件末尾 kReadoutFontTier):
// 所以下面的竖直排布在**每个分辨率上**是同一套比例,墨迹实测值按比例缩。
// 竖直排布(480 基准,按 `radius` = **外沿**的语义算 —— 见 ArcStyle):
//   外弧带(radius 205 / width 24):外沿 y=35、**内沿 y=59**(在数字宽度两端更低)
//   数字中心   y = digit_cy        ← 默认 88,48 号墨迹实测 y[71..104]
//   单位中心   y = unit_cy         ← 默认 110,18 号墨迹实测 y[107..120]
//   表情顶边   y=120  ← 再往下会被表情压到
//
// ★★ 副表读数(水温/进气,18 号)在**下半圆那一侧**,它的可用带子比上面窄得多 ——
//   2026-09-27 深夜车主实屏报了"字不见了"(第二次),量完才看清 384 那版被夹住了:
//     · **表情图的不透明下沿**:300×300 的表情图居中 ⇒ 屏幕 y∈[90,389],下巴到 **389**
//       (逐行扫 alpha 平面量出来的,不是推的);而 18 号墨迹是 cy-3..cy+10
//       ⇒ 384 那版占 **381..394** ⇒ **上面 9 行正压在下巴上**;
//     · **灯条**原来占 y=395..435 —— ★ 2026-10-15 车主把整条六格指示灯栏删了
//       （`lamp_view.h` 与 `build_lamps` 一起删）⇒ 那一段现在是**空的**；
//       下面的下界仍然按 436 定，因为那是"副弧之下、表情下巴之下"量出来的，
//       与灯条在不在无关（少一个占用者只会更宽松，不会更紧）。
//     · 再往下是**下半圆的副弧**(radius 168 / width 10 ⇒ 底部 y=398..408)。
//   ⇒ 真正空着的是"副弧之下"。
//   ★★ 副表字号 2026-09-27 从"跟单位共用 18 号"里**分出来独立一档、默认 24 号**
//     （车主："字也太小了，能不能加大几号"）。字号变大 ⇒ 墨迹也变高，**实测**
//     （pcpreview 落帧逐像素量，不是估的）：18 号 = cy-3..cy+10（13 行）；
//     **24 号 = cy-8..cy+8（17 行）** ⇒ 同样 cy=440 会让 24 号字顶上 4 行压到原来的灯条下沿 435。
//   ⇒ 默认取 **444**：24 号墨迹 = **436..452**，三样（下巴 389 / 副弧 408 / 原灯条 435）
//     全躲开，且 x 方向 ±30 处仍在 240 内切圆内（许可到 478）。
//   ★ 下界是硬的、量过：**18 号字** 436 ⇒ 还压 183px、438 ⇒ 61px、440 起 0；
//     **24 号字的下界是 444**（墨迹顶 cy-8 必须 ≥ 436）。上限 468 由内切圆给。
//   ★ 判据钉在 `test_readout_defaults_fit_gap`(下界=436、上界=内切圆与解析夹取)。
//
// ★ 这几个数是**实测量出来的**(固件落帧读墨迹),不是从公式推的。
//   曾经这里按"带宽居中"的旧模型写着弧带 23..47 —— 那是错的(外沿语义下是 35..59)。
// ★★ 2026-09-24 第七轮把**数字从 72 下移到 88**：旧位置墨迹顶 55 比弧带内沿 59
//   还高 4 像素(数字轻轻啃住弧)，而"啃掉的那一段"在落帧上是数得出来的
//   （197~358 像素，见 tools/theme-editor/check-readout-clearance.js）。
//   新位置墨迹顶 71，比弧带内沿在**同样 x** 上的高度还低 ⇒ 数字完全离开弧带。
//   ★ 下移的上限由**单位**卡住：单位墨迹下沿(cy+10)不许超过表情图名义顶边 120
//     ⇒ unit_cy ≤ 110；而数字墨迹下沿(cy+16)又必须在单位墨迹上沿(cy-3)之上
//     ⇒ unit_cy > digit_cy + 19。两个不等式一起解出 (88, 110) 这一组。
//   test_readout_defaults_fit_gap 按这个口径钉着(允许 ≤6px 蹭边、绝不许压表情)。
// ★ 不要为了"更靠上"把 digit_cy 往前挪 —— 再往上就是明显的"数字骑在弧上"。
// ============================================================
// 读数底板不透明度的**默认值**。
//
// ★ 这个数**只在这里定义一次**：`theme_set_defaults()` 拿它当缺省（R3 之后它是
//   主题键 `readout.plate_opa`），`src/dash_ui.cpp` 只读主题宏 `READOUT_PLATE_OPA`，
//   不再自带一份 —— 两处各写一个数迟早分叉（症状：主题里没写这个键时，真机底板
//   与"默认值"对不上，而且没人看得出来）。
// ★ 255 × 15 / 100 = **38**：LVGL 的透明度按 255 计；15% 是车主三轮刷机
//   （50% → 30% → 15%）定下来的那一档，与 R3 之前 `dash_ui.cpp` 里的
//   `kReadoutPlateOpa` **逐位相同**。
static const uint8_t kReadoutPlateOpaDefault = (uint8_t)(255 * 15 / 100);   // 38
static_assert(kReadoutPlateOpaDefault == 38,
              "底片默认不透明度必须是 38/255：它就是车主在实屏上定的那一档(15%)，"
              "改它等于改所有旧主题在真机上的样子");

struct ReadoutTheme {
  // ---- ★★ 主题格式修订 R2（2026-10-15，车主："转速/速度现在就拆开"）----
  //   大数字颜色从"一个共用"拆成**三个字段**（语义见下），字段顺序=序列化顺序这条
  //   规矩照旧（本结构体的顺序与 theme_store.cpp 的解析顺序一一对应）：
  //     · digit_color —— **回落值**：转速/速度各自没有自己的键时用它。
  //       老主题文件里只有这一个键 ⇒ 解析器把 rpm_color / speed_color 都填成它
  //       （见 theme_store.cpp 的 readout 段），两屏颜色与拆分前**逐像素一致**。
  //     · rpm_color   —— 转速屏（左屏）大数字。
  //     · speed_color —— 速度屏（右屏）大数字。
  //   ★ 哪一屏取哪一个由**这一屏的角色**决定（不是屏号）：渲染侧见
  //     src/dash_ui.cpp 的 digit_base_color() —— 两块板跑同一份主题，
  //     各自按自己的表（左=转速、右=速度）取自己的颜色。
  //   ★ 兼容性（两条都由上面这套"缺键回落 + 不认识的键跳过"保证）：
  //       旧主题（无这两个键）→ 回落 digit_color ✓
  //       旧固件（不认识这两个键）→ 跳过，什么都不发生 ✓
  uint32_t digit_color;     // 大数字颜色(回落值;也是"主读数既非转速也非速度"时的中立色)
  uint32_t rpm_color;       // 转速屏(左屏)大数字颜色(缺键 → digit_color)
  uint32_t speed_color;     // 速度屏(右屏)大数字颜色(缺键 → digit_color)
  // ---- ★★ 主题格式修订 R4（2026-10-15，车主："rpm / km/h 的字体颜色是不是漏掉了，
  //      分别和转速/速度两个字体颜色绑定好了"）----
  //   **主读数单位**（大数字下面那行 "rpm" / "km/h"）自 R4 起**跟随该屏的数字颜色**：
  //     左屏（转速屏）单位 = rpm_color；右屏（速度屏）单位 = speed_color。
  //   渲染侧只有一处判据：src/dash_ui.cpp 的 build_readout() —— 它与大数字**共用**
  //     digit_base_color(pk) 同一个值，而且**仍然只在建屏时设一次样式**（铁律 ①：
  //     逐帧碰样式 = 整对象标脏 ⇒ 重绘 ×5 ⇒ 持续横纹）。
  //   ⇒ 车主报的那件事就消失了："数字改成自己的颜色了，单位还挂在旧灰上"。
  //   ★★ `unit_color` **保留**（字段、默认值 0x9AA0A6、theme_store.cpp 的解析、
  //      转发宏 READOUT_UNIT_COLOR 一个都不动）：
  //        · 它是**给人手改**的主题文件里既有的键 —— 删掉或改语义会让已导出的
  //          主题文件"静默换意思"（本仓库最忌这一类）；
  //        · R4 起**主读数单位不再读它**，它现在没有任何渲染消费者（此前只有那一处）。
  //      ⇒ 兼容性：老主题里的 unit_color 照旧被解析、照旧留在文件里，只是画面上不再生效；
  //        想让单位换颜色，改的是该屏的 rpm_color / speed_color。
  uint32_t unit_color;      // 单位文字颜色 —— R4 起**主读数单位不再用它**（键保留）
  uint32_t coolant_color;   // 水温数字颜色(左屏副表)
  uint32_t intake_color;    // 进气温度数字颜色(右屏副表)

  // ---- ★★ 主题格式修订 R3（2026-10-15，车主："底板深浅要能在编辑器里调"）----
  //   读数底片（大数字/单位/副读数下面那块半透明近黑圆角垫片 —— 见 src/dash_ui.cpp
  //   的 build_readout_plates）的**不透明度**。
  //   ★ 量纲 = **0..255**：与主题里**既有**的 opa 键同一个域 —— `ArcStyle::track_opa`
  //     （解析处 `(uint8_t)u`、渲染处直接喂 LVGL 的 `lv_opa_t`、预览里 `/255`）和
  //     本文件的 `track_opa` 默认 153 都是这一套。**别在这里再发明 0..100 或 0..1。**
  //     （编辑器车主模式把它**显示**成百分比，那只是显示，存进文件的仍是 0..255。）
  //   ★ 默认 **38 = 15%**：与拆出这个键之前那个编译期常量**逐位相同**
  //     （原来是 `kReadoutPlateOpa = 255*15/100 = 38`）⇒ 主题里不写这个键时，
  //     屏上底板与今天一模一样（旧主题照旧）。
  //   ★ 0 = 底板完全透明（相当于关掉底板）；255 = 全不透明（会压住表情，不推荐）。
  uint8_t  plate_opa;       // 读数底板不透明度 0..255(缺键 → 38 = 15%)

  // ★ 字号存的是**档位编号**(0=大数字档、1=单位/副表档),不是点数 ——
  //   点数由 THEME_DISPLAY_RES 查 kReadoutFontTier 决定(见文件末尾那张表)。
  //   所以同一份 theme.json 在 240 与 480 上都成立:480 上 48/18 号,
  //   240 上 24/10 号。**不要把点数写进主题文件** —— 那样换屏就废。
  uint8_t  digit_font;      // 0 = 大数字档
  uint8_t  unit_font;       // 1 = 单位档(km/h / rpm)
  uint8_t  sub_font;        // 2 = 副表档(水温/进气读数;2026-09-27 从"跟单位共用"分出来)

  int16_t  digit_cy;        // 大数字中心 y(480 基准;两屏共用)
  int16_t  unit_cy;         // 单位中心 y
  int16_t  coolant_cy;      // 水温数字中心 y(左屏底部)
  int16_t  intake_cy;       // 进气温度数字中心 y(右屏底部 —— 与水温对称)

  uint8_t  show_units;      // 0 = 只显示数字不显示单位
  uint8_t  show_coolant;    // 0 = 不显示水温数字
  uint8_t  show_intake;     // 0 = 不显示进气温度数字
};

// 字体选择:0 = 大数字档,1 = 单位/副表档。
// 用编号而不是直接存指针,是为了让主题能存进 JSON(指针没法序列化)。
// ★ 点数由分辨率决定(见文件末尾 kReadoutFontTier),这里只收编号。
const lv_font_t* readout_font(uint8_t which);

// 存档:数字读数的转发宏(与其它主题字段同一套用法)
//
// ★ 大数字颜色有**三个**入口，用哪一个是"这一屏的角色"说了算（见 dash_ui.cpp
//   的 digit_base_color()）：
//     READOUT_RPM_COLOR   —— 转速屏（左屏）大数字
//     READOUT_SPEED_COLOR —— 速度屏（右屏）大数字
//     READOUT_DIGIT_COLOR —— **回落值**：上面两个键在主题文件里没有时解析器填进来的
//                             就是它（见 theme_store.cpp）；主读数既不是转速也不是
//                             速度时（副表当了唯一那条弧）也用它。
//   ⇒ 渲染侧**只**按角色取前两个；直译 `READOUT_DIGIT_COLOR` 会让两屏又变成同一个颜色。
//   ★★ R4（2026-10-15）：上面那句里的 `READOUT_DIGIT_COLOR` 现在**只有两种情形**会用到 ——
//     "主读数既不是转速也不是速度"（副表当了唯一那条弧）、以及"数字颜色缺键时的回落值"。
//     主读数的**单位**也改读同一个 `digit_base_color()`（左屏 rpm_color / 右屏 speed_color）。
//   ★★ `READOUT_UNIT_COLOR` **保留但不再被任何渲染代码使用**（R4 起主读数单位跟随数字色）；
//     见上面 ReadoutTheme 里 R4 那段注记 —— 谁要再拿它画东西，先想清楚"单位为什么又独立了"。
#define READOUT_DIGIT_COLOR   (g_theme.readout.digit_color)
#define READOUT_RPM_COLOR     (g_theme.readout.rpm_color)
#define READOUT_SPEED_COLOR   (g_theme.readout.speed_color)
#define READOUT_UNIT_COLOR    (g_theme.readout.unit_color)   // R4 起无渲染消费者(键保留)
#define READOUT_COOLANT_COLOR (g_theme.readout.coolant_color)
#define READOUT_INTAKE_COLOR  (g_theme.readout.intake_color)
#define READOUT_DIGIT_FONT    readout_font(g_theme.readout.digit_font)
#define READOUT_UNIT_FONT     readout_font(g_theme.readout.unit_font)
#define READOUT_SUB_FONT      readout_font(g_theme.readout.sub_font)
#define READOUT_DIGIT_CY      (g_theme.readout.digit_cy)
#define READOUT_UNIT_CY       (g_theme.readout.unit_cy)
#define READOUT_COOLANT_CY    (g_theme.readout.coolant_cy)
#define READOUT_INTAKE_CY     (g_theme.readout.intake_cy)
#define READOUT_SHOW_UNITS    (g_theme.readout.show_units)
#define READOUT_SHOW_COOLANT  (g_theme.readout.show_coolant)
#define READOUT_SHOW_INTAKE   (g_theme.readout.show_intake)
// 读数底板的不透明度（R3 起是主题键；缺键时固件默认 38 = 15%）。
// ★ 渲染侧**只在建屏时读一次**（底片是静态对象，见 src/dash_ui.cpp 的三条铁律）。
#define READOUT_PLATE_OPA     (g_theme.readout.plate_opa)

// ---- 主题数据（全部运行时可改） ----
// 字段顺序即序列化顺序,改动要在 theme_store.cpp 里同步。
//
// ★★ 2026-10-15 更正一句**从前写错的话**（原来这里写的是"并升版本号"）：
//   **本仓库没有"主题格式版本号"这个字段**，也没有任何地方读它 —— 主题分区里
//   就是一段**裸 JSON 文本**（见 theme_store.h：无包头、无魔数，上限 4KB）。
//   全库真正存在的版本号只有三处，都不是主题格式：
//     · `kImageBlobVersion`（image_blob.h）/ `IMAGE_VERSION`（asset-package-core.js）
//       —— **image.bin 的二进制包头**版本；
//     · `.golden-hashes.json` 的 `"version"` —— **指纹证据文件**自己的 schema 版本。
//   兼容性靠的是**按键名寻址**这一套，与版本号无关：
//     · 缺键     → 继承默认值（theme_parse_json 先 theme_set_defaults 打底）；
//     · 不认识的键 → 跳过它的值（扫描器的 skipValue），所以旧固件遇到新文件不会炸；
//     · 加新键   → 只要**缺键时有明确回落**，旧主题文件照旧能用。
//   ⇒ 加字段的正确做法是：在**本文件**（字段 + 默认值 + 转发宏）、theme_store.cpp
//     （解析 + 缺键回落）、ui_theme.cpp（钳制，如需要）、src/dash_ui.cpp（取用）
//     四处同步，并写一条**格式修订注记**（见下面 readout 的"修订 R2"）。
struct Theme {
  // 全局 / 表情
  uint32_t bg_color;        // 屏幕底色
  int32_t  face_size;       // 表情区域边长(480 基准)
  uint32_t face_bg_idle;    // 表情底色（常态）
  uint32_t face_bg_redline; // 表情底色（红区）
  uint32_t face_ink;        // 五官颜色

  // 五官几何(480 基准)
  uint8_t eye_l_x, eye_r_x, eye_y;
  uint8_t eye_normal_w, eye_normal_h;
  // ★ eye_surprise 这个**键名是历史遗留**:第 4 槽位当年叫"惊喜",现在是
  //   "超速"(>130 km/h),外形仍是"大圆眼 + O 形嘴"的报警样子。
  //   键名不改 —— theme.json 是用户手里的文件,改键名会让已导出的主题
  //   静默丢掉这个字段(theme_store 对不认识的键是"跳过"而不是报错)。
  uint8_t eye_surprise;     // 第 4 槽位(超速):大圆眼
  uint8_t eye_narrow_h;     // 巡航/运动/红区:眯眼
  uint8_t mouth_line_x, mouth_line_y, mouth_line_w, mouth_line_h;
  uint8_t mouth_o_x, mouth_o_y, mouth_o_size;

  // 量程
  //   coolant:正常水温 85~105,表盘只画"有意义的窗口"
  //   intake :进气温度从环境温度(冷启动)一路被机舱烤到 60~70(堵车热浸),
  //            所以窗口取 0~80 —— 常温 20~30 落在 1/4~3/8,热浸顶到 3/4 以上,
  //            一眼能看出"进气被烤热了没有"。冬天零下时弧趴在 0,
  //            数字照实显示负数(与水温低于 60 时同理)。
  float coolant_min_c, coolant_max_c;
  float intake_min_c, intake_max_c;

  // ------------------------------------------------------------
  // 数字读数(转速/速度数字 + 单位 + 副表数字)
  //
  // 布局按实车(法系:左=转速表,右=速度表)与使用习惯定死:
  //   · 转速/速度的**大数字**放表盘正上方(y = 弧带最高点 23 到表情顶边 120 之间)
  //   · 单位(km/h、rpm)紧跟数字下方
  //   · **副表数字放该屏底部**:左屏 = 水温,右屏 = 进气温度(左右对称)
  //   中央 240×240 留给表情图。
  //
  // 位置按 **480 基准**书写,渲染时乘 theme_scale()(与弧/表情同一套规矩)。
  // 这些值可调,但要注意上方可用高度只有约 97 像素 —— 字号加大就会压到弧或表情。
  // ------------------------------------------------------------
  ReadoutTheme readout;

  // 开机动画(ms):淡入 → 双屏错峰扫表 → 表情出现
  uint32_t boot_fade_ms;
  uint32_t boot_sweep_start_ms;
  uint32_t boot_stagger_ms;
  uint32_t boot_sweep_rise_ms;
  uint32_t boot_sweep_hold_ms;
  uint32_t boot_sweep_fall_ms;
  uint32_t boot_face_start_ms;
  // ★ 字段名保留了历史(JSON 键不能随便改,老主题文件要能继续读),
  //   但含义已变:眨眼状态删掉后,它是"表情出现后的一拍收尾"。
  //   总时长 = boot_face_start_ms + 这一拍(见 theme_clamp)。
  uint32_t boot_face_blink_ms;
  uint32_t boot_total_ms;      // 由上面几个推导,加载时重算

  // 两屏的弧
  ScreenTheme screens[kScreenCount];
};

// 正常渲染弧值缓动:指数趋近速率(1/s),与渲染频率解耦。
// 换算自旧实现 kArcSmooth=0.25/每 200ms 档:0.75^5≈0.237/s → 速率 -ln(0.237)≈1.44
static const float kArcSmoothPerSec = 1.44f;

// 编译期默认主题（与 2026-09 的原始值完全一致）
inline void theme_set_defaults(Theme& t) {
  t.bg_color        = 0x141414;
  t.face_size       = 200;
  t.face_bg_idle    = 0xFFFFFF;
  t.face_bg_redline = 0xFF4D4D;
  t.face_ink        = 0x1A1A1A;

  t.eye_l_x = 38;  t.eye_r_x = 136; t.eye_y = 66;
  t.eye_normal_w = 26; t.eye_normal_h = 26;
  t.eye_surprise = 44; t.eye_narrow_h = 8;
  t.mouth_line_x = 75; t.mouth_line_y = 138; t.mouth_line_w = 50; t.mouth_line_h = 6;
  t.mouth_o_x = 80;    t.mouth_o_y = 120;    t.mouth_o_size = 40;

  t.coolant_min_c = 60.0f;
  t.coolant_max_c = 130.0f;
  t.intake_min_c  = 0.0f;
  t.intake_max_c  = 80.0f;

  // 数字读数。位置见 ReadoutTheme 的说明:干净可用的是 59..120 那 61 像素
  // （弧带内沿 59 → 表情图名义顶边 120）。
  t.readout.digit_color   = 0xFFFFFF;   // 白
  // ★★ 主题格式修订 R2（2026-10-15）：转速/速度**各自**一个大数字颜色。
  //   默认值 = `digit_color` 的默认值（直接取它，不写第二遍白）⇒
  //   主题文件里**不写**这两个键时，两屏大数字的颜色与拆分前**逐像素一致**。
  t.readout.rpm_color     = t.readout.digit_color;   // 转速屏(左屏)
  t.readout.speed_color   = t.readout.digit_color;   // 速度屏(右屏)
  t.readout.unit_color    = 0x9AA0A6;   // 灰 —— R4 起主读数单位不再用它(键与默认值保留)
  t.readout.coolant_color = 0x7CFF6B;   // 与水温弧同色,一眼能对上
  t.readout.intake_color  = 0xFFB020;   // 与进气弧同色(琥珀 —— 速度表主色是蓝,不撞)
  // ★ 主题格式修订 R3（2026-10-15）：底板深浅成了主题键 `plate_opa`（0..255）。
  //   默认值取 kReadoutPlateOpaDefault（38 = 15%）⇒ 不写这个键时与 R3 之前
  //   逐位一致（旧主题在真机上照旧）。
  t.readout.plate_opa     = kReadoutPlateOpaDefault;
  t.readout.digit_font    = 0;          // 48 号
  t.readout.unit_font     = 1;          // 18 号
  t.readout.sub_font      = 2;          // 24 号(副表读数:车主 2026-09-27 说"字太小")
  // ★★ 2026-09-24 第七轮：**数字下移 16px、单位下移 3px**（原 72 / 107）。
  //   起因：车主报"外弧被数字挡住"。落帧实测（pcpreview 2.8C 档，见
  //   tools/theme-editor/check-readout-clearance.js）：48 号墨迹顶从 y=55 起，
  //   而主弧带**内沿**在圆心正上方是 y=59、在数字宽度两端更低 ⇒ 旧位置有
  //   197~358 个数字像素**落在弧带上**（把弧啃掉一小段）。新位置墨迹 y[71..104]，
  //   弧带内沿在同样的 x 上是 y≤70 ⇒ **0 像素**（同一脚本前后值钉着）。
  //   ★ 单位只能下移 3px：它的墨迹下沿必须 ≤ 表情图名义顶边 120（墨迹 = cy+10）
  //     ⇒ unit_cy ≤ 110，而它同时必须 **> digit_cy+19**（数字墨迹下沿 cy+16
  //     要在单位墨迹上沿 cy-3 之上）⇒ 110 是唯一同时满足两头的值。
  //   ★ 弧带内沿那 6px 的蹭边余量在新位置**没有用掉**：墨迹顶 71 > 内沿 70 ⇒ 不再蹭弧。
  t.readout.digit_cy      = 88;         // 48 号墨迹实测 → 占 71..104(整条都在弧带内沿之下)
  t.readout.unit_cy       = 110;        // 18 号墨迹实测 → 占 107..120(下沿正好到表情图顶边)
  t.readout.coolant_cy    = 444;        // ★ 24 号字墨迹 436..452:副弧/灯条/下巴**全躲开**
  t.readout.intake_cy     = 444;        // 同上 —— 与水温**在各自屏上**同一行
  t.readout.show_units    = 1;
  t.readout.show_coolant  = 1;
  t.readout.show_intake   = 1;

  t.boot_fade_ms        = 250;
  t.boot_sweep_start_ms = 250;
  t.boot_stagger_ms     = 100;
  t.boot_sweep_rise_ms  = 380;
  t.boot_sweep_hold_ms  = 140;
  t.boot_sweep_fall_ms  = 420;
  t.boot_face_start_ms  = 1000;
  t.boot_face_blink_ms  = 130;   // 表情出现后的一拍收尾(名字是历史遗留,见字段注释)
  t.boot_total_ms       = t.boot_face_start_ms + t.boot_face_blink_ms;

  // ★ 屏幕布局按**法系车**来:左 = 转速表,右 = 速度表。
  //   (标致 206 实车就是这样,别按"左车速右转速"的日德习惯改回去。)
  //   水温表在**转速表**上,所以水温弧属于左屏。

  // 左屏 = 转速表:转速弧 + 水温弧(水温在表盘下方)
  //
  // ★ 两条弧**开口方向刻意相反**(用户要求:同向看着怪):
  //     转速弧(外):135 → 405,**缺口在正下方**,拱在表盘上方
  //     水温弧(内):  0 → 180,**缺口在正上方**,兜在表盘下方
  //   于是"外圈拱上面、内圈兜下面",一眼能分清哪条是哪条。
  //
  //   水温弧为什么是 0→180(而不是更短的弧):LVGL 的弧只能从 start 顺时针画到
  //   end,而"缺口在正上方"= 覆盖区必须落在下半圆,唯一干净的写法就是
  //   [0°, 180°](3 点钟 → 6 点钟 → 9 点钟)。代价是它的**填充方向是从右往左**
  //   (右边先亮、绕到左边),不像转速表那样从左往右。屏幕上有数字读数,
  //   所以不靠弧的左右去区分冷热;真要 C 在左 H 在右,得把弧放到上半圆
  //   (缺口朝下),那样又和转速表同向了 —— 这两个只能选一个。
  ScreenTheme& L = t.screens[0];
  L.arc_count = 2; L.show_face = 1;
  L.arcs[0] = ArcStyle{ ArcKind::Rpm,     135, 405, 205, 24,
                        lv_color_hex(0x232323), 153, lv_color_hex(0xFF5C5C), 0 };
  // reverse=1:水温弧从**左端(9 点钟)**起涨 —— 与转速弧形成镜像关系
  L.arcs[1] = ArcStyle{ ArcKind::Coolant,   0, 180, 168, 10,
                        lv_color_hex(0x232323), 153, lv_color_hex(0x7CFF6B), 1 };

  // 右屏 = 速度表:车速弧 + 进气温度弧
  //
  // ★ 进气温度弧(2026-09 用户实测 OBD 010F 可用后加的)按"左右对称"设计:
  //   它和水温弧用**完全相同的几何**(0→180、radius 168、width 10、reverse=1),
  //   只是挂到另一块表上 —— 于是两块表看起来是同一套仪表的两个实例:
  //     左(转速表):外圈转速弧(拱上) + 内圈水温弧(兜下)
  //     右(速度表):外圈车速弧(拱上) + 内圈进气温度弧(兜下)
  //   颜色用琥珀而不是绿色:速度表主色是蓝,绿色留给水温,
  //   三种颜色分属三条弧,扫一眼就知道哪条是哪条。
  // ★ 顺序很重要:车速弧必须是 arcs[0] —— 大数字显示"第一条非副表的弧",
  //   把副表排前面会让速度表的大数字变成进气温度(dash_ui 的 primary_kind)。
  ScreenTheme& R = t.screens[1];
  R.arc_count = 2; R.show_face = 1;
  R.arcs[0] = ArcStyle{ ArcKind::Speed,   135, 405, 205, 24,
                        lv_color_hex(0x232323), 153, lv_color_hex(0x39C5FF), 0 };
  R.arcs[1] = ArcStyle{ ArcKind::Intake,    0, 180, 168, 10,
                        lv_color_hex(0x232323), 153, lv_color_hex(0xFFB020), 1 };
}

// 全局主题**指针**。
//
// 为什么是指针而不是可写实例（重要，别改回实例）：
//   主题在运行期是**只读**的 —— LVGL 只把里面的颜色/尺寸读出去用，
//   不需要一份可写的 DRAM 副本。所以:
//     · 默认主题是 const,放 flash(.rodata)
//     · 从 flash 主题分区加载成功后,把指针指到解析结果
//   实测(esp32dev,320KB DRAM):把主题做成**可写全局实例**会让 LVGL 多占
//   约 36KB —— 颜色不再是编译期常量后 LVGL 无法把样式对象折叠掉。
//   改回实例会直接顶爆 dram0_0_seg(D24A 实测溢出 8.8KB)。
//
// 取值方式:代码里照旧写 THEME_BG_COLOR 这些名字,它们是转发宏(见文件末尾)。
extern const Theme* g_theme_ptr;

// 默认主题(flash 里的常量),theme_load() 失败时指针指向它
const Theme& theme_defaults();
// 加载缓冲:theme_store 解析主题文件时直接解到这里
void theme_loaded_slot(Theme** out);
// 解析成功后把生效指针切到加载缓冲
void theme_use_loaded();

#define g_theme (*g_theme_ptr)

// 启动时确保主题可用。
// ★ 只在"当前没有可用主题"时落到默认值 —— **不会**覆盖已加载的主题。
//   调用方典型顺序:theme_load() 然后 dash_ui_init()(内部调本函数)。
//   早期版本在这里无条件重置,导致刚加载的主题被丢掉
//   (日志说"已加载",画面却是默认配色)。
void theme_reset_to_defaults();
// 无条件回到默认主题(显式"恢复默认"用)
void theme_force_defaults();
// 值域兜底 + 派生字段重算。两条路径共用（见 ui_theme.cpp）。
void theme_clamp(Theme& t);

// ★★ 2026-10-15：原来这里还有两个**灯条专用**的编译期颜色
//   （`THEME_LAMP_COLOR` 琥珀 / `THEME_LAMP_ALERT_COLOR` 告警描边）。
//   车主当天下令把整条六格指示灯栏从 UI 上删掉（原话："车灯车门原车仪表显示更好"）
//   ⇒ 那两个常量随 `lib/dashcore/lamp_view.h` 与 `dash_ui.cpp` 的 build_lamps
//   那一段一起删了。**红区那条告警色没有消失**：它换了身份继续用在
//   "转速弧进红区那一段染红"上 —— 见下面 `THEME_REDLINE_ARC_COLOR`。

// 转速弧**进红区那一段**的颜色 + 加粗量（2026-10-15 新增）。
// ★ 为什么用**编译期常量**而不是主题键：与原来那两个灯条颜色同一条理由 ——
//   `Theme` 结构体加一个字段就是一次**主题格式修订**：多一个键，别人手里已导出的
//   主题文件里没有它（照旧能读，靠缺键继承默认），但"这个值该不该由主题说了算"
//   是本条要回答的问题；
//   而"红区要一眼看得见"属于**安全提示**，不该是可以被主题改没的东西。
//   （★ 2026-10-15 更正：这里从前写的是"加字段要升版本号、会让已导出的主题全部需要
//     重导" —— 本仓库**没有**版本号字段，"需要重导"这个后果也不成立，见上面
//     Theme 结构体前的说明。结论不变：安全/可读性颜色不进主题。）
//   ⇒ 颜色取自既有的红区语义（`FACE_BG_REDLINE` 那一族），加粗量取弧宽的 1/4。
static const uint32_t THEME_REDLINE_ARC_COLOR = 0xFF4D4D;  // 与红区告警同色

// ---------------- 转发宏 ----------------
// 代码里仍写原来的名字,但值来自运行时主题。dash_ui_init() 之前
// 不要用这些宏（那时主题还没 load）。
#define THEME_BG_COLOR      (lv_color_hex(g_theme.bg_color))
#define THEME_FACE_SIZE     (g_theme.face_size)
#define FACE_BG_IDLE        (lv_color_hex(g_theme.face_bg_idle))
#define FACE_BG_REDLINE     (lv_color_hex(g_theme.face_bg_redline))
#define FACE_INK            (lv_color_hex(g_theme.face_ink))
#define FACE_EYE_L_X        (g_theme.eye_l_x)
#define FACE_EYE_R_X        (g_theme.eye_r_x)
#define FACE_EYE_Y          (g_theme.eye_y)
#define EYE_NORMAL_W        (g_theme.eye_normal_w)
#define EYE_NORMAL_H        (g_theme.eye_normal_h)
#define EYE_SURPRISE        (g_theme.eye_surprise)
#define EYE_NARROW_H        (g_theme.eye_narrow_h)
#define MOUTH_LINE_X        (g_theme.mouth_line_x)
#define MOUTH_LINE_Y        (g_theme.mouth_line_y)
#define MOUTH_LINE_W        (g_theme.mouth_line_w)
#define MOUTH_LINE_H        (g_theme.mouth_line_h)
#define MOUTH_O_X           (g_theme.mouth_o_x)
#define MOUTH_O_Y           (g_theme.mouth_o_y)
#define MOUTH_O_SIZE        (g_theme.mouth_o_size)
#define kCoolantMinC        (g_theme.coolant_min_c)
#define kCoolantMaxC        (g_theme.coolant_max_c)
#define kIntakeMinC         (g_theme.intake_min_c)
#define kIntakeMaxC         (g_theme.intake_max_c)
#define kScreens            (g_theme.screens)
#define BOOT_FADE_MS        (g_theme.boot_fade_ms)
#define BOOT_SWEEP_START_MS (g_theme.boot_sweep_start_ms)
#define BOOT_STAGGER_MS     (g_theme.boot_stagger_ms)
#define BOOT_SWEEP_RISE_MS  (g_theme.boot_sweep_rise_ms)
#define BOOT_SWEEP_HOLD_MS  (g_theme.boot_sweep_hold_ms)
#define BOOT_SWEEP_FALL_MS  (g_theme.boot_sweep_fall_ms)
#define BOOT_FACE_START_MS  (g_theme.boot_face_start_ms)
#define BOOT_FACE_BLINK_MS  (g_theme.boot_face_blink_ms)
#define BOOT_TOTAL_MS       (g_theme.boot_total_ms)

// ============================================================
// 字号随分辨率(2026-09-20,240×240 那块板点出来的)
//
// 症状:几何(弧/表情/读数位置)早就跟着 THEME_DISPLAY_RES 缩了,但**字没有** ——
//   240 屏上 48 号数字横着 240 像素除以字宽 = 5 个字就占满整屏,读数是糊的。
//
// 规矩(与弧/表情同一条,只是字体没法乘小数):
//   · 主题里存的仍是**档位**(digit_font / unit_font),档位不变;
//   · 点数 = 该档在 480 基准的点数 × theme_scale(),再**向下取到 LVGL 现成的字号**。
//     48 × 0.5 = 24、18 × 0.5 = 9 → 取 10(往大取一档,9 号不在 LVGL 的表里)。
//   · 表里挑的是"这一档在这个屏上该多大"的点数,不是分辨率分支 ——
//     以后加 360 或 800 的屏,只在这张表后面加一行,别去 dash_ui 里写 if。
//
// 为什么不用 LVGL 的字体缩放:lv_font 的缩放版要额外开 LV_FONT_FMT_TXT 之类,
//   而且小字号缩出来是糊的;直接用现成点阵更清楚,代价只是多两份字体数据。
//
// 实测(固件落帧量墨迹,与 test_readout_defaults_fit_gap 的 480 口径同一套):
//     480:大数字 48 号墨迹高 34(cy=72 → 55..88)、单位 18 号高 13(cy=107 → 104..117)
//     240:同一块地方只有 61×0.5 ≈ 30 像素高(外弧带内沿 59→29.5、表情顶边 120→60),
//          24 号墨迹高约 17(cy=36 → 约 28..44)、10 号高约 7(cy=53.5 → 约 50..57)
// ★ 240 那几个**还没在实屏上量过**(屏刚点亮,owner 会看):
//   量出来比预期大或小,只改上面这张表的一个数 —— 位置与几何都不用动。
// ============================================================
static const uint8_t kReadoutFontTierCount = 3;   // 0=大数字 1=单位 2=副表(水温/进气)
static const uint8_t kReadoutResTierCount  = 2;   // 0=480 基准 1=240

// [分辨率档][字号档] = 点阵点数。
//   480 那列是**原始设计值**,240 那列 = 原始值 × 0.5(向下取到现成字号)。
//   constexpr 而不是 static const:下面的 static_assert 要在编译期读它 ——
//   写成 static const 会得到 "not usable in a constant expression"。
static constexpr uint8_t kReadoutFontPx[kReadoutResTierCount][kReadoutFontTierCount] = {
  // 大数字   单位    副表(水温/进气)
  {    48,       18,      24   },   // 480 基准(THEME_BASE_RES)
  {    24,       10,      14   },   // 240×240,微雪 DualEye-Touch-LCD-1.28
};
// ★ 2026-09-27 深夜加的**第三档(副表)**:车主实屏"字太小了,加大几号"。
//   副表原来跟"单位(km/h/rpm)"共用 18 号 —— 但单位是**说明性**的,副表是**读数**,
//   两者不该同一个字号。240 那列取 14:24 × 0.5 = 12,往大取到 LVGL 现成的 14
//   (12 没编进来;include/lv_conf.h 只使能了 10/14/18/24/48)。

// constexpr:两个宏都是编译期常量,所以这张表也是 —— 数组下标是编译期算出来的。
static constexpr uint8_t readout_res_tier() {
  return (THEME_DISPLAY_RES * 2 <= THEME_BASE_RES) ? 1u : 0u;
}
static constexpr uint8_t readout_font_px(uint8_t which) {
  return kReadoutFontPx[readout_res_tier()][which < kReadoutFontTierCount ? which : 0u];
}

// 这张表只有三档字号,少一档就会读到别的档去(不报错,只是字不对) —— 钉住。
static_assert(kReadoutFontTierCount == 3,
              "字号档 0=大数字 / 1=单位 / 2=副表:加档要同步 theme_clamp 的上限判断");
static_assert(kReadoutFontPx[0][0] == 48 && kReadoutFontPx[0][1] == 18 &&
              kReadoutFontPx[0][2] == 24,
              "480 那列必须是原始设计值(48 / 18 / 24),否则老屏上的字会变大小");
static_assert(kReadoutFontPx[1][0] < kReadoutFontPx[0][0] &&
              kReadoutFontPx[1][1] < kReadoutFontPx[0][1] &&
              kReadoutFontPx[1][2] < kReadoutFontPx[0][2],
              "小屏那列必须比 480 那列小,否则这次改动等于没做");
