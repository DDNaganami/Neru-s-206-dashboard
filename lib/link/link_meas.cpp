// 双板链路 —— 测速 / 测丢包（实现）。判据、门槛、信封格式的**唯一出处**在 link_meas.h。
//
// 本文件只有三件事：
//   ① 把一帧测量信封编码进**既有**的 v1 帧（`encodeFrame`，协议一个字节没动）；
//   ② 收端算间隔直方图 / 丢包率 / 分位数（整数、无浮点、无动态分配）；
//   ③ 两行 ASCII 汇总（只格式化，打印在调用方）。
#include "link_meas.h"

namespace dashlink {

// ============================================================
// 发端
// ============================================================

void MeasSender::reset() {
  mActive = false;
  mSent = 0;
  mPlanned = 0;
  mNextMs = 0;
  mT0 = 0;
  mSeq = 0;
  mPeriodMs = kDefaultPeriodMs;
  mRole = kRoleMaster;
}

void MeasSender::start(uint32_t now_ms, uint8_t role, uint32_t count, uint16_t period_ms) {
  mRole = role;
  mPlanned = (count == 0u) ? kDefaultCount : count;
  mPeriodMs = (period_ms == 0u) ? kDefaultPeriodMs : period_ms;
  mSent = 0;
  mSeq = 0;
  mT0 = now_ms;
  mNextMs = now_ms;      // 第一帧**立刻**（下一次 poll 就发）
  mActive = true;
}

uint8_t MeasSender::poll(uint32_t now_ms, LinkPhy& phy) {
  if (!mActive) return 0u;
  if (mSent >= mPlanned) { mActive = false; return 0u; }

  uint8_t frames = 0u;
  while (frames < kMaxPerPoll && mSent < mPlanned) {
    if ((int32_t)(now_ms - mNextMs) < 0) break;      // 还没到点（无符号时间安全比较）

    // ---- 造信封（8 B，逐字段见 link_meas.h） ----
    uint8_t payload[kMeasEnvelopeLen] = {0};
    payload[0] = kMeasMagic[0];
    payload[1] = kMeasMagic[1];
    payload[2] = kMeasMagic[2];
    payload[3] = kMeasMagic[3];
    payload[kMeasOffVer] = kMeasVer;
    uint16_t seq = (uint16_t)(mSeq & kMeasSeqMask);
    if (mSent == 0u) seq = (uint16_t)(seq | kMeasFirstBit);   // 这一发是新 burst 的第一帧
    payload[kMeasOffSeq + 0u] = (uint8_t)((seq >> 8) & 0xFFu); // 大端（§2 的多字节口径）
    payload[kMeasOffSeq + 1u] = (uint8_t)(seq & 0xFFu);
    // 发端单调毫秒的**低 16 位**，大端（65.5 s 绕一圈；见 link_meas.h 的信封表）
    const uint16_t t16 = (uint16_t)(now_ms & 0xFFFFu);
    payload[kMeasOffMs + 0u] = (uint8_t)((t16 >> 8) & 0xFFu);
    payload[kMeasOffMs + 1u] = (uint8_t)(t16 & 0xFFu);

    uint8_t frame[kFrameBytesMax] = {0};
    const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, kMeasEnvelopeLen, mRole,
                                  frame, (uint16_t)sizeof(frame));
    if (n == 0u) { mActive = false; break; }        // 参数非法：不可能，但别静默转圈

    const size_t wrote = phy.write(frame, (size_t)n);
    if (wrote == 0u) break;                          // PHY 环满：这一拍不发了，下一拍再来
    // ★ ESP-NOW 那一档的 write() 是**整帧进出**（环里放不下就返回 0）；
    //   这里仍然按"可能只写了一半"来判：写不满就**不再推进序号**，
    //   序号只在整帧交出去之后才 +1（否则收端会看到"序号跳了但帧没来"，
    //   把它算成丢包 —— 那是我们自己的记账错，不该记在链路上）。
    if (wrote != (size_t)n) break;

    ++mSent;
    ++mSeq;
    mNextMs = (uint32_t)(mNextMs + mPeriodMs);
    ++frames;
  }
  if (mSent >= mPlanned) mActive = false;
  return frames;
}

// ============================================================
// 收端
// ============================================================

void MeasReceiver::reset() {
  mQHead = 0;
  mQTail = 0;
  for (uint8_t i = 0; i < kMeasRxQueue; ++i) {
    mQSeq[i] = 0;
    mQMs[i] = 0;
    mQSentMs[i] = 0;
  }
  for (uint16_t i = 0; i < kMeasGapBins; ++i) mBins[i] = 0;
  mRes = MeasRxResult{};
  mActive = false;
  mHavePrev = false;
  mEnded = false;
  mLastSeq = 0;
  mPrevMs = 0;
  mFirstMs = 0;
  mLastMs = 0;
  mCoalesced = 0;
  mQueueDrop = 0;
  mPrevSentMs = 0;
  mHaveSentMs = false;
  mWireGapMax = 0;
}

bool MeasReceiver::noteArrival(const uint8_t* payload, uint8_t payload_len, uint32_t now_ms) {
  if (payload == nullptr || payload_len < kMeasEnvelopeLen) return false;
  if (payload[0] != kMeasMagic[0] || payload[1] != kMeasMagic[1] ||
      payload[2] != kMeasMagic[2] || payload[3] != kMeasMagic[3]) {
    return false;
  }
  if (payload[kMeasOffVer] != kMeasVer) return false;   // 信封版本不认 ⇒ 交给常规链路去判

  const uint16_t seq_raw = (uint16_t)(((uint16_t)payload[kMeasOffSeq] << 8) |
                                      (uint16_t)payload[kMeasOffSeq + 1u]);
  const uint16_t sent_ms16 = (uint16_t)(((uint16_t)payload[kMeasOffMs] << 8) |
                                        (uint16_t)payload[kMeasOffMs + 1u]);

  // 入队（满了就丢**到达记录**并计数：宁少一条样本，也绝不在 WiFi 任务里等）
  const uint8_t next = (uint8_t)((mQHead + 1u) % kMeasRxQueue);
  if (next == mQTail) {
    ++mQueueDrop;
    return true;      // 仍然是测量帧（调用方不要再把它喂给常规链路）
  }
  mQSeq[mQHead] = seq_raw;
  mQMs[mQHead] = now_ms;
  mQSentMs[mQHead] = sent_ms16;
  mQHead = next;
  return true;
}

uint8_t MeasReceiver::poll() {
  uint8_t n = 0u;
  while (n < kMeasRxPollMax && mQTail != mQHead) {
    const uint16_t seq_raw = mQSeq[mQTail];
    const uint32_t ms_now = mQMs[mQTail];
    const uint16_t ms_sent = mQSentMs[mQTail];
    mQTail = (uint8_t)((mQTail + 1u) % kMeasRxQueue);
    const bool first = (seq_raw & kMeasFirstBit) != 0u;
    drainOne((uint16_t)(seq_raw & kMeasSeqMask), ms_now, ms_sent, first);
    ++n;
  }
  // 同一拍取到多条 ⇒ 它们的时间戳相同 ⇒ 后面的间隔会算成 0。
  // 这是"主循环没跟上"，不是链路的抖动 ⇒ 单独计数（见 link_meas.h）。
  if (n > kMeasCoalesceSlack) {
    mCoalesced += (uint32_t)(n - kMeasCoalesceSlack);
    mRes.coalesced = mCoalesced;
  }
  mRes.queue_drop = mQueueDrop;
  return n;
}

void MeasReceiver::drainOne(uint16_t seq, uint32_t ms_now, uint16_t ms_sent16, bool first_of_burst) {
  if (first_of_burst) {
    // 新的一次 burst：清掉上一次的统计（**只清统计**，队列/环是另一回事）
    mActive = true;
    mEnded = false;
    mHavePrev = false;
    mHaveSentMs = false;
    mWireGapMax = 0;
    for (uint16_t i = 0; i < kMeasGapBins; ++i) mBins[i] = 0;
    mRes = MeasRxResult{};
    mRes.coalesced = mCoalesced;      // 这两个是"本机"的读数，不随 burst 清零
    mRes.queue_drop = mQueueDrop;
    mFirstMs = ms_now;
  } else if (!mActive) {
    // 没在 burst 里、又不是第一帧 ⇒ 上一次 burst 的尾巴（或开机残留）：**不采纳**
    // （采纳了会把"两次 burst 之间的空档"算成一个巨大的间隔 ⇒ 假的不合格）。
    return;
  }

  ++mRes.frames_rx;
  mLastMs = ms_now;
  mLastSeq = seq;
  if (mRes.frames_rx == 1u) {
    mRes.seq_min = seq;
    mRes.seq_max = seq;
  } else {
    if (seq < (uint16_t)mRes.seq_min) mRes.seq_min = seq;
    // 序号 bit15 是标志位、已被掩掉 ⇒ 这里只比 15 位的值（一次 burst ≤ 2000 帧 ⇒ 不绕圈）
    if (seq > (uint16_t)mRes.seq_max) mRes.seq_max = seq;
  }

  // ---- 收端间隔（到达时刻）----
  if (mHavePrev) {
    const uint32_t gap = (uint32_t)(ms_now - mPrevMs);   // 无符号差 ⇒ 回绕也安全
    ++mRes.gap_samples;
    if (gap > mRes.gap_max_ms) mRes.gap_max_ms = gap;
    const uint16_t bin = measGapBinOf(gap);
    ++mBins[bin];
    if (bin == kMeasGapOverflowIx) ++mRes.over95;
  }
  mPrevMs = ms_now;
  mHavePrev = true;

  // ---- 发端间隔（信封里那 16 位毫秒，按 65536 取模展开）----
  if (mHaveSentMs) {
    const uint16_t delta = (uint16_t)(ms_sent16 - mPrevSentMs);
    if (delta > mWireGapMax) mWireGapMax = delta;
  }
  mPrevSentMs = ms_sent16;
  mHaveSentMs = true;

  // ---- 收尾（每一次都重算，这样"什么时候看都是最新的"）----
  mRes.expected = (uint32_t)(mRes.seq_max - mRes.seq_min) + 1u;
  mRes.lost = (mRes.frames_rx < mRes.expected) ? (mRes.expected - mRes.frames_rx) : 0u;
  mRes.loss_per_100k = mRes.expected
                           ? (uint32_t)(((uint64_t)mRes.lost * 100000ull) / (uint64_t)mRes.expected)
                           : 0u;
  mRes.p50_ms = percentile(50u, 100u);
  mRes.p95_ms = percentile(95u, 100u);
  mRes.p99_ms = percentile(99u, 100u);
  mRes.wire_gap_max_ms = mWireGapMax;
  mRes.span_arrival_ms = (uint32_t)(mLastMs - mFirstMs);
}

uint32_t MeasReceiver::percentile(uint32_t num, uint32_t den) const {
  uint64_t total = 0;
  for (uint16_t i = 0; i < kMeasGapBins; ++i) total += mBins[i];
  if (total == 0u) return 0u;
  const uint64_t want = (total * (uint64_t)num + (uint64_t)den - 1ull) / (uint64_t)den;  // 上取整
  uint64_t acc = 0;
  for (uint16_t i = 0; i < kMeasGapBins; ++i) {
    acc += mBins[i];
    if (acc >= want) return (uint32_t)i * kMeasGapBinMs;
  }
  return (uint32_t)(kMeasGapBins - 1u) * kMeasGapBinMs;
}

bool MeasReceiver::endedBy(uint32_t now_ms, uint32_t idle_ms) {
  if (!mActive || mEnded) return false;
  if ((uint32_t)(now_ms - mLastMs) < idle_ms) return false;
  mEnded = true;
  mActive = false;
  return true;
}

const char* MeasRxResult::verdict() const {
  if (!enough()) return "样本不足";
  if (!passLoss()) return "不适合(丢包)";
  if (!passGap()) return "不适合(最大间隔)";
  if (!passJitter()) return "不适合(抖动)";
  return "适合";
}

// ============================================================
// 两行 ASCII 汇总（只格式化；打印在调用方）
// ============================================================
namespace {

class Buf {
 public:
  Buf(char* p, int cap) : mP(p), mCap(cap) {}
  void ch(char c) { if (mN < mCap - 1) mP[mN++] = c; }
  void str(const char* s) { while (*s) ch(*s++); }
  void num(uint32_t v) {
    char t[11];
    uint8_t n = 0;
    if (v == 0u) { ch('0'); return; }
    while (v > 0u && n < sizeof(t)) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0u) ch(t[--n]);
  }
  int done() { if (mCap > 0) mP[mN < mCap ? mN : mCap - 1] = '\0'; return (int)mN; }

 private:
  char* mP;
  int   mCap;
  int   mN = 0;
};

}  // namespace

int measFormatRxSummary(const MeasRxResult& r, char* out, int cap) {
  if (out == nullptr || cap <= 0) return 0;
  Buf b(out, cap);
  b.str("meas rx: frames=");
  b.num(r.frames_rx);
  b.str(" seq=");
  b.num(r.seq_min);
  b.str("..");
  b.num(r.seq_max);
  b.str(" expected=");
  b.num(r.expected);
  b.str(" lost=");
  b.num(r.lost);
  b.str(" (");
  // 丢包率按**百分比、两位小数**打（0.25 % 这种要看得出来）：loss_per_100k 除以 1000
  // 正好是"万分位" ⇒ 整数部分 = /1000、两位小数 = %100。
  b.num(r.loss_per_100k / 1000u);
  b.ch('.');
  {
    const uint32_t frac = (r.loss_per_100k % 1000u) / 10u;   // 0..99
    if (frac < 10u) b.ch('0');
    b.num(frac);
  }
  b.str("%) gap_max=");
  b.num(r.gap_max_ms);
  b.str("ms p50=");
  b.num(r.p50_ms);
  b.str("ms p95=");
  b.num(r.p95_ms);
  b.str("ms p99=");
  b.num(r.p99_ms);
  b.str("ms over95=");
  b.num(r.over95);
  b.str(" n=");
  b.num(r.gap_samples);
  b.str(" wire_gap_max=");
  b.num(r.wire_gap_max_ms);
  b.str("ms span=");
  b.num(r.span_arrival_ms);
  b.str("ms coalesced=");
  b.num(r.coalesced);
  b.str(" qdrop=");
  b.num(r.queue_drop);
  b.str(" -> ");
  b.str(r.verdict());
  b.str(" (loss<0.1% gap_max<100ms p99<20ms n>=");
  b.num(kMeasThresholds.min_rx_frames);
  b.str(")");
  return b.done();
}

int measFormatTxSummary(const MeasSender& s, uint32_t now_ms, char* out, int cap) {
  if (out == nullptr || cap <= 0) return 0;
  Buf b(out, cap);
  const uint32_t span = (uint32_t)(now_ms - s.t0());
  b.str("meas tx: sent=");
  b.num(s.sent());
  b.str("/");
  b.num(s.planned());
  b.str(" period=");
  b.num(s.periodMs());
  b.str("ms span=");
  b.num(span);
  b.str("ms -> ");
  // 发端那条门槛只有"真的按节奏发出来了"：发满计划帧数即算过
  // ★ 收端判据（丢包/间隔/抖动）只有收端才量得到 —— 别拿发端的"发出去了"当"收到了"。
  b.str(s.sent() >= s.planned() ? "发满" : "未发满");
  return b.done();
}

}  // namespace dashlink
