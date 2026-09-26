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
// ★ 接线常量（43 发 / 44 收 / UART0 / 115200）的唯一出处 —— ⑥ 那条从板侧用例要读它。
//   它同时也是"两个角色同一组脚"那份判据的**代码出处**（见 link_phy_pins.h 文件头）。
#include "link_phy_pins.h"
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

// ------------------------------------------------------------
// ★★ 生产主板上那条**实测出来的**速率下限（2026-09-25）——"没有 VAN 时不许满线速"
// ------------------------------------------------------------
// 背景（上板实测，写在 main.cpp 的 `kLinkDataMinIntervalMs` 那一段）：`due()` 对
// `snapshot_ms == 0`（本机一帧 0x824 都没收到过）有一条**刻意的**"照发"口径 ——
// 它把 `now_ms` 当快照时刻，于是**主循环每一圈都能发一帧**。2.8C 上主循环 ≈900 圈/秒
// ⇒ 实测 COM6 上解出来是 **DATA 946 帧/秒、12288 B/s = 115200 8N1 的满线速**。
// 生产主板现在显式 `setMinIntervalMs(12)`（= 契约 §3 的 ≈80 Hz 那一档）。
//
// 本用例把**那一行设定的效果**钉在宿主机上，两个方向都要：
//   ① 满速灌（模拟 1 kHz 的主循环）⇒ 输出被压到 ≈1/12；
//   ② **真实的 0x824 节奏**（12.5 ms 一份新快照）⇒ **一帧都不许丢**
//      （这是"限速会不会误伤正常路径"那一问的答案）。
static void test_link_app_data_sender_min_interval_matches_contract_rate(void) {
  const VehicleState st = masterSnapshot();
  const DataSourceStatus src = masterSources();
  DataMsg m;

  // ---- ① 没有 VAN 快照（snapshot_ms == 0）+ 1 kHz 主循环 ⇒ 被压到 ~80 Hz ----
  {
    DataSender ds;
    ds.setMinIntervalMs(12u);            // = main.cpp 的 kLinkDataMinIntervalMs
    uint32_t t = 0;
    for (int i = 0; i < 1000; ++i) {     // 1 秒、每 1 ms 一圈
      ds.due(t, 0u, st, src, &m);        // snapshot_ms = 0 ⇒ 走"照发"那一条
      t += 1u;
    }
    // 1000 圈里最多 ceil(1000/12)+1 = 84 帧（第一帧不判窗口）
    TEST_ASSERT_TRUE_MESSAGE(ds.sent() <= 84u,
                             "没有 VAN 时 DATA 没有被 12ms 窗口压住（会把 115200 打成满线速）");
    TEST_ASSERT_TRUE_MESSAGE(ds.sent() >= 80u, "12ms 窗口压得太狠了（契约要的是 ≈80 Hz）");
  }

  // ---- ② 真有 0x824（12.5 ms 一份新快照）⇒ 12 ms 窗口一帧都不许丢 ----
  {
    DataSender ds;
    ds.setMinIntervalMs(12u);
    uint32_t t = 0;
    for (int i = 0; i < 80; ++i) {       // 1 秒、80 份快照（≈79.7 Hz 的实测到达率）
      TEST_ASSERT_TRUE_MESSAGE(ds.due(t, t, st, src, &m),
                               "真实的 0x824 节奏被限速窗口误伤了（12ms < 12.5ms 才是对的）");
      t += 12500u / 1000u;               // 12.5 ms
    }
    TEST_ASSERT_EQUAL_UINT32(80u, ds.sent());
  }
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

// ------------------------------------------------------------
// ⑥ ★★ 从板侧**接线口径那一份**（2026-09-25 新增）—— "从板也带真 PHY"之后
//    这一条成了必须有的那份用例：它跑的是**从板镜像**在台面上的实际形状。
// ------------------------------------------------------------
// 为什么需要它（本组用例补的是哪个缺口）：
//   · 在"只有主板有 PHY"的那一版里，从板那一侧**永远是空壳 PHY**，所以
//     "从板收 TICK/DATA 之后做什么"这条路只有上面 ⑤ 那两条**逐帧**用例在看着；
//   · 2026-09-25 起两块板都带真 PHY（同一组脚、同一个 115200），`main.cpp` 的
//     从板分支里多了三件事，每一件都**不会有编译期信号**：`LinkPhyUart::begin()`、
//     `LinkTime::reset()`、以及 loop 里那两行排水（`LinkTx::pump` + `pumpTx`）；
//   · 方向还有一个**很容易搞反**的地方：`LinkTx` 的 payload 是"先入队、再由
//     `pump()` 排进 PHY" —— 只 enqueue 不 pump，链路上一个字节都不会出现
//     （这条正是主板上板时最容易漏的一行）。
//
// 本用例把上面那三件事**按生产形状**跑一遍（主线 = 8 帧 TICK + 1 帧 DATA，
// 用的是 `lib/link` 里既有的 `TickGen` 与 `DataSender`，不是手写的字节），
// 然后钉住：从板侧的 PHY **接线口径**（§0 的 43 发 / 44 收 / UART0 / 115200）
// 与"收到的 DATA 走既有 `FieldSource::Link` 通道"这一条。
static void test_link_slave_side_real_phy_wiring_and_data_channel(void) {
  // ---- ① 接线口径：从板与主板读的是**同一组常量**，不分叉（§0 那条★）
  //     `link_role.h` 的 LINK_ROLE 在本构建（native）里是默认值 0 = 从板，
  //     也就是从板镜像的口径 ⇒ 下面这几条就是从板镜像的编译期事实。
  TEST_ASSERT_EQUAL_UINT8(dashlink::kRoleSlave, dashlink::kLocalRole);
  TEST_ASSERT_EQUAL_INT(43, (int)dashlink::kLinkTxPin);   // 43 发
  TEST_ASSERT_EQUAL_INT(44, (int)dashlink::kLinkRxPin);   // 44 收
  TEST_ASSERT_EQUAL_INT(0, (int)dashlink::kLinkUartPort); // UART0
  TEST_ASSERT_EQUAL_UINT32(115200u, dashlink::kLinkBaud); // §1.1 115200 8N1

  // ---- ② 主线：主板发 8 帧 TICK + 1 帧 DATA ----
  FakeLinkPhy phy_master, phy_slave;
  phy_master.connect(&phy_slave);

  LinkTx tx;
  TickGen tick;          // 与主板镜像用的是同一个类（§3：50 Hz / 20 ms）
  DataSender ds;         // 同上（§3：DATA 跟随 0x824 到达）
  tick.reset(1000u);     // 主板自己的单调毫秒

  const VehicleState snapshot = masterSnapshot();
  const DataSourceStatus sources = masterSources();

  uint32_t sent = 0;
  for (uint32_t i = 0; i <= 7u; ++i) {
    TickMsg tm;
    TEST_ASSERT_TRUE(tick.due(1000u + i * 20u, &tm));
    uint8_t p[kTickLen];
    TEST_ASSERT_TRUE(packTick(tm, p));
    TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Tick, p, kTickLen, kRoleMaster));
    ++sent;
  }
  DataMsg dm;
  TEST_ASSERT_TRUE(ds.due(2000u, 0u, snapshot, sources, &dm));
  uint8_t dp[kDataLen];
  TEST_ASSERT_TRUE(packData(dm, dp));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Data, dp, kDataLen, kRoleMaster));
  ++sent;

  // ★ "只 enqueue 不 pump"这条：排进 PHY **之前**，链路上必须一个字节都没有。
  //   ★ `LinkTx::queued()` 报的是**字节数**（不是帧数）—— 8×TICK(12 B) + 1×DATA(13 B)
  //     = 109 B（帧长用 `frameBytesForLen()` 算，别在用例里手抄 12/13）。
  const uint32_t expect_bytes =
      8u * (uint32_t)frameBytesForLen(kTickLen) + (uint32_t)frameBytesForLen(kDataLen);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)sent, 9u);            // 8 TICK + 1 DATA
  TEST_ASSERT_EQUAL_UINT32(expect_bytes, (uint32_t)tx.queued());
  TEST_ASSERT_EQUAL_INT(0, phy_slave.available());
  TEST_ASSERT_EQUAL_UINT32(0u, phy_slave.wroteBytes);
  while (tx.queued() > 0u) tx.pump(phy_master);
  TEST_ASSERT_EQUAL_UINT32(0u, tx.queued());
  TEST_ASSERT_EQUAL_UINT32(expect_bytes, phy_slave.available());

  // ---- ③ 从板侧：与 main.cpp 的从板分支同形（LinkRx + LinkTime + 数据层）----
  LinkRx rx;
  rx.setLocalRole(dashlink::kLocalRole);   // = kRoleSlave（§5：编译期唯一权威）
  LinkTime lt;
  lt.reset();
  VehicleDataService svc(nullptr);
  svc.begin();
  svc.update(1000u);

  const uint32_t t = 3000u;
  uint32_t ticks_seen = 0;
  bool got_data = false;
  LinkData ld;
  Frame f;
  // ★ 方向/顺序判据：收到的**每**一帧都必须是对端（kRoleMaster）发的 ——
  //   写成 kLocalRole 的话每一帧都会撞上 §5 ① 而被丢（role_conflict 涨、frames_ok 为 0），
  //   这正是 2026-09-23 那次"收了很多字节却 0 帧"的元凶（见 link_role.h 那段）。
  while (rx.poll(phy_slave, &f)) {
    TEST_ASSERT_EQUAL_UINT8(kRoleMaster, f.role);
    if (handleInbound(f, &lt, t, &ld)) {
      if (f.type == (uint8_t)MsgType::Tick) ++ticks_seen;
      if (f.type == (uint8_t)MsgType::Data) { svc.applyLinkData(ld); got_data = true; }
      continue;
    }
    TEST_FAIL_MESSAGE("从板收到了 HELLO/STATUS/EVENT —— 本用例只发了 TICK/DATA");
  }
  lt.update(t);

  TEST_ASSERT_EQUAL_UINT32((uint32_t)sent, rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().role_conflict);   // §5 ① 一次都不许触发
  TEST_ASSERT_EQUAL_UINT32(8u, ticks_seen);
  TEST_ASSERT_TRUE(got_data);
  TEST_ASSERT_TRUE(lt.tickSeen());
  TEST_ASSERT_EQUAL_UINT8(7u, lt.lastSeq());
  TEST_ASSERT_TRUE(lt.state() == LinkTimeState::Locked);

  // ---- ④ ★ 数据仍旧走**既有**的 FieldSource::Link 通道（没有新造数据通路）----
  const VehicleState st = svc.update(t);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, st.rpm);
  TEST_ASSERT_EQUAL_FLOAT(64.0f, st.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(90.0f, st.coolant_c);
  TEST_ASSERT_EQUAL_FLOAT(25.0f, st.intake_c);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Link);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Link);

  // ---- ⑤ 排水那一行（从板 loop() 里的 `g_link_tx.pump()` + `pumpTx()`）：
  //     ★★ 2026-09-27 **本条的期望值改了**（这是本单唯一改动既有期望值的一处，
  //     原因是它钉的那条契约本身变了，不是用例算错了）：
  //       原来写的是"从板今天**不发帧**（§3：TICK/DATA 只由主板发）⇒ 环必须空、
  //       pump 一个字节都不许写出去（写了就是'两个发送方'，§5 的现场事故）"。
  //       而 §3 表的 `0x01` 写的是 HELLO **双向**、`0x30` 的方向写的就是 **B→A** ——
  //       也就是说"从板一个字节都不发"**才是**那个契约缺口（§3 的"双向"当时只兑现了
  //       A→B 那一半）。所以这一段的判据换成"**从板发出去的东西必须是它该发的**"：
  //       · 本用例**没有把它自己的帧排出去过**（它收完就结束，没调 `link_slave_tick`
  //         那一支）⇒ `tx` 这个**局部**对象仍旧是空的，`pump` 仍旧一个字节都不写
  //         （这一半原样保留：`LinkTx` 自己不会凭空产生字节）；
  //       · 而"从板会发什么、什么时候发、发多少"改由**新的一组用例**钉：
  //         `test_link_slave_tx.cpp`（HELLO 5 s/ack 停发、STATUS 2 Hz/逐字节载荷、
  //         发不挤占收、发送预算）。这样两边各管一段、不留空档。
  TEST_ASSERT_EQUAL_UINT32(0u, tx.queued());
  phy_slave.clearCounters();
  tx.pump(phy_slave);
  TEST_ASSERT_EQUAL_UINT32(0u, phy_slave.writeCalls);
  TEST_ASSERT_EQUAL_UINT32(0u, phy_slave.wroteBytes);
}

// 角色冲突的**另一半**面（§5 ①）：上面那条是从板收到"从板帧"（两块板都刷了
// 从板镜像）。这一条是**主板收到"主板帧"**（两块板都刷了主板镜像）——
// 同一份判据、另一个方向，也是 §7 失败模式表第 6 行。
static void test_link_app_end_to_end_role_conflict_master_side_drops(void) {
  FakeLinkPhy phy_a, phy_b;
  phy_a.connect(&phy_b);
  LinkTx tx;
  TickMsg tm;
  tm.tick_ms = 1000u;
  tm.seq = 1u;
  uint8_t payload[kTickLen];
  TEST_ASSERT_TRUE(packTick(tm, payload));
  // 故意用**主板**角色发 TICK（= 两块板刷了同一份主板固件）
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Tick, payload, kTickLen, kRoleMaster));
  while (tx.queued() > 0u) tx.pump(phy_a);

  LinkRx rx;
  rx.setLocalRole(kRoleMaster);
  Frame f;
  TEST_ASSERT_FALSE(rx.poll(phy_b, &f));            // 被丢了（§5 ①：丢帧 + 计数）
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().role_conflict);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().crc_err);  // ★ 别去怀疑 PHY/分帧：CRC 一个都没错
  TEST_ASSERT_TRUE(rx.roleConflictSeen());
}

// ------------------------------------------------------------
// ⑦ VAN 原始帧转发（2026-09-27，`0x21 VANRAW`）—— 主板发、从板自己解
// ------------------------------------------------------------
// 这一组的判据分三层（每层都能单独失败，所以分开写）：
//   ① **搬运无损**：`VanPacket` → `VanRawMsg` → 打包 → 解包 → `VanPacket`，
//      逐字节相等（含 cmd/ack/fcs_ok 三段与 iden 的 12 位）；
//   ② **队列的边界**：整帧进出、满了丢整帧、超长（VIN）单独计数且**绝不截断**，
//      以及**跨环回绕**后取出来的记录仍然完整（这是环形缓冲最经典的错法）；
//   ③ **端到端**：主板把原始帧发出去 → 假 PHY → 从板解回来 → 喂进**从板的**
//      `VanSource` ⇒ 从板自己解出了车速/转速（在真板上表现为 `SRC speed=van`
//      而不是 `link`），而链路上**一个 DATA 帧都不需要**。
static VanPacket sampleSpeedPacket(uint16_t iden = 0x824u, uint8_t len = 7u) {
  VanPacket p{};
  p.iden = iden;
  p.cmd = 0x8u;
  p.ack = 0u;
  p.fcs_ok = 1u;
  p.len = len;
  p.data[0] = 0x18;   // 转速 0x18F8 = 6392 ⇒ 799.0 rpm
  p.data[1] = 0xF8;
  p.data[2] = 0x27;   // 车速 39 计数 ⇒ 99.84 km/h
  p.data[3] = 0x10;
  p.data[4] = 0x00;
  p.data[5] = 0x00;
  p.data[6] = 0x4Cu;
  p.rx_ms = 12345u;
  return p;
}

static void test_link_vanraw_packet_roundtrip_is_lossless(void) {
  const VanPacket src = sampleSpeedPacket();
  const VanRawMsg m = vanRawFromPacket(src);
  TEST_ASSERT_EQUAL_HEX16(0x824u, m.iden);
  TEST_ASSERT_EQUAL_HEX8(0x8u, m.cmd);
  TEST_ASSERT_FALSE(m.ack);
  TEST_ASSERT_TRUE(m.fcs_ok);
  TEST_ASSERT_EQUAL_HEX8(7u, m.len);
  uint8_t p[kLenMax];
  const uint8_t n = packVanRaw(m, p);
  TEST_ASSERT_EQUAL_UINT8(11u, n);
  VanRawMsg back;
  TEST_ASSERT_TRUE(unpackVanRaw(p, n, &back));
  const VanPacket dst = unpackVanRawToVanPacket(back, 999u);
  TEST_ASSERT_EQUAL_HEX16(src.iden, dst.iden);
  TEST_ASSERT_EQUAL_HEX8(src.cmd, dst.cmd);
  TEST_ASSERT_EQUAL_HEX8(src.ack, dst.ack);
  TEST_ASSERT_EQUAL_HEX8(src.fcs_ok, dst.fcs_ok);
  TEST_ASSERT_EQUAL_HEX8(src.len, dst.len);
  for (uint8_t i = 0; i < src.len; ++i) TEST_ASSERT_EQUAL_HEX8(src.data[i], dst.data[i]);
  // rx_ms 用的是**接收侧**给的时刻（从板没有主板的 T0，见 link_app.h）
  TEST_ASSERT_EQUAL_UINT32(999u, dst.rx_ms);

  // 坏帧的 `fcs_ok` 也要**原样搬**（丢掉它 => 从板会把主板已判坏的帧当好的用）
  VanPacket bad = src;
  bad.fcs_ok = 0u;
  const VanRawMsg bm = vanRawFromPacket(bad);
  TEST_ASSERT_FALSE(bm.fcs_ok);
  uint8_t bp[kLenMax];
  const uint8_t bn = packVanRaw(bm, bp);
  VanRawMsg bb;
  TEST_ASSERT_TRUE(unpackVanRaw(bp, bn, &bb));
  TEST_ASSERT_FALSE(bb.fcs_ok);
  TEST_ASSERT_EQUAL_HEX8(0u, unpackVanRawToVanPacket(bb, 1u).fcs_ok);
}

// 装不下的一帧（VIN 的 17 字节）：`vanRawFromPacket()` 照实填 len（**不截断**），
// 由队列判"放不下 ⇒ 丢 + 计数"。
static void test_link_vanraw_too_long_is_rejected_not_truncated(void) {
  VanPacket vin{};
  vin.iden = 0xE24u;
  vin.cmd = 0x8u;
  vin.fcs_ok = 1u;
  vin.len = 17u;                 // van_source.h 的 kVanVinLen
  for (uint8_t i = 0; i < 17u; ++i) vin.data[i] = (uint8_t)('A' + i);
  const VanRawMsg m = vanRawFromPacket(vin);
  TEST_ASSERT_EQUAL_HEX8(17u, m.len);            // 原样，不截断
  uint8_t buf[kLenMax];
  memset(buf, 0xEE, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT8(0u, packVanRaw(m, buf));   // 装不下 ⇒ 一个字节都不写
  TEST_ASSERT_EQUAL_HEX8(0xEEu, buf[0]);
  VanRawQueue q;
  TEST_ASSERT_FALSE(q.push(m));
  TEST_ASSERT_EQUAL_UINT32(1u, q.tooLong());
  TEST_ASSERT_EQUAL_UINT32(0u, q.dropped());
  TEST_ASSERT_EQUAL_UINT32(0u, q.pushed());
  TEST_ASSERT_EQUAL_UINT16(0u, q.queuedBytes());
  uint8_t out[kLenMax];
  TEST_ASSERT_EQUAL_UINT8(0u, q.pop(out, kLenMax));   // 环是空的
}

static void test_link_vanraw_queue_order_and_drop_on_full(void) {
  VanRawQueue q;
  // ① 顺序：先进先出，取出来的载荷与 push 进去的逐字节一致
  const uint8_t lens[3] = {7u, 11u, 0u};
  for (uint8_t k = 0; k < 3u; ++k) {
    VanPacket p = sampleSpeedPacket((uint16_t)(0x800u + k), lens[k]);
    p.len = lens[k];
    TEST_ASSERT_TRUE(q.push(vanRawFromPacket(p)));
  }
  TEST_ASSERT_EQUAL_UINT32(3u, q.pushed());
  for (uint8_t k = 0; k < 3u; ++k) {
    uint8_t out[kLenMax];
    const uint8_t n = q.pop(out, kLenMax);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(kVanRawHdrLen + lens[k]), n);
    VanRawMsg m;
    TEST_ASSERT_TRUE(unpackVanRaw(out, n, &m));
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(0x800u + k), m.iden);
  }
  TEST_ASSERT_EQUAL_UINT16(0u, q.queuedBytes());

  // ② 满了丢**整帧**：一条条塞到塞不下为止，被拒的那一帧不许留半截在环里
  VanRawQueue q2;
  uint32_t ok = 0;
  VanPacket p = sampleSpeedPacket();      // 每帧 11 B 载荷
  while (q2.push(vanRawFromPacket(p))) ++ok;
  TEST_ASSERT_EQUAL_UINT32(ok, q2.pushed());
  TEST_ASSERT_EQUAL_UINT32(1u, q2.dropped());
  TEST_ASSERT_TRUE(q2.queuedBytes() <= VanRawQueue::kRingBytes);
  TEST_ASSERT_TRUE((uint32_t)q2.queuedBytes() + 11u > (uint32_t)VanRawQueue::kRingBytes);
  // 环里剩下的那些必须**每一帧都完整**（丢的是最后那一帧，不是把别人截断了）
  uint8_t out[kLenMax];
  uint8_t n;
  uint32_t drained = 0;
  while ((n = q2.pop(out, kLenMax)) != 0u) {
    VanRawMsg m;
    TEST_ASSERT_TRUE(unpackVanRaw(out, n, &m));
    TEST_ASSERT_EQUAL_HEX8(7u, m.len);
    ++drained;
  }
  TEST_ASSERT_EQUAL_UINT32(ok, drained);
  TEST_ASSERT_EQUAL_UINT16(0u, q2.queuedBytes());
}

// ★ 环形缓冲最经典的错法：**跨环尾的那一条记录**。push/pop 各写一遍 head/tail
//   取模，只要有一处算错，前几十条都对、偏偏在绕回那一圈读出乱码 ——
//   而真机上那表现为"偶尔一帧车速乱跳"，几乎不可能定位。
//   本条用"**填到快满、再全排空**"的循环强制读写指针各绕很多圈：
//     · 每轮先填到再也放不下一条（但**一条都不许丢**：填之前先问容量）；
//     · 再整环排空，逐条验"先进先出 + 内容完整"。
//   ★ 第一版写的是"每轮 push 3 / pop 2"（想让它慢慢积压），结果环在 ~40 轮就满了、
//     push 开始返回 false —— 那是**队列的正确行为**，但用例把它当成了失败。
//     教训：这类用例必须自己保证"不越界"，不能指望容量够大。
static void test_link_vanraw_queue_wraps_correctly(void) {
  VanRawQueue q;
  VanPacket p = sampleSpeedPacket();
  uint32_t seq = 0;      // 已 push 的条数
  uint32_t expect = 0;   // 环里**最老**那一条的标记（= 已 pop 的条数）
  const uint8_t rec_max = (uint8_t)(kVanRawHdrLen + kVanRawMaxData);   // 最长记录 16 B
  for (uint32_t round = 0; round < 64u; ++round) {
    while ((uint32_t)q.queuedBytes() + (uint32_t)rec_max <= (uint32_t)VanRawQueue::kRingBytes) {
      p.data[6] = (uint8_t)seq;                // 每条记录带一个不同的标记字节
      p.len = (uint8_t)(7u + (round % 5u));    // 长度也在变（7..11）
      for (uint8_t i = 7u; i < p.len; ++i) p.data[i] = (uint8_t)(0x30u + i);
      TEST_ASSERT_TRUE(q.push(vanRawFromPacket(p)));
      ++seq;
    }
    uint8_t out[kLenMax];
    uint8_t n;
    while ((n = q.pop(out, kLenMax)) != 0u) {
      VanRawMsg m;
      TEST_ASSERT_TRUE(unpackVanRaw(out, n, &m));
      // ★ 先进先出：这一条必须是"环里最老的那一条"，而不是"刚 push 的那一条"
      TEST_ASSERT_EQUAL_HEX8((uint8_t)expect, m.data[6]);
      ++expect;
    }
    TEST_ASSERT_EQUAL_UINT16(0u, q.queuedBytes());   // 排空后一条不剩
  }
  TEST_ASSERT_EQUAL_UINT32(seq, expect);        // 一条不多、一条不少
  TEST_ASSERT_EQUAL_UINT32(seq, q.pushed());
  TEST_ASSERT_EQUAL_UINT32(0u, q.dropped());    // 全程一次都没丢
  TEST_ASSERT_EQUAL_UINT32(0u, q.tooLong());
}

// 端到端：**只用 VANRAW**（一个 DATA 帧都不发）驱动从板的数据层。
// 这正是真板上"从板不接收发器也能有车速/转速/灯位"的那条路。
static void test_link_vanraw_end_to_end_slave_decodes_van_itself(void) {
  FakeLinkPhy phy_a, phy_b;
  phy_a.connect(&phy_b);
  LinkTx tx;
  VanRawQueue q;

  // 主板侧：VAN 来的原始帧 → 队列 → 链路
  VanPacket src = sampleSpeedPacket();
  TEST_ASSERT_TRUE(q.push(vanRawFromPacket(src)));
  uint8_t payload[kLenMax];
  const uint8_t n = q.pop(payload, kLenMax);
  TEST_ASSERT_TRUE(n != 0u);
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::VanRaw, payload, n, kRoleMaster));
  while (tx.queued() > 0u) tx.pump(phy_a);

  // 从板侧：解帧 → 还原 VanPacket → 喂**从板自己的** VanSource
  LinkRx rx;
  rx.setLocalRole(kRoleSlave);
  Frame f;
  TEST_ASSERT_TRUE(rx.poll(phy_b, &f));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::VanRaw, f.type);
  VanRawMsg vm;
  TEST_ASSERT_TRUE(unpackVanRaw(f.payload, f.len, &vm));
  VehicleDataService svc;
  svc.onVanPacket(unpackVanRawToVanPacket(vm, 1000u));
  TEST_ASSERT_TRUE(svc.vanSource().hasSpeed());
  TEST_ASSERT_TRUE(svc.vanSource().hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(99.84f, svc.vanSource().speedKmh());
  TEST_ASSERT_EQUAL_FLOAT(799.0f, svc.vanSource().rpm());
  // 合并进快照之后，这两格的来源是 **Van**（从板自己解出来的），不是 Link
  const VehicleState st = svc.update(1000u);
  TEST_ASSERT_EQUAL_FLOAT(99.84f, st.speed_kmh);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)svc.status().speed);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)svc.status().rpm);

  // 灯位那一帧（0x4FC，11 字节数据）是**最长的能搬的一帧** —— 走到数据层之后，
  // 从板的"只有 VAN 才有"的那几格（转向灯/近光/位置灯）也应当跟着有值。
  VanPacket lights{};
  lights.iden = 0x4FCu;
  lights.cmd = 0xCu;
  lights.fcs_ok = 1u;
  lights.len = 11u;
  lights.data[5] = (uint8_t)(0x04u | 0x40u);   // 左转向 + 近光
  lights.rx_ms = 1100u;
  VanRawQueue q2;
  TEST_ASSERT_TRUE(q2.push(vanRawFromPacket(lights)));
  LinkTx tx2;
  const uint8_t n2 = q2.pop(payload, kLenMax);
  TEST_ASSERT_TRUE(n2 != 0u);
  TEST_ASSERT_TRUE(tx2.enqueueFrame((uint8_t)MsgType::VanRaw, payload, n2, kRoleMaster));
  while (tx2.queued() > 0u) tx2.pump(phy_a);
  Frame f2;
  TEST_ASSERT_TRUE(rx.poll(phy_b, &f2));
  VanRawMsg vm2;
  TEST_ASSERT_TRUE(unpackVanRaw(f2.payload, f2.len, &vm2));
  svc.onVanPacket(unpackVanRawToVanPacket(vm2, 1100u));
  const VehicleState st2 = svc.update(1100u);
  TEST_ASSERT_TRUE(st2.indicator_left);
  TEST_ASSERT_FALSE(st2.indicator_right);
  TEST_ASSERT_TRUE(st2.low_beam);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)svc.status().indicator_left);
}

void register_link_app_tests(void) {
  RUN_TEST(test_link_app_src_matches_field_source);
  RUN_TEST(test_link_app_data_pack_unpack_roundtrip);
  RUN_TEST(test_link_app_data_pack_clamps);
  RUN_TEST(test_link_app_data_sender_follows_snapshot);
  RUN_TEST(test_link_app_data_sender_min_interval);
  RUN_TEST(test_link_app_data_sender_min_interval_matches_contract_rate);
  RUN_TEST(test_link_app_data_sender_zero_stamp_is_not_a_sentinel);
  RUN_TEST(test_link_source_never_beats_van_or_obd);
  RUN_TEST(test_link_source_fills_sim_and_falls_back_after_3s);
  RUN_TEST(test_link_source_none_field_is_not_applied);
  RUN_TEST(test_link_source_does_not_break_van_priority);
  RUN_TEST(test_link_app_end_to_end_master_to_slave_data);
  RUN_TEST(test_link_app_end_to_end_tick_feeds_time);
  RUN_TEST(test_link_app_end_to_end_role_conflict_drops);
  RUN_TEST(test_link_app_end_to_end_role_conflict_master_side_drops);
  RUN_TEST(test_link_slave_side_real_phy_wiring_and_data_channel);
  RUN_TEST(test_link_vanraw_packet_roundtrip_is_lossless);
  RUN_TEST(test_link_vanraw_too_long_is_rejected_not_truncated);
  RUN_TEST(test_link_vanraw_queue_order_and_drop_on_full);
  RUN_TEST(test_link_vanraw_queue_wraps_correctly);
  RUN_TEST(test_link_vanraw_end_to_end_slave_decodes_van_itself);
}
