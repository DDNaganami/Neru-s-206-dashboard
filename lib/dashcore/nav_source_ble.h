#pragma once
#include "nav_state.h"

// ============================================================
// BLE 导航源（阶段 A 骨架）—— 手机做 central，本板做 peripheral
//
// 只在定义了 `DASH_NAV_BLE` 的 env 里编（需要 NimBLE-Arduino 2.x）。
// 其它 env 里这个文件是空的 TU：不引入依赖、也不会因为缺 `<NimBLEDevice.h>` 编不过。
//
// 契约（与 `android/amap-bridge/NavLink.kt` 逐项对齐）：
//   广播名   `206dash-nav`
//   服务     `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
//   写特征   `6e400002-b5a3-f393-e0a9-e50e24dcca9e`（手机 write-no-response）
//   载荷     `lib/dashcore/nav_proto.h` 的 15~39 字节帧，可粘包/半包
//           ⇒ 一律喂 `nav_source_push()`，**不要**假设一次 notify 就是一帧
//
// ★ 为什么把"收字节"和"解析字节"分开：NimBLE 的回调跑在**它自己的任务**里，
//   而 `NavState` 是主循环在读（跟 OBD 那条链路同一个模式）。所以回调只做
//   "把字节塞进环形缓冲"，真正解析在 `nav_source_ble_tick()`（主循环上下文），
//   避免临界区里做 memmove/CRC。
// ============================================================

// 起 BLE（peripheral + 广播 + 收到字节入环）。返回 false = NimBLE 没起来。
bool nav_source_ble_start();

// 主循环每拍调：把环形缓冲里的字节喂给 nav_source，更新 NavState。
// 返回这一拍上账的帧数（0 = 没有新数据）。
size_t nav_source_ble_tick(uint32_t now_ms);

// 最近一次解析出来的状态（主循环渲染用；超时判据仍由 nav_state_tick 负责）。
const NavState& nav_source_ble_state();
