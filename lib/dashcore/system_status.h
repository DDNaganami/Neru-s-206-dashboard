#pragma once
#include <stdint.h>
#include <stdio.h>    // snprintf（诊断页的行文本）
#include <string.h>
// ============================================================
// 系统状态层 —— 「**屏上的数是不是实测数据**」+ 「诊断页要显示什么」
// （2026-09-24 新增；对应 ARCHITECTURE.md 的显示约定一节）
//
// ★★ 为什么要有这一层（产品要求，原文见 ARCHITECTURE.md）：
//   **当仪表显示的不是实测数据时，必须在屏上让驾驶员看得出来。**
//   理由不是洁癖：VAN 断流/数据冻结时，数据层会**按既有规则回退到 Sim**
//   （`data_service.cpp` 的 `kStaleMs = 3000`）⇒ 指针照样动、数字照样跳，
//   但那是**假数据**。驾驶员看到一块"一切正常"的表，而表在撒谎。
//
// ★ 为什么放在 lib/dashcore/（而不是写进 src/dash_ui.cpp）：
//   ① **native 能测**：`lib_archive = no` 的 native 构建**只编 lib/**，
//      所以"想测谁就把它放进 lib/"（与 lamp_view.h / panel_view.h 同一条
//      结构约束）。本文件里那几条判据（四个触发条件、去抖、限速、可恢复、
//      诊断页字段映射）算错**不会报错**，只会让屏上多一个永不消失的角标、
//      或者该提示的时候静默 —— 那正是最需要机器钉住的一类东西。
//   ② **pcpreview 与真机跑同一份判据**：预览里按出来的提示，与车上跳出来的
//      提示是同一段代码算的 ⇒ "预览里有、车上没有"这种分叉在结构上不可能。
//
// ★ 本文件**不碰 LVGL、不碰硬件、不依赖 Arduino**（纯逻辑 + 时间戳），
//   与 alerts.h 同一套分层。dash_ui 只负责把结论变成像素。
// ============================================================

// ------------------------------------------------------------
// 一、数据可信度（"数据不可信"提示的判据）
// ------------------------------------------------------------

// 四个触发条件（**全部用既有可得信息，一个新数据源都没有**）。
// ★ 数值有意义（`kNone` 在枚举首位 = 默认不提示），且**按顺序 = 严重程度**：
//   数组下标就是优先级，`DataTrustReasonName()` 的 switch 与之逐条对齐。
enum class DataTrustReason : uint8_t {
  kNone = 0,       // 数据可信（或还没到该提示的时候）—— 不显示任何角标
  kDataFallback,   // ① 数据来源回退到 `Sim`（`data_service` 已有的来源档位）
  kVanStale,       // ② VAN 断流超时（帧计数/时间戳不再前进）
  kDataFrozen,     // ③ 数据冻结（帧还在来，值长时间一个字节都不变
                   //    —— ★ 前提：发动机在转 / 车在动，见 kTrustEngineRunningRpm）
  kLinkFallback,   // ④ 双板链路降到最低档（`LinkTime` 的 SimFallback，**仅从板**）
  kCount
};
const char* dataTrustReasonName(DataTrustReason r);       // ASCII（日志用）
const char* dataTrustReasonText(DataTrustReason r);       // 中文短文（诊断页用）

// ------------------------------------------------------------
// 二、阈值与限速（**这些数就是文档里要写的那几个参数**）
//
// ★ 为什么"变坏"和"变好"用**两个不同的窗口**：
//   · 变坏慢一点（1.5 s）：VAN 的 0x824 是 ≈79.7 Hz，但**回放/停车**时可能
//     成串地来；一个 1.5 s 的窗口能吃掉"偶发丢一帧"与"贴帧的间隙"，
//     而真正断流时 1.5 s 已经足够快（人还没发现指针不动，角标先出来）。
//   · 变好快一点（0.8 s）：数据回来了却还挂着"数据不可信"是**反向的谎** ——
//     它会让驾驶员不敢相信屏上的真值。恢复要干脆。
// ★ 两个窗口都是**连续成立**才认（不是累计）—— 与 alerts 的去抖是同一条
//   纪律（累计计时会让一串噪声把提示"攒"出来）。
// ★ 一声提示音：**每次"变坏"只响一次**（`beep_due` 只置一次），且受
//   `kTrustBeepMinIntervalMs` 限速 ⇒ 数据在阈值上下抖时不会变成滴滴叫。
//   `kTrustBeepMs` 是**标称时长**，交给 `Buzzer` 那一侧落（有源蜂鸣器只能
//   开/关 ⇒ 就是"开这么久然后关"，见 buzzer.h 的分层说明）。
// ------------------------------------------------------------
static const uint32_t kTrustSysDebounceMs = 1500u;   // 判"不可信"：连续成立这么久
static const uint32_t kTrustSysRecoverMs  = 800u;    // 判"恢复"：连续成立这么久

// 数据冻结窗口。**比上面两个窗口长一个量级**，这是刻意的：
//   车速/转速在真车上长时间不变是**可能的**（怠速等红灯、堵车停停走走、
//   长下坡匀速），所以判据取"20 秒里一个字节都没变"—— 那时"值不变"更可能
//   是上游卡住而不是车真的不动。★ 它只判**值**，不判"帧有没有来"：
//   帧不来那一档由 kVanStaleMs 管（两个条件各自独立，见 evaluate()）。
static const uint32_t kTrustFreezeWindowMs = 20000u;

// ------------------------------------------------------------
// ★★ 数据冻结的**前提**：「值不变」只有在**发动机在转 / 车在动**时才可疑。
//
// 为什么非要有这个前提（2026-09-24 追加；它消掉的是一类**假警报**）：
//   熄火通电在车里听歌时，车速恒 0、转速恒 0 —— 这两个数**本来就该不动**。
//   没有前提的判据会在 20 秒后给屏上挂一个"数据不可信"，而那是在冤枉一块
//   好表：假警报多了，真的那一次就没人看了。
//
// 判据（`evaluate()` 里算成 `engine_or_moving` 交给 freezeCheck）：
//     `rpm > kTrustEngineRunningRpm || speed_kmh > 0`
//   · `rpm > 500`：怠速约 700~900 rpm ⇒ 500 以下一律当成"不在转"
//     （起动机拖着发动机但还没着、或 VAN 上就是 0）。
//     ★ 是**严格大于**：`rpm` 恰好 500 **不算**"在转"（边界由
//       test_system_status.cpp 的 `test_sys_freeze_rpm_boundary_500` 钉住）。
//   · `speed_kmh > 0`：**为什么除了转速还要看车速** —— 万一 VAN 冻在
//     "转速读 0 但车其实在跑"那一帧，只看 rpm 会把**真故障**漏掉 ⇒
//     车在动的时候，静止的数据一律可疑。
//
// ★ 代价（同样的字写进 ARCHITECTURE.md「显示约定」那一节，别只留在这里）：
//   **停车且熄火时的真实冻结不再报。** 可以接受，三条理由：
//     ① 那种场合（熄火听歌、车库停着）显示通常也是关着的；
//     ② 另外两条网照旧兜着 —— ①来源回退 Sim（`kDataFallback`）
//        ②VAN 断流（`kVanStale`）：真坏了（帧不来 / 数据层回落 Sim）照样报，
//        只是"值冻住"这一条不再**单独**触发；
//     ③ 反过来（熄火停车时挂个"不可信"）是**必然发生**的假警报，比漏报更伤。
//
// ★★ 不要把水温 / 进气算进这个前提或冻结判据 ✗
//   （写在这里，免得以后有人"顺手补全"）：水温 20 秒不变是**正常**的 ——
//   冷却液热惯量那么大，一分钟不动都正常；进气温度同理。把它们算进来，
//   判据在真车上会**一直**成立 ⇒ 角标常挂 = 又一个假警报。
//   这一层判的只是"车速与转速这两个**本该一直在动**的数有没有动"。
// ------------------------------------------------------------
static const uint32_t kTrustEngineRunningRpm = 500u;

// VAN 断流阈值：帧时间戳超过它没前进就算断流。
//   ★ 取 3000 ms 是**刻意与 data_service 的 `kStaleMs`（3 秒）同一口径** ——
//     那正是"数据层开始回落 Sim"的那一刻。比它短 ⇒ 角标会比假数据先出现
//     （屏上是真值却报"不可信"）；比它长 ⇒ 假数据已经上屏了角标还没出来。
static const uint32_t kTrustVanStaleMs = 3000u;

// 提示音（**一声轻提示**）。
// ★ 300 ms 是**硬上限**（有源蜂鸣器 + "持续高电平拉低 3.3V 轨"这条机制
//   尚未排除 ⇒ 不许长时间连续高电平，见 ARCHITECTURE.md 的提示音说明）。
//   120 ms 与 alerts 的 `beep_ms` 默认值同量级：听得见、不刺耳。
static const uint32_t kTrustBeepMs = 120u;
static const uint32_t kTrustBeepMinIntervalMs = 5000u;   // 两次"变坏"提示的最小间隔
static_assert(kTrustBeepMs <= 300u, "单次哔必须 <= 300ms（见文档的提示音说明）");

// VAN 帧计数（**既有计数器，只是换个出口**）。
// `frames` = 解出的帧数、`fcs_ok` = 其中 CRC 通过的、`edges` = 边沿数。
// ★ 出处：`VanPhyWire::Stats`（lib/dashcore/van_phy_wire.h）。真板上由
//   `VanPhyGpio` 每秒打进串口 `van: edges=… frames=… fcs_ok=…` 那一行；
//   这里是同一组数进诊断页。抓帧盒（esp32dev）与预览上没有物理层 ⇒ 全 0。
struct VanCounters {
  uint32_t frames = 0;
  uint32_t fcs_ok = 0;
  uint32_t edges  = 0;
  bool     live   = false;   // 这些数**此刻还在涨**吗（调用方判：本秒与上秒不同）
};

// 诊断页/判据要的全部输入。**一个字段都不新增数据源**：
//   每一格都能在 `main.cpp` 现有的那几行串口日志里找到对应项。
struct SysStatusInputs {
  // ---- ① 数据来源档位（data_service 的 DataSourceStatus）----
  // ★ 用 `uint8_t` + 名字而不是 `FieldSource`：本文件刻意**不 include
  //   data_service.h**（那会把 obd_source/van_source 一整串拖进来），
  //   数值由 data_service.h 的 `fieldSourceName()` 保证一致。
  uint8_t speed_src = 0;        // (uint8_t)FieldSource
  uint8_t rpm_src   = 0;
  uint8_t coolant_src = 0;
  uint8_t intake_src  = 0;

  // ---- ② VAN 断流（既有时间戳/计数）----
  bool     van_ever_framed = false;   // 开机以来收到过 VAN 帧吗
  uint32_t van_age_ms = UINT32_MAX;   // 最近一帧的年龄（UINT32_MAX = 从没有过）
  VanCounters van{};                  // 帧计数（诊断页显示）

  // ---- ③ 数据冻结（既有快照的**数值**）----
  float speed_kmh = 0.0f;
  float rpm = 0.0f;

  // ---- ④ 双板链路（LinkTime 的三档；**仅从板**）----
  // ★ 主板（LINK_ROLE==1）不上报这一格：主板自己的数据来自它自己的 VAN/OBD，
  //   链路坏不坏不影响右屏那几个数是不是实测值 ⇒ `link_known=false`。
  bool     link_known = false;
  uint8_t  link_state = 0;       // (uint8_t)dashlink::LinkTimeState
  uint32_t link_tick_age_ms = 0;
  uint16_t link_ticks_seen = 0;
  uint16_t link_seq_gaps = 0;
  uint16_t link_seq_missing = 0;
  int32_t  link_offset_ms = 0;
  // ★★ 本机角色（2026-09-25 新增）—— 0 = 从板（左）/ 1 = 主板（右），取值与
  //   `lib/link/link_role.h` 的 `kRoleSlave`/`kRoleMaster` 一致（§2 的 ROLE 字段）。
  //   ★ 为什么这一格要有：两块 2.8C **外观完全一样**，而角色是**编译期**定死的
  //     （§5：编译期 env 是唯一权威、运行期没有任何代码能改它）⇒ 手里这块板到底是哪
  //     一角色，只有"刷进去的那份固件"知道。把它写进诊断页，就是让这件事**看得见**。
  //   ★ 数值用 `uint8_t` 而不是 `FieldSource` 那类枚举：本文件刻意不 include
  //     `link_role.h`（同上面 ① 的理由，那会把 link_frame 一整串拖进来）。
  uint8_t  link_role = 0;

  // ---- ⑤ OBD（未接就是"未连接"）----
  bool    obd_enabled = false;
  uint8_t obd_speed_supported = 0xFF;   // -1/0/1（0xFF = 未知）
  bool    obd_speed_polled = false;
  bool    obd_support_known = false;
  float   obd_rpm_hz = 0.0f, obd_coolant_hz = 0.0f;
  float   obd_intake_hz = 0.0f, obd_speed_hz = 0.0f;

  // ---- ⑥ 内存 / 面板 / 帧率（串口已有，只是换个出口）----
  uint32_t heap_free_kb = 0;
  uint32_t psram_free_kb = 0;
  bool     psram_present = false;
  uint32_t ui_fps10 = 0;          // 渲染帧率 ×10（0.1 fps 分辨率；整数，免浮点）
  uint32_t display_frames = 0;    // 面板累计帧数（驱动不上报时 = 0）
  bool     display_stats_known = false;
  uint32_t display_fps10 = 0;
  uint32_t flush_max_us = 0;
  uint32_t copy_max_us = 0;
  uint32_t panel_mask_px = 0;     // 预览遮罩提醒过的像素数（设备端恒 0）

  // ---- ⑥.5 面板健康守护（**第 ② 层防线**，见 lib/dashcore/panel_guard.h）----
  // ★ 为什么这一格要上诊断页：业主的要求是"**仪表盘必须常亮**"，
  //   而"黑屏风险"在此之前只能从"屏是不是黑的"看出来（那就太晚了）。
  //   把守护的读数放上屏 ⇒ 它**变成可看到的数据**：
  //     `rd_ok` 在涨 = 守护在跑（每 2 秒一次；0 = 守护没上线/没跑起来）；
  //     `fix`/`bl` 不为 0 = 真的发生过"扩展器位被改写 / 背光被改"并且**已经修回来**；
  //     `anomaly` 不为 0 而 `fix` 是 0 = 查到过异常但没修（那是另一回事）。
  // ★ 设备端之外的构建（预览/抓帧盒）恒为 0 —— 那些构建里根本没有面板可守。
  uint32_t guard_rd_ok = 0;       // 回读成功的次数
  uint32_t guard_fix = 0;         // 按影子重写扩展器的次数（= 救回来的次数）
  uint32_t guard_bl = 0;          // 背光被重设的次数
  uint32_t guard_anomaly = 0;     // 发现不一致的次数（含已修的）
  uint32_t guard_uptime_ms = 0;   // 守护上线多久（诊断页那一行要的是"它还在跑吗"）

  // ---- ⑥.6 ★★ 跨重启留档（2026-09-25 新增，见 lib/dashcore/boot_persist.h）----
  // ★ 为什么要有这一格：上面那四个数**一重启就归零** ⇒ "某次夜里守护救过几回"
  //   正好在黑屏事故最需要它的时候丢掉。这里给的是**累计**（跨重启）+ 两个"新不新"的读数。
  // ★ 口径（别读歪）：
  //   · `guard_total_*` = **上一次开机为止的累计 + 本次**（开机时把本次的基数写进 NVS，
  //     之后搭 10 分钟一次的心跳落盘）⇒ 它是"这块板历史上总共"的数；
  //   · `guard_tot_snaps` = 累计值**落盘过几次**（跨重启单调 +1）：
  //     它变了 ⇒ 从那一次起累计值里含了上一次运行的尾巴；
  //   · `boot_count` = 第几次上电/复位（既有的 `bootn`）；`kBootCountUnknown` = 没记录；
  //   · `boot_hb_n` = 心跳**累计**写过几次（0 且开机不到 10 分钟也是正常的）。
  // ★ 设备端之外的构建（预览/抓帧盒）恒为 0 / `-` —— 那些构建里没有 NVS。
  uint32_t guard_total_rd = 0;
  uint32_t guard_total_fix = 0;
  uint32_t guard_total_bl = 0;
  uint32_t guard_total_anom = 0;
  uint32_t guard_tot_snaps = 0;
  uint32_t boot_hb_n = 0;
  uint32_t boot_count = 0;

  // ---- ⑦ 告警 / 提示音状态（诊断页要显示"静音开关"）----
  bool     beep_muted = false;
  uint8_t  alert_active = 0;      // (uint8_t)AlertKind
};

// 诊断页的行数上限。
// ★ 为什么存**成品字符串**而不是"格式串 + 参数"：诊断页 5 Hz 重画，而
//   "哪一格显示什么"是纯逻辑（native 逐字对账）、"变成像素"是 dash_ui 的事
//   —— 与 lamp_view.h 把"亮不亮"与"怎么画"分开是同一条分层。行数上限 16：
//   最长那一页 11 行，留出余量。
static const uint8_t kDiagMaxLines = 16;

// 诊断页那一格"第几次上电/复位"的**没有记录**哨兵（= `boot_persist.h` 的
// `kBootUpUnknown` 同一个数）。★ 本文件刻意**不 include** `boot_persist.h`：
// 那一份是设备侧的持久化层，而这一份是"屏上显示什么"的纯映射 —— 两者只有一个
// 约定的常量相同，各自持有比互相拉依赖干净（数值由 `test_boot_persist.cpp` 与
// `test_system_status.cpp` 两边各钉一次）。
static const uint32_t kBootCountUnknown = UINT32_MAX;

// 诊断页的两页。★ 为什么**只有两页**：内容要"平时不显示、进去才看"，
//   页数多了就得设计翻页 UI（那本身就是新交互）；两页刚好一屏一页，
//   用同一个键循环（K），退出用 Esc/X（与"全部复位"同一个键）。
enum class DiagPage : uint8_t { Sys = 0, Link, Count };
static const uint8_t kDiagPageCount = (uint8_t)DiagPage::Count;
const char* diagPageTitle(DiagPage p);

// ------------------------------------------------------------
// 三、数据可信度状态机
//
// 用法（与 data_service / alerts 同一个套路）：主循环每圈调一次
//     const DataTrustReason r = g_sys.update(inputs, now_ms);
//     if (g_sys.beepDue()) { beep(); }      // 一声轻提示（受静音与限速约束）
// ------------------------------------------------------------
class SystemStatus {
 public:
  // 推进状态机，返回**当前该显示的那一条**（`kNone` = 屏上不显示角标）。
  DataTrustReason update(const SysStatusInputs& in, uint32_t now_ms);

  // ---- 只读状态（UI / 日志 / 诊断页用）----
  bool untrusted() const { return active_ != DataTrustReason::kNone; }
  DataTrustReason active() const { return active_; }
  // 从"可信"到"不可信"的**首次**跳变计数（诊断页显示；也是用例的抓手）
  uint32_t episodes() const { return episodes_; }
  // 这一拍该不该"轻提示一声"。★ 每次变坏只置一次，且与上一声至少隔
  // `kTrustBeepMinIntervalMs`；调用方读完即清（下一次 update() 会清）。
  bool beepDue() const { return beep_due_; }

  // 清空记忆（换数据源/复位时用）。**不清** `episodes_` 与上一声的时刻 ——
  // 累计统计不该因为一次复位就查不出来（与 Alerts::reset 同一条口径）。
  void reset();

 private:
  // 去抖：条件**连续**成立 `need_ms` 才算（与 alerts.cpp 的 debounce 逐字同一条）。
  struct Cond {
    bool on = false;
    bool run = false;
    uint32_t since_ms = 0;
  };
  static void debounce(Cond& c, bool raw, uint32_t now_ms, uint32_t need_ms);

  // 数据冻结：记住上一次的**原始字节**，只要有一个字节变了就刷新窗口。
  // ★ 用 memcmp 比 float 而不是 `==`：NaN 与 -0.0 这两种值用 `==` 比会得出
  //   "没变"或"变了"的反直觉结果，而这里要回答的只是"字节流有没有动过"。
  float last_speed_ = 0.0f;
  float last_rpm_ = 0.0f;
  bool  freeze_armed_ = false;      // 有没有过一次基线
  uint32_t freeze_since_ms_ = 0;    // "值开始不变"的时刻

  Cond bad_{};                      // "不可信"这一路的去抖
  Cond good_{};                     // "恢复"这一路的去抖
  DataTrustReason raw_ = DataTrustReason::kNone;   // 去抖前的原始判据（最高优先级那条）
  DataTrustReason active_ = DataTrustReason::kNone;
  bool freeze_now_ = false;         // 去抖前的"冻结"结论（evaluate 内部用）

  uint32_t episodes_ = 0;
  bool     beep_due_ = false;
  uint32_t last_beep_ms_ = 0;

  DataTrustReason evaluate(const SysStatusInputs& in, uint32_t now_ms);
};

// ------------------------------------------------------------
// 四、诊断页
//
// ★ **合规说明（重要，别让后来的人以为违反了裁决）**：
//   ARCHITECTURE.md §8 的 **L11** 定的是「主板『从板离线』**角标** v1 不做，
//   从板在线状态只进 `STATUS` 日志、不上屏」（owner 裁决，2026-09-22）。
//   本诊断页**不是常驻角标**：它平时**完全不显示**，只由长按/组合键主动唤出
//   （pcpreview 是 `K` 键），退出即消失 ⇒ 与 L11 不冲突。L11 禁的是"让从板
//   在线状态**常驻**在表盘上"，而诊断页是**主动查询**式的（与插上 USB-C 看
//   串口是同一类行为，只是换了个出口）。
//   ★ 同理，§3 表 `STATUS` 超时门限 30 s（原 L13「只用于日志、不上屏」）
//     也**没有被动过**：诊断页读的是从板自己的 `LinkTime` 状态，不是主板对
//     从板在线性的判断。
// ------------------------------------------------------------
// 诊断页一行的文本缓冲长度。
// ★ 40 字节是**按最坏一行算出来的**：480 档 12 号字宽约 6 px ⇒ 一屏最多约
//   68 列，而 240 档（历史 DualEye）只有 34 列 ⇒ 40 是一个两边都放得下的量级；
//   真超了由 snprintf 截断（截断比溢出好，且截断在 ASCII 边界上）。
static const uint8_t kDiagLineCap = 40;

// 一页诊断页的内容。
// ★ 存的是**已经格式化好的文本**（不是格式串 + 参数）：诊断页 5 Hz 重画，
//   而"哪一格显示什么"是纯逻辑（native 逐字对账）、"变成像素"是 dash_ui 的事
//   —— 与 lamp_view.h 把"亮不亮"与"怎么画"分开是同一条分层。
//   ★ 也刻意不使用"格式串穿到 UI 再 snprintf"：那要把可变参数穿过结构体，
//     `-Wformat-extra-args` 在两个编译器上抱怨的方式不同，而这是纯赔本的复杂。
struct DiagView {
  const char* title = "";                              // 页面名（"DIAG" / "DIAG-LINK"）
  char        line[kDiagMaxLines][kDiagLineCap] = {};  // 每行文本（空串 = 这一行不画）
  uint8_t     lines = 0;
  bool        muted = false;                           // 静音开关现在的状态（页面上要显示）
};

// 把输入落成一页（`page` 越界 ⇒ 自动折回第 0 页）。**纯函数**。
DiagView diagBuild(DiagPage page, const SysStatusInputs& in);

// 把一页拼成一段文本（每行一个 '\n'；`buf` 由调用方给，不做动态分配）。
// 返回写入的行数。用途：native 用例逐字对账"哪一格显示什么"
// （比在像素上断言稳得多），以及文档里贴"诊断页长什么样"。
int diagRenderText(const DiagView& v, char* buf, size_t cap);

// ------------------------------------------------------------
// 五、几个小工具（纯函数，用例直接调）
// ------------------------------------------------------------
// 年龄 → "1234" / "-"（拿不到就是 "-"）。
// ★ 不许把 UINT32_MAX 直接打出来：那是 4294967295，到车上会被读成"这个数有意义"
//   （仓库里为同一件事踩过一次，见 main.cpp 的 SRC-VAN age 那几行）。
void fmtAgeMs(char* buf, size_t cap, uint32_t age_ms);
