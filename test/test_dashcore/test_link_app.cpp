// 双板链路协议 v1 —— **数据接线**用例（§1.2 ③ / §3 / §5 + data_service 的第五档）
//
// 这一组回答四个问题（都是"编译期看不出来、只能跑"的）：
//   ① §3 的 2 位来源编码与 `FieldSource` 是不是一一对上（对不上就是**静默错位**：
//      从板会把"假数据"当"真值"显示，屏幕上没有任何提示）；
//   ② 量纲换算（rpm×8 / 车速÷2.56 / 温度+40）往返是否无损、越界是否被钳住；
//   ③ 主板侧的 DATA 节奏是不是真的"跟随 0x824 到达"（§3 明写**不另建定时器**）；
//   ④ ★ **本轮唯一改 data_service 的地方**：新增的 Link 档会不会动到既有优先级
//      （车速 Van > Obd > Sim、转速 Obd > Van > Sim、水温/进气 Obd > Sim）。
//
//   ⑤ 最后三条是端到端的：主板打包 → 假 PHY → 从板收 → 喂进从板的 data_service →
//      断言值真的变成了 Link。这把"两块板之间的那一截"整条串起来跑了一遍。
#include <unity.h>

#include <string.h>

#include "data_service.h"
#include "fake_link_phy.h"
#include "link_app.h"
#include "link_frame.h"
#include "link_msg.h"
#include "link_rx.h"
#include "link_tx.h"
#include "vehicle_state.h"

using namespace dashlink;

namespace {

// 造一个"主板快照"：四个标量都写在量纲换算**正好无损**的值上，免得断言里出现浮点毛刺。
//   rpm 1000      → raw = 8000（×8，无损）
//   车速 64.0     → raw = 25（64/2.56，无损）
//   水温 90.0     → raw = 130（+40）
//   进气 25.0     → raw = 65
VehicleState masterSnapshot() {
  VehicleState st;
  st.rpm = 1000.0f;
  st.speed_kmh = 64.0f;
  st.coolant_c = 90.0f;
  st.intake_c = 25.0f;
  return st;
}

DataSourceStatus masterSources() {
  DataSourceStatus s;
  s.rpm = FieldSource::Van;
  s.speed = FieldSource::Van;
  s.coolant = FieldSource::Obd;
  s.intake = FieldSource::Obd;
  return s;
}

}  // namespace

// ------------------------------------------------------------
// ① 来源编码（§3 的 2 位 flags）
// ------------------------------------------------------------
// ★ 这条对不上不会有任何编译期信号：`Src` 与 `FieldSource` 的数值一旦错位，
//   从板就会把"Van 真值"当成"Sim 假数据"（或反过来）—— 屏幕上完全看不出来。
static void test_link_app_src_matches_field_source(void) {
  TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)Src::None);
  TEST_ASSERT_EQUAL_UINT8(1u, (uint8_t)Src::Sim);
  TEST_ASSERT_EQUAL_UINT8(2u, (uint8_t)Src::Obd);
  TEST_ASSERT_EQUAL_UINT8(3u, (uint8_t)Src::Van);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::None, (uint8_t)Src::None);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Sim, (uint8_t)Src::Sim);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Obd, (uint8_t)Src::Obd);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)Src::Van);

  // 双向映射（前四档）
  const FieldSource fs[4] = {FieldSource::None, FieldSource::Sim, FieldSource::Obd,
                             FieldSource::Van};
  for (int i = 0; i < 4; ++i) {
    const Src s = fieldSourceToSrc(fs[i]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)i, (uint8_t)s);
    TEST_ASSERT_TRUE(srcToFieldSource(s) == fs[i]);
  }
  // ★ 第五档（Link）**编不进 2 位**：按 Sim 编。
  //   为什么不是 Van：从板连收发器都没有（§0），报 Van 等于谎报"我本地听到了总线"。
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::Sim, (uint8_t)fieldSourceToSrc(FieldSource::Link));
  TEST_ASSERT_EQUAL_INT(4, (int)FieldSource::Link);   // 数值口径（见 data_service.h）
  TEST_ASSERT_TRUE(FieldSource::Link != FieldSource::Van);
}

// ------------------------------------------------------------
// ② 量纲（§3 的载荷表 + link_msg.h 的换算）
// ------------------------------------------------------------
static void test_link_app_data_pack_unpack_roundtrip(void) {
  const VehicleState st = masterSnapshot();
  const DataMsg m = packLinkData(st, masterSources());

  // 逐字段对上 §3 的量纲
  TEST_ASSERT_EQUAL_UINT16(8000u, m.rpm_raw);       // 1000 rpm × 8
  TEST_ASSERT_EQUAL_UINT8(25u, m.speed_raw);        // 64 km/h ÷ 2.56
  TEST_ASSERT_EQUAL_UINT8(130u, m.coolant_raw);     // 90℃ + 40
  TEST_ASSERT_EQUAL_UINT8(65u, m.intake_raw);       // 25℃ + 40

  // flags 的位号（§3：rpm=bit7..6、speed=bit5..4、coolant=bit3..2、intake=bit1..0）
  // 期望：rpm=3(Van) speed=3(Van) coolant=2(Obd) intake=2(Obd)
  //       ⇒ 11 11 10 10b = 0xFA
  // ★ 这个字面量是**照着 §3 的位号手算**出来的，不是从实现里抄的 ——
  //   抄实现就失去了用例的意义（位号写反时两边一起错、自己验自己）。
  TEST_ASSERT_EQUAL_HEX8(0xFAu, m.flags);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::Van, (uint8_t)dataFlagsGet(m.flags, kFieldRpm));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::Van, (uint8_t)dataFlagsGet(m.flags, kFieldSpeed));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::Obd, (uint8_t)dataFlagsGet(m.flags, kFieldCoolant));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::Obd, (uint8_t)dataFlagsGet(m.flags, kFieldIntake));

  // 解回来：物理量与来源都无损（这几个值刻意选在换算的无损点上）
  const LinkData d = unpackDataToLinkData(m, 12345u);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, d.rpm);
  TEST_ASSERT_EQUAL_FLOAT(64.0f, d.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(90.0f, d.coolant_c);
  TEST_ASSERT_EQUAL_FLOAT(25.0f, d.intake_c);
  TEST_ASSERT_TRUE(d.rpm_src == FieldSource::Van);
  TEST_ASSERT_TRUE(d.speed_src == FieldSource::Van);
  TEST_ASSERT_TRUE(d.coolant_src == FieldSource::Obd);
  TEST_ASSERT_TRUE(d.intake_src == FieldSource::Obd);
  TEST_ASSERT_EQUAL_UINT32(12345u, d.rx_ms);
}

// 越界不许回绕（与 link_msg.cpp 的钳制一致）—— 负值/NaN ⇒ 0，超上限 ⇒ 字段上限
static void test_link_app_data_pack_clamps(void) {
  VehicleState st;
  st.rpm = -100.0f;            // 负转速不该变成一个大正数
  st.speed_kmh = 1e9f;         // 超上限 ⇒ 255 计数（652.8 km/h）
  st.coolant_c = -100.0f;      // ≤ -40℃ ⇒ 0
  st.intake_c = 1000.0f;       // ≥ 215℃ ⇒ 255
  const DataMsg m = packLinkData(st, masterSources());
  TEST_ASSERT_EQUAL_UINT16(0u, m.rpm_raw);
  TEST_ASSERT_EQUAL_UINT8(255u, m.speed_raw);
  TEST_ASSERT_EQUAL_UINT8(0u, m.coolant_raw);
  TEST_ASSERT_EQUAL_UINT8(255u, m.intake_raw);
}

// ------------------------------------------------------------
// ③ DATA 的节奏（§3：跟随 0x824 到达，**不另建定时器**）
// ------------------------------------------------------------
static void test_link_app_data_sender_follows_snapshot(void) {
  DataSender ds;
  const VehicleState st = masterSnapshot();
  const DataSourceStatus src = masterSources();
  DataMsg m;

  // ★ 判据是"**快照时刻变了没有**"，不是"now 走到哪了" —— 所以下面的 now_ms
  //   只用来模拟主循环一圈一圈往前跑（真实里它每次都不一样），
  //   而 snapshot_ms 才是 0x824 的到达时刻（≈80 次/秒）。
  TEST_ASSERT_TRUE(ds.due(1000u, 1000u, st, src, &m));    // 第一份快照 ⇒ 发
  TEST_ASSERT_FALSE(ds.due(1001u, 1000u, st, src, &m));   // 主循环又转了一圈，快照没变 ⇒ 不发
  TEST_ASSERT_FALSE(ds.due(1002u, 1000u, st, src, &m));
  TEST_ASSERT_FALSE(ds.due(1003u, 1000u, st, src, &m));
  TEST_ASSERT_EQUAL_UINT32(1u, ds.sent());

  TEST_ASSERT_TRUE(ds.due(1004u, 1004u, st, src, &m));    // 新一帧 0x824 ⇒ 发
  TEST_ASSERT_FALSE(ds.due(1005u, 1004u, st, src, &m));   // 同一帧不再发
  TEST_ASSERT_EQUAL_UINT32(2u, ds.sent());

  // 一圈里连着好几帧 0x824（被 LVGL 拖慢过）：第一份发出去，同一份**不再重发**
  // —— 这就是"不补发突发"（§1.3）。
  TEST_ASSERT_TRUE(ds.due(1005u, 1005u, st, src, &m));
  TEST_ASSERT_FALSE(ds.due(1005u, 1005u, st, src, &m));
  TEST_ASSERT_EQUAL_UINT32(3u, ds.sent());
  TEST_ASSERT_EQUAL_UINT32(1005u, ds.lastSentMs());
  TEST_ASSERT_TRUE(ds.due(1005u, 1006u, st, src, &m));    // 快照时刻变了 ⇒ 发
  TEST_ASSERT_FALSE(ds.due(1006u, 1006u, st, src, &m));   // 同一份 ⇒ 不发
  TEST_ASSERT_EQUAL_UINT32(4u, ds.sent());
  TEST_ASSERT_TRUE(ds.due(1026u, 1026u, st, src, &m));    // §3 的 50 Hz 节奏：下一格
  TEST_ASSERT_EQUAL_UINT32(5u, ds.sent());
  TEST_ASSERT_EQUAL_UINT32(1026u, ds.lastSentMs());

  // ★ 没有 VAN 的那一路（snapshot_ms == 0，"车睡着、VAN 没帧"）：**照发** ——
  //   从板上没有本地源，不发它就只剩 Sim 假数据了（§3 把 DATA 归到 TICK 那三档）。
  //   速率由主循环周期 + ③ 的限速兜着；真发快了 LinkTx 的环满会整帧丢（§1.2 ②）。
  DataSender ds2;
  TEST_ASSERT_TRUE(ds2.due(2000u, 0u, st, src, &m));
  TEST_ASSERT_TRUE(ds2.due(2020u, 0u, st, src, &m));      // 20 ms 后：新时刻 ⇒ 再发
  TEST_ASSERT_EQUAL_UINT32(2u, ds2.sent());
  // 同一时刻重复调用（同一个主循环圈里被调两次）⇒ 靠 min-interval 兜住，不会双发
  ds2.setMinIntervalMs(20u);
  TEST_ASSERT_FALSE(ds2.due(2020u, 0u, st, src, &m));
  TEST_ASSERT_EQUAL_UINT32(2u, ds2.sent());
}

// 可选的降频下限：丢的是**中间那些快照**，不是排队补发（§1.2 ③ 的精神）
static void test_link_app_data_sender_min_interval(void) {
  DataSender ds;
  ds.setMinIntervalMs(100u);
  const VehicleState st = masterSnapshot();
  const DataSourceStatus src = masterSources();
  DataMsg m;

  TEST_ASSERT_TRUE(ds.due(0u, 0u, st, src, &m));          // 第一帧：发过没有 = false ⇒ 不判限速
  // 50 ms 后来了新快照：窗口内（50 - 0 < 100）⇒ 不发。
  TEST_ASSERT_FALSE(ds.due(50u, 50u, st, src, &m));
  // ★ 关键：被丢掉的那份快照**不许**把窗口往后推 —— 窗口永远从"上一份**发出去的**"
  //   快照算起。否则 min=100 会变成"每来一份快照就再锁 100 ms"，节流就成了自锁。
  TEST_ASSERT_FALSE(ds.due(90u, 90u, st, src, &m));       // 90 - 0 = 90 < 100 ⇒ 仍不发
  TEST_ASSERT_TRUE(ds.due(110u, 110u, st, src, &m));      // 110 - 0 = 110 ≥ 100 ⇒ 发
  TEST_ASSERT_EQUAL_UINT32(2u, ds.sent());
  // 窗口过了之后发的是**当下最新**的那一份（110 那份），不是被丢掉的那几份
  TEST_ASSERT_EQUAL_UINT32(110u, ds.lastSentMs());
  // 复位后一切从头开始（reset 也要把限速窗口清掉，不然重启后第一帧会被莫名挡掉）
  ds.reset();
  TEST_ASSERT_TRUE(ds.due(1u, 1u, st, src, &m));
  TEST_ASSERT_EQUAL_UINT32(1u, ds.sent());
}

// ★ "上电那一帧"的哨兵那条坑：快照时刻 0 是**合法**值（millis() 从复位起算，
//   第一帧 0x824 完全可能落在 0~20 ms 内），所以"发过没有"必须是**显式**标志。
//   拿 mLastUpdateMs == 0 当哨兵时，限速窗口会少判一次 —— 板上只表现为"偶尔多发
//   一帧"，几乎不可能靠肉眼发现，所以单开一条用例。
static void test_link_app_data_sender_zero_stamp_is_not_a_sentinel(void) {
  DataSender ds;
  ds.setMinIntervalMs(100u);
  const VehicleState st = masterSnapshot();
  const DataSourceStatus src = masterSources();
  DataMsg m;

  TEST_ASSERT_TRUE(ds.due(0u, 0u, st, src, &m));    // 上电第一帧（快照时刻 = 0）
  TEST_ASSERT_FALSE(ds.due(0u, 0u, st, src, &m));   // 立刻再来一次：窗口内，不许再发
  TEST_ASSERT_FALSE(ds.due(50u, 50u, st, src, &m)); // 50-0 = 50 < 100
  TEST_ASSERT_EQUAL_UINT32(1u, ds.sent());
  TEST_ASSERT_TRUE(ds.due(100u, 100u, st, src, &m)); // 100-0 = 100 ≥ 100
  TEST_ASSERT_EQUAL_UINT32(2u, ds.sent());
}

// ------------------------------------------------------------
// ④ ★ 本轮唯一改 data_service 的地方：Link 档**不许**动既有优先级
// ------------------------------------------------------------
// 这一条是"没动既有语义"的**正面**证据：主板（Van/Obd）先把字段占住，从板收到
// 链路 DATA 之后，那些字段的 fieldSource **必须一个都不变**。
static void test_link_source_never_beats_van_or_obd(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  // VAN 广播进来一帧：车速 100 计数、转速 0x18F8（§van_source.h 的 7 字节帧）
  VanPacket p{};
  p.iden = VanSource::kSpeedIden;
  p.len = 7;
  p.data[0] = 0x18; p.data[1] = 0xF8;   // 799 rpm
  p.data[2] = 0x64;                     // 100 计数 = 100 × 2.56 km/h
  p.rx_ms = t;
  svc.onVanPacket(p);
  VehicleState st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Van);
  const float van_speed = st.speed_kmh;
  const float van_rpm = st.rpm;

  // 链路来一份**不一样**的快照（车速 5 km/h、转速 6000 rpm，来源全标 Van）
  LinkData d;
  d.speed_kmh = 5.0f;
  d.rpm = 6000.0f;
  d.coolant_c = 111.0f;
  d.intake_c = 44.0f;
  d.speed_src = FieldSource::Van;
  d.rpm_src = FieldSource::Van;
  d.coolant_src = FieldSource::Van;
  d.intake_src = FieldSource::Van;
  d.rx_ms = t;
  svc.applyLinkData(d);
  st = svc.update(t);

  // ★ 车速与转速仍然归 VAN（链路一点都不许抢）
  TEST_ASSERT_TRUE_MESSAGE(svc.status().speed == FieldSource::Van,
                           "链路的 DATA 不许压过本机 VAN 的车速（优先级 Van > Link）");
  TEST_ASSERT_TRUE_MESSAGE(svc.status().rpm == FieldSource::Van,
                           "链路的 DATA 不许压过本机 VAN 的转速（既有口径 Van 优先）");
  TEST_ASSERT_EQUAL_FLOAT(van_speed, st.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(van_rpm, st.rpm);

  // 水温/进气：这两项 VAN 上没有（§0），所以它们**该**被链路接手（本来要落 Sim）
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Link);
  TEST_ASSERT_EQUAL_FLOAT(111.0f, st.coolant_c);
  TEST_ASSERT_EQUAL_FLOAT(44.0f, st.intake_c);
}

// 从板（**没有任何本地源**）：链路 DATA 接手全部四格；且 3 秒不新鲜后逐字段回退 Sim。
// ★ 3 秒这条沿用 data_service 既有规则（kStaleMs），没有为链路新开一套。
static void test_link_source_fills_sim_and_falls_back_after_3s(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;
  svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);

  LinkData d;
  d.speed_kmh = 64.0f;
  d.rpm = 1000.0f;
  d.coolant_c = 90.0f;
  d.intake_c = 25.0f;
  d.speed_src = FieldSource::Van;   // 主板说"这是 VAN 真值"
  d.rpm_src = FieldSource::Van;
  d.coolant_src = FieldSource::Van;
  d.intake_src = FieldSource::Van;
  d.rx_ms = t;
  svc.applyLinkData(d);
  VehicleState st = svc.update(t);

  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Link);
  TEST_ASSERT_EQUAL_FLOAT(64.0f, st.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, st.rpm);
  TEST_ASSERT_EQUAL_FLOAT(90.0f, st.coolant_c);
  TEST_ASSERT_EQUAL_FLOAT(25.0f, st.intake_c);

  // 每字段来源跟着数据一起过来了 ⇒ data_service 的 status 里能看出"真值还是假数据"
  TEST_ASSERT_EQUAL_STRING("link", fieldSourceName(svc.status().speed));

  // 链路静默 3.5 秒（§3：同 TICK 那三档，>3 s ⇒ 回退 Sim）⇒ 四格全回 Sim。
  // ★ 这里刻意**不**清空已显示的数据（§2：宁可显示旧值，也不要闪 0）——
  //   回退的是"来源"，值本身由 Sim 覆盖（与既有 Van/Obd 的行为完全一致）。
  t += 3500;
  svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Sim);
}

// 某一格标了 None（= 这一格没有有效值，§3 的 2 位编码里 0 的含义）⇒ 该格**不覆盖**。
// 这一条防的是"flags 解错位之后整片字段被 0 覆盖"那类静默事故。
static void test_link_source_none_field_is_not_applied(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;
  const VehicleState sim = svc.update(t);   // 先拿一份 Sim 的基准

  LinkData d;
  d.speed_kmh = 123.0f;
  d.rpm = 5000.0f;
  d.coolant_c = 200.0f;
  d.intake_c = 200.0f;
  d.speed_src = FieldSource::None;   // 车速这一格没有有效值
  d.rpm_src = FieldSource::None;
  d.coolant_src = FieldSource::None;
  d.intake_src = FieldSource::None;
  d.rx_ms = t;
  svc.applyLinkData(d);
  const VehicleState st = svc.update(t);

  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);   // 四格都没被覆盖
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Sim);
  TEST_ASSERT_EQUAL_FLOAT(sim.speed_kmh, st.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(sim.rpm, st.rpm);
  TEST_ASSERT_EQUAL_FLOAT(sim.coolant_c, st.coolant_c);
  TEST_ASSERT_EQUAL_FLOAT(sim.intake_c, st.intake_c);
}

// ★ VAN 的既有优先级也不能被链路动到。
//   为什么值得单独一条：转速那一格在既有实现里是"Obd > Van > Sim"（与车速相反），
//   而链路这四格**一起**进来，很容易写成"先按链路覆盖、再让 OBD 压过来"——
//   那样在从板上是等价的，但在**同时接了 OBD 的板子**上就会改变行为。
//   本轮的写法（只在 status.x == Sim 时接手）从结构上排除了这件事，这里钉住它。
static void test_link_source_does_not_break_van_priority(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  // 链路先来（充满四格）
  LinkData d;
  d.speed_kmh = 10.0f;
  d.rpm = 900.0f;
  d.coolant_c = 30.0f;
  d.intake_c = 31.0f;
  d.speed_src = FieldSource::Van;
  d.rpm_src = FieldSource::Van;
  d.coolant_src = FieldSource::Van;
  d.intake_src = FieldSource::Van;
  d.rx_ms = t;
  svc.applyLinkData(d);
  VehicleState st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Link);

  // VAN 再进来：车速/转速该被 VAN 接手（Van > Link），水温/进气 VAN 上没有 ⇒ 保持 Link
  VanPacket p{};
  p.iden = VanSource::kSpeedIden;
  p.len = 7;
  p.data[0] = 0x10; p.data[1] = 0x00;   // 0x1000 = 4096 计数 × 0.125 = 512 rpm
  p.data[2] = 0x0A;                     // 10 计数
  p.rx_ms = t;
  svc.onVanPacket(p);
  st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Link);   // 水温没有 VAN 源
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Link);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, st.coolant_c);
  // VAN 的转速值确实盖掉了链路的
  TEST_ASSERT_EQUAL_FLOAT(4096.0f * VanSource::kRpmScale, st.rpm);
}

// ------------------------------------------------------------
// ⑤ 端到端：主板打包 → 假 PHY → 从板收 → 喂进从板的 data_service
// ------------------------------------------------------------
// 这一条把"两块板之间的那一截"整条串起来（真实 UART 之外的部分全在这条路径上）。
// ★ 用的 PHY 是 test/test_dashcore/fake_link_phy.h（可编程故障注入），
//   所以这条覆盖的是"链路正常"这一路 —— 故障那部分由 test_link_phy.cpp 管。
static void test_link_app_end_to_end_master_to_slave_data(void) {
  FakeLinkPhy phy_a, phy_b;
  phy_a.connect(&phy_b);      // A = 主板侧，B = 从板侧

  // ---- 主板侧 ----
  LinkTx tx;
  DataSender ds;
  const VehicleState snapshot = masterSnapshot();
  const DataSourceStatus sources = masterSources();
  DataMsg dm;
  TEST_ASSERT_TRUE(ds.due(1000u, 0u, snapshot, sources, &dm));

  uint8_t payload[kDataLen];
  TEST_ASSERT_TRUE(packData(dm, payload));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Data, payload, kDataLen, kRoleMaster));
  while (tx.queued() > 0u) tx.pump(phy_a);

  // ---- 从板侧 ----
  LinkRx rx;
  rx.setLocalRole(kRoleSlave);
  LinkTime lt;
  lt.reset();
  VehicleDataService svc(nullptr);
  svc.begin();
  const uint32_t t = 2000;
  svc.update(t);

  Frame f;
  TEST_ASSERT_TRUE(rx.poll(phy_b, &f));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Data, f.type);
  TEST_ASSERT_EQUAL_UINT8(kRoleMaster, f.role);

  LinkData ld;
  TEST_ASSERT_TRUE(handleInbound(f, &lt, t, &ld));
  svc.applyLinkData(ld);
  const VehicleState st = svc.update(t);

  // ---- 从板的显示数据层：四个值都到位，且来源是 Link ----
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, st.rpm);
  TEST_ASSERT_EQUAL_FLOAT(64.0f, st.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(90.0f, st.coolant_c);
  TEST_ASSERT_EQUAL_FLOAT(25.0f, st.intake_c);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Link);

  // ★ 角色的"接收侧那一半"也顺手验了：DATA 只影响数据新鲜度，**不动时间基准**
  //   （§3：DATA 的超时策略与 TICK 同档，但时间基准仍由 TICK 定）。
  TEST_ASSERT_FALSE(lt.tickSeen());
  TEST_ASSERT_EQUAL_UINT32(0u, lt.dataAgeMs());
}

// 端到端（TICK）：从板的时基要被喂上，并且状态切到 Locked（§4 的三级超时）
static void test_link_app_end_to_end_tick_feeds_time(void) {
  FakeLinkPhy phy_a, phy_b;
  phy_a.connect(&phy_b);
  LinkTx tx;
  TickMsg tm;
  tm.tick_ms = 5000u;
  tm.seq = 7u;
  uint8_t payload[kTickLen];
  TEST_ASSERT_TRUE(packTick(tm, payload));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Tick, payload, kTickLen, kRoleMaster));
  while (tx.queued() > 0u) tx.pump(phy_a);

  LinkRx rx;
  rx.setLocalRole(kRoleSlave);
  LinkTime lt;
  lt.reset();
  Frame f;
  TEST_ASSERT_TRUE(rx.poll(phy_b, &f));
  LinkData ld;
  TEST_ASSERT_TRUE(handleInbound(f, &lt, 6000u, &ld));   // 本机 6000ms 收到 tick_ms=5000

  TEST_ASSERT_TRUE(lt.tickSeen());
  TEST_ASSERT_EQUAL_UINT8(7u, lt.lastSeq());
  TEST_ASSERT_TRUE(lt.state() == LinkTimeState::Locked);
  TEST_ASSERT_EQUAL_INT32(5000 - 6000, lt.offsetMs());
  // 20 ms 后的下一帧 TICK ⇒ 仍然有基准（§3：>100 ms 才失基准）
  lt.update(6000u + 20u);
  TEST_ASSERT_TRUE(lt.haveBasis());
}

// 角色冲突（§5 ①）：从板收到 ROLE==0（自己也是从板）的帧 ⇒ **丢帧 + 计数**。
// 这条在端到端路径上再钉一次（test_link_phy.cpp 里已有帧层的版本）——
// 因为"两块板刷了同一份固件"是**最容易发生**的现场事故（§7 的失败模式 6）。
static void test_link_app_end_to_end_role_conflict_drops(void) {
  FakeLinkPhy phy_a, phy_b;
  phy_a.connect(&phy_b);
  LinkTx tx;
  uint8_t payload[kDataLen] = {0};
  // 故意用**从板**角色发 DATA（= 两块板刷了同一份从板固件）
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Data, payload, kDataLen, kRoleSlave));
  while (tx.queued() > 0u) tx.pump(phy_a);

  LinkRx rx;
  rx.setLocalRole(kRoleSlave);
  Frame f;
  TEST_ASSERT_FALSE(rx.poll(phy_b, &f));            // 被丢了
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().role_conflict);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().frames_ok);
  TEST_ASSERT_TRUE(rx.roleConflictSeen());
}

void register_link_app_tests(void) {
  RUN_TEST(test_link_app_src_matches_field_source);
  RUN_TEST(test_link_app_data_pack_unpack_roundtrip);
  RUN_TEST(test_link_app_data_pack_clamps);
  RUN_TEST(test_link_app_data_sender_follows_snapshot);
  RUN_TEST(test_link_app_data_sender_min_interval);
  RUN_TEST(test_link_app_data_sender_zero_stamp_is_not_a_sentinel);
  RUN_TEST(test_link_source_never_beats_van_or_obd);
  RUN_TEST(test_link_source_fills_sim_and_falls_back_after_3s);
  RUN_TEST(test_link_source_none_field_is_not_applied);
  RUN_TEST(test_link_source_does_not_break_van_priority);
  RUN_TEST(test_link_app_end_to_end_master_to_slave_data);
  RUN_TEST(test_link_app_end_to_end_tick_feeds_time);
  RUN_TEST(test_link_app_end_to_end_role_conflict_drops);
}
