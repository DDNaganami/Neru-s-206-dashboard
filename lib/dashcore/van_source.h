#pragma once
#include <stdint.h>

// VAN 总线数据源:接收原始帧,提取车速与转速。
//
// ★ 车速/转速帧的字段已用**两份实车抓包**定案(2026-09-20;复现工具见
//   ACCEPTANCE.md 文末那条记录与 tools/van-decode/):
//     IDEN 0x824 / CMD 0x8,BSI → Dashboard,7 个数据字节:
//       data[0..1] = 转速 x8(大端,16 位)  例 1C A2 = 7330 → 7330/8 = 916.25 rpm
//       data[2]    = 车速,**单字节**;**1 计数 = 2.56 km/h**(2026-09-22 实测定标,见 .cpp)
//       data[3]    = 未知(与 data[2] 反相相关,疑似同量的低分辨率副本)
//       data[4..5] = 里程/位移累计量(16 位大端,**单调不减**,只在行驶时增长)
//       data[6]    = 帧序号(滚动计数)
//   为什么这三条可信(不是照抄公开文档):
//     · 字节边界与位序是**往返自检**过的 —— 用固件自己的 encodeFrame() 造帧、
//       再用同一套边界读回来,7 个数据字节逐字节一致;且 FCS(crc15_van_iso,
//       覆盖 IDEN+CMD+DATA)在**怠速抓包 1704/1704 帧、行驶抓包 34051/34051 帧**
//       全中 ⇒ 字节流就是线上字节流,没有别的自洽读法。
//     · 转速:点火瞬间 raw 从恒 0 跳到 6152(=769rpm,起动机拖动),峰值 10692
//       (=1336rpm),随后稳定在 **均值 7237.6 / sd 215**(=904.7rpm,sd 26.9);
//       地面真值怠速 900rpm ⇒ 偏差 0.52%。除以 4 会得 1809rpm(差 101%),排除。
//       行驶抓包 5988..47100(=748..5888rpm),仍在断油 6300 以下 ⇒ 量程自洽。
//     · 车速:data[2] 在**所有 5 个停车窗口**(位移累计量完全不动)内恒为 0/1/2;
//       行驶段与位移累计量的增长率(dS/dt)回归得 R²=0.994(残差 σ=0.70),
//       ∫data[2]dt 与 ΔS 的分段比值恒定在 0.037 m/count(10 段一致)
//       ⇒ 它是**速率**而不是累计量,且两条独立积分量互相印证。
//       |Δdata[2]|/Δt 的 p99 = 19.9 km/h/s < 21.6 km/h/s(乘用车加速度上限)。
//   ★★ 车速的绝对刻度已定(2026-09-22):**1 计数 = 2.56 km/h**(`kSpeedScale = 2.56f`)。
//     怎么测的:蓝牙 ELM327 的 PID **010D** 当真值,与板子记的 `0x824.data[2]`
//     两个串口接**同一台笔记本**(时间戳同源),368.2 s 行驶 / 599 个真值样本
//     (成功率 100%),284 对非零样本回归:
//       自由截距 真值 = 2.5549 x 计数 + 0.301   R² = 0.9984  σ = 1.18 km/h
//       过原点   真值 = 2.5679 x 计数
//     ★ 数据**只能分辨到 2.55~2.57** 这个窄带(斜率的 95% 置信区间 [2.543, 2.567],
//       2.56 与 2.571 的 RMSE 差 0.0004 km/h ⇒ 不可分辨)⇒ **2.56 是取整**,
//       不是"精确值",别把这条读过头。
//     原始数据:`tools/serial-capture/drive-2026-09-22-{van,obd}.csv`;
//     回归与配对方式的完整数字见 ACCEPTANCE.md 的 2026-09-22 汇总条目,
//     协议侧汇总见 VAN-PROTOCOL.md 的「车速」一节。
//   ☆ 历史(已被 2026-09-22 实测取代,只作沿革记录,别当现状):
//     · 最早 0.01 —— 来自公开文档,把 data[2..3] 当 16 位 ×100 km/h 读,与实车对不上;
//     · 之后 1.0(「1 计数 = 1 km/h」)—— 只有"物理自洽"、**没有地面真值**;
//       实测证明**差 2.5 倍**(真值 100 km/h 时计数才 39)。
//       ★ 当时的"先按 1.0 上屏、用表盘脸的档位复核(30/65/95/130)"这套做法
//         **已作废**:刻度已用真值定标,不需要再靠表盘反推;而且那块屏(DualEye)
//         2026-09-22 已退货、手上无屏,这条路本来也走不通。
//       ★ 四段定速(20/40/60/80)的标定跑仍按用户决定**不做** ——
//         以后顺手有 OBD 010D 日志即可复核(见 ACCEPTANCE.md 文末)。
//   来源(仅作背景,字段布局以实测为准):
//     morcibacsi/psa_van_bus_packet_descriptions (github)
//
// 物理层(SN65HVD230 模块 + VanBus 库,或 RMT 驱动)由外部接好,
// 把收到的原始帧转成 VanPacket 喂给 onPacket()。
// 线路层(SOF/4B5B/CRC-15/帧尾)由 van_wire.h 负责;van_phy.h 把两者接起来。
//
// ★★ 2026-09-24 新增:三类**已实测解出**的字段接进数据层
//   (`0x4FC` 的灯位域 / 门状态,`0xE24` 的 VIN)。
//
//   为什么放在 VanSource 里、而不是在 data_service 里另开一个"VAN 灯解析":
//     这三类字段和车速/转速一样,都是"**只有 VAN 这一个来源**"的字段 ⇒
//     解包必须贴着帧格式走(与车速/转速同一处、同一套 `data[i] < len` 判据),
//     而合并/来源标注才是 data_service 的事。两边分工见
//     `VAN-PROTOCOL.md` §8.1 那张表。
//
//   ★ 每一个常量的出处(协议侧;**照文档的确定部分实现,不确定的一个都没猜**):
//     0x4FC / CMD 0xC,`data[5]` 是**位域**(§4.3 / §4.4,实测提交 ee15e9e / 1a5d69b / 2826610):
//        bit2 = 0x04 = 左转向   bit3 = 0x08 = 右转向   (两位或 = 0x0C = 双闪)
//        bit6 = 0x40 = 近光     bit7 = 0x80 = 仪表盘灯
//     `data[1]` = 门状态位(§4.6,实测提交 9ad3d10)——
//        ⚠ **左右门不可分辨**(§6 撤回①):就我们抓的这段总线找不到能区分左右门的字节,
//          所以这里**只给"门信号有没有动"这个定量结果,不给左/右**。
//        ⚠ 而且它更像**瞬时/边沿信号**而不是稳态位:门开那几段脉冲**段内就在跳变**
//          (§4.6 末条),所以这里同样**不做 `==1 就是门开着` 这种读取**,只给
//          `doorActivity()`(= 这个字节相对**本帧之前见过的静息值**变了)。
//     0xE24 / CMD 0x8,`data[0..16]` = **17 字节明文 ASCII VIN**(§4.7,实测提交 f839a7d)。
//        ★ 不需要和 `0x5E4` 拼 —— `0xE24` 自己就是完整的 17 位(17 == 17 就是证据)。
//
//   ★ 采样坑(§4.3,必须照它设计):`0x4FC` 只有 **4.7 帧/秒**,而转向灯闪烁
//     全周期 0.80 s(≈1.25 Hz)⇒ 闪烁波形是**欠采样**的,"亮"只持续 1~3 帧。
//     所以灯位**不能**按"这一帧的位"直接当现状用 —— 见 VanSource::lights() 的
//     **保持窗口**(kIndicatorHoldMs),它把欠采样补回来。
struct VanLights {
  uint8_t raw = 0;           // data[5] 原文(诊断用;位定义见上)
  bool left = false;         // bit2
  bool right = false;        // bit3
  bool hazard = false;       // bit2|bit3 同时置位(§4.3:"双闪 = 左|右"独立证明它是位域)
  bool low_beam = false;     // bit6 = 近光
  bool dashboard = false;    // bit7 = 仪表盘灯(灯杆第 1 档)
};

// 位掩码常量(与 §4.3/§4.4 的表逐条对应,不写裸数字)
static const uint8_t kVanLightLeft      = 0x04u;
static const uint8_t kVanLightRight     = 0x08u;
static const uint8_t kVanLightLowBeam   = 0x40u;
static const uint8_t kVanLightDashboard = 0x80u;

// 灯位的**保持窗口**(ms)。为什么必须有它:
//   0x4FC 只有 4.7 帧/秒(≈213 ms 一帧),而闪烁全周期 0.80 s ⇒ 一帧"亮"之后
//   下面两三帧很可能是"灭"那一半。若按帧直读,屏上的箭头会以 ≈1.7 Hz 乱抖。
//   ★ 600 ms 的来历:`0x4FC` 帧间隔是指数分布(速率 4.7/s),连续 k 帧都落在
//     闪烁的"灭"半周期(0.40 s)内的概率 = (1-e^(-0.4×4.7))^k ≈ 0.154^k ⇒
//     k=2 时 2.4% 的"亮"段会被短一截;k=3 时 0.4%。取 600 ms 是"看得见的余量",
//     而不是精算值 —— 真车上以"箭头不闪、灭灯后约 0.6 s 内消失"为准。
static const uint32_t kIndicatorHoldMs = 600u;

// 帧长度:§3 的实测值(0x4FC = 11 字节、0xE24 = 17 字节)。
// ★ 别按旧文档的 4 字节 —— 那一条正是 §3.0 逐行重核时改掉的 4 行之一,
//   而且 `data[5]` 本来就需要 n ≥ 6。
static const uint8_t kVanLightLen = 11u;
static const uint8_t kVanVinLen   = 17u;

// VIN 缓冲长度:17 位 + '\0'。**17 是 VIN 的硬长度**(§4.7:17 字节 == 17 位字符,
// 这本身就是"不用跟别的族拼"的证据),所以按它定长。
static const uint8_t kVanVinChars = 17u;
static const uint8_t kVanVinBuf   = kVanVinChars + 1u;

// 字段宽度说明:协议规范里 IDEN 是 15 位(0x000/0xFFF 保留),CMD 是 5 位
// (EXT/RAK/RW/RTR,EXT 为保留位、应为 1)。本结构按 12 位 IDEN + 4 位 CMD
// 承载 —— 这是公开抓包的实际读法,15 位与 12 位的换算关系尚未用真实位流
// 确认(见 ACCEPTANCE.md 的实车必验清单)。
struct VanPacket {
  uint16_t iden;          // 12 位有效(高 4 位保留为 0)
  uint8_t  cmd;           // 4 位命令字段(EXT 位隐含为 1,见上)
  uint8_t  ack;           // 1 = 总线上有接收方应答(ACK 位为 dominant)
  uint8_t  fcs_ok;        // 1 = CRC-15 校验通过
  uint8_t  data[28];
  uint8_t  len;
  uint32_t rx_ms;
};

class VanSource {
public:
  VanSource() = default;

  void begin() {}  // 物理层初始化由外部驱动完成
  void onPacket(const VanPacket& pkt);
  // 超时回退由 data_service 处理;**灯位不需要 tick** —— 它的保持窗口是
  // 由 `lightsRecent(now)` 按 `now` 现算的(见 kIndicatorHoldMs 的说明),
  // 于是"没有帧的时候灯自己灭"这件事不依赖谁按点调 tick。
  void tick(uint32_t now_ms) { (void)now_ms; }

  bool hasSpeed() const { return speed_valid_; }
  bool hasRpm() const { return rpm_valid_; }
  float speedKmh() const { return speed_kmh_; }
  float rpm() const { return rpm_; }
  uint32_t lastUpdateMs() const { return last_update_ms_; }

  // ---------------- 灯位(0x4FC.data[5],已实测解出) ----------------
  // 收到过合法的灯帧 → true。**与"灯亮着没有"是两件事**:
  // 前者是"这一格有没有数据",后者要过保持窗口。

  // 最近一次解出的灯位(不看保持窗口)。没有过灯帧时全 false / raw = 0。
  const VanLights& lights() const { return lights_; }
  bool hasLights() const { return lights_seen_; }
  uint32_t lightsLastMs() const { return lights_ms_; }

  // 带**保持窗口**的现状:`now - lights_ms_ < kIndicatorHoldMs`。
  // ★ 上层(UI/告警)该用这一个,不要用 lights() 里的裸位 —— 理由见 kIndicatorHoldMs。
  bool lightsRecent(uint32_t now_ms) const {
    return lights_seen_ && lights_ms_ != 0u &&
           (uint32_t)(now_ms - lights_ms_) < kIndicatorHoldMs;
  }

  // ---------------- 门状态(0x4FC.data[1]) ----------------
  // ★ **只给"动过没有",不给"哪扇门 / 开着还是关着"** ——
  //   左右门可分辨性是**未解**(§6 撤回①),而"==1 就是门开着"被 §4.6 明确否掉
  //   (脉冲段内还在 `00`↔`01` 跳变)。所以这里刻意**不提供** `doorOpen()` 这种 API:
  //   名字一旦叫"门开着",上层就会有人当真值用。
  //
  //   本函数回答的是:"`data[1]` 相对**我们见过的静息值**变过没有,且变化在
  //   `window_ms` 之内"。静息值取**第一次见到的那个值**(实车静息是 `0x00`,
  //   见 §4.6 的表),所以第一次收到帧就建立了基线、不会自己触发。
  bool doorActivity(uint32_t now_ms, uint32_t window_ms) const {
    return door_base_seen_ && door_change_ms_ != 0u &&
           (uint32_t)(now_ms - door_change_ms_) < window_ms;
  }
  bool hasDoor() const { return door_base_seen_; }
  uint8_t doorRaw() const { return door_raw_; }
  uint32_t doorChangeMs() const { return door_change_ms_; }

  // ---------------- VIN(0xE24,17 字节明文) ----------------
  bool hasVin() const { return vin_valid_; }
  const char* vin() const { return vin_; }
  uint32_t vinLastMs() const { return vin_ms_; }

  // 实车帧格式与默认常量不符时,先用这个在运行时改,确认后写回常量
  // ★ 语义见 onPacket():speed_offset 指向**单字节**车速,scale 只做乘法
  //   (默认 2.56f ⇒ 1 计数 = 2.56 km/h,见 .cpp 的实测定标说明)。
  void configureSpeedFrame(uint16_t iden, uint8_t speed_offset, float speed_scale);

  static void dumpRaw(const VanPacket& pkt);  // 嗅探模式

  static const uint16_t kSpeedIden   = 0x824;
  static const uint8_t  kSpeedOffset = 2;       // data[2],**单字节** km/h
  static const float    kSpeedScale;            // 2.56(1 计数 = 2.56 km/h,实测)
  static const uint8_t  kRpmOffset   = 0;       // data[0..1],16 位大端
  static const float    kRpmScale;              // 0.125(÷8)

  // 灯/门 与 VIN 两族的 IDEN(§3 的帧目录表:0x4FC/0xC 与 0xE24/0x8)
  static const uint16_t kLightIden  = 0x4FC;
  static const uint8_t  kLightOffset = 5;       // data[5],位域(§4.3/§4.4)
  static const uint8_t  kDoorOffset  = 1;       // data[1],门状态位(§4.6)
  static const uint16_t kVinIden    = 0xE24;
  // ★ CMD 不参与本类的匹配:现有 onPacket 只按 IDEN 分支(车速/转速都没有判 cmd),
  //   这里**沿用同一套语义**,以免出现"车速不看 cmd、灯却看 cmd"的不一致。
  //   §3 的表里这两族的 CMD 都是 0x8 / 0xC 之外的组合各有实测,留待将来需要时再收紧。

private:
  bool speed_valid_ = false;
  bool rpm_valid_ = false;
  float speed_kmh_ = 0.0f;
  float rpm_ = 0.0f;
  uint32_t last_update_ms_ = 0;

  uint16_t speed_iden_ = kSpeedIden;
  uint8_t  speed_offset_ = kSpeedOffset;
  float    speed_scale_ = kSpeedScale;

  // ---- 灯位 ----
  VanLights lights_{};
  bool lights_seen_ = false;
  uint32_t lights_ms_ = 0;     // 最近一次灯帧(0 = 还没见过)

  // ---- 门状态 ----
  uint8_t door_raw_ = 0;
  bool door_base_seen_ = false;   // 静息基线建立了吗(第一帧就是基线)
  uint8_t door_base_ = 0;
  uint32_t door_change_ms_ = 0;   // 最近一次"与基线不同"的时刻(0 = 从未)

  // ---- VIN ----
  char vin_[kVanVinBuf] = {0};
  bool vin_valid_ = false;
  uint32_t vin_ms_ = 0;
};
