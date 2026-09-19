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
// 帧结构(TS = Time Slice = 线上一个位的时间)。
// ★ 下面这套是**实测**定案的(2026-09-19,5 分钟实车抓包 drive5min.csv,
//   17106/17106 帧;仓库里的 1.2s 切片 66/66 帧;两处独立抓包互证):
//   Start of frame 10 TS   固定 0000111101, 提供同步沿(**裸槽**,不走 4B5B)
//   Identifier     15 TS   3 个 4B5B 组 = **12 位**(线上读法;0x824 就是它)
//   Command         5 TS   1 个 4B5B 组 = 4 位(EXT / RAK / R-W / RTR)
//   Data         10n TS    0..224 字节,每字节 2 组
//   FCS            20 TS   4 个 4B5B 组 = **16 位 = 15 位 CRC + 1 个固定 0 位**
//   End of data     0 TS   ★ 不额外占槽:FCS 末字节的 bit0(恒 0)与它后面
//                          那个编码位(恒 dominant)构成一对 0 = 一次
//                          E-Manchester 违约。这一位就是 EOD。
//   Acknowledge     2 TS   1 个 recessive + 1 个 dominant(要有应答时);
//                          实测只在 CMD bit2=1 且 RTR=0(0xC/0xE)的帧上出现,
//                          0x8/0xF 的帧两槽都是 recessive ⇒ 直接并进空闲,
//                          帧体因此**少 2 槽**(这也是帧长随报文族变化的原因之一)
//   End of frame    8 TS   8 个连续 recessive
//   Inter-frame     8 TS   帧间至少 8 TS recessive(不算帧内)
// 于是帧的槽数 = 10+15+5+10n+20 (+2 若被应答) = 50 + 10n (+2)。
// 实测各报文族:0x824/0x8 = 120 槽(7 字节,无 ACK)、0x464/0xC = 102(5 字节,
// 有 ACK)、0x4EC/0xF = 50(0 字节,无 ACK)…… 16 个族全部逐帧吻合。
//
// 编码:E-Manchester(= 4B/5B)。每 4 个数据位编成 5 个 TS,第 5 位是
// 编码位(E-Manchester bit),解码时必须丢弃。
// ★ 实测的编码位规则比"丢弃第 5 位"更强:**第 5 位 = 第 4 位的反相**。
//   所以正常的 5 槽组永远含一次跳变,而**全帧恰好最后一组违反它**(= EOD)。
//   这条既是"帧尾在哪"的唯一可靠判据,也是下面 FCS 字段里那个"固定 0 位"
//   的由来(bit0=0 与 EOD 槽=0 相等 ⇒ 违约)。
// ★ 槽时间用**实测值 8.25µs(≈121kbit/s)**,不是规范的 125kbit/s:
//   2026-09-18/19 实车逻辑分析仪抓包(30s + 5min)拟合出槽时间 **8.250µs**,
//   跳变间隔全部落在它的 1/2/3/4/5 倍上;125 kbit/s ⟹ 8.00µs 只是
//   **规范标称值**,按它解码每 32 位就漂掉一个整槽 —— 这正是固件
//   `frames=0` 的直接原因(实测 3.1% 偏差)。下面的 kTsNs 是唯一时基。
// 由此得到一个很有用的性质:帧内最多 4 个连续 recessive,所以
// 「8 个连续 recessive」只能出现在 ACK/EOF —— 帧尾判据无歧义。
//
// 原"未验证项"三条(字节内位序 / FCS 字节序 / 206 实车 IDEN)已在 2026-09-19
// 用真实抓包全部定案,见上面对应的位置与 ACCEPTANCE.md 文末那条定案记录:
//   位序 = MSB-first;FCS = 16 位大端字段(高字节先发);206 实车 IDEN = 0x824。
// ============================================================

namespace van {

// 时间一律用 ns 记,内部不依赖浮点,保证宿主机测试与 MCU 行为逐位一致。
//
// ★ 唯一的时基是 kTsNs —— 任何"边沿/槽 → 时间"的换算都必须用它:
//   pushEdge 的 dt_ns → 槽数、测试夹具的槽宽、离线脚本的量化单元,全是它。
//   **不要再从 kBitRateHz 反推时基**:kBitRateHz 只是规范标称值(125 kbit/s
//   ⟹ 8.00µs),实车实测是 8.25µs;两者差 3.1%,照标称值解码必然漂槽。
static const uint32_t kBitRateHz   = 125000u;    // ★ 仅规范标称:125 kbit/s ≠ 实测
static const uint32_t kTsNs        = 8250u;      // 实车实测 ≈121 kbit/s:一个 TS = 8250 ns
static const uint8_t  kFcsSlots    = 20u;        // FCS 字段槽数(实测;4 个 4B5B 组)
static const uint8_t  kAckSlots    = 2u;         // ACK 槽数(实测;仅被应答的帧有)
static const uint32_t kSlotsMax    = 2300u;      // 10+15+5+224*10+20+2+8
static const uint8_t  kDataMax     = 224u;       // 协议允许的数据上限
// 常见 VAN-INFO 帧承载 28 字节;取 40 是为了同时兜住:
//   - VIN 帧(VinBus 那类)会上到 32 字节以上
//   - BitDecoder 的输出队列也按这个容量走,长空闲段不会溢出
static const uint8_t  kDataDefault = 40u;

// SOF / CMD 位定义
// ------------------------------------------------------------
// SOF:**固定的 10 TS 同步图案**,不是 4B5B 数据字节
// ------------------------------------------------------------
// 规范(Graham Auld,本文件一直在引的那份):SOF = 10 TS,0000111101。
//
// ★ 这是一个反复踩过的概念坑,写在这里免得再犯:
//   SOF **不走 4B5B**。它是"裸"的 10 个槽,后面才开始对
//   IDEN/CMD/DATA/FCS 做 4B5B 编码。所以:
//     · 不能用 putByte(0x0F) 生成它 —— putByte 把 0x0F 当数据位、
//       插进两个编码位,得到 0000111111(第 9 槽成了 1),
//       与规范 0000111101 差在第 9 槽 → matcher 永远对不上,
//       滑窗冻在 sofBits=9(实测症状)。
//     · 也不能拿"折 10 槽 == 0x0F"当 SOF 正确的证据 —— 折 4B5B 时
//       第 5/10 槽是被丢掉的编码位,折回 0x0F 只说明"数据位还是那个字节"。
//       真正按规范 0000111101 折(同样丢第 5/10 位)得到的是 0x0E。
//     · kSofByte = 0x0F 只是"前 8 个裸槽 = 00001111",不是折叠结果。
//
// 编码器与 matcher **共用下面这两个常数**。
static const uint16_t kSofPattern = 0x003Du;     // 0000111101(MSB 先写)
static const uint8_t  kSofSlots   = 10u;
static const uint8_t  kSofByte    = 0x0Fu;       // 仅指"前 8 个裸槽 = 00001111"
static const uint8_t kCmdExtMask   = 0x8u;       // CMD 位 3:EXT,保留,应为 1
// CMD 位 2:规范注释写的是 RAK(旧注释写"1 = 无需应答")。
// ★ 实测(2026-09-19,17106 帧)**与旧注释相反**:FCS 之后到底有没有 ACK 两槽,
//   与这一位一一对应 ——
//     bit2=1 且 RTR=0(0xC / 0xE):帧尾多 2 槽,第 2 槽 dominant ⇒ 有接收方应答
//     bit2=0(0x8)或 RTR=1(0xF):帧尾没有那 2 槽(全 recessive,并进空闲)
//   所以按实测理解:这一位表示"本帧**要**应答"。语义名字先不改(会牵动日志/
//   上层),但判断一律以实测为准 —— encodeFrame 就按上面这条写 ACK 槽。
static const uint8_t kCmdRakMask   = 0x4u;
static const uint8_t kCmdRwMask    = 0x2u;       // CMD 位 1:1 = 读, 0 = 写
static const uint8_t kCmdRtrMask   = 0x1u;       // CMD 位 0:1 = 无数据(请求)

// 帧尾到底有没有那 2 个 ACK 槽(实测规律,见上面 kCmdRakMask 的说明)
inline bool cmdExpectsAck(uint8_t cmd) {
  return (cmd & kCmdRakMask) != 0u && (cmd & kCmdRtrMask) == 0u;
}

// 帧阶段。
enum class FramePhase : uint8_t { Idle = 0, InFrame = 1, Eof = 2 };

// CRC-15(CAN-15 那条):x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1
// ★ **不是** VAN 的 FCS —— 只留给历史测试当"另一条多项式"的对照,别拿它校验帧。
uint16_t crc15(const uint8_t* data, uint16_t len);
uint16_t crc15_extend(uint16_t crc, uint8_t byte);

// ---- IDEN 字段的字节布局(编解码唯一的契约,只在这里定义) ----
// 规范里 IDEN 是 **15 位**,但线上只用 15 个 TS = 3 个 4B5B 组承载 **12 位**,
// 所以实际能收到、能还原的就是 12 位(0x824 这类);bit12..14 不在线上。
// ★ 字节边界是**实测**定案的(2026-09-19 实车抓包);三个 4B5B 组按"每 2 组
//   1 个字节"落进字节流,于是:
//     byte1 = IDEN 的 bit11..4      ← 组 1(bit11..8)+ 组 2(bit7..4)
//     byte2 的高 4 位 = IDEN 的 bit3..0 ← 组 3
//     byte2 的低 4 位 = CMD
//   ★ 旧实现是 byte1 = IDEN 低 8 位、byte2 高 4 位 = IDEN 高位 —— **错的**,
//     它与唯一一份真实抓包解出的字节流(0x82 0x48 ⇒ IDEN 0x824)对不上;
//     真实抓包测试里也早写了这条不一致,只是当时没改。现在按实测改掉。
inline uint8_t idenByte1(uint16_t iden) { return (uint8_t)((iden >> 4) & 0xFFu); }
inline uint8_t idenByte2(uint16_t iden, uint8_t cmd) {
  return (uint8_t)(((iden & 0x0Fu) << 4) | (cmd & 0x0Fu));
}
// 12 位从字节还原(bit0..11)
inline uint16_t idenFromBytes(uint8_t b1, uint8_t b2) {
  return (uint16_t)((((uint16_t)b1 << 4) | ((uint16_t)b2 >> 4)) & 0x0FFFu);
}
inline uint8_t cmdFromByte2(uint8_t b2) { return (uint8_t)(b2 & 0x0Fu); }
// IDEN 的 bit12..14(15 位里线上没传的部分,恒 0)
inline uint8_t idenHighBits(uint16_t iden) { return (uint8_t)((iden >> 12) & 0x07u); }
inline uint16_t makeIden(uint16_t iden12, uint8_t high3) {
  return (uint16_t)((iden12 & 0x0FFFu) | ((uint16_t)(high3 & 0x07u) << 12));
}

// 一帧解出来的结果
struct Frame {
  // 12 位 IDEN(bit0..11);高 3 位线上不存在,默认 0(见 idenHighBits 说明)。
  uint16_t ident    = 0;       // 12 位有效
  uint8_t  cmd      = 0;       // 4 位命令字段(EXT 位隐含为 1)
  uint8_t  ack      = 0;       // 1 = 总线有应答(帧尾 ACK 位为 dominant)
  uint8_t  data[kDataDefault] = {0};
  uint8_t  len      = 0;
  uint16_t fcs      = 0;       // 线上收到的 FCS(= 16 位字段的 bit15..1)
  uint16_t fcs_calc = 0;       // 本地算出的 FCS
  bool     fcs_ok   = false;
  bool     fcs_le   = false;   // ★ 字段序已定:16 位大端(高字节先发),恒 false。
                               //   保留这个成员只为兼容旧调用方,不要再按它分支。
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
// crc15():多项式 0x4599 = **CAN-15**,不是 VAN 的。
//   ★ 它**不是**本总线的 FCS。实测(17106 帧)0 命中。只留给历史测试当对照,
//     新代码一律不许用它校验帧。
//
// crc15_van_iso():**就是 VAN 的 FCS,已定案**。
//   多项式 x^15+x^11+x^10+x^9+x^8+x^7+x^4+x^3+x^2+1(完整掩码 0x8F9D,
//   寄存器 15 位故实取 0x0F9D),初值 0x7FFF,输出取反(^0x7FFF),
//   MSB-first、不反射。
//   覆盖范围:IDEN(12 位)+ CMD(4 位)+ 全部数据字节,按线上字节流喂进来
//   —— 也就是 [idenByte1, idenByte2, data...] 这 2+len 个字节。
//   **不含 SOF,也不含 FCS 自己**。
//   ★ 它给出 15 位值;线上那个 16 位字段 = (crc << 1),最低位恒 0。
//   ★ 实测:drive5min.csv 17106/17106 帧、sample-diffmanchester.csv 66/66 帧
//     逐帧命中(两条独立抓包)。复现工具:tools/van-decode/fit_fcs.py。
//   ★ 历史上"复现不出公开抓包的 FCS"的原因**不是多项式**:旧代码用的是
//     错的字段边界(18 槽 FCS + IDEN 低位先行的字节打包),覆盖的字节本身就
//     不是线上那几个字节。边界一对,这条多项式立刻全中。
uint16_t crc15(const uint8_t* data, uint16_t len);
uint16_t crc15_van_iso(const uint8_t* data, uint16_t len);

// FCS 字段的编解码:线上是 16 位大端(2 个字节,高字节先发),
// 内容 = [15 位 CRC][1 个固定 0 位]。那个固定 0 位是 EOD 的一半(见文件头),
// 所以校验前必须先确认它是 0 —— 否则等于把 EOD 当成了 CRC 的一位。
inline uint16_t fcsFieldFromCrc(uint16_t crc) { return (uint16_t)((crc & 0x7FFFu) << 1); }
inline uint16_t fcsCrcFromField(uint16_t field) { return (uint16_t)((field >> 1) & 0x7FFFu); }
inline bool fcsFieldWellFormed(uint16_t field) { return (field & 1u) == 0u; }

// 帧字节解析:把线上字节流 [IDEN(byte1), IDEN/CMD(byte2), DATA..., FCS 2 字节]
// 解成 Frame。数据长度未知,用 FCS 反推:候选长度从长到短逐个算 crc15_van_iso
// 与末尾两字节比对(长优先 —— 真实帧长就是缓冲里那一条)。
// 返回 true ⇒ fcs_ok=true 且 fcs/fcs_calc 都是那个 15 位值。
// 返回 false ⇒ 没有任何长度吻合(帧结构或 FCS 约定与实际不符)。
//
// ★ 长度为什么能反推:线上字节流是 2 字节头 + n 字节数据 + 2 字节 FCS,
//   数据长度由报文族(IDEN/CMD)决定,帧里没有长度字段 —— 只能靠 FCS 定位。
//   这条约定已用真实抓包定案,见上面 crc15_van_iso 的实测说明。
bool parseFrameBytes(const uint8_t* bytes, uint16_t n, Frame* out);

// 解出的字节的去处。设置接收器后,字节在**解出时立即**回调,
// 而不是等调用方 drain 队列 —— 一个边沿区间内可能同时含数据字节和
// 帧尾(EOF),若等区间处理完再取,EOF 之后的字节会丢。
class ByteSink {
public:
  virtual ~ByteSink() = default;

  // ★ SOF 命中时**先**调这个,再调 onByte()。
  //
  // 为什么必须有它:pushEdge 一次只能回一个 Ev,而**一次边沿里会先出现
  // SOF、紧接着吐出 IDEN 字节**(SOF 之后紧跟的槽会立刻凑出第一个字节)。
  // 靠"边沿返回后再判断 phase"必然丢一头 —— 要么解析器没武装就丢字节,
  // 要么漏掉帧起点。有了这个回调,帧起点与数据字节走**同一条时序**:
  //   onFrameStart() → onByte(IDEN 低字节) → onByte(其余…)
  // 调用保证:此时 SOF 的 10 槽已收完、相位已清零、
  // 本沿里**还没有**任何数据字节被吐出。
  virtual void onFrameStart() {}

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

  // 当前帧阶段(诊断用;帧的收尾以 FrameParser 是否有待收帧为准)
  FramePhase phase() const { return mPhase; }

  // 收尾之后把阶段落回 Idle(仅供 VanPhyWire::finish 收尾用)。
  // 单独开这个口子而不是复用 resync():resync() 会一并清掉相位对齐状态,
  // 在这里用它反而会让 finish() 之后读到的 phase 不是 Idle。
  void markIdle() { mPhase = FramePhase::Idle; mArmed = false; }

  // SOF 匹配进度(诊断/测试用)
  uint8_t sofBits() const { return mSofBits; }

  // 诊断快照:排查"帧为什么没收尾"时,光看返回值(Ev)不够,
  // 需要同时看到相位与连续 recessive 计数。有了它就不用往库里插
  // #ifdef printf 探针(上两次就因为探针编译不过白费了两轮)。
  struct Snap {
    FramePhase phase = FramePhase::Idle;
    uint16_t   bit_count = 0;      // 当前字节已过的 TS 数(0..9)
    uint16_t   recessive_run = 0;  // 连续 recessive 槽计数
    uint8_t    dom_run = 0;        // 连续 dominant 槽数(EOD 判据)
    uint8_t    since_eod = 0;      // EOD 之后过了几个槽(255 = 还没见到 EOD)
    // 帧尾 ACK 判据的四个量(见 ackDominant()):最近两个 dominant 段的长度与间隔
    uint8_t    last_dom_run = 0;
    uint8_t    prev_dom_run = 0;
    uint8_t    last_dom_gap = 0;
    uint8_t    cur_gap = 0;
    bool       ack_dominant = false;
    bool       armed = false;      // 是否已见过 SOF(开始收字节)
    bool       has_level = false;
    bool       level = false;
    int        queued = 0;
    bool       overflow = false;
    bool       need_resync = false;
  };
  Snap snap() const;

  // ★ 明确结束当前帧:把"帧界"从"还有没有边沿"里解耦出来。
  //   正在收帧 → 丢弃尾部半截位、切到 Eof、返回 Ev::EndOfFrame
  //   不在帧内 → 返回 Ev::None(幂等)
  //   注意:它**不关帧** —— 真正调 FrameParser::endFrame() 的是
  //   VanPhyWire::finish()。这里只负责让解码器放下尾部残留。
  Ev finish();

  // 最近一帧的 ACK 位是否为 dominant(1 = 总线上有接收方应答)。
  //
  // ★ 判据按**实测**的帧尾结构来(2026-09-19,17106 帧):
  //     帧体 = …数据 + FCS(20 槽)。FCS 末字节的 bit0(=固定 0)与紧随的
  //     编码位(=EOD)连成 2 个 dominant;再往后 2 个槽才是 ACK:
  //       未被应答:两槽都 recessive(与 EOF/空闲连成一片)
  //       被应答  :第 1 槽 recessive、第 2 槽 dominant
  //   所以尾部形态只有两种,而且**都能从"最后两个 dominant 段"认出来**:
  //     被应答  = [≥2 dominant 的 EOD 段] [1 个 recessive] [1 个 dominant]
  //     未应答  = [≥2 dominant 的 EOD 段] [recessive …]
  //   实现就按这个:记住最后两个 dominant 段的长度与它们之间的 recessive 间隔,
  //   ack = (最后一段长 1)&&(间隔 1)&&(前一段长 ≥2)。
  //   ★ 旧实现是"EOD 之后 4 槽窗口内出现过 dominant",实测下**恒为 false**
  //     (窗口第一槽是 EOD 自己的第 2 个 dominant,第 2 槽是 ACK 的 recessive,
  //      一进窗口就被清掉,之后再也不会被置回)。所以旧代码里 ack 一直是 0。
  bool ackDominant() const { return mAckDominant; }

 private:
  static const uint8_t kQueueMax = 40;   // 与 kDataDefault 对齐,避免长空闲段溢出
  void processSlot(bool level);
  // SOF 槽级匹配:命中时先 onFrameStart() 再返回 true(清相位、armed、InFrame)
  bool sofFeed(bool level);

  uint64_t mCurNs    = 0;
  bool     mHasLevel = false;
  bool     mLevel    = false;
  uint16_t mBitCount = 0;      // 当前字节已过的 TS 数
  uint8_t  mMask     = 0x80u;
  uint8_t  mByte     = 0;
  uint16_t mRecessiveRun = 0;  // 连续 recessive 槽计数(EOF 判据)
  uint8_t  mDomRun = 0;        // 当前连续 dominant 槽数(EOD 判据)
  uint8_t  mSinceEod = 0;      // EOD 之后过了几个槽(255 = 还没见到 EOD;诊断用)
  // ---- 帧尾 ACK 判据的状态:最近两个 dominant 段的长度与间隔 ----
  // (为什么不用 mSinceEod 那一套:见 ackDominant() 的说明)
  uint8_t  mLastDomRun = 0;    // 最近一个已结束的 dominant 段长度
  uint8_t  mPrevDomRun = 0;    // 再往前那一段的长度
  uint8_t  mLastDomGap = 0;    // 最近一段之前的 recessive 间隔(槽)
  uint8_t  mCurGap = 0;        // 当前这一段 dominant 之前的 recessive 间隔
  bool     mAckDominant = false;   // 由上面三个数在每槽重算(见 ackDominant())
  bool     mNeedResync = false;    // 见过 EOF,下一沿前重新对齐字节相位
  bool     mArmed = false;         // 见到 SOF 后才开始把字节入队
  FramePhase mPhase = FramePhase::Idle;   // 帧阶段(finish() 用)
  uint8_t  mSofBits = 0;           // SOF 匹配器已收的槽数(0..kSofSlots)
  uint16_t mSofAcc = 0;            // SOF 匹配器移位寄存器(低位是最后收到的槽)
  bool     mEofLatched = false;    // 本次空隙已报过(避免重复报)
  // 帧间空隙超时(默认 1ms ≈ 121 槽)。必须远大于帧内最长连续 recessive
  // (全 1 数据字节 = 10 槽 ≈ 82.5µs @8.25µs/槽),又远小于帧间空闲(实车常见 ms 级)。
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

  // ★ 明确声明"新的一帧从这里开始"。
  //   帧起点由 BitDecoder 匹配到 10 槽 SOF 后经 ByteSink::onFrameStart()
  //   回调过来(VanPhyWire 的 relay 接的);解析器**不再**靠
  //   "首字节 == 0x0F"猜起点 —— 那条判据既与规范 SOF 不符,
  //   也会把帧内数据里的 0x0F 误判成新帧。
  void beginFrame(uint64_t ns);

  // 喂一个解码出的字节;只有 beginFrame() 之后才会被收下
  bool pushByte(uint8_t b, uint64_t ns, Frame* out);

  // 缓冲里是否已经凑出一个**FCS 校验通过**的完整帧。
  // 这是唯一可靠的"帧已完整"判据 —— "8 个连续 recessive" 会被帧内
  // 合法数据误触发(实测 8A 22 5A 这帧的帧体里就有一段),不能当帧尾用。
  bool hasCompleteFrame() const;

  // 通知"帧结束":收尾当前帧。
  // ack_dominant 传 BitDecoder::ackDominant(),会记进 Frame::ack。
  bool endFrame(uint64_t ns, Frame* out, bool ack_dominant = false);

  // 手上是否攒着一帧还没收尾(诊断/测试用)。
  // 区分"根本没收到帧"和"收到了但没收尾" —— 这两种的修法完全不同,
  // 只看 frames 计数分不出来。
  bool inFrame() const { return mInFrame; }
  uint16_t pendingBytes() const { return mCount; }

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
