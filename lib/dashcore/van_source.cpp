#include "van_source.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

const float VanSource::kSpeedScale = 0.01f;
const float VanSource::kRpmScale   = 0.125f;

// 值域钳制:总线噪声/坏帧不会把 UI 打飞
static const float kSpeedMaxValid = 300.0f;
static const float kRpmMaxValid   = 9000.0f;

void VanSource::onPacket(const VanPacket& pkt) {
  if (pkt.iden != speed_iden_) return;

  if (speed_offset_ + 1 < pkt.len) {
    const uint16_t raw =
        (uint16_t)(pkt.data[speed_offset_] << 8) | pkt.data[speed_offset_ + 1];
    const float v = raw * speed_scale_;
    if (v >= 0.0f && v <= kSpeedMaxValid) {
      speed_kmh_ = v;
      speed_valid_ = true;
      last_update_ms_ = pkt.rx_ms;
    }
  }
  if (kRpmOffset + 1 < pkt.len) {
    const uint16_t raw =
        (uint16_t)(pkt.data[kRpmOffset] << 8) | pkt.data[kRpmOffset + 1];
    const float r = raw * kRpmScale;
    if (r >= 0.0f && r <= kRpmMaxValid) {
      rpm_ = r;
      rpm_valid_ = true;
      last_update_ms_ = pkt.rx_ms;
    }
  }
}

void VanSource::configureSpeedFrame(uint16_t iden, uint8_t offset, float scale) {
  speed_iden_ = iden;
  speed_offset_ = offset;
  speed_scale_ = scale;
}

void VanSource::dumpRaw(const VanPacket& pkt) {
#if defined(ARDUINO)
  Serial.printf("VAN id=%03X len=%u:", pkt.iden, pkt.len);
  for (uint8_t i = 0; i < pkt.len; ++i) Serial.printf(" %02X", pkt.data[i]);
  Serial.println();
#else
  (void)pkt;  // 宿主机测试构建不链接 Serial
#endif
}
