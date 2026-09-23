#include "van_source.h"
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

#if defined(ARDUINO)
#include <Arduino.h>
#endif

// ★★ 车速定标 = 2.56 : 1（2026-09-22 **首次有地面真值**，实测定标）
//
//   怎么测的:蓝牙 ELM327(COM6，自报 v1.5，能读 K 线)读 PID 010D 当真值，
//     同时用我们自己的板子记 0x824 的 data[2]，两个串口接**同一台笔记本**
//     ⇒ 时间戳同源、不用事后对齐。整趟行驶 368 秒 / 599 个车速样本(成功率 100%)。
//   回归(286 个非零配对样本):
//     自由截距: 真值 = 2.5552 × 计数 + 0.364   R² = 0.993   σ = 2.45 km/h
//     过原点  : 真值 = 2.571  × 计数           R² = 0.9929  σ = 2.46 km/h
//     ⇒ 取 **2.56**。
//
//   ★ 这条推翻了之前两次的写法，两次都错:
//     · 最早 0.01(把 data[2..3] 当 16 位 ×100 km/h) —— 来自公开文档，与实车对不上;
//     · 之后改成 1.0(「1 计数 = 1 km/h」) —— 当时只有"物理自洽"没有真值，
//       实测证明**差 2.5 倍**：真值 100 km/h 时计数才 39。
//   分档均值单调规整(0-9km/h→1.93 计数 … 100+km/h→38.33 计数)，是干净的线性编码。
//
//   ⚠ 已知边界:本轮覆盖 0..100 km/h。计数上界 39 ⇒ 按 2.56 换算是约 **100 km/h**，
//     所以 8 位字段在这个标度下**量程只到 100 km/h**；再快会饱和（本次没跑到，未验）。
const float VanSource::kSpeedScale = 2.56f;
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
