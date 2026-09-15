#pragma once
#include <stdint.h>

// VAN 总线数据源:接收原始帧,提取车速与转速。
//
// 车速/转速帧(PSA VAN,VAN-INFO 网络,307 实测 / 206 适用,待实车验证):
//   IDEN 0x824,BSI → Dashboard,7 字节:
//     data[0..1] = 转速 x8(大端)     例 18 F8 → 6392/8 = 799 rpm
//     data[2..3] = 车速 x100 km/h(大端) 例 00 00 → 0 km/h
//     data[4..6] = 序号
//   来源: morcibacsi/psa_van_bus_packet_descriptions (github)
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
  void configureSpeedFrame(uint16_t iden, uint8_t speed_offset, float speed_scale);

  static void dumpRaw(const VanPacket& pkt);  // 嗅探模式

  static const uint16_t kSpeedIden   = 0x824;
  static const uint8_t  kSpeedOffset = 2;       // data[2..3]
  static const float    kSpeedScale;            // 0.01(÷100)
  static const uint8_t  kRpmOffset   = 0;       // data[0..1]
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
