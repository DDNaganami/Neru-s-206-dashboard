// ============================================================
// Preview-input and round-panel-overlay tests (host)
//
// This file was test_ui_lamps.cpp until 2026-10-15. Two of its three groups
// were about the SIX-CELL LAMP ROW (slot geometry inside the inscribed circle,
// clearance from the aux readout, the six-slot mapping, the alert overlay).
// The owner had the whole row removed from the UI, so those four test
// functions and the `lamp_view.h` include went away with it.
//
// What is left is the other two groups, both still load-bearing:
//   1. preview injection semantics (key map, control-file syntax, merge rules,
//      the mask toggle) -- the ONLY way "reproduce this state" in a report can
//      be trusted;
//   2. the round-panel geometry and overlay shading rules ("what is actually
//      visible on the 2.8C round screen") -- a wrong mask does not crash, it
//      just lies about which pixels the driver will ever see.
//
// Why these live in a native test at all: this is the ONLY native test file
// allowed to include `src/` (platformio.ini adds `-I src` for [env:native]).
// The .cpp files under src/ are NOT linked, so only pure header logic can be
// touched here -- any LVGL/Arduino symbol would fail at link time (that gate
// is deliberate).
// ============================================================
#include <unity.h>
#include <stdio.h>           // snprintf（那两条"够不够得着阈值"的断言要用）
#include <string.h>
#include <math.h>

#include "preview_input.h"   // preview_apply_key / preview_apply_control_text
#include "panel_view.h"      // round-screen visible area (2.8C tier)
#include "alerts.h"          // AlertsConfig：注入值必须够得着的那两个阈值

// ============================================================
// 五、注入:键位（每个键一指，X/Esc 全清）
// ============================================================
void test_preview_keys(void) {
  PreviewInput in;
  TEST_ASSERT_FALSE(in.any());

  // 转向灯是**开关**（按一下开、再按一下关）
  TEST_ASSERT_TRUE(preview_apply_key(in, PreviewKey::Left));
  TEST_ASSERT_TRUE(in.left && in.left_set);
  preview_apply_key(in, PreviewKey::Left);
  TEST_ASSERT_FALSE(in.left);
  TEST_ASSERT_TRUE(in.left_set);          // 设过就一直是"设过"（关也是一个选择）

  preview_apply_key(in, PreviewKey::Right);
  preview_apply_key(in, PreviewKey::Hazard);
  preview_apply_key(in, PreviewKey::LowBeam);
  preview_apply_key(in, PreviewKey::PositionLamp);
  preview_apply_key(in, PreviewKey::Door);
  TEST_ASSERT_TRUE(in.right && in.hazard && in.low_beam && in.position && in.door);

  // 超速/红区注入的是**具体数值**（阈值 + 余量），不是布尔
  preview_apply_key(in, PreviewKey::Overspeed);
  TEST_ASSERT_TRUE(in.speed_set);
  // ★★ 2026-10-15：注入值 **130 → 160**（阈值当天 120 → 150，见下一条用例）。
  TEST_ASSERT_EQUAL_FLOAT(160.0f, in.speed_kmh);
  preview_apply_key(in, PreviewKey::Redline);
  TEST_ASSERT_TRUE(in.rpm_set);
  TEST_ASSERT_TRUE(in.rpm > 5500.0f);          // 高于红区阈值（现在是 5500）

  // ★★ 2026-10-15：`M`（静音开关）**已删**。这条用例原本在这里翻两下 `mute`。
  //   蜂鸣器砍掉之后没有可掐的东西 ⇒ `PreviewKey::Mute` 这个枚举值也不存在了；
  //   下面"全清"那一段跟着少一条 `in.mute` 断言。
  //   仍然钉住的是"全清之后**所有**通道都回去"。

  // 全清:所有旗标都要回去（否则"按 X 之后还有一格亮着"永远查不出来）
  preview_apply_key(in, PreviewKey::Clear);
  TEST_ASSERT_FALSE(in.any());
  TEST_ASSERT_FALSE(in.speed_set && in.rpm_set);

  // 不认的键要**明确返回 false**（调用方据此打一行提示，别静默吞掉）
  TEST_ASSERT_FALSE(preview_apply_key(in, PreviewKey::None));
}

// ★★ 2026-10-15 新增：**超速那个注入值必须够得着告警阈值**
//
//   起因：把 `overspeed_kmh` 从 120 改成 150 的时候，`PreviewKey::Overspeed`
//   原来注入的 130 km/h **低于**阈值 ⇒ 按 O **不再触发超速告警**、屏上也就
//   不会染红。这正是"改了一个数、另一个地方悄悄失效"那一类问题。
//   ⇒ 注入值当天一起提到 **160**（阈值 + 10，与红区 6000 同一个思路），
//     而这一条用例把它钉住：**注入值够不着阈值时当场红**。
void test_preview_injection_reaches_threshold(void) {
  PreviewInput in;
  preview_apply_key(in, PreviewKey::Overspeed);
  char msg[192];
  snprintf(msg, sizeof(msg),
           "PreviewKey::Overspeed 注入 %.0f km/h，低于超速阈值 %.0f —— 按 O 不会染红",
           (double)in.speed_kmh, (double)AlertsConfig{}.overspeed_kmh);
  TEST_ASSERT_TRUE_MESSAGE(in.speed_kmh >= AlertsConfig{}.overspeed_kmh, msg);

  // 红区那一条同理（阈值 5500）：够不着就没有"按 R 进红区"这回事
  PreviewInput r;
  preview_apply_key(r, PreviewKey::Redline);
  char msg2[192];
  snprintf(msg2, sizeof(msg2),
           "PreviewKey::Redline 注入 %.0f rpm，低于红区阈值 %.0f —— 按 R 不会进红区",
           (double)r.rpm, (double)AlertsConfig{}.redline_rpm);
  TEST_ASSERT_TRUE_MESSAGE(r.rpm >= AlertsConfig{}.redline_rpm, msg2);
}

// ============================================================
// 六、注入:控制文件语法（键名、注释、空行、真假写法、不认的键）
// ============================================================
void test_preview_control_text(void) {
  PreviewInput in;
  // 一份"像报告里会贴的"控制文件
  // ★★ 2026-10-15：`mute=yes` 那一行**去掉了**（那个键随蜂鸣器删除）。
  //   ⇒ 认出来的条数从 7 变成 6（`left` / `door` / `speed` / `hazard` /
  //     `position` / `low_beam`），而 `nonsense=1` 照旧不算。
  //   ★ 这里刻意**没有**把 `mute=yes` 留在文本里当"不认的键"的例子：
  //     它的语义是"这个键曾经存在过、现在删了"，与 `nonsense` 那种"从来没有过"
  //     是两回事；老控制文件的兼容性由**另一条用例**管（见
  //     test_preview_control_text_ignores_unknown_keys 里那条 `mute=1`）。
  const char* text =
      "# 打左灯 + 开门 + 超速\n"
      "left=1\n"
      "\n"
      "  door = 1  \n"
      "speed=140.5   # 单位 km/h\n"
      "hazard=0\n"
      "position=on\n"
      "low_beam=true\n"
      "nonsense=1\n";

  // 认出来的 6 条：left / door / speed / hazard / position / low_beam
  //   ★ 一开始写的是 8（把注释行与空行也算进去了）—— 数一遍就知道不对，
  //     而这条断言的价值恰恰在于"数字必须数清楚"（`nonsense=1` 不算）。
  const int n = preview_apply_control_text(in, text);
  TEST_ASSERT_EQUAL_INT(6, n);
  TEST_ASSERT_TRUE(in.left && in.left_set);
  TEST_ASSERT_TRUE(in.door && in.door_set);
  TEST_ASSERT_TRUE(in.speed_set);
  TEST_ASSERT_EQUAL_FLOAT(140.5f, in.speed_kmh);
  TEST_ASSERT_TRUE(in.hazard_set);
  TEST_ASSERT_FALSE(in.hazard);         // hazard=0 也要"设过"
  TEST_ASSERT_TRUE(in.position && in.position_set);   // on
  TEST_ASSERT_TRUE(in.low_beam && in.low_beam_set);   // true

  // ★★ 2026-10-15 新增：**删掉的键（`mute=`）与从来没认过的键一样安全**
  //   —— 老控制文件里写着 `mute=1` 的人不会崩、也不会被静默吞掉别的键。
  //   （返回值里不算它，调用方据此打一行提示；这正是这一段原本就有的语义。）
  {
    PreviewInput old;
    const int got = preview_apply_control_text(old, "mute=1\nleft=1\n");
    TEST_ASSERT_EQUAL_INT(1, got);        // 只有 left 算
    TEST_ASSERT_TRUE(old.left && old.left_set);
  }

  // 认不出的键**不许**把别的键带歪（也不许报错崩掉）
  PreviewInput only_bad;
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(only_bad, "what=1\n# 全注释\n"));
  TEST_ASSERT_FALSE(only_bad.any());

  // 空文本 / 空指针是安全的（"没有控制文件"就是这一条）
  PreviewInput e;
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(e, ""));
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(e, nullptr));

  // clear=1 要把整份注入清掉
  PreviewInput c;
  preview_apply_control_text(c, "left=1 right=1 door=1 rpm=6000\n");
  TEST_ASSERT_TRUE(c.any());
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(c, "clear=1\n"));
  TEST_ASSERT_FALSE(c.any());
}

// ============================================================
// 七、遮罩开关（V 键 / `mask=`）：默认开、翻转、clear 回到"开"
// ============================================================
void test_preview_mask_toggle(void) {
  PreviewInput in;
  // 默认就是"开"——2.8C 档落的第一张图就该带可视圈标注
  TEST_ASSERT_TRUE(in.panel_mask);
  TEST_ASSERT_FALSE(in.panel_mask_set);      // 但"没显式设过"

  TEST_ASSERT_TRUE(preview_apply_key(in, PreviewKey::ToggleMask));
  TEST_ASSERT_FALSE(in.panel_mask);
  TEST_ASSERT_TRUE(in.panel_mask_set);       // 显式选择过 ⇒ 控制文件不许盖回去
  preview_apply_key(in, PreviewKey::ToggleMask);
  TEST_ASSERT_TRUE(in.panel_mask);

  // ★ 只按 V **不该**让 any() 变成 true：遮罩不是车辆状态，
  //   否则 `inject:` 那行回执会假装"车状态变了"。
  PreviewInput only_mask;
  preview_apply_key(only_mask, PreviewKey::ToggleMask);
  TEST_ASSERT_FALSE(only_mask.any());
  TEST_ASSERT_FALSE(only_mask.panel_mask);

  // 控制文件：mask=0 / mask=1 都算"设过"
  PreviewInput f;
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(f, "mask=0\n"));
  TEST_ASSERT_FALSE(f.panel_mask);
  TEST_ASSERT_TRUE(f.panel_mask_set);
  TEST_ASSERT_FALSE(f.any());
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(f, "mask=1\n"));
  TEST_ASSERT_TRUE(f.panel_mask);

  // clear=1 把遮罩复位成默认的"开"（"回到假数据的样子"= 带标注的样子）
  PreviewInput c;
  preview_apply_control_text(c, "mask=0 left=1\n");
  TEST_ASSERT_FALSE(c.panel_mask);
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(c, "clear=1\n"));
  TEST_ASSERT_TRUE(c.panel_mask);
  TEST_ASSERT_FALSE(c.panel_mask_set);
}

// ============================================================
// 八、注入 → 快照：双闪与左右箭头的合并语义
//   （这一条是"实测踩到"才加的：控制文件里写了 left=1，屏上却一直是
//     双闪的样子 —— 因为上一次注入的 hazard=1 把左右箭头也置位了，
//     而后来那次只写 left/right 时没人去清另外一位。）
// ============================================================
void test_preview_apply_snapshot(void) {
  VehicleState st{};
  TEST_ASSERT_FALSE(st.indicator_left || st.indicator_right || st.hazard);
  TEST_ASSERT_FALSE(st.low_beam || st.position_lamp || st.door_activity);

  // ---- ① 没注入过的通道一个都不许动 ----
  {
    PreviewInput none;
    VehicleState s{};
    s.speed_kmh = 42.0f;
    s.coolant_c = 88.0f;
    preview_apply_snapshot(none, s);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, s.speed_kmh);
    TEST_ASSERT_EQUAL_FLOAT(88.0f, s.coolant_c);
    TEST_ASSERT_FALSE(s.indicator_left || s.indicator_right || s.hazard);
  }

  // ---- ② hazard=1：双闪 + 左右箭头一起（数据层的位域语义）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "hazard=1\n");
    preview_apply_snapshot(in, st);
    TEST_ASSERT_TRUE(st.hazard);
    TEST_ASSERT_TRUE(st.indicator_left);
    TEST_ASSERT_TRUE(st.indicator_right);
  }

  // ---- ③ hazard=0：三位一起清（★ 这一条就是踩到的那个）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "hazard=0\n");
    preview_apply_snapshot(in, st);
    TEST_ASSERT_FALSE(st.hazard);
    TEST_ASSERT_FALSE(st.indicator_left);
    TEST_ASSERT_FALSE(st.indicator_right);
  }

  // ---- ④ 同一帧里 `hazard=1 left=0 right=0` 也要表达得出来 ----
  //   （这就是"双闪先施加、箭头后施加"那个顺序换来的表达力）
  //   ★ 控制文件是**一行一条**：三个键要写三行（写成一行里的三个空格分隔项
  //     时，第二项会被当成第一项的"值" —— 实测踩到，parse 只认了 1 条）。
  {
    PreviewInput in;
    const int parsed = preview_apply_control_text(in, "hazard=1\nleft=0\nright=0\n");
    TEST_ASSERT_EQUAL_INT(3, parsed);
    TEST_ASSERT_TRUE(in.hazard_set);
    TEST_ASSERT_TRUE(in.left_set);
    TEST_ASSERT_TRUE(in.right_set);
    TEST_ASSERT_TRUE(in.hazard);
    TEST_ASSERT_FALSE(in.left);
    TEST_ASSERT_FALSE(in.right);
    VehicleState s{};                     // 干净状态：这一条验的是"合并语义"本身
    preview_apply_snapshot(in, s);
    TEST_ASSERT_TRUE(s.hazard);
    TEST_ASSERT_FALSE(s.indicator_left);
    TEST_ASSERT_FALSE(s.indicator_right);
    // 再施加一遍还是同一结果（幂等：控制文件是每帧重读的）
    preview_apply_snapshot(in, s);
    TEST_ASSERT_TRUE(s.hazard);
    TEST_ASSERT_FALSE(s.indicator_left);
  }

  // ---- ⑤ 单打一个左转：hazard 只按"注入写过没有"走（逐通道语义）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "hazard=0\nleft=1\n");
    VehicleState s{};                     // ★ 每一段都用干净状态：注入只覆写
    preview_apply_snapshot(in, s);        //   它**写过**的那几位（这一条本身就是
    TEST_ASSERT_FALSE(s.hazard);          //   语义的一部分，别拿上一段的残留当输入）
    TEST_ASSERT_TRUE(s.indicator_left);
    TEST_ASSERT_FALSE(s.indicator_right);
    // 而"上一次注入留下的值"要能延续：同一份注入连续施加两次结果必须一样
    VehicleState s2{};
    preview_apply_snapshot(in, s2);
    TEST_ASSERT_EQUAL_INT(s.indicator_left, s2.indicator_left);
    TEST_ASSERT_EQUAL_INT(s.hazard, s2.hazard);
  }

  // ---- ⑥ 其余通道：灯位 / 门 / speed / rpm ----
  //   ★ 一行一条（控制文件的语法就是"每行一个键=值"；写成一行里的多个
  //     空格分隔项时，第二个开始会被当成第一个的"值" —— 实测 parse 只认 1 条）
  {
    PreviewInput in;
    preview_apply_control_text(in,
        "low_beam=1\nposition=1\ndoor=1\nspeed=130\nrpm=6000\n");
    VehicleState s{};
    preview_apply_snapshot(in, s);
    TEST_ASSERT_TRUE(s.low_beam);
    TEST_ASSERT_TRUE(s.position_lamp);
    TEST_ASSERT_TRUE(s.door_activity);
    TEST_ASSERT_EQUAL_FLOAT(130.0f, s.speed_kmh);
    TEST_ASSERT_EQUAL_FLOAT(6000.0f, s.rpm);
  }

  // ---- ⑦ 清过的注入不许再改快照（`clear=1` 之后是"没注入过"）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "left=1\nspeed=200\nclear=1\n");
    VehicleState s{};
    s.speed_kmh = 55.0f;
    preview_apply_snapshot(in, s);
    TEST_ASSERT_FALSE(s.indicator_left);
    TEST_ASSERT_EQUAL_FLOAT(55.0f, s.speed_kmh);
  }
}

// ============================================================
// 九、「2.8C（最终板）」的圆屏几何与遮罩着色规则
//
// 这一组的价值：遮罩算歪**不会报错** —— 只会让"圆外压暗"那块形状不对，
// 而那正是"素材会不会被圆边切掉"的唯一参照。所以逐条钉住：
//   ① 内切正方形 = 336 / 168（与 tools/theme-editor/asset-spec.js 的
//      circleSafeSide() 同一套算式 —— 两边不一致时这一条会红）；
//   ② 有效区 Ø70.13mm、1 像素 0.1461mm 的换算；
//   ③ 着色规则三段（圆内不动 / 圈带半边暗 / 四角打点）；
//   ④ 遮罩**一个圆内像素都不许改**（那是真机画面）。
// ============================================================
void test_panel_round_geometry(void) {
  // ---- ① 内切正方形（两条链同一个数）----
  TEST_ASSERT_EQUAL_INT32(336, panelInscribedSquareSide(480));
  TEST_ASSERT_EQUAL_INT32(168, panelInscribedSquareSide(240));
  // 它必须是 **4 的倍数**（素材尺寸对齐口径，网页那边也是这条）
  TEST_ASSERT_EQUAL_INT32(0, panelInscribedSquareSide(480) % 4);
  TEST_ASSERT_EQUAL_INT32(0, panelInscribedSquareSide(240) % 4);
  // 而它必须真的落在圆里：对角线一半 <= 半径
  {
    const double half = panelInscribedSquareSide(480) * sqrt(2.0) / 2.0;
    TEST_ASSERT_TRUE(half <= panelRadiusPx(480) + 1e-6);
  }

  // ---- ② 物理口径：Ø70.13mm ⇒ 0.1461 mm/px ----
  TEST_ASSERT_EQUAL_INT32(7013, kPanelActiveAreaMm10);          // 70.13 mm（单位 1e-2 mm）
  TEST_ASSERT_EQUAL_INT32(1461, panelMmPerPx10000(480));        // 0.1461 mm/px（单位 1e-4 mm）
  // 480 个像素整好铺满有效区（换算与直径自洽）
  TEST_ASSERT_EQUAL_INT32(7013, panelPxToMm100(480, 480));      // = 70.13 mm
  TEST_ASSERT_EQUAL_INT32(3506, panelPxToMm100(240, 480));      // 半径 ≈ 35.06 mm

  // ---- ③ 圆内 / 圆外：四个角一定在圆外，屏心一定在圆内 ----
  TEST_ASSERT_TRUE(panelPixelVisible(240, 240, 480));
  TEST_ASSERT_TRUE(panelPixelVisible(240, 0, 480));     // 上边中点（在圆上）
  TEST_ASSERT_TRUE(panelPixelVisible(0, 240, 480));     // 左边中点
  TEST_ASSERT_FALSE(panelPixelVisible(0, 0, 480));      // 四角
  TEST_ASSERT_FALSE(panelPixelVisible(479, 479, 480));
  TEST_ASSERT_FALSE(panelPixelVisible(0, 479, 480));
  TEST_ASSERT_FALSE(panelPixelVisible(479, 0, 480));
  // 圆外像素占比应当明显小于一半（四角那四块 ≈ 21%）
  {
    const int32_t outside = panelOutsidePixelCount(480);
    const int32_t total = 480 * 480;
    TEST_ASSERT_TRUE(outside > 0);
    TEST_ASSERT_TRUE(outside < total / 4);
  }

  // ---- ④ 着色规则三段 ----
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeInside, panelShadeAt(240, 240, 480));
  // ★ 边界这一条挑的是**圆外头几像素**那一圈：圆内最边上一列/一行是
  //   (0,225) / (240,0)（距离 239.94 / 239.5，还在圆里），往斜下走两像素
  //   就出圆了 —— (0,224) 的距离正好 240.00，落在参考圈带里。
  //   （第一版把坐标写成 (0,240)/(240,1)，那两点其实**在圆内** ⇒ 用例当场红。
  //    这几个点是用 `sqrt(240² - (x+0.5-240)²)` 算出来的，不是估的。）
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeInside, panelShadeAt(0, 225, 480));
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeInside, panelShadeAt(240, 0, 480));
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeRing, panelShadeAt(0, 224, 480));
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeRing, panelShadeAt(1, 212, 480));
  {
    const uint16_t c1 = panelShadeAt(2, 2, 480);
    const uint16_t c2 = panelShadeAt(4, 2, 480);
    TEST_ASSERT_TRUE(c1 == kPanelShadeCornerA || c1 == kPanelShadeCornerB);
    TEST_ASSERT_TRUE(c2 == kPanelShadeCornerA || c2 == kPanelShadeCornerB);
    TEST_ASSERT_TRUE(c1 != c2);       // 棋盘格：相邻两格必须不一样（否则看不出是标注）
  }
  // 圈带宽度按分辨率缩（480 档 3 px，240 档也不小于 2）
  TEST_ASSERT_EQUAL_INT32(3, panelRingWidthPx(480));
  TEST_ASSERT_TRUE(panelRingWidthPx(240) >= 2);
}

void test_panel_overlay_leaves_inside_untouched(void) {
  // 造一份"每个像素都不一样"的缓冲（值 = 索引的低 16 位），跑完遮罩之后
  // **圆内必须逐像素等于原值**、圆外必须逐像素变了。这一条把
  // "遮罩会不会误伤真机可见区"钉死。
  static uint16_t buf[480 * 480];
  static uint16_t orig[480 * 480];
  for (int32_t i = 0; i < 480 * 480; ++i) {
    // ★ 三个通道**都非零**：R = 0x1F、B = 0x1F、G = (i & 0x3F) + 1。
    //   为什么刻意避开任何通道为 0：通道值 0 乘任何亮度还是 0，那样
    //   "圆外的像素被改过没有"会**假绿**（实测踩到：第一版用
    //   `0x0800 + (i & 0x7FF)`，于是 G 在 1/2 亮度下被 0x0800>>1 抹掉、
    //   看起来"没变"，用例报 7 个圆外像素未变 —— 数是对的，图案不对）。
    buf[i] = (uint16_t)(0x801Fu | ((uint16_t)((i & 0x3Fu) + 1u) << 5));
    orig[i] = buf[i];
  }
  const int32_t changed = panelApplyOverlayRgb565(buf, 480);
  TEST_ASSERT_EQUAL_INT32(panelOutsidePixelCount(480), changed);

  int32_t inside_changed = 0, outside_unchanged = 0;
  for (int32_t y = 0; y < 480; ++y) {
    for (int32_t x = 0; x < 480; ++x) {
      const int32_t i = y * 480 + x;
      if (panelPixelVisible(x, y, 480)) {
        if (buf[i] != orig[i]) ++inside_changed;
      } else {
        if (buf[i] == orig[i]) ++outside_unchanged;
      }
    }
  }
  TEST_ASSERT_EQUAL_INT32(0, inside_changed);
  TEST_ASSERT_EQUAL_INT32(0, outside_unchanged);

  // 缩放是"按通道"的（不是把 16 位整数整体乘）—— 否则半边暗会串色。
  // 圈带（k=128）下，一个纯红像素应当还是"红占优"。
  {
    uint16_t px = (uint16_t)(0x1Fu << 11);      // RGB565 纯红
    const uint16_t k = kPanelShadeRing;
    const uint16_t r = panelScaleRgb565Channel((uint16_t)((px >> 11) & 0x1Fu), k);
    const uint16_t g = panelScaleRgb565Channel((uint16_t)((px >> 5) & 0x3Fu), k);
    const uint16_t b = panelScaleRgb565Channel((uint16_t)(px & 0x1Fu), k);
    TEST_ASSERT_EQUAL_UINT16(16, r);      // 31 * 128/256 ≈ 15.5 → 16
    TEST_ASSERT_EQUAL_UINT16(0, g);
    TEST_ASSERT_EQUAL_UINT16(0, b);
  }
}

void register_preview_input_tests(void) {
  RUN_TEST(test_preview_keys);
  RUN_TEST(test_preview_injection_reaches_threshold);
  RUN_TEST(test_preview_control_text);
  RUN_TEST(test_preview_mask_toggle);
  RUN_TEST(test_preview_apply_snapshot);
  RUN_TEST(test_panel_round_geometry);
  RUN_TEST(test_panel_overlay_leaves_inside_untouched);
}
