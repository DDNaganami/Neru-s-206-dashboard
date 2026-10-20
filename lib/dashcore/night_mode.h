#pragma once
#include <stdint.h>

// ============================================================
// 夜间模式的**纯判据**（不碰 LVGL、不碰硬件、不依赖 Arduino）
//
// ★ 为什么单独一层：这条逻辑要判的是"车灯亮了几秒 ⇒ 该不该进夜间"，
//   而它一旦写进 render 循环里，就既没法单测、又会被人顺手改成"每帧都判"。
//   所以这里只有两个纯结构体 + 一个纯函数 —— native 用例直接喂时间戳。
//
// ★ 触发源用**车灯**（近光 / 仪表盘灯），不用时钟：
//   板上那颗 PCF85063 只有硬件、没有驱动，也没有对时来源
//   （见 `docs/NIGHT-MODE.md` §1）。而 `VehicleState` 里的
//   `low_beam` / `position_lamp` 是 VAN `0x4FC` 解出来的、**已经过保持窗口**的真值，
//   零新增硬件。什么时候该暗，车自己知道。
//
// ★ 迟滞为什么是 3 s 进 / 10 s 出：
//   进得快 → 一开灯就暗下来（这是车主想要的即时感）；
//   退得慢 → 隧道口、会车闪灯、灯杆抖动都不会让屏来回跳
//   （一跳就是一次整屏重绘，夜里特别刺眼）。
// ============================================================

struct NightModeConfig {
  uint32_t enter_ms = 3000;    // 灯亮持续这么久 ⇒ 进夜间
  uint32_t exit_ms  = 10000;   // 灯灭持续这么久 ⇒ 回白天
  uint8_t  day_pct  = 75;      // 白天基准亮度（%，车主 2026-10-17 定）
  uint8_t  night_pct = 30;     // 夜间亮度（%,先按 30 起步，车上看着调）
};

struct NightModeState {
  bool     night = false;   // 当前是不是夜间态
  bool     want  = false;   // 正在等的目标态（与 night 不同 = 计时中）
  uint32_t since = 0;       // 进入"这个候选态"的时刻（ms）
};

// 每拍调一次。返回 **true = 这一拍翻转了**（调用方据此去写背光/主题 -- 只写一次！）。
//
// lamp_on  = `low_beam || position_lamp`（已经过 VAN 的保持窗口，别拿原始位域进来）
// data_ok  = 数据可信（`DataTrustReason::kNone`）
//
// ★ `data_ok == false` 时**保持当前态，而且不动计时器**：
//   数据断了我们并不知道车灯到底亮着没有，此时翻转只会造成"夜里忽然变亮"，
//   比"该暗的没暗"难看得多。这条与告警层"宁可少报"是同一个口径。
// ★ 时间用无符号减法比较，32 位回绕安全（`now - since` 在回绕时仍然正确）。
inline bool night_mode_update(NightModeState& st, uint32_t now, bool lamp_on, bool data_ok,
                              const NightModeConfig& cfg) {
  if (!data_ok) return false;            // 状态不明 ⇒ 一个字节都不动

  if (lamp_on == st.night) {             // 已经是目标态：计时器归零，不翻转
    st.want  = st.night;
    st.since = now;
    return false;
  }
  if (lamp_on != st.want) {              // 目标态刚刚变了：从头计时
    st.want  = lamp_on;
    st.since = now;
    return false;
  }
  const uint32_t need = lamp_on ? cfg.enter_ms : cfg.exit_ms;
  if ((uint32_t)(now - st.since) < need) return false;

  st.night = lamp_on;                    // 到点了 ⇒ 翻转
  st.want  = lamp_on;
  st.since = now;
  return true;
}

// 当前该给多少背光百分比（翻转后调用方拿它去写一次）。
inline uint8_t night_mode_pct(const NightModeState& st, const NightModeConfig& cfg) {
  return st.night ? cfg.night_pct : cfg.day_pct;
}
