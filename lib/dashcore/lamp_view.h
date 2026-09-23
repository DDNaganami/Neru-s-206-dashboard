#pragma once
#include <stdint.h>
#include "vehicle_state.h"

// ============================================================
// 指示灯槽位（2026-09-24 新增）
//
// ★ 为什么在 lib/dashcore/ 而不是 src/ui_model.h：
//   这个头是**纯映射**（车状态 + 告警 → 六格灯的亮/闪/描边），不碰 LVGL、
//   不碰硬件、也不碰 Arduino —— 而"几何算错"与"哪一格该亮"恰恰是最需要
//   机器钉住的两件事（算错不会报错，只会让灯条在圆屏上被削掉一角、
//   或者压住水温数字）。住在 lib/ 里，native 用例就能直接 include 它。
//   ★ 另一半原因是构建的硬约束：native 的 lib_archive = no 构建**只编
//     lib/ 下的源码**（不编 src/）—— 实测把纯函数放在 src/*.cpp 里会让用例
//     拿到 undefined symbol。所以"想测谁就把它放进 lib/"是有意的结构约束。
//
// ★ 位置口径（480 基准，与弧/读数同一套 theme_scale() 缩放）：
//   灯条压在**表盘底部**那条留白里 —— 副表读数（水温/进气）在 y=384，
//   它的墨迹下沿约 y≈396（18 号字高 13）⇒ 灯条从 y=395 起、到 435 止。
//   这一段**整条都落在内切圆里**（几何用例逐角核对），圆屏上不会被削掉。
//
// ★ 占位图形：**本机一律用占位几何**（简单多边形/圆/圈），不画正式素材 ——
//   真屏到了再换，让"逻辑"与"美术"解耦（换素材只改 src/dash_ui.cpp 的
//   build_lamps 那一段，本文件与 alerts 一个字都不用动）。
// ============================================================
enum class LampSlot : uint8_t {
  LeftArrow = 0,   // 左转向
  RightArrow,      // 右转向
  Hazard,          // 双闪
  LowBeam,         // 近光
  PositionLamp,    // 仪表盘灯
  Door,            // 门提示
  Count
};
static const uint8_t kLampSlotCount = (uint8_t)LampSlot::Count;

// 告警号的**本地复述**（`AlertKind` 的数值，见 lib/dashcore/alerts.h）。
// ★ 为什么不直接 include alerts.h：本文件是"把数据变成视图"的纯映射层，
//   让它依赖告警状态机会把两个模块绑在一起（告警层改一个内部枚举，
//   表盘就得跟着重编）。所以这里复述一次数值，**并用 static_assert 钉住**——
//   复述错了编译期就报出来（见文件末尾那几条）。
//   ★ 写成 `typedef enum …` 而不是匿名 `enum : uint8_t`：后者在 C++ 里是
//     GCC 扩展（Clang 直接报 "a type specifier is required"），typedef 写法
//     两条编译链都干净。
//   ★ 新增告警时：先加 alerts.h 的枚举，再加这里一行 + static_assert 一行。
typedef enum : uint8_t {
  kAlertNone      = 0,
  kAlertOverspeed = 1,
  kAlertRedline   = 2,
  kAlertDoor      = 3,
  kAlertTurnSig   = 4,
} AlertIdMirror;

// 灯条几何（480 基准）。
// ★ 这几个数是**算出来的、不是抄来的**：6 个槽 × 40 宽 + 5 × 6 间隔 = 270，
//   居中 ⇒ 首槽左沿 x=105。不变式（四角必须落在内切圆内、顶边不许压副表小字）
//   由 test_ui_lamps.cpp 逐条钉住 ⇒ 破了当场红。
static const int32_t kLampSize   = 40;    // 每个槽的边长
static const int32_t kLampGap    = 6;     // 槽间距
static const int32_t kLampCy     = 415;   // 槽中心 y
static const int32_t kLampRowW   = kLampSlotCount * kLampSize +
                                   (kLampSlotCount - 1) * kLampGap;   // = 270
static const int32_t kLampX0     = (480 - kLampRowW) / 2;            // = 105

// 屏幕基准分辨率（与 ui_theme.h 的 THEME_BASE_RES 同一个数）。
// ★ 这里复述一份是因为本文件**不依赖 ui_theme.h**（那个头要 LVGL，
//   而本文件要能在宿主机上单独编）。几何常量都是 480 基准，改屏另说。
static const int32_t kLampBaseRes = 480;

// 某个槽的左沿 / 上沿（480 基准）
inline int32_t lampLeft(LampSlot s) {
  return kLampX0 + (int32_t)s * (kLampSize + kLampGap);
}
inline int32_t lampTop(LampSlot) { return kLampCy - kLampSize / 2; }

struct LampView {
  bool on[kLampSlotCount] = {};            // 这一格现在该不该亮
  uint8_t pulse[kLampSlotCount] = {};      // 亮度 0..255（0 = 不亮；转向灯/告警会闪）
  bool alert[kLampSlotCount] = {};         // 是不是"告警层正在报的那一条"（画描边用）
};

// 把"车状态 + 告警状态"落成六格灯。
//
// `alert_on` = 告警层现在正在报的那一条（AlertKind，由 main 传进来 —— 这样
//   本文件不必 include alerts.h，两个模块保持互不依赖；数值一致由文件末尾的
//   static_assert 钉住）。
// `alert_pulse` = 告警那一拍的脉冲（true = 这一拍闪）。它由 main 从
//   `Alerts::beeping()`（或它驱动的一个短脉冲）给 —— **屏上闪与蜂鸣器同拍**
//   是一条信息，两者不同步会很怪。
//
// ★ 超速/红区**没有专属槽位**（不新开 L 号、也不为未解字段建空壳 UI）：
//   它们借"左右箭头 / 仪表盘灯"那两格的位置闪 —— 这正是"占位"的意思。
//   正式素材到位后它们会有自己的槽位，那时只改下面这张映射表。
//
// 转向灯/双闪的显示闪烁：半周期（ms）。
// 实车闪烁全周期实测 0.80 s（§4.3，三段一致）⇒ 半周期 0.40 s。
// 这里取 375（周期 0.75 s）是**显示用的近似值**：屏上多闪一点点没有坏处，
// 而"看着像在闪"比"精确复现 1.25 Hz"重要。真车标定时改这一个数。
static const uint32_t kLampBlinkHalfMs = 375u;

inline LampView make_lamps(const VehicleState& s, uint8_t alert_on, uint32_t now_ms,
                           bool alert_pulse) {
  LampView v{};
  // ★ 极性约定：`now` 落在第 **偶数** 个半周期里 = 亮。
  //   于是"打灯那一刻(now=0)是亮的" —— 这一条不只是好看：它让"刚打灯 →
  //   屏上立刻有反应"，而不是先黑 375 ms（看着像没反应）。
  //   （第一版写成 `(now / half) & 1` —— 那是反的，实测用例当场红了：
  //     t=0 判成"灭"。极性这种东西一定要有断言钉着。）
  const bool blink_half = ((now_ms / kLampBlinkHalfMs) % 2u) == 0u;
  // 转向灯：亮着的时候在闪（欠采样已被数据层的 600 ms 保持窗口补平，
  // 所以这里 s.indicator_* 在"打灯期间"是连续的 true，见 van_source.h）
  const bool left  = s.indicator_left;
  const bool right = s.indicator_right;

  // 槽位与"哪条告警"的对应（见 alerts.h 的 AlertKind 顺序）
  v.on[(uint8_t)LampSlot::LeftArrow]    = left  && blink_half;
  v.on[(uint8_t)LampSlot::RightArrow]   = right && blink_half;
  v.on[(uint8_t)LampSlot::Hazard]       = s.hazard && blink_half;
  v.on[(uint8_t)LampSlot::LowBeam]      = s.low_beam;         // 灯杆控制的稳态灯：不闪
  v.on[(uint8_t)LampSlot::PositionLamp] = s.position_lamp;    // 同上
  v.on[(uint8_t)LampSlot::Door]         = s.door_activity;    // 有活动就提示

  for (uint8_t i = 0; i < kLampSlotCount; ++i) {
    v.pulse[i] = v.on[i] ? 255u : 0u;
  }

  // ---- 告警闪烁：正在报的那一条对应的灯，盖过它自己的节奏 ----
  // 为什么用"告警相位"而不是再算一个：屏上的闪与蜂鸣器要**同一拍**
  // （听起来在叫、看起来在闪 = 一条信息；两者不同步会很怪）。
  // 超速/红区没有专属灯位（不新开 L 号、也不为未解字段建空壳 UI），
  // 所以它们借**转向灯/门**那两格的位置闪 —— 这正是"占位"的意思：
  // 正式素材到位后，超速/红区会有自己的槽位，那时只改这张映射表。
  switch (alert_on) {
    case kAlertOverspeed:   // 超速：借左/右两格一起闪（最显眼的位置）
      if (alert_pulse) {
        v.on[(uint8_t)LampSlot::LeftArrow] = true;
        v.on[(uint8_t)LampSlot::RightArrow] = true;
        v.pulse[(uint8_t)LampSlot::LeftArrow] = 255u;
        v.pulse[(uint8_t)LampSlot::RightArrow] = 255u;
        v.alert[(uint8_t)LampSlot::LeftArrow] = true;
        v.alert[(uint8_t)LampSlot::RightArrow] = true;
      }
      break;
    case kAlertRedline:     // 转速红区：借"仪表盘灯"那一格
      if (alert_pulse) {
        v.on[(uint8_t)LampSlot::PositionLamp] = true;
        v.pulse[(uint8_t)LampSlot::PositionLamp] = 255u;
        v.alert[(uint8_t)LampSlot::PositionLamp] = true;
      }
      break;
    case kAlertDoor:        // 门：门那一格本来就是亮的，告警时只加描边
      if (s.door_activity) v.alert[(uint8_t)LampSlot::Door] = true;
      break;
    case kAlertTurnSig:     // 转向灯忘关：对应那一格加描边（它本来就在闪）
      if (left)  v.alert[(uint8_t)LampSlot::LeftArrow] = true;
      if (right) v.alert[(uint8_t)LampSlot::RightArrow] = true;
      break;
    default:
      break;
  }
  return v;
}

// ★ 复述的那些告警号必须与 alerts.h 一致（不一致 = 屏上闪错的那一格）。
//   用 `#if __has_include` 是刻意的：本头文件在**没有** alerts.h 的构建里
//   （将来若有人把 lib/dashcore 拆开）也要能单独编——那时这两条断言自动消失，
//   而只要 alerts.h 在场就一定被检查（native 与四个固件目标全都在场）。
#if defined(__has_include)
#if __has_include("alerts.h")
#include "alerts.h"
static_assert(kAlertOverspeed == (uint8_t)AlertKind::Overspeed,
              "ui_model 的 kAlertOverspeed 与 alerts.h 的 AlertKind 对不上");
static_assert(kAlertRedline == (uint8_t)AlertKind::Redline,
              "ui_model 的 kAlertRedline 与 alerts.h 的 AlertKind 对不上");
static_assert(kAlertDoor == (uint8_t)AlertKind::Door,
              "ui_model 的 kAlertDoor 与 alerts.h 的 AlertKind 对不上");
static_assert(kAlertTurnSig == (uint8_t)AlertKind::TurnSignal,
              "ui_model 的 kAlertTurnSig 与 alerts.h 的 AlertKind 对不上");
#endif
#endif
