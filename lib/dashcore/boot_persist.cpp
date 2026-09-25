// ============================================================
// 跨重启留档（BootPersist）的实现 —— 2026-09-25
//
// ★ 这个文件里**没有一行硬件代码**：NVS 读写是注入的回调、时钟由调用方给。
//   判据与理由全部写在 `boot_persist.h` 的文件头（那才是"为什么"的家），
//   这里只写"怎么落"。
// ============================================================
#include "boot_persist.h"

BootPersist::BootPersist(BootNvsReadFn read, BootNvsWriteFn write,
                         BootNvsReadStrFn read_str, BootNvsWriteStrFn write_str,
                         void* ctx)
    : read_(read), write_(write), read_str_(read_str), write_str_(write_str), ctx_(ctx) {}

uint32_t BootPersist::readU32(BootKey k, uint32_t dflt) {
  if (read_ == nullptr) return dflt;
  uint32_t v = 0;
  if (!read_(k, &v, ctx_)) return dflt;
  return v;
}

bool BootPersist::writeU32(BootKey k, uint32_t v) {
  if (write_ == nullptr) return false;
  return write_(k, v, ctx_);
}

BootInfo BootPersist::begin(uint32_t now_ms, uint32_t reason_raw, const char* reason_name) {
  BootInfo& b = boot_;
  b = BootInfo{};
  b.reason_raw  = reason_raw;
  b.reason_name = (reason_name != nullptr) ? reason_name : "UNKNOWN";

  if (read_ == nullptr || write_ == nullptr) {
    // 没有注入 ⇒ 这一层等于不在（`nvs-`）：`boot_count` 仍是 1（"这一份固件跑了一次"，
    // 不是假数据），而 `prev_*` 全部保持"没有记录"。
    b.nvs_ok = false;
    b.boot_count = 1;
    started_ = false;
    return b;
  }

  // ---- ① 先把"上一次"读出来（**必须在任何写之前**：下面第 ② 步会覆盖这些键）----
  //   ★ 回调的语义是"**false = 这个键没有 / 读不动**"（两者在判据上同义）。
  //     所以这里**不能**用"一个键都没读到"当"NVS 坏了" —— 全新板子 / 第一次跑带本层
  //     的固件**就是**一个键都没有，而那是最正常不过的一种情况。
  //     "NVS 到底能不能用"这件事由下面的**写**来判（写成功 ⇒ 这一层是活的）。
  uint32_t old_boot_up = 0;
  const bool have_boot_up = read_(BootKey::PrevUpMs, &old_boot_up, ctx_);
  uint32_t old_hb = 0;
  const bool have_hb = read_(BootKey::HeartbeatMs, &old_hb, ctx_);
  uint32_t n_old = 0;
  const bool have_n = read_(BootKey::BootCount, &n_old, ctx_);
  uint32_t pr_raw = 0;
  const bool have_pr = read_(BootKey::PrevReasonRaw, &pr_raw, ctx_);
  char pr_name[sizeof(b.prev_reason_name)] = "";
  bool have_pr_name = false;
  if (read_str_ != nullptr) have_pr_name = read_str_(BootKey::PrevReason, pr_name, sizeof(pr_name), ctx_);
  // 守护累计的四个基线 + 落盘次数 + 心跳次数。
  uint32_t g_rd = 0, g_fix = 0, g_bl = 0, g_anm = 0, g_snap = 0, hb_n = 0;
  const bool have_g_rd  = read_(BootKey::GuardRd, &g_rd, ctx_);
  const bool have_g_fix = read_(BootKey::GuardFix, &g_fix, ctx_);
  const bool have_g_bl  = read_(BootKey::GuardBl, &g_bl, ctx_);
  const bool have_g_anm = read_(BootKey::GuardAnom, &g_anm, ctx_);
  read_(BootKey::GuardSnaps, &g_snap, ctx_);
  read_(BootKey::HeartbeatN, &hb_n, ctx_);

  // ---- ② 把"上一次运行了多久"算出来（同上：必须在写之前用旧值算）----
  b.prev_up_ms = boot_prev_up_ms(old_hb, have_hb, old_boot_up, have_boot_up);
  // ---- ③ 记下"上一次的 reason"（旧的 `b_prevr` 就是上一次开机写下的"本次"）----
  if (have_pr_name && pr_name[0] != '\0') {
    b.prev_valid = true;
    for (uint32_t i = 0; i < sizeof(b.prev_reason_name) - 1u && pr_name[i] != '\0'; ++i) {
      b.prev_reason_name[i] = pr_name[i];
    }
    b.prev_reason_raw = have_pr ? pr_raw : 0u;
  } else if (have_pr) {
    // 名字没存下来但原始值在 ⇒ 仍然是"有记录"（调用方按 raw 显示）。
    b.prev_valid = true;
    b.prev_reason_raw = pr_raw;
  }

  // ---- ④ 把**本次**写下去（这一次写不能省，见 .h 里 `begin()` 的说明）----
  //   ★ 顺序：reason 先写、uptime 后写 —— 万一在两次写之间掉电，
  //     "上一次的 reason" 已经是最新的一次（取证最要紧的就是它）。
  //   ★ `ok` 从"写下去没有"起算：全新 NVS 上**没有任何键可读**，能证明这一层活着的
  //     只有"写成功"（读回来的 false 是"没有这个键"，不是"坏了"）。
  bool ok = true;
  if (write_str_ != nullptr) ok = write_str_(BootKey::PrevReason, b.reason_name, ctx_) && ok;
  ok = writeU32(BootKey::PrevReasonRaw, reason_raw) && ok;
  ok = writeU32(BootKey::PrevUpMs, now_ms) && ok;   // ★ 本次开机时的 uptime（通常 ~200ms）

  // ---- ⑤ 启动次数 +1（既有键 `bootn`：**不许改名**，改名等于把历史清零）----
  b.boot_count = (have_n ? n_old : 0u) + 1u;
  ok = writeU32(BootKey::BootCount, b.boot_count) && ok;

  // ---- ⑥ 守护累计的基线（本次尚未落盘 ⇒ 本次增量为 0）----
  if (have_g_rd)  base_.rd = g_rd;
  if (have_g_fix) base_.fix = g_fix;
  if (have_g_bl)  base_.bl = g_bl;
  if (have_g_anm) base_.anom = g_anm;
  snaps_ = g_snap;
  hb_n_  = hb_n;

  // ---- ⑦ 心跳从"现在"起算（第一次落盘在 10 分钟之后；开机头 10 分钟不写 NVS）----
  next_hb_ms_ = boot_next_heartbeat(now_ms, kBootHeartbeatMs);
  // ★★ NVS 到底能不能用 —— 判据是**写**，不是读：
  //   · 读回来的 false 的含义是"**没有这个键**"（全新板子 / 第一次跑带本层的固件就是
  //     一个键都没有 ⇒ 那是最正常的情况，**不许**把它当成"NVS 坏了"）；
  //   · 只有"写不下去"才说明这一层真的不工作 ⇒ 开机那行标 `nvs-`，
  //     **明说**这一次的计数没有落盘，而不是假装 0 次；
  //   · 写不下去时 `started_ = false` ⇒ `tick()` 什么都不做（不去每 10 分钟撞一次墙）。
  b.nvs_ok = ok;
  started_ = ok;
  return b;
}

bool BootPersist::writeSnapshot(uint32_t now_ms, const GuardTotals& cur) {
  bool ok = true;
  ok = writeU32(BootKey::HeartbeatMs, now_ms) && ok;
  hb_last_ms_ = now_ms;
  ok = writeU32(BootKey::HeartbeatN, hb_n_ + 1u) && ok;
  // 累计值 = 基线 + 本次（饱和相加）。
  ok = writeU32(BootKey::GuardRd,   boot_accum(base_.rd, cur.rd)) && ok;
  ok = writeU32(BootKey::GuardFix,  boot_accum(base_.fix, cur.fix)) && ok;
  ok = writeU32(BootKey::GuardBl,   boot_accum(base_.bl, cur.bl)) && ok;
  ok = writeU32(BootKey::GuardAnom, boot_accum(base_.anom, cur.anom)) && ok;
  ok = writeU32(BootKey::GuardSnaps, snaps_ + 1u) && ok;
  // ★ 计数只在**写成功**时才前进：写失败还当成功会让"心跳在跑吗"这一格撒谎
  //   （而这一格恰恰是"板子自己重启过没有"的判据）。
  if (ok) {
    hb_n_ += 1u;
    snaps_ += 1u;
  }
  boot_.nvs_ok = boot_.nvs_ok && ok;
  return ok;
}

bool BootPersist::tick(uint32_t now_ms, const GuardTotals& cur) {
  if (!started_) return false;
  if (!boot_heartbeat_due(now_ms, next_hb_ms_)) return false;
  const bool ok = writeSnapshot(now_ms, cur);
  // ★ 到点就要排下一次 —— 无论写没写成功：写失败时**不重试到成功为止**
  //   （那会在 NVS 坏掉时变成每轮一次写），照常等下一个 10 分钟。
  next_hb_ms_ = boot_next_heartbeat(now_ms, kBootHeartbeatMs);
  return ok;
}

bool BootPersist::flushNow(uint32_t now_ms, const GuardTotals& cur) {
  if (!started_) return false;
  return writeSnapshot(now_ms, cur);
}
