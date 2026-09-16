#include "boot_anim.h"
#include "ui_theme.h"

static float smoothstep(float k) {
  if (k <= 0.0f) return 0.0f;
  if (k >= 1.0f) return 1.0f;
  return k * k * (3.0f - 2.0f * k);
}

void BootAnim::start(uint32_t now_ms) {
  start_ms_ = now_ms;
  end_ms_ = now_ms + BOOT_TOTAL_MS;
}

uint8_t BootAnim::screenOpa(uint32_t now_ms) const {
  const uint32_t t = now_ms - start_ms_;
  if (t >= BOOT_FADE_MS) return 255;
  return (uint8_t)((255u * t) / BOOT_FADE_MS);
}

float BootAnim::arcProgress(uint32_t now_ms, uint8_t screen) const {
  const uint32_t t = now_ms - start_ms_;
  const uint32_t s = BOOT_SWEEP_START_MS + (uint32_t)screen * BOOT_STAGGER_MS;

  if (t < s) return 0.0f;
  if (t < s + BOOT_SWEEP_RISE_MS) {
    return smoothstep((float)(t - s) / (float)BOOT_SWEEP_RISE_MS);
  }
  if (t < s + BOOT_SWEEP_RISE_MS + BOOT_SWEEP_HOLD_MS) return 1.0f;
  if (t < s + BOOT_SWEEP_RISE_MS + BOOT_SWEEP_HOLD_MS + BOOT_SWEEP_FALL_MS) {
    const uint32_t f = t - s - BOOT_SWEEP_RISE_MS - BOOT_SWEEP_HOLD_MS;
    return 1.0f - smoothstep((float)f / (float)BOOT_SWEEP_FALL_MS);
  }
  return 0.0f;
}

uint8_t BootAnim::faceStage(uint32_t now_ms) const {
  const uint32_t t = now_ms - start_ms_;
  return (t < BOOT_FACE_START_MS) ? 0 : 1;   // 0=隐藏,1=显示(眨眼状态已删)
}
