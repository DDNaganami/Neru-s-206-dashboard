#pragma once
#include <stdint.h>

// 开机动画状态机:全屏淡入 → 双屏错峰扫表(满弧停留后回零) → 表情睁眼眨眼 → 结束。
// 纯时间计算,不含 LVGL 依赖;输出由 dash_ui 应用到控件。
// 所有时长常量在 ui_theme.h(BOOT_*),换节奏只改那里。
class BootAnim {
public:
  void start(uint32_t now_ms);
  bool active(uint32_t now_ms) const { return now_ms < end_ms_; }

  uint8_t screenOpa(uint32_t now_ms) const;                    // 0..255 全屏透明度
  float arcProgress(uint32_t now_ms, uint8_t screen) const;    // 0..1 扫表进度(右屏错峰)
  uint8_t faceStage(uint32_t now_ms) const;                    // 0=隐藏 1=眨眼 2=睁眼

private:
  uint32_t start_ms_ = 0;
  uint32_t end_ms_ = 0;
};
