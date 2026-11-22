// 阶段 B：路线几何（0xA8）的解码 + 与快照帧混流的用例。
// 为什么要测：路线帧和快照帧**共用同一个 BLE 环缓冲**，两种魔数交替出现；
// 只要长度算法或"找魔数"那一行走偏，症状是"屏上路线偶尔缺一段"——
// 静默、难查，所以这里把混流/坏帧/截断都钉住。
#include <unity.h>
#include <string.h>

#include "nav_route.h"
#include "nav_source.h"

static NavRoute makeRoute(uint8_t count) {
  NavRoute r;
  r.seq = 9;
  r.count = count;
  for (uint8_t i = 0; i < count; ++i) {
    r.pts[i].dx = (int16_t)(i * 10);
    r.pts[i].dy = (int16_t)(-(int16_t)(i * 20));
  }
  return r;
}

// ① 往返：点序、符号、CRC 都要原样
static void test_route_roundtrip(void) {
  NavRoute r = makeRoute(5);
  uint8_t buf[128];
  const size_t n = nav_route_pack(r, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(4u + 5u * 4u + 2u, (uint32_t)n);
  NavRoute g;
  TEST_ASSERT_TRUE(nav_route_unpack(buf, n, &g));
  TEST_ASSERT_EQUAL_UINT8(5, g.count);
  TEST_ASSERT_EQUAL_INT16(40, g.pts[4].dx);
  TEST_ASSERT_EQUAL_INT16(-80, g.pts[4].dy);
}

// ② 满 24 点：帧长 102，且能解回来（一个 BLE 包放得下）
static void test_route_max_points(void) {
  NavRoute r = makeRoute(kNavRouteMax);
  uint8_t buf[160];
  const size_t n = nav_route_pack(r, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(4u + 24u * 4u + 2u, (uint32_t)n);
  NavRoute g;
  TEST_ASSERT_TRUE(nav_route_unpack(buf, n, &g));
  TEST_ASSERT_EQUAL_UINT8(kNavRouteMax, g.count);
}

// ③ 坏帧全拒（点数越界/长度不精确/CRC/版本/魔数）
static void test_route_rejects(void) {
  NavRoute r = makeRoute(3);
  uint8_t buf[64];
  const size_t n = nav_route_pack(r, buf, sizeof(buf));
  NavRoute g;
  uint8_t bad[64];

  memcpy(bad, buf, n); bad[0] = 0xA7;
  TEST_ASSERT_FALSE(nav_route_unpack(bad, n, &g));
  memcpy(bad, buf, n); bad[1] = 2;
  TEST_ASSERT_FALSE(nav_route_unpack(bad, n, &g));
  memcpy(bad, buf, n); bad[3] = 0;
  TEST_ASSERT_FALSE(nav_route_unpack(bad, n, &g));
  memcpy(bad, buf, n); bad[3] = (uint8_t)(kNavRouteMax + 1);
  TEST_ASSERT_FALSE(nav_route_unpack(bad, n, &g));
  TEST_ASSERT_FALSE(nav_route_unpack(buf, n - 1, &g));
  memcpy(bad, buf, n); bad[n - 1] ^= 0xFF;
  TEST_ASSERT_FALSE(nav_route_unpack(bad, n, &g));
}

// ④ 混流：快照帧 + 路线帧 + 快照帧 一次喂进，两样都要上账
static void test_mixed_stream(void) {
  NavSource src;
  NavState st;

  NavFrame f;
  f.turn = (uint8_t)NavTurn::Right;
  f.dist_m = 120;
  f.road_len = 0;
  uint8_t snap[64];
  const size_t ns = nav_pack(f, snap, sizeof(snap));

  NavRoute r = makeRoute(4);
  uint8_t route[128];
  const size_t nr = nav_route_pack(r, route, sizeof(route));

  uint8_t both[256];
  size_t o = 0;
  memcpy(both + o, snap, ns); o += ns;
  memcpy(both + o, route, nr); o += nr;
  memcpy(both + o, snap, ns); o += ns;

  nav_source_push(src, both, o, 1000, st);
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_EQUAL_UINT16(120, st.dist_m);
  TEST_ASSERT_TRUE(st.route.valid);
  TEST_ASSERT_EQUAL_UINT8(4, st.route.count);
  TEST_ASSERT_EQUAL_INT16(30, st.route.pts[3].dx);
}

// ⑤ 路线超时降级：3 s 后不再画（但快照数据不受影响）
static void test_route_stale(void) {
  NavSource src;
  NavState st;
  NavRoute r = makeRoute(2);
  uint8_t buf[64];
  const size_t n = nav_route_pack(r, buf, sizeof(buf));
  nav_source_push(src, buf, n, 5000, st);
  TEST_ASSERT_TRUE(st.route.valid);
  TEST_ASSERT_FALSE(nav_route_tick(st.route, 5000 + 2999));
  TEST_ASSERT_TRUE(nav_route_tick(st.route, 5000 + 3000));
  TEST_ASSERT_FALSE(st.route.valid);
}

void register_nav_route_tests(void) {
  RUN_TEST(test_route_roundtrip);
  RUN_TEST(test_route_max_points);
  RUN_TEST(test_route_rejects);
  RUN_TEST(test_mixed_stream);
  RUN_TEST(test_route_stale);
}
