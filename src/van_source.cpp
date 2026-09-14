#include "van_source.h"
#include <Arduino.h>

const float VanSource::kSpeedScale = 0.01f;
const float VanSource::kRpmScale   = 0.125f;

void VanSource::onPacket(const VanPacket& pkt) {
  if (pkt.iden != speed_iden_) return;

  if (speed_offset_ + 1 < pkt.len) {
    const uint16_t raw =
        (uint16_t)(pkt.data[speed_offset_] << 8) | pkt.data[speed_offset_ + 1];
    speed_kmh_ = raw * speed_scale_;
    speed_valid_ = true;
    last_update_ms_ = pkt.rx_ms;
  }
  if (kRpmOffset + 1 < pkt.len) {
    const uint16_t raw =
        (uint16_t)(pkt.data[kRpmOffset] << 8) | pkt.data[kRpmOffset + 1];
    rpm_ = raw * kRpmScale;
    rpm_valid_ = true;
    last_update_ms_ = pkt.rx_ms;
  }
}

void VanSource::configureSpeedFrame(uint16_t iden, uint8_t offset, float scale) {
  speed_iden_ = iden;
  speed_offset_ = offset;
  speed_scale_ = scale;
}

void VanSource::dumpRaw(const VanPacket& pkt) {
  Serial.printf("VAN id=%03X len=%u:", pkt.iden, pkt.len);
  for (uint8_t i = 0; i < pkt.len; ++i) Serial.printf(" %02X", pkt.data[i]);
  Serial.println();
}
