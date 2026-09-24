// 主题解析器测试(宿主机)
//
// 为什么值得测:主题文件是**外部生成、人会手改**的输入,而且是设备启动路径上
// 第一件事。坏数据最坏的后果不是"主题不好看",而是 boot_anim 里拿 0 当除数
// 直接崩、或者 LVGL 拿到负半径画出鬼东西 —— 所以钳制逻辑必须有测试兜住。
#include <unity.h>
#include <string.h>
#include "ui_theme.h"
#include "theme_store.h"

// 每个用例前把主题恢复成默认值,避免用例间相互影响
static void resetTheme() {
  theme_reset_to_defaults();
}

// 完整主题文件（编辑器导出的形式）必须逐字段生效
static void test_parse_full_theme(void) {
  const char* json = R"({"theme":{
    "bg_color": 0x101820,
    "face_size": 220,
    "coolant_min_c": 50, "coolant_max_c": 120,
    "boot_fade_ms": 111, "boot_sweep_rise_ms": 222,
    "face": { "eye_l_x": 11, "eye_r_x": 22, "eye_y": 33, "mouth_o_size": 44 },
    "screens": [
      { "arc_count": 2, "show_face": 1, "arcs": [
          { "kind": 0, "start_deg": 100, "end_deg": 400, "radius": 190, "width": 20,
            "track_color": 0x111111, "track_opa": 128, "value_color": 0x00FF00 },
          { "kind": 2, "start_deg": 140, "end_deg": 320, "radius": 150, "width": 8,
            "track_color": 0x222222, "track_opa": 100, "value_color": 0x0000FF } ] },
      { "arc_count": 1, "show_face": 0, "arcs": [
          { "kind": 1, "start_deg": 100, "end_deg": 400, "radius": 200, "width": 22,
            "track_color": 0x333333, "track_opa": 90, "value_color": 0xFF0000 } ] } ] }})";

  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_HEX32(0x101820, t.bg_color);
  TEST_ASSERT_EQUAL_INT32(220, t.face_size);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, t.coolant_min_c);
  TEST_ASSERT_EQUAL_FLOAT(120.0f, t.coolant_max_c);
  TEST_ASSERT_EQUAL_UINT32(111, t.boot_fade_ms);
  TEST_ASSERT_EQUAL_UINT32(222, t.boot_sweep_rise_ms);
  TEST_ASSERT_EQUAL_UINT8(11, t.eye_l_x);
  TEST_ASSERT_EQUAL_UINT8(44, t.mouth_o_size);
  TEST_ASSERT_EQUAL_UINT8(2, t.screens[0].arc_count);
  TEST_ASSERT_EQUAL_UINT8(1, t.screens[1].arc_count);
  TEST_ASSERT_EQUAL_UINT8(0, t.screens[1].show_face);   // 关了表情
  TEST_ASSERT_EQUAL_INT32(190, t.screens[0].arcs[0].radius);
  TEST_ASSERT_EQUAL_UINT8(128, t.screens[0].arcs[0].track_opa);
  // lv_color_to_u32 返回 XRGB8888(alpha 恒为 0xFF),所以只比低 24 位
  TEST_ASSERT_EQUAL_HEX32(0x00FF00,
                          lv_color_to_u32(t.screens[0].arcs[0].value_color) & 0xFFFFFFu);
  // 派生字段:总时长 = 表情开始 + 一拍收尾(眨眼状态删掉后不再是 4 拍)
  TEST_ASSERT_EQUAL_UINT32(t.boot_face_start_ms + t.boot_face_blink_ms,
                           t.boot_total_ms);
}

// 字段缺失 = 继承默认值(这是"可手写残缺主题"的实现方式)
static void test_parse_partial_keeps_defaults(void) {
  Theme def;
  theme_set_defaults(def);

  const char* json = R"({"theme":{"bg_color": 0xABCDEF}})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_HEX32(0xABCDEF, t.bg_color);
  TEST_ASSERT_EQUAL_INT32(def.face_size, t.face_size);           // 未提到的保持默认
  TEST_ASSERT_EQUAL_UINT32(def.boot_fade_ms, t.boot_fade_ms);
  TEST_ASSERT_EQUAL_UINT8(def.screens[0].arc_count, t.screens[0].arc_count);
}

// 顶层直接写主题对象也要接受(手写时更省事)
static void test_parse_bare_root(void) {
  const char* json = R"({"bg_color": 0x123456, "face_size": 210})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_HEX32(0x123456, t.bg_color);
  TEST_ASSERT_EQUAL_INT32(210, t.face_size);
}

// 未知字段要忽略而不是失败(向前兼容:以后加字段,旧固件还能读)
static void test_parse_ignores_unknown(void) {
  const char* json = R"({"theme":{
     "bg_color": 0x445566,
     "future_field": {"nested": [1,2,3], "s": "text"},
     "another": 42,
     "face_size": 200 }})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_HEX32(0x445566, t.bg_color);
  TEST_ASSERT_EQUAL_INT32(200, t.face_size);
}

// ★ 关键:坏值必须被钳制,不能让设备崩
static void test_clamp_bad_values(void) {
  // 开机时长全 0 → boot_anim 里会用它做除数,必须被抬到非零
  const char* json0 = R"({"theme":{
     "boot_fade_ms": 0, "boot_sweep_rise_ms": 0, "boot_sweep_fall_ms": 0,
     "boot_face_blink_ms": 0, "boot_face_start_ms": 0 }})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json0, (uint32_t)strlen(json0), t));
  TEST_ASSERT_TRUE(t.boot_fade_ms > 0);
  TEST_ASSERT_TRUE(t.boot_sweep_rise_ms > 0);
  TEST_ASSERT_TRUE(t.boot_sweep_fall_ms > 0);
  TEST_ASSERT_TRUE(t.boot_face_blink_ms > 0);
  TEST_ASSERT_TRUE(t.boot_face_start_ms > 0);
  TEST_ASSERT_TRUE(t.boot_total_ms > 0);

  // 量程反了 → 回默认
  const char* json1 = R"({"theme":{"coolant_min_c": 200, "coolant_max_c": 100}})";
  TEST_ASSERT_TRUE(theme_parse_json(json1, (uint32_t)strlen(json1), t));
  TEST_ASSERT_TRUE(t.coolant_max_c > t.coolant_min_c);

  // 半径/线宽越界 → 钳到合理区间（LVGL 对负半径不会报错,只会画错）
  const char* json2 = R"({"theme":{"screens":[
     {"arc_count": 9, "arcs":[
        {"radius": -50, "width": 0, "start_deg": 300, "end_deg": 100, "reverse": 7}]},
     {"arc_count": 1, "arcs":[{"radius": 99999, "width": 500}]}]}})";
  TEST_ASSERT_TRUE(theme_parse_json(json2, (uint32_t)strlen(json2), t));
  TEST_ASSERT_TRUE(t.screens[0].arc_count <= kMaxArcs);   // 弧数量上限
  TEST_ASSERT_TRUE(t.screens[0].arcs[0].radius >= 10);
  TEST_ASSERT_TRUE(t.screens[0].arcs[0].width >= 1);
  TEST_ASSERT_TRUE(t.screens[0].arcs[0].end_deg > t.screens[0].arcs[0].start_deg);
  TEST_ASSERT_TRUE(t.screens[1].arcs[0].radius <= 240);
  TEST_ASSERT_TRUE(t.screens[1].arcs[0].width <= 60);
  // reverse 是开关,一律归一到 0/1(写 7 也当 1)
  TEST_ASSERT_EQUAL_UINT8(1, t.screens[0].arcs[0].reverse);

  // face_size 越界
  const char* json3 = R"({"theme":{"face_size": 5}})";
  TEST_ASSERT_TRUE(theme_parse_json(json3, (uint32_t)strlen(json3), t));
  TEST_ASSERT_TRUE(t.face_size >= 40);
}

// 损坏输入不能崩(宁可返回失败并保留原主题)
static void test_parse_garbage_is_safe(void) {
  Theme t;
  TEST_ASSERT_FALSE(theme_parse_json(nullptr, 0, t));
  TEST_ASSERT_FALSE(theme_parse_json("", 0, t));
  TEST_ASSERT_FALSE(theme_parse_json("{", 1, t));
  TEST_ASSERT_FALSE(theme_parse_json("not json at all", 15, t));
  TEST_ASSERT_FALSE(theme_parse_json("[]", 2, t));
  // 数值被截断:不该崩,解析出的内容也必须是钳制过的
  const char* trunc = R"({"theme":{"bg_color": 0x00, "face_size": )";
  (void)theme_parse_json(trunc, (uint32_t)strlen(trunc), t);
  TEST_ASSERT_TRUE(t.face_size >= 40 || t.face_size == 200);
}

// 默认主题自己必须是合法的(钳制不该改动它)
static void test_defaults_are_valid(void) {
  Theme d;
  theme_set_defaults(d);
  Theme before = d;
  theme_clamp(d);
  TEST_ASSERT_EQUAL_INT32(before.face_size, d.face_size);
  TEST_ASSERT_EQUAL_FLOAT(before.coolant_min_c, d.coolant_min_c);
  TEST_ASSERT_EQUAL_FLOAT(before.coolant_max_c, d.coolant_max_c);
  TEST_ASSERT_EQUAL_UINT32(before.boot_total_ms, d.boot_total_ms);
  TEST_ASSERT_EQUAL_UINT32(before.boot_fade_ms, d.boot_fade_ms);
}

// ============================================================
// 屏 ↔ 表的对应关系(**这是产品契约,不是实现细节**)
//
// 法系车(标致 206 实车)的仪表布局是 **左 = 转速表,右 = 速度表**,
// 水温表在转速表上。按"左车速右转速"的日德习惯写就会左右装反 ——
// 而**装反了不会有任何报错**:两屏都能正常画,只是画的是另一个表的数据。
//
// 之前没有任何测试锁这个映射(改错了全绿),这条补上。
// ============================================================
static void test_screen_gauge_mapping(void) {
  Theme d;
  theme_set_defaults(d);

  // 左屏 = 转速表:外圈转速弧 + 内圈水温弧(水温在转速表上)
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, d.screens[0].arc_count,
                                  "左屏应有两条弧(转速 + 水温)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ArcKind::Rpm, (uint8_t)d.screens[0].arcs[0].kind,
                                  "左屏外弧必须是转速(法系车左=转速表)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ArcKind::Coolant, (uint8_t)d.screens[0].arcs[1].kind,
                                  "水温弧必须在左屏(转速表)上");

  // 右屏 = 速度表:外圈车速弧 + 内圈进气温度弧(2026-09 新增,与左屏对称)
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, d.screens[1].arc_count,
                                  "右屏应有两条弧(车速 + 进气温度)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ArcKind::Speed, (uint8_t)d.screens[1].arcs[0].kind,
                                  "右屏外弧必须是车速(法系车右=速度表)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ArcKind::Intake, (uint8_t)d.screens[1].arcs[1].kind,
                                  "进气温度弧必须在右屏(速度表)上");

  // ★ 副表几何必须与水温**完全一致**,只换屏、换颜色 ——
  //   这样两块表看起来是同一套仪表的两个实例(用户要的"对称")。
  //   颜色不同是刻意的:车速蓝 / 转速红 / 水温绿 / 进气琥珀。
  const ArcStyle& cool = d.screens[0].arcs[1];
  const ArcStyle& take = d.screens[1].arcs[1];
  TEST_ASSERT_EQUAL_INT32(cool.start_deg, take.start_deg);
  TEST_ASSERT_EQUAL_INT32(cool.end_deg, take.end_deg);
  TEST_ASSERT_EQUAL_INT32(cool.radius, take.radius);
  TEST_ASSERT_EQUAL_INT32(cool.width, take.width);
  TEST_ASSERT_EQUAL_UINT8(cool.reverse, take.reverse);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, take.reverse,
                                  "进气温度弧也要镜像(从左端起涨),与水温一致");
  TEST_ASSERT_TRUE_MESSAGE(take.value_color.red != cool.value_color.red ||
                           take.value_color.green != cool.value_color.green ||
                           take.value_color.blue != cool.value_color.blue,
                           "进气温度弧不能与水温弧同色(两条弧要能一眼分清)");
}

// 屏 ↔ 表情图片角色的对应在 test_image_blob.cpp 里(那里才有 ImageRole)。

// ============================================================
// 数字读数(转速/速度大数字 + 单位 + 水温)
//
// 为什么值得测:这些值**只影响观感,错了不会崩** —— 但恰恰是这种字段
// 最容易"解析写漏了、界面上调半天没反应"。所以逐字段对一遍。
// ============================================================
static void test_parse_readout(void) {
  const char* json = R"({"theme":{
    "readout": {
      "digit_color": 0x00FF88,
      "unit_color": 0x112233,
      "coolant_color": 0xFF00FF,
      "digit_font": 1,
      "unit_font": 0,
      "digit_cy": 50,
      "unit_cy": 90,
      "coolant_cy": 400,
      "show_units": 0,
      "show_coolant": 0
    }}})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_HEX32(0x00FF88, t.readout.digit_color);
  TEST_ASSERT_EQUAL_HEX32(0x112233, t.readout.unit_color);
  TEST_ASSERT_EQUAL_HEX32(0xFF00FF, t.readout.coolant_color);
  TEST_ASSERT_EQUAL_UINT8(1, t.readout.digit_font);
  TEST_ASSERT_EQUAL_UINT8(0, t.readout.unit_font);
  TEST_ASSERT_EQUAL_INT(50, t.readout.digit_cy);
  TEST_ASSERT_EQUAL_INT(90, t.readout.unit_cy);
  TEST_ASSERT_EQUAL_INT(400, t.readout.coolant_cy);
  TEST_ASSERT_EQUAL_UINT8(0, t.readout.show_units);
  TEST_ASSERT_EQUAL_UINT8(0, t.readout.show_coolant);
}

// 缺 readout → 整体继承默认值(老主题文件必须继续能用)
static void test_parse_readout_missing_keeps_defaults(void) {
  Theme def;
  theme_set_defaults(def);

  const char* json = R"({"theme":{"bg_color": 0x101010}})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_HEX32(def.readout.digit_color, t.readout.digit_color);
  TEST_ASSERT_EQUAL_INT(def.readout.digit_cy, t.readout.digit_cy);
  TEST_ASSERT_EQUAL_INT(def.readout.unit_cy, t.readout.unit_cy);
  TEST_ASSERT_EQUAL_INT(def.readout.coolant_cy, t.readout.coolant_cy);
  TEST_ASSERT_EQUAL_UINT8(def.readout.show_units, t.readout.show_units);
  TEST_ASSERT_EQUAL_UINT8(def.readout.show_coolant, t.readout.show_coolant);

  // 只写一半 → 写了的生效、没写的继承
  const char* half = R"({"theme":{"readout":{"digit_cy": 40}}})";
  TEST_ASSERT_TRUE(theme_parse_json(half, (uint32_t)strlen(half), t));
  TEST_ASSERT_EQUAL_INT(40, t.readout.digit_cy);
  TEST_ASSERT_EQUAL_INT(def.readout.unit_cy, t.readout.unit_cy);
}

// 越界必须被钳制:字号只认 0/1(别的值会让 readout_font 返回空指针),
// 位置钳在表盘内(挪到屏幕外就成了"读数不见了"这种查半天的怪事)。
static void test_clamp_readout(void) {
  const char* json = R"({"theme":{"readout":{
     "digit_font": 7, "unit_font": 9,
     "digit_cy": -100, "unit_cy": 9999, "coolant_cy": -5,
     "show_units": 42, "show_coolant": 7 }}})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_TRUE(t.readout.digit_font <= 1);
  TEST_ASSERT_TRUE(t.readout.unit_font <= 1);
  TEST_ASSERT_TRUE(t.readout.digit_cy >= 10 && t.readout.digit_cy <= 115);
  TEST_ASSERT_TRUE(t.readout.unit_cy >= 10 && t.readout.unit_cy <= 119);
  TEST_ASSERT_TRUE(t.readout.coolant_cy >= 200 && t.readout.coolant_cy <= 470);
  TEST_ASSERT_EQUAL_UINT8(1, t.readout.show_units);     // 开关一律归一到 0/1
  TEST_ASSERT_EQUAL_UINT8(1, t.readout.show_coolant);
}

// 默认读数的位置契约(按 `radius` = **外沿**的语义算):
//   外弧带 radius 205 / width 24 → 占 y 35..59(内沿 59)
//   表情图从 y=120 开始
//   ① 单位墨迹**绝不能**碰到表情图(≤120)
//   ② 数字墨迹不许明显骑到弧带内沿上(内沿 59,这里允许 ≤6px 的蹭边):
//      ★ 2026-09-24 第七轮**没有用掉**这 6px —— 默认值现在是
//        digit_cy=88(墨迹 71..104)、unit_cy=110(墨迹 107..120),
//        数字整条都在弧带内沿之下(落帧实测 0 像素落在弧带上,见
//        tools/theme-editor/check-readout-clearance.js)。这条断言留着,
//        是为了**不让默认值再被挪回弧上**(旧值 72 的墨迹顶 55 比内沿还高 4px)。
//   ③ 数字与单位不能挤在一起、也不能离太远
// 墨迹偏移按**实测**(固件落帧):48 号数字墨迹高 34px → [cy-17, cy+16];
// 18 号高 13px → [cy-3, cy+10]。换字号时这几个数要跟着量。
// 这条是几何契约:改默认位置时它会告诉你越界了。
static void test_readout_defaults_fit_gap(void) {
  Theme d;
  theme_set_defaults(d);
  const int32_t kArcBandInnerY = 59;    // 外弧带内沿(205-24 → 240-181=59)
  const int32_t kFaceTopY = 120;        // 表情图顶边
  const int32_t kDigitTopOff = -17;     // 48 号墨迹:上偏 17
  const int32_t kDigitBotOff = 16;      //           下偏 16
  const int32_t kUnitTopOff = -3;       // 18 号墨迹:上偏 3
  const int32_t kUnitBotOff = 10;       //           下偏 10

  const int32_t digit_top = d.readout.digit_cy + kDigitTopOff;
  const int32_t digit_bottom = d.readout.digit_cy + kDigitBotOff;
  const int32_t unit_top = d.readout.unit_cy + kUnitTopOff;
  const int32_t unit_bottom = d.readout.unit_cy + kUnitBotOff;

  TEST_ASSERT_TRUE_MESSAGE(digit_top >= kArcBandInnerY - 6,
                           "数字明显骑到弧带上了(最多只允许蹭 6 像素)");
  TEST_ASSERT_TRUE_MESSAGE(unit_bottom <= kFaceTopY, "单位会被表情图压住");
  TEST_ASSERT_TRUE_MESSAGE(digit_bottom < unit_top, "数字必须在单位上方");
  TEST_ASSERT_TRUE_MESSAGE(unit_top - digit_bottom <= 30, "数字和单位之间空太多");
  // 水温读数在表盘底部:要在圆心以下,又不能跑到屏幕外
  TEST_ASSERT_TRUE(d.readout.coolant_cy > 240);
  TEST_ASSERT_TRUE(d.readout.coolant_cy < 430);
}

// ★ 涨幅方向(镜像):水温弧必须是从左端起涨,否则它(下方半圆)只会从右边开始亮。
//   这是用户提的"位置对了但涨幅方向反了,得做一下镜像",所以钉住默认值。
static void test_coolant_arc_is_mirrored(void) {
  Theme d;
  theme_set_defaults(d);

  const ArcStyle& coolant = d.screens[0].arcs[1];
  TEST_ASSERT_EQUAL_UINT8((uint8_t)ArcKind::Coolant, (uint8_t)coolant.kind);
  TEST_ASSERT_EQUAL_INT(0, coolant.start_deg);      // 3 点钟
  TEST_ASSERT_EQUAL_INT(180, coolant.end_deg);      // 9 点钟 → 开口朝上
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, coolant.reverse,
                                  "水温弧要从 end 端(9 点钟)起涨,否则只会从右边开始亮");

  // 另外两条弧保持默认方向(从 start 端起涨)
  TEST_ASSERT_EQUAL_UINT8(0, d.screens[0].arcs[0].reverse);   // 转速
  TEST_ASSERT_EQUAL_UINT8(0, d.screens[1].arcs[0].reverse);   // 车速
}

// 缺 reverse 字段的老主题 → 默认 0(从 start 端起涨),不能是随机值
static void test_reverse_defaults_to_zero(void) {
  const char* json = R"({"theme":{"screens":[
     {"arc_count": 1, "arcs":[{"kind": 2, "start_deg": 0, "end_deg": 180}]}]}})";
  Theme t;
  TEST_ASSERT_TRUE(theme_parse_json(json, (uint32_t)strlen(json), t));
  TEST_ASSERT_EQUAL_UINT8(0, t.screens[0].arcs[0].reverse);
}

// ★ 240×240 那块板(微雪 DualEye-Touch-LCD-1.28)上的字号必须跟着分辨率缩。
//
// 症状(owner 实屏报的):几何早就乘 theme_scale() 缩了一半,但**字没有** ——
// 48 号数字在 240 宽的屏上五个字符就占满整行,读数糊成一片。
// 现在点数由 THEME_DISPLAY_RES 查表得到(kReadoutFontPx),这里钉住、
// 并用与 480 同一条几何契约复算一遍:同一条竖直带里,缩一半的字仍然放得下。
//
// 口径与 test_readout_defaults_fit_gap 完全一样(480 基准,圆心 240):
//   外弧带内沿 = 240 - (205 - 24) = 59;表情图顶边 = 120;可用带高 61。
//
// 墨迹高用**实测比例**:48 号数字实测墨迹 34px(cy=72 → 55..88),
// 而它的 line_height 是 52 —— 也就是说**墨迹只有行高的约 0.7 倍**。
// 这里按 0.7 估,理由:宁可估宽(估宽了反而更容易暴露"压到表情"),
// 实测那条在 test_readout_defaults_fit_gap 里钉着(它用 34/13 两个实测数)。
static int32_t inkHalf(int32_t px) { return (px * 7 + 5) / 20; }   // ≈ 0.35 × 字号

static void test_readout_font_scales_with_resolution(void) {
  // ① 点数表:480 那列是原始设计值,小屏那列必须更小
  TEST_ASSERT_TRUE_MESSAGE(readout_font_px(0) <= 48,
                           "大数字档不能超过 480 基准的 48 号");
  TEST_ASSERT_TRUE_MESSAGE(readout_font_px(1) <= 18,
                           "单位档不能超过 480 基准的 18 号");

  // 本机编译分辨率决定用哪一列 —— native 默认 480,240 那份由 env 的
  // -DTHEME_DISPLAY_RES=240 决定,这里两种都自洽(测试对两档都跑一遍)。
  const bool is240 = (readout_res_tier() == 1);

  const int32_t inner480 = 59;                 // 外弧带内沿(480 基准)
  const int32_t faceTop480 = 120;              // 表情图顶边(480 基准)
  // 位置跟着 theme_scale() 走(与 dash_ui.cpp 的 ts() 同一套换算)
  const int32_t digitCy = is240 ? 36 : 72;     // 72 × 240/480
  const int32_t unitCy  = is240 ? 53 : 107;    // 107 × 240/480 ≈ 53.5

  // ② 两档各自钉死点数:480 = 48/18(原始设计),240 = 24/10(×0.5 后取现成字号)
  if (is240) {
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(24, readout_font_px(0), "240 档大数字 = 48 × 240/480");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(10, readout_font_px(1),
                                    "240 档单位 = 18 × 0.5 = 9,取 LVGL 现成的 10 号");
  } else {
    TEST_ASSERT_EQUAL_UINT8(48, readout_font_px(0));
    TEST_ASSERT_EQUAL_UINT8(18, readout_font_px(1));
  }

  // ③ 竖直带里放得下:数字不许明显骑到弧带上,单位不许被表情压住。
  //    480 上数字允许蹭 4 像素(实测 55 对 59,刻意接受),240 上按同样比例给 2。
  const int32_t innerLimit = is240 ? (inner480 / 2 - 2) : (inner480 - 6);
  const int32_t faceLimit  = is240 ? (faceTop480 / 2 - 2) : (faceTop480 - 2);
  const int32_t digitTop = digitCy - inkHalf((int32_t)readout_font_px(0));
  const int32_t unitBottom = unitCy + inkHalf((int32_t)readout_font_px(1));

  TEST_ASSERT_TRUE_MESSAGE(digitTop >= innerLimit, "数字墨迹明显骑到弧带上了");
  TEST_ASSERT_TRUE_MESSAGE(unitBottom <= faceLimit, "单位墨迹会被表情图压住");
  TEST_ASSERT_TRUE_MESSAGE(digitTop < digitCy && digitCy < unitCy,
                           "数字要在单位上方,且都在自己中心附近");

  // ④ 字号档位越界必须兜底(绝不能返回空指针 —— LVGL 拿到 NULL 字体会崩)
  TEST_ASSERT_NOT_NULL(readout_font(0));
  TEST_ASSERT_NOT_NULL(readout_font(1));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(readout_font_px(0), readout_font_px(99),
                                  "越界档位要回落到 0 档,而不是读越界");
  TEST_ASSERT_EQUAL_PTR_MESSAGE(readout_font(0), readout_font(99),
                                "越界档位要拿到 0 档那张字体,不能是空指针");
}

void register_theme_store_tests(void) {
  RUN_TEST(test_parse_full_theme);
  RUN_TEST(test_parse_partial_keeps_defaults);
  RUN_TEST(test_parse_bare_root);
  RUN_TEST(test_parse_ignores_unknown);
  RUN_TEST(test_clamp_bad_values);
  RUN_TEST(test_parse_garbage_is_safe);
  RUN_TEST(test_screen_gauge_mapping);
  RUN_TEST(test_defaults_are_valid);
  RUN_TEST(test_parse_readout);
  RUN_TEST(test_parse_readout_missing_keeps_defaults);
  RUN_TEST(test_clamp_readout);
  RUN_TEST(test_readout_defaults_fit_gap);
  RUN_TEST(test_readout_font_scales_with_resolution);
  RUN_TEST(test_coolant_arc_is_mirrored);
  RUN_TEST(test_reverse_defaults_to_zero);
}
