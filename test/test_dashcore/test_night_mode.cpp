// 夜间模式判据的宿主机用例：进/出迟滞、抖动不跳、数据不可信时不动、亮度档映射。
//
// 为什么值得测：这条判据一旦错，症状是"夜里屏忽然亮一下"或"进了隧道不暗"，
// 两种都没法在台面上靠眼睛复现（要真的开灯、真的等 3 秒）。而它本身是纯函数，
// 喂时间戳就能把所有边界钉死 —— 这正是把它从 render 循环里拆出来的理由。
#include <unity.h>
#include <string.h>
#include "night_mode.h"

// ★ 每个用例自己拿一份默认配置（Unity 一个测试二进制只有**一个** setUp/tearDown，
//   那边在 test_main.cpp 里；这里再定义一份就是重复符号，链接当场失败）。
static NightModeConfig defaultCfg(void) { return NightModeConfig(); }

// ① 一开灯**不能立刻**进夜间：不到 3 s 必须还是白天
static void test_enter_needs_three_seconds(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  bool flipped = false;
  for (uint32_t t = 0; t < cfg.enter_ms; t += 100) {
    if (night_mode_update(st, t, true, true, cfg)) flipped = true;
  }
  TEST_ASSERT_FALSE_MESSAGE(flipped, "不到 enter_ms 就翻转了（会一开灯就闪一下）");
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_TRUE_MESSAGE(night_mode_update(st, cfg.enter_ms, true, true, cfg),
                           "刚好到 enter_ms 这一刻应当翻转（边界包含）");
  TEST_ASSERT_TRUE(st.night);
  TEST_ASSERT_EQUAL_UINT8(cfg.night_pct, night_mode_pct(st, cfg));
}

// ② 灯灭之后要**等够 10 s** 才回白天（隧道口/闪灯不许来回跳）
static void test_exit_needs_ten_seconds(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  night_mode_update(st, 0, true, true, cfg);
  night_mode_update(st, cfg.enter_ms, true, true, cfg);
  TEST_ASSERT_TRUE(st.night);

  const uint32_t t0 = cfg.enter_ms + 1000;
  bool flipped = false;
  for (uint32_t t = t0; t < t0 + cfg.exit_ms; t += 100) {
    if (night_mode_update(st, t, false, true, cfg)) flipped = true;
  }
  TEST_ASSERT_FALSE_MESSAGE(flipped, "不到 exit_ms 就回白天了（会车闪灯就会跳）");
  TEST_ASSERT_TRUE(st.night);
  TEST_ASSERT_TRUE(night_mode_update(st, t0 + cfg.exit_ms, false, true, cfg));
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_EQUAL_UINT8(cfg.day_pct, night_mode_pct(st, cfg));
}

// ③ 计时中间灯抖一下 ⇒ **从头计时**，不能攒够 3 s 就进
static void test_flicker_restarts_timer(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  night_mode_update(st, 0, true, true, cfg);          // 开始计时
  night_mode_update(st, 2500, true, true, cfg);       // 2.5 s
  night_mode_update(st, 2600, false, true, cfg);      // 灯灭了 ⇒ 目标态变了
  TEST_ASSERT_FALSE(st.night);
  // 再亮起来，从这一刻重新算 3 s
  night_mode_update(st, 2700, true, true, cfg);
  TEST_ASSERT_FALSE(night_mode_update(st, 2700 + 2000, true, true, cfg));
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_TRUE(night_mode_update(st, 2700 + 3000, true, true, cfg));
  TEST_ASSERT_TRUE(st.night);
}

// ④ 数据不可信时**保持当前态、也不动计时器**（复电后接着算，不从头）
static void test_data_untrusted_holds_state(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  night_mode_update(st, 0, true, true, cfg);
  // 数据断了 5 s（> enter_ms）：不许翻转
  TEST_ASSERT_FALSE(night_mode_update(st, 2000, true, false, cfg));
  TEST_ASSERT_FALSE(night_mode_update(st, 5000, true, false, cfg));
  TEST_ASSERT_FALSE(st.night);
  // 复电后计时器仍在 0（没被数据断的那几拍重置），到 3 s 就该进
  TEST_ASSERT_TRUE(night_mode_update(st, cfg.enter_ms, true, true, cfg));
  TEST_ASSERT_TRUE(st.night);
  // 夜间态下一旦不可信，同样保持（不许自己回白天）
  TEST_ASSERT_FALSE(night_mode_update(st, 20000, false, false, cfg));
  TEST_ASSERT_TRUE(st.night);
}

// ⑤ 一直在白天：不翻转、不误报（每一拍都返回 false）
static void test_stays_day_without_lamps(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  for (uint32_t t = 0; t < 60000; t += 250) {
    TEST_ASSERT_FALSE(night_mode_update(st, t, false, true, cfg));
  }
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_EQUAL_UINT8(cfg.day_pct, night_mode_pct(st, cfg));
}

// ⑥ 时间戳回绕（30 天后 now 会绕回来）：无符号减法必须仍然正确
static void test_timestamp_wrap_is_safe(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  const uint32_t start = 0xFFFFFF00u;      // 距回绕 256 ms
  night_mode_update(st, start, true, true, cfg);
  TEST_ASSERT_FALSE(night_mode_update(st, 0x00000100u, true, true, cfg));   // 已过 512 ms
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_TRUE(night_mode_update(st, start + cfg.enter_ms, true, true, cfg));  // 3 s 后
  TEST_ASSERT_TRUE(st.night);
}


// ⑦ ★ 导航驱动（2026-11-21 车主：夜间优先跟高德昼夜走）
//   nav 的结论是"此刻该不该用夜间配色"，比车灯（会车闪灯/隧道开灯都会误触发）更准。
//   判据：nav 可用时**立即**采信（不进迟滞窗口）；不可用时退回车灯那套原逻辑。
static void test_nav_source_wins_over_lamps(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;

  // 车灯没开、但高德说"现在该夜间" ⇒ 立刻进夜间，来源记 nav
  TEST_ASSERT_TRUE(night_mode_update_nav(st, 0u, false, true, true, true, cfg));
  TEST_ASSERT_TRUE(st.night);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)NightSource::Nav, (uint8_t)st.src);
  TEST_ASSERT_EQUAL_UINT8(cfg.night_pct, night_mode_pct(st, cfg));

  // 已经在夜间：再来一帧"夜间"⇒ 不报翻转（不许每帧都写背光）
  TEST_ASSERT_FALSE(night_mode_update_nav(st, 100u, false, true, true, true, cfg));

  // 高德说"白天" ⇒ 立刻回白天（车灯此时还亮着也不管）
  TEST_ASSERT_TRUE(night_mode_update_nav(st, 200u, true, true, false, true, cfg));
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)NightSource::Nav, (uint8_t)st.src);
}

// ⑧ nav 不可用（没连/超时）⇒ 退回车灯：仍然要等 3 s，来源记 lamps
static void test_nav_unavailable_falls_back_to_lamps(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  // nav_ok=false，车灯亮：3 s 之前不许翻
  TEST_ASSERT_FALSE(night_mode_update_nav(st, 0u, true, false, false, true, cfg));
  TEST_ASSERT_FALSE(st.night);
  TEST_ASSERT_FALSE(night_mode_update_nav(st, cfg.enter_ms - 1u, true, false, false, true, cfg));
  TEST_ASSERT_TRUE(night_mode_update_nav(st, cfg.enter_ms, true, false, false, true, cfg));
  TEST_ASSERT_TRUE(st.night);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)NightSource::Lamps, (uint8_t)st.src);
}

// ⑨ nav 从"有"变成"没有"：不立刻改主意（否则一掉线屏就闪一下）
static void test_nav_dropout_does_not_flip_immediately(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  night_mode_update_nav(st, 0u, false, true, true, true, cfg);   // nav 说夜间
  TEST_ASSERT_TRUE(st.night);
  // 掉线：nav_ok=false、车灯也没亮 ⇒ 车灯判据要等 exit_ms（10 s）才回白天
  // ★ 计时从**掉线那一刻**（1000 ms）算起，不是从开机算起：车灯判据重新计时。
  const uint32_t drop = 1000u;
  TEST_ASSERT_FALSE(night_mode_update_nav(st, drop, false, false, false, true, cfg));
  TEST_ASSERT_TRUE(st.night);
  TEST_ASSERT_FALSE(night_mode_update_nav(st, drop + cfg.exit_ms - 1u, false, false, false, true, cfg));
  TEST_ASSERT_TRUE(st.night);
  TEST_ASSERT_TRUE(night_mode_update_nav(st, drop + cfg.exit_ms, false, false, false, true, cfg));
  TEST_ASSERT_FALSE(st.night);
}

// ⑩ 数据不可信 + nav 不可用 ⇒ 一个字节都不动（原口径保留）
static void test_nav_and_data_both_untrusted_holds(void) {
  const NightModeConfig cfg = defaultCfg();
  NightModeState st;
  TEST_ASSERT_FALSE(night_mode_update_nav(st, 0u, true, false, false, false, cfg));
  TEST_ASSERT_FALSE(st.night);
  for (uint32_t t = 0; t < 60000u; t += 500u) {
    TEST_ASSERT_FALSE(night_mode_update_nav(st, t, true, false, false, false, cfg));
  }
  TEST_ASSERT_FALSE(st.night);
}

void register_night_mode_tests(void) {
  RUN_TEST(test_enter_needs_three_seconds);
  RUN_TEST(test_exit_needs_ten_seconds);
  RUN_TEST(test_flicker_restarts_timer);
  RUN_TEST(test_data_untrusted_holds_state);
  RUN_TEST(test_stays_day_without_lamps);
  RUN_TEST(test_timestamp_wrap_is_safe);
  RUN_TEST(test_nav_source_wins_over_lamps);
  RUN_TEST(test_nav_unavailable_falls_back_to_lamps);
  RUN_TEST(test_nav_dropout_does_not_flip_immediately);
  RUN_TEST(test_nav_and_data_both_untrusted_holds);
}
