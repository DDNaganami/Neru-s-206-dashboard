#pragma once
#include <stdint.h>

#include "link_frame.h"
#include "link_phy.h"

// ============================================================
// 双板链路 —— **测速 / 测丢包**（RF 实测那一单要跑的那一件东西）
//
// 用途只有一句话：**回答"这条链路能不能用"**。判据是契约给的量级
// （TICK 50 Hz + DATA ~80 Hz，见 §1.1 那张预算表），而不是"看着挺快"。
//
// ★★ 本文件**不新造一条链路、也不动协议**（这是硬约束）：
//   · 线上跑的仍旧是 v1 帧（`encodeFrame()`：SYNC/VER/TYPE/LEN/ROLE/CRC-15
//     一个字节都没变），RX 侧仍旧用 `LinkRx` 解帧、CRC 不过就丢；
//   · 唯一的差别是 **DATA 帧的载荷里放的是本文件定义的"测量信封"**
//     （魔数 + 版本 + 序号 + 发端时刻），而不是四个车辆标量 —— 因为 DATA 的载荷
//     对接收侧本来就是**不透明的**（宽度由 §2 的 LEN 表定死、语义由 §3 的字段表定），
//     而"载荷里放什么"是发送侧的事（同一条已在用：`lib/dashcore/image_blob.h`
//     用 `TYPE=0x20` + `LEN=16` 承载图片二进制）。
//   · ★ 信封 **9 B** ⇒ 帧长 16 B（= 7 + 9；契约 DATA 是 13 B，所以测量帧长 3 B）。
//     这一点点变长对三条判据（丢包率 / 间隔 / 抖动）**没有影响** —— 它们与帧长无关；
//     换来的是"序号 + 发端时刻"两个字段都有足够宽度。
//     ★ **契约那三种帧的长度一个字节都没动**（`kDataLen` 仍是 6 B）：测量帧只是把
//       **同一个 `TYPE = 0x20`** 的载荷填到 9 B，多出来的是冗余。
//   · ★ 承载它的还是 **0x20 = DATA**（契约里已有的类型）⇒ **没有新增消息类型**。
//
// ★★ 两个"时刻"都有，而且**分开报**（这条是本文件的判据核心）：
//   · `gap_*`（收端）= **包到达那一刻**（WiFi 任务的接收回调里 `millis()`）之间的间隔
//     ⇒ 它量的是**链路**；
//   · `wire_gap_*`（发端）= 信封里带的**发端**时刻之间的间隔
//     ⇒ 它量的是"**发端究竟有没有按节奏发**"。
//   ⇒ 两个都看：`gap` 大而 `wire_gap` 小 ⇒ 是**收端本机**（主循环/LVGL）在拖，
//     不是链路；两个都大 ⇒ 是**发端**没发出来。混在一起看就会得出错结论。
//
// ★★ 判据门槛（**数值的唯一出处在这里**，文档与日志都引它）：
//   | 量 | 门槛 | 为什么是这个数 |
//   |---|---|---|
//   | **丢包率** | < **0.1 %** | §4 的三级超时靠 TICK 兜：50 Hz 下丢一帧 ≈ 20 ms 空档，`>100 ms` 才失基准 ⇒ 单帧丢得起的量级是"千分之一档"，不是"百分之一档"。0.1 % ≈ "一分钟里最多丢 3 帧"。 |
//   | **连续最大间隔** | < **100 ms** | 这是 §4 给从板的**第一档**超时（`>100 ms` 失基准）。链路自己不许去撞它 —— 撞上就等于"链路在替时基状态机做决定"。 |
//   | **p99 抖动** | < **20 ms** | 一个 TICK 周期（50 Hz）。两屏相位对齐要的是"节奏稳"；抖动超过一格就意味着**偶尔少一拍**。 |
//   | 最小样本量 | ≥ **300 帧** | 0.1 % 的丢包率要 1000 帧才分辨得出（1 帧 = 0.1 %）；300 帧是"**能不能用**"这条粗判据的下限 —— 低于它只报数、**不下结论**（`verdict()` 返回"样本不足"）。 |
//
//   ★★ 达不到就**如实报"不适合"**：`kMeasThresholds` 是常量、`measPass*()` 是纯函数，
//     **没有**"差一点就放过"的口子。也**不许**为了让某次实测好看而把这三个数改大 ——
//     要改就改契约（§1.1 / §4），并写清为什么。
// ============================================================

namespace dashlink {

// ---- 测量信封（放在 DATA 帧的载荷里，见文件头） ----
// 线上形状（**9 B**，逐字段）：
//   | 偏移 | 宽 | 字段 |
//   |    0 |  4 | 魔数 `DSM1`（正文 ASCII，**不含** 0x5A(SYNC) 那种会与分帧撞的字节） |
//   |    4 |  1 | 信封版本（与协议 VER **分开**：它只是"载荷怎么摆"的版本） |
//   |    5 |  2 | 序号，**大端**；bit15 = "这一发是一次新 burst 的第一帧" |
//   |    7 |  2 | 发端单调毫秒的**低 16 位**，大端（只用于"发端有没有按节奏发"这条自校验） |
//   ⇒ 信封 **9 B**，帧长 = 7 + 9 = **16 B**。
//   ★ 与契约那三种帧的关系（**别把它读成"改了协议"**）：契约里 `kDataLen = 6 B`
//     （帧长 13 B）**一个字都没改** —— 测量帧只是把**同一个 `TYPE = 0x20`** 的载荷
//     填到 9 B（多出来的是冗余字节，收端按信封自己的长度取用）。
//     为什么是 9 而不是 6：要给"序号 + 发端时刻"留够宽度（4+1+2+2 = 9），
//     而 9 仍然落在 §2 的 LEN 合法范围（4..16）之内。
//   ★ 为什么是"低 16 位"而不是低 8 位：8 位在 256 ms 就绕一圈，而**卡顿正是我们要量的
//     东西**（一次 300 ms 的停顿会算成 44）⇒ 16 位（65.5 s 绕一圈）对"一次 burst ≤ 20 s"够用。
static const uint8_t kMeasMagic[4] = {'D', 'S', 'M', '1'};
static const uint8_t kMeasVer         = 1u;
static const uint8_t kMeasEnvelopeLen = 9u;   // 4+1+2+2；≤ 契约 LEN 上限 16，也 ≤ 9 B 预算
static const uint8_t kMeasSeqBytes    = 2u;
static const uint8_t kMeasOffVer      = 4u;
static const uint8_t kMeasOffSeq      = 5u;
static const uint8_t kMeasOffMs       = 7u;    // 2 B，大端，低 16 位

// 序号的宽度与"第一帧"标志位（见上面那张表）。
static const uint16_t kMeasSeqMask   = 0x7FFFu;
static const uint16_t kMeasFirstBit  = 0x8000u;

// ★ 编译期把"信封塞得进 v1 帧"这条钉死（§2 的 LEN 合法范围 4..16）。
static_assert(kMeasEnvelopeLen >= kLenMin && kMeasEnvelopeLen <= kLenMax,
              "测量信封必须落在 §2 的 LEN 合法范围(4..16)里");
static_assert(kMeasEnvelopeLen <= 9u,
              "测量信封必须**不长于**契约 DATA 的载荷(9 B)：长了就等于'把帧加长再测速'，"
              "那时延不能代表真实 DATA 帧");
static_assert(kMeasOffMs + 2u == kMeasEnvelopeLen, "信封偏移表与长度必须对齐");

// ---- 判据门槛（数值的唯一出处） ----
struct MeasThresholds {
  uint32_t loss_max_per_100k = 100u;   // 丢包率上限：100/100000 = 0.1 %
  uint32_t gap_max_ms        = 100u;   // 连续最大间隔上限（§4 第一档）
  uint32_t p99_jitter_max_ms = 20u;    // p99 抖动上限（一个 TICK 周期）
  uint32_t min_rx_frames     = 300u;   // 低于它就只报数、不下结论
};
static const MeasThresholds kMeasThresholds{};

// ---- 纯判据（宿主机用例逐条钉住；没有浮点、没有"差不多"） ----
inline bool measPassLoss(uint32_t rx, uint32_t expected, const MeasThresholds& t = kMeasThresholds) {
  if (expected == 0u) return false;
  const uint32_t lost = (expected > rx) ? (expected - rx) : 0u;
  // 交叉相乘避免浮点：lost/expected < loss_max/100000
  return (uint64_t)lost * 100000ull < (uint64_t)t.loss_max_per_100k * (uint64_t)expected;
}
inline bool measPassGap(uint32_t max_gap_ms, const MeasThresholds& t = kMeasThresholds) {
  return max_gap_ms < t.gap_max_ms;
}
inline bool measPassJitter(uint32_t p99_ms, const MeasThresholds& t = kMeasThresholds) {
  return p99_ms < t.p99_jitter_max_ms;
}

// ---- 发端（两侧都能用：v1 契约是双向的，见 §3 的 HELLO/STATUS） ----
class MeasSender {
 public:
  // 一次 burst 默认发多少帧。★ 它是**有限**的：实测时不需要人守着看表，
  // 也不会一直占着射频；发完自动停（`active()` 变 false）。
  static const uint32_t kDefaultCount = 2000u;
  // 一拍最多发几个包（单次调用有上界 ⇒ 可随时被打断，§1.3）。
  static const uint8_t kMaxPerPoll = 4u;

  void reset();

  // 开始一次 burst（会**清零**计数并给第一帧打上 kMeasFirstBit）。
  // `count == 0` ⇒ 用 kDefaultCount；`period_ms == 0` ⇒ 用 kDefaultPeriodMs。
  static const uint16_t kDefaultPeriodMs = 10u;   // 100 Hz
  void start(uint32_t now_ms, uint8_t role, uint32_t count = kDefaultCount,
             uint16_t period_ms = kDefaultPeriodMs);
  void stop() { mActive = false; }

  // 主循环里调。返回本次真的交给 PHY 的**帧数**（0 = 没到点 / 没在跑 / 已发完）。
  // ★ 非阻塞：只调 `phy.write()` —— 在 ESP-NOW 那一档里它一个字节都不碰射频
  //   （只拷进自有环），真的交给驱动是 `pumpTx()`。
  uint8_t poll(uint32_t now_ms, LinkPhy& phy);

  bool     active()   const { return mActive; }
  uint16_t periodMs() const { return mPeriodMs; }
  uint32_t sent()     const { return mSent; }
  uint32_t planned()  const { return mPlanned; }
  uint16_t seqLast()  const { return (uint16_t)(mSeq ? mSeq - 1u : 0u); }
  uint32_t t0()       const { return mT0; }      // burst 起点（发端单调毫秒）

 private:
  bool     mActive   = false;
  uint8_t  mRole     = kRoleMaster;
  uint32_t mSent     = 0;
  uint32_t mPlanned  = 0;
  uint32_t mNextMs   = 0;
  uint32_t mT0       = 0;
  uint16_t mPeriodMs = kDefaultPeriodMs;
  uint16_t mSeq      = 0;
};

// ---- 收端 ----
// 间隔直方图：1 ms 一格、0..94 ms，最后一格是"≥95 ms"的溢出格。
// ★ 为什么 1 ms 一格：门槛里最小的数是 20 ms（一个 TICK 周期），1 ms 对它是 5 % 的
//   量化误差，够用。★ 为什么只到 95 ms：正好压在"连续最大间隔 < 100 ms"这条门槛
//   下面 —— 超过 95 ms 的样本落进溢出格，**同时**一定会把 `gap_max_ms` 顶上去，
//   所以任何"接近超时"的形态都不会被直方图悄悄吞掉。
static const uint16_t kMeasGapBins       = 96u;
static const uint16_t kMeasGapBinMs      = 1u;
static const uint16_t kMeasGapOverflowIx = (uint16_t)(kMeasGapBins - 1u);   // 最后一格 = ≥95 ms
inline uint16_t measGapBinOf(uint32_t gap_ms) {
  if (gap_ms >= (uint32_t)(kMeasGapOverflowIx) * kMeasGapBinMs) return kMeasGapOverflowIx;
  return (uint16_t)(gap_ms / kMeasGapBinMs);
}

// 到达记录队列（接收回调写、主循环读）。深度按"主循环最坏 200 ms 一圈"这条
// 实测节拍算：100 Hz 的 burst × 0.2 s = 20 条 ⇒ 64 深有 3 倍余量。
// ★ 为什么要留这么宽：主循环在设备上实测 ~900 圈/秒（`loop: n=… (/s)`），
//   所以正常情况下一圈只到 0~1 帧；这条余量是为"某一圈被渲染/刷屏拖住"准备的。
//   ★ 再满就丢**到达记录**并计数（`queue_drop`）—— 宁可少一条统计样本，
//   也绝不在 WiFi 任务里等（那是"日志同步写"栽过的同一类事故）。
static const uint8_t kMeasRxQueue = 64u;
// 一次 `poll()` 最多取几条（有上界）。
static const uint8_t kMeasRxPollMax = 8u;
// 主循环没跟上时，同一次 `poll()` 取到多条 ⇒ 它们的时间戳相同 ⇒ 间隔算成 0。
// 这不是链路的问题，所以**单列一个计数**（`coalesced`），不要拿它当抖动。
static const uint8_t kMeasCoalesceSlack = 1u;

struct MeasRxResult {
  uint32_t frames_rx     = 0;   // 收到的**测量信封**帧数
  uint32_t seq_min       = 0;   // 见到的最小序号
  uint32_t seq_max       = 0;   // 见到的最大序号
  uint32_t expected      = 0;   // seq_max - seq_min + 1（"这一段本该到多少帧"）
  uint32_t lost          = 0;   // expected - frames_rx（含中途空洞）
  uint32_t loss_per_100k = 0;   // 丢包率 ×100000（整数：好打印、好判据、无浮点）
  uint32_t gap_max_ms    = 0;   // ★ 收端：相邻两帧**到达时刻**的最大间隔
  uint32_t gap_samples   = 0;   // 参与统计的间隔样本数（= frames_rx - 1）
  uint32_t p50_ms        = 0;   // 中位数（直方图）
  uint32_t p95_ms        = 0;
  uint32_t p99_ms        = 0;
  uint32_t over95        = 0;   // ≥95 ms 的样本数（溢出格）
  // ★ 发端另一路（信封里带的那 8 位毫秒，按 256 取模展开）：
  uint32_t wire_gap_max_ms = 0; // 发端相邻两帧的**发出**间隔最大值
  uint32_t span_arrival_ms = 0; // 收端到达时刻的首尾差（≈ 这一次 burst 有多长）
  uint32_t coalesced     = 0;   // 主循环没跟上、一次 poll 取走多帧的次数
  uint32_t queue_drop    = 0;   // 到达记录因队列满而丢（**统计本身**的损失）

  // 判据（见 kMeasThresholds）。样本不够时三个都 false，由 verdict() 说明原因。
  bool passLoss()   const { return enough() && measPassLoss(frames_rx, expected); }
  bool passGap()    const { return enough() && measPassGap(gap_max_ms); }
  bool passJitter() const { return enough() && measPassJitter(p99_ms); }
  bool enough()     const { return frames_rx >= kMeasThresholds.min_rx_frames; }
  const char* verdict() const;   // "适合" / "不适合" / "样本不足"
};

class MeasReceiver {
 public:
  void reset();

  // ★★ 只在**接收回调**里调（WiFi 任务上下文）：检查信封 + 打时间戳 + 进小环。
  //    这里**不格式化、不打日志、不算直方图**（那三件事都会阻塞 WiFi 任务）。
  //    返回 true = 这是一帧测量帧（调用方据此**不要**再把它喂给常规链路）。
  bool noteArrival(const uint8_t* payload, uint8_t payload_len, uint32_t now_ms);

  // 主循环里调：把队列里的到达记录取出来算间隔/丢包/直方图。
  // 返回本次处理了几条（有上界 kMeasRxPollMax）。
  uint8_t poll();

  // burst 结束的判据：在跑、且 `idle_ms` 内没再来过帧。
  // 返回 true = **这一次调用**刚刚判定结束（调用方据此打汇总行，只打一次）。
  bool endedBy(uint32_t now_ms, uint32_t idle_ms = 250u);
  bool inBurst() const { return mActive; }
  uint32_t lastMs() const { return mLastMs; }
  uint32_t firstMs() const { return mFirstMs; }
  const MeasRxResult& result() const { return mRes; }

 private:
  void drainOne(uint16_t seq, uint32_t ms_now, uint16_t ms_sent16, bool first_of_burst);
  uint32_t percentile(uint32_t num, uint32_t den) const;   // 直方图分位

  // ---- 回调侧（写）。★ volatile 不是同步原语（见 link_phy_uart.cpp 里同一形态的
  //      说明）：单生产者 + 单消费者、只在 WiFi 任务与主循环各跑一次，够了。
  volatile uint8_t  mQHead = 0;
  volatile uint8_t  mQTail = 0;
  volatile uint16_t mQSeq[kMeasRxQueue] = {0};
  volatile uint32_t mQMs[kMeasRxQueue]  = {0};
  volatile uint16_t mQSentMs[kMeasRxQueue] = {0};   // 信封里那 16 位发端毫秒

  // ---- 主循环侧 ----
  uint32_t mBins[kMeasGapBins] = {0};
  MeasRxResult mRes;
  bool     mActive   = false;
  bool     mHavePrev = false;
  bool     mEnded    = false;
  uint16_t mLastSeq  = 0;
  uint32_t mPrevMs   = 0;
  uint32_t mFirstMs  = 0;
  uint32_t mLastMs   = 0;
  uint32_t mCoalesced = 0;
  uint32_t mQueueDrop = 0;
  // 发端那 16 位毫秒的"按 65536 取模展开"：把 16 位小量还原成增量
  uint16_t mPrevSentMs = 0;
  bool     mHaveSentMs = false;
  uint16_t mWireGapMax = 0;
};

// ---- 汇总行（ASCII；**只格式化、不打印** —— 打印由调用方的 dash_logf 做，
//      这样"日志走哪条路"仍然只有 `lib/dashcore/dash_log.h` 一处口径） ----
// 返回写进 out 的字符数（不含结尾 NUL）；`out` 至少 320 B。
int measFormatRxSummary(const MeasRxResult& r, char* out, int cap);
int measFormatTxSummary(const MeasSender& s, uint32_t now_ms, char* out, int cap);

}  // namespace dashlink
