// 双板链路协议 v1 —— **时基**用例（契约：ARCHITECTURE.md §4 + §3 的 TICK/DATA 两行）
//
// 三块：主板侧的 TickGen（50 Hz、单调、回绕、不补发突发）、从板侧的偏移估计
// （滑动最小 + 一阶低通）与三级超时状态机（100 ms / 500 ms / 3 s）、
// 以及跨屏扫表用的栅格助手。
//
// ★ 全部用**合成 tick 序列**驱动，不需要任何硬件：正常 / 丢包 / 抖动 / 断链 / 恢复。
#include <unity.h>
#include <stdio.h>

#include "link_time.h"

using namespace link;

namespace {

void assertState(LinkTimeState want, LinkTimeState got, const char* msg) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE(linkTimeStateName(want), linkTimeStateName(got), msg);
}

// 合成一帧 TICK：主板历元比本机早/晚 epoch_ms，再叠加抖动 jitter_ms（只往"慢"的方向）
TickMsg makeTick(int32_t local_ms, int32_t epoch_ms, int32_t jitter_ms, uint8_t seq) {
  TickMsg m;
  m.tick_ms = (uint32_t)(local_ms + epoch_ms + jitter_ms);
  m.seq = seq;
  return m;
}

}  // namespace

// ------------------------------------------------------------
// 主板侧：TickGen
// ------------------------------------------------------------
static void test_link_time_tickgen_50hz_monotonic(void) {
  TickGen g;
  g.reset(0);
  TickMsg m;
  TEST_ASSERT_TRUE(g.due(0, &m));            // 第一圈立刻发一帧
  TEST_ASSERT_EQUAL_HEX32(0u, m.tick_ms);
  TEST_ASSERT_EQUAL_UINT8(0u, m.seq);
  TEST_ASSERT_FALSE(g.due(0, &m));           // 同一毫秒不会再来一帧
  TEST_ASSERT_FALSE(g.due(19, &m));
  TEST_ASSERT_TRUE(g.due(20, &m));
  TEST_ASSERT_EQUAL_HEX32(20u, m.tick_ms);
  TEST_ASSERT_EQUAL_UINT8(1u, m.seq);

  // 20 ms 一拍地跑 1 秒：应该有 50 拍，且 tick_ms 严格递增（§3：50 Hz）
  uint16_t n = 1;
  uint32_t last = m.tick_ms;
  for (uint32_t t = 21; t <= 1000u; ++t) {
    if (g.due(t, &m)) {
      TEST_ASSERT_TRUE(m.tick_ms > last);
      last = m.tick_ms;
      ++n;
    }
  }
  TEST_ASSERT_EQUAL_UINT16(50u, n);
  TEST_ASSERT_EQUAL_UINT16(51u, g.ticksSent());   // 加上 t = 0 那一拍
  TEST_ASSERT_EQUAL_UINT8(51u, g.seq());
}

// 主循环被拖慢 105 ms：**不补发突发**（一次只发一帧，seq 只 +1），
// 但栅格相位不乱（下一拍仍在 20 ms 的倍数上），tick_ms 仍是真实毫秒。
static void test_link_time_tickgen_no_catchup_burst(void) {
  TickGen g;
  g.reset(0);
  TickMsg m;
  TEST_ASSERT_TRUE(g.due(0, &m));
  TEST_ASSERT_TRUE(g.due(105, &m));
  TEST_ASSERT_EQUAL_HEX32(105u, m.tick_ms);      // §4：主板自己的单调毫秒
  TEST_ASSERT_EQUAL_UINT8(1u, m.seq);            // 只跳了一格
  for (uint32_t t = 105u; t < 120u; ++t) {
    TEST_ASSERT_FALSE_MESSAGE(g.due(t, &m), "错过的槽位不许补发（会把突发占满）");
  }
  TEST_ASSERT_TRUE(g.due(120, &m));
  TEST_ASSERT_EQUAL_UINT8(2u, m.seq);
  TEST_ASSERT_EQUAL_HEX32(120u, m.tick_ms);
  TEST_ASSERT_EQUAL_UINT16(3u, g.ticksSent());
}

// 32 位毫秒回绕（49.7 天）：有符号差必须仍然正确（§4 的 d 就是这么算的）
static void test_link_time_tickgen_32bit_wrap(void) {
  const uint32_t before = 0xFFFFFFF6u;   // 回绕前 10 ms
  TickGen g;
  g.reset(before);
  TickMsg m;
  TEST_ASSERT_TRUE(g.due(before, &m));
  TEST_ASSERT_EQUAL_HEX32(before, m.tick_ms);
  TEST_ASSERT_FALSE(g.due(0x00000005u, &m));      // 回绕后过了 15 ms，还差 5 ms
  TEST_ASSERT_TRUE(g.due(0x0000000Au, &m));       // 回绕后 20 ms，正好一拍
  TEST_ASSERT_EQUAL_HEX32(0x0000000Au, m.tick_ms);
  TEST_ASSERT_EQUAL_INT32(20, (int32_t)(m.tick_ms - before));
  TEST_ASSERT_EQUAL_UINT8(1u, m.seq);
}

// ------------------------------------------------------------
// 从板侧：偏移估计
// ------------------------------------------------------------
static void test_link_time_offset_locks_without_jitter(void) {
  LinkTime lt;
  lt.reset();
  const int32_t epoch = -3700;      // 主板比本机早 3.7 s 上电 ⇒ d 是负的
  uint32_t local = 1000;
  for (uint8_t i = 0; i < 10; ++i) {
    const TickMsg m = makeTick((int32_t)local, epoch, 0, i);
    lt.onTick(m, local);
    lt.update(local);
    local += 20u;
  }
  lt.update(local);
  assertState(LinkTimeState::Locked, lt.state(), "10 拍之后必须锁住");
  TEST_ASSERT_TRUE(lt.haveBasis());
  TEST_ASSERT_EQUAL_INT32(epoch, lt.offsetMs());                    // 无抖动 ⇒ 精确
  TEST_ASSERT_EQUAL_HEX32((uint32_t)((int32_t)local + epoch), lt.masterNowMs());
  TEST_ASSERT_EQUAL_HEX32(20u, lt.tickAgeMs());
  TEST_ASSERT_EQUAL_UINT16(10u, lt.ticksSeen());
  TEST_ASSERT_EQUAL_UINT8(9u, lt.lastSeq());
  TEST_ASSERT_EQUAL_UINT16(0u, lt.seqGaps());
}

// 抖动（串口单向延迟只有正抖动）：估计值**只会偏晚、绝不会偏早**，
// 且收敛到抖动下沿附近（滑动最小 + 一阶低通的固定点在最小值 ±4 ms 内）。
static void test_link_time_offset_min_filter_under_jitter(void) {
  LinkTime lt;
  lt.reset();
  const int32_t epoch = 5000;
  const int32_t jit[10] = {0, 3, 7, 1, 15, 2, 0, 9, 4, 6};
  uint32_t local = 100;
  for (uint16_t i = 0; i < 400u; ++i) {
    const TickMsg m = makeTick((int32_t)local, epoch, jit[i % 10u], (uint8_t)i);
    lt.onTick(m, local);
    lt.update(local);
    local += 20u;
  }
  lt.update(local);
  assertState(LinkTimeState::Locked, lt.state(), "有抖动也要锁住");
  TEST_ASSERT_TRUE_MESSAGE(lt.offsetMs() >= epoch, "取最小 ⇒ 估计值不许早于真实历元差");
  TEST_ASSERT_TRUE_MESSAGE(lt.offsetMs() - epoch <= 8,
                           "收敛后偏差必须远小于 §4 的 40 ms 目标（这里是 ±4 ms 量化）");
  TEST_ASSERT_EQUAL_UINT8(kOffsetWindow, lt.windowSamples());
  // 相位误差：两屏对齐用的是 masterNowMs()，它也必须落在同一个误差带里
  const int32_t phase_err = (int32_t)lt.masterNowMs() - (int32_t)(local + (uint32_t)epoch);
  TEST_ASSERT_TRUE(phase_err >= 0);
  TEST_ASSERT_TRUE(phase_err <= 8);
}

// 丢包（seq 跳变）只影响计数，不影响时基：TICK 的 tick_ms 仍然把它们锁住
static void test_link_time_seq_gap_counting(void) {
  LinkTime lt;
  lt.reset();
  const uint8_t seqs[] = {0u, 1u, 2u, 5u, 6u, 6u, 7u};   // 2→5 缺 2 拍；6→6 是重复
  uint32_t local = 1000;
  for (uint8_t i = 0; i < sizeof(seqs); ++i) {
    const TickMsg m = makeTick((int32_t)local, 1000, 0, seqs[i]);
    lt.onTick(m, local);
    lt.update(local);
    local += 20u;
  }
  TEST_ASSERT_EQUAL_UINT16(2u, lt.seqGaps());       // 2→5(缺 2)、6→6(重复)
  TEST_ASSERT_EQUAL_UINT16(2u, lt.seqMissing());    // 重复那次没有"缺几个"可言
  TEST_ASSERT_EQUAL_UINT16(7u, lt.ticksSeen());
  TEST_ASSERT_EQUAL_UINT8(7u, lt.lastSeq());
  assertState(LinkTimeState::Locked, lt.state(), "seq 跳变不影响时基");

  // 255 → 0 的回绕**不是**丢帧（有符号/环形差都算 1）
  LinkTime lt2;
  lt2.reset();
  const uint8_t wrap[] = {254u, 255u, 0u, 1u};
  local = 500;
  for (uint8_t i = 0; i < sizeof(wrap); ++i) {
    const TickMsg m = makeTick((int32_t)local, 0, 0, wrap[i]);
    lt2.onTick(m, local);
    lt2.update(local);
    local += 20u;
  }
  TEST_ASSERT_EQUAL_UINT16(0u, lt2.seqGaps());
  TEST_ASSERT_EQUAL_UINT16(0u, lt2.seqMissing());
}

// ------------------------------------------------------------
// 三级超时（§3 的 TICK 行：>100 ms 失基准 / >500 ms 降级 / >3 s 回退 Sim）
// 边界是 `>`：恰好 100 ms 仍算有基准
// ------------------------------------------------------------
static void test_link_time_state_ladder_boundaries(void) {
  LinkTime lt;
  lt.reset();
  const TickMsg m = makeTick(1000, 500, 0, 0);
  lt.onTick(m, 1000);

  struct Case {
    uint32_t age;
    LinkTimeState want;
  };
  const Case cases[] = {
      {0u, LinkTimeState::Locked},
      {99u, LinkTimeState::Locked},
      {100u, LinkTimeState::Locked},        // 恰好 100 ms：契约写的是">100 ms"
      {101u, LinkTimeState::NoBasis},       // 时间基准失效 ⇒ 回退本地时钟
      {500u, LinkTimeState::NoBasis},
      {501u, LinkTimeState::Degraded},      // 链路降级（L10：冻结最后值 + 表情退常态）
      {3000u, LinkTimeState::Degraded},     // 恰好 3 s 还没到"回退 Sim"
      {3001u, LinkTimeState::SimFallback},  // 沿用 data_service 的 3 秒规则
      {60000u, LinkTimeState::SimFallback},
  };
  for (uint8_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    lt.update(1000u + cases[i].age);
    char msg[96];
    snprintf(msg, sizeof(msg), "距上一帧 TICK %u ms 的状态不对", (unsigned)cases[i].age);
    assertState(cases[i].want, lt.state(), msg);
    TEST_ASSERT_EQUAL_UINT32(cases[i].age, lt.tickAgeMs());
  }
}

// 开机以来一次 TICK 都没见过：不许自称"有基准"（否则刚上电 50 ms 的从板会
// 拿一个不存在的偏移去对齐扫表），一律回退 Sim，且 masterNow 就是本地时钟
static void test_link_time_never_seen_tick_is_sim_fallback(void) {
  LinkTime lt;
  lt.reset();
  const uint32_t times[] = {0u, 50u, 120u, 600u, 5000u};
  for (uint8_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
    lt.update(times[i]);
    assertState(LinkTimeState::SimFallback, lt.state(), "没见过 TICK ⇒ 只能自跑");
    TEST_ASSERT_EQUAL_HEX32(times[i], lt.masterNowMs());   // 回退本地时钟
  }
  TEST_ASSERT_FALSE(lt.haveBasis());
  TEST_ASSERT_FALSE(lt.tickSeen());
  TEST_ASSERT_FALSE(lt.offsetValid());
  TEST_ASSERT_EQUAL_UINT16(0u, lt.ticksSeen());
}

// 断链 → 回退 Sim → 主板(重启后)回来 ⇒ 无需重启从板，自动重锁
static void test_link_time_recovery_relocks_after_sim(void) {
  LinkTime lt;
  lt.reset();
  lt.onTick(makeTick(1000, 99000, 0, 7), 1000);
  lt.update(1000);
  TEST_ASSERT_EQUAL_INT32(99000, lt.offsetMs());

  lt.update(4500);                       // 3.5 s 没有 TICK
  assertState(LinkTimeState::SimFallback, lt.state(), "3.5 s 没消息 ⇒ 回退 Sim");
  TEST_ASSERT_FALSE_MESSAGE(lt.offsetValid(), "回退 Sim 时旧估计必须作废（对端可能重启了）");
  TEST_ASSERT_EQUAL_HEX32(4500u, lt.masterNowMs());

  // 主板重启：millis() 从头来 ⇒ 历元差整个变了
  lt.onTick(makeTick(5000, -4960, 0, 0), 5000);
  lt.update(5000);
  assertState(LinkTimeState::Locked, lt.state(), "主板回来 ⇒ 自动恢复（无需重启从板）");
  TEST_ASSERT_EQUAL_INT32(-4960, lt.offsetMs());     // 一眼重锁，不是慢慢爬
  TEST_ASSERT_EQUAL_HEX32(40u, lt.masterNowMs());
}

// 断链之后**同一个历元**（只是断了几百毫秒）：估计值不能被清掉重来，
// 否则恢复瞬间两屏会跳一下
static void test_link_time_short_gap_keeps_offset(void) {
  LinkTime lt;
  lt.reset();
  uint32_t local = 1000;
  for (uint8_t i = 0; i < 30; ++i) {
    lt.onTick(makeTick((int32_t)local, 12345, 0, i), local);
    lt.update(local);
    local += 20u;
  }
  TEST_ASSERT_EQUAL_INT32(12345, lt.offsetMs());
  local += 300u;                          // 断 300 ms（>100 ms ⇒ 失基准）
  lt.update(local);
  assertState(LinkTimeState::NoBasis, lt.state(), "300 ms ⇒ 失基准、回退本地时钟");
  TEST_ASSERT_EQUAL_HEX32(local, lt.masterNowMs());
  TEST_ASSERT_EQUAL_UINT8(0u, lt.windowSamples());   // 滑窗清空了（"重新攒"）

  local += 20u;
  lt.onTick(makeTick((int32_t)local, 12345, 0, 30), local);
  lt.update(local);
  assertState(LinkTimeState::Locked, lt.state(), "TICK 回来就立刻有基准");
  TEST_ASSERT_EQUAL_INT32(12345, lt.offsetMs());     // 历元没变 ⇒ 估计值照旧
}

// 断链之后对端**换了历元**（重启）：旧的最小值不许把估计"钉"住一秒多
static void test_link_time_epoch_jump_relocks(void) {
  LinkTime lt;
  lt.reset();
  uint32_t local = 1000;
  for (uint8_t i = 0; i < 20; ++i) {
    lt.onTick(makeTick((int32_t)local, 100000, 0, i), local);
    lt.update(local);
    local += 20u;
  }
  TEST_ASSERT_EQUAL_INT32(100000, lt.offsetMs());

  local += 400u;                            // 断 400 ms（>100 ms ⇒ 失基准，还没到降级）
  lt.update(local);
  assertState(LinkTimeState::NoBasis, lt.state(), "400 ms ⇒ 失基准");

  local += 700u;                            // 对端重启后的新历元
  for (uint8_t i = 0; i < 40; ++i) {
    lt.onTick(makeTick((int32_t)local, -3000, 0, i), local);
    lt.update(local);
    local += 20u;
  }
  assertState(LinkTimeState::Locked, lt.state(), "新历元的 TICK 进来就该锁住");
  TEST_ASSERT_EQUAL_INT32(-3000, lt.offsetMs());   // ★ 历元跳变 ⇒ 一眼重锁
}

// DATA 走同一套三档（§3 的 DATA 行"同 TICK 那三档"），但两路各自独立
static void test_link_time_data_state_is_independent(void) {
  LinkTime lt;
  lt.reset();
  lt.onTick(makeTick(1000, 2000, 0, 0), 1000);
  lt.onData(1000);
  lt.update(1000);
  assertState(LinkTimeState::Locked, lt.state(), "TICK");
  assertState(LinkTimeState::Locked, lt.dataState(), "DATA");

  lt.update(1200);                          // 两路都老了 200 ms
  assertState(LinkTimeState::NoBasis, lt.state(), "TICK 老了 200 ms");
  assertState(LinkTimeState::NoBasis, lt.dataState(), "DATA 老了 200 ms");

  lt.onTick(makeTick(1200, 2000, 0, 1), 1200);   // 只有 TICK 回来
  lt.update(1200);
  assertState(LinkTimeState::Locked, lt.state(), "TICK 回来了");
  assertState(LinkTimeState::NoBasis, lt.dataState(), "DATA 还没回来（两路独立）");
  TEST_ASSERT_EQUAL_UINT32(0u, lt.tickAgeMs());
  TEST_ASSERT_EQUAL_UINT32(200u, lt.dataAgeMs());

  lt.update(5600);                          // TICK 也断了 >3 s
  assertState(LinkTimeState::SimFallback, lt.state(), "3.4 s ⇒ 回退 Sim");
  assertState(LinkTimeState::SimFallback, lt.dataState(), "DATA 更早就该回退 Sim");
}

// §4 的栅格：从板 UI 就绪后等到下一个 tick 栅格再启动扫表动画
static void test_link_time_animation_grid(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, msUntilGrid(0u));
  TEST_ASSERT_EQUAL_UINT32(99u, msUntilGrid(1u));
  TEST_ASSERT_EQUAL_UINT32(1u, msUntilGrid(99u));
  TEST_ASSERT_EQUAL_UINT32(0u, msUntilGrid(100u));      // 正好在栅格上 ⇒ 立刻
  TEST_ASSERT_EQUAL_UINT32(50u, msUntilGrid(150u));
  TEST_ASSERT_EQUAL_UINT32(66u, msUntilGrid(1234u));
  TEST_ASSERT_EQUAL_UINT32(0u, msUntilGrid(1234u, 0u)); // 非法栅格：不许除零
  TEST_ASSERT_EQUAL_UINT32(0u, msUntilGrid(1000u, 200u));   // 1000 正好是 200 的整数倍
  TEST_ASSERT_EQUAL_UINT32(100u, msUntilGrid(1100u, 200u));
  TEST_ASSERT_EQUAL_UINT32(0u, msUntilGrid(1200u, 200u));
  TEST_ASSERT_EQUAL_UINT32(kAnimGridMs, 100u);          // 契约建议的栅格
}

// 状态名（日志/STATUS 用）不许变
static void test_link_time_state_names(void) {
  TEST_ASSERT_EQUAL_STRING("locked", linkTimeStateName(LinkTimeState::Locked));
  TEST_ASSERT_EQUAL_STRING("no_basis", linkTimeStateName(LinkTimeState::NoBasis));
  TEST_ASSERT_EQUAL_STRING("degraded", linkTimeStateName(LinkTimeState::Degraded));
  TEST_ASSERT_EQUAL_STRING("sim", linkTimeStateName(LinkTimeState::SimFallback));
  // 三档的毫秒数就是契约原文（§3 的 TICK 行）
  TEST_ASSERT_EQUAL_UINT32(20u, kTickPeriodMs);
  TEST_ASSERT_EQUAL_UINT32(100u, kBasisLostMs);
  TEST_ASSERT_EQUAL_UINT32(500u, kDegradeMs);
  TEST_ASSERT_EQUAL_UINT32(3000u, kSimFallbackMs);
  TEST_ASSERT_EQUAL_UINT32(100u, kAnimGridMs);
}

void register_link_time_tests(void) {
  RUN_TEST(test_link_time_tickgen_50hz_monotonic);
  RUN_TEST(test_link_time_tickgen_no_catchup_burst);
  RUN_TEST(test_link_time_tickgen_32bit_wrap);
  RUN_TEST(test_link_time_offset_locks_without_jitter);
  RUN_TEST(test_link_time_offset_min_filter_under_jitter);
  RUN_TEST(test_link_time_seq_gap_counting);
  RUN_TEST(test_link_time_state_ladder_boundaries);
  RUN_TEST(test_link_time_never_seen_tick_is_sim_fallback);
  RUN_TEST(test_link_time_recovery_relocks_after_sim);
  RUN_TEST(test_link_time_short_gap_keeps_offset);
  RUN_TEST(test_link_time_epoch_jump_relocks);
  RUN_TEST(test_link_time_data_state_is_independent);
  RUN_TEST(test_link_time_animation_grid);
  RUN_TEST(test_link_time_state_names);
}
