// 地图图片分片（0xA9）与重组的用例。
// 为什么测：图片是**多片**，丢一片/乱序/跨帧都会让"整图"永远收不齐或拼错——
// 而拼错的 JPEG 在屏上只是"解码失败"或花屏，看不出是传输问题 ⇒ 必须在这一层钉住。
#include <unity.h>
#include <string.h>

#include "nav_img.h"
#include "nav_source.h"

static uint8_t g_data[64];

static void fill(uint8_t* p, size_t n, uint8_t seed) {
  for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(seed + i * 7u);
}

// ① 单片的打包/解包往返（含 CRC 覆盖范围）
static void test_img_chunk_roundtrip(void) {
  fill(g_data, 10, 1);
  NavImgChunk c;
  c.frame_id = 3; c.idx = 1; c.total = 4; c.len = 10; c.data = g_data;
  uint8_t buf[64];
  const size_t n = nav_img_pack_chunk(c, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(7u + 10u + 2u, (uint32_t)n);
  NavImgChunk g;
  TEST_ASSERT_TRUE(nav_img_unpack_chunk(buf, n, &g));
  TEST_ASSERT_EQUAL_UINT8(3, g.frame_id);
  TEST_ASSERT_EQUAL_UINT8(1, g.idx);
  TEST_ASSERT_EQUAL_UINT8(4, g.total);
  TEST_ASSERT_EQUAL_UINT16(10, g.len);
  TEST_ASSERT_EQUAL_MEMORY(g_data, g.data, 10);
}

// ② 坏片一律拒绝（魔数/版本/越界 idx/total/长度不匹配/CRC）
static void test_img_chunk_rejects(void) {
  fill(g_data, 8, 2);
  NavImgChunk c;
  c.frame_id = 1; c.idx = 0; c.total = 2; c.len = 8; c.data = g_data;
  uint8_t buf[64];
  const size_t n = nav_img_pack_chunk(c, buf, sizeof(buf));
  NavImgChunk g;
  uint8_t bad[64];

  memcpy(bad, buf, n); bad[0] = 0xA8;
  TEST_ASSERT_FALSE(nav_img_unpack_chunk(bad, n, &g));
  memcpy(bad, buf, n); bad[1] = 9;
  TEST_ASSERT_FALSE(nav_img_unpack_chunk(bad, n, &g));
  memcpy(bad, buf, n); bad[3] = 2;      // idx >= total
  TEST_ASSERT_FALSE(nav_img_unpack_chunk(bad, n, &g));
  memcpy(bad, buf, n); bad[4] = 0;      // total = 0
  TEST_ASSERT_FALSE(nav_img_unpack_chunk(bad, n, &g));
  TEST_ASSERT_FALSE(nav_img_unpack_chunk(buf, n - 1, &g));
  memcpy(bad, buf, n); bad[n - 1] ^= 0xFF;
  TEST_ASSERT_FALSE(nav_img_unpack_chunk(bad, n, &g));
}

// ③ 乱序收齐：3 片按 2,0,1 顺序到达，仍应拼成原图
static void test_img_reassemble_out_of_order(void) {
  uint8_t img[3 * kNavImgChunkMax];
  fill(img, 3 * 256, 5);
  NavImgRx rx;
  nav_img_rx_init(rx, nullptr, 0);           // 先确认没缓冲时安全
  uint8_t buf[3 * kNavImgChunkMax];
  nav_img_rx_init(rx, buf, sizeof(buf));

  const uint8_t order[3] = {2, 0, 1};
  bool done = false;
  for (int i = 0; i < 3; ++i) {
    NavImgChunk c;
    c.frame_id = 7; c.total = 3; c.idx = order[i]; c.len = 256;
    c.data = img + (size_t)order[i] * 256;
    NavImgChunk packed = c;
    uint8_t frame[300];
    const size_t n = nav_img_pack_chunk(packed, frame, sizeof(frame));
    NavImgChunk g;
    TEST_ASSERT_TRUE(nav_img_unpack_chunk(frame, n, &g));
    done = nav_img_rx_push(rx, g);
  }
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_MEMORY(img, buf, 3 * 256);
  TEST_ASSERT_EQUAL_UINT32(3 * 256, (uint32_t)rx.len);
}

// ④ 丢片：永远收不齐（回来的是 false，不会误判完成）
static void test_img_missing_chunk_never_completes(void) {
  uint8_t buf[3 * kNavImgChunkMax];
  NavImgRx rx;
  nav_img_rx_init(rx, buf, sizeof(buf));
  uint8_t data[256];
  fill(data, 256, 9);
  for (uint8_t i = 0; i < 2; ++i) {          // 只发 0、1，缺 2
    NavImgChunk c;
    c.frame_id = 1; c.total = 3; c.idx = i; c.len = 256; c.data = data;
    TEST_ASSERT_FALSE(nav_img_rx_push(rx, c));
  }
  TEST_ASSERT_FALSE(rx.complete);
  TEST_ASSERT_EQUAL_UINT8(2, rx.got);
}

// ⑤ 跨帧：新 frame_id 到达 ⇒ 丢弃旧进度（旧的半张图不许和新图拼在一起）
static void test_img_new_frame_resets(void) {
  uint8_t buf[2 * kNavImgChunkMax];
  NavImgRx rx;
  nav_img_rx_init(rx, buf, sizeof(buf));
  uint8_t a[256], b[256];
  fill(a, 256, 1); fill(b, 256, 2);

  NavImgChunk c1;
  c1.frame_id = 1; c1.total = 2; c1.idx = 0; c1.len = 256; c1.data = a;
  TEST_ASSERT_FALSE(nav_img_rx_push(rx, c1));

  NavImgChunk c2;                              // 帧 2 的第 0 片直接取代旧进度
  c2.frame_id = 2; c2.total = 2; c2.idx = 0; c2.len = 256; c2.data = b;
  TEST_ASSERT_FALSE(nav_img_rx_push(rx, c2));
  TEST_ASSERT_EQUAL_UINT8(1, rx.got);
  TEST_ASSERT_EQUAL_UINT8(2, rx.frame_id);
}

// ⑥ 走 nav_source：0xA9 分片与 0xA8 路线混在同一条流里都能上账
static void test_img_through_stream(void) {
  uint8_t buf[2 * kNavImgChunkMax];
  NavImgRx rx;
  nav_img_rx_init(rx, buf, sizeof(buf));

  NavSource src;
  src.img = &rx;

  NavState st;
  uint8_t img[2 * 256];
  fill(img, sizeof(img), 3);
  uint8_t frame0[300], frame1[300];
  NavImgChunk c0;
  c0.frame_id = 9; c0.total = 2; c0.idx = 0; c0.len = 256; c0.data = img;
  NavImgChunk c1 = c0; c1.idx = 1; c1.data = img + 256;
  const size_t n0 = nav_img_pack_chunk(c0, frame0, sizeof(frame0));
  const size_t n1 = nav_img_pack_chunk(c1, frame1, sizeof(frame1));

  NavFrame f;
  f.turn = 2; f.dist_m = 55;
  uint8_t snap[64];
  const size_t ns = nav_pack(f, snap, sizeof(snap));

  uint8_t stream[1024];
  size_t o = 0;
  memcpy(stream + o, snap, ns); o += ns;
  memcpy(stream + o, frame0, n0); o += n0;
  memcpy(stream + o, frame1, n1); o += n1;

  nav_source_push(src, stream, o, 1000, st);
  TEST_ASSERT_TRUE(st.valid);           // 快照帧照旧工作
  TEST_ASSERT_EQUAL_UINT16(55, st.dist_m);
  TEST_ASSERT_EQUAL_UINT32(1, src.img_ready);
  TEST_ASSERT_TRUE(rx.complete);
  TEST_ASSERT_EQUAL_MEMORY(img, buf, 512);
}

void register_nav_img_tests(void) {
  RUN_TEST(test_img_chunk_roundtrip);
  RUN_TEST(test_img_chunk_rejects);
  RUN_TEST(test_img_reassemble_out_of_order);
  RUN_TEST(test_img_missing_chunk_never_completes);
  RUN_TEST(test_img_new_frame_resets);
  RUN_TEST(test_img_through_stream);
}
