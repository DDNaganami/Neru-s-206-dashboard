#pragma once
#include <stdint.h>
#include "alerts.h"   // AlertKind（判据层的唯一出处）

// ============================================================
// 告警 → 视图 的那一层（纯映射，不碰 LVGL、不碰硬件、不依赖 Arduino）
//
// ★★ 2026-10-15（车主）：**整条指示灯栏删掉了**。
//   原话："车灯车门原车仪表显示更好" —— 左右转 / 双闪 / 近光 / 仪表盘灯 / 门
//   这六格灯不再画，它们原来住在本文件的前身 `lamp_view.h`（六格槽位枚举 +
//   灯条几何 + `make_lamps()`），那个头文件已随本次改动**整个删掉**。
//   ★ 数据层**一个字都没动**：VAN 照旧解 `0x4FC` 的灯位与门位、照旧进
//     `VehicleState`（`van_source.cpp` / `data_service.cpp` / `vehicle_state.h`），
//     只是现在没有任何地方把它画出来（串口日志照旧报，见 `SRC-VAN` 那两行）。
//   ★ `alerts.*` 里"门/转向"的**判断逻辑也一个字都没动**（车主只砍了蜂鸣器）——
//     告警该报还报，只是不再有蜂鸣器发声、也不再有灯可以闪。
//
// ★ 本文件现在只剩下**一件事**：把"告警层此刻报的是哪一条"传给 UI。
//   为什么还要一个结构体（而不是直接传 `uint8_t`）：口径与 `panel_view.h` /
//   `preview_input.h` 一样 —— `dash_ui` 拿到的是**已判好的结论**，它自己不做
//   任何判断；将来若告警要重新上屏（换形态：文字条 / 表盘变色 / 新的角标），
//   加字段只改这一个头 + dash_ui 那一段，`alerts.*` 一行都不用动。
//
// ★ 告警号的本地复述（`kAlertNone` … `kAlertTurnSig`）也一起删了：
//   本文件现在**直接 include `alerts.h`**（它在 lib/dashcore/ 里，
//   而 native 构建本来就编 lib/）⇒ 不再需要"复述数值 + static_assert 钉住"
//   那一套（那套在 `lamp_view.h` 里的唯一理由是"两个模块互不依赖"，
//   而现在这份映射本来就只做一件事：把 AlertKind 原样递给 UI）。
// ============================================================

struct AlertView {
  // 告警层此刻正在报的那一条（`AlertKind::None` = 没有告警）。
  // ★ 它是**判据层的结论**，不是 UI 算出来的 —— 由 main 从 `Alerts::update()`
  //   的返回值原样传进来（见 src/main.cpp 的告警那一段）。
  AlertKind kind = AlertKind::None;
};

inline AlertView make_alert_view(AlertKind k) {
  AlertView v;
  v.kind = k;
  return v;
}
