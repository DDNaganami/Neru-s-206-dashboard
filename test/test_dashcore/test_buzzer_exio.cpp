// ============================================================
// 真机蜂鸣器驱动（TCA9554 的 EXIO8，有源）—— 2026-09-24 新增
//
// 这一组回答的是"**接上去之后会不会把屏搞坏**"这一半（另一半是车主的耳朵：
// 有源/无源的实测在 `docs/RGB-PANEL-2.8C.md` §13.6）。所以每一条都对着
// `buzzer_exio.h` 文件头那三条硬约束中的一条：
//
//   ① 开/关只动目标位、其余位按原值写回（**掩码正确**）
//      ⇒ 那颗芯片的输出寄存器里同时挂着 LCD_RST/LCD_CS，写错一位 = 一次面板复位；
//   ② `Long` 降级成 **3 短哔**（不许长鸣）；
//   ③ 单次哔的 `ms` 上限被夹到 ≤ 300ms（**每一相**都夹，不是只看标称值）；
//   ④ 静音时不写寄存器（`beep()` 压根不该被调到；真调到了也不许动总线）；
//   ⑤ 到时刻自动关（**假时钟**逐格推进，不依赖真 `millis()`）。
//
// ★ 为什么用**假时钟 + 假端口**而不是真 `millis()`/真 I2C：
//   `buzzer_exio.h` 把"输出"与"时钟"都做成了注入点，正是为了这里 ——
//   宿主机上没有 TCA9554，也没有可控的 `millis()`；而"哪一位变了"这件事
//   只有把写下去的字节**记下来**才能逐位断言。
// ============================================================
#include <unity.h>
#include <string.h>
#include <stdint.h>

#include "buzzer_exio.h"
#include "system_status.h"   // kTrustBeepMs：那一声轻提示的标称时长（也要 ≤ 300ms）

namespace {

// ---- 假时钟：用例自己推进；`BuzzerExio` 只透过注入的函数读它 ----
uint32_t g_now_ms = 0;
uint32_t fakeNow() { return g_now_ms; }

// ---- 假端口：记录**每一次**开/关（含电平），供"只剩目标位在动"逐位核对 ----
const int kMaxEvents = 32;
struct ExioEvent {
  bool    on;
  uint8_t reg;
};
ExioEvent g_ev[kMaxEvents];
int      g_ev_n = 0;

// RST(EXIO1)/CS(EXIO3) 在初始化之后是**高**，蜂鸣器(EXIO8) 是**低**
// —— 这就是真机上 `g_exio_out` 稳态那个 0x05（见 dash_display_rgb.cpp 的自检回读）。
uint8_t g_reg = 0x05;

void fakeSet(bool on, void* ctx) {
  (void)ctx;
  if (on) g_reg |= (uint8_t)(1u << 7);
  else    g_reg &= (uint8_t)~(1u << 7);
  if (g_ev_n < kMaxEvents) {
    g_ev[g_ev_n].on = on;
    g_ev[g_ev_n].reg = g_reg;
    ++g_ev_n;
  }
}

void resetHarness() {
  g_now_ms = 0;
  g_ev_n = 0;
  g_reg = 0x05;
}

// 把事件表清空（每个"要数几次开关"的小节开头调一次，读起来更清楚）。
void clearEvents() { g_ev_n = 0; }

// 这一组事件的电平序列（true = 响），只看"开/关"的次序。
int transitions(bool* out, int cap) {
  int n = 0;
  for (int i = 0; i < g_ev_n && n < cap; ++i) out[n++] = g_ev[i].on;
  return n;
}

// 把一段序列**逐毫秒**推到 `end_ms`（模拟主循环每轮都来一次）。
// ★ 每毫秒推一次而不是只跳一格：这样"到点关"这件事是被 tick 抓到的，
//   而不是被某一次大步长跳过去的（后者会掩盖"没关"的 bug）。
void runTo(BuzzerExio& b, uint32_t end_ms) {
  while (g_now_ms <= end_ms) {
    b.tick();
    ++g_now_ms;
  }
  --g_now_ms;   // 让调用方能算准"现在停在哪一刻"
}

// 逐毫秒推到"这一拍自然走完"为止 —— **不重调 `beep()`**。
// ★ 为什么不能写成 `while (active) tick();`：`tick()` 会**先**推进时钟、
//   序列又在同一微秒里走完 ⇒ 循环体跑完时 `active_` 已经是 false，
//   于是"完成"的那一次 tick 在日志/事件表里就不算数了（少记一次）。
//   ⇒ 这里用"见过 active"这个闩锁来判收尾，并且**不在收尾后再 tick**。
void runSequence(BuzzerExio& b) {
  bool seen = false;
  for (int guard = 0; guard < 20000; ++guard) {
    if (b.sequenceActive()) seen = true;
    else if (seen) return;     // 走完了：立刻停手（再 tick 就会开始新的一拍）
    b.tick();
    ++g_now_ms;
  }
  TEST_FAIL_MESSAGE("buzzer sequence did not finish (guard tripped)");
}

}  // namespace

// ------------------------------------------------------------
// ① 掩码：开/关只动 EXIO8 那一位，RST/CS 一个字节都不许动
// ------------------------------------------------------------
void test_buzzer_exio_mask_only_target_bit(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);      // 上电静音：RST/CS 照旧高、EXIO8 低

  b.beep(BeepPattern::Short, 120);
  runTo(b, 400);                            // 整拍跑完（含收尾）

  TEST_ASSERT_TRUE(g_ev_n >= 2);            // 至少有"开"与"关"两次
  // ★ 核心断言：**每一次**写回之后，低 5 位（EXIO0..EXIO4，含 RST=bit0/CS=bit2）
  //   必须还是初始那个 0b00101 —— 也就是"只有 bit7 在动"。
  for (int i = 0; i < g_ev_n; ++i) {
    TEST_ASSERT_EQUAL_HEX8(0x05, (uint8_t)(g_ev[i].reg & 0x1Fu));
  }
  // 关的那一次：只有 bit7 被清 ⇒ 回到 0x05（既没有长鸣，也没有碰到别的位）
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
  TEST_ASSERT_FALSE(b.pulseActive());
  TEST_ASSERT_FALSE(b.sequenceActive());
}

// ------------------------------------------------------------
// ② `Long`（长鸣）在真机上降级成 **3 短哔**，且每一哔都 ≤ 300ms
// ------------------------------------------------------------
void test_buzzer_exio_long_degrades_to_three_short(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();

  const BeepPulses ps = pulsesFor(BeepPattern::Long, 120);
  TEST_ASSERT_EQUAL_UINT8(3, ps.n);         // ★ 3 短哔（不是 1 声长鸣）
  for (uint8_t i = 0; i < ps.n; ++i) {
    TEST_ASSERT_TRUE(ps.on_ms[i] > 0u);
    TEST_ASSERT_TRUE(ps.on_ms[i] <= kMaxPulseMs);
  }

  b.beep(BeepPattern::Long, 120);
  TEST_ASSERT_EQUAL_UINT8(3, b.pulseCount());

  // 电平序列必须是 开/关/开/关/开/关（3 声，且每声后面都真的关了）
  runSequence(b);
  TEST_ASSERT_FALSE(b.sequenceActive());

  bool lv[16];
  const int n = transitions(lv, 16);
  TEST_ASSERT_EQUAL_INT(6, n);
  TEST_ASSERT_TRUE(lv[0]); TEST_ASSERT_FALSE(lv[1]);
  TEST_ASSERT_TRUE(lv[2]); TEST_ASSERT_FALSE(lv[3]);
  TEST_ASSERT_TRUE(lv[4]); TEST_ASSERT_FALSE(lv[5]);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);      // 收尾仍是静音
}

// ★ 反面：`Short` 只响 1 声、`Triple` 响 3 声 —— 免得"全都降级成 3 声"
void test_buzzer_exio_pulse_counts(void) {
  TEST_ASSERT_EQUAL_UINT8(0, pulsesFor(BeepPattern::Silent, 120).n);
  TEST_ASSERT_EQUAL_UINT8(1, pulsesFor(BeepPattern::Short, 120).n);
  TEST_ASSERT_EQUAL_UINT8(3, pulsesFor(BeepPattern::Triple, 120).n);
  TEST_ASSERT_EQUAL_UINT8(4, pulsesFor(BeepPattern::Urgent, 120).n);
  TEST_ASSERT_EQUAL_UINT8(3, pulsesFor(BeepPattern::Long, 120).n);   // ← 降级
}

// ------------------------------------------------------------
// ③ `ms` 上限被夹到 ≤ 300ms —— 标称值、以及**每一相**都要夹
// ------------------------------------------------------------
void test_buzzer_exio_clamps_ms_to_300(void) {
  // ★ 硬约束的正身：单次哔 ≤ 300ms（`ARCHITECTURE.md` §4 第 2 条）
  TEST_ASSERT_EQUAL_UINT32(300u, kMaxPulseMs);
  TEST_ASSERT_TRUE(kTrustBeepMs <= kMaxPulseMs);   // 那一声轻提示也必须合规

  // 标称值超了：夹到 300
  const BeepPulses big = pulsesFor(BeepPattern::Short, 5000u);
  TEST_ASSERT_EQUAL_UINT8(1, big.n);
  TEST_ASSERT_EQUAL_UINT32(300u, big.on_ms[0]);

  // ★ 多相序列里**每一相**都要夹（只夹第一相是最容易漏的一种写法）
  const BeepPulses tri = pulsesFor(BeepPattern::Triple, 900u);
  TEST_ASSERT_EQUAL_UINT8(3, tri.n);
  for (uint8_t i = 0; i < tri.n; ++i) TEST_ASSERT_EQUAL_UINT32(300u, tri.on_ms[i]);

  // 真实走一遍：把 "开" 的那两相之间的时长量出来，必须 ≤ 300ms
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  b.beep(BeepPattern::Short, 5000u);
  runTo(b, 1000);
  bool lv[16];
  const int n = transitions(lv, 16);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_TRUE(lv[0]);            // 开
  TEST_ASSERT_FALSE(lv[1]);           // 关
  TEST_ASSERT_TRUE(b.lastPulseMs() <= kMaxPulseMs);

  // 0ms 不许变成"长鸣"：夹到下限 1ms（方向是"极短"，不是"一直响"）
  const BeepPulses zero = pulsesFor(BeepPattern::Short, 0u);
  TEST_ASSERT_EQUAL_UINT8(1, zero.n);
  TEST_ASSERT_TRUE(zero.on_ms[0] >= 1u);
  TEST_ASSERT_TRUE(zero.on_ms[0] <= kMaxPulseMs);
}

// ------------------------------------------------------------
// ④ 静音（`m`/NVS 那条路）时不写寄存器
//
// ★ 静音的判据**不在驱动里**（驱动只管"怎么落"，见 buzzer.h 的分层口径）：
//   `main.cpp` 在静音时**根本不调 `beep()`**（`g_sys.beepDue()` 那一支里有
//   `!g_beep_muted && !g_alerts.muted()`）。所以这里钉的是**驱动那一半**：
//   没被 `beep()` 调到 ⇒ 一根总线都不动；而"到了也不许响"由 `Silent` 兜住。
// ------------------------------------------------------------
void test_buzzer_exio_silent_writes_nothing(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  // ★ `begin()` 的语义是"回到已知的静音"：本机**本来就要求它是关的**
  //   ⇒ 电平没变 ⇒ 一次总线都不该动（真机上那一份 `dash_buzzer_set()`
  //   也只在位真的变了的时候才写，见那里的自证日志）。
  const int after_begin = g_ev_n;
  TEST_ASSERT_EQUAL_INT(0, after_begin);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);

  // 「静音期间主循环每轮照样做的那两件事」：只 tick，不 beep
  runTo(b, 500);
  TEST_ASSERT_EQUAL_INT(after_begin, g_ev_n);   // 一次都不许多
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
  TEST_ASSERT_FALSE(b.pulseActive());
  TEST_ASSERT_FALSE(b.sequenceActive());
  TEST_ASSERT_FALSE(b.pulseActive());

  // 就算真被调到 `Silent`：一声都不响、总线也不动
  b.beep(BeepPattern::Silent, 120);
  TEST_ASSERT_FALSE(b.sequenceActive());   // `Silent` ⇒ 压根没有序列
  runTo(b, 900);
  TEST_ASSERT_EQUAL_INT(after_begin, g_ev_n);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);

  // 静音期间收到 `off()`（主循环"不该响"那一支每轮都调）同样不许动总线
  b.off();
  runTo(b, 1200);
  TEST_ASSERT_EQUAL_INT(after_begin, g_ev_n);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
}

// ------------------------------------------------------------
// ⑤ 到时刻自动关（假时钟；不依赖真 `millis()`）
// ------------------------------------------------------------
void test_buzzer_exio_auto_off_at_deadline(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  const int after_begin = g_ev_n;         // begin() 那一次"关"（不算进下面的数）

  b.beep(BeepPattern::Short, 120);
  TEST_ASSERT_TRUE(b.sequenceActive());

  // ★ 起表之后的**第一次 tick 就起第一声**（与"起表"隔了多久无关：真机上这里
  //   正好跨过一次 flush）—— 所以第一个事件就是"开"，电平 0x05 -> 0x85。
  g_now_ms = 5u;                          // 故意隔了 5ms 才轮到主循环
  b.tick();
  TEST_ASSERT_EQUAL_INT(after_begin + 1, g_ev_n);
  TEST_ASSERT_TRUE(b.pulseActive());
  TEST_ASSERT_TRUE(g_ev[g_ev_n - 1].on);
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);    // 0x05 | bit7 —— 只有蜂鸣器那一位变高

  // 到点**之前**一格都不许关：这一相的时长从"真的开"那一刻算 ⇒ 125ms 时还在响
  g_now_ms = 124u;
  b.tick();
  TEST_ASSERT_TRUE(b.pulseActive());
  TEST_ASSERT_EQUAL_INT(after_begin + 1, g_ev_n);
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);

  // 到点（5 + 120 = 125ms）⇒ 自动关
  g_now_ms = 125u;
  b.tick();
  TEST_ASSERT_FALSE(b.pulseActive());
  TEST_ASSERT_EQUAL_INT(after_begin + 2, g_ev_n);
  TEST_ASSERT_FALSE(g_ev[g_ev_n - 1].on);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
  TEST_ASSERT_FALSE(b.sequenceActive());  // 单相序列：这一拍到此结束

  // 时钟继续走：不许自己再响起来
  runTo(b, 2000);
  TEST_ASSERT_FALSE(b.sequenceActive());
  TEST_ASSERT_EQUAL_INT(after_begin + 2, g_ev_n);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
}

// ★ 主循环每轮都调 `beep()`（见 main.cpp 的告警那一段）⇒ 必须幂等：
//   重复调用**不许**把相位归零（否则多相序列永远走不完，听起来只有一声）。
void test_buzzer_exio_repeat_beep_is_idempotent(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  b.beep(BeepPattern::Triple, 120);
  TEST_ASSERT_EQUAL_UINT8(3, b.pulseCount());

  // 每毫秒都重调一次 beep()（模拟主循环每轮都调），一路推到序列走完。
  // ★ 这条用例的全部意义就在"**重复调用不许把相位归零**"：
  //   若 beep() 每次都重新起表，这里只会看到 1 声（而不是 3 声）。
  bool lv[16];
  bool seen = false;
  for (int guard = 0; guard < 20000; ++guard) {
    if (b.sequenceActive()) seen = true;
    else if (seen) break;
    b.beep(BeepPattern::Triple, 120);
    b.tick();
    ++g_now_ms;
  }
  TEST_ASSERT_TRUE(seen);
  TEST_ASSERT_FALSE(b.sequenceActive());

  int n = transitions(lv, 16);
  TEST_ASSERT_EQUAL_INT(6, n);            // 仍然是 3 声（没有被重复调用打成 1 声）
  TEST_ASSERT_TRUE(lv[0]); TEST_ASSERT_FALSE(lv[1]);
  TEST_ASSERT_TRUE(lv[2]); TEST_ASSERT_FALSE(lv[3]);
  TEST_ASSERT_TRUE(lv[4]); TEST_ASSERT_FALSE(lv[5]);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);

  // ★ 序列走完之后**同模式同时长再来一次**必须能响（幂等缓存要清干净）
  g_ev_n = 0;
  b.beep(BeepPattern::Triple, 120);
  runSequence(b);
  n = transitions(lv, 16);
  TEST_ASSERT_EQUAL_INT(6, n);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
}

// ★ `off()` 是"**取消**"（静音那一跳的掐断路径）：立刻静默，不许等这一拍放完。
void test_buzzer_exio_off_cancels_immediately(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  b.beep(BeepPattern::Triple, 200);

  g_now_ms = 1u; b.tick();
  TEST_ASSERT_EQUAL_INT(1, g_ev_n);
  TEST_ASSERT_TRUE(g_ev[0].on);            // 正在响
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);

  b.off();                                  // ← 掐断
  TEST_ASSERT_FALSE(b.pulseActive());
  TEST_ASSERT_FALSE(b.sequenceActive());
  TEST_ASSERT_EQUAL_INT(2, g_ev_n);        // "开" + "关"，不多不少
  TEST_ASSERT_FALSE(g_ev[1].on);           // 最后一次写的是"关"
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);

  // 掐断之后时间继续走：不许自己再响起来
  const int n_before = g_ev_n;
  runTo(b, 3000);
  TEST_ASSERT_EQUAL_INT(n_before, g_ev_n);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);

  // ★ 而且"取消"要能**再响**：掐断之后同模式同时长必须还能起一拍
  //   （`clearSequence()` 把幂等缓存一起清了 —— 没清的话这一声会被永久吞掉）
  bool lv2[16];
  clearEvents();
  b.beep(BeepPattern::Triple, 200);
  TEST_ASSERT_TRUE(b.sequenceActive());
  runSequence(b);
  TEST_ASSERT_EQUAL_INT(6, transitions(lv2, 16));   // 3 声又走了一整轮
  TEST_ASSERT_TRUE(lv2[0]); TEST_ASSERT_FALSE(lv2[1]);
  TEST_ASSERT_TRUE(lv2[4]); TEST_ASSERT_FALSE(lv2[5]);
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
}

// ★ `begin()` 允许重复调用（接口这么写的），而且**每次都回到已知的静音**。
void test_buzzer_exio_begin_is_idempotent(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  // 本机一上电本来就要求"关" ⇒ 没有变化 ⇒ 一次总线都不该动
  // （真机上那一条由 `dash_buzzer_set()` 的位比较兜着，见它的自证日志）。
  TEST_ASSERT_EQUAL_INT(0, g_ev_n);
  b.begin();
  b.begin();
  TEST_ASSERT_EQUAL_INT(0, g_ev_n);       // 电平没变 ⇒ 不反复写
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
  TEST_ASSERT_EQUAL_STRING("exio8", b.name());

  // 正在响的时候 `begin()` ⇒ 必须回到静音（掉电/重启那条路的语义）
  b.beep(BeepPattern::Short, 200);
  g_now_ms = 10u; b.tick();
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);
  b.begin();
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
  TEST_ASSERT_FALSE(b.pulseActive());
  TEST_ASSERT_FALSE(b.sequenceActive());
}

// ============================================================
// ★★ ⑥ 绝对上限兜底（2026-09-25 新增）—— `Buzzer::safety()`
//
// 起因（车主原话，逐字）："**现在会长鸣一会儿，画面也卡住了**"。
// 这块板上的蜂鸣器是**软开关**（写 TCA9554 的 EXIO8，没有硬件定时）⇒
// "到点关"那个动作**只在主循环转得动的时候**才被执行。而 `tick()` 是主循环调的
// ⇒ 主循环一停，"到点关"就再也不会发生，高电平**留在总线上谁也关不掉**。
//
// ★ 所以这一组测的是"**tick() 没被调用**"这一种病：
//   这里**刻意不调 `tick()`**（也不调 `off()`），只推进假时钟再调 `safety()` ——
//   断言高电平被无条件关掉、序列被丢掉、`safetyCuts()` +1。
//
// ★ 另一半（反向判据）：**合法的长序列不许被它误掐** ——
//   `safety()` 放行的最坏情况是"一直到上限为止都没调用 tick"，而合法序列里
//   `tick()` 每轮都在跑、到点就关 ⇒ 上限必须**严于**最长的一拍，否则正常告警
//   会听起来少一声（那种 bug 极难查：它只在"某几拍恰好很慢"的时候出现）。
// ============================================================

// 上限那个数本身：与"最长的一拍"的关系（`Urgent` = 4×300 + 3×80 = 1440ms）。
void test_buzzer_exio_safety_is_the_absolute_cap(void) {
  TEST_ASSERT_EQUAL_UINT32(2000u, kBuzzerSafetyMs);
  const uint32_t worst = 4u * kMaxPulseMs + 3u * kGapMs;   // Urgent 的最坏时长
  TEST_ASSERT_EQUAL_UINT32(1440u, worst);
  TEST_ASSERT_TRUE(worst < kBuzzerSafetyMs);   // ★ 上限必须严于最长的一拍
}

// 正向：起表之后**谁都不再调 tick()**（模拟"主循环停住"）⇒ 到上限就地关掉。
void test_buzzer_exio_safety_ignores_state_machine(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();
  TEST_ASSERT_EQUAL_UINT32(0u, b.safetyCuts());

  // 3 短哔里最长的一相是 300ms：正常走的话 1060ms 就收完了。
  b.beep(BeepPattern::Long, 300);
  g_now_ms = 10u;
  b.tick();                                     // 起第一声（唯一一次 tick）
  TEST_ASSERT_TRUE(b.sequenceActive());
  TEST_ASSERT_TRUE(b.pulseActive());
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);          // 真的在响（EXIO8 = 1）

  // ★ 从此**不再 tick**（= 主循环被堵住），只推进时钟 + 每"圈"调一次 safety。
  //   上限之前：一个字节都不许动。
  for (g_now_ms = 11u; g_now_ms < kBuzzerSafetyMs; ++g_now_ms) b.safety();
  TEST_ASSERT_TRUE(b.sequenceActive());
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);
  TEST_ASSERT_EQUAL_UINT32(0u, b.safetyCuts());

  // 到上限的那一拍：无条件关掉 + 丢序列 + 计数 +1（**不依赖** tick 是否被调用过）
  g_now_ms = kBuzzerSafetyMs;
  b.safety();
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);          // ★ 静音了（电平原样，只有 bit7 被清）
  TEST_ASSERT_FALSE(b.sequenceActive());
  TEST_ASSERT_FALSE(b.pulseActive());
  TEST_ASSERT_EQUAL_UINT32(1u, b.safetyCuts());
  TEST_ASSERT_EQUAL_STRING("exio8", b.name());

  // ★ 掐断之后**必须能再响**：`clearSequence()` 会把幂等缓存一起清掉，
  //   否则"同模式同长的下一次 beep"会被永久吞掉（那种 bug 看起来像"只响第一次"）。
  b.beep(BeepPattern::Short, 120);
  TEST_ASSERT_TRUE(b.sequenceActive());
  g_now_ms += 1u;
  b.tick();
  TEST_ASSERT_EQUAL_HEX8(0x85, g_reg);
  TEST_ASSERT_EQUAL_UINT32(1u, b.safetyCuts());   // 计数是**累计**的，不因新序列归零
}

// 反向：**合法序列不许被它误掐** —— `tick()` 每轮都在跑的那条正常路径。
void test_buzzer_exio_safety_does_not_cut_legal_pulses(void) {
  resetHarness();
  BuzzerExio b(&fakeSet, nullptr, &fakeNow);
  b.begin();

  // 最长的一拍（`Urgent` = 4 声 × 300ms + 3 个 80ms 间隙）：每毫秒 tick + safety 各一次
  // （与真机主循环同形），全程**一次都不许**被上限掐掉。
  b.beep(BeepPattern::Urgent, 300);
  bool seen = false;
  for (int guard = 0; guard < 40000; ++guard) {
    if (b.sequenceActive()) seen = true;
    else if (seen) break;
    b.tick();
    b.safety();
    ++g_now_ms;
  }
  TEST_ASSERT_TRUE(seen);
  TEST_ASSERT_FALSE(b.sequenceActive());
  TEST_ASSERT_EQUAL_UINT32(0u, b.safetyCuts());   // ★ 一次都没掐
  TEST_ASSERT_EQUAL_HEX8(0x05, g_reg);
  // 4 声都真的响过：电平跳变 4 开 4 关
  bool lv[16];
  TEST_ASSERT_EQUAL_INT(8, transitions(lv, 16));
}

void register_buzzer_exio_tests(void) {
  RUN_TEST(test_buzzer_exio_mask_only_target_bit);
  RUN_TEST(test_buzzer_exio_long_degrades_to_three_short);
  RUN_TEST(test_buzzer_exio_pulse_counts);
  RUN_TEST(test_buzzer_exio_clamps_ms_to_300);
  RUN_TEST(test_buzzer_exio_silent_writes_nothing);
  RUN_TEST(test_buzzer_exio_auto_off_at_deadline);
  RUN_TEST(test_buzzer_exio_repeat_beep_is_idempotent);
  RUN_TEST(test_buzzer_exio_off_cancels_immediately);
  RUN_TEST(test_buzzer_exio_begin_is_idempotent);
  // ★★ 2026-09-25 新增（"屏卡死 + 蜂鸣器长鸣"那一单）：**绝对上限兜底**。
  //   上面那九条管的是"序列怎么走"；这一条管的是"**序列没机会走**" ——
  //   真机上那种形态是"主循环停住 ⇒ 到点关那个动作根本没被执行 ⇒ 高电平留在总线上"。
  RUN_TEST(test_buzzer_exio_safety_is_the_absolute_cap);
  RUN_TEST(test_buzzer_exio_safety_does_not_cut_legal_pulses);
  RUN_TEST(test_buzzer_exio_safety_ignores_state_machine);
}
