#pragma once
#include <stdint.h>

#include "link_role.h"   // LINK_ROLE / kRoleMaster / kRoleSlave（§5 的**唯一**权威）

// ============================================================
// ★★ 角色 ↔ 屏幕的映射（2026-09-25 新增）—— "**哪块板显示哪一屏**"的唯一出处
//
// 车主原话（本单的需求文档）："**副表怎么也给你刷成速度表了**" ——
// 新板（`COM8`，刷的是 `esp32s3-rgb-slave`）上电后显示的是**速度表**，
// 而它该显示的是**转速表（左屏 + 水温副表）**。
//
// ------------------------------------------------------------
// 一、主题里那两个屏是什么（**不是**"两块玻璃"，是**两套表盘**）
// ------------------------------------------------------------
//   · `Theme::screens[0]` = **左屏**：外圈**转速**弧 + 内圈**水温**副表；
//   · `Theme::screens[1]` = **右屏**：外圈**车速**弧 + 内圈**进气温度**副表。
//   （`lib/themetool/ui_theme.h` 的 `theme_reset_to_defaults()` 就是这两条；
//     `dash_ui.cpp` 里所有的表层字段都读 `kScreens[s]`，即 `g_theme.screens[s]`。）
//
// ------------------------------------------------------------
// 二、契约里的角色（§5）：主板 = **右**，从板 = **左**
// ------------------------------------------------------------
//   · `LINK_ROLE == 1` ⇒ **主板**（`MASTER (RIGHT)`）⇒ 该显示 **右屏（速度表）**；
//   · `LINK_ROLE == 0` ⇒ **从板**（`SLAVE (LEFT)`）  ⇒ 该显示 **左屏（转速表 + 水温）**。
//   ★ 这条对应关系**本来只存在于两处旁证里**，没有一行代码/文档把它写成判据：
//     ① 开机标签的文案（`dash_ui.cpp` 的 `build_role_label()`：
//        `MASTER (RIGHT)` / `SLAVE (LEFT)`）；
//     ② 车道文档里"左屏 = 转速表"那句（`ui_model.h` 的 `ArcDashView` 注释）。
//     ⇒ 本单把它**写进代码（本文件）+ 写进文档（`ARCHITECTURE.md` 显示约定）**，
//       因为"哪块板显示哪一屏"是**产品口径**，不该只靠文案暗示。
//
// ------------------------------------------------------------
// 三、为什么映射是"下标对调"而不是"换一块屏"
// ------------------------------------------------------------
//   本轮的 RGB 驱动是**单屏**版本：`g_left` 与 `g_right` 指向**同一个**
//   `lv_display_t`（第二块屏到货后才分叉，见 `src/dash_display_rgb.cpp` 文件头
//   "双屏出路"）。所以今天**两块玻璃上跑的其实是同一份 LVGL 画面**，
//   而"哪一屏会被真正看见"由**创建顺序**决定 —— `lv_obj_create(nullptr)` 建的屏
//   谁最后建、谁就在这个 display 上（实测：右屏压住左屏 ⇒ 车主看到的就是速度表）。
//
//   ⇒ 于是"从板要显示左屏"这件事**只**需要一处改动：让**从板**把
//     `screens[0]`（转速表那套）放进**最后建**的那一屏。
//     ★ 这一次改动**没有**碰：`LINK_ROLE` 的语义（§5 一行未动）、链路协议（§5/§3/§2）、
//       面板驱动、`g_left/g_right` 的指向、两个 `lv_display_t` 的创建顺序。
//       `dash_ui.cpp` 里只把"第 s 屏用哪套表盘/哪套表情/哪套图片角色"换成走本文件的
//       两个映射函数 —— 那是**同一个角色口径**在四个调用点上的落法，不是新判据。
//
// ------------------------------------------------------------
// 四、边界（写清楚，免得被读成"双屏已经成了"）
// ------------------------------------------------------------
//   · 每一块板上**那一屏是"角色该显示的那一屏"**：
//       主板末屏 = `screens[1]`（速度表），从板末屏 = `screens[0]`（转速表）；
//     而**另一屏**（没被显示的那一屏）跑的是**互补**的那一套 —— 它今天落在
//     同一个 display 上、被压住看不见，第二块屏到货后**自然就是对的**
//     （两块屏各自要显示什么，从今天起由本文件说了算）。
//   · 表情（`Face`）与图片资源（`ImageRole`）也是**按屏分**的
//     （`face_stages.h` 的 `kFaceRoleId[screen][slot]`）⇒ 它们必须跟着同一个映射走，
//     否则会出现"转速表的弧配速度表的表情"（那种错不报错、只是看着不对）。
//     `dash_ui.cpp` 里 `ScreenUi::idx`（= 图片按屏取的那个下标）也走这里。
// ============================================================

namespace dashlayout {

// 屏幕数：两屏（左/右）。★ 与 `ui_theme.h` 的 `Theme::screens[2]` 是同一个数。
static const uint8_t kScreenCount = 2u;

// 设备上的两块屏：末屏（今天唯一可见的那一屏）。
// ★ 为什么叫"末屏"而不是"右屏"：名字要说的是**为什么它可见**（最后建 = 压在上面），
//   而不是它装在哪 —— 装在哪由角色决定（下面 `roleThemeIndex()`）。
static const uint8_t kScreenTop = 1u;

// 本机该显示**主题里的哪一屏**：主板 ⇒ 1（速度表），从板 ⇒ 0（转速表）。
inline uint8_t roleThemeIndex() {
#if LINK_ROLE == 1
  return 1u;   // 主板（右）：速度表 + 进气温度
#else
  return 0u;   // 从板（左）：转速表 + 水温
#endif
}

// 第 s 屏（`g_ui[s]` / `g_screens[s]`）该用主题里的哪一套表盘？
// ★ 判据：**末屏 = 本机角色该显示的那一屏**；另一屏 = 互补的那一套（今天被压住，
//   第二块屏到货后自然成立）。
inline uint8_t themeIndexForScreen(uint8_t s) {
  return (s == kScreenTop) ? roleThemeIndex() : (uint8_t)(1u - roleThemeIndex());
}

// 第 s 屏该用哪一套**表情/图片角色**（`face_stages.h` 的 `kFaceRoleId[screen][…]`）。
// ★ 它与表盘**必须是同一个下标**：表情是"看这一屏那块表"给的
//   （`ui_model.h`：左屏只看转速、右屏只看车速）⇒ 这里直接复用上面那一个函数，
//   不另写第二条判据（两条判据 = 迟早会分叉）。
inline uint8_t faceIndexForScreen(uint8_t s) { return themeIndexForScreen(s); }

// 第 s 屏该显示的那块表**主弧是哪一种**？（日志/自证用；`ArcKind` 的两个值在这里
// 只作为**编号**出现，避免本文件去 include LVGL/主题头 —— 它是宿主机也要编的纯头。）
//   0 = 速度表（Speed），1 = 转速表（Rpm）—— 与 `ArcKind` 的枚举顺序一致。
inline uint8_t primaryKindCodeForScreen(uint8_t s) {
  return (themeIndexForScreen(s) == 1u) ? 0u : 1u;
}

// 纯 ASCII 的名字（日志用；本构建只使能 Montserrat，中文一个字形都画不出来）。
inline const char* panelNameForScreen(uint8_t s) {
  return (themeIndexForScreen(s) == 1u) ? "RIGHT/speed+intake" : "LEFT/rpm+coolant";
}
inline const char* roleName() {
#if LINK_ROLE == 1
  return "MASTER";
#else
  return "SLAVE";
#endif
}

}  // namespace dashlayout
