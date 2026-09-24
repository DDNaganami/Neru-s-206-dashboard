#pragma once
#include "ui_model.h"
#include "system_status.h"   // SystemStatus / SysStatusInputs / DiagView（值语义）

void dash_ui_init();
void dash_ui_tick(uint32_t now_ms);            // 每个主循环都调:LVGL 心跳
// 5Hz 由主循环节流。
//
// ★ 2026-09-24 新增了三个参数，**每一个都是必填**（不给默认值）：
//   默认值会让"接线漏了一处"变成静默行为（角标永远不显示 / 诊断页永远不出现 /
//   静音状态永远显示 0），而那正是最难查的一类。
//   ① `SystemStatus& sys`（**非 const**）：渲染这一拍由 dash_ui 推进
//      "数据不可信"状态机（判据/去抖/限速都在 lib/dashcore/system_status.h，
//      这里只是每 200 ms 喂它一次并读结论）⇒ 必须可写。
//   ② `const SysStatusInputs& diag`：诊断页要显示的原始数据（VAN 计数、
//      来源档位、链路三档、OBD、heap/PSRAM、帧率…）。诊断页关着时一个字节都不读。
//   ③ `bool beep_muted` / `alert_active` 已经包含在 `diag` 里（那两个字段），
//      所以不另开参数 —— 少一个能对不上的地方。
//   ★ 设备端与 pcpreview 走的是同一份（本函数里没有 `#if`）—— 上板验证时
//     "预览里看得见、车上看不见"这种分叉在结构上不可能发生。
void dash_ui_render(const ArcDashView& v, const LampView& lamps, SystemStatus& sys,
                    const SysStatusInputs& diag, uint32_t now_ms);

// 诊断页的开/关与翻页（由主循环的按键驱动；真机上是长按/组合键）。
// ★ 这两个函数**不在** dash_ui 里读键盘：输入从哪来是 main 的事，
//   dash_ui 只回答"画成什么样"（与 lamp_view.h 的分层同一条）。
void dash_ui_diag_toggle();
void dash_ui_diag_next();
bool dash_ui_diag_open();
// 当前是第几页（0 起）。主循环靠它判断"再按一次 K 是不是该关了"
//   （翻过最后一页 ⇒ 关闭；这样"打开/翻页/关闭"共用同一个键）。
uint8_t dash_ui_diag_page();
