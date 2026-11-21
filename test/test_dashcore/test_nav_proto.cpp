// 导航短报文 + 导航状态的宿主机用例。
//
// 为什么值得测：这条链路的两个失败模式都会"看起来很正常的错"——
//   ① 线上字节排错（小端写成大端 / CRC 覆盖范围错）⇒ 屏上显示"前方 4 米右转"这种荒唐数；
//   ② 断线后继续显示旧数据 ⇒ 该转弯时不转弯。
// 两个都不报错，只有单测/上车能抓。所以这里钉住：黄金字节、CRC、长度精确性、
// 超时降级、昼夜结论的"新鲜才可信"。
#include <unity.h>
#include <string.h>

#include "nav_proto.h"
#include "nav_state.h"

// ---------------- 夹具：一帧"右转 300 m / 42 km/h / 中山路" ----------------
static NavFrame makeFixture() {
  NavFrame f;
  f.turn         = (uint8_t)NavTurn::Right;   // 3
  f.dist_m       = 300;
  f.speed_kph    = 42;
  f.heading_cdeg = 12345;
  f.remain_min   = 8;
  f.speed_limit  = 60;
  f.flags        = (uint8_t)(kNavFlagNight | kNavFlagNightValid);
  f.seq          = 7;
  const char* road = "中山路";                 // UTF-8 9 字节
  f.road_len = (uint8_t)strlen(road);
  memcpy(f.road, road, f.road_len);
  return f;
}

// ① 黄金字节：布局/小端/CRC 覆盖范围一次钉死（改协议必然红）
static void test_pack_golden_bytes(void) {
  NavFrame f = makeFixture();
  uint8_t buf[64];
  const size_t n = nav_pack(f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(25, n);            // 14 头 + 9 路名 + 2 CRC

  static const uint8_t want[25] = {
      0xA7, 0x01, 0x07, 0x03, 0x03,        // magic / ver / seq / turn / flags
      0x2C, 0x01,                          // dist_m = 300 (小端)
      0x2A,                                // speed = 42
      0x39, 0x30,                          // heading = 12345 (小端)
      0x08, 0x3C,                          // remain_min = 8 / speed_limit = 60
      0x09, 0x00,                          // road_len = 9 / reserved
      0xE4, 0xB8, 0xAD, 0xE5, 0xB1, 0xB1, 0xE8, 0xB7, 0xAF,   // "中山路"
      0x73, 0xCA,                          // CRC-16/CCITT-FALSE = 0xCA73 (小端)
  };
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, buf, sizeof(want), "黄金字节不一致（协议布局被改了）");
  TEST_ASSERT_EQUAL_UINT16(0xCA73u, nav_crc16(buf, n - 2u));
}

// ② 往返：所有字段原样回来，路名以 '\0' 结尾（LVGL 要 C 字符串）
static void test_roundtrip_all_fields(void) {
  NavFrame f = makeFixture();
  uint8_t buf[64];
  const size_t n = nav_pack(f, buf, sizeof(buf));
  NavFrame g;
  TEST_ASSERT_TRUE(nav_unpack(buf, n, &g));
  TEST_ASSERT_EQUAL_UINT8(f.turn, g.turn);
  TEST_ASSERT_EQUAL_UINT16(f.dist_m, g.dist_m);
  TEST_ASSERT_EQUAL_UINT8(f.speed_kph, g.speed_kph);
  TEST_ASSERT_EQUAL_UINT16(f.heading_cdeg, g.heading_cdeg);
  TEST_ASSERT_EQUAL_UINT8(f.remain_min, g.remain_min);
  TEST_ASSERT_EQUAL_UINT8(f.speed_limit, g.speed_limit);
  TEST_ASSERT_EQUAL_UINT8(f.flags, g.flags);
  TEST_ASSERT_EQUAL_UINT8(f.seq, g.seq);
  TEST_ASSERT_EQUAL_UINT8(f.road_len, g.road_len);
  TEST_ASSERT_EQUAL_STRING("中山路", g.road);
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)g.road[g.road_len]);   // 结尾 0 在
}

// ③ 空路名：帧长必须是 16，且能解回来
static void test_empty_road(void) {
  NavFrame f = makeFixture();
  f.road_len = 0;
  memset(f.road, 0, sizeof(f.road));
  uint8_t buf[64];
  const size_t n = nav_pack(f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(kNavFrameMin, n);
  NavFrame g;
  TEST_ASSERT_TRUE(nav_unpack(buf, n, &g));
  TEST_ASSERT_EQUAL_STRING("", g.road);
}

// ④ 坏帧一律拒绝（不许把半个字段喂给 UI）
static void test_rejects_bad_frames(void) {
  NavFrame f = makeFixture();
  uint8_t buf[64];
  const size_t n = nav_pack(f, buf, sizeof(buf));
  NavFrame g;

  uint8_t bad[64];

  memcpy(bad, buf, n); bad[0] = 0xA6;                       // 魔数
  TEST_ASSERT_FALSE(nav_unpack(bad, n, &g));

  memcpy(bad, buf, n); bad[1] = 2;                          // 版本
  TEST_ASSERT_FALSE(nav_unpack(bad, n, &g));

  memcpy(bad, buf, n); bad[3] ^= 0x01;                      // 载荷改了但 CRC 没跟着改
  TEST_ASSERT_FALSE(nav_unpack(bad, n, &g));

  memcpy(bad, buf, n); bad[n - 1] ^= 0xFF;                  // CRC 高字节
  TEST_ASSERT_FALSE(nav_unpack(bad, n, &g));

  TEST_ASSERT_FALSE(nav_unpack(buf, n - 1, &g));            // 截断
  TEST_ASSERT_FALSE(nav_unpack(buf, n + 1, &g));            // 多一字节（长度必须精确）

  memcpy(bad, buf, n + 1); bad[12] = (uint8_t)(kNavRoadMax + 1);   // road_len 越界
  TEST_ASSERT_FALSE(nav_unpack(bad, n + 1, &g));

  TEST_ASSERT_FALSE(nav_unpack(nullptr, n, &g));
  TEST_ASSERT_FALSE(nav_unpack(buf, n, nullptr));
  TEST_ASSERT_FALSE(nav_unpack(buf, 0, &g));
}

// ⑤ 打包时容量不足 / road_len 越界 ⇒ 返回 0（不写越界）
static void test_pack_guards(void) {
  NavFrame f = makeFixture();
  uint8_t small[20];
  TEST_ASSERT_EQUAL_UINT32(0, nav_pack(f, small, sizeof(small)));   // 需要 25

  uint8_t buf[64];
  f.road_len = (uint8_t)(kNavRoadMax + 1);
  TEST_ASSERT_EQUAL_UINT32(0, nav_pack(f, buf, sizeof(buf)));

  TEST_ASSERT_EQUAL_UINT32(0, nav_pack(f, nullptr, 0));
}

// ⑥ 状态：收到即有效；到 stale_ms 才失效；早一拍不许失效
static void test_state_stale_after_timeout(void) {
  NavTimeouts t;                      // 默认 3000 ms
  NavState s;
  NavFrame f = makeFixture();
  TEST_ASSERT_FALSE(s.valid);
  TEST_ASSERT_FALSE(nav_state_tick(s, 1000, t));   // 从未收到 ⇒ 不翻转

  nav_state_apply(s, f, 1000);
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_TRUE(s.ever_received);
  TEST_ASSERT_EQUAL_UINT8(3, s.turn);
  TEST_ASSERT_EQUAL_UINT16(300, s.dist_m);
  TEST_ASSERT_EQUAL_STRING("中山路", s.road);
  TEST_ASSERT_EQUAL_UINT32(1, s.good_frames);

  TEST_ASSERT_FALSE(nav_state_tick(s, 1000 + t.stale_ms - 1, t));   // 差 1 ms 还有效
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_TRUE(nav_state_tick(s, 1000 + t.stale_ms, t));        // 到点失效，且报翻转
  TEST_ASSERT_FALSE(s.valid);
  TEST_ASSERT_FALSE(nav_state_tick(s, 1000 + t.stale_ms + 1000, t)); // 只说一次
}

// ⑦ 昼夜结论：必须"新鲜 + 带结论"才可信
static void test_night_only_when_fresh(void) {
  NavTimeouts t;
  NavState s;
  NavFrame f = makeFixture();        // flags 带 Night|NightValid
  nav_state_apply(s, f, 0);
  TEST_ASSERT_TRUE(nav_state_night_ok(s));
  TEST_ASSERT_TRUE(nav_state_night_is_night(s));

  // 白天那一帧：结论是"白天"，仍然算"有结论"
  NavFrame d = f;
  d.flags = kNavFlagNightValid;
  nav_state_apply(s, d, 100);
  TEST_ASSERT_TRUE(nav_state_night_ok(s));
  TEST_ASSERT_FALSE(nav_state_night_is_night(s));

  // 不带结论的帧：不能驱动夜间模式（避免把上一帧的结论当现在）
  NavFrame noflag = f;
  noflag.flags = 0;
  NavState s2;
  nav_state_apply(s2, noflag, 0);
  TEST_ASSERT_FALSE(nav_state_night_ok(s2));

  // 超时后：新鲜度没了 ⇒ 夜间模式不许再用它
  nav_state_tick(s, 100 + t.stale_ms, t);
  TEST_ASSERT_FALSE(nav_state_night_ok(s));
}

// ⑧ 序号回绕 + 角度防御（25999/36000+ 的输入只影响旋转角）
static void test_seq_wrap_and_heading_clamp(void) {
  NavState s;
  NavFrame f = makeFixture();
  f.seq = 254;
  nav_state_apply(s, f, 0);
  f.seq = 255;
  nav_state_apply(s, f, 10);
  f.seq = 0;
  nav_state_apply(s, f, 20);
  TEST_ASSERT_EQUAL_UINT8(0, s.seq);
  TEST_ASSERT_EQUAL_UINT32(3, s.good_frames);

  f.heading_cdeg = 36000;            // 越界：取模后是 0
  nav_state_apply(s, f, 30);
  TEST_ASSERT_EQUAL_UINT16(0, s.heading_cdeg);
  f.heading_cdeg = 35999;
  nav_state_apply(s, f, 40);
  TEST_ASSERT_EQUAL_UINT16(35999, s.heading_cdeg);
}

// ⑨ 距离分档（UI 复用；<1 km 用米、≥1 km 用 0.1 km）
static void test_dist_formatting(void) {
  bool km = false;
  TEST_ASSERT_EQUAL_UINT32(300, nav_dist_value(300, &km));
  TEST_ASSERT_FALSE(km);
  TEST_ASSERT_EQUAL_UINT32(999, nav_dist_value(999, &km));
  TEST_ASSERT_FALSE(km);
  TEST_ASSERT_EQUAL_UINT32(10, nav_dist_value(1000, &km));   // 1.0 km
  TEST_ASSERT_TRUE(km);
  TEST_ASSERT_EQUAL_UINT32(12, nav_dist_value(1200, &km));   // 1.2 km
  TEST_ASSERT_TRUE(km);
  TEST_ASSERT_EQUAL_UINT32(13, nav_dist_value(1250, &km));   // 0.1 km 四舍五入
  TEST_ASSERT_TRUE(km);
  TEST_ASSERT_EQUAL_UINT32(0, nav_dist_value(0, &km));
  TEST_ASSERT_FALSE(km);
}

void register_nav_tests(void) {
  RUN_TEST(test_pack_golden_bytes);
  RUN_TEST(test_roundtrip_all_fields);
  RUN_TEST(test_empty_road);
  RUN_TEST(test_rejects_bad_frames);
  RUN_TEST(test_pack_guards);
  RUN_TEST(test_state_stale_after_timeout);
  RUN_TEST(test_night_only_when_fresh);
  RUN_TEST(test_seq_wrap_and_heading_clamp);
  RUN_TEST(test_dist_formatting);
}
