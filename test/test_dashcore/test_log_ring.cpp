// ============================================================
// 日志环 + 节流闸门的判据层（2026-09-26 新增）
//
// 这一组回答的是本单那个**唯一目标**："主循环永不因日志阻塞"。
// 起因（实测原文见 `ARCHITECTURE.md` §7.5.6 / `ACCEPTANCE.md` 同名那一节）：
//   同一个镜像、同一次上电 —— 有人读 `COM8` 时主循环 **3.3 万圈/秒**；
//   没人读的那 28 秒里**一共只跑了 2 圈**（20120 ms + 7689 ms）。
//   根因是 `dash_logf()` 当场 `Serial.write()`，而 HWCDC 在"没人读"时
//   会一路撞驱动超时（`tx_timeout_ms = 100` × `max_consec_timeouts = 20`）。
//   而**车上的常态就是没人读** ⇒ 装车即废。
//
// 修法在 `lib/dashcore/dash_log.h`：`dash_logf()` 只进环、排空按预算 + 先问能不能写。
// 这一组钉的正是那三条**契约**（也是"改坏了会怎样"的读数）：
//
//   ① **环满 = 整行丢 + 计数**，而且 `write()` 的返回值必须是 0
//      —— 一个字节都不许进（半行会把剩下的日志结构打乱，见文件头）；
//   ② **排空有预算且会停手**：sink 说"写不出去"（返回 0）时必须**立刻**返回，
//      不许重试、不许忙等 —— 这是"永不阻塞"那句在代码里唯一的位置；
//   ③ **丢弃/挡住必须可观测**（`droppedBytes()` / `dropCount()` / `blockedDrains()`），
//      它们会出现在串口 5 秒摘要那一行的 `log … drop=… blocked=…` 里。
//
// ★ 另外还钉了节流闸门（`RateGate`）：本单把它挂在 `206 dash ok` 与两行 `rgb:`
//   周期性遥测上。闸门本身只有两种行为（到点放行 / 值变了放行），
//   而"哪些行**不许**挂闸门"是纪律、不是代码 —— 写在 `dash_log.h` 与调用点注释里。
// ============================================================
#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "dash_log.h"

// ------------------------------------------------------------
// 测试用的替身：一块小环 + 一个"想堵就堵"的端口（sink）
// ------------------------------------------------------------
namespace {

// 512 B 的小环，方便"填满"（设备上是 8192 B，见 DASHLOG_RING_BYTES）。
constexpr size_t kTestCap = 512u;

struct Harness {
  dashlogring::Ring ring;
  uint8_t storage[kTestCap];
  // 端口替身
  uint8_t out[4096];
  size_t out_len = 0;
  size_t max_chunk = 64u;   // 一次最多吃多少（模拟 availableForWrite 的粒度）
  bool open = true;         // false = "没人读/端口满" ⇒ sink 一律返回 0
  size_t calls = 0;         // sink 被调了几次（判"有没有重试"）

  void init() {
    out_len = 0;
    max_chunk = 64u;
    open = true;
    calls = 0;
    ring.init(storage, sizeof(storage));
  }

  // 排空一次（budget = 这一轮最多交付多少字节）
  size_t drain(size_t budget) {
    return ring.drain(budget, [this](const uint8_t* p, size_t n) -> size_t {
      ++calls;
      if (!open) return 0u;                       // "端口满" ⇒ 一个字节都不写
      size_t take = (n < max_chunk) ? n : max_chunk;
      if (out_len + take > sizeof(out)) take = sizeof(out) - out_len;
      for (size_t i = 0; i < take; ++i) out[out_len + i] = p[i];
      out_len += take;
      return take;
    });
  }
};

// 造一段可辨认的载荷（首字节 = tag，后面是递增，方便查顺序）
void fill(uint8_t* dst, size_t n, uint8_t tag) {
  for (size_t i = 0; i < n; ++i) dst[i] = (uint8_t)(tag + i);
}

}  // namespace

// ============================================================
// 一、环满：**整段丢弃 + 计数**，绝不"塞一半"
// ============================================================
void test_logring_drops_whole_record_when_full(void) {
  Harness h;
  h.init();
  uint8_t rec[128];
  fill(rec, sizeof(rec), 0x10);

  // 512 / 128 = 4 条正好填满
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(512u, (unsigned)h.ring.size());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.space());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.droppedBytes());

  // ★ 第 5 条：放不下 ⇒ **一个字节都不进**，而且被记下来
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(512u, (unsigned)h.ring.size());   // 老数据一个字没动
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.droppedBytes());
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.ring.dropCount());

  // 再丢两条：计数是"字节"和"整段"两个口径
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(384u, (unsigned)h.ring.droppedBytes());
  TEST_ASSERT_EQUAL_UINT32(3u, (unsigned)h.ring.dropCount());
}

// ★ 这条是"整段丢弃"的**正面判据**：绝不出现"半条"
//   （半条会把剩下的日志结构打乱 —— 排障时最费时间的就是这个）
void test_logring_never_writes_a_partial_record(void) {
  Harness h;
  h.init();
  uint8_t big[400];
  fill(big, sizeof(big), 0xA0);
  uint8_t small[100];
  fill(small, sizeof(small), 0xB0);

  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.write(big, sizeof(big)));
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.size());

  // 100 ≤ 112（剩余空间）⇒ 进得去
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.ring.write(small, sizeof(small)));
  TEST_ASSERT_EQUAL_UINT32(500u, (unsigned)h.ring.size());

  // 现在只剩 12 B：任何 > 12 B 的记录都必须是"整段丢"
  uint8_t tiny[13];
  fill(tiny, sizeof(tiny), 0xC0);
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.write(tiny, sizeof(tiny)));
  TEST_ASSERT_EQUAL_UINT32(500u, (unsigned)h.ring.size());
  // 12 B 的还进得去（边界：**刚好放得下**不许被丢）
  uint8_t fit[12];
  fill(fit, sizeof(fit), 0xD0);
  TEST_ASSERT_EQUAL_UINT32(12u, (unsigned)h.ring.write(fit, sizeof(fit)));
  TEST_ASSERT_EQUAL_UINT32(512u, (unsigned)h.ring.size());
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.ring.dropCount());
}

// ============================================================
// 二、环是**环**：绕回去之后字节顺序仍然逐字节正确
// ============================================================
void test_logring_wraps_and_keeps_byte_order(void) {
  Harness h;
  h.init();
  uint8_t rec[100];
  char tag = 'a';
  // 写 5 条（500 B），再排掉 400 B ⇒ 头指针已经绕到中间
  for (int i = 0; i < 5; ++i) {
    fill(rec, sizeof(rec), (uint8_t)tag++);
    TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.ring.write(rec, sizeof(rec)));
  }
  h.max_chunk = 4096u;  // 这一轮让端口"一次吃得下"（否则会被 64 B 那一档挡住）
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.drain(400u));
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.ring.size());
  // 现在再写 4 条 ⇒ 一定跨过缓冲区末尾（环绕那一支）
  for (int i = 0; i < 4; ++i) {
    fill(rec, sizeof(rec), (uint8_t)tag++);
    TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.ring.write(rec, sizeof(rec)));
  }
  TEST_ASSERT_EQUAL_UINT32(500u, (unsigned)h.ring.size());

  h.out_len = 0;
  h.max_chunk = 4096u;  // 一次全拉出来，方便逐字节比对
  TEST_ASSERT_EQUAL_UINT32(500u, (unsigned)h.drain(4096u));
  TEST_ASSERT_EQUAL_UINT32(500u, (unsigned)h.out_len);

  // 期望：'e'(第 5 条) 之后是 'f' 'g' 'h' 'i'
  uint8_t expect[500];
  size_t off = 0;
  for (char c = 'e'; c <= 'i'; ++c) {
    fill(expect + off, 100u, (uint8_t)c);
    off += 100u;
  }
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, h.out, 500);
}

// ============================================================
// 三、排水**按预算**：一轮最多交付 budget 字节，剩下的留在环里（不丢！）
// ============================================================
void test_logring_drain_respects_budget(void) {
  Harness h;
  h.init();
  uint8_t rec[200];
  fill(rec, sizeof(rec), 0x30);
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.size());

  // budget = 100 ⇒ 这一轮只出 100，环里剩 300（**不丢**）
  h.max_chunk = 4096u;
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.drain(100u));
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.out_len);
  TEST_ASSERT_EQUAL_UINT32(300u, (unsigned)h.ring.size());

  // 再来三轮，把剩下的排完
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.drain(100u));
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.drain(100u));
  TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)h.drain(100u));
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.size());
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.out_len);
  // ★ 累计交付 = 400，累计丢弃 = 0（预算不是丢字节，是"晚一点送"）
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.drainedBytes());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.droppedBytes());
}

// ============================================================
// 四、★★ 端口写不出去时：**立刻停手**（不重试、不忙等、不丢）
//   这就是"主循环永不因日志阻塞"那句判据在代码里唯一的位置。
// ============================================================
void test_logring_drain_stops_at_once_when_port_is_blocked(void) {
  Harness h;
  h.init();
  uint8_t rec[128];
  fill(rec, sizeof(rec), 0x40);
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(256u, (unsigned)h.ring.size());

  h.open = false;   // ← "没人读串口"这个条件
  const size_t moved = h.drain(512u);

  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)moved);          // 一个字节都没出去
  TEST_ASSERT_EQUAL_UINT32(256u, (unsigned)h.ring.size());  // 数据还在（不算丢）
  // ★ 关键：sink **只被调用了一次** —— 调用两次就意味着"它在重试"，
  //   而重试正是阻塞的来源（HWCDC 的 write() 内部就是这个形状）。
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.calls);
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.ring.blockedDrains());

  // 端口恢复 ⇒ 下一轮接着排（丢的那 0 字节照旧是 0）
  h.open = true;
  h.max_chunk = 4096u;
  TEST_ASSERT_EQUAL_UINT32(256u, (unsigned)h.drain(512u));
  TEST_ASSERT_EQUAL_UINT32(256u, (unsigned)h.out_len);
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.droppedBytes());
}

// ★ 端口"只吃一半"（`availableForWrite()` 报得比环里欠的小）：
//   这一轮到此为止，**剩下的下一轮**，同样不许重试。
void test_logring_drain_stops_when_port_accepts_only_part(void) {
  Harness h;
  h.init();
  uint8_t rec[128];
  fill(rec, sizeof(rec), 0x50);
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(128u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(512u, (unsigned)h.ring.size());

  h.max_chunk = 30u;                       // 一次只吃 30 B
  const size_t moved = h.drain(512u);
  TEST_ASSERT_EQUAL_UINT32(30u, (unsigned)moved);          // 就这一口
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.calls);         // 没有"再来一口"
  TEST_ASSERT_EQUAL_UINT32(482u, (unsigned)h.ring.size()); // 剩下的都还在
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.ring.blockedDrains());
}

// ============================================================
// 五、统计可观测（判据：串口摘要行里的 `log … drop=… blocked=…`）
// ============================================================
void test_logring_stats_are_observable(void) {
  Harness h;
  h.init();
  uint8_t rec[200];
  fill(rec, sizeof(rec), 0x60);

  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.write(rec, sizeof(rec)));
  // ★ "环里最满到过多少"就在这一刻读：200 B 在环里。
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.highWaterMark());
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.writtenBytes());
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.writtenBytes());
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.highWaterMark());
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.size());

  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.write(rec, sizeof(rec)));  // 满 ⇒ 丢
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.droppedBytes());
  TEST_ASSERT_EQUAL_UINT32(1u, (unsigned)h.ring.dropCount());
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.writtenBytes());  // 丢的**不算**写过

  h.max_chunk = 4096u;
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.drain(4096u));
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)h.ring.drainedBytes());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.blockedDrains());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.size());  // 排空了

  // `clear()`：显式清屏那一路（返回被丢掉的字节数）
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.write(rec, sizeof(rec)));
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.clear());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)h.ring.size());
  // ★ 清掉的不算 "dropped"（那是"环满丢的"）—— 两个口径不能混。
  TEST_ASSERT_EQUAL_UINT32(200u, (unsigned)h.ring.droppedBytes());
}

// ★ 设备端那个 8 KB 的环：容量口径必须与 `DASHLOG_RING_BYTES` 一致
//   （"环多大"是排障时要对的一个数 —— 它决定了"没人读时能攒多久"）。
void test_logring_device_capacity_matches_constant(void) {
  TEST_ASSERT_EQUAL_UINT32((unsigned)DASHLOG_RING_BYTES, (unsigned)dashlog::ring().capacity());
  TEST_ASSERT_EQUAL_UINT32((unsigned)DASHLOG_DRAIN_BUDGET, 512u);
#if DASHLOG_RING_BYTES < 4096u
  TEST_FAIL_MESSAGE("环太小：没人读时攒不下几行日志（见 dash_log.h 的说明）");
#endif
}

// ============================================================
// 六、节流闸门（`RateGate`）：到点放行 / 值变了放行
//   ★ 这一层是"顺手节流"那一半的判据。**哪些行不许挂闸门**是纪律（写在各调用点），
//     这里只钉闸门本身的行为。
// ============================================================
void test_rategate_releases_on_interval(void) {
  dashlogring::RateGate g(2000u);   // 与 `206 dash ok` / `rgb:` 同一档

  TEST_ASSERT_TRUE(g.take(0u));      // 第一次一定放行（last=0 ⇒ due）
  TEST_ASSERT_FALSE(g.take(1u));     // 还没到点
  TEST_ASSERT_FALSE(g.take(1999u));
  TEST_ASSERT_TRUE(g.take(2000u));   // 到点
  TEST_ASSERT_FALSE(g.take(3999u));
  TEST_ASSERT_TRUE(g.take(4000u));

  // ★ 时钟回绕（`millis()` 49.7 天那个坑）：差值的算法必须用无符号减法
  dashlogring::RateGate w(1000u);
  TEST_ASSERT_TRUE(w.take(0xFFFFF800u));
  TEST_ASSERT_FALSE(w.take(0xFFFFF900u));   // +256 ms，没过 1000
  TEST_ASSERT_TRUE(w.take(0x00000800u));    // 绕回来 +2304 ms ⇒ 到点
}

void test_rategate_changed_releases_on_value_change(void) {
  dashlogring::RateGate g(2000u);
  // 值没变 ⇒ 不放行（即使过了很久；它要回答的是"变了没"）
  TEST_ASSERT_TRUE(g.changed(0u, 7));
  TEST_ASSERT_FALSE(g.changed(5000u, 7));
  TEST_ASSERT_FALSE(g.changed(9000u, 7));
  // 变了 ⇒ 放行
  TEST_ASSERT_TRUE(g.changed(10000u, 8));
  TEST_ASSERT_FALSE(g.changed(11000u, 8));
  TEST_ASSERT_TRUE(g.changed(12000u, 9));
  // 大整数（守护那几个累计计数用的就是这一档）
  dashlogring::RateGate b(2000u);
  TEST_ASSERT_TRUE(b.changed(0u, 4000000000ull));
  TEST_ASSERT_FALSE(b.changed(100u, 4000000000ull));
  TEST_ASSERT_TRUE(b.changed(200u, 4000000001ull));
}

// ★ 闸门的 interval 本身也要可读 —— 判据行里"为什么是 2 秒"要有出处。
void test_rategate_interval_is_queryable(void) {
  dashlogring::RateGate g(2000u);
  TEST_ASSERT_EQUAL_UINT32(2000u, (unsigned)g.interval());
  g.setInterval(5000u);
  TEST_ASSERT_EQUAL_UINT32(5000u, (unsigned)g.interval());
  TEST_ASSERT_TRUE(g.take(0u));
  TEST_ASSERT_FALSE(g.take(4999u));
  TEST_ASSERT_TRUE(g.take(5000u));
}

// ============================================================
// 七、★ 本单的**核心判据**：进环这条路与"端口堵不堵"完全无关
//   `write()` 是纯内存拷贝 ⇒ 同样的写入量，端口关着与开着**结果逐字节相同**
//   （差别只在"排不排得出去"，不在"写不写得进去"）。
// ============================================================
void test_logring_write_is_independent_of_port_state(void) {
  uint8_t rec[100];
  fill(rec, sizeof(rec), 0x70);

  Harness openp;
  openp.init();
  openp.open = true;
  Harness shut;
  shut.init();
  shut.open = false;

  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)openp.ring.write(rec, sizeof(rec)));
    TEST_ASSERT_EQUAL_UINT32(100u, (unsigned)shut.ring.write(rec, sizeof(rec)));
  }
  TEST_ASSERT_EQUAL_UINT32((unsigned)openp.ring.size(), (unsigned)shut.ring.size());
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)shut.ring.size());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)shut.ring.droppedBytes());  // 还没满 ⇒ 不丢

  // 排空：开着的排出去 400；关着的**一个字节都没出去**，但**也一个字节都没丢**
  openp.max_chunk = 4096u;
  shut.max_chunk = 4096u;
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)openp.drain(512u));
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)shut.drain(512u));
  TEST_ASSERT_EQUAL_UINT32(400u, (unsigned)shut.ring.size());
  TEST_ASSERT_EQUAL_UINT32(0u, (unsigned)shut.ring.droppedBytes());
}

// ============================================================
// 注册
// ============================================================
void register_log_ring_tests(void) {
  RUN_TEST(test_logring_drops_whole_record_when_full);
  RUN_TEST(test_logring_never_writes_a_partial_record);
  RUN_TEST(test_logring_wraps_and_keeps_byte_order);
  RUN_TEST(test_logring_drain_respects_budget);
  RUN_TEST(test_logring_drain_stops_at_once_when_port_is_blocked);
  RUN_TEST(test_logring_drain_stops_when_port_accepts_only_part);
  RUN_TEST(test_logring_stats_are_observable);
  RUN_TEST(test_logring_device_capacity_matches_constant);
  RUN_TEST(test_rategate_releases_on_interval);
  RUN_TEST(test_rategate_changed_releases_on_value_change);
  RUN_TEST(test_rategate_interval_is_queryable);
  RUN_TEST(test_logring_write_is_independent_of_port_state);
}
