#pragma once
#include <stdint.h>

#include "link_frame.h"

// ============================================================
// 双板链路协议 v1 —— **消息载荷**（§3 的消息表）
//
// 逐字节按契约表打包/解包：载荷布局、字段宽度、量纲全部只在这里定义一次。
// 本层**不碰** UART、不碰时基（link_time.h）、不碰收发缓冲（link_tx/rx.h）。
//
// ★ 两处契约没写、由本文件定下来的口径（回报里已列出，两板固件必须跟着改）：
//   ① **多字节字段一律大端**。§3 的表里只有 DATA.rpm_raw 明确写了 "u16 BE"，
//      其余 u16/u32（fw_ver / build_tag / tick_ms / uptime_ms / 各计数器 /
//      EVENT.value）没标字节序。协议里混用两种字节序没有任何理由，而仓库
//      全线（VAN 的 IDEN/CMD/FCS、帧头、CRC 字段）都是 MSB-first 大端
//      ⇒ 一律大端，与那一条显式的 "BE" 一致。
//   ② **DATA.flags 的 2 位分配**。契约只写"每字段 2 位 = FieldSource"，
//      没写哪两位归哪个字段。这里按表里字段**出现的顺序**从高位往低位排
//      （与仓库 MSB-first 口径一致）：
//          bit7..6 = rpm、bit5..4 = speed、bit3..2 = coolant、bit1..0 = intake
//      下面 dataFlagsPack()/dataFlagsGet() 是唯一读写入口，别手写移位。
//   ③ STATUS.flags 的**位号**契约也只列了含义（ver_mismatch / role_conflict /
//      无 DATA 超时 / 温度弧无源…），没给位号 ⇒ 本文件定 kStFlag* 四个位。
//
// ★ 与 data_service.h 的 FieldSource 的关系：2 位编码 0/1/2/3 就是
//   None/Sim/Obd/Van —— 数值**刻意与 FieldSource 对齐**（用例里有一条静态
//   对账）。但本层**不 include** data_service.h：那个头拉 Arduino.h，
//   而 lib/link 是纯逻辑层（宿主机、将来 pcpreview 都要能编）。
//
// ★ §3 记的那个已知缺口：FieldSource **没有"主机/链路"这一档** ——
//   从板把收到的 DATA 喂进自己的 data_service 时得先加一档（或绕过它直接写
//   视图）。本轮不碰 data_service（口径见任务：只许新增文件）。
// ============================================================

namespace link {

// ---- 载荷长度（§3 逐字节表的宽度之和；用例与"帧长 = 7 + LEN"对账） ----
static const uint8_t kHelloLen  = 5u;    // fw_ver u16 + build_tag u16 + boot_reason u8
static const uint8_t kTickLen   = 5u;    // tick_ms u32 + seq u8
static const uint8_t kDataLen   = 6u;    // rpm_raw u16 + speed_raw + coolant_raw + intake_raw + flags
static const uint8_t kStatusLen = 16u;   // 见下
static const uint8_t kEventLen  = 4u;    // evt_id u8 + value u16 + face u8

// TYPE → 该消息在 v1 的载荷长度（未知 TYPE 返回 0）。
// ★ 它**只**用来做长度自检/日志，不是 decodeFrame 的判据：§2 的次版本规矩
//   允许已知 TYPE "只加尾巴"，所以载荷比这个长是合法的（unpackX 认识前半截）。
uint8_t payloadLenForType(uint8_t type);

// ---- 0x01 HELLO（双向，上电 1 次、之后每 5 s 重发直到收到对端 HELLO） ----
// 幂等、可重复、无超时概念。
struct HelloMsg {
  uint16_t fw_ver     = 0;   // 固件版本，**与协议 VER 分开**（§3 原话）
  uint16_t build_tag  = 0;   // 构建标记（两板"是不是同一份固件"靠它 + fw_ver）
  uint8_t  boot_reason = 0;  // 上电原因：取值由发送侧定（本仓库暂无既有编码）
};

// ---- 0x10 TICK（A→B，50 Hz / 20 ms；车睡着、VAN 没帧时也照发） ----
struct TickMsg {
  uint32_t tick_ms = 0;   // 主板**单调**毫秒（从复位起算，32 位 ⇒ 49.7 天回绕）
  uint8_t  seq     = 0;   // 回绕/丢帧可见（丢帧由它跳变看得出来）
};

// ---- 0x20 DATA（A→B，跟随 VAN 0x824 到达 ≈79.7 Hz，不另建定时器） ----
// 从板上**没有**任何本地源 ⇒ "这个值是真值还是假数据"只能跟着数据一起过来，
// 这就是 flags 存在的理由（对应 DataSourceStatus 的字段级优先级）。
struct DataMsg {
  uint16_t rpm_raw     = 0;   // = 转速 × 8（VAN 线上原值，与 kRpmScale=0.125 配套）
  uint8_t  speed_raw   = 0;   // 原始计数：1 计数 = 2.56 km/h（2026-09-22 实测定标）
  uint8_t  coolant_raw = 0;   // = ℃ + 40
  uint8_t  intake_raw  = 0;   // = ℃ + 40
  uint8_t  flags       = 0;   // 每字段 2 位来源，见 dataFlagsPack()
};

// ---- 0x30 STATUS（B→A，2 Hz / 500 ms） ----
// "单一日志出口"的基础 + 链路质量；uptime_ms 用来发现"从板在反复重启"（§7 #7）。
struct StatusMsg {
  uint16_t fw_ver        = 0;
  uint32_t uptime_ms     = 0;
  uint16_t frames_ok     = 0;   // 收下的帧
  uint16_t frames_dropped = 0;  // 丢掉的帧（§2 的三种：crc / bad_len / unknown_type）
  uint16_t crc_err       = 0;
  uint16_t last_gap_ms   = 0;   // 最近一次帧间隔（详见回报里的契约歧义）
  uint8_t  left_face     = 0;   // 左屏当前档位 = expression.h 的 Face 槽位下标
  uint8_t  flags         = 0;   // kStFlag*
};

// STATUS.flags 位（★ 位号由本文件定，见文件头 ③）
static const uint8_t kStFlagVerMismatch  = 1u << 0;   // §2：主版本不匹配（告警过）
static const uint8_t kStFlagRoleConflict = 1u << 1;   // §5：收到过同角色的帧
static const uint8_t kStFlagNoData       = 1u << 2;   // 无 DATA 超时（§3 DATA 行）
static const uint8_t kStFlagTempNoSource = 1u << 3;   // 温度弧无源（§3 原话）

// ---- 0x40 EVENT（双向，v1 实际只用 B→A；事件发生即发、限速 ≥100 ms 防抖） ----
// ★ **不重传**：丢一条 EVENT 不会漏掉持续状态 —— 当前档位每 500 ms 由
//   STATUS.left_face 兜底（刻意设计：用状态帧兜事件，省掉重传逻辑）。
struct EventMsg {
  uint8_t  evt_id = 0;   // kEvt*
  uint16_t value  = 0;   // 事件的附加值（进出、档位号…由各事件自己解释）
  uint8_t  face   = 0;   // 相关档位 = expression.h 的 Face 槽位下标（0..6）
};

// EVENT.evt_id 取值（§3；全部落在仓库已有的概念上）
enum class EvtId : uint8_t {
  RedlineEdge  = 0x01u,   // 红区进入/退出（左屏转速档 Face::Redline：进 5880 / 退 5720）
  OverspeedEdge = 0x02u,  // 超速进入/退出：号段留给"两台表一起提示"的将来，v1 **不过链路**
  FaceChange   = 0x03u,   // 表情档位变化（载荷 face = 新档位）
  SourceChange = 0x04u,   // 数据源变化（Van ↔ Obd ↔ Sim 的 3 秒回退）
  SweepDone    = 0x05u,   // 扫表完成（BOOT_TOTAL_MS 走完）
  ModeChange   = 0x06u,   // ★ **v1 不做**：号段保留、不许挪作他用（§3 / §8 L9）
};

// evt_id 是不是 v1 定义过的（0x01..0x05；0x06 保留号段里"v1 不做"，
// 但**收到它不算错** —— 见下面 unpackEvent 的说明）。
inline bool evtIdKnown(uint8_t id) {
  return id >= (uint8_t)EvtId::RedlineEdge && id <= (uint8_t)EvtId::SweepDone;
}

// ---- 字段来源的 2 位编码（§3 的 flags 列） ----
// 数值与 data_service.h 的 FieldSource 一一对应：0 None / 1 Sim / 2 Obd / 3 Van。
enum class Src : uint8_t { None = 0u, Sim = 1u, Obd = 2u, Van = 3u };

// DATA.flags 的字段下标（顺序与 §3 表里字段出现的顺序一致）
static const uint8_t kFieldRpm     = 0u;
static const uint8_t kFieldSpeed   = 1u;
static const uint8_t kFieldCoolant = 2u;
static const uint8_t kFieldIntake  = 3u;

// 打包/取值（唯一入口，别手写移位）：
//   flags = rpm<<6 | speed<<4 | coolant<<2 | intake
uint8_t dataFlagsPack(Src rpm, Src speed, Src coolant, Src intake);
Src     dataFlagsGet(uint8_t flags, uint8_t field);   // field = kField*(0..3)

// ---- 量纲换算（含钳制：越界值不许回绕、不许变成负数） ----
//   rpm   : raw = rpm × 8            （kRpmScale = 0.125）
//   speed : raw = km/h ÷ 2.56        （kSpeedScale = 2.56，1 计数 = 2.56 km/h）
//   温度  : raw = ℃ + 40             （0 → -40℃，255 → 215℃）
// 浮点只在这一层出现（与 data_service / van_source 的量纲层一致）；NaN / 负值
// 一律钳到 0，超上限钳到字段上限。
static const float kRpmCountsPerRpm   = 8.0f;
static const float kRpmScale          = 0.125f;   // 与 VanSource::kRpmScale 同值
static const float kSpeedKmhPerCount  = 2.56f;    // 与 VanSource::kSpeedScale 同值
static const float kTempOffsetC       = 40.0f;

uint16_t rpmToRaw(float rpm);         // 0..65535
float    rawToRpm(uint16_t raw);
uint8_t  speedToRaw(float kmh);       // 0..255
float    rawToSpeedKmh(uint8_t raw);
uint8_t  tempToRaw(float celsius);    // 0..255
float    rawToTempC(uint8_t raw);

// ---- 打包 / 解包 ----
// pack：写 kXLen 个字节，返回 false = out 为空。
// unpack：len **≥** kXLen 时解出前半截并返回 true（§2 的次版本规矩：
//   "已知 TYPE 的载荷只许加尾巴、不许改前缀 ⇒ 新固件发的东西老固件也认得前半截"）；
//   len 比 kXLen 短 ⇒ false（前缀都不全，解不了）。
//   注意与 decodeFrame 的分工：帧层的 LEN 判据是 v1 的 5..16，消息层只管
//   "这 kXLen 个字节够不够解出这条消息"。
bool packHello(const HelloMsg& m, uint8_t* out);
bool unpackHello(const uint8_t* p, uint8_t len, HelloMsg* out);
bool packTick(const TickMsg& m, uint8_t* out);
bool unpackTick(const uint8_t* p, uint8_t len, TickMsg* out);
bool packData(const DataMsg& m, uint8_t* out);
bool unpackData(const uint8_t* p, uint8_t len, DataMsg* out);
bool packStatus(const StatusMsg& m, uint8_t* out);
bool unpackStatus(const uint8_t* p, uint8_t len, StatusMsg* out);
bool packEvent(const EventMsg& m, uint8_t* out);
// ★ 事件号未知也照样解出来（不丢）：§2 的"未知丢帧"规矩是给 **TYPE** 的，
//   把未知 evt_id 丢掉会破坏"次版本只加东西"的向前兼容。要不要记日志由调用方判
//   （evtIdKnown()）。EVENT 的载荷长度 v1 固定 4 B，认不出的事件也占这 4 B。
bool unpackEvent(const uint8_t* p, uint8_t len, EventMsg* out);

}  // namespace link
