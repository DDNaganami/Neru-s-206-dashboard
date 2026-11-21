// 导航字节流的**粘包/半包**用例。BLE 一次 notify 不是一帧：可能只给半帧、
// 也可能两帧挤在一起、还可能夹着日志噪音。这一层若错，屏上会出现
// "距离变成 65535" 或 "永远不动" 这类无法归因的症状，所以逐条钉住。
#include <unity.h>

#include "nav_proto.h"
#include "nav_source.h"

static NavFrame frame_a() {
  NavFrame f;
  f.turn = (uint8_t)NavTurn::Right;
  f.dist_m = 400;
  f.speed_kph = 55;
  f.heading_cdeg = 9000;
  f.seq = 1;
  f.road_len = 0;
  return f;
}

static NavFrame frame_b() {
  NavFrame f = frame_a();
  f.turn = (uint8_t)NavTurn::Left;
  f.dist_m = 120;
  f.speed_kph = 30;
  f.heading_cdeg = 18000;
  f.seq = 2;
  const char* r = "长安街";
  f.road_len = (uint8_t)strlen(r);
  memcpy(f.road, r, f.road_len);
  return f;
}

// ① 一整帧一次喂进
static void test_single_frame(void) {
  NavSource src;
  NavState st;
  uint8_t buf[64];
  const size_t n = nav_pack(frame_a(), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)nav_source_push(src, buf, n, 1000, st));
  TEST_ASSERT_TRUE(st.valid);
  TEST_ASSERT_EQUAL_UINT16(400, st.dist_m);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)NavTurn::Right, st.turn);
}

// ② 半包：每次只喂 1 字节，凑够才上账（且只上账一次）
static void test_byte_by_byte(void) {
  NavSource src;
  NavState st;
  uint8_t buf[64];
  const size_t n = nav_pack(frame_b(), buf, sizeof(buf));
  size_t applied = 0;
  for (size_t i = 0; i < n; ++i) applied += nav_source_push(src, &buf[i], 1, 2000, st);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)applied);
  TEST_ASSERT_EQUAL_STRING("长安街", st.road);
  TEST_ASSERT_EQUAL_UINT32(1, src.frames);
}

// ③ 两帧粘在一起：一次喂进要解出 2 帧，且字段是**后一帧**的
static void test_two_frames_glued(void) {
  NavSource src;
  NavState st;
  uint8_t a[64], b[64];
  const size_t na = nav_pack(frame_a(), a, sizeof(a));
  const size_t nb = nav_pack(frame_b(), b, sizeof(b));
  uint8_t both[128];
  memcpy(both, a, na);
  memcpy(both + na, b, nb);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)nav_source_push(src, both, na + nb, 3000, st));
  TEST_ASSERT_EQUAL_UINT16(120, st.dist_m);      // 后一帧覆盖
  TEST_ASSERT_EQUAL_STRING("长安街", st.road);
}

// ④ 前面夹噪音：魔数之前的字节被丢弃并计数，帧照收
static void test_garbage_prefix(void) {
  NavSource src;
  NavState st;
  uint8_t buf[64];
  const size_t n = nav_pack(frame_a(), buf, sizeof(buf));
  uint8_t noisy[80] = {0x01, 0x02, 'x', 'y', 0xA7, 0x00};   // 注意第 5 个字节是"假魔数"
  memcpy(noisy + 6, buf, n);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)nav_source_push(src, noisy, 6 + n, 4000, st));
  TEST_ASSERT_EQUAL_UINT16(400, st.dist_m);
  TEST_ASSERT_TRUE(src.garbage_bytes > 0);
  TEST_ASSERT_TRUE(src.crc_errors > 0);          // 假魔数那一次被当坏帧跳过
}

// ⑤ 坏 CRC 之后紧跟真帧：丢 1 字节重新对齐后必须还能收到真帧
static void test_resync_after_bad_crc(void) {
  NavSource src;
  NavState st;
  uint8_t buf[64];
  const size_t n = nav_pack(frame_a(), buf, sizeof(buf));
  buf[n - 1] ^= 0xFF;                             // 弄坏 CRC
  uint8_t stream[128];
  memcpy(stream, buf, n);
  memcpy(stream + n, buf, n);                     // 第二份是好的
  // 还原第二份的 CRC
  NavFrame f = frame_a();
  uint8_t good[64];
  const size_t ng = nav_pack(f, good, sizeof(good));
  memcpy(stream + n, good, ng);
  const size_t applied = nav_source_push(src, stream, n + ng, 5000, st);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)applied);
  TEST_ASSERT_EQUAL_UINT16(400, st.dist_m);
  TEST_ASSERT_TRUE(src.crc_errors > 0);
}

void register_nav_source_tests(void) {
  RUN_TEST(test_single_frame);
  RUN_TEST(test_byte_by_byte);
  RUN_TEST(test_two_frames_glued);
  RUN_TEST(test_garbage_prefix);
  RUN_TEST(test_resync_after_bad_crc);
}
