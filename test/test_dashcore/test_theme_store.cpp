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
  // 派生字段:必须由"表情开始 + 4 次眨眼"算出来
  TEST_ASSERT_EQUAL_UINT32(t.boot_face_start_ms + 4 * t.boot_face_blink_ms,
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
        {"radius": -50, "width": 0, "start_deg": 300, "end_deg": 100}]},
     {"arc_count": 1, "arcs":[{"radius": 99999, "width": 500}]}]}})";
  TEST_ASSERT_TRUE(theme_parse_json(json2, (uint32_t)strlen(json2), t));
  TEST_ASSERT_TRUE(t.screens[0].arc_count <= kMaxArcs);   // 弧数量上限
  TEST_ASSERT_TRUE(t.screens[0].arcs[0].radius >= 10);
  TEST_ASSERT_TRUE(t.screens[0].arcs[0].width >= 1);
  TEST_ASSERT_TRUE(t.screens[0].arcs[0].end_deg > t.screens[0].arcs[0].start_deg);
  TEST_ASSERT_TRUE(t.screens[1].arcs[0].radius <= 240);
  TEST_ASSERT_TRUE(t.screens[1].arcs[0].width <= 60);

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

  // 右屏 = 速度表
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, d.screens[1].arc_count,
                                  "右屏应有一条弧(车速)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ArcKind::Speed, (uint8_t)d.screens[1].arcs[0].kind,
                                  "右屏必须是车速(法系车右=速度表)");
}

// 屏 ↔ 表情图片角色的对应在 test_image_blob.cpp 里(那里才有 ImageRole)。

void register_theme_store_tests(void) {
  RUN_TEST(test_parse_full_theme);
  RUN_TEST(test_parse_partial_keeps_defaults);
  RUN_TEST(test_parse_bare_root);
  RUN_TEST(test_parse_ignores_unknown);
  RUN_TEST(test_clamp_bad_values);
  RUN_TEST(test_parse_garbage_is_safe);
  RUN_TEST(test_screen_gauge_mapping);
  RUN_TEST(test_defaults_are_valid);
}
