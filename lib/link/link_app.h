#pragma once
#include <stdint.h>

#include "data_service.h"   // LinkData / FieldSource / VehicleState（-I lib/dashcore）
#include "link_frame.h"
#include "link_msg.h"
#include "link_phy.h"
#include "link_role.h"
#include "link_rx.h"
#include "link_time.h"
#include "link_tx.h"

// ============================================================
// 双板链路协议 v1 —— **应用层接线**（§1.2 ③ / §3 / §4 / §5）
//
// 这一层只做三件"协议 ↔ 本仓库"之间的事，别的都不做：
//   ① `Src`（0..3，§3 的 2 位编码）↔ `FieldSource`（data_service 的五档）互转 ——
//      ★ 就是 §3 表下那条自记缺口要的那一档（`FieldSource::Link`）；
//   ② **主板侧**：把"主循环里的快照"打包成 `DATA`（§1.2 ③：从快照发、不从回调发），
//      并按"跟随 VAN 0x824 到达"的节奏限速（§3 的 DATA 行）；
//   ③ **从板侧**：把收到的 `DATA` 解成 `LinkData`，交给 `VehicleDataService`。
//
// ★ 本层**不碰** UART、不碰 LVGL、不碰 millis()（时间由调用方传进来）——
//   于是它在宿主机上可测（`test_link_app.cpp` 把"发 → 收 → 喂进 data_service"
//   整条链串起来跑一遍，用的是 test/test_dashcore/fake_link_phy.h）。
//
// ★ 三条"别在这里做"的事（做了就违反 §1.2）：
//   · 别在 `LinkRx` 解出帧的当场顺手发帧 —— 收帧与发帧都只在主循环里；
//   · 别自己起定时器发 `DATA`：§3 明写"跟随 VAN 0x824 到达，**不另建定时器**"；
//   · 别把 `DATA` 的内容**缓存起来等以后发**：那会让"从快照发"退化成"从旧值发"。
//
// ★★ 2026-09-27 补的契约缺口（**从板也会发**）：§3 的 `HELLO` 是**双向**的，
//   而 §3 的 `STATUS`(`0x30`) 方向就是 **B → A** —— 但从板那一侧**一行发送代码都没有**
//   （`main.cpp` 里从板的 `LinkTx` 环恒空、`pump()` 是空转）⇒ v1 的"双向"只兑现了
//   A→B 那一半，主板那 30 s 的"从板无响应"判据永远为真。本文件补上从板侧那两件：
//     ① `helloDue()` —— HELLO 的重发口径（**与主板逐字同一条**：上电 1 次、
//        之后每 5 s，直到收到对端 HELLO）；
//     ② `StatusSender` —— STATUS 的 2 Hz 节奏（§3 表 `0x30` 行的 500 ms）。
//   ★ 本节**不动协议**：消息类型、LEN、字段次序、CRC 覆盖范围、`LINK_ROLE` 的编译期
//     权威地位（§5）一个字都没改。新增的只有"谁在什么时刻 enqueue"。
// ============================================================

namespace dashlink {

// ------------------------------------------------------------
// ① 来源编码互转（§3 的 2 位 flags ↔ data_service 的 FieldSource）
// ------------------------------------------------------------
// 前四档数值刻意一一对应（link_msg.h 有静态对账用例）。Link（= 4）**编不进 2 位**
// ⇒ 它只出现在"本机作为从板"这一类场景里；§3 的 DATA.flags 是从板**收到**的东西，
// 与 Link 无关。
Src          fieldSourceToSrc(FieldSource f);
FieldSource  srcToFieldSource(Src s);

// 一帧 DATA 载荷 → LinkData（含量纲换算与每字段来源；rx_ms 由调用方给）。
LinkData unpackDataToLinkData(const DataMsg& m, uint32_t rx_ms);

// 打包：把"当前快照 + 每字段来源"做成 DATA 载荷。
//   · 四个标量按 §3/§link_msg.h 的量纲换算（rpm×8、车速÷2.56、温度+40，含钳制）；
//   · 来源位由 dataFlagsPack() 给（**别手写移位**，§3 的位号）；
//   · ★ Link 这一档**编不进** flags（只有 2 位）⇒ 按 Sim 编（见 .cpp 的说明）。
DataMsg packLinkData(const VehicleState& st, const DataSourceStatus& src);

// ------------------------------------------------------------
// ② HELLO 的重发口径（§3 表 `0x01` 行）—— **两个角色共用同一条**
// ------------------------------------------------------------
// 契约原文：`HELLO` **双向**，上电 1 次，之后每 **5 s** 重发，**直到收到对端 HELLO**；
// 幂等、可重复、**无超时概念**。
//
// ★ 为什么把它抽成一个函数（而不是在 main.cpp 的两支里各写一遍 if）：
//   主板那一支原来自己写了一遍（`g_link_hello_ms == 0 || now - ms >= 5000`），
//   而 2026-09-27 补从板发送时**必须**与它逐字同口径 —— 抄一遍就一定会漂；
//   抽出来之后"5 s"只在这里出现一次，两个角色共用，用例也只钉一处。
//
// 用法（调用方自己持有 `last_ms` 与"收到过对端 HELLO 没有"这两个状态）：
//     uint32_t last = 0; bool acked = false;
//     if (helloDue(now, last, acked)) { last = now; ...enqueueFrame(Hello...); }
//   ★ 先判、后推时刻：`last_ms` 由调用方在**真的要发**那一拍写。
inline bool helloDue(uint32_t now_ms, uint32_t& last_ms, bool acked) {
  if (acked) return false;                                   // 收到对端 HELLO ⇒ 停发（§3）
  if (last_ms == 0u) return true;                            // 上电第一次
  return (int32_t)(now_ms - last_ms) >= (int32_t)kHelloRepeatMs;
}

// ------------------------------------------------------------
// ② 从板侧：STATUS 的发送节奏（§3 的 `0x30` 行，2 Hz / 500 ms）
// ------------------------------------------------------------
// 契约原文：`STATUS`(`0x30`) 方向 **B → A**，频率 **2 Hz（500 ms）**，载荷 16 B。
// 形状与下面的 `DataSender` **刻意同形**（`due(now, out)` ⇒ 调用方直接
// `enqueueFrame(Status, …)`）：本层只算"该不该发、发什么"，**不碰 PHY、不碰缓冲**。
//
// ★ 为什么用"到点了"而不是别的东西：与 `DATA` 不同，`STATUS` **没有**任何外部事件
//   可以跟随（契约给的就是一条定时节奏），所以它是本仓库里唯一一个**自带周期**的
//   发送者。周期仍然由调用方的主循环驱动（不在这里起定时器 —— 本层不碰 millis()）。
class StatusSender {
 public:
  void reset();

  // 到点了吗？true ⇒ 已把 out 填好（调用方直接 enqueueFrame(Status, …) 即可）。
  //   now_ms = 主循环当前毫秒（调用方给的，本层不读 millis()）
  bool due(uint32_t now_ms, StatusMsg* out);

  uint32_t sent() const { return mSent; }
  uint32_t lastSentMs() const { return mLastSentMs; }

 private:
  // ★ 与 DataSender 同一条理由：不用"0"当"还没发过"的哨兵（now_ms 完全可能是 0）。
  bool     mHasSent    = false;
  uint32_t mLastSentMs = 0;
  uint32_t mSent       = 0;
};

// ------------------------------------------------------------
// ② 从板侧：STATUS.flags 这四位怎么来（§3 表 `0x30` 行的位号）
// ------------------------------------------------------------
// ★ 这里只填**从板确实有生产者**的那三位，另外一位**老老实实留 0**：
//   · `bit0 ver_mismatch`  ← `LinkRx::verMismatchSeen()`（§2：主版本不匹配告警过）
//   · `bit1 role_conflict` ← `LinkRx::roleConflictSeen()`（§5 ①：收到过同角色的帧）
//   · `bit2 无 DATA 超时`   ← `LinkTime::dataState()`：**§3 的 DATA 行**把 DATA 的
//     超时策略与 TICK 归成一档（>100 ms 失基准 / >500 ms 降级 / >3 s 回退 Sim）⇒
//     "无 DATA 超时" = `dataState()` 已经**不是 Locked**（含"开机以来一帧都没见过"，
//     那时 `LinkTime` 自己就报 SimFallback）。★ 没有新闸门、没有新阈值 ——
//     用的就是 §3/§4 既有那三档。
//   · ★ `bit3 温度弧无源`：**v1 从板这一侧没有生产者，恒 0**。理由（别按字段名猜）：
//     从板的水温/进气**只能**来自主板那条 `DATA`（它自己没有 VAN、也没有本地 OBD），
//     而 §3 的 `DATA.flags` 每个字段只有 2 位（0 None/1 Sim/2 Obd/3 Van）——
//     **"链路"这一档编不进去**（`link_app.cpp` 的 `fieldSourceToSrc()` 把 Link 编成 Sim）。
//     ⇒ "温度弧有没有源"这件事从板答不出来，而"敢不敢报一个自己也不确定的值"的
//     答案是不报（§8 的口径：没生产者就不填，见 `STATUS.last_gap_ms` 那条定案）。
uint8_t slaveStatusFlags(bool ver_mismatch, bool role_conflict, LinkTimeState data_state);

// ------------------------------------------------------------
// ② **发送预算**：把这些消息都按契约频率跑，一秒吃掉多少线时（毫秒）
// ------------------------------------------------------------
// ★ 为什么它是一段**代码**而不是注释里的一句话：本单让**从板也开口说话**了
//   （HELLO + STATUS），而契约 §1.1 那张占空比表是按"两边都说话"算的 —— 也就是说
//   "从板那 2 Hz 到底占多少"这件事必须**算得出来、且能被用例钉住**，
//   否则将来谁把 STATUS 提到 10 Hz、或者把 DATA 的上限调大，都没有任何信号。
//
// 口径（与 §1.1 那张表逐列对齐）：
//   · 一字节 8N1 = **10 bit**；线速率 = kLinkBaud bit/s
//   · 一帧的线时 = `frameBytesForLen(len) × 10 / kLinkBaud`（秒）⇒ 换成毫秒
//   · 一秒里的线时 = 各消息的**帧率**乘各自的帧线时，然后相加
// ★ 结果与 §1.1 原话对账（`test_link_slave_tx.cpp` 里逐条钉着）：
//     主板 A→B：`DATA`(79.7 Hz, 13 B) + `TICK`(50 Hz, 12 B) ≈ **142 ms/s ≈ 14%**
//     从板 B→A：`STATUS`(2 Hz, 23 B) + `HELLO`(≤0.2 Hz, 12 B) ≈ **4.2 ms/s ≈ 0.42%**
//   ⇒ 合计 ≈ **14.6%**，与契约"≈15%（A→B ≈14%，B→A ≈0.4%）"逐项吻合。
// ★ `payload_len` 是**载荷**长度（不是整帧）：整帧 = 7 + 载荷（§2 的 `frameBytesForLen`）。
//   本函数内部自己套那 7 个字节的开销 ⇒ 调用方传 `kStatusLen`（= 16）而**不是** 23，
//   传 23 会算成 30 字节的帧（本仓库的用例第一次就是这么写错的，见 `test_link_slave_tx.cpp`）。
uint32_t lineMsPerSecondForFrame(uint8_t payload_len, uint32_t frames_per_second);

// 本仓库那两个发送方**按契约频率全速跑**时的合计线时（毫秒/秒）。
//   master_data_hz / master_tick_hz —— 主板那一侧（§3：DATA ≈79.7、TICK 50）
//   slave_status_hz  / slave_hello_hz —— 从板那一侧（§3：STATUS 2、HELLO ≤0.2）
//   master_vanraw_hz —— ★ 2026-09-27 新增的 `0x21 VANRAW`（跟随母线上每一帧 VAN，
//     所以取与 DATA 同一个量级：总线上 ≈80 Hz 的那一帧就是车速帧）
// ★ 默认参数**就是契约值**，调用方（`main.cpp` 的注释、用例、将来的诊断页）
//   不必各抄一遍数字；要算"提高频率会怎样"时显式传别的值。
// ★★ 加 VANRAW 之后**契约 §1.1 那张占空比表就不完整了**（它写的是 A→B ≈14%）：
//   算出来的口径（115200 8N1，整帧 = 7 + 载荷）——
//     DATA   80 Hz × 13 B =  90 ms/s
//     VANRAW 80 Hz × 18 B = 125 ms/s   ← 本单新加
//     TICK   50 Hz × 12 B =  52 ms/s
//     STATUS  2 Hz × 23 B =   4 ms/s
//   ⇒ A→B **267 ms/s**（原 142）、两侧合计 **271 ms/s ≈ 27%**。
//   ★ 无线档（ESP-NOW）不受这条约束，实测 0 丢包（见 ACCEPTANCE 的 2026-09-27 条）；
//     这条算术管的是**有线那一档**（UART0 @115200，已退役但仍能编）。
uint32_t linkBudgetMsPerSecond(uint32_t master_data_hz  = 80u,
                               uint32_t master_tick_hz  = 50u,
                               uint32_t slave_status_hz = 2u,
                               uint32_t slave_hello_hz  = 0u,
                               uint32_t master_vanraw_hz = 80u);

// ------------------------------------------------------------
// ②' VAN 原始帧转发（`0x21 VANRAW`）—— 主板发、从板解
// ------------------------------------------------------------
// 用途：让从板**不用自己再接一套 VAN 收发器**也能拿到"只有 VAN 才有"的字段
//   （灯位 / 门 / VIN，见 van_source.h）；将来往 `VanSource` 里加新字段时，
//   链路层**一个字都不用改** —— 这正是"搬原始帧"相对"再扩几个 DATA 字段"的好处。
//
// ★ 分工（与 `unpackDataToLinkData` 逐字同形）：
//   · 主板侧：`VanRawMsg` 从 `VanPacket` 来（`vanRawFromPacket()`），
//     打包用 `packVanRaw()`（link_msg.h），**本层不碰字节序**；
//   · 从板侧：`unpackVanRaw()` 解出 `VanRawMsg`，再用 `unpackVanRawToVanPacket()`
//     还原成 `VanPacket`，喂给**从板自己的** `VanSource`。
// ★ 为什么还原成 `VanPacket`（而不是让 VanSource 认识 VanRawMsg）：
//   `VanSource` 是数据层（lib/dashcore），它只认 `VanPacket`；
//   让数据层 include 协议头会倒过来依赖 —— 与 `LinkData` 那条同一理由
//   （见 data_service.h 的 `LinkData` 注释）。
// ★ `rx_ms` 由调用方给：从板要用**自己**的毫秒（它没有主板的 T0），
//   否则 `VanSource` 的 3 秒新鲜度判据在从板上没有意义。
// ★ 注意 `VanPacket` 是**全局作用域**的（在 lib/dashcore/van_source.h 里，
//   由上面 include 的 data_service.h 带进来）—— 这里**不要**写 `struct VanPacket;`
//   当"前向声明"：本文件整段在 `namespace dashlink` 里，那一行会声明出一个
//   **另一个** `dashlink::VanPacket`（不完整类型）⇒ 链接期报
//   "incomplete result type 'VanPacket'"（实测踩到，就这么写的）。
VanPacket unpackVanRawToVanPacket(const VanRawMsg& m, uint32_t rx_ms);
VanRawMsg vanRawFromPacket(const VanPacket& pkt);

// ------------------------------------------------------------
// ②'' 原始帧的**转发队列**（主板侧，`0x21` 的发送侧）
// ------------------------------------------------------------
// 为什么需要一个队列（而不是"收到一帧就顺手发一帧"）：
//   · §1.2 ①：**不在回调里发**。VAN 的收帧回调只往这里拷字节（纯内存），
//     真的编码/入 `LinkTx` 由主循环做 —— 与 DATA"从快照发"同一条纪律；
//   · 一次 `g_van_phy.tick()` 可能一口气交付好几帧（被 LVGL 拖慢过之后尤其如此），
//     而主循环的发送点在后面 ⇒ 中间必须有缓冲，否则要么丢帧、要么在回调里发。
// 形状与 `LinkTx` **刻意同形**（整帧进出、满了丢**整帧**、单生产者+单消费者）：
//   · 生产者 = VAN 收帧那一路（`g_van_phy.tick()` / 串口回放），
//   · 消费者 = 主循环里的 `link_master_tick()`。
//   ★ 两者**都在主循环上下文**（`VanPhyGpio` 的 ISR 只入队边沿，解帧发生在
//     `tick()` 里）⇒ 这里没有中断并发，head/count 两个索引就够。
//     **将来若有谁把 push 挪进 ISR，这个类必须先加临界区** —— 写在这里当路标。
// 容量 512 B 的来历：一帧最多 16 B 载荷 ⇒ 至少能装 32 帧；
//   按 VAN 车速帧 ≈80 Hz、主循环 ≈9k 圈/秒算，正常一圈最多积压 1~2 帧，
//   512 B 给的是"主循环被抢占 ~100 ms"（实测最长 187 ms）时的余量。
class VanRawQueue {
 public:
  static const uint16_t kRingBytes = 512u;

  void reset();

  // 放进一帧（**整帧进出**）。返回 false 的两种情形都**只丢这一帧并计数**：
  //   · 这一帧的数据太长（`kVanRawMaxData` 装不下，例如 VIN 那 17 字节）⇒ tooLong()++
  //   · 环里放不下整帧                                                   ⇒ dropped()++
  bool push(const VanRawMsg& m);

  // 取出一帧：写入 out（容量 cap），返回**写入的载荷字节数**（0 = 环空或 cap 不够）。
  // `cap < kLenMax` 时**不动环**、返回 0（调用方给够就行）。
  uint8_t pop(uint8_t* out, uint8_t cap);

  uint32_t pushed()  const { return mPushed; }
  uint32_t dropped() const { return mDropped; }
  uint32_t tooLong() const { return mTooLong; }
  uint16_t queuedBytes() const { return mCount; }

 private:
  uint8_t  mBuf[kRingBytes] = {0};
  uint16_t mTail  = 0;      // 读位置
  uint16_t mCount = 0;      // 环里现有字节数
  uint32_t mPushed  = 0;
  uint32_t mDropped = 0;
  uint32_t mTooLong = 0;
};

// ------------------------------------------------------------
// ③ 主板侧：DATA 的发送节奏（§3 的 DATA 行）
// ------------------------------------------------------------
// 契约原文：DATA **跟随 VAN 0x824 到达（≈79.7 Hz）**，**不另建定时器**。
// 实现方式：调用方把"这一份快照是什么时候的"（`VanSource::lastUpdateMs()` 就是
// 0x824 最后一次到达的时刻）传进来；本类只在它**变了**的时候发一帧。
//   · 为什么用"变了"而不是"到点了"：0x824 是**广播**进来的（没有定时器可跟）；主循环
//     一圈跑很多次，而 0x824 只有 ≈80 次/秒 ⇒ 这个判据天然把速率压到 VAN 的到达率上。
//   · 一圈里连收了好几帧 0x824（被 LVGL 拖慢过）时**只发一帧**：与 TickGen 的
//     "不补发突发"同一口径（§1.3：单次很短、可随时被打断）。
class DataSender {
 public:
  // 可选下限（0 = 不额外限速，契约口径就是"跟随 0x824"）。
  // ★ §1.1 的占空比表按 ≈79.7 Hz 算下来 A→B 只占 ~14% ⇒ **没有必要**降频。
  //   真要为别的目的降频（比如调试），用这个参数，别去改判断条件。
  void setMinIntervalMs(uint32_t ms) { mMinIntervalMs = ms; }
  void reset();

  // 该发吗？true ⇒ 已把 out 填好（调用方直接 enqueueFrame(Data, …) 即可）。
  //   now_ms      = 主循环当前毫秒
  //   snapshot_ms = **这一份快照的时刻**（0x824 的 lastUpdateMs；没有 VAN 时传 0）
  //   st/src      = 快照本身 + 每字段来源（§3 的 flags 从这里来）
  // ★ 本函数**不碰 PHY、不碰缓冲**：只算"该不该发/发什么"（§1.2 ③ 的形状）。
  bool due(uint32_t now_ms, uint32_t snapshot_ms, const VehicleState& st,
           const DataSourceStatus& src, DataMsg* out);

  uint32_t sent() const { return mSent; }
  uint32_t lastSentMs() const { return mLastSentMs; }

 private:
  // ★ `mHasSent` 是**显式**的"发过没有"，不用"mLastUpdateMs == 0"当哨兵：
  //   快照时刻 0 是**合法**的（`VanSource::last_update_ms` 初值就是 0，
  //   而 millis() 从复位起算、上电后第一帧 VAN 完全可能落在 0~20 ms 内）。
  //   拿 0 当哨兵会让"上电那一帧"被当成"还没发过"，于是限速窗口少判一次 ——
  //   这种错在板上只会表现为"偶尔多发一帧"，几乎不可能靠肉眼发现。
  bool     mHasSent = false;     // 有没有真的发出去过一帧
  uint32_t mLastUpdateMs = 0;    // 上一份**发出去的**快照的时刻
  uint32_t mLastSentMs   = 0;
  uint32_t mMinIntervalMs = 0;
  uint32_t mSent = 0;
};

// ------------------------------------------------------------
// ③ 从板侧：一帧收下来之后做什么（纯路由，不碰 UART）
// ------------------------------------------------------------
// 返回 true = 这一帧被"消化"了：
//   · TICK  → 喂 LinkTime（§4：算偏移、推三级超时）
//   · DATA  → 解成 LinkData 写进 out_data（调用方在 update() 之前 applyLinkData）
//   · 其它（HELLO/STATUS/EVENT）→ 返回 false，交给调用方（日志/角色对账）
// ★ 为什么 TICK/DATA 走这里而 STATUS/EVENT 不走：后者是"往日志里写一行"的事，
//   与数据层无关，留在 main.cpp 更清楚（也避免本层依赖 dash_log）。
bool handleInbound(const Frame& f, LinkTime* t, uint32_t now_ms, LinkData* out_data);

}  // namespace dashlink
