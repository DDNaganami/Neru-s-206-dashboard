#include "van_source.h"
#include "dash_log.h"   // 日志同时打到 USB-CDC 与 UART0(见文件头说明)

#if defined(ARDUINO)
#include <Arduino.h>
#endif

// ★ 车速定标 = 1.0:1 计数 = 1 km/h(字段是**单字节**,见 van_source.h 的实测说明)。
//   历史值是 0.01(把 data[2..3] 当 16 位 x100 km/h 读)—— 那条来自公开文档,
//   与两份实车抓包都对不上,已按实测改掉。
const float VanSource::kSpeedScale = 1.0f;
const float VanSource::kRpmScale   = 0.125f;

// 值域钳制:总线噪声/坏帧不会把 UI 打飞
static const float kSpeedMaxValid = 300.0f;
static const float kRpmMaxValid   = 9000.0f;

void VanSource::onPacket(const VanPacket& pkt) {
  if (pkt.iden != speed_iden_) return;

  // 车速是**单字节**(data[speed_offset_])。上限 300 只在有人把 scale 调大时
  // 才可能触发;正常 8 位字段最大 255 km/h,天然在范围内。
  if (speed_offset_ < pkt.len) {
    const float v = (float)pkt.data[speed_offset_] * speed_scale_;
    if (v >= 0.0f && v <= kSpeedMaxValid) {
      speed_kmh_ = v;
      speed_valid_ = true;
      last_update_ms_ = pkt.rx_ms;
    }
  }
  // 转速是 16 位大端(data[0..1]),x8 ⇒ 原始值 = rpm * 8。
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
  dash_logf("VAN id=%03X len=%u:", pkt.iden, pkt.len);
  for (uint8_t i = 0; i < pkt.len; ++i) dash_logf(" %02X", pkt.data[i]);
  dash_logf("\n");
#else
  (void)pkt;  // 宿主机测试构建不链接 Serial
#endif
}
