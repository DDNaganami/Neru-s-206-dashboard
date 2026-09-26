// ============================================================
// 射频仲裁（BLE OBD ↔ ESP-NOW 链路）—— 2026-09-27 车上实测后新增
//
// 这一组钉的是 `lib/dashcore/radio_arbiter.h` 里的**策略**，不是硬件：
//   ① `LinkStartGate`：开机"先让 BLE 连、再启链路"，且**到点必须开闸**
//      （仪表主命脉 > 一个可选的 OBD 数据源）；
//   ② `RadioArbiter`：BLE 建连窗口内把共存偏好切给 BT，但**一次最多 8 秒**、
//      让出后**至少冷却 30 秒**，且"连上 + grace 到"要能正常让出。
//
// ★ 为什么这些必须是**纯逻辑 + 假时钟**：真 `millis()` 在宿主机上不可控，
//   而"第几毫秒开闸/让出"正是要逐条断言的（与 `test_panel_guard.cpp` 同一条纪律）。
// ★ 回绕安全单独有两条用例：`now_ms` 是 `millis()`，49.7 天回绕一次，
//   而"空闲时 cool_until_ 还停在 0"与"回绕后 now 很小"这两种情况必须都对。
// ============================================================
#include <unity.h>
#include <stdint.h>

#include "radio_arbiter.h"

using dashcore::LinkStartGate;
using dashcore::RadioArbiter;

// ---------------------------------------------------------------------------
// ① 启动闸门
// ---------------------------------------------------------------------------
static void test_gate_no_ble_opens_immediately(void) {
  // 没有 BLE 这一路的构建（有线档 / 从板 / pcpreview）：**第一圈就开**
  TEST_ASSERT_TRUE(LinkStartGate::open(false, false, 0u));
  TEST_ASSERT_TRUE(LinkStartGate::open(false, false, 1u));
  TEST_ASSERT_TRUE(LinkStartGate::open(false, true, 0u));
}

static void test_gate_opens_when_obd_ready(void) {
  // 连上就立刻开，不用等满窗口
  TEST_ASSERT_TRUE(LinkStartGate::open(true, true, 0u));
  TEST_ASSERT_TRUE(LinkStartGate::open(true, true, 1u));
}

static void test_gate_waits_but_then_opens_hard(void) {
  // 没连上 ⇒ 等；但**到点一定开**（不会因为 OBD 连不上就让从板一直没数据）
  TEST_ASSERT_FALSE(LinkStartGate::open(true, false, 0u));
  TEST_ASSERT_FALSE(LinkStartGate::open(true, false, LinkStartGate::kWaitMaxMs - 1u));
  TEST_ASSERT_TRUE(LinkStartGate::open(true, false, LinkStartGate::kWaitMaxMs));
  TEST_ASSERT_TRUE(LinkStartGate::open(true, false, LinkStartGate::kWaitMaxMs + 1u));
  // 窗口上限是**文档里写着的那个数**（15s 秒 = `setConnectTimeout(15)` 一整次尝试）
  TEST_ASSERT_EQUAL_UINT32(15000u, LinkStartGate::kWaitMaxMs);
}

// ---------------------------------------------------------------------------
// ② 优先权仲裁
// ---------------------------------------------------------------------------
static void test_arb_idle_never_holds(void) {
  RadioArbiter a;
  TEST_ASSERT_FALSE(a.update(0u, false, false));
  TEST_ASSERT_FALSE(a.holding());
  TEST_ASSERT_FALSE(a.update(100u, false, true));   // 连上了但不忙 ⇒ 不用抢
  TEST_ASSERT_FALSE(a.holding());
  TEST_ASSERT_EQUAL_UINT32(0u, a.windows());
}

static void test_arb_holds_while_busy_then_capped(void) {
  RadioArbiter a;
  TEST_ASSERT_TRUE(a.update(1000u, true, false));   // 忙 ⇒ 抢
  TEST_ASSERT_TRUE(a.holding());
  TEST_ASSERT_EQUAL_UINT32(1u, a.windows());
  // 7.999 秒还在占用
  TEST_ASSERT_TRUE(a.update(1000u + RadioArbiter::kHoldMaxMs - 1u, true, false));
  TEST_ASSERT_TRUE(a.holding());
  // 到 8 秒 ⇒ 让出，且记一笔"被上限掐断"
  TEST_ASSERT_FALSE(a.update(1000u + RadioArbiter::kHoldMaxMs, true, false));
  TEST_ASSERT_FALSE(a.holding());
  TEST_ASSERT_EQUAL_UINT32(1u, a.capped());
  TEST_ASSERT_EQUAL_UINT32(0u, a.releasedOk());
  // ★ 口径就是这几个数（改动它们要连着文档一起改）
  TEST_ASSERT_EQUAL_UINT32(8000u, RadioArbiter::kHoldMaxMs);
  TEST_ASSERT_EQUAL_UINT32(30000u, RadioArbiter::kCooldownMs);
  TEST_ASSERT_EQUAL_UINT32(2000u, RadioArbiter::kGraceAfterOkMs);
}

static void test_arb_cooldown_blocks_then_allows(void) {
  RadioArbiter a;
  const uint32_t t0 = 500u;
  TEST_ASSERT_TRUE(a.update(t0, true, false));
  const uint32_t released_at = t0 + RadioArbiter::kHoldMaxMs;
  TEST_ASSERT_FALSE(a.update(released_at, true, false));       // 被掐断
  // 冷却期里"还在忙"也不抢：这正是"8 秒之后把射频还给链路"那条承诺
  TEST_ASSERT_FALSE(a.update(released_at + 1u, true, false));
  TEST_ASSERT_FALSE(a.update(released_at + RadioArbiter::kCooldownMs - 1u, true, false));
  TEST_ASSERT_EQUAL_UINT32(1u, a.windows());
  // 冷却一到 ⇒ 再抢（BLE 仍有机会反复尝试，只是不再霸占）
  TEST_ASSERT_TRUE(a.update(released_at + RadioArbiter::kCooldownMs, true, false));
  TEST_ASSERT_EQUAL_UINT32(2u, a.windows());
}

static void test_arb_releases_after_connect_plus_grace(void) {
  RadioArbiter a;
  TEST_ASSERT_TRUE(a.update(0u, true, false));
  // 第 1 秒连上：先进入 grace（点数只在第一次看到 ready 时打）
  TEST_ASSERT_TRUE(a.update(1000u, true, true));
  TEST_ASSERT_TRUE(a.holding());
  // grace 还没到 ⇒ 继续压一会儿（服务发现 + 订阅 + 第一条指令要几个来回）
  TEST_ASSERT_TRUE(a.update(1000u + RadioArbiter::kGraceAfterOkMs - 1u, true, true));
  TEST_ASSERT_TRUE(a.holding());
  // grace 到 ⇒ **正常让出**（不是被上限掐断），并开始冷却
  TEST_ASSERT_FALSE(a.update(1000u + RadioArbiter::kGraceAfterOkMs, true, true));
  TEST_ASSERT_FALSE(a.holding());
  TEST_ASSERT_EQUAL_UINT32(1u, a.releasedOk());
  TEST_ASSERT_EQUAL_UINT32(0u, a.capped());
}

static void test_arb_grace_does_not_extend_forever(void) {
  // ★ 这条钉的是一个很容易写错的地方：若每圈都把 `ready_at_` 刷新成 now，
  //   `now - ready_at_` 永远是 0 ⇒ grace 永远不过期 ⇒ 变成永久占用。
  RadioArbiter a;
  TEST_ASSERT_TRUE(a.update(0u, true, true));      // 一进来就连上
  uint32_t t = 0u;
  for (int i = 0; i < 200; ++i) {                  // 每 10ms 调一次，共 2 秒
    t += 10u;
    a.update(t, true, true);
  }
  TEST_ASSERT_TRUE(a.holding());                   // 2 秒时才刚到 grace 边界
  TEST_ASSERT_FALSE(a.update(t + 10u, true, true));// 再过一拍 ⇒ 必须让出
  TEST_ASSERT_EQUAL_UINT32(1u, a.releasedOk());
}

static void test_arb_reconnect_waits_for_cooldown(void) {
  // 掉线后重连：busy 又变 true，但冷却没到 ⇒ 不许马上再抢
  RadioArbiter a;
  TEST_ASSERT_TRUE(a.update(0u, true, false));                 // 抢
  TEST_ASSERT_FALSE(a.update(RadioArbiter::kHoldMaxMs, true, false));  // 被掐断 → 冷却
  TEST_ASSERT_FALSE(a.update(10000u, false, false));           // 链路空闲期
  TEST_ASSERT_FALSE(a.update(11000u, true, false));            // 又想抢 ⇒ 冷却中，不给
  TEST_ASSERT_EQUAL_UINT32(1u, a.windows());
}

static void test_arb_rollover_safe(void) {
  // ★ `millis()` 49.7 天回绕：占用上限与冷却都必须照常成立
  RadioArbiter a;
  const uint32_t t0 = 0xFFFFFF00u;                 // 离回绕还有 256ms
  TEST_ASSERT_TRUE(a.update(t0, true, false));
  TEST_ASSERT_TRUE(a.update(t0 + 256u, true, false));              // 已经回绕（= 0x00000000）
  TEST_ASSERT_TRUE(a.update(t0 + 7999u, true, false));             // 7.999s
  TEST_ASSERT_FALSE(a.update(t0 + 8000u, true, false));            // 8s ⇒ 让出
  TEST_ASSERT_EQUAL_UINT32(1u, a.capped());
  TEST_ASSERT_FALSE(a.update(t0 + 8000u + 29999u, true, false));   // 冷却中
  TEST_ASSERT_TRUE(a.update(t0 + 8000u + 30000u, true, false));    // 冷却到 ⇒ 再抢
}

static void test_arb_ready_while_not_holding_no_hold(void) {
  // 冷却期内已经连上（ready=true）：不该因为 ready 就去抢射频
  RadioArbiter a;
  TEST_ASSERT_FALSE(a.update(123u, false, true));
  TEST_ASSERT_FALSE(a.holding());
  TEST_ASSERT_EQUAL_UINT32(0u, a.windows());
}

void register_radio_arbiter_tests(void) {
  RUN_TEST(test_gate_no_ble_opens_immediately);
  RUN_TEST(test_gate_opens_when_obd_ready);
  RUN_TEST(test_gate_waits_but_then_opens_hard);
  RUN_TEST(test_arb_idle_never_holds);
  RUN_TEST(test_arb_holds_while_busy_then_capped);
  RUN_TEST(test_arb_cooldown_blocks_then_allows);
  RUN_TEST(test_arb_releases_after_connect_plus_grace);
  RUN_TEST(test_arb_grace_does_not_extend_forever);
  RUN_TEST(test_arb_reconnect_waits_for_cooldown);
  RUN_TEST(test_arb_rollover_safe);
  RUN_TEST(test_arb_ready_while_not_holding_no_hold);
}
