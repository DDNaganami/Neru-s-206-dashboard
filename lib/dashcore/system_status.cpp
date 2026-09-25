#include "system_status.h"
#include <stdarg.h>   // va_list / vsnprintf（诊断页的行格式化）

// ============================================================
// 系统状态层的实现（判据 + 诊断页的字段映射）
//
// ★ 本文件里**没有一行**碰 LVGL / Arduino / 硬件：它只做"算"与"格式化"。
//   所以它能在宿主机上被逐条测掉（test_system_status.cpp），
//   而 pcpreview 与真机跑的是同一份（见 system_status.h 的文件头）。
// ============================================================

// ------------------------------------------------------------
// 与既有常量的**复述**（本文件刻意不 include data_service.h / link_time.h：
//   那两个头会拖进 obd_source / van_source / 协议层，而这一层只是"显示与判据"，
//   让它反过来依赖它们会把模块绑在一起）。
//   数值写在这里、由文件末尾的 static_assert 钉住 ⇒ 复述错了**编译期**报出来，
//   而不是"该报假数据时屏上静默"（那正是这一层最怕的失败方式）。
// ------------------------------------------------------------
static const uint8_t kFieldSourceSimValue = 1u;      // FieldSource::Sim
static const uint8_t kLinkStateSimFallback = 3u;     // LinkTimeState::SimFallback

// ------------------------------------------------------------
// 一、名字
// ------------------------------------------------------------
const char* dataTrustReasonName(DataTrustReason r) {
  switch (r) {
    case DataTrustReason::kNone:         return "none";
    case DataTrustReason::kDataFallback: return "sim-fallback";
    case DataTrustReason::kVanStale:     return "van-stale";
    case DataTrustReason::kDataFrozen:   return "data-frozen";
    case DataTrustReason::kLinkFallback: return "link-sim-fallback";
    case DataTrustReason::kCount:        break;
  }
  return "?";
}

// 诊断页上的中文短文。★ 与 `dataTrustReasonName()` 一一对应（用例逐条比对）。
//   刻意短：诊断页一行要放得下"原因 + 它还带了哪些数"。
const char* dataTrustReasonText(DataTrustReason r) {
  switch (r) {
    case DataTrustReason::kNone:         return "实测数据";
    case DataTrustReason::kDataFallback: return "来源回退 Sim";
    case DataTrustReason::kVanStale:     return "VAN 断流";
    case DataTrustReason::kDataFrozen:   return "数据冻结";
    case DataTrustReason::kLinkFallback: return "链路回退 Sim";
    case DataTrustReason::kCount:        break;
  }
  return "?";
}

const char* diagPageTitle(DiagPage p) {
  switch (p) {
    case DiagPage::Sys:   return "DIAG";
    case DiagPage::Link:  return "DIAG-LINK";
    case DiagPage::Count: break;
  }
  return "DIAG";
}

// ------------------------------------------------------------
// 二、小工具
// ------------------------------------------------------------
void fmtAgeMs(char* buf, size_t cap, uint32_t age_ms) {
  if (!buf || cap == 0) return;
  if (age_ms == UINT32_MAX) {
    snprintf(buf, cap, "-");          // 拿不到 ⇒ "-"，绝不打出 4294967295
    return;
  }
  snprintf(buf, cap, "%lu", (unsigned long)age_ms);
}

int32_t fmtTenths(float v) {
  // 只做"×10 并四舍五入"：负值也要对（round-half-away-from-zero 足够，
  // 诊断页没有"必须银行家舍入"的口径）。
  const float x = v * 10.0f;
  return (int32_t)(x >= 0.0f ? (x + 0.5f) : (x - 0.5f));
}

// ------------------------------------------------------------
// 三、判据
// ------------------------------------------------------------
void SystemStatus::debounce(Cond& c, bool raw, uint32_t now_ms, uint32_t need_ms) {
  // 与 alerts.cpp 的 `Alerts::debounce` **逐字同一条**：
  //   条件不成立 ⇒ 立刻清（恢复是立即的，晚一步没有好处）；
  //   条件成立且连续达到 need_ms ⇒ 置位。
  if (!raw) {
    c.on = false;
    c.run = false;
    return;
  }
  if (!c.run) {
    c.run = true;
    c.since_ms = now_ms;
  }
  if (!c.on && (uint32_t)(now_ms - c.since_ms) >= need_ms) c.on = true;
}

// 数据冻结：值有没有在本窗口内**一个字节都没变**（而且"值不变"本身可疑）。
// ★ 三个刻意的前置：
//   ① `van_live`（帧还在来）—— 帧不来那一档由 kTrustVanStaleMs 管，两个条件
//      各自独立判；否则"断流 20 秒"会同时报成"冻结"，屏上多一条无意义的理由。
//   ② ★★ `engine_or_moving`（**发动机在转 / 车在动**）—— 这是本判据的前提，
//      理由与代价见 system_status.h 的 `kTrustEngineRunningRpm`：熄火停车时
//      车速/转速恒 0 是**正常**的，没有这一条就会挂一个假警报。
//      ★ 车速那半条（`speed_kmh > 0`）不是冗余：VAN 冻在"转速读 0 而车在跑"
//        那一帧时，只看 rpm 会把真故障漏掉。
//   ③ 判据只看**车速与转速**两个数（**不含水温/进气**：那两格长时间不变是
//      正常的，把它们算进来角标会常挂 —— 见头文件那段 ✗）。
static bool freezeCheck(float speed, float rpm, bool van_live, bool engine_or_moving,
                        uint32_t now_ms, float& last_speed, float& last_rpm,
                        bool& armed, uint32_t& since_ms) {
  if (!armed) {
    armed = true;
    last_speed = speed;
    last_rpm = rpm;
    since_ms = now_ms;
    return false;
  }
  const bool changed = memcmp(&speed, &last_speed, sizeof(float)) != 0 ||
                       memcmp(&rpm, &last_rpm, sizeof(float)) != 0;
  if (changed) {
    last_speed = speed;
    last_rpm = rpm;
    since_ms = now_ms;
    return false;
  }
  // ★ 前提先判：值不变但"本来就不该动"（熄火停车）⇒ 不报，也不去动窗口簿记
  //   （窗口照旧从"上一次值变化"算起，所以发动机一着、值一跳就重新计时）。
  if (!engine_or_moving) return false;
  if (!van_live) return false;      // 断流那一档不归这里判（见上）
  return (uint32_t)(now_ms - since_ms) >= kTrustFreezeWindowMs;
}

DataTrustReason SystemStatus::evaluate(const SysStatusInputs& in, uint32_t now_ms) {
  // ---- 四条判据，各自独立算（**不短路**：冻结窗口的簿记必须每拍都推进，
  //      跳着算会让"值开始不变"的时刻错位）----
  // ① 双板链路降到最低档（仅从板）。
  const bool link_fb = in.link_known && in.link_state == kLinkStateSimFallback;

  // ② VAN 断流：见过帧、但最近一帧已经超过阈值（3 s，与数据层的 kStaleMs 同口径）。
  const bool van_stale = in.van_ever_framed && in.van_age_ms != UINT32_MAX &&
                         in.van_age_ms > kTrustVanStaleMs;

  // ③ 数据冻结：帧还在来，值却长时间一个字节都不变。
  const bool van_live = in.van_ever_framed && in.van_age_ms != UINT32_MAX &&
                        in.van_age_ms <= kTrustVanStaleMs;
  // ★ 冻结判据的**前提**：发动机在转（`rpm` **严格**大于 500）或车在动（`speed > 0`）。
  //   两个理由（为什么两半都要）见头文件 kTrustEngineRunningRpm 那一段：
  //     ① 熄火停车时这两个数本来就该不动 ⇒ 没有前提就是假警报；
  //     ② 只看 rpm 会漏掉"VAN 冻在转速读 0 而车在跑"那一帧。
  const bool engine_or_moving = (in.rpm > (float)kTrustEngineRunningRpm) ||
                                (in.speed_kmh > 0.0f);
  freeze_now_ = freezeCheck(in.speed_kmh, in.rpm, van_live, engine_or_moving, now_ms,
                            last_speed_, last_rpm_, freeze_armed_, freeze_since_ms_);

  // ④ 来源回退到 Sim：车速 / 转速 / 水温 / 进气四格里有任何一格是 Sim。
  //   ★ 为什么只看这四格：它们是**上屏的四个标量**（弧 + 大数字 + 副表数字）。
  //     灯位/门/VIN 那几格只有 Van/None 两种取值（见 data_service.h），
  //     不可能出现 Sim，所以不必判 —— 也不必为它们编一条新判据。
  const bool sim_fallback = in.speed_src == kFieldSourceSimValue ||
                            in.rpm_src == kFieldSourceSimValue ||
                            in.coolant_src == kFieldSourceSimValue ||
                            in.intake_src == kFieldSourceSimValue;

  // ---- 取**最高优先级**的那一条（数组下标 = 严重程度）----
  // 顺序的判据（为什么是这个顺序）：
  //   · `VanStale` 最重：连帧都没有了，任何值都不再有来源；
  //   · `DataFallback` 次之：屏上正跑的是假数据 —— **这正是产品要求要报的那件事**；
  //   · `DataFrozen` 再次之：数据"看着是真的"但其实卡住了；
  //   · `LinkFallback` 最轻：从板与主板的链路退到 Sim，而屏上那几个数**可能**
  //     仍然来自本地 Sim（那时上面的 DataFallback 会先命中）。
  DataTrustReason raw = DataTrustReason::kNone;
  if (van_stale)         raw = DataTrustReason::kVanStale;
  else if (sim_fallback) raw = DataTrustReason::kDataFallback;
  else if (freeze_now_)  raw = DataTrustReason::kDataFrozen;
  else if (link_fb)      raw = DataTrustReason::kLinkFallback;
  raw_ = raw;
  return raw;
}

DataTrustReason SystemStatus::update(const SysStatusInputs& in, uint32_t now_ms) {
  beep_due_ = false;

  const DataTrustReason raw = evaluate(in, now_ms);
  const bool bad = (raw != DataTrustReason::kNone);

  // 两路去抖（"变坏"与"变好"各有各的窗口，见 system_status.h 的阈值说明）：
  //   bad 路：变成不可信要连续 kTrustSysDebounceMs；
  //   good 路：恢复要连续 kTrustSysRecoverMs。
  // ★ 两路各自独立推进（都每拍调一次）—— 这正是"变坏慢、变好快"能同时成立的写法。
  debounce(bad_, bad, now_ms, kTrustSysDebounceMs);
  debounce(good_, !bad, now_ms, kTrustSysRecoverMs);

  // ---- 状态迁移 ----
  // 初始：bad_ / good_ 都还没置位 ⇒ 屏上不显示（"刚上电还没数据"不是"不可信"，
  // 开机动画那 1.1 秒里挂个角标反而像故障）。
  if (active_ == DataTrustReason::kNone) {
    if (bad_.on) {
      active_ = raw;                        // 用去抖窗口**结束时**那一条
      ++episodes_;
      // ★ 一声轻提示：只在"变坏"这一跳置一次，且与上一声至少隔
      //   kTrustBeepMinIntervalMs（数据在阈值上下抖时不会变成滴滴叫）。
      //   ★ 静音开关**不在这里**判：静音是"人的选择"，由调用方在把 beepDue()
      //     交给蜂鸣器之前过一道 —— 这一层刻意不持有静音状态，免得两处各记一份。
      if (last_beep_ms_ == 0u ||
          (uint32_t)(now_ms - last_beep_ms_) >= kTrustBeepMinIntervalMs) {
        beep_due_ = true;
        last_beep_ms_ = (now_ms == 0u) ? 1u : now_ms;   // 0 保留给"还没响过"
      }
    }
  } else {
    if (good_.on) {
      active_ = DataTrustReason::kNone;     // 数据恢复 ⇒ 提示**自动消失**
      bad_.on = false;                      // 下一次变坏重新走完整的去抖窗口
      bad_.run = false;
    } else if (bad_.on && raw != DataTrustReason::kNone) {
      // 一直不可信时：理由可以**升级/降级**（例如 VAN 断流修好、但来源仍是 Sim）
      // ⇒ 角标上的字跟着变。这不重新计时、也不再响一声（那是同一次"不可信"）。
      active_ = raw;
    }
  }
  return active_;
}

void SystemStatus::reset() {
  bad_ = Cond{};
  good_ = Cond{};
  raw_ = DataTrustReason::kNone;
  active_ = DataTrustReason::kNone;
  freeze_now_ = false;
  freeze_armed_ = false;
  freeze_since_ms_ = 0;
  beep_due_ = false;
  // ★ 刻意不清 episodes_ 与 last_beep_ms_：累计统计与"上一声什么时候响的"
  //   不该因为一次复位就查不出来（与 Alerts::reset 同一条口径）。
}

// ------------------------------------------------------------
// 四、诊断页的字段映射
//
// ★ "哪一格显示什么"写成**纯函数 + 成品文本**：用例逐字对账
//   （`diagRenderText`），而 dash_ui 只负责把文本画到标签上。
//   于是"数字在动"这件事能在宿主机上验，不必靠看像素。
// ------------------------------------------------------------

// 往 DiagView 里塞一行（`fmt` 自己带参数；装不下由 snprintf 截断）。
// 返回 false = 行数已满（后面的行被丢掉）。
static bool diagLine(DiagView& v, const char* fmt, ...) {
  if (v.lines >= kDiagMaxLines) return false;
  char* dst = v.line[v.lines];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, kDiagLineCap, fmt, ap);
  va_end(ap);
  ++v.lines;
  return true;
}

// 字段来源的短名（复述 `fieldSourceName()`；只在诊断页用）。
// ★ 与 data_service.cpp 的那一份**必须逐条一致**（用例对账），改一处要改两处。
static const char* srcShort(uint8_t src) {
  switch (src) {
    case 0: return "none";
    case 1: return "sim";     // ★ 这个字就是"假数据"的信号，诊断页要显眼
    case 2: return "obd";
    case 3: return "van";
    case 4: return "link";
    default: return "?";
  }
}

DiagView diagBuild(DiagPage page, const SysStatusInputs& in) {
  DiagView v{};
  if ((uint8_t)page >= kDiagPageCount) page = DiagPage::Sys;   // 越界折回，不擅自扩页
  v.title = diagPageTitle(page);
  v.muted = in.beep_muted;

  char age[16];
  fmtAgeMs(age, sizeof(age), in.van_age_ms);

  if (page == DiagPage::Sys) {
    // ---- 第 1 页：**数据这一路**（"屏上的数从哪来、可不可信"）----
    // 读数用整数（车速取整 / 转速取整）：诊断页要看的是"在不在动"，
    //   小数位在 480 档上是噪声，而且整数格式在两个构建上行为一致。
    diagLine(v, "v=%ld km/h  rpm=%ld", (long)(int32_t)(in.speed_kmh + 0.5f),
             (long)(int32_t)(in.rpm + 0.5f));
    diagLine(v, "src spd=%s", srcShort(in.speed_src));
    diagLine(v, "    rpm=%s", srcShort(in.rpm_src));
    diagLine(v, "    cool=%s", srcShort(in.coolant_src));
    diagLine(v, "    intake=%s", srcShort(in.intake_src));
    diagLine(v, "van frames=%lu fcs=%lu", (unsigned long)in.van.frames,
             (unsigned long)in.van.fcs_ok);
    diagLine(v, "    edges=%lu age=%s", (unsigned long)in.van.edges, age);
    if (in.psram_present) {
      diagLine(v, "heap=%luKB psram=%luKB", (unsigned long)in.heap_free_kb,
               (unsigned long)in.psram_free_kb);
    } else {
      diagLine(v, "heap=%luKB psram=-", (unsigned long)in.heap_free_kb);
    }
    diagLine(v, "ui=%lu.%lufps", (unsigned long)(in.ui_fps10 / 10u),
             (unsigned long)(in.ui_fps10 % 10u));
    if (in.display_stats_known) {
      diagLine(v, "panel=%lu.%lufps f=%lu", (unsigned long)(in.display_fps10 / 10u),
               (unsigned long)(in.display_fps10 % 10u),
               (unsigned long)in.display_frames);
      diagLine(v, "flush=%luus copy=%luus", (unsigned long)in.flush_max_us,
               (unsigned long)in.copy_max_us);
    } else {
      // 驱动没上报面板统计（本轮：RGB 驱动还没有这个出口）⇒ 明确写 n/a，
      // 而不是显示 0 —— 0 会被读成"面板一帧都没刷"（那是另一回事）。
      diagLine(v, "panel=- (driver n/a)");
    }
    diagLine(v, "mute=%ld  K:next", (long)(in.beep_muted ? 1 : 0));
    // ★ 面板健康守护那行（2026-09-24，"仪表盘必须常亮"的**可见化**）：
    //   `rd` = 守护跑了多久/回读过几次（**在涨就说明守护活着**）；
    //   `fix` = 按影子把扩展器重写回来的次数（LCD_RST/LCD_CS 被改写那条路径）；
    //   `bl` = 背光被重设的次数；`anom` = 发现过几次不一致（含已修的）。
    //   ★ 这一行**恒显示**（不写 n/a）：设备上它是真的，预览/抓帧盒上它恒 0
    //     —— 而"0"在那里本来就是正确的读数（那些构建里没有面板可守）。
    diagLine(v, "guard %lus rd=%lu fix=%lu bl=%lu anom=%lu",
             (unsigned long)(in.guard_uptime_ms / 1000u),
             (unsigned long)in.guard_rd_ok, (unsigned long)in.guard_fix,
             (unsigned long)in.guard_bl, (unsigned long)in.guard_anomaly);
    // ★★ 跨重启累计那一行（2026-09-25 新增，见 lib/dashcore/boot_persist.h）：
    //   · `s` 前缀 = **sum**（从这块板第一次跑本层固件起，跨重启累加）；
    //   · `k` = 累计值**落盘过几次**（跨重启单调 +1）—— "k 变了 ⇒ 累计里已经含了
    //     上一次运行的尾巴"；`k=0` 且开机不到 10 分钟也是正常的（心跳还没到点）；
    //   · `n` = 第几次上电/复位（既有的 `bootn`）；`hb` = 心跳累计次数。
    //   ★ 这一行与上面那一行的区别就是本单要解决的痛点：**上一行随重启归零，
    //     这一行不归零** —— "某次夜里守护救过几回"从此不再丢。
    //   ★ 拿不到（预览/抓帧盒：那些构建里没有 NVS）就明确写 `-`，不许假装 0 次。
    diagLine(v, "sum rd=%lu fix=%lu bl=%lu anom=%lu k=%lu",
             (unsigned long)in.guard_total_rd, (unsigned long)in.guard_total_fix,
             (unsigned long)in.guard_total_bl, (unsigned long)in.guard_total_anom,
             (unsigned long)in.guard_tot_snaps);
    if (in.boot_count == 0u || in.boot_count == kBootCountUnknown) {
      diagLine(v, "n=- hb=%lu", (unsigned long)in.boot_hb_n);
    } else {
      diagLine(v, "n=%lu hb=%lu", (unsigned long)in.boot_count,
               (unsigned long)in.boot_hb_n);
    }  } else {
    // ---- 第 2 页：**链路与告警**（"两台板之间那条线好不好"）----
    // ★★ 第一行**永远**是本机角色（2026-09-25 新增）—— 两块 2.8C 外观一样，
    //   而角色是编译期定死的（§5）⇒ 打开诊断页第一眼就该知道"手里这块是谁"。
    //   ★ 纯 ASCII（本构建只使能 Montserrat，中文一个字形都画不出来，见 build_diag 那段）。
    diagLine(v, "role=%s (%s)", in.link_role == 1u ? "MASTER" : "SLAVE",
             in.link_role == 1u ? "right/A" : "left/B");
    if (in.link_known) {
      diagLine(v, "link state=%ld age=%lums", (long)(int32_t)in.link_state,
               (unsigned long)in.link_tick_age_ms);
      diagLine(v, "   ticks=%lu gap=%lu", (unsigned long)in.link_ticks_seen,
               (unsigned long)in.link_seq_gaps);
      diagLine(v, "   miss=%lu off=%ldms", (unsigned long)in.link_seq_missing,
               (long)(int32_t)in.link_offset_ms);
    } else {
      // 主板（LINK_ROLE==1）：它没有 LinkTime（那是从板侧的时基）。
      // ★ 这一格刻意**不显示"从板在不在线"** —— 那件事照 §8 的 L11/L13
      //   只进日志（30 s 门限、只用于日志、不上屏）。诊断页不越过那条裁决。
      diagLine(v, "link - (MASTER: log only)");
    }
    if (in.obd_enabled) {
      diagLine(v, "obd rpm=%.1f cool=%.1f", (double)in.obd_rpm_hz,
               (double)in.obd_coolant_hz);
      diagLine(v, "    intake=%.1f speed=%.1f", (double)in.obd_intake_hz,
               (double)in.obd_speed_hz);
      const char* sup = "?";
      if (in.obd_support_known) sup = (in.obd_speed_supported == 1) ? "yes" : "no";
      diagLine(v, "    010D=%s polled=%ld", sup, (long)(in.obd_speed_polled ? 1 : 0));
    } else {
      diagLine(v, "obd n/a (no ELM327)");   // "未接就显示未连接"
    }
    diagLine(v, "alert=%ld mute=%ld", (long)(int32_t)in.alert_active,
             (long)(in.beep_muted ? 1 : 0));
    diagLine(v, "mask=%lupx preview", (unsigned long)in.panel_mask_px);
    diagLine(v, "K:next  Esc:exit");
  }
  return v;
}

int diagRenderText(const DiagView& v, char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  buf[0] = '\0';
  size_t used = 0;
  int n = 0;
  for (uint8_t i = 0; i < v.lines && i < kDiagMaxLines; ++i) {
    if (v.line[i][0] == '\0') continue;
    const size_t len = strlen(v.line[i]);
    if (used + len + 2 >= cap) break;    // 装不下就停（不截半行）
    memcpy(buf + used, v.line[i], len);
    used += len;
    buf[used++] = '\n';
    buf[used] = '\0';
    ++n;
  }
  return n;
}

// ------------------------------------------------------------
// 五、与既有常量的对账（**编译期**，不用等屏上出错）
// ------------------------------------------------------------
// `FieldSource` 的数值（lib/dashcore/data_service.h）：None=0 / Sim=1 / Obd=2
//   / Van=3 / Link=4。
static_assert(kFieldSourceSimValue == 1u, "FieldSource::Sim 的数值变了？见 data_service.h");
// `LinkTimeState::SimFallback` 的数值（lib/link/link_time.h）：Locked=0 /
//   NoBasis=1 / Degraded=2 / SimFallback=3。
static_assert(kLinkStateSimFallback == 3u, "LinkTimeState::SimFallback 的数值变了？");
// `kDiagMaxLines` 必须放得下两页里最长的那一页（第 1 页 11 行）。
static_assert(kDiagMaxLines >= 12u, "诊断页的行数上限被改小了，最长那页会截断");
