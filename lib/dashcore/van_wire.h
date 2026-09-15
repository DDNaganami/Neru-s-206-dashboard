#pragma once
#include <stdint.h>
#include "van_source.h"   // VanPacket
// ============================================================
// VAN 线路层（PSA VAN bus wire layer）—— 纯逻辑,无 Arduino/寄存器依赖
//
// 依据: Graham Auld「VAN bus line protocol」
//   http://graham.auld.me.uk/projects/vanbus/lineprotocol.html
// 经 morcibacsi/VanAnalyzer 转述并实现(可作对照实现):
//   https://github.com/morcibacsi/VanAnalyzer
//
// 帧结构(TS = Time Slice = 线上一个位的时间):
//   Start of frame 10 TS   固定 0000111101, 提供同步沿
//   Identifier     15 TS   低值 ID = 高优先级仲裁; 0x000/0xFFF 保留
//   Command         5 TS   EXT / RAK / R-W / RTR 四位
//   Data         8n TS     0..224 字节
//   FCS            18 TS   CRC-15, 覆盖 identifier+command+data
//   End of data     2 TS   一对 0 = 一次 E-Manchester 违约, 标记数据结束
//   Acknowledge     2 TS   两个 recessive; 接收方在第 2 位发 dominant 应答
//   End of frame    8 TS   8 个连续 recessive
//   Inter-frame     8 TS   帧间至少 8 TS recessive(不算帧内)
//
// 编码:E-Manchester(= 4B/5B)。每 4 个数据位编成 5 个 TS,第 5 位是
// 编码位(E-Manchester bit),解码时必须丢弃。125 kbit/s 时 1 TS = 8 µs。
// 由此得到一个很有用的性质:帧内最多 4 个连续 recessive,所以
// 「8 个连续 recessive」只能出现在 ACK/EOF —— 帧尾判据无歧义。
//
// 未验证项(待实车/逻辑分析仪确认,见 test_van_wire.cpp 顶部说明):
//   字节内的位序(本文档按 VanAnalyzer 的 MSB-first 实现)
//   FCS 两字节的先后顺序(代码里两种都试,以真实抓包为准)
//   206 实车 IDEN 与 307 参考值是否一致
// ============================================================

namespace van {

// 1 TS = 1/125000 s = 8 µs。时间一律用 ns 记,内部不依赖浮点,
// 保证宿主机测试与 MCU 行为逐位一致。
static const uint32_t kBitRateHz   = 125000u;
static const uint32_t kTsNs        = 8000u;      // 一个 TS = 8000 ns
static const uint32_t kSlotsMax    = 2112u;      // SOF10+IDEN15+CMD5+224B*10+FCS18+EOD/ACK/EOF
static const uint8_t  kDataMax     = 224u;       // 协议允许的数据上限
// 常见 VAN-INFO 帧承载 28 字节;取 40 是为了同时兜住:
//   - VIN 帧(VinBus 那类)会上到 32 字节以上
//   - BitDecoder 的输出队列也按这个容量走,长空闲段不会溢出
static const uint8_t  kDataDefault = 40u;

// SOF / CMD 位定义
static const uint8_t kSofByte      = 0x0Fu;      // SOF 前 8 个 TS:0000 1111
static const uint8_t kCmdExtMask   = 0x8u;       // CMD 位 3:EXT,保留,应为 1
static const uint8_t kCmdRakMask   = 0x4u;       // CMD 位 2:RAK,1 = 无需应答
static const uint8_t kCmdRwMask    = 0x2u;       // CMD 位 1:1 = 读, 0 = 写
static const uint8_t kCmdRtrMask   = 0x1u;       // CMD 位 0:1 = 无数据(请求)

// CRC-15:x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1
uint16_t crc15(const uint8_t* data, uint16_t len);
uint16_t crc15_extend(uint16_t crc, uint8_t byte);

// ---- IDEN 字段的字节布局(编解码唯一的契约,只在这里定义) ----
// 协议规范的 IDEN 是 **15 位**(0x000 与 0xFFF 保留为特殊用途)。
// 线上用 15 个 TS 承载,VanAnalyzer 实测按"整字节"取,即:
//   byte1 = IDEN 的 bit0..7        (低 8 位)
//   byte2 的高 4 位 = IDEN 的 bit8..11
//   byte2 的低 4 位 = CMD
// 也就是一次整字节读取只能拿到 12 位,IDEN 的 bit12..14 落在 SOF 字段里
// (SOF 若是 10 TS 就与 IDEN 重叠 2 位)。**这一步换算尚未用真实位流确认**,
// 所以本实现:
//   - 结构里保留完整的 15 位 iden(能表达 0x000/0xFFF)
//   - 字节编解码只搬运 12 位,bit12..14 由 iden_high_bits 单独携带
// 实车抓到原始位流后,只需改这四个 helper 与 parseFrameBytes 的取位。
inline uint8_t idenByte1(uint16_t iden) { return (uint8_t)(iden & 0xFFu); }
inline uint8_t idenByte2(uint16_t iden, uint8_t cmd) {
  return (uint8_t)(((cmd & 0x0Fu) << 4) | ((iden >> 8) & 0x0Fu));
}
// 12 位从字节还原(bit0..11)
inline uint16_t idenFromBytes(uint8_t b1, uint8_t b2) {
  return (uint16_t)(((uint16_t)(b2 & 0x0Fu) << 8) | b1);
}
inline uint8_t cmdFromByte2(uint8_t b2) { return (uint8_t)((b2 >> 4) & 0x0Fu); }
// IDEN 的 bit12..14(15 位里超出整字节读取的部分)
inline uint8_t idenHighBits(uint16_t iden) { return (uint8_t)((iden >> 12) & 0x07u); }
inline uint16_t makeIden(uint16_t iden12, uint8_t high3) {
  return (uint16_t)((iden12 & 0x0FFFu) | ((uint16_t)(high3 & 0x07u) << 12));
}

// 一帧解出来的结果
struct Frame {
  // 完整 15 位 IDEN:bit0..11 来自整字节读取,bit12..14 单独携带
  // (见上面 idenHighBits 的说明)。这样 0x000/0xFFF 保留值也能表达。
  uint16_t ident    = 0;       // 15 位有效(高 3 位默认 0)
  uint8_t  cmd      = 0;       // 4 位命令字段(EXT 位隐含为 1)
  uint8_t  ack      = 0;       // 1 = 总线有应答(帧尾 ACK 位为 dominant)
  uint8_t  data[kDataDefault] = {0};
  uint8_t  len      = 0;
  uint16_t fcs      = 0;       // 线上收到的 FCS
  uint16_t fcs_calc = 0;       // 本地算出的 FCS
  bool     fcs_ok   = false;
  bool     fcs_le   = true;    // FCS 两字节顺序:true = 低字节先到
  bool     overflow = false;   // 数据超过 kDataDefault,已截断
  uint64_t start_ns = 0;
  uint64_t end_ns   = 0;
};

// iden12 为 0 时按"整字节读取的 12 位"给出,高位单独设置
inline void setIden(Frame& f, uint16_t iden12, uint8_t high3) {
  f.ident = makeIden(iden12, high3);
}

// 已知的 cmd 字段解码(便于日志/调试打印)
struct CmdBits { bool ext; bool rak; bool rw; bool rtr; };
CmdBits decodeCmd(uint8_t cmd);

// ---- CRC-15:两个不同的多项式,别混用 ----
//
// crc15():当前**在用**的那个,多项式 0x4599。
//   ★ 0x4599 其实是 **CAN-15** 的多项式,不是 VAN/TSS463 的。
//     实测:它对 "123456789" 给出 0x059E,与 CAN-15 的公认校验值一致。
//   ★ 它也**复现不出**公开抓包的 FCS(见下),所以它现在同时是
//     "可能用错了" + "无法验证" 的状态。
//
// crc15_van_iso():按 Graham Auld 描述 + TSS463 手册写的那条 ——
//   多项式 x^15+x^11+x^10+x^9+x^8+x^7+x^4+x^3+x^2+1(完整掩码 0x8F9D,
//   寄存器 15 位故实取 0x0F9D),初值 0x7FFF,发送前取反。
//   ★ 它同样**复现不出**那 5 帧公开抓包的 FCS,详见 test_van_wire.cpp 里
//     test_van_iso_crc_against_public_frames 的结论 —— 那条测试把
//     "所有 15 位多项式 + 初值/取反/左右移 + 所有 FCS 分界" 全枚举了一遍,
//     命中数为 0。也就是说**问题不在多项式选哪个**。
uint16_t crc15(const uint8_t* data, uint16_t len);
uint16_t crc15_van_iso(const uint8_t* data, uint16_t len);

// 帧字节解析:把 IDEN,CMD,DATA...,FCS_lo,FCS_hi 尝试解成 Frame。
// 数据长度未知,用 FCS 反推:从最短候选长度起逐个算 CRC-15 比对。
// 返回 true 表示有候选长度 FCS 吻合(fcs_ok=true,且记录本轮 fcs_le),
// 返回 false 表示没有任何长度吻合(帧结构或 FCS 约定与实际不符)。
//
// 已知未定项:CRC-15 多项式取自公开规范;但用 VanAnalyzer readme 的真实
// 抓包做全多项式枚举后仍无法复现其 FCS,说明覆盖范围/字节序/字段拆解与
// 公开描述有出入。实车第一帧必须验这条(见 test_van_wire.cpp 的说明)。
bool parseFrameBytes(const uint8_t* bytes, uint16_t n, Frame* out);

// 解出的字节的去处。设置接收器后,字节在**解出时立即**回调,
// 而不是等调用方 drain 队列 —— 一个边沿区间内可能同时含数据字节和
// 帧尾(EOF),若等区间处理完再取,EOF 之后的字节会丢。
class ByteSink {
public:
  virtual ~ByteSink() = default;
  virtual void onByte(uint8_t b) = 0;
};

// ------------------------------------------------------------
// 位流解码器:输入是按时间排好的电平边沿,输出 4B/5B 解出的字节。
// 不在任何地方访问寄存器/中断,宿主机可喂合成波形,
// MCU 侧把 GPIO 中断记下的边沿时间戳喂进来即可。
// ------------------------------------------------------------
class BitDecoder {
 public:
  // 一次 pushEdge 的结果
  enum class Ev : uint8_t {
    None,      // 这一沿没有完成任何事件
    Byte,      // 至少解出 1 个字节,用 available()/takeByte() 逐个取走
    EndOfFrame // 连续 8 个 recessive = 帧结束
  };

  // 设了 sink 就走回调(推荐:不丢字节);没设就进队列等 drain。
  void setByteSink(ByteSink* s) { mSink = s; }

  void reset();

  // 重新对齐字节相位:丢掉半截字节、把 TS 计数与掩码归零。
  // 帧间空隙(>=8 个连续 recessive)之后必须重新对齐 —— 总线空闲的长度
  // 不受控,若不归零,残留的半个字节会让下一帧整体错位
  // (实测:1000 槽空闲后首帧被解成 0x70 而不是 0x0F)。
  // 解码器在 EndOfFrame 之后会自动做这件事。
  void resync();

  // 帧间空隙的超时阈值(µs)。两次边沿间隔超过它,说明总线进入空闲:
  // 该区间不再逐槽展开,直接重新对齐相位。真实硬件必须有这条 ——
  // 否则一个几十毫秒的空闲会被当成上千个帧内槽处理。
  void setGapTimeoutUs(uint32_t us) { mGapTimeoutNs = (uint64_t)us * 1000ull; }

  // 喂一次电平变化。ns = 该边沿时间戳(ns,单调递增);
  // level = 变化后的电平(true = recessive/高阻).
  //
  // 一个边沿到下一个边沿之间可能横跨很多个槽(总线空闲时尤其明显),
  // 所以这一沿可能一次解出多个字节:结果进内部队列,调用方必须
  // 用 available()/takeByte() 取空后再喂下一沿。
  // (早期版本每沿只返回一个字节就提前返回,把区间里剩余的槽位丢掉了,
  //  一段空闲之后整帧错位 —— 这是设计缺陷,不要再退回去。)
  Ev pushEdge(uint64_t ns, bool level);

  int available() const { return mCount; }

  // 取走一个已解出的字节(队列空时返回 0)
  uint8_t takeByte();

  // 只读查看队列头的值(调试用)
  uint8_t peekByte() const { return mCount ? mQueue[mHead] : 0; }

  // 最近一帧的 ACK 位是否为 dominant(1 = 总线上有接收方应答)。
  //
  // 协议里 ACK 是 EOD 之后的 2 个 TS,第 2 位被接收方拉成 dominant 表示应答。
  // 识别方法(不能简单看"帧尾前有没有 dominant",数据里到处是 dominant):
  //   1) EOD = 恰好 2 个 dominant 落在两个 recessive 之间(一次 E-Manchester 违约)
  //   2) EOD 之后的 4 个槽 = ACK(2) + EOF 起头 2 个,窗口内出现 dominant 即为应答
  // 本项目只监听不应答,所以这个位反映的是"总线上别的节点有没有在应答",
  // 同时也可用来判断帧尾判据是否可靠(ACK 是 recessive 时全 1 数据可能误判帧尾)。
  bool ackDominant() const { return mAckDominant; }

 private:
  static const uint8_t kQueueMax = 40;   // 与 kDataDefault 对齐,避免长空闲段溢出
  void processSlot(bool level);

  uint64_t mCurNs    = 0;
  bool     mHasLevel = false;
  bool     mLevel    = false;
  uint16_t mBitCount = 0;      // 当前字节已过的 TS 数
  uint8_t  mMask     = 0x80u;
  uint8_t  mByte     = 0;
  uint16_t mRecessiveRun = 0;  // 连续 recessive 槽计数(EOF 判据)
  uint8_t  mDomRun = 0;        // 当前连续 dominant 槽数(EOD 判据)
  uint8_t  mSinceEod = 0;      // EOD 之后过了几个槽(255 = 还没见到 EOD)
  bool     mAckDominant = false;   // ACK 窗口内是否出现 dominant
  bool     mNeedResync = false;    // 见过 EOF,下一沿前重新对齐字节相位
  bool     mArmed = false;         // 见到 SOF 后才开始把字节入队
  bool     mEofLatched = false;    // 本次空隙已报过帧尾(避免重复报)
  // 帧间空隙超时(默认 1ms = 125 槽)。必须远大于帧内最长连续 recessive
  // (全 1 数据字节是 10 槽 = 80µs),又远小于帧间空闲(实车常见 ms 级)。
  uint64_t mGapTimeoutNs = 1000000ull;

  uint8_t  mQueue[kQueueMax] = {0};
  uint8_t  mHead = 0;
  uint8_t  mCount = 0;
  ByteSink* mSink = nullptr;   // 非空则字节直接回调,不入队
  bool     mOverflow = false;  // 队列满导致丢字节(异常,应记录)

 public:
  bool overflowed() const { return mOverflow; }
};

// ------------------------------------------------------------
// 帧解析器:把 BitDecoder 的字节流按 SOF 分帧,维护每帧的字节缓冲,
// 遇到 EOF 判据时用 parseFrameBytes() 校验 FCS。
// ------------------------------------------------------------
class FrameParser {
 public:
  void reset();

  // 喂一个解码出的字节;返回 true 表示 out 是一帧(可能 fcs_ok=false)
  bool pushByte(uint8_t b, uint64_t ns, Frame* out);

  // 缓冲里是否已经凑出一个**FCS 校验通过**的完整帧。
  // 这是唯一可靠的"帧已完整"判据 —— "8 个连续 recessive" 会被帧内
  // 合法数据误触发(实测 8A 22 5A 这帧的帧体里就有一段),不能当帧尾用。
  bool hasCompleteFrame() const;

  // 通知"帧结束"(总线空闲或超时):收尾当前帧。
  // ack_dominant 传 BitDecoder::ackDominant(),会记进 Frame::ack。
  bool endFrame(uint64_t ns, Frame* out, bool ack_dominant = false);

 private:
  bool mInFrame = false;
  uint16_t mCount = 0;
  uint8_t  mBuf[kDataDefault + 4] = {0};
  uint64_t mStartNs = 0;
  Frame    mFrame;
};

// ------------------------------------------------------------
// 编码器(测试/仿真/回放用;固件不发送):
// 把一帧编成 4B/5B 的槽序列,slots[i] = true 表示 recessive。
// 返回写入槽数,0 表示参数非法。
// ------------------------------------------------------------
uint32_t encodeFrame(const Frame& f, uint8_t* slots, uint32_t slots_cap);

// 把线路层解出的一帧转成数据层的 VanPacket
// (15 位 IDEN 原样带走;data 超过 VanPacket 容量时按上限截断)
void frameToPacket(const Frame& f, uint32_t rx_ms, VanPacket* out);

}  // namespace van
