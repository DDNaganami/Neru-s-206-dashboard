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
//   ★★ 已定(2026-09-22):1 计数 = **2.56 km/h**,用蓝牙 ELM327 的 PID 010D 当真值实测回归得到
//     "车速"列全是 0 —— 车没动,见 ACCEPTANCE.md)。2026-09-20 用户决定:
//     先按 1.0 上屏,用**表盘脸的档位**复核(30/65/95/130 —— 若真值差一倍,
//     "市区"脸会等到真车速约 60 才出现);不再要求 20/40/60/80 定速跑,
//     以后顺手有 OBD 日志时采几个 PID 010D 散点定标即可(见 ACCEPTANCE.md 文末)。
//   来源(仅作背景,字段布局以实测为准):
//     morcibacsi/psa_van_bus_packet_descriptions (github)
//
// 物理层(SN65HVD230 模块 + VanBus 库,或 RMT 驱动)由外部接好,
// 把收到的原始帧转成 VanPacket 喂给 onPacket()。
// 线路层(SOF/4B5B/CRC-15/帧尾)由 van_wire.h 负责;van_phy.h 把两者接起来。
//
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
  void tick(uint32_t now_ms) { (void)now_ms; }  // 超时回退由 data_service 处理

  bool hasSpeed() const { return speed_valid_; }
  bool hasRpm() const { return rpm_valid_; }
  float speedKmh() const { return speed_kmh_; }
  float rpm() const { return rpm_; }
  uint32_t lastUpdateMs() const { return last_update_ms_; }

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

private:
  bool speed_valid_ = false;
  bool rpm_valid_ = false;
  float speed_kmh_ = 0.0f;
  float rpm_ = 0.0f;
  uint32_t last_update_ms_ = 0;

  uint16_t speed_iden_ = kSpeedIden;
  uint8_t  speed_offset_ = kSpeedOffset;
  float    speed_scale_ = kSpeedScale;
};
