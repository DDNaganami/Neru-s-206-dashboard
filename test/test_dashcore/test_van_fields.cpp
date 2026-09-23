// ============================================================
// 已解字段接进数据层 —— 用例（2026-09-24）
//
// 覆盖三类字段（全部**只有 VAN 一个来源**）:
//   · 0x4FC 的 data[5] 灯位域（§4.3/§4.4）与 data[1] 门信号（§4.6）
//   · 0xE24 的 17 字节明文 ASCII VIN（§4.7）
//
// ★ 这里的期望值一律**从常量推导**（kVanLightLeft / kVanVinChars …），
//   不写裸数字 —— 与 test_data_service.cpp 里那条"期望值由标度常量推导"
//   是同一套纪律（那次 kSpeedScale 一改就留下 3 条陈旧断言）。
//
// ★ 数据来源说明（"用既有回放/切片数据"这一条怎么落的）:
//   两份切片 `tools/van-decode/sample-speed-824.csv` / `sample-diffmanchester.csv`
//   是**逻辑分析仪原始边沿**（`Time [s],Channel 0` 只有电平跳变），不是解好的帧 ——
//   仓库里**没有** 0x4FC / 0xE24 的帧文本（`tools/serial-capture/` 里也没有
//   `VAN <iden> …` 行）。所以本文件按**协议文档 §4.3/§4.4/§4.6/§4.7 的实测结论**
//   造帧（帧长用 §3 的实测值 kVanLightLen / kVanVinLen，位掩码用 §4 的常量），
//   而**真实边沿 → 帧**这一段仍然由 test_van_real_capture.cpp 拿那两份切片钉着
//   （33 帧 / 黄金值），两边合起来才是"从线上字节到字段"的整条链。
// ============================================================
#include <unity.h>
#include <string.h>
#include "test_helpers.h"
#include "data_service.h"
#include "van_source.h"
#include "alerts.h"       // 只为"红区地标与表情一致"那条对照(AlertsConfig{}.redline_rpm)

// 以 50ms 步长推进虚拟时间,反复跑 update（与 test_data_service.cpp 同一套）
static void advance_ms(VehicleDataService& svc, uint32_t& t, uint32_t ms) {
  const uint32_t end = t + ms;
  while (t < end) {
    t += 50;
    svc.update(t);
  }
}

// ---- 造帧小工具（每一条都先清零，免得上一帧的字节漏进来）----

// 0x4FC 灯/门帧：data[5] = 灯位域、data[1] = 门信号；帧长用 §3 的实测 11
static void feedLights(VehicleDataService& svc, uint8_t light_bits, uint8_t door,
                       uint32_t now_ms) {
  VanPacket p{};
  p.iden = VanSource::kLightIden;
  p.len = kVanLightLen;
  p.data[VanSource::kLightOffset] = light_bits;
  p.data[VanSource::kDoorOffset] = door;
  p.rx_ms = now_ms;
  svc.onVanPacket(p);
}

// 0xE24 VIN 帧：data[0..16] = 17 字节；帧长用 §3 的实测 17
static void feedVin(VehicleDataService& svc, const char* vin, uint32_t now_ms) {
  VanPacket p{};
  p.iden = VanSource::kVinIden;
  p.len = kVanVinLen;
  memcpy(p.data, vin, kVanVinChars);
  p.rx_ms = now_ms;
  svc.onVanPacket(p);
}

// ============================================================
// 一、没有这些帧时:字段保持默认、**不误报**
// ============================================================
// ★ 这一组是"数据层没接错"的第一道闸门:一个坏实现（比如把没收到帧当成
//   "门开着"、或者来源格默认写成 Van）会在这里当场红。
void test_lights_absent_keeps_defaults_and_no_source(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;
  const VehicleState st = svc.update(t);
  advance_ms(svc, t, 2000);   // 跑一会儿,确认"时间过去"也不会自己冒出值来

  // 值:一个都没亮 / 没有门活动 / VIN 是空串(不是"未知"占位符)
  TEST_ASSERT_FALSE(st.indicator_left);
  TEST_ASSERT_FALSE(st.indicator_right);
  TEST_ASSERT_FALSE(st.hazard);
  TEST_ASSERT_FALSE(st.position_lamp);
  TEST_ASSERT_FALSE(st.low_beam);
  TEST_ASSERT_FALSE(st.door_activity);
  TEST_ASSERT_EQUAL_STRING("", st.vin);

  // 来源:七格全 None —— 没有帧就是"这一格没有有效值",不许默认成 Van
  const DataSourceStatus& s = svc.status();
  TEST_ASSERT_TRUE(s.indicator_left == FieldSource::None);
  TEST_ASSERT_TRUE(s.indicator_right == FieldSource::None);
  TEST_ASSERT_TRUE(s.hazard == FieldSource::None);
  TEST_ASSERT_TRUE(s.position_lamp == FieldSource::None);
  TEST_ASSERT_TRUE(s.low_beam == FieldSource::None);
  TEST_ASSERT_TRUE(s.door == FieldSource::None);
  TEST_ASSERT_TRUE(s.vin == FieldSource::None);

  // age:三格都是"没有数据"的哨兵,而不是某个算出来的小数
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.lights_age_ms);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.door_age_ms);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.vin_age_ms);
}

// ============================================================
// 二、灯位解包:位域边界
// ============================================================
// ★ 逐位一张表:值 → 期望(左/右/双闪/近光/仪表盘灯)。
//   这里**刻意**包含 0x84(仪表盘灯 + 左转)这种组合 —— 它正是"等值判断"
//   会踩的坑:`v == 0x04` 的写法会把 0x84 判成"没有左转"。
void test_light_bitfield_decoding(void) {
  static const struct { uint8_t v; bool l, r, hz, lb, dash; } kCases[] = {
    // v     左     右     双闪   近光   仪表盘
    { 0x00, false, false, false, false, false },   // 全灭(§4.3 的表)
    { 0x04, true,  false, false, false, false },   // 左转向
    { 0x08, false, true,  false, false, false },   // 右转向
    { 0x0C, true,  true,  true,  false, false },   // 双闪 = 左|右(位域的独立证据)
    { 0x40, false, false, false, true,  false },   // 近光(灯杆第 2 档)
    { 0x80, false, false, false, false, true  },   // 仪表盘灯(灯杆第 1 档)
    { 0xC0, false, false, false, true,  true  },   // 两档灯同时(近光 + 仪表盘灯)
    { 0x84, true,  false, false, false, true  },   // ★ 组合:等值判断会踩的那个
    { 0x8C, true,  true,  true,  false, true  },   // ★ 双闪 + 仪表盘灯
    { 0xFF, true,  true,  true,  true,  true  },   // 全置位:每一位都独立解出来
  };

  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    VehicleDataService svc(nullptr);
    svc.begin();
    const uint32_t t = 1000;
    feedLights(svc, kCases[i].v, 0x00, t);
    const VehicleState st = svc.update(t);

    TEST_ASSERT_EQUAL_HEX8(kCases[i].v, svc.vanSource().lights().raw);
    TEST_ASSERT_EQUAL(kCases[i].l,    st.indicator_left);
    TEST_ASSERT_EQUAL(kCases[i].r,    st.indicator_right);
    TEST_ASSERT_EQUAL(kCases[i].hz,   st.hazard);
    TEST_ASSERT_EQUAL(kCases[i].lb,   st.low_beam);
    TEST_ASSERT_EQUAL(kCases[i].dash, st.position_lamp);

    // 来源:五格都是 Van(唯一来源),age 是刚收到那一帧的年龄
    TEST_ASSERT_TRUE(svc.status().indicator_left == FieldSource::Van);
    TEST_ASSERT_TRUE(svc.status().indicator_right == FieldSource::Van);
    TEST_ASSERT_TRUE(svc.status().hazard == FieldSource::Van);
    TEST_ASSERT_TRUE(svc.status().position_lamp == FieldSource::Van);
    TEST_ASSERT_TRUE(svc.status().low_beam == FieldSource::Van);
    TEST_ASSERT_EQUAL_UINT32(0u, svc.status().lights_age_ms);
  }
}

// ============================================================
// 三、保持窗口(欠采样的补法):亮 → 过窗灭;窗口内"再亮一次"续上
// ============================================================
void test_indicator_hold_window(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  feedLights(svc, kVanLightLeft, 0x00, t);
  TEST_ASSERT_TRUE(svc.update(t).indicator_left);

  // 窗口内(600 ms 里走了 500)仍算亮着 —— 这就是"4.7 帧/秒 + 0.8 s 闪烁"
  // 那笔欠采样的账(见 kIndicatorHoldMs 的推导)。
  advance_ms(svc, t, 500);
  TEST_ASSERT_TRUE(svc.vanSource().lightsRecent(t));
  TEST_ASSERT_TRUE(svc.update(t).indicator_left);
  TEST_ASSERT_TRUE(svc.status().indicator_left == FieldSource::Van);
  TEST_ASSERT_EQUAL_UINT32(500u, svc.status().lights_age_ms);

  // 越过窗口(再过 200 ms,合计 700 > 600)⇒ 灯灭、来源回 None
  advance_ms(svc, t, 200);
  const VehicleState st = svc.update(t);
  TEST_ASSERT_FALSE(svc.vanSource().lightsRecent(t));
  TEST_ASSERT_FALSE(st.indicator_left);
  TEST_ASSERT_TRUE(svc.status().indicator_left == FieldSource::None);

  // ★ 关键的一条:数据本身还在(lightsLastMs 没变、raw 还是 0x04),
  //   "灭"是**窗口判定**的结果,不是把数据抹了 ——
  //   UI 要靠这个区分"有数据但灯灭"与"根本没数据"。
  TEST_ASSERT_TRUE(svc.vanSource().hasLights());
  TEST_ASSERT_EQUAL_HEX8(kVanLightLeft, svc.vanSource().lights().raw);
  TEST_ASSERT_EQUAL_UINT32(700u, svc.status().lights_age_ms);

  // 窗口内再收到一帧"亮" ⇒ 计时重新开始(灯接着亮)
  feedLights(svc, kVanLightLeft, 0x00, t);
  TEST_ASSERT_TRUE(svc.update(t).indicator_left);
  TEST_ASSERT_EQUAL_UINT32(0u, svc.status().lights_age_ms);
}

// ============================================================
// 四、门信号:只报"动过",不报"哪扇门 / 开着还是关着"
// ============================================================
// ★ 用例名与注释都刻意避开"门开着"三个字:左右门**不可分辨 = 未解**
//   (§6 撤回①),而"==1 就是门开着"被 §4.6 否掉(脉冲段内还在 00↔01 跳变)。
void test_door_activity_only(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  // 第一帧就是**静息基线**(实车静息 0x00,§4.6 的表)⇒ 不报
  feedLights(svc, 0x00, 0x00, t);
  TEST_ASSERT_FALSE(svc.update(t).door_activity);
  TEST_ASSERT_TRUE(svc.status().door == FieldSource::Van);   // 收到过 ⇒ 有来源
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, svc.status().door_age_ms);  // 还没变化过

  // 相对基线变了(0x00 → 0x01)⇒ 报"动过",age 归零
  t += 250;
  feedLights(svc, 0x00, 0x01, t);
  TEST_ASSERT_TRUE(svc.update(t).door_activity);
  TEST_ASSERT_EQUAL_UINT32(0u, svc.status().door_age_ms);

  // ★ 段内跳变(§4.6:门开那几段**段内就在 00↔01 跳**)⇒ 仍然是"动过"
  t += 250;
  feedLights(svc, 0x00, 0x00, t);
  TEST_ASSERT_TRUE(svc.update(t).door_activity);

  // 动作结束、静默 9 秒(> 8 秒的活动窗口)⇒ 不再报;
  //   而**来源格仍是 Van**(这一格问的是"有没有数据",不是"门在不在动")
  advance_ms(svc, t, 9000);
  TEST_ASSERT_FALSE(svc.update(t).door_activity);
  TEST_ASSERT_TRUE(svc.status().door == FieldSource::Van);
}

// ============================================================
// 五、VIN:17 字节明文解出来;坏帧被拒;不需要跟别的族拼
// ============================================================
void test_vin_decoding(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  const uint32_t t = 1000;
  // §4.7 的实测原文:0xE24 的 17 个数据字节就是 17 位 VIN
  feedVin(svc, "VF32DNFUR2W005450", t);
  const VehicleState st = svc.update(t);

  TEST_ASSERT_EQUAL_STRING("VF32DNFUR2W005450", st.vin);
  TEST_ASSERT_EQUAL_UINT8(kVanVinChars, (uint8_t)strlen(st.vin));
  TEST_ASSERT_TRUE(svc.status().vin == FieldSource::Van);
  TEST_ASSERT_EQUAL_UINT32(0u, svc.status().vin_age_ms);
  TEST_ASSERT_TRUE(svc.vanSource().hasVin());

  // ★ 单独的 0x5E4 **不该**被当成 VIN(§4.7 的自我更正:不需要拼任何东西)
  VanPacket p{};
  p.iden = 0x5E4;
  p.len = 2;
  p.data[0] = 0x00;
  p.data[1] = 0x01;
  p.rx_ms = t + 100;
  svc.onVanPacket(p);
  const VehicleState st2 = svc.update(t + 100);
  TEST_ASSERT_EQUAL_STRING("VF32DNFUR2W005450", st2.vin);   // 不变
}

void test_vin_rejects_non_printable_and_short(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  const uint32_t t = 1000;

  // 先收一帧**好**的:坏帧之后必须还是它(不能把好值冲掉,也不能变空串)
  feedVin(svc, "VF32DNFUR2W005450", t);
  svc.update(t);
  TEST_ASSERT_TRUE(svc.vanSource().hasVin());

  // ① 有一个字节不可打印(0x01)⇒ 整帧丢
  VanPacket bad{};
  bad.iden = VanSource::kVinIden;
  bad.len = kVanVinLen;
  memcpy(bad.data, "VF32DNFUR2W005450", kVanVinChars);
  bad.data[5] = 0x01;                 // 把 'N' 改成控制字符
  bad.rx_ms = t + 100;
  svc.onVanPacket(bad);
  TEST_ASSERT_EQUAL_STRING("VF32DNFUR2W005450", svc.update(t + 100).vin);

  // ② 高字节(> 0x7E)同样不可打印 ⇒ 丢
  VanPacket high{};
  high.iden = VanSource::kVinIden;
  high.len = kVanVinLen;
  memcpy(high.data, "VF32DNFUR2W005450", kVanVinChars);
  high.data[0] = 0xC3;                // UTF-8 首字节那类
  high.rx_ms = t + 200;
  svc.onVanPacket(high);
  TEST_ASSERT_EQUAL_STRING("VF32DNFUR2W005450", svc.update(t + 200).vin);

  // ③ 短帧(只来了 16 字节)⇒ 半截 VIN 比没有更糟,直接丢
  VanPacket shortv{};
  shortv.iden = VanSource::kVinIden;
  shortv.len = kVanVinChars - 1;
  memcpy(shortv.data, "VF32DNFUR2W00545", kVanVinChars - 1);
  shortv.rx_ms = t + 300;
  svc.onVanPacket(shortv);
  TEST_ASSERT_EQUAL_STRING("VF32DNFUR2W005450", svc.update(t + 300).vin);

  // ④ 全新对象上只来坏帧 ⇒ 一直是空串(不是"收下了垃圾")
  VehicleDataService svc2(nullptr);
  svc2.begin();
  VanPacket only{};
  only.iden = VanSource::kVinIden;
  only.len = kVanVinLen;
  memset(only.data, 0x00, kVanVinLen);   // 17 个 0x00:不可打印
  only.rx_ms = t;
  svc2.onVanPacket(only);
  TEST_ASSERT_EQUAL_STRING("", svc2.update(t).vin);
  TEST_ASSERT_FALSE(svc2.vanSource().hasVin());
  TEST_ASSERT_TRUE(svc2.status().vin == FieldSource::None);
}

// ============================================================
// 六、短帧不该读越界
// ============================================================
// ★ 三处 `offset < len` 判据一起钉:灯帧只有 5 字节(读不到 data[5])时
//   不许解包、也不许把来源标成 Van;kVinIden 的短帧已被上一条覆盖。
void test_short_light_frame_is_ignored(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  const uint32_t t = 1000;

  VanPacket p{};
  p.iden = VanSource::kLightIden;
  p.len = VanSource::kLightOffset;   // 5 字节 ⇒ data[5] 读不到
  p.data[VanSource::kDoorOffset] = 0x01;   // 门字节倒是读得到
  p.rx_ms = t;
  svc.onVanPacket(p);
  const VehicleState st = svc.update(t);

  TEST_ASSERT_FALSE(svc.vanSource().hasLights());
  TEST_ASSERT_FALSE(st.indicator_left);
  TEST_ASSERT_TRUE(svc.status().indicator_left == FieldSource::None);
  TEST_ASSERT_TRUE(svc.status().lights_age_ms == UINT32_MAX);
  // 门字节在同一帧里、且读得到 ⇒ 门这一路照常工作(两条判据是独立的)
  TEST_ASSERT_TRUE(svc.vanSource().hasDoor());
  TEST_ASSERT_EQUAL_HEX8(0x01, svc.vanSource().doorRaw());
}

// ============================================================
// 七、★ 没动既有优先级(两条硬证据)
// ============================================================
// ① 灯/门/VIN 这三族**不与车速/转速抢**:三个字段的来源互不影响。
// ② 链路那一套(Link 只填空位)对它们**完全不适用**:送一份"四格齐全"的
//    LinkData 进来,灯位那一格也不许变(Buzzer/Link 的载荷里根本没有灯)。
void test_new_fields_do_not_touch_existing_priority(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  feedLights(svc, kVanLightLeft | kVanLightLowBeam, 0x00, t);
  feedVin(svc, "VF32DNFUR2W005450", t);
  const VehicleState st = svc.update(t);

  // 灯位到了(来源 Van),而车速/转速/水温/进气**一个都没被这几帧改掉**:
  // 全是 Sim(本机没有 OBD、也没有 0x824 帧) —— 这就是"抢不走"的正面证据。
  TEST_ASSERT_TRUE(svc.status().indicator_left == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().vin == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Sim);

  // 反过来:只有 0x824 帧时,灯位那七格一个都不许被标成 Van
  VehicleDataService svc2(nullptr);
  svc2.begin();
  VanPacket sp{};
  sp.iden = VanSource::kSpeedIden;
  sp.len = 7;
  sp.data[0] = 0x18;
  sp.data[1] = 0xF8;
  sp.data[2] = 0x0A;            // 10 个计数 ⇒ 25.6 km/h
  sp.rx_ms = t;
  svc2.onVanPacket(sp);
  svc2.update(t);
  TEST_ASSERT_TRUE(svc2.status().speed == FieldSource::Van);
  TEST_ASSERT_TRUE(svc2.status().indicator_left == FieldSource::None);
  TEST_ASSERT_TRUE(svc2.status().hazard == FieldSource::None);
  TEST_ASSERT_TRUE(svc2.status().position_lamp == FieldSource::None);
  TEST_ASSERT_TRUE(svc2.status().low_beam == FieldSource::None);
  TEST_ASSERT_TRUE(svc2.status().door == FieldSource::None);
  TEST_ASSERT_TRUE(svc2.status().vin == FieldSource::None);
}

// ============================================================
// 八、告警层要用到的三个判据(在**数据层这一侧**先钉住)
// ============================================================
// ★ 这三条不是重复测 alerts.cpp(那边有独立的去抖/限速/静音用例),
//   而是钉住"数据层送给告警层的三个输入是对的口径" ——
//   接口两侧各钉一次,才不会出现"数据层改了口径、告警层还在按旧口径算"。
void test_alert_inputs_from_van(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  // ① 双闪:两位都置位(hazard=true)⇒ 这是"人主动按的",不是"转向灯忘关"
  feedLights(svc, 0x0Cu, 0x00, t);
  const VehicleState hz = svc.update(t);
  TEST_ASSERT_TRUE(hz.indicator_left && hz.indicator_right);
  TEST_ASSERT_TRUE(hz.hazard);          // ← alerts 用它把"忘关"那条排除掉

  // ② 车速:由 kSpeedScale 推导(不写 km/h 字面量)
  VanPacket sp{};
  sp.iden = VanSource::kSpeedIden;
  sp.len = 7;
  sp.data[0] = 0x18;
  sp.data[1] = 0xF8;                    // 799 rpm
  sp.data[2] = 0x32;                    // 50 个计数
  sp.rx_ms = t;
  svc.onVanPacket(sp);
  const VehicleState st = svc.update(t);
  TEST_ASSERT_EQUAL_FLOAT(50.0f * VanSource::kSpeedScale, st.speed_kmh);

  // ③ 转速:同一个 0x824 帧的 data[0..1] ÷8 = 799 rpm
  TEST_ASSERT_EQUAL_FLOAT(799.0f, st.rpm);
  //    ★ 顺带钉住告警层那条"红区"的**地标数**:5800 与 expression.cpp 的
  //      `kRpmRedlineFrom`(那里是 private 的 static,外面读不到)是**同一个数**。
  //      两边各自记一份是有意的(表情档位 vs 告警阈值将来可能各自调),
  //      但今天必须相等 —— 谁调歪了,这里红。
  TEST_ASSERT_EQUAL_FLOAT(5800.0f, AlertsConfig{}.redline_rpm);
}

void register_van_fields_tests(void) {
  RUN_TEST(test_lights_absent_keeps_defaults_and_no_source);
  RUN_TEST(test_light_bitfield_decoding);
  RUN_TEST(test_indicator_hold_window);
  RUN_TEST(test_door_activity_only);
  RUN_TEST(test_vin_decoding);
  RUN_TEST(test_vin_rejects_non_printable_and_short);
  RUN_TEST(test_short_light_frame_is_ignored);
  RUN_TEST(test_new_fields_do_not_touch_existing_priority);
  RUN_TEST(test_alert_inputs_from_van);
}
