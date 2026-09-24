// ============================================================
// 系统状态层用例（2026-09-24）
//
// 两件事各测各的：
//   ① **"数据不可信"提示**：四个触发条件都要有正反两面
//      （不成立不报 / 成立才报 / 去抖 / 恢复自动消失 / 一声轻提示只响一次 +
//        限速 + 不相干的条件不许误报）
//   ② **诊断页**：字段映射逐字对账（哪一格显示什么）+ 页数边界 + 拿不到就是 "-"
//
// ★ 全部是**纯逻辑**用例：不碰 LVGL、不碰串口、不读时钟 —— `now_ms` 由用例
//   自己喂，所以 20 秒的冻结窗口在用例里是零成本的。
// ★ 时间口径与 test_alerts.cpp 逐字同一条：一律用**绝对时刻**推进
//   （`t = X; update(in, t)` 一个点一个点走），**不用**"从某个起点累加 dt"
//   的循环 —— 去抖是"从条件**首次成立**那一刻起连续算"，累加式循环里
//   "循环起点"与"条件首次成立"不是一回事（那一轮五条用例集体假红过）。
// ★ 测试输出**一律纯 ASCII**（README 那条纪律：中文 printf 会在 GBK 控制台
//   上抛 UnicodeEncodeError 并把用例统计打乱）。
// ============================================================
#include <unity.h>
#include <string.h>

#include "preview_input.h"   // K / T 两个键的语义（新增的那两位）
#include "system_status.h"

// 一份"一切正常"的输入：VAN 在上、来源是 van、链路有基准。
// 每一格都显式赋值 —— 默认值（van_age_ms = UINT32_MAX = 从没有过帧）本身
// 就是一个**有意义的状态**（"还没接 VAN"），不该拿它当"正常"用。
static SysStatusInputs healthy() {
  SysStatusInputs in{};
  in.speed_src = 3;   // van
  in.rpm_src = 3;
  in.coolant_src = 2; // obd
  in.intake_src = 2;
  in.van_ever_framed = true;
  in.van_age_ms = 10;
  in.speed_kmh = 60.0f;
  in.rpm = 2500.0f;
  in.link_known = true;
  in.link_state = 0;  // Locked
  return in;
}

// ============================================================
// 一、什么都不成立 ⇒ 不提示、不响
// ============================================================
void test_sys_idle_by_default(void) {
  SystemStatus s;
  for (uint32_t t = 1000; t <= 6000; t += 100) {
    TEST_ASSERT_TRUE(s.update(healthy(), t) == DataTrustReason::kNone);
  }
  TEST_ASSERT_FALSE(s.untrusted());
  TEST_ASSERT_EQUAL_UINT32(0u, s.episodes());
  TEST_ASSERT_FALSE(s.beepDue());

  // ★ "刚上电、一帧 VAN 都还没有"**不算**不可信（`van_ever_framed = false`）：
  //   开机动画那 1.1 秒里挂个角标会像故障，而不是"还没数据"。
  SystemStatus s2;
  SysStatusInputs boot{};
  boot.speed_src = 1;   // sim（没接任何硬件时数据层就是 Sim）
  boot.rpm_src = 1;
  boot.coolant_src = 1;
  boot.intake_src = 1;
  boot.van_ever_framed = false;
  boot.van_age_ms = UINT32_MAX;
  boot.speed_kmh = 12.0f;
  boot.rpm = 1200.0f;
  for (uint32_t t = 0; t <= 500; t += 100) s2.update(boot, t);
  // ⇒ 来源确实是 Sim（这一条**应该**报），但要在去抖窗口之后 —— 见下一条用例。
  TEST_ASSERT_TRUE(s2.active() == DataTrustReason::kNone);
}

// ============================================================
// 二、条件①：来源回退到 Sim ⇒ 去抖 1500 ms 后报，恢复 800 ms 后消失
//
// 这条是**产品要求**那一句的主路径：屏上跑的是假数据，必须看得出来。
// ============================================================
void test_sys_sim_fallback_debounce_and_recover(void) {
  SystemStatus s;
  SysStatusInputs in = healthy();
  in.speed_src = 1;   // ★ 车速回退到 Sim（VAN 断流后数据层就是这么标的）

  // 去抖窗口**差一点**不够 ⇒ 还不报（1500 - 1400 = 100）
  s.update(in, 1000);
  TEST_ASSERT_TRUE(s.update(in, 2499) == DataTrustReason::kNone);
  // 恰好到窗口 ⇒ 报
  TEST_ASSERT_TRUE(s.update(in, 2500) == DataTrustReason::kDataFallback);
  TEST_ASSERT_TRUE(s.untrusted());
  TEST_ASSERT_EQUAL_UINT32(1u, s.episodes());
  TEST_ASSERT_TRUE(s.beepDue());     // ★ 变坏这一拍要给一声轻提示

  // 一直坏着 ⇒ 不会每拍都"该响"（一声就是一声）
  for (uint32_t t = 2600; t <= 6000; t += 100) {
    s.update(in, t);
    TEST_ASSERT_FALSE(s.beepDue());
  }
  TEST_ASSERT_EQUAL_UINT32(1u, s.episodes());

  // 数据恢复：good 路要连续 800 ms（差一点仍挂着）
  SysStatusInputs ok = healthy();
  s.update(ok, 6100);
  TEST_ASSERT_TRUE(s.update(ok, 6899) == DataTrustReason::kDataFallback);
  TEST_ASSERT_TRUE(s.update(ok, 6900) == DataTrustReason::kNone);
  TEST_ASSERT_FALSE(s.untrusted());

  // ★ 再坏一次 ⇒ **重新计时**（不是"记着上次已经过了窗口"）
  s.update(in, 7000);
  TEST_ASSERT_TRUE(s.update(in, 8000) == DataTrustReason::kNone);
  TEST_ASSERT_TRUE(s.update(in, 8500) == DataTrustReason::kDataFallback);
  TEST_ASSERT_EQUAL_UINT32(2u, s.episodes());
}

// ============================================================
// 三、条件②：VAN 断流（帧时间戳不再前进）
// ============================================================
void test_sys_van_stale(void) {
  SystemStatus s;
  SysStatusInputs in = healthy();
  // 断流 3.5 秒（> kTrustVanStaleMs = 3000）。★ 来源这一格**仍然写 van**：
  // 这一条要单独验"断流"这条判据，所以把来源那一条按住不动（真实情况下
  // 数据层的 3 秒规则会同时把它变成 Sim —— 那时优先级会取 VanStale，见下）。
  in.van_age_ms = 3500;
  s.update(in, 1000);
  TEST_ASSERT_TRUE(s.update(in, 2500) == DataTrustReason::kVanStale);
  TEST_ASSERT_EQUAL_STRING("van-stale", dataTrustReasonName(s.active()));

  // 恰好 3000 ms **不算**断流（判据是 `>`，与数据层的 kStaleMs 同一条边界）
  SystemStatus s2;
  SysStatusInputs edge = healthy();
  edge.van_age_ms = 3000;
  s2.update(edge, 1000);
  TEST_ASSERT_TRUE(s2.update(edge, 2500) == DataTrustReason::kNone);
}

// ============================================================
// 四、条件③：数据冻结（帧还在来，值 20 秒一个字节都不变）
// ============================================================
void test_sys_data_frozen(void) {
  SystemStatus s;
  SysStatusInputs in = healthy();
  in.speed_kmh = 42.0f;
  in.rpm = 1500.0f;

  // 前 20 秒：值不变但窗口没到 ⇒ 不报
  s.update(in, 1000);
  TEST_ASSERT_TRUE(s.update(in, 20000) == DataTrustReason::kNone);
  // 窗口越过（1000 + 20000 = 21000）⇒ **判据**成立，但提示还要过去抖窗口
  //   （1500 ms）⇒ 这一拍仍不报。★ 这一条刻意分两步断言：它把"判据"与
  //   "什么时候上屏"两件事分开钉住（第一版就是在这里把两者当成同一件事，
  //   用例当场红了 —— 而实现是对的）。
  TEST_ASSERT_TRUE(s.update(in, 21000) == DataTrustReason::kNone);
  // 21000 + 1500 = 22500 ⇒ 报"冻结"
  TEST_ASSERT_TRUE(s.update(in, 22500) == DataTrustReason::kDataFrozen);

  // ★ 值一动 ⇒ 立刻刷新窗口（冻结的判据是"值多久没动过"，不是"有没有报过"）
  in.speed_kmh = 43.0f;
  s.update(in, 21100);
  TEST_ASSERT_TRUE(s.update(in, 30000) == DataTrustReason::kNone);

  // ★ 断流时**不报冻结**：那一档由 van-stale 管（两个条件各自独立，
  //   否则屏上会同时挂着两条理由）
  SystemStatus s2;
  SysStatusInputs stale = healthy();
  stale.speed_kmh = 42.0f;
  stale.rpm = 1500.0f;
  stale.van_age_ms = 3500;      // 断流
  s2.update(stale, 1000);
  TEST_ASSERT_TRUE(s2.update(stale, 60000) == DataTrustReason::kVanStale);
}

// ============================================================
// 五、条件④：双板链路降到最低档（仅从板）
// ============================================================
void test_sys_link_fallback(void) {
  // 从板：link_known + SimFallback(3) ⇒ 报
  SystemStatus s;
  SysStatusInputs in = healthy();
  in.speed_src = 3;   // 来源那一格按住 van ⇒ 这一条只能由"链路"触发
  in.link_state = 3;  // SimFallback
  s.update(in, 1000);
  TEST_ASSERT_TRUE(s.update(in, 2500) == DataTrustReason::kLinkFallback);

  // ★ 主板（link_known = false）⇒ **同样输入不报**：
  //   主板自己的数据来自它自己的 VAN/OBD，链路坏不坏不影响右屏那几个数。
  SystemStatus s2;
  SysStatusInputs master = in;
  master.link_known = false;
  s2.update(master, 1000);
  TEST_ASSERT_TRUE(s2.update(master, 2500) == DataTrustReason::kNone);
}

// ============================================================
// 六、优先级：四个条件同时成立时，取最重的那一条
// ============================================================
void test_sys_priority(void) {
  SystemStatus s;
  SysStatusInputs all = healthy();
  all.speed_src = 1;              // ① Sim
  all.van_age_ms = 9000;          // ② 断流
  all.link_state = 3;             // ④ 链路
  all.speed_kmh = 0.0f;           // ③ 冻结（值也不动）
  all.rpm = 0.0f;
  s.update(all, 1000);
  TEST_ASSERT_TRUE(s.update(all, 2500) == DataTrustReason::kVanStale);

  // 断流修好（帧回来了）⇒ 降一档到"来源回退"（字跟着变，但不重新计时、不再响）
  SysStatusInputs in2 = all;
  in2.van_age_ms = 20;
  in2.speed_kmh = 12.0f;
  in2.rpm = 900.0f;
  TEST_ASSERT_TRUE(s.update(in2, 2600) == DataTrustReason::kDataFallback);
  TEST_ASSERT_FALSE(s.beepDue());
  TEST_ASSERT_EQUAL_UINT32(1u, s.episodes());
}

// ============================================================
// 七、一声轻提示：限速 5 秒（数据在阈值上下来回抖时不许滴滴叫）
// ============================================================
void test_sys_beep_rate_limit(void) {
  SystemStatus s;
  SysStatusInputs bad = healthy();
  bad.rpm_src = 1;                 // 只有来源这一条
  SysStatusInputs ok = healthy();

  s.update(bad, 0);
  TEST_ASSERT_TRUE(s.update(bad, 1500) == DataTrustReason::kDataFallback);
  TEST_ASSERT_TRUE(s.beepDue());

  // 立刻恢复（过了 good 窗口）再立刻变坏 ⇒ **不许再响**（离上一声 < 5 s）
  s.update(ok, 1600);
  s.update(ok, 2500);
  TEST_ASSERT_FALSE(s.untrusted());
  s.update(bad, 2600);
  TEST_ASSERT_TRUE(s.update(bad, 4200) == DataTrustReason::kDataFallback);
  TEST_ASSERT_FALSE(s.beepDue());          // ← 限速掐掉了这一声

  // 再恢复、等到离第一声超过 5 s 再坏 ⇒ 这一声该响
  s.update(ok, 4300);
  s.update(ok, 5200);
  TEST_ASSERT_FALSE(s.untrusted());
  s.update(bad, 5300);
  TEST_ASSERT_TRUE(s.update(bad, 6900) == DataTrustReason::kDataFallback);
  TEST_ASSERT_TRUE(s.beepDue());
  TEST_ASSERT_EQUAL_UINT32(3u, s.episodes());

  // reset():状态清空，但累计计数与"上一声的时刻"**不清**
  s.reset();
  TEST_ASSERT_FALSE(s.untrusted());
  TEST_ASSERT_EQUAL_UINT32(3u, s.episodes());
}

// ============================================================
// 八、阈值常量本身（它们是文档里写的参数，改动了就要有人知道）
// ============================================================
void test_sys_thresholds(void) {
  TEST_ASSERT_EQUAL_UINT32(1500u, kTrustSysDebounceMs);
  TEST_ASSERT_EQUAL_UINT32(800u, kTrustSysRecoverMs);
  TEST_ASSERT_EQUAL_UINT32(20000u, kTrustFreezeWindowMs);
  TEST_ASSERT_EQUAL_UINT32(3000u, kTrustVanStaleMs);
  TEST_ASSERT_EQUAL_UINT32(5000u, kTrustBeepMinIntervalMs);
  // ★ 单次哔 <= 300 ms：有源蜂鸣器 + "持续高电平拉低 3.3V 轨"那条机制尚未排除
  //   ⇒ 不许出现长时间连续高电平（见 ARCHITECTURE.md 的提示音说明）。
  TEST_ASSERT_TRUE(kTrustBeepMs <= 300u);
  // 变坏比变好慢（这是"不刺眼"的另一半：恢复要干脆）
  TEST_ASSERT_TRUE(kTrustSysDebounceMs > kTrustSysRecoverMs);
  // 冻结窗口比去抖窗口长一个量级（否则停车等红灯会误报）
  TEST_ASSERT_TRUE(kTrustFreezeWindowMs >= 10000u);
}

// ============================================================
// 九、诊断页：第 1 页（数据这一路）的字段映射
// ============================================================
void test_diag_page_sys_fields(void) {
  SysStatusInputs in = healthy();
  in.van.frames = 12345;
  in.van.fcs_ok = 12300;
  in.van.edges = 99999;
  in.van_age_ms = 42;
  in.heap_free_kb = 217;
  in.psram_present = true;
  in.psram_free_kb = 7285;
  in.ui_fps10 = 52;              // 5.2 fps（主循环是 5 Hz 渲染档）
  in.display_stats_known = true;
  in.display_frames = 777;
  in.display_fps10 = 55;
  in.flush_max_us = 1300;
  in.copy_max_us = 640;

  const DiagView v = diagBuild(DiagPage::Sys, in);
  char buf[1024];
  const int n = diagRenderText(v, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n >= 10);

  // 逐格对账（**不比对整段文本**：加一行就会让断言失效，而"某一格显示错了"
  // 才是要抓的东西）
  TEST_ASSERT_NOT_NULL(strstr(buf, "v=60 km/h  rpm=2500"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "src spd=van"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "rpm=van"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "cool=obd"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "intake=obd"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "van frames=12345 fcs=12300"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "edges=99999 age=42"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "heap=217KB psram=7285KB"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "ui=5.2fps"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "panel=5.5fps f=777"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "flush=1300us copy=640us"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "mute=0"));
  TEST_ASSERT_EQUAL_STRING("DIAG", v.title);
}

// ============================================================
// 十、诊断页：拿不到就是 "-"（绝不显示 4294967295 / 假装 0）
// ============================================================
void test_diag_missing_is_dash(void) {
  SysStatusInputs in = healthy();
  in.van_age_ms = UINT32_MAX;        // 从没有过帧
  in.psram_present = false;          // 没有 PSRAM（esp32dev 那一档）
  in.display_stats_known = false;    // 驱动没上报面板统计
  const DiagView v = diagBuild(DiagPage::Sys, in);
  char buf[1024];
  diagRenderText(v, buf, sizeof(buf));

  TEST_ASSERT_NOT_NULL(strstr(buf, "age=-"));
  TEST_ASSERT_NULL(strstr(buf, "4294967295"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "psram=-"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "panel=- (driver n/a)"));
  // 单格的对账：fmtAgeMs 自己也要对
  char age[16];
  fmtAgeMs(age, sizeof(age), UINT32_MAX);
  TEST_ASSERT_EQUAL_STRING("-", age);
  fmtAgeMs(age, sizeof(age), 1234);
  TEST_ASSERT_EQUAL_STRING("1234", age);
}

// ============================================================
// 十一、诊断页：第 2 页（链路 / OBD / 静音）的字段映射
// ============================================================
void test_diag_page_link_fields(void) {
  SysStatusInputs in = healthy();
  in.link_state = 1;             // NoBasis
  in.link_tick_age_ms = 240;
  in.link_ticks_seen = 5000;
  in.link_seq_gaps = 3;
  in.link_seq_missing = 7;
  in.link_offset_ms = -12;
  in.obd_enabled = true;
  in.obd_rpm_hz = 28.5f;
  in.obd_coolant_hz = 1.0f;
  in.obd_intake_hz = 1.0f;
  in.obd_speed_hz = 0.0f;
  in.obd_support_known = true;
  in.obd_speed_supported = 1;
  in.obd_speed_polled = true;
  in.beep_muted = true;
  in.alert_active = 2;           // redline

  const DiagView v = diagBuild(DiagPage::Link, in);
  char buf[1024];
  diagRenderText(v, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("DIAG-LINK", v.title);
  TEST_ASSERT_TRUE(v.muted);
  TEST_ASSERT_NOT_NULL(strstr(buf, "link state=1 age=240ms"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "ticks=5000 gap=3"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "miss=7 off=-12ms"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "obd rpm=28.5 cool=1.0"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "intake=1.0 speed=0.0"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "010D=yes polled=1"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "alert=2 mute=1"));
}

// ============================================================
// 十二、诊断页：未接 OBD ⇒ "未连接"；主板 ⇒ 不显示从板在线性（L11）
// ============================================================
void test_diag_obd_not_connected_and_master_link(void) {
  SysStatusInputs in = healthy();
  in.obd_enabled = false;        // 没插 ELM327
  in.link_known = false;         // 主板
  const DiagView v = diagBuild(DiagPage::Link, in);
  char buf[1024];
  diagRenderText(v, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "obd n/a (no ELM327)"));
  // ★ 主板那一格**只写"链路这一档由主板的日志负责"**，不显示从板在不在线：
  //   §8 L11/L13 裁决的是"从板在线状态只进 STATUS 日志、不上屏"。
  TEST_ASSERT_NOT_NULL(strstr(buf, "link - (master: log only)"));
  TEST_ASSERT_NULL(strstr(buf, "离线"));
}

// ============================================================
// 十三、诊断页：页数边界 + 翻页循环 + 每页行数不超上限
// ============================================================
void test_diag_pages_and_bounds(void) {
  TEST_ASSERT_EQUAL_UINT8(2u, kDiagPageCount);
  // 越界的页号 ⇒ 折回第 0 页（不擅自扩页、也不返回空页）
  const DiagView v = diagBuild((DiagPage)7, healthy());
  TEST_ASSERT_EQUAL_STRING("DIAG", v.title);
  // 两页都要放得下（行数 <= 上限），而且**每一页至少有 5 行**
  // （一行都没有 = 用户按了 K 却看到一块空屏）
  for (uint8_t p = 0; p < kDiagPageCount; ++p) {
    const DiagView d = diagBuild((DiagPage)p, healthy());
    TEST_ASSERT_TRUE(d.lines >= 5);
    TEST_ASSERT_TRUE(d.lines <= kDiagMaxLines);
    for (uint8_t i = 0; i < d.lines; ++i) {
      // 每行都得有内容（空行 = 排版事故），且不超过行缓冲（会被截断）
      TEST_ASSERT_TRUE(strlen(d.line[i]) > 0);
      TEST_ASSERT_TRUE(strlen(d.line[i]) < kDiagLineCap);
    }
  }
  // 翻页键的语义在 preview_input.h 的用例里（test_ui_lamps.cpp 的 test_preview_keys）
}

// ============================================================
// 十四、理由名 / 页名：ASCII 那份（日志用）逐条对账
// ============================================================
void test_sys_names(void) {
  TEST_ASSERT_EQUAL_STRING("none", dataTrustReasonName(DataTrustReason::kNone));
  TEST_ASSERT_EQUAL_STRING("sim-fallback",
                           dataTrustReasonName(DataTrustReason::kDataFallback));
  TEST_ASSERT_EQUAL_STRING("van-stale", dataTrustReasonName(DataTrustReason::kVanStale));
  TEST_ASSERT_EQUAL_STRING("data-frozen", dataTrustReasonName(DataTrustReason::kDataFrozen));
  TEST_ASSERT_EQUAL_STRING("link-sim-fallback",
                           dataTrustReasonName(DataTrustReason::kLinkFallback));
  // 中文那份非空（原文见 ARCHITECTURE.md 的显示约定一节；不断言具体字，
  // 免得改一个字就要动用例 —— 但**必须**非空，屏上不能是空白）
  for (uint8_t i = 0; i <= (uint8_t)DataTrustReason::kCount; ++i) {
    const char* txt = dataTrustReasonText((DataTrustReason)i);
    TEST_ASSERT_NOT_NULL(txt);
    TEST_ASSERT_TRUE(strlen(txt) > 0);
  }
}

// ============================================================
// 十五、预览的两个新键（K / T）的语义
// ============================================================
void test_sys_preview_keys(void) {
  PreviewInput in;

  // `T`（把数据层口径切成"实测"）：与转向灯同一种"开关"语义，但是**绝对值型注入**
  // ⇒ 它进 `any()`（控制文件要按绝对值每帧重新施加它）。
  TEST_ASSERT_FALSE(in.sim_ok);
  TEST_ASSERT_TRUE(preview_apply_key(in, PreviewKey::Sim));
  TEST_ASSERT_TRUE(in.sim_ok && in.sim_ok_set);
  TEST_ASSERT_TRUE(in.any());
  preview_apply_key(in, PreviewKey::Sim);
  TEST_ASSERT_FALSE(in.sim_ok);
  TEST_ASSERT_TRUE(in.sim_ok_set);        // 设过就一直是"设过"（关也是一个选择）

  // `K`（诊断页）：**事件**，不是注入 ⇒ 只置一个"请求"位，
  //   ★ 而且它**不许**进 `any()`（否则主循环会为它打一行 "inject:" 回执，
  //     那行是给车状态用的 —— 屏上什么都没变却报"注入生效"最误导人）。
  PreviewInput in2;
  TEST_ASSERT_FALSE(in2.any());
  TEST_ASSERT_TRUE(preview_apply_key(in2, PreviewKey::Diag));
  TEST_ASSERT_TRUE(in2.diag_toggle_req);
  TEST_ASSERT_FALSE(in2.any());
  // 它也不该碰任何"数值/开关"注入位
  TEST_ASSERT_FALSE(in2.sim_ok_set || in2.speed_set || in2.left_set || in2.mute);

  // 控制文件的 `sim=` / `diag=` 与上面两个键同口径
  PreviewInput f;
  TEST_ASSERT_TRUE(preview_apply_control_text(f, "sim=1\ndiag=1\n") >= 2);
  TEST_ASSERT_TRUE(f.sim_ok && f.sim_ok_set);
  TEST_ASSERT_TRUE(f.diag_toggle_req);
  // `diag=0` 不算一条（事件型输入没有"复位"这回事）⇒ 不会把请求位置起来
  PreviewInput f2;
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(f2, "diag=0\n"));
  TEST_ASSERT_FALSE(f2.diag_toggle_req);
  // `clear=1` 要把 sim 那一位也清掉（否则"按 X 之后角标还挂着"永远查不出来）
  preview_apply_control_text(f, "clear=1\n");
  TEST_ASSERT_FALSE(f.sim_ok);
  TEST_ASSERT_FALSE(f.sim_ok_set);
}

void register_system_status_tests(void) {
  RUN_TEST(test_sys_idle_by_default);
  RUN_TEST(test_sys_sim_fallback_debounce_and_recover);
  RUN_TEST(test_sys_van_stale);
  RUN_TEST(test_sys_data_frozen);
  RUN_TEST(test_sys_link_fallback);
  RUN_TEST(test_sys_priority);
  RUN_TEST(test_sys_beep_rate_limit);
  RUN_TEST(test_sys_thresholds);
  RUN_TEST(test_diag_page_sys_fields);
  RUN_TEST(test_diag_missing_is_dash);
  RUN_TEST(test_diag_page_link_fields);
  RUN_TEST(test_diag_obd_not_connected_and_master_link);
  RUN_TEST(test_diag_pages_and_bounds);
  RUN_TEST(test_sys_names);
  RUN_TEST(test_sys_preview_keys);
}
