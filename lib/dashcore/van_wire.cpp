#include "van_wire.h"
#include <string.h>

// 说明:本文件是 VAN 线路层的纯逻辑实现。历史上在这里踩过的坑都写在
// 对应位置,改动前请先读注释 —— 它们都是实测出来的,不是理论推导。

namespace van {

// ---------------- CRC-15 ----------------
// 多项式 x^15+x^14+x^10+x^8+x^7+x^4+x^3+1 → 位掩码 0x4599
static const uint16_t kCrcPoly = 0x4599u;

uint16_t crc15_extend(uint16_t crc, uint8_t byte) {
  crc ^= (uint16_t)byte << 7;        // 对齐到 15 位寄存器的次高位
  for (uint8_t b = 0; b < 8; ++b) {
    if (crc & 0x4000u) {
      crc = (uint16_t)((crc << 1) ^ kCrcPoly);
    } else {
      crc = (uint16_t)(crc << 1);
    }
    crc &= 0x7FFFu;
  }
  return crc;
}

uint16_t crc15(const uint8_t* data, uint16_t len) {
  uint16_t crc = 0;
  for (uint16_t i = 0; i < len; ++i) crc = crc15_extend(crc, data[i]);
  return crc;
}

CmdBits decodeCmd(uint8_t cmd) {
  CmdBits c;
  c.ext = (cmd & kCmdExtMask) != 0;
  c.rak = (cmd & kCmdRakMask) != 0;
  c.rw  = (cmd & kCmdRwMask) != 0;
  c.rtr = (cmd & kCmdRtrMask) != 0;
  return c;
}

// ---------------- 帧字节解析 ----------------
// 输入:[IDENlo, IDENhi/CMD, DATA..., FCS_a, FCS_b]
// 数据长度未知,用 FCS 反推:逐个候选长度算 CRC-15,与末尾两字节比对。
bool parseFrameBytes(const uint8_t* bytes, uint16_t n, Frame* out) {
  if (!bytes || !out || n < 4) return false;   // 2 字节头 + 2 字节 FCS 是下限

  Frame f;
  f.ident = idenFromBytes(bytes[0], bytes[1]);
  f.cmd   = cmdFromByte2(bytes[1]);

  const uint8_t fcs_a = bytes[n - 2];
  const uint8_t fcs_b = bytes[n - 1];
  const uint16_t fcs_le = (uint16_t)((fcs_b << 8) | fcs_a);   // 低字节先到
  const uint16_t fcs_be = (uint16_t)((fcs_a << 8) | fcs_b);   // 高字节先到

  // 头部 2 字节 + 尾部 2 字节固定,中间才是数据
  const uint16_t max_len = (uint16_t)(n - 4);
  const uint16_t try_len = (max_len > kDataDefault) ? kDataDefault : max_len;
  if (max_len > kDataDefault) f.overflow = true;

  for (uint16_t len = 0; len <= try_len; ++len) {
    const uint16_t covered = (uint16_t)(2 + len);   // IDENlo + IDENhi/CMD + data
    const uint16_t calc = crc15(bytes, covered);
    if (calc != fcs_le && calc != fcs_be) continue;

    f.len = (uint8_t)len;
    for (uint16_t i = 0; i < len; ++i) f.data[i] = bytes[2 + i];
    f.fcs_le   = (calc == fcs_le);
    f.fcs      = calc;
    f.fcs_calc = calc;
    f.fcs_ok   = true;
    *out = f;
    return true;
  }
  return false;
}

// ---------------- BitDecoder ----------------
void BitDecoder::reset() {
  mCurNs = 0;
  mHasLevel = false;
  mLevel = false;
  mBitCount = 0;
  mMask = 0x80u;
  mByte = 0;
  mRecessiveRun = 0;
  mDomRun = 0;
  mSinceEod = 0xFFu;
  mAckDominant = false;
  mNeedResync = false;
  mArmed = false;
  mEofLatched = false;
  mHead = 0;
  mCount = 0;
  mOverflow = false;
}

void BitDecoder::resync() {
  mBitCount = 0;
  mMask = 0x80u;
  mByte = 0;
  mRecessiveRun = 0;
  mDomRun = 0;
  mSinceEod = 0xFFu;
  mArmed = false;      // 等下一个 SOF 才重新开始解字节
  mOverflow = false;
}

uint8_t BitDecoder::takeByte() {
  if (mCount == 0) return 0;
  const uint8_t b = mQueue[mHead];
  mHead = (uint8_t)((mHead + 1) % kQueueMax);
  --mCount;
  return b;
}

// 一个 TS 的采样交给 4B/5B 解码:每 5 个 TS 的第 5 个是 E-Manchester 编码位
// (丢弃),其余 4 位按 MSB-first 装进当前字节。
void BitDecoder::processSlot(bool level) {
  const uint16_t pos = (uint16_t)((mBitCount + 1) % 5);
  if (pos != 0) {
    if (level) mByte = (uint8_t)(mByte | mMask);   // recessive = 1
    mMask = (uint8_t)(mMask >> 1);
  } else if (mBitCount == 9) {
    // 本字节的第 2 个编码位:字节完成。
    // 未武装(总线空闲/等 SOF)时只认 SOF 字节,其余丢弃 —— 空闲段会解出
    // 成百上千个无意义字节,若都留下会冲爆队列。
    const bool is_sof = (mByte == kSofByte);
    const bool accept = mArmed || is_sof;
    if (accept) {
      if (is_sof && !mArmed) {
        mArmed = true;
        mOverflow = false;         // 新帧开始,清掉上一段空闲的溢出标记
      }
      if (mSink) {
        mSink->onByte(mByte);      // 直接回调:一个区间内的字节一个不丢
      } else if (mCount < kQueueMax) {
        const uint8_t tail = (uint8_t)((mHead + mCount) % kQueueMax);
        mQueue[tail] = mByte;
        ++mCount;
      } else {
        mOverflow = true;
      }
    }
    mByte = 0;
    mBitCount = 0;
    mMask = 0x80u;
    return;                                        // 不 ++,下一槽即新字节的 bit0
  }
  ++mBitCount;
}

BitDecoder::Ev BitDecoder::pushEdge(uint64_t ns, bool level) {
  if (!mHasLevel) {
    mCurNs = ns;
    mLevel = level;
    mHasLevel = true;
    mRecessiveRun = level ? 1u : 0u;
    return Ev::None;
  }
  if (level == mLevel) return Ev::None;

  const uint64_t dt_ns = ns - mCurNs;
  uint32_t slots = (uint32_t)((dt_ns + kTsNs / 2) / kTsNs);
  if (slots == 0) slots = 1;

  // 帧间空隙:两次边沿间隔超过阈值,说明总线已进入空闲。此时**不能**把
  // 这个区间逐槽展开 —— 一个几十毫秒的空闲会变成上千个"帧内槽",
  // 直接冲垮相位与帧尾判定。只报一次帧尾并重新对齐。
  // 阈值必须远大于帧内最长连续 recessive(全 1 数据字节 = 10 槽 = 80µs)。
  if (dt_ns > mGapTimeoutNs) {
    const bool was_eof = mEofLatched;
    resync();
    mLevel = level;
    mCurNs = ns;
    mEofLatched = true;         // 空隙内不重复报帧尾
    return was_eof ? Ev::None : Ev::EndOfFrame;
  }

  // 连续 recessive 的计数只用于 ACK 窗口识别与总线空闲判定;
  // **不在这里触发复位** —— "8 个连续 recessive"会被帧内合法数据误触发
  // (实测 8A 22 5A 这帧的帧体里就有一段),帧内的误触发若复位相位,
  // 后面的字节会全部读不到。帧的完整性由 FrameParser 的 FCS 校验判定,
  // 帧间复位由 pushEdge 开头的超时分支负责。
  const bool prev_level = mLevel;

  for (uint32_t i = 0; i < slots; ++i) {
    if (prev_level) {
      ++mRecessiveRun;
      if (mSinceEod != 0xFFu) {
        if (mSinceEod < 4u) mAckDominant = false;   // ACK 窗口内全是 recessive
        ++mSinceEod;
      }
      mDomRun = 0;
    } else {
      // 恰好 2 个 dominant 落在两个 recessive 之间 = EOD(E-Manchester 违约),
      // 这是识别帧尾 ACK 窗口的锚点。
      ++mDomRun;
      if (mSinceEod == 0xFFu && mRecessiveRun >= 2u && mDomRun == 1u) {
        mSinceEod = 0;
        mAckDominant = true;    // 本槽(ACK 第 1 位)就是 dominant
      }
      mRecessiveRun = 0;
    }
    processSlot(prev_level);
  }

  mLevel = level;
  mCurNs = ns;
  return (mCount > 0) ? Ev::Byte : Ev::None;
}

// ---------------- FrameParser ----------------
void FrameParser::reset() {
  mInFrame = false;
  mCount = 0;
  mStartNs = 0;
  mFrame = Frame();
}

bool FrameParser::pushByte(uint8_t b, uint64_t ns, Frame* out) {
  (void)out;
  if (!mInFrame) {
    if (b != kSofByte) return false;    // 没对齐,继续等 SOF
    mInFrame = true;
    mCount = 0;
    mStartNs = ns;
    return false;
  }
  if (mCount < sizeof(mBuf)) mBuf[mCount] = b;
  ++mCount;
  return false;
}

bool FrameParser::hasCompleteFrame() const {
  if (!mInFrame) return false;
  if (mCount < 4u || mCount > sizeof(mBuf)) return false;
  Frame probe;
  const uint16_t n = mCount;
  return parseFrameBytes(mBuf, n, &probe);
}

bool FrameParser::endFrame(uint64_t ns, Frame* out, bool ack_dominant) {
  if (!mInFrame) {
    reset();
    return false;
  }

  const uint16_t n = (mCount > sizeof(mBuf)) ? (uint16_t)sizeof(mBuf) : mCount;
  Frame f;
  const bool ok = (n >= 4u) && parseFrameBytes(mBuf, n, &f);
  if (!ok) {
    // 没有凑出 FCS 通过的帧:丢弃缓冲重来。
    // 注意不要在这里保留缓冲 —— 调用方只在"确实有一帧"或"确定空闲"时
    // 才会调本函数,残留数据只会污染下一帧。
    mInFrame = false;
    mCount = 0;
    return false;
  }

  mInFrame = false;
  f.start_ns = mStartNs;
  f.end_ns = ns;
  f.ack = ack_dominant ? 1u : 0u;
  if (out) *out = f;

  // 帧结束后必须清零计数,否则下一帧的字节会追加到本帧缓冲后面
  // (曾因此第二帧永远解不出来 —— 数据长度/CRC 全被污染)。
  mCount = 0;
  return true;
}

// ---------------- 编码器(4B5B) ----------------
namespace {

struct SlotWriter {
  uint8_t* p = nullptr;
  uint32_t cap = 0;
  uint32_t n = 0;
  uint16_t slot_in_byte = 0;   // 当前字节内已写的槽数 0..9
  uint8_t  mask = 0x80u;
  uint8_t  byte = 0;

  SlotWriter(uint8_t* buf, uint32_t capacity) : p(buf), cap(capacity) {}

  // 写一个槽。is_data_bit=false 表示 E-Manchester 编码位:恒为 recessive,
  // 不参与累加,只推进字节内位置。
  void putSlot(bool busy, bool is_data_bit) {
    if (n < cap) p[n] = busy ? 1u : 0u;
    ++n;
    if (is_data_bit) {
      if (busy) byte = (uint8_t)(byte | mask);
      mask = (uint8_t)(mask >> 1);
      ++slot_in_byte;
      return;
    }
    // 编码位在字节内的位置是 4 和 9。必须在这里 return:否则结构体尾部
    // 会把剩余槽位补成 recessive,整个帧体被"1 的海洋"淹没。
    if (slot_in_byte == 4) {
      mask = 0x08u;
      slot_in_byte = 5;
    } else {
      slot_in_byte = 0;
      mask = 0x80u;
      byte = 0;
    }
  }

  // 写整字节:4B5B = 每 4 个数据位后跟 1 个编码位,共 10 槽。
  // 顺序必须是 [d0..d3 s][d4..d7 s],不是"8 位数据 + 2 个编码位"。
  void putByte(uint8_t v) {
    for (uint8_t half = 0; half < 2; ++half) {
      uint8_t m = (uint8_t)(half == 0 ? 0x80u : 0x08u);
      for (uint8_t i = 0; i < 4; ++i) {
        putSlot((v & m) != 0, true);
        m = (uint8_t)(m >> 1);
      }
      putSlot(true, false);   // E-Manchester 编码位,恒为 recessive
    }
  }
};

}  // namespace

uint32_t encodeFrame(const Frame& f, uint8_t* slots, uint32_t slots_cap) {
  if (!slots || slots_cap < 32u) return 0;
  if (f.len > kDataDefault) return 0;

  SlotWriter w(slots, slots_cap);

  // 帧结构:SOF → IDENlo → (IDENhi|CMD) → DATA → FCS → EOD 违约 → ACK → EOF
  w.putByte(kSofByte);
  w.putByte(idenByte1(f.ident));
  w.putByte(idenByte2(f.ident, f.cmd));

  uint8_t body[kDataDefault + 2];
  body[0] = idenByte1(f.ident);
  body[1] = idenByte2(f.ident, f.cmd);
  for (uint8_t i = 0; i < f.len; ++i) {
    body[2 + i] = f.data[i];
    w.putByte(f.data[i]);
  }
  const uint16_t crc = crc15(body, (uint16_t)(2 + f.len));
  w.putByte((uint8_t)(crc & 0xFFu));
  w.putByte((uint8_t)((crc >> 8) & 0xFFu));

  // EOD:一对 dominant = 一次 E-Manchester 违约,接在最后一个字节的槽位上
  // (线上的 EOD 就是接着数据发的,不按字节补齐 —— 补齐会插入额外槽位,
  //  使后面的字节全部错位)。
  w.putSlot(false, false);
  w.putSlot(false, false);
  // ACK:2 个槽,第 1 个 recessive、第 2 个 **dominant(表示已被应答)**。
  // 这不是装饰:EOF 判据是"8 个连续 recessive",而某些数据的尾部本身就会
  // 带出 8 个连续 recessive(实测 8A 22 5A 这帧的帧体里就有一段),
  // 真实总线靠 ACK 位的 dominant 打破它。若把 ACK 写成不应答,就等于造了
  // 一个真实总线上不存在的帧 —— 解码器会在帧中途误判帧尾。
  w.putSlot(true, false);
  w.putSlot(false, false);
  // EOF:8 个连续 recessive,标记帧结束
  for (uint8_t i = 0; i < 8u; ++i) w.putSlot(true, false);

  if (w.n > slots_cap) return 0;
  return w.n;
}

// ---------------- 帧 → 数据层包 ----------------
void frameToPacket(const Frame& f, uint32_t rx_ms, VanPacket* out) {
  if (!out) return;
  out->iden = f.ident;          // 15 位原样带走
  out->cmd  = (uint8_t)(f.cmd & 0x0Fu);
  out->ack  = (uint8_t)(f.ack ? 1u : 0u);
  out->fcs_ok = (uint8_t)(f.fcs_ok ? 1u : 0u);
  uint8_t n = f.len;
  if (n > sizeof(out->data)) n = (uint8_t)sizeof(out->data);   // 按数据层容量截断
  out->len = n;
  for (uint8_t i = 0; i < n; ++i) out->data[i] = f.data[i];
  out->rx_ms = rx_ms;
}

}  // namespace van
