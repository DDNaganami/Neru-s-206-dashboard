// ============================================================
// 面板健康守护（PanelGuard）—— 2026-09-24 新增
//
// 这一组回答的是"**仪表盘黑屏这件事，软件这一侧能不能自己发现、自己修**"
// （业主原话："上实车的时候可不能这样，这毕竟是仪表盘，要常亮的"）。
// 每一条都对着 `panel_guard.h` 文件头里的一个判据：
//
//   ① 影子与**回读一致** ⇒ **一次都不写**（不许"每 2 秒无条件重写一遍"）；
//   ② 不一致 ⇒ **只按影子重写一次**、计数 +1、日志原文可对账；
//   ③ 回读**失败**（无应答）**不算**不一致（不重写、单独计数）；
//   ④ 背光占空被改 ⇒ 重设；
//   ⑤ 检查**每 2 秒一次**（不是每个 tick）—— "约 2 秒内发现"这条判据的时间账；
//   ⑥ 连续异常才请求**一次**自动重初始化（不是周期性重初始化 ⇒ 不许闪屏）。
//
// ★ 为什么用**假端口 + 假时钟**：宿主机上没有 TCA9554、也没有 LEDC，
//   而"写了几次、写的什么值"只有把注入回调收到的东西**记下来**才能逐条断言。
//   时钟同理 —— 真 `millis()` 在宿主机上不可控（与 test_buzzer_exio.cpp 同一条纪律）。
// ============================================================
#include <unity.h>
#include <string.h>
#include <stdint.h>

#include "panel_guard.h"

namespace {

// ---- 假硬件 + 假时钟 ----
struct Harness {
  uint8_t  reg   = 0x05;    // 扩展器**真实**的输出寄存器（初始化后的稳态：RST/CS 高）
  uint8_t  shadow = 0x05;   // 我们要求的（守护的判据）
  uint32_t duty  = 512;     // LEDC 当前占空比
  uint32_t want  = 512;     // 我们要求的
  bool     ack   = true;    // 回读有没有应答
  bool     exio_write_effective = true;  // 重写之后"真实寄存器"跟不跟着变（注入故障时置 false）

  int      rd_n = 0;        // 读过几次
  int      wr_n = 0;        // 重写扩展器几次
  int      bl_wr_n = 0;     // 重设背光几次
  int      reinit_n = 0;    // 重初始化被调用几次
  uint8_t  last_wr = 0;     // 最近一次重写的字节

  int      log_n = 0;
  uint8_t  log_which[16] = {};
  uint32_t log_a[16] = {};
  uint32_t log_b[16] = {};
};

Harness H;

bool hReadExio(uint8_t* out, void* ctx) {
  Harness* h = (Harness*)ctx;
  ++h->rd_n;
  if (!h->ack) return false;
  *out = h->reg;
  return true;
}
void hWriteExio(uint8_t shadow, void* ctx) {
  Harness* h = (Harness*)ctx;
  ++h->wr_n;
  h->last_wr = shadow;
  if (h->exio_write_effective) h->reg = shadow;
}
uint32_t hReadDuty(void* ctx) { return ((Harness*)ctx)->duty; }
void hWriteDuty(uint32_t d, void* ctx) {
  Harness* h = (Harness*)ctx;
  ++h->bl_wr_n;
  h->duty = d;
}
void hReinit(void* ctx) { ++((Harness*)ctx)->reinit_n; }
void hLog(uint8_t which, uint32_t a, uint32_t b, void* ctx) {
  Harness* h = (Harness*)ctx;
  if (h->log_n < 16) {
    h->log_which[h->log_n] = which;
    h->log_a[h->log_n] = a;
    h->log_b[h->log_n] = b;
  }
  ++h->log_n;
}

PanelGuard makeGuard() {
  return PanelGuard(hReadExio, hWriteExio, hReadDuty, hWriteDuty, hReinit, hLog, &H);
}

// ---- 用例自己的时钟 ----
uint32_t now_ms = 0;

// 从当前时刻**逐毫秒**推进到 `end_ms`（模拟主循环每轮调一次 tick）。
// ★ 逐毫秒而不是一步跳过去：这样"到点检查"是被 tick 抓到的，而不是被一次大跳越过。
void runTo(PanelGuard& g, uint32_t end_ms) {
  while (now_ms <= end_ms) {
    g.tick(now_ms);
    ++now_ms;
  }
  --now_ms;
}

void resetHarness() {
  H = Harness{};
  now_ms = 0;
}

}  // namespace

// ============================================================
// 一、一致 ⇒ 一次都不写（"每 2 秒无条件重写"是错的）
// ============================================================
void test_panelguard_match_writes_nothing(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(H.shadow);
  g.setBacklightDuty(H.want);
  g.begin(now_ms);

  runTo(g, 10000);          // 10 秒 = 5 个检查周期
  TEST_ASSERT_EQUAL_UINT32(5u, g.checks());
  TEST_ASSERT_EQUAL_UINT32(5u, g.rdOk());
  TEST_ASSERT_EQUAL_UINT32(0u, g.rdErr());
  TEST_ASSERT_EQUAL_UINT32(0u, g.fixExio());
  TEST_ASSERT_EQUAL_UINT32(0u, g.fixBl());
  TEST_ASSERT_EQUAL_UINT32(0u, g.anomalies());
  TEST_ASSERT_EQUAL_UINT32(0u, g.reinitCount());
  TEST_ASSERT_EQUAL_INT(0, H.wr_n);         // ★ 关键：总线上**一个字节都没写**
  TEST_ASSERT_EQUAL_INT(0, H.bl_wr_n);
  TEST_ASSERT_EQUAL_INT(0, H.log_n);        // 一切正常 ⇒ 一行日志都没有（不刷屏）
  TEST_ASSERT_EQUAL_UINT8(0u, g.streak());
}

// ============================================================
// 二、不一致 ⇒ **只按影子重写一次** + 计数 +1 + 日志原文可对账
//    （这就是"LCD_RST/LCD_CS 被意外改写"那条黑屏路径的修复动作）
// ============================================================
void test_panelguard_mismatch_rewrites_shadow_once(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(0x05u);
  // ★ 背光这一路也要声明期望值：不声明的话守护会拿"0"去比真实的 512，
  //   于是每个周期都多出一次"背光不一致"（这个坑当场踩过一次）——
  //   本用例只谈扩展器那一路。
  g.setBacklightDuty(H.want);
  g.begin(now_ms);

  // 注入：真实寄存器被写成了 0x04（EXIO1 = LCD_RST 那一位掉了）—— 一次，只注入一次。
  H.reg = 0x04u;

  runTo(g, 2000);           // 正好一个周期 ⇒ 检查一次
  TEST_ASSERT_EQUAL_UINT32(1u, g.checks());
  TEST_ASSERT_EQUAL_UINT32(1u, g.rdOk());
  TEST_ASSERT_EQUAL_UINT32(1u, g.fixExio());     // 修了一次
  TEST_ASSERT_EQUAL_UINT32(1u, g.anomalies());
  TEST_ASSERT_EQUAL_INT(1, H.wr_n);              // ★ 只写一次（不是每个 tick 都写）
  TEST_ASSERT_EQUAL_UINT8(0x05u, H.last_wr);     // ★ 写的是**影子**，不是读回来的 0x04
  TEST_ASSERT_EQUAL_UINT8(0x05u, H.reg);         // 修复后真实寄存器回到影子
  // ★ 连续异常计数在这一拍已经是 **1**（不是 0）：这一拍的异常才刚被记上，
  //   而"连续够不够 3 次"要等后面两拍才判（见用例六）。
  TEST_ASSERT_EQUAL_UINT8(1u, g.streak());
  // 日志：`panelguard: exio rd=0x04 shadow=0x05 -> rewritten` 的那两个数
  TEST_ASSERT_EQUAL_INT(1, H.log_n);
  TEST_ASSERT_EQUAL_UINT8(kPanelGuardLogExio, H.log_which[0]);
  TEST_ASSERT_EQUAL_UINT32(0x04u, H.log_a[0]);
  TEST_ASSERT_EQUAL_UINT32(0x05u, H.log_b[0]);

  // 后面的周期恢复一致 ⇒ 不再写（"修好就停手"）
  runTo(g, 10000);
  TEST_ASSERT_EQUAL_INT(1, H.wr_n);
  TEST_ASSERT_EQUAL_UINT32(1u, g.fixExio());
  TEST_ASSERT_EQUAL_UINT32(5u, g.checks());      // 2s/4s/6s/8s/10s —— 含修好的那一次
}

// ============================================================
// 三、回读**失败**不算不一致（无应答与"真被改了"是两件事）
// ============================================================
void test_panelguard_read_error_is_not_mismatch(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(0x05u);
  g.setBacklightDuty(H.want);   // 只谈"回读失败"这一路（背光那一路要摆平）
  g.begin(now_ms);
  H.ack = false;            // 总线无应答

  runTo(g, 4000);
  TEST_ASSERT_EQUAL_UINT32(2u, g.checks());
  TEST_ASSERT_EQUAL_UINT32(2u, g.rdErr());
  TEST_ASSERT_EQUAL_UINT32(0u, g.rdOk());
  TEST_ASSERT_EQUAL_UINT32(0u, g.fixExio());     // ★ 一次都不重写
  TEST_ASSERT_EQUAL_UINT32(0u, g.anomalies());
  TEST_ASSERT_EQUAL_INT(0, H.wr_n);
  TEST_ASSERT_EQUAL_INT(2, H.log_n);
  TEST_ASSERT_EQUAL_UINT8(kPanelGuardLogRdErr, H.log_which[0]);
}

// ============================================================
// 四、背光占空被改 ⇒ 重设（且只重设一次）
// ============================================================
void test_panelguard_backlight_rewrite(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(H.shadow);
  g.setBacklightDuty(512u);
  g.begin(now_ms);

  H.duty = 0u;              // 背光被谁改成全灭（"玻璃黑了"的另一条路径）
  runTo(g, 2000);
  TEST_ASSERT_EQUAL_UINT32(1u, g.fixBl());
  TEST_ASSERT_EQUAL_UINT32(1u, g.anomalies());
  TEST_ASSERT_EQUAL_INT(1, H.bl_wr_n);
  TEST_ASSERT_EQUAL_UINT32(512u, H.duty);
  TEST_ASSERT_EQUAL_UINT8(kPanelGuardLogBl, H.log_which[0]);
  TEST_ASSERT_EQUAL_UINT32(0u, H.log_a[0]);
  TEST_ASSERT_EQUAL_UINT32(512u, H.log_b[0]);

  runTo(g, 6000);
  TEST_ASSERT_EQUAL_INT(1, H.bl_wr_n);           // 恢复后不再写
  TEST_ASSERT_EQUAL_UINT32(1u, g.fixBl());
}

// ============================================================
// 五、检查频率：**每 2 秒一次**（"约 2 秒内发现"这条判据的时间账）
//    ★ 尤其要钉住"开机后头 2 秒不检查"与"每个 tick 不检查"
// ============================================================
void test_panelguard_period_is_two_seconds(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(H.shadow);
  g.begin(now_ms);

  // 前 1999ms：一个 tick 都不许触发检查
  runTo(g, 1999);
  TEST_ASSERT_EQUAL_UINT32(0u, g.checks());
  TEST_ASSERT_EQUAL_INT(0, H.rd_n);

  // 第 2000ms：正好一次
  runTo(g, 2000);
  TEST_ASSERT_EQUAL_UINT32(1u, g.checks());
  TEST_ASSERT_EQUAL_INT(1, H.rd_n);

  // 到 2001ms 也只有一次
  runTo(g, 2001);
  TEST_ASSERT_EQUAL_UINT32(1u, g.checks());

  // 4000ms：两次
  runTo(g, 4000);
  TEST_ASSERT_EQUAL_UINT32(2u, g.checks());
}

// ============================================================
// 六、连续异常 N 次 ⇒ 请求**一次**自动重初始化（不是周期性重初始化）
// ============================================================
void test_panelguard_auto_reinit_after_streak(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(0x05u);
  g.setBacklightDuty(H.want);   // 本用例只谈扩展器那一路（见上一条的说明）
  g.begin(now_ms);

  H.reg = 0x04u;
  H.exio_write_effective = false;   // 注入"改回来了但还是不对"的持续故障（写不进去）

  runTo(g, 2000);
  TEST_ASSERT_EQUAL_UINT8(1u, g.streak());
  TEST_ASSERT_EQUAL_UINT32(0u, g.reinitCount());
  TEST_ASSERT_FALSE(g.takeReinitRequest());

  runTo(g, 4000);
  TEST_ASSERT_EQUAL_UINT8(2u, g.streak());
  TEST_ASSERT_EQUAL_UINT32(0u, g.reinitCount());

  runTo(g, 6000);
  // ★ 第 3 次连续异常 ⇒ 请求自动重初始化**一次**；请求发出后计数清零
  //   （所以这一拍读到的是 0，而"攒够了几次"由下面那行日志的 `xN` 记着）。
  TEST_ASSERT_EQUAL_UINT8(0u, g.streak());
  TEST_ASSERT_EQUAL_UINT32(1u, g.reinitCount());
  // ★ 请求是**边沿**语义：读一次就清（主循环据此在安全时刻执行重初始化）
  TEST_ASSERT_TRUE(g.takeReinitRequest());
  TEST_ASSERT_FALSE(g.takeReinitRequest());
  TEST_ASSERT_EQUAL_UINT8(kPanelGuardLogAuto, H.log_which[H.log_n - 1]);
  TEST_ASSERT_EQUAL_UINT32(3u, H.log_a[H.log_n - 1]);

  // 冷却期内不再请求（哪怕连续异常又攒够了）—— 不许变成"周期性重跑 41 步"
  runTo(g, 30000);
  TEST_ASSERT_EQUAL_UINT32(1u, g.reinitCount());
  TEST_ASSERT_FALSE(g.takeReinitRequest());

  // 显示驱动报"重初始化完了" ⇒ 计数清零、重新起算
  g.noteReinit(now_ms);
  TEST_ASSERT_EQUAL_UINT8(0u, g.streak());
  TEST_ASSERT_FALSE(g.takeReinitRequest());

  // 冷却期过（60 秒）之后仍然异常 ⇒ 才允许再赌一次
  runTo(g, now_ms + 61000u);
  TEST_ASSERT_EQUAL_UINT32(2u, g.reinitCount());
  TEST_ASSERT_TRUE(g.takeReinitRequest());
}

// ============================================================
// 七、没注入 reinit 回调时**永不请求**（预览/其余构建就算误编进来也不会动面板）
// ============================================================
void test_panelguard_no_reinit_callback_never_requests(void) {
  resetHarness();
  PanelGuard g(hReadExio, hWriteExio, hReadDuty, hWriteDuty, nullptr, hLog, &H);
  g.setShadow(0x05u);
  g.begin(now_ms);
  H.reg = 0x04u;
  H.exio_write_effective = false;

  runTo(g, 30000);
  TEST_ASSERT_EQUAL_UINT32(0u, g.reinitCount());
  TEST_ASSERT_FALSE(g.takeReinitRequest());
  TEST_ASSERT_EQUAL_INT(0, H.reinit_n);
}

// ============================================================
// 八、没 begin 过 ⇒ tick 什么都不做（"忘了 begin"不会变成每轮一次 I2C 读）
// ============================================================
void test_panelguard_tick_before_begin_is_noop(void) {
  resetHarness();
  PanelGuard g = makeGuard();
  g.setShadow(H.shadow);
  runTo(g, 10000);
  TEST_ASSERT_EQUAL_UINT32(0u, g.checks());
  TEST_ASSERT_EQUAL_INT(0, H.rd_n);
}

// ============================================================
// 九、纯判据函数本身（注入回调之外的那两条规则）
// ============================================================
void test_panelguard_pure_predicates(void) {
  TEST_ASSERT_TRUE(panel_guard_exio_matches(true, 0x05u, 0x05u));
  TEST_ASSERT_FALSE(panel_guard_exio_matches(true, 0x04u, 0x05u));
  // ★ 读失败 ⇒ 视为"匹配"（= 不重写），见 panel_guard_exio_matches 的说明
  TEST_ASSERT_TRUE(panel_guard_exio_matches(false, 0x00u, 0x05u));
  TEST_ASSERT_TRUE(panel_guard_backlight_matches(512u, 512u));
  TEST_ASSERT_FALSE(panel_guard_backlight_matches(0u, 512u));
}

void register_panel_guard_tests(void) {
  RUN_TEST(test_panelguard_match_writes_nothing);
  RUN_TEST(test_panelguard_mismatch_rewrites_shadow_once);
  RUN_TEST(test_panelguard_read_error_is_not_mismatch);
  RUN_TEST(test_panelguard_backlight_rewrite);
  RUN_TEST(test_panelguard_period_is_two_seconds);
  RUN_TEST(test_panelguard_auto_reinit_after_streak);
  RUN_TEST(test_panelguard_no_reinit_callback_never_requests);
  RUN_TEST(test_panelguard_tick_before_begin_is_noop);
  RUN_TEST(test_panelguard_pure_predicates);
}
