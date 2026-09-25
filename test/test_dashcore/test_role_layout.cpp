// ============================================================
// 角色 ↔ 屏幕的映射（`lib/dashcore/dash_role_layout.h`）—— 2026-09-25 新增
//
// 这一组回答的是车主那句原话背后的那一问：
//   "**副表怎么也给你刷成速度表了**" —— 新板（`COM8`，刷的是 `esp32s3-rgb-slave`）
//   上电显示的是**速度表**，而它该显示**转速表 + 水温**。
//
// 为什么值得一组用例（而不是"改一行算了"）：
//   · 这条对应关系以前**只活在两处旁证里** —— 开机标签的文案
//     （`dash_ui.cpp` 的 `MASTER (RIGHT)` / `SLAVE (LEFT)`）与 `ui_model.h` 的一句注释。
//     **没有判据、没有文档** ⇒ 谁把它写反了都不会有任何信号（屏上照样出画面，
//     只是出的是另一块表）—— 而"屏上出的是哪块表"正是车主唯一能看见的交付物。
//   · 它同时是**四个调用点**的口径（表盘弧/读数/表情/图片角色）：
//     分叉的症状是"转速表的弧配速度表的表情"，不报错、只是看着不对。
//
// ★ 判据的形状（三条，缺一不可）：
//     ① 末屏（今天**唯一可见**的那一屏，见 `kScreenTop` 的说明）**就是**本机角色
//        该显示的那一屏 —— 主板 ⇒ 速度表、从板 ⇒ 转速表；
//     ② **另一屏是互补的那一套**（两屏不许是同一套：那正是本单要修的 bug 的形态）；
//     ③ 表情/图片角色与表盘**共用同一个下标**（`faceIndexForScreen` 与
//        `themeIndexForScreen` 恒等）—— 两条判据 = 迟早分叉。
//
// ★ 本文件是**编译期角色**（`LINK_ROLE`）的宿主面：native 构建里 `LINK_ROLE` 默认 0
//   （= 从板，见 `link_role.h`）⇒ 下面那两条按角色的断言在宿主机上验的是**从板**那一侧，
//   而主板那一侧由 `#if LINK_ROLE == 1` 的编译期分支保证（同一个表达式，不可能分叉）。
//   同一条纪律在 `test_link_phy.cpp` / `test_link_app.cpp` 里已经在用。
// ============================================================
#include <unity.h>
#include <stdint.h>
#include <string.h>   // strcmp（名字那两条判据用）

#include "dash_role_layout.h"
#include "link_role.h"

// ============================================================
// 一、本机该显示哪一屏（`roleThemeIndex()`）—— 契约 §5 的角色口径
//   · `LINK_ROLE == 1` ⇒ 主板（MASTER (RIGHT)）⇒ `screens[1]` = 速度表 + 进气温度；
//   · `LINK_ROLE == 0` ⇒ 从板（SLAVE (LEFT)）  ⇒ `screens[0]` = 转速表 + 水温。
//   ★ 主题里那两个下标的含义（`ui_theme.h` 的 `theme_reset_to_defaults()`）：
//     0 = 外圈 Rpm + 内圈 Coolant；1 = 外圈 Speed + 内圈 Intake。
// ============================================================
void test_role_layout_theme_index_follows_role(void) {
#if LINK_ROLE == 1
  TEST_ASSERT_EQUAL_UINT8(1u, dashlayout::roleThemeIndex());   // 主板 = 速度表
#else
  TEST_ASSERT_EQUAL_UINT8(0u, dashlayout::roleThemeIndex());   // 从板 = 转速表
#endif
}

// ============================================================
// 二、**末屏 = 本机角色该显示的那一屏**（本单 B 的核心判据）
//   ★ 为什么判"末屏"：RGB 驱动是**单屏**版本 —— `g_left` 与 `g_right` 指向同一个
//     `lv_display_t`（第二块屏到货后才分叉），所以今天**两块玻璃上是同一份画面**，
//     而"哪一屏会被看见"由**创建顺序**决定（末屏压在上面）。
//     ⇒ "从板要显示转速表"这件事，落法就是"**从板的末屏 = 转速表那一套**"。
// ============================================================
void test_role_layout_last_screen_is_the_role_panel(void) {
  TEST_ASSERT_EQUAL_UINT8(dashlayout::roleThemeIndex(),
                          dashlayout::themeIndexForScreen(dashlayout::kScreenTop));
  TEST_ASSERT_EQUAL_UINT8(1u, dashlayout::kScreenTop);   // 末屏下标就是 1（两屏版）
  TEST_ASSERT_EQUAL_UINT8(2u, dashlayout::kScreenCount);
}

// ============================================================
// 三、另一屏是**互补**的那一套（反向判据：两屏不许是同一套）
//   ★ 这一条正是车主看到的那个 bug 的可执行形态：改之前两块屏都拿 `screens[s]`
//     ⇒ 末屏恒为 `screens[1]`（速度表），**从板也显示速度表**。
//     ⇒ 所以这里要求 `slots=[A|B]` 里 A ≠ B，而且末屏是角色那一块。
// ============================================================
void test_role_layout_other_screen_is_complementary(void) {
  const uint8_t a = dashlayout::themeIndexForScreen(0u);
  const uint8_t b = dashlayout::themeIndexForScreen(1u);
  TEST_ASSERT_NOT_EQUAL(a, b);                  // 两屏**必须**是不同的两套表
  TEST_ASSERT_TRUE(a < 2u && b < 2u);           // 且都在主题的两个槽里
  TEST_ASSERT_EQUAL_UINT8(1u, (uint8_t)(a + b)); // 0/1 各用一次（互补，没有重复）
#if LINK_ROLE == 1
  TEST_ASSERT_EQUAL_UINT8(1u, b);               // 主板：末屏 = 速度表
  TEST_ASSERT_EQUAL_UINT8(0u, a);
#else
  TEST_ASSERT_EQUAL_UINT8(0u, b);               // 从板：末屏 = 转速表（本单要的）
  TEST_ASSERT_EQUAL_UINT8(1u, a);
#endif
}

// ============================================================
// 四、表情/图片角色与表盘**共用同一个下标**
//   ★ 为什么必须恒等：`kFaceRoleId[screen][slot]`（`face_stages.h`）是**按屏分**的
//     —— 左屏（转速表）有红区/高转、右屏（速度表）有超速/市区。而 `ScreenUi::idx`
//     就是那个下标（`dash_ui.cpp` 里 `g_ui[s].idx = faceIndexForScreen(s)`）。
//     表盘走一套、表情走另一套 ⇒ "转速表的弧配速度表的表情"（不报错、只是看着不对）。
// ============================================================
void test_role_layout_face_and_theme_share_one_index(void) {
  for (uint8_t s = 0; s < dashlayout::kScreenCount; ++s) {
    TEST_ASSERT_EQUAL_UINT8(dashlayout::themeIndexForScreen(s),
                            dashlayout::faceIndexForScreen(s));
  }
}

// ============================================================
// 五、主弧那一位（日志/自证用）：0 = 速度表、1 = 转速表
//   ★ 与 `ui_theme.h` 的 `ArcKind{Speed, Rpm, Coolant, Intake}` 的前两位同一口径；
//     这一格是"屏上跑的是哪块表"在**串口日志**里能自证的那个数（`layout:` 那一行
//     与 `206 dash ok … | role=… last=…` 用的是同一套名字）。
// ============================================================
void test_role_layout_primary_kind_code(void) {
#if LINK_ROLE == 1
  TEST_ASSERT_EQUAL_UINT8(0u, dashlayout::primaryKindCodeForScreen(1u));   // Speed
#else
  TEST_ASSERT_EQUAL_UINT8(1u, dashlayout::primaryKindCodeForScreen(1u));   // Rpm
#endif
  // 两屏必须是**不同的**主表（互补那一套的另一个说法）
  TEST_ASSERT_NOT_EQUAL(dashlayout::primaryKindCodeForScreen(0u),
                        dashlayout::primaryKindCodeForScreen(1u));
}

// ============================================================
// 六、名字（纯 ASCII —— 本构建只使能 Montserrat）
//   ★ 这两条名字是给人看日志用的：`LEFT/rpm+coolant` / `RIGHT/speed+intake`。
//     它们必须**互不相同**且非空（否则日志上两屏就分不出来了）。
// ============================================================
void test_role_layout_names_are_distinct_ascii(void) {
  const char* a = dashlayout::panelNameForScreen(0u);
  const char* b = dashlayout::panelNameForScreen(1u);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_TRUE(a[0] != '\0' && b[0] != '\0');
  TEST_ASSERT_TRUE(strcmp(a, b) != 0);
  // 纯 ASCII：每一个字节都必须 < 0x80（中文字面量在这个构建里画不出来，
  // 日志这条路上还会踩 GBK 控制台 —— README 那条纪律）。
  for (const char* p = a; *p; ++p) TEST_ASSERT_TRUE((uint8_t)*p < 0x80u);
  for (const char* p = b; *p; ++p) TEST_ASSERT_TRUE((uint8_t)*p < 0x80u);
  // 角色名也必须是纯 ASCII（它与开机那行 `role=MASTER/SLAVE` 同一口径）
  const char* r = dashlayout::roleName();
  TEST_ASSERT_NOT_NULL(r);
  for (const char* p = r; *p; ++p) TEST_ASSERT_TRUE((uint8_t)*p < 0x80u);
#if LINK_ROLE == 1
  TEST_ASSERT_EQUAL_STRING("MASTER", r);
#else
  TEST_ASSERT_EQUAL_STRING("SLAVE", r);
#endif
}

void register_role_layout_tests(void) {
  RUN_TEST(test_role_layout_theme_index_follows_role);
  RUN_TEST(test_role_layout_last_screen_is_the_role_panel);
  RUN_TEST(test_role_layout_other_screen_is_complementary);
  RUN_TEST(test_role_layout_face_and_theme_share_one_index);
  RUN_TEST(test_role_layout_primary_kind_code);
  RUN_TEST(test_role_layout_names_are_distinct_ascii);
}
