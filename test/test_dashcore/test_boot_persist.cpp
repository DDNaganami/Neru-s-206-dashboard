// ============================================================
// 跨重启留档（BootPersist）—— 2026-09-25 新增
//
// 这一组回答的是**昨晚那件悬案**："累计启动次数 31 → 32 多了一次，可是上一次的
// 复位原因没有留档 ⇒ 判不出那次是 USB 被断电（POWERON）还是板子自己掉电（BROWNOUT）"。
// 每一条都对着 `boot_persist.h` 文件头里的一个判据：
//
//   ① NVS 空（新板 / 第一次跑带本层的固件）⇒ 各项报"没有记录"，**不许**假装 0
//      —— ★ 但**也不许**把"空"当成"坏"：`nvs_ok` 的判据是**写**，不是读（这一条踩过一次）；
//   ② 开机时把"本次 reason + 本次 uptime"写下去 ⇒ 下一次开机读到的就是**这一次**；
//   ③ "上一次跑了多久" = `max(最后一次心跳, 上一次开机时的 uptime)`（对上一次的下界）；
//   ④ **心跳到 10 分钟才写**（不是每个 tick 都写 —— 那会把 NVS 写穿）；
//   ⑤ 边界：上一次跑了 0ms / 超过 10 分钟 / 心跳键没有了 / 启动计数键被删；
//   ⑥ 守护四个计数**跨重启累加**（基线 + 本次），且累计落盘次数跨重启单调 +1；
//   ⑦ `heartbeat_due` 的 49.7 天回绕（`millis()` 溢出）不能判错；
//   ⑧ 写失败不许当成功（`nvs_ok` 翻假、心跳次数不前进）。
//
// ★ 为什么用**假 NVS + 假时钟**：宿主机上没有 flash、没有 NVS，也没有 `millis()`
//   （与 test_panel_guard.cpp / test_buzzer_exio.cpp 同一条纪律）。
//   而"哪几个键被写了、写成了什么"只有把注入回调收到的东西记下来才能逐条断言。
// ============================================================
#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "boot_persist.h"

namespace {

// ---- 假 NVS：一张小表，带"这个键在不在"的标记 ----
const int kKeyMax = (int)BootKey::Count;

struct FakeNvs {
  bool     has_u[kKeyMax]   = {};      // 整数键在不在
  uint32_t u[kKeyMax]       = {};      // 整数值
  bool     has_s[kKeyMax]   = {};
  char     s[kKeyMax][24]   = {};

  int      reads = 0;                  // 读过几次（判"心跳不乱读"）
  int      writes = 0;                 // 写过几次
  bool     fail_write = false;         // 注入"写失败"（NVS 写不动）
  bool     fail_read  = false;         // 注入"读失败"（NVS 读不动）

  int      wr_key[64] = {};            // 写过哪些键（按顺序）
  uint32_t wr_val[64] = {};
  int      wr_snap[64] = {};           // 第几次写（用来判"一次心跳写了几个键"）
  int      wr_n = 0;
};

FakeNvs N;

bool nvRead(BootKey k, uint32_t* out, void*) {
  ++N.reads;
  if (N.fail_read) return false;
  const int i = (int)k;
  if (i < 0 || i >= kKeyMax) return false;
  if (!N.has_u[i]) return false;        // 没有这个键 ⇒ "没有记录"
  *out = N.u[i];
  return true;
}

bool nvWrite(BootKey k, uint32_t v, void*) {
  if (N.fail_write) return false;
  const int i = (int)k;
  if (i < 0 || i >= kKeyMax) return false;
  N.has_u[i] = true;
  N.u[i] = v;
  ++N.writes;
  if (N.wr_n < 64) {
    N.wr_key[N.wr_n] = i;
    N.wr_val[N.wr_n] = v;
    N.wr_snap[N.wr_n] = N.writes;       // 用累计写次数当"第几笔"
  }
  ++N.wr_n;
  return true;
}

bool nvReadStr(BootKey k, char* out, uint32_t cap, void*) {
  if (N.fail_read) return false;
  const int i = (int)k;
  if (i < 0 || i >= kKeyMax || !N.has_s[i]) return false;
  for (uint32_t j = 0; j < cap; ++j) {
    out[j] = N.s[i][j];
    if (out[j] == '\0') return true;
  }
  out[cap - 1u] = '\0';
  return true;
}

bool nvWriteStr(BootKey k, const char* s, void*) {
  if (N.fail_write) return false;
  const int i = (int)k;
  if (i < 0 || i >= kKeyMax) return false;
  N.has_s[i] = true;
  uint32_t j = 0;
  for (; j + 1u < sizeof(N.s[i]) && s[j] != '\0'; ++j) N.s[i][j] = s[j];
  N.s[i][j] = '\0';
  return true;
}

void nvsReset() { N = FakeNvs{}; }

// 假 NVS 里"上一个进程"留下的东西。
void seedKeyU(BootKey k, uint32_t v) { N.has_u[(int)k] = true; N.u[(int)k] = v; }
void seedKeyS(BootKey k, const char* v) {
  N.has_s[(int)k] = true;
  uint32_t j = 0;
  for (; j + 1u < sizeof(N.s[(int)k]) && v[j] != '\0'; ++j) N.s[(int)k][j] = v[j];
  N.s[(int)k][j] = '\0';
}

// 写一笔整数键的计数（判"心跳一次写了几个键"用）。
int writesOfKey(BootKey k) {
  int n = 0;
  for (int i = 0; i < N.wr_n && i < 64; ++i) {
    if (N.wr_key[i] == (int)k) ++n;
  }
  return n;
}
GuardTotals T(uint32_t rd, uint32_t fix, uint32_t bl, uint32_t anom) {
  GuardTotals t;
  t.rd = rd; t.fix = fix; t.bl = bl; t.anom = anom;
  return t;
}

BootPersist mk() { return BootPersist(nvRead, nvWrite, nvReadStr, nvWriteStr, nullptr); }

}  // namespace

// ============================================================
// ① NVS 空：不许假装有记录 / 不许假装 0
// ============================================================
void test_bootpersist_first_boot_on_blank_nvs(void) {
  nvsReset();
  BootPersist b = mk();
  const BootInfo in = b.begin(218u, 1u, "POWERON");   // 典型：开机 218ms 时打点

  TEST_ASSERT_EQUAL_STRING("POWERON", in.reason_name);
  TEST_ASSERT_EQUAL_UINT32(1u, in.reason_raw);
  TEST_ASSERT_EQUAL_UINT32(1u, in.boot_count);        // 第一次跑带本层的固件 ⇒ n=1
  TEST_ASSERT_FALSE(in.prev_valid);                   // ★ 没有"上一次" —— 如实说没有
  TEST_ASSERT_EQUAL_UINT32(kBootUpUnknown, in.prev_up_ms);
  TEST_ASSERT_TRUE(in.nvs_ok);                        // 空 NVS 不是坏 NVS（键已经写下去了）

  // ★ 本次的三项必须已经落盘（这是"下一次开机知道这一次为什么起来"的唯一保证）
  TEST_ASSERT_TRUE(N.has_u[(int)BootKey::PrevReasonRaw]);
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::PrevReasonRaw]);
  TEST_ASSERT_TRUE(N.has_s[(int)BootKey::PrevReason]);
  TEST_ASSERT_EQUAL_STRING("POWERON", N.s[(int)BootKey::PrevReason]);
  TEST_ASSERT_TRUE(N.has_u[(int)BootKey::PrevUpMs]);
  TEST_ASSERT_EQUAL_UINT32(218u, N.u[(int)BootKey::PrevUpMs]);
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::BootCount]);
}

// ============================================================
// ② ★★ 本单的核心：又一次开机 ⇒ 读回**上一次**的 reason + 上一次跑了多久
//    （这正是昨晚缺的那一格：`31 → 32` 那一次到底是 POWERON 还是 BROWNOUT）
// ============================================================
void test_bootpersist_second_boot_recovers_prev(void) {
  nvsReset();
  // 上一个进程：BROWNOUT 起来、跑了 27 分钟、最后一次心跳记在 27 分整。
  seedKeyS(BootKey::PrevReason, "BROWNOUT");
  seedKeyU(BootKey::PrevReasonRaw, 9u);              // ESP_RST_BROWNOUT = 9（本工程那版 IDF）
  seedKeyU(BootKey::PrevUpMs, 30000u);               // 那次开机时 30ms 打点
  seedKeyU(BootKey::HeartbeatMs, 1620000u);          // = 27 分钟
  seedKeyU(BootKey::HeartbeatN, 2u);
  seedKeyU(BootKey::BootCount, 32u);

  BootPersist b = mk();
  const BootInfo in = b.begin(205u, 1u, "POWERON");  // 这一次是上电（车主插 USB）

  TEST_ASSERT_TRUE(in.prev_valid);
  TEST_ASSERT_EQUAL_STRING("BROWNOUT", in.prev_reason_name);   // ★ 悬案的那一格
  TEST_ASSERT_EQUAL_UINT32(9u, in.prev_reason_raw);
  TEST_ASSERT_EQUAL_UINT32(1620000u, in.prev_up_ms);           // 27 分钟
  TEST_ASSERT_EQUAL_UINT32(27u, boot_minutes(in.prev_up_ms));
  TEST_ASSERT_EQUAL_UINT32(33u, in.boot_count);                // 31→32→33 这一套口径
  TEST_ASSERT_TRUE(in.nvs_ok);
  // 本次已经把自己的 reason 覆盖成"当前这一次"（下一次开机读到的就是它）
  TEST_ASSERT_EQUAL_STRING("POWERON", N.s[(int)BootKey::PrevReason]);
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::PrevReasonRaw]);
  TEST_ASSERT_EQUAL_UINT32(205u, N.u[(int)BootKey::PrevUpMs]);  // ★ 本次的 uptime = 0min
  TEST_ASSERT_EQUAL_UINT32(33u, N.u[(int)BootKey::BootCount]);
}

// ============================================================
// ③ "上一次跑了多久"的算法与边界
// ============================================================
void test_bootpersist_prev_up_boundaries(void) {
  // 都没有记录 ⇒ 未知（不是 0）
  TEST_ASSERT_EQUAL_UINT32(kBootUpUnknown,
                           boot_prev_up_ms(0u, false, 0u, false));
  // 只有心跳 ⇒ 用心跳那个
  TEST_ASSERT_EQUAL_UINT32(600000u, boot_prev_up_ms(600000u, true, 0u, false));
  // 只有"上一次开机时的 uptime" ⇒ 用它（**0ms 也是一个合法读数**：上一次刚起来就重启了）
  TEST_ASSERT_EQUAL_UINT32(0u, boot_prev_up_ms(0u, false, 0u, true));
  TEST_ASSERT_EQUAL_UINT32(0u, boot_minutes(0u));
  // 两个都有 ⇒ 取更大的那个（开机时的 uptime 一般更小 ⇒ 心跳赢）
  TEST_ASSERT_EQUAL_UINT32(1620000u, boot_prev_up_ms(1620000u, true, 30000u, true));
  // 反过来（心跳更小：例如心跳之后又跑了一段才重启）⇒ 仍是取大者
  TEST_ASSERT_EQUAL_UINT32(1800000u, boot_prev_up_ms(1620000u, true, 1800000u, true));
  // 哨兵不许被当成一个真值参与比较
  TEST_ASSERT_EQUAL_UINT32(30000u,
                           boot_prev_up_ms(kBootUpUnknown, true, 30000u, true));
  // 换算：向下取整（`prev_up=9min` 一定意味着"不到 10 分钟"）
  TEST_ASSERT_EQUAL_UINT32(9u, boot_minutes(599999u));
  TEST_ASSERT_EQUAL_UINT32(10u, boot_minutes(600000u));
  TEST_ASSERT_EQUAL_UINT32(0u, boot_minutes(59999u));
  TEST_ASSERT_EQUAL_UINT32(60u, boot_minutes(3600000u));   // 一小时
}

// ============================================================
// ④ ★ 心跳：到 10 分钟才写（不是每个 tick 都写）
// ============================================================
void test_bootpersist_heartbeat_only_at_10min(void) {
  nvsReset();
  BootPersist b = mk();
  b.begin(200u, 1u, "POWERON");
  const int w0 = N.writes;

  // 头 10 分钟：一个 tick 都不写
  TEST_ASSERT_FALSE(b.tick(200u, T(0, 0, 0, 0)));
  TEST_ASSERT_FALSE(b.tick(1000u, T(1, 0, 0, 0)));
  TEST_ASSERT_FALSE(b.tick(599999u, T(300, 0, 1, 1)));
  TEST_ASSERT_EQUAL_INT(w0, N.writes);                 // ★ NVS 一个字节都没动
  TEST_ASSERT_EQUAL_UINT32(0u, b.heartbeats());
  TEST_ASSERT_EQUAL_UINT32(0u, b.snapshots());

  // 到点（200 + 600000 = 600200）⇒ 写一次
  TEST_ASSERT_TRUE(b.tick(600200u, T(301, 1, 1, 2)));
  TEST_ASSERT_EQUAL_UINT32(600200u, N.u[(int)BootKey::HeartbeatMs]);
  TEST_ASSERT_EQUAL_UINT32(1u, b.heartbeats());
  TEST_ASSERT_EQUAL_UINT32(1u, b.snapshots());
  TEST_ASSERT_EQUAL_UINT32(600200u, b.lastHeartbeatMs());
  TEST_ASSERT_TRUE(N.has_u[(int)BootKey::HeartbeatN]);
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::HeartbeatN]);
  // ★ 一次心跳写了**固定 7 个键**（uptime / 心跳次数 / rd / fix / bl / anom / 落盘次数）
  //   —— 写 NVS 的次数就是"磨损"的账（10 分钟一次 ≈ 5.3 万次/年，见 boot_persist.h）。
  TEST_ASSERT_EQUAL_INT(w0 + 7, N.writes);
  TEST_ASSERT_EQUAL_INT(1, writesOfKey(BootKey::HeartbeatMs));
  // 到点之后再 tick 也不重复写（下一次要等又 10 分钟：600200 + 600000 = 1200200）
  const int w1 = N.writes;
  TEST_ASSERT_FALSE(b.tick(600201u, T(301, 1, 1, 2)));
  TEST_ASSERT_FALSE(b.tick(1200199u, T(600, 1, 1, 2)));   // 距下一次心跳到期还差 1ms
  TEST_ASSERT_EQUAL_INT(w1, N.writes);
  // 第二个 10 分钟 ⇒ 第二次心跳
  TEST_ASSERT_TRUE(b.tick(1200200u, T(601, 1, 1, 2)));
  TEST_ASSERT_EQUAL_UINT32(2u, b.heartbeats());
  TEST_ASSERT_EQUAL_UINT32(1200200u, N.u[(int)BootKey::HeartbeatMs]);
}

// ============================================================
// ⑤ `millis()` 回绕（49.7 天）不能把"到期"判错
// ============================================================
void test_bootpersist_heartbeat_due_wraps(void) {
  TEST_ASSERT_TRUE(boot_heartbeat_due(1000u, 1000u));          // 刚好到点
  TEST_ASSERT_TRUE(boot_heartbeat_due(1001u, 1000u));
  TEST_ASSERT_FALSE(boot_heartbeat_due(999u, 1000u));
  // 回绕：到期点在 UINT32_MAX 附近，now 已经绕到小值
  TEST_ASSERT_TRUE(boot_heartbeat_due(5u, 4294967000u));
  TEST_ASSERT_FALSE(boot_heartbeat_due(4294966000u, 5u));      // 反向：还没到
  // 饱和：`now + period` 溢出 ⇒ 不产生"永远不到期"，也不回绕成小值
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, boot_next_heartbeat(UINT32_MAX - 10u, kBootHeartbeatMs));
  TEST_ASSERT_EQUAL_UINT32(600200u, boot_next_heartbeat(200u, kBootHeartbeatMs));
}

// ============================================================
// ⑥ ★ 守护计数跨重启累计（基线 + 本次）
// ============================================================
void test_bootpersist_guard_totals_accumulate(void) {
  nvsReset();
  // 上一个进程留档：rd=42 fix=1 bl=0 anom=1、落盘过 5 次。
  seedKeyU(BootKey::GuardRd, 42u);
  seedKeyU(BootKey::GuardFix, 1u);
  seedKeyU(BootKey::GuardBl, 0u);
  seedKeyU(BootKey::GuardAnom, 1u);
  seedKeyU(BootKey::GuardSnaps, 5u);
  seedKeyU(BootKey::BootCount, 7u);

  BootPersist b = mk();
  const BootInfo in = b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_EQUAL_UINT32(42u, b.guardBase().rd);
  TEST_ASSERT_EQUAL_UINT32(1u, b.guardBase().fix);
  TEST_ASSERT_EQUAL_UINT32(0u, b.guardBase().bl);
  TEST_ASSERT_EQUAL_UINT32(1u, b.guardBase().anom);
  TEST_ASSERT_EQUAL_UINT32(5u, b.snapshots());
  TEST_ASSERT_EQUAL_UINT32(8u, in.boot_count);

  // 本次跑到 10 分钟、守护又干了活：rd=30、fix=0、bl=2、anom=3
  //   ⇒ 累计应当是 rd=72 fix=1 bl=2 anom=4（= 基线 + 本次）★ 不再随重启丢失
  TEST_ASSERT_TRUE(b.tick(600200u, T(30u, 0u, 2u, 3u)));
  TEST_ASSERT_EQUAL_UINT32(72u, N.u[(int)BootKey::GuardRd]);
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::GuardFix]);
  TEST_ASSERT_EQUAL_UINT32(2u, N.u[(int)BootKey::GuardBl]);
  TEST_ASSERT_EQUAL_UINT32(4u, N.u[(int)BootKey::GuardAnom]);
  TEST_ASSERT_EQUAL_UINT32(6u, N.u[(int)BootKey::GuardSnaps]);   // 落盘次数单调 +1

  // ★ 跨重启再走一遍：这一次开机读回的基线应当已经含上一次的尾巴
  BootPersist b2 = mk();
  const BootInfo in2 = b2.begin(210u, 1u, "POWERON");
  TEST_ASSERT_EQUAL_UINT32(72u, b2.guardBase().rd);
  TEST_ASSERT_EQUAL_UINT32(4u, b2.guardBase().anom);
  TEST_ASSERT_EQUAL_UINT32(9u, in2.boot_count);
  TEST_ASSERT_EQUAL_UINT32(600200u, in2.prev_up_ms);              // 上一次跑了 10 分钟
  TEST_ASSERT_EQUAL_UINT32(10u, boot_minutes(in2.prev_up_ms));
  // 本次再攒 8 次回读 ⇒ 累计 80
  TEST_ASSERT_TRUE(b2.tick(600300u, T(8u, 0u, 0u, 0u)));
  TEST_ASSERT_EQUAL_UINT32(80u, N.u[(int)BootKey::GuardRd]);
}

// 累计相加要饱和（不许回绕成 0 —— 那会让"救过几回"看起来是零）
void test_bootpersist_accum_saturates(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, boot_accum(0u, 0u));
  TEST_ASSERT_EQUAL_UINT32(72u, boot_accum(42u, 30u));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, boot_accum(UINT32_MAX, 1u));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, boot_accum(UINT32_MAX - 1u, 5u));
}

// ============================================================
// ⑦ 边界：启动计数键被删 / NVS 坏了 / 写失败
// ============================================================
void test_bootpersist_missing_bootcount_starts_at_one(void) {
  nvsReset();
  seedKeyU(BootKey::GuardRd, 3u);     // 有别的键（⇒ 不是一个空 NVS）
  BootPersist b = mk();
  const BootInfo in = b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_EQUAL_UINT32(1u, in.boot_count);        // 计数键没了 ⇒ 从 1 起算，而不是 0
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::BootCount]);
  TEST_ASSERT_TRUE(in.nvs_ok);                        // 空 NVS 不是坏 NVS（键已经写下去了）
}

// ★ 读不到某个键 ≠ NVS 坏了：**全新板子就是一个键都没有**（这一条踩过一次）
void test_bootpersist_read_miss_is_not_broken_nvs(void) {
  nvsReset();
  N.fail_read = true;                 // 每个键都读不到
  BootPersist b = mk();
  const BootInfo in = b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_TRUE(in.nvs_ok);        // ★ 写成功了 ⇒ 这一层是活的（不是 nvs-）
  TEST_ASSERT_FALSE(in.prev_valid);   // 但确实**没有**"上一次" ⇒ 显示 prev=-
  TEST_ASSERT_EQUAL_UINT32(kBootUpUnknown, in.prev_up_ms);
  TEST_ASSERT_EQUAL_UINT32(1u, in.boot_count);
  TEST_ASSERT_TRUE(N.has_u[(int)BootKey::BootCount]);    // 计数照样落下去了
  TEST_ASSERT_EQUAL_UINT32(1u, N.u[(int)BootKey::BootCount]);
  N.fail_read = false;
  TEST_ASSERT_TRUE(b.tick(600200u, T(1u, 0u, 0u, 0u)));  // 心跳照常
}

// ★ 但**写不下去**必须报出来（`nvs-`），而且不许去每 10 分钟撞一次墙
void test_bootpersist_nvs_write_failure_is_reported(void) {
  nvsReset();
  N.fail_write = true;
  BootPersist b = mk();
  const BootInfo in = b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_FALSE(in.nvs_ok);                        // ★ 报 nvs-，不假装成功
  TEST_ASSERT_EQUAL_UINT32(1u, in.boot_count);         // 计数仍然从 1 起算（不假数据）
  TEST_ASSERT_FALSE(b.tick(600200u, T(1u, 0u, 0u, 0u)));   // 没上线 ⇒ tick 什么都不做
  TEST_ASSERT_EQUAL_UINT32(0u, b.heartbeats());
  TEST_ASSERT_EQUAL_UINT32(0u, b.snapshots());
}

void test_bootpersist_heartbeat_write_failure_not_counted(void) {
  nvsReset();
  BootPersist b = mk();
  b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_TRUE(b.nvsOk());
  N.fail_write = true;
  TEST_ASSERT_FALSE(b.tick(600200u, T(10u, 0u, 0u, 0u)));  // 写失败 ⇒ 返回 false
  TEST_ASSERT_EQUAL_UINT32(0u, b.heartbeats());            // ★ 不许当成功（否则这一格撒谎）
  TEST_ASSERT_EQUAL_UINT32(0u, b.snapshots());
  TEST_ASSERT_FALSE(b.nvsOk());
  // ★ 写失败**不重试到成功为止**：下一个 tick 也不写（要等下一个 10 分钟）
  TEST_ASSERT_FALSE(b.tick(600300u, T(11u, 0u, 0u, 0u)));
  N.fail_write = false;
  TEST_ASSERT_TRUE(b.tick(1200400u, T(12u, 0u, 0u, 0u)));   // 下一个 10 分钟边界照常写
  TEST_ASSERT_EQUAL_UINT32(1u, b.heartbeats());
}

// ============================================================
// ⑧ flushNow：**即将重启**时手动落一次盘（不改心跳节拍）
// ============================================================
void test_bootpersist_flush_now(void) {
  nvsReset();
  BootPersist b = mk();
  b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_TRUE(b.flushNow(12345u, T(6u, 1u, 0u, 1u)));
  TEST_ASSERT_EQUAL_UINT32(12345u, N.u[(int)BootKey::HeartbeatMs]);
  TEST_ASSERT_EQUAL_UINT32(6u, N.u[(int)BootKey::GuardRd]);
  TEST_ASSERT_EQUAL_UINT32(1u, b.heartbeats());
  // ★ 心跳节拍不受影响：下一次仍然是"开机 + 10 分钟"，不是"flush + 10 分钟"
  TEST_ASSERT_FALSE(b.tick(600100u, T(7u, 1u, 0u, 1u)));
  TEST_ASSERT_TRUE(b.tick(600200u, T(7u, 1u, 0u, 1u)));
}

// ============================================================
// ⑨ prev= 的名字读回来之后，开机那一行能拼成文档里那个形状
//    （字符串拼接在 main.cpp，这里只钉"存量"这一半）
// ============================================================
void test_bootpersist_prev_name_roundtrip(void) {
  nvsReset();
  seedKeyS(BootKey::PrevReason, "TASK_WDT");     // 最长的那一类名字（8 字符）
  seedKeyU(BootKey::PrevReasonRaw, 6u);
  seedKeyU(BootKey::BootCount, 3u);
  BootPersist b = mk();
  const BootInfo in = b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_TRUE(in.prev_valid);
  TEST_ASSERT_EQUAL_STRING("TASK_WDT", in.prev_reason_name);
  TEST_ASSERT_EQUAL_UINT32(6u, in.prev_reason_raw);
  TEST_ASSERT_EQUAL_UINT32(4u, in.boot_count);
  TEST_ASSERT_TRUE(sizeof(in.prev_reason_name) >= 10u);   // "TASK_WDT" + NUL 必须放得下
}

// 只有原始值、名字没存下来 ⇒ 仍然算"有记录"（按 raw 显示）—— 少一档信息不算没记录
void test_bootpersist_prev_raw_only_is_still_valid(void) {
  nvsReset();
  seedKeyU(BootKey::PrevReasonRaw, 4u);
  seedKeyU(BootKey::BootCount, 11u);
  BootPersist b = mk();
  const BootInfo in = b.begin(200u, 1u, "POWERON");
  TEST_ASSERT_TRUE(in.prev_valid);
  TEST_ASSERT_EQUAL_UINT32(4u, in.prev_reason_raw);
  TEST_ASSERT_EQUAL_STRING("", in.prev_reason_name);
  TEST_ASSERT_EQUAL_UINT32(12u, in.boot_count);
}

// ============================================================
// 注册
// ============================================================
void register_boot_persist_tests(void) {
  RUN_TEST(test_bootpersist_first_boot_on_blank_nvs);
  RUN_TEST(test_bootpersist_second_boot_recovers_prev);
  RUN_TEST(test_bootpersist_prev_up_boundaries);
  RUN_TEST(test_bootpersist_heartbeat_only_at_10min);
  RUN_TEST(test_bootpersist_heartbeat_due_wraps);
  RUN_TEST(test_bootpersist_guard_totals_accumulate);
  RUN_TEST(test_bootpersist_accum_saturates);
  RUN_TEST(test_bootpersist_missing_bootcount_starts_at_one);
  RUN_TEST(test_bootpersist_read_miss_is_not_broken_nvs);
  RUN_TEST(test_bootpersist_nvs_write_failure_is_reported);
  RUN_TEST(test_bootpersist_heartbeat_write_failure_not_counted);
  RUN_TEST(test_bootpersist_flush_now);
  RUN_TEST(test_bootpersist_prev_name_roundtrip);
  RUN_TEST(test_bootpersist_prev_raw_only_is_still_valid);
}
