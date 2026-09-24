#include <Arduino.h>
#include <string.h>     // memcmp:pcpreview 的注入快照比对(设备侧也编,无副作用)
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

// ★ 单板回环验证固件（`env:esp32s3-linkloop`）要与本文件**二选一**：同一个 env 只能
//   有一个 setup()/loop()。开了这个宏就把整个 main.cpp 摘掉，由 src/link_loopback.cpp
//   提供入口（见那个文件的头注释）。
//   ★ 为什么用宏、不用 platformio 的 build_src_filter 排除文件：宏是"少编一段代码"，
//     IDE 里看得见、LDF 也不会因为少一个 .cpp 而少扫出依赖；两者效果一样但前者更难踩坑。
#if !defined(LINK_LOOPBACK_FIRMWARE)

// ★ 开机自检与 esp_partition 只有**设备端**才有(宿主机没有 ESP.* / esp_partition.h)。
//   用 ARDUINO 判定:真 Arduino 框架(esp32dev / esp32s3)会定义它,
//   pcpreview 的宿主机桩不定义 —— 于是同一份 main.cpp 两端都能编。
#if defined(ARDUINO)
#include <esp_partition.h>
#include <esp_timer.h>   // 上电示位标(见 g_boot_stage / beacon_cb)
#define DASH_DEVICE_SELFTEST 1
#endif
#include "data_service.h"
#include "dash_display.h"
#include "dash_ui.h"
#include "alerts.h"       // 告警层:只用已解字段(超速/红区/门/转向灯忘关)
#include "buzzer.h"       // 蜂鸣器抽象(L14 建议 = 从板本地发声;真机实现待 L14 点头)
#include "expression.h"   // Face（STATUS.left_face 要报"左屏当前档位"，§3）
#include "image_load.h"
#include "link_app.h"     // 双板链路 v1 的应用层接线（§1.2 ③ / §3 / §4 / §5）
#include "link_phy_pins.h"
#include "link_role.h"    // LINK_ROLE / kLocalRole（编译期是唯一权威，§5）
#include "link_time.h"
#include "link_tx.h"
#include "theme_store.h"
#include "system_status.h"   // 数据不可信提示 + 诊断页（判据/映射都在 lib/dashcore/）
#include "ui_model.h"
#include "van_phy.h"
#include "van_replay.h"

// pcpreview 的输入注入（键盘 + preview/inject.txt）—— 只在这个 env 里编
// （实现整个在 src/preview_input.cpp 的 `#if defined(DASH_DISPLAY_PREVIEW)` 里）。
#if defined(DASH_DISPLAY_PREVIEW)
#include "preview_input.h"
#endif

#if LINK_ROLE == 1
// 链路物理层（真实 UART）只在**主板**这一侧编译：从板（LINK_ROLE==0）不发车数据、
// 只收 —— 而"收"用的还是同一个 LinkPhyUart 类，但把类编进来会拉上 link_phy_uart.cpp
// 那条"链路 UART 与日志 UART 不许是同一个"的编译期闸门（判据现在是 dash_log.h 的
// DASH_LOG_UART0，见那个文件头）。本轮按 §5 的口径只把**主板**这一侧
// 接上（编译期 env 是唯一权威）；从板的接收路径由 lib/link 的用例整条覆盖
// （test/test_dashcore/test_link_app.cpp 的端到端那一条：发 → 收 → 喂进 data_service）。
#include "link_phy_uart.h"
#endif

// ============================================================================
//  OBD(K 线)串口接线 —— 2026-09-21 启用(此前一直是注释,设备端从不问 OBD)
// ============================================================================
//  ELM327 USB(CH340,带开关)的**数据线**接 S3 的 **UART1**:
//      ELM327 TX  ->  ESP32 GPIO17(OBD_RX_PIN,板子收)
//      ELM327 RX  <-  ESP32 GPIO18(OBD_TX_PIN,板子发)
//      GND        <-> GND        两个 USB 只取数据,**别把 5V 对插**
//  波特率 38400 8N1(ELM327 默认;克隆板个别是 9600,换这个数值得改 kObdBaud)。
//
//  ★ 怎么核对接线:看开机后串口里那两行 ——
//      obd: ECU 位图 0x… -> 车速(010D)支持/不支持,车速轮询已开/关闭
//      SRC-Hz rpm=… cool=… intake=… speed=…    ← 每个字段的**实测**刷新率
//    位图那行**一直不出现** = 板子发出去的 AT 序列 ELM327 没回 ⇒ 优先把
//    17/18 对调再试(廉价克隆板的 K 线可能只有单方向能跑,见 PINOUT.md 那条注)。
//    没有 OBD 硬件时编译加 -DOBD_SERIAL=0,退回 Sim 假数据。
//
//  0100 位图与"要不要给 010D 一个轮询时隙"的关系见 lib/dashcore/obd_source.h;
//  车速级优先级(Van > Obd > Sim)见 data_service.h。
//  Serial1(UART1)在本仓库其余地方**没有被用过**,不存在串口互抢。
#if !defined(OBD_SERIAL)
#define OBD_SERIAL 1
#endif
#if !defined(OBD_RX_PIN)
#define OBD_RX_PIN 17
#endif
#if !defined(OBD_TX_PIN)
#define OBD_TX_PIN 18
#endif
static const uint32_t kObdBaud = 38400;

static VehicleDataService g_data(nullptr);

// ============================================================================
//  告警层 + 蜂鸣器（2026-09-24 新增）
// ============================================================================
// 触发源**只用已解字段**：超速（车速）/ 转速红区（5800）/ 门（"动过"）/
// 转向灯亮太久。判据、去抖、最短重复间隔、静音全在 lib/dashcore/alerts.*
// （纯逻辑，native 用例逐条测掉），这里只做三件事：
//   ① 每轮喂一份快照给 Alerts；
//   ② 按它的结论驱动蜂鸣器（Buzzer 抽象后面的实现）；
//   ③ 把结论交给 UI（灯位闪烁 + 告警描边）。
//
// ★ 蜂鸣器挂哪个实现（**这是 §8 L14 的落点**）：
//   · 现在挂的是 `BuzzerNull`（不发声）—— 因为 2.8C 还没到货、
//     而且 L14「由哪块板发声」**还没裁决**（本轮按"从板本地发声"**建议**实现，
//     见 buzzer.h 的文件头）。
//   · pcpreview 上换成 `BuzzerHost`（打印 `BEEP pattern=… ms=…` 一行，
//     可选 -DBUZZER_HOST_SOUND=1 出系统提示音）⇒ 模拟页上能看见"什么时候会响"。
//   · 真机实现是**另一个 Buzzer 子类**（TCA9554 的 EXIO8，零额外引脚）——
//     L14 一旦点头，只改下面这几行的构造，alerts 与 UI 一行都不用动。
//     在那之前**不写一个没验过的 I2C 写时序**（没硬件，验不了）。
static Alerts g_alerts;
static BuzzerNull g_buzzer_null;
#if defined(DASH_DISPLAY_PREVIEW)
static BuzzerHost g_buzzer_host;
#endif
static Buzzer* g_buzzer = &g_buzzer_null;

// 告警状态的一行回执（只在**变了**的时候打：每 5 秒那行只报"当前是什么"）。
static uint8_t g_alert_last = 0xFF;   // 0xFF = 还没打过
static uint32_t g_beep_seen = 0;

// ============================================================================
//  系统状态层（2026-09-24 新增）—— 「数据不可信」提示 + 诊断页
// ============================================================================
// ★ 产品要求（原文见 ARCHITECTURE.md 的显示约定一节）：
//   **当仪表显示的不是实测数据时，必须在屏上让驾驶员看得出来。**
//   判据/去抖/限速/诊断页的字段映射**全部**在 lib/dashcore/system_status.*
//   （纯逻辑，native 用例逐条钉住），这里只做四件事：
//     ① 每拍把 `SysStatusInputs` 拼出来（**全部来自既有信息，零新数据源**）；
//     ② 把结论交给 dash_ui（角标 + 诊断页）；
//     ③ 按 `beepDue()` 响一声轻提示（走既有 `Buzzer` 抽象 + 既有静音开关）；
//     ④ 静音开关的**掉电保存**（见下面 load/save 的说明）。
static SystemStatus g_sys;
// VAN 帧的**本机计数**：`van.raw_frames` 那一行用。
// ★ 为什么在 main 自己数而不是读物理层的 `VanPhyWire::Stats`：
//   ① 物理层的统计只在 `VanPhyGpio` 上有（`VanPhyStub` 没有这个接口），而
//      `g_van_phy` 的类型是编译期二选一 ⇒ 在 main 里读它会变成一串 #if；
//   ② 这个数要的语义是"**我这一侧**解出了多少帧"，而 sink 是每条路径
//      （物理层 / 串口回放）的唯一汇合点 ⇒ 在这里数最准，也最省。
//   ③ 真正的线上统计（edges / fcs_ok / 队列溢出）仍然由物理层那一行
//      `van: edges=… frames=…` 每秒打在串口上 —— 诊断页只是"换个出口"，
//      不替代它。
static uint32_t g_van_frames_seen = 0;
static uint32_t g_van_frames_fcs_ok = 0;
static uint32_t g_van_last_rx_ms = 0;
// 渲染帧率的 EMA（×10 定点，0.1 fps 分辨率）。为什么在 main 里算：
//   渲染是主循环按 200 ms 节流调的，帧率就是"这条节流有没有被卡住"的直接证据
//   （RGB 那条路上第一次整屏刷新要 ≈1 秒 ⇒ 那一秒 fps 会掉下来）。
//   用整数 EMA（alpha = 1/8）而不是浮点：只为了一个显示值不值当用浮点。
static uint32_t g_ui_fps10 = 0;
// 静音开关的掉电保存（设备端用 Preferences/NVS；预览端没有 NVS）。
// ★ 口径：**默认有声**（= 不静音），静音是车主的**选择**，要能跨上电记住。
static bool g_beep_muted = false;
#if defined(ARDUINO)
#include <Preferences.h>
static Preferences g_prefs;
static const char* kPrefsNamespace = "dash";
static const char* kPrefsMuteKey   = "mute";
static void mute_load() {
  if (!g_prefs.begin(kPrefsNamespace, /*readOnly=*/true)) return;   // 没有 NVS ⇒ 保持默认
  g_beep_muted = g_prefs.getBool(kPrefsMuteKey, false);
  g_prefs.end();
}
static void mute_save(bool m) {
  if (!g_prefs.begin(kPrefsNamespace, /*readOnly=*/false)) return;
  g_prefs.putBool(kPrefsMuteKey, m);
  g_prefs.end();
}
#else
// 预览端：静音状态由 `preview/inject.txt` 的 `mute=` / 键 `M` 管（见 preview_input.h），
// 不落盘 —— 落盘会让"重新跑一次预览"继承上一次的静音，演示时反而容易误会。
static void mute_load() {}
static void mute_save(bool) {}
#endif  // ARDUINO

#if defined(DASH_DISPLAY_PREVIEW)
// pcpreview 的注入快照 + 它改过的字段。设备固件里没有这一段。
static PreviewInput g_preview;
static PreviewInput g_preview_prev;
static uint32_t g_lamp_pulse_ms = 0;
static bool g_lamp_pulse = false;
// ★ 2026-09-24：告警那一拍的"屏上闪"要**落进帧里**才算数。
//   起因（实测会误导人）：`Alerts::beeping()` 只持续 `beep_ms`（120ms），
//   而预览**每 200ms 才落一帧** ⇒ 有将近一半的落帧时刻那一拍已经过去了。
//   于是"按 O 触发超速，屏上却没闪"会时不时出现 —— 而判据本身是对的
//   （真机 60fps 下每次都看得见），纯粹是**采样**问题。
//   修法：脉冲开始时记住当时的帧号，**一直保持到显示侧真的落了新的一帧**
//   （`dash_display_preview_frames()` 变了）为止。判据/去抖/蜂鸣器一个字
//   都没改（那三层在 lib/dashcore/alerts.*，native 用例逐条钉着）——
//   这里只是"让那一拍活到被拍下来"。
static uint32_t g_lamp_pulse_frame = 0;
static bool g_lamp_pulse_armed = false;

// ★ 2026-09-24（第二轮）：预览里演示"数据不可信提示"的那一位。
//   `sim_ok` **不是车辆状态**（见 preview_input.h 的说明），所以它不进
//   `preview_apply_snapshot()`，只在这里单独取出来给 sys_inputs_build 的调用点用。
//   ★ 它的含义是"**把这一拍的数据层模拟成实测**"（来源 Van + VAN 帧新鲜）——
//     因为 pcpreview 上本来就没有 VAN，判据（正确地）认为"数据不可信"一直成立。
static bool g_sim_ok = false;
// 诊断页"翻页请求"的上一拍取值（边沿检测用；见 loop 里那一段）。
static bool g_diag_req_last = false;

// 把注入施加到快照上。
// ★ 施加的位置在 `g_data.update()` **之后**、`make_view()` **之前**：
//   注入是"人替传感器说话"，所以它压过数据层的合并结果；
//   而它**不回流**进 data_service（不碰优先级、不产生协议行为，见 preview_input.h ③）。
static void preview_apply(const PreviewInput& in, VehicleState& st) {
  // ★ 用"与上一帧比**变了**没有"来决定要不要打印：控制文件是每帧重读的，
  //   不变时刷屏没意义。
  const bool changed = memcmp(&in, &g_preview_prev, sizeof(PreviewInput)) != 0;
  g_preview_prev = in;
  if (changed) {
    // ★ 遮罩那一位（`mask`）与车辆状态**分开报**：`in.any()` 只看车状态，
    //   所以"只按了 V"这一种变化单列一行 —— 否则按 V 之后屏幕上一行回执都没有，
    //   而"按了没反应"是最难查的现象。
    if (in.any()) {
      dash_logf("inject: L=%d R=%d haz=%d low=%d pos=%d door=%d spd=%.0f rpm=%.0f mute=%d\n",
                (int)in.left, (int)in.right, (int)in.hazard, (int)in.low_beam,
                (int)in.position, (int)in.door, in.speed_kmh, in.rpm, (int)in.mute);
    }
    dash_logf("inject: round mask = %s (2.8C tier; key V)\n", in.panel_mask ? "ON" : "OFF");
    dash_display_preview_set_panel_mask(in.panel_mask);
  }
  // ★ 施加的**纯逻辑**在 lib/dashcore/preview_input.h 的 preview_apply_snapshot()
  //   （双闪与左右箭头的合并语义、*_set 的"没注入过 ≠ 注入成 0"都在那里，
  //    由 native 用例逐条钉住）—— 这里只留下"回执 + 遮罩推送 + 静音"三件 IO。
  preview_apply_snapshot(in, st);
  g_alerts.setMuted(in.mute);
}
#endif  // DASH_DISPLAY_PREVIEW
#if defined(ARDUINO)
// 挂上 OBD 串口,并把 VehicleDataService 的指针换成 &Serial1。
// ★ 必须在 setup() 里做,不能在静态初始化期做:Serial1 的 begin() 要等
//   运行时(时钟/外设都就绪)才安全。
// 返回该服务,setup() 里这样用:`g_data = attachObdSerial();`
VehicleDataService& attachObdSerial() {
#if OBD_SERIAL
  Serial1.begin(kObdBaud, SERIAL_8N1, OBD_RX_PIN, OBD_TX_PIN);
  g_data = VehicleDataService(&Serial1);
  dash_logf("obd: UART1 已挂上 ELM327, RX=GPIO%d TX=GPIO%d @%u 8N1\n",
            (int)OBD_RX_PIN, (int)OBD_TX_PIN, (unsigned)kObdBaud);
#else
  dash_logf("obd: 未启用(-DOBD_SERIAL=0),只跑 Sim 假数据\n");
#endif
  return g_data;
}
#else
// ============================================================================
//  pcpreview（宿主机）：**没有 UART1，也没有 ELM327**
// ============================================================================
// ★ 这一段是 2026-09-24 补的，补的是一个**一直存在的缺口**（不是本轮引入的
//   行为改变）：`g_data = attachObdSerial()` 那句调用**没有**被 `#if defined(ARDUINO)`
//   圈起来，而函数**只在 ARDUINO 那一支里定义** ⇒ pcpreview 编译到那句时就报
//   `use of undeclared identifier 'attachObdSerial'`，**整个宿主机预览编不出来**。
//   （实测复核：拿 `git show HEAD:src/main.cpp` 用 pcpreview 的**逐字编译参数**
//     预 processed 一遍，`attachObdSerial()` 只剩调用点、没有定义 ⇒ 起点上就编不过。
//     仓库里的 `.pio/build/` 也只有 `esp32dev` 一个目录 ⇒ pcpreview 在本机
//     从来没成功编过一次。所以本轮"pcpreview 要 SUCCESS"这条必须先把这里修掉。）
//
// 修法刻意选**"给预览一份显式的空实现"**，而不是把调用点也圈进 ARDUINO：
//   · 预览里 OBD 本来就该是关的（宿主机没有 Serial1；`OBD_SERIAL` 默认 1），
//     所以这里 `g_data` 保持构造时的 `nullptr`（= `ObdSource` 不启用），
//     与"没插 OBD"完全同一条路径 —— 预览的弧/表情继续由 `sim_source` 驱动。
//   · 而且它会**明确打一行日志**。圈掉调用点的话是"静默地没有"，而这正是
//     最难查的一类（"预览里为什么没有 OBD"根本不会有人问，直到真车对不上）。
VehicleDataService& attachObdSerial() {
  dash_logf("obd: pcpreview 宿主机没有 UART1/ELM327 ⇒ 不启用 OBD(弧/表情走 Sim 假数据)\n");
  return g_data;
}
#endif  // ARDUINO

// VAN 物理层:默认是桩(无硬件)。
//   ★ 收发器(SN65HVD230)到货后:编译时加 -DVAN_PHY_GPIO=1
//     (见 platformio.ini 的 [env:esp32s3]),RO 接 GPIO16,D/TX 接 3V3(★ 不能悬空:低=显性=会主动干扰总线)、RS 接 GND。
//   换成 VanPhyGpio 之后,数据层一行都不用改 —— 这正是当初把它抽成
//   VanPhy 接口的目的(见 van_phy.h)。
// 数据源不是 VanSink,用 VanSourceSink 转一层;而我们要**打印**每一帧,
// 所以在中间再插一层 VanLogSink(见下)。
static VanSourceSink g_van_sink(&g_data.vanSource());

#if defined(VAN_PHY_GPIO)
#include "van_phy_gpio.h"
static VanPhyGpio g_van_phy;
#else
static VanPhyStub g_van_phy;
#endif

// ============================================================================
//  VAN 帧嗅探(编译期开关) —— 2026-09-22 新增
// ============================================================================
// 为什么要有它:实测发现 VanLogSink 那句 "VAN %03X ..." 帧行**一行都没打印**
//   (格式串确实在固件里、sink 链接法也对、dash_logf 无节流、frames 计数在涨)。
//   后果不只是"少看几行日志":**实车流里到底有哪些 IDEN 变得无法观察** ——
//   而"车速(IDEN 0x824)在不在流里"直接决定 SRC speed 能不能从 sim 变成 van。
//
//   所以这个开关做两件事:
//     ① 按 IDEN 计数(不依赖任何字符串格式化)—— 绕开"帧行不打印"那条路,
//        用最朴素的方式回答"流里有哪些帧、各多少";
//     ② 每秒打一行 top-N + 总帧数 —— 如果这一行能出来而 "VAN %03X" 出不来,
//        就把问题缩小到"那一条 dash_logf 调用"上,而不是整个 sink。
//
// 用法:python -m platformio run -e esp32s3-vansniff -t upload --upload-port COM3
//   ★ 默认关(VAN_SNIFF=0):嗅探表占 4096×2B + 每轮一次扫描,不该进常规固件。
#if !defined(VAN_SNIFF)
#define VAN_SNIFF 0
#endif

#if VAN_SNIFF
namespace {
// 12 位 IDEN ⇒ 4096 个桶。uint16_t 计数够(每秒最多几百帧,溢出前早打过日志了)。
uint16_t g_iden_count[4096] = {};
uint32_t g_iden_total = 0;
uint32_t g_iden_last_report_ms = 0;

void van_sniff_note(uint16_t iden) {
  ++g_iden_count[iden & 0x0FFFu];
  ++g_iden_total;
}

void van_sniff_report(uint32_t now_ms) {
  if (now_ms - g_iden_last_report_ms < 1000) return;
  g_iden_last_report_ms = now_ms;
  if (g_iden_total == 0) {
    dash_logf("sniff: 还没解出任何帧\n");
    return;
  }
  // ★ 全量输出(2026-09-22 改):原来只打 top-6,但那样**答不了"总线上有多少种帧"**
  //   —— top-6 只覆盖约 84%,剩下的低count IDEN 看不见。现在先报种类数,
  //   再把**所有**出现过的 IDEN 按计数降序打完(12 位 IDEN 最多 4096 种,
  //   实车就十几种,每行 220 字节够装 14 个左右,必要时分行)。
  uint16_t present = 0;
  for (int i = 0; i < 4096; ++i) if (g_iden_count[i]) ++present;
  dash_logf("sniff: 共 %lu 帧 / %u 种 IDEN\n",
            (unsigned long)g_iden_total, (unsigned)present);

  // 选择排序:每轮挑剩下的最大者。实车种类少,直接 O(n²) 扫 4096 也行。
  bool shown[4096] = {};
  char buf[220];
  int n = 0;
  buf[0] = 0;
  for (uint16_t pick = 0; pick < present; ++pick) {
    uint16_t best = 0;
    bool found = false;
    for (int i = 0; i < 4096; ++i) {
      if (shown[i] || g_iden_count[i] == 0) continue;
      if (!found || g_iden_count[i] > g_iden_count[best]) { best = (uint16_t)i; found = true; }
    }
    if (!found) break;
    shown[best] = true;
    // 一行装得下就续着写,装不下就先 flush
    if (n > (int)sizeof(buf) - 24) {
      dash_logf("sniff:   %s\n", buf);
      n = 0; buf[0] = 0;
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%03X=%u ",
                  (unsigned)best, (unsigned)g_iden_count[best]);
  }
  if (n > 0) dash_logf("sniff:   %s\n", buf);

  // 车速帧单独点名 —— 它决定 SRC speed 能不能从 sim 变 van
  dash_logf("sniff: 车速帧 0x824 = %u%s\n",
            (unsigned)g_iden_count[0x824],
            g_iden_count[0x824] ? "" : "  <-- 流里没有这一帧,所以 speed 只能是 sim");
}
}  // namespace
#endif  // VAN_SNIFF

// 打印每一帧 VAN,格式**故意与 van_replay 的行格式一致**:
//     VAN 824 18 F8 27 10 00 00 00
// 于是"车上抓到的串口日志"可以直接粘回设备的串口(或喂给宿主机测试)来回放 ——
// 抓帧、分析、复现用的是同一份文本,不用转换。
// 校验不过的帧单独用 '# ' 开头打印(它不能拿去回放,但"有没有收到东西"要看它)。
class VanLogSink : public VanSink {
public:
  // 只要 824(车速/转速)这一帧?先全打 —— 反查协议时缺的就是"别的帧长什么样"。
  void onPacket(const VanPacket& pkt) override {
    // ★ 诊断页要的两个数（2026-09-24）：本机解出的帧数 + 最近一帧的时刻。
    //   在这里数（而不是读物理层的 Stats）的理由见 `g_van_frames_seen` 的说明。
    //   它两条路径都覆盖：GPIO 物理层收帧、以及串口离线回放。
    ++g_van_frames_seen;
    if (pkt.fcs_ok) ++g_van_frames_fcs_ok;
    g_van_last_rx_ms = pkt.rx_ms;
#if VAN_SNIFF
    van_sniff_note(pkt.iden);   // ★ 计数不依赖任何字符串格式化,绕开"帧行不打印"
#endif
    if (!pkt.fcs_ok) {
      dash_logf("# VAN 校验失败 iden=%03X len=%u\n", (unsigned)pkt.iden, (unsigned)pkt.len);
    } else {
      dash_logf("VAN %03X", (unsigned)pkt.iden);
      for (uint8_t i = 0; i < pkt.len; ++i) dash_logf(" %02X", (unsigned)pkt.data[i]);
      dash_logf("   # cmd=%u ack=%u\n", (unsigned)pkt.cmd, (unsigned)pkt.ack);
    }
    if (next_) next_->onPacket(pkt);   // 转发给数据源:打印归打印,数据照收
  }
  void setNext(VanSink* n) { next_ = n; }

private:
  VanSink* next_ = nullptr;
};

static VanLogSink g_van_log;

// ============================================================================
//  双板链路 v1 的接线（**两个角色共用**：收帧 → 路由）
// ============================================================================
//  契约：ARCHITECTURE.md「## 双板链路协议 v1 范围」的 §1.2（发送侧三条硬约束）、
//  §3（消息表）、§4（时基）、§5（角色）。
//
//  ★ 为什么"收"这一段两个角色共用：主板收 B 的 STATUS/EVENT（只进日志），从板收
//    A 的 TICK/DATA（进数据层）——**同一套解帧、重同步、角色自检**，只是消费方式
//    不同。所以两边的解帧都走 `dashlink::LinkRx`，且都只从主循环里 poll。
//
//  ★ 三条纪律（§1.2，违反了不会有编译期信号）：
//    ① **不在 ISR/回调里发**：链路帧只进 `LinkTx` 的环（纯内存），真的写 UART 的
//       只有主循环末尾那两次 `pump()`；VAN 的 ISR（`van_isr_thunk`）一行都不碰链路。
//    ② **不够就丢整帧**：`LinkTx::enqueue` 空间不够就整帧丢并计数；
//       `LinkPhyUart::availableForWrite()` 报 0 时一个字节都不写（绝不忙等）。
//    ③ **从快照发、不从回调发**：DATA 的内容来自 `g_data.update(now)` 的**返回值**
//       （主循环里的快照），而不是 VAN 帧回调里顺手发。
//
//  ★ 与日志口的关系：链路走 UART0（43/44，§0），而**日志已经不占 UART0 了** ——
//    §0「载体」那条待办（2026-09-23）已由 `lib/dashcore/dash_log.h` 的
//    `DASH_LOG_UART0` 收口：只要这份固件里有链路 PHY（`-DLINK_PHY_UART=1`，见
//    platformio.ini 的 [env:esp32s3]），`dash_log_begin()` 就不开 `Serial0`、
//    `dash_logf()` 也不写 `Serial0`，日志只走原生 USB-CDC ⇒ 43/44 上是**只有链路**
//    一个占用者（下面 `g_link_phy.begin(false)` 那一行）。
//    ★ 这条关系在编译期还有一道闸门看着（link_phy_uart.cpp）：谁要显式
//    `-DDASH_LOG_UART0=1` 把日志又放回 UART0，就与链路撞车、直接编不过。
#if LINK_ROLE == 1
static dashlink::LinkPhyUart g_link_phy;   // §0：115200 8N1，UART0，TX=GPIO43 / RX=GPIO44
static dashlink::LinkTx      g_link_tx;    // §1.2 ②：自有环 ≥512 B，整帧进出
static dashlink::LinkRx      g_link_rx;    // §2 的重同步 + §5 的角色冲突自检
static dashlink::TickGen     g_link_tick;  // §3 的 TICK（50 Hz / 20 ms）
static dashlink::DataSender  g_link_data;  // §3 的 DATA（跟随 0x824 到达，不另建定时器）

// 主板自己的固件版本/构建标记（§3 的 HELLO：`fw_ver` 与协议 `VER` **分开**）。
// 取值口径：仓库里没有既有编码（§3 也这么说），所以先定 0/0 并把出处写在这里 ——
// 真要拿它判"两块板是不是同一份固件"，得在两块板各自刷同一份固件时才可比。
static const uint16_t kLinkFwVer    = 0;
static const uint16_t kLinkBuildTag = 0;

static uint32_t g_link_hello_ms = 0;         // 上一次发 HELLO 的时刻（0 = 还没发过）
static uint32_t g_link_peer_ms  = 0;         // 最近一次收到**对端任何一帧**的时刻
static bool     g_link_hello_acked = false;  // 收到过对端 HELLO ⇒ 停止重发（§3）
#endif  // LINK_ROLE == 1

// ★ 一条"UART 还没接上"的桩 PHY —— **只在从板侧**用。
//
//   为什么从板侧本轮不接真 UART（这是**刻意的**，不是漏了）：
//     · 从板的接收路径与主板共用同一套 `LinkRx`/`LinkTime`/`applyLinkData`，
//       这套逻辑已经由 native 用例**整条**跑通（发一帧 → 过假 PHY → 收到 →
//       喂进 VehicleDataService → 断言值变成 Link，见 test_link_app.cpp）；
//     · 剩下没验的那一段是"这块板子上 43/44 的电平长什么样" —— 那是**最终 2.8"板**
//       到手之后的事（§5：两个角色 env 要等它落地；§8 L1 还留着 ⓐⓑⓒ 三条要实测）；
//     · 用真实 UART 的话，这块**裸 S3 devkit** 上从板会去读 GPIO44 —— 而那个脚接的是
//       板载 CH343P 桥（§8 L1），拿它当输入到底干不干净正是待实测的 ⓑ。
//   ⇒ 接真 UART 只需要把 g_link_phy_slave 换成 `LinkPhyUart`、在 setup 里 begin()
//      （一行），并恢复 platformio.ini 里那条闸门。**本轮不猜、不预置**。
class LinkPhyNull : public dashlink::LinkPhy {
 public:
  int available() override { return 0; }
  int read() override { return -1; }              // 非阻塞契约：没有就是 -1
  int availableForWrite() override { return 0; }  // 0 ⇒ 上层一个字节都不写（§1.2 ②）
  size_t write(const uint8_t*, size_t) override { return 0; }
  bool online() const override { return false; }  // 没接线 ⇒ 不读不写
};

#if LINK_ROLE != 1
static LinkPhyNull        g_link_phy_slave;  // ★ 见上面 LinkPhyNull 的说明（本轮刻意不接真 UART）
static dashlink::LinkRx    g_link_rx;        // 收：§2 的重同步 + §5 的角色冲突自检
static dashlink::LinkTime  g_link_time;      // §4：TICK 偏移估计 + 三级超时
#endif

static uint32_t last_ui_ms = 0;
static uint32_t last_status_ms = 0;
static uint32_t last_render_tick_ms = 0;   // 渲染帧率 EMA 用（见 g_ui_fps10）
static bool     g_obd_enabled = false;     // 这一份固件里 OBD 到底启没启用（诊断页）
// 这一拍的车状态快照（**供 `sys_inputs_build()` 读值**）。
// ★ 为什么单独存一份而不是多传一个参数：`sys_inputs_build()` 在 `loop()` 里
//   被调用时 `st_mut` 就在手边，但把它当参数传会让这个函数与"哪一份快照"
//   耦合；而诊断/判据要的只是"这一拍上屏的那两个数" ⇒ 在这里存一份**只读用途**
//   的副本最不容易读歪（写入点只有 loop 里那一处，就在算视图之前）。
static VehicleState st_state_cache;

// ============================================================================
//  把"这一拍的全部系统状态"拼成一份 `SysStatusInputs`
// ============================================================================
// ★★ 三条纪律（这一段的全部意义就在这三条上，别扩）：
//   ① **零新数据源**：下面每一个字段都能在 main.cpp 现有的那几行串口日志里
//      找到对应项（SRC / SRC-Hz / SRC-VAN / van: edges= / link: / BEACON 的
//      heap+psram）。这一层只是"把已有的数换个出口"（→ 屏上的诊断页）。
//   ② **不新增采集、不做判断**：判据全在 lib/dashcore/system_status.*。
//      这里连一次比较都不做（除了"有没有过帧"这种**存在性**事实）。
//   ③ **不引入协议/调度行为**：不造帧、不改优先级、不动任何时间基。
//
// ★ 时间戳的口径（最容易写歪的一处）：`van_age_ms` 用的是 **VAN 帧自己的
//   rx_ms**（与 data_service 的 3 秒回退判据**同一个时基**），不是"主循环
//   什么时候轮到" —— 用后者会把渲染节流也算成"数据变旧了"。
static SysStatusInputs sys_inputs_build(uint32_t now_ms) {
  SysStatusInputs in{};
  const DataSourceStatus& st = g_data.status();

  // ---- ① 数据来源档位（四格上屏的标量）----
  in.speed_src   = (uint8_t)st.speed;
  in.rpm_src     = (uint8_t)st.rpm;
  in.coolant_src = (uint8_t)st.coolant;
  in.intake_src  = (uint8_t)st.intake;

  // ---- ② VAN 断流 / 帧计数 ----
  in.van_ever_framed = (g_van_frames_seen != 0u);
  in.van_age_ms = in.van_ever_framed ? (uint32_t)(now_ms - g_van_last_rx_ms)
                                     : UINT32_MAX;
  in.van.frames = g_van_frames_seen;
  in.van.fcs_ok = g_van_frames_fcs_ok;
  // edges 只有物理层那一条路才有（串口回放的帧是"文本已经被信任"的 ⇒ 没有
  // 边沿统计）。没有物理层时照实为 0 —— 诊断页上把 frames 与 edges 一起看
  // 就能区分"这一档没有物理层"与"物理层收不到东西"（后者是 edges 涨、frames 为 0）。
  in.van.edges  = 0;

  // ---- ③ 数据冻结（既有快照的数值）----
  in.speed_kmh = st_state_cache.speed_kmh;
  in.rpm       = st_state_cache.rpm;
  // ---- ④ 双板链路（**仅从板**；主板没有 LinkTime）----
#if LINK_ROLE != 1
  in.link_known       = true;
  in.link_state       = (uint8_t)g_link_time.state();
  in.link_tick_age_ms = g_link_time.tickAgeMs();
  in.link_ticks_seen  = g_link_time.ticksSeen();
  in.link_seq_gaps    = g_link_time.seqGaps();
  in.link_seq_missing = g_link_time.seqMissing();
  in.link_offset_ms   = g_link_time.offsetMs();
#else
  // 主板：这一格**不是"不知道"**，而是"这件事不归它判" —— 从板在线性照
  // §8 的 L11/L13 只进日志（30 s 门限、只用于日志、不上屏）。
  // 诊断页那一行会写清楚（见 system_status.cpp 的 diagBuild）。
  in.link_known = false;
#endif

  // ---- ⑤ OBD ----
  in.obd_enabled         = g_obd_enabled;
  in.obd_support_known   = st.obd_support_known;
  in.obd_speed_polled    = st.obd_speed_polled;
  in.obd_speed_supported = (uint8_t)(int8_t)st.obd_speed_supported;
  in.obd_rpm_hz     = st.obd_rpm_hz;
  in.obd_coolant_hz = st.obd_coolant_hz;
  in.obd_intake_hz  = st.obd_intake_hz;
  in.obd_speed_hz   = st.obd_speed_hz;

  // ---- ⑥ 内存 / 帧率 ----
#if defined(DASH_DEVICE_SELFTEST)
  in.heap_free_kb  = (uint32_t)(ESP.getFreeHeap() / 1024u);
  in.psram_present = (ESP.getPsramSize() != 0u);
  in.psram_free_kb = in.psram_present ? (uint32_t)(ESP.getFreePsram() / 1024u) : 0u;
#else
  // 宿主机（pcpreview）：没有堆统计可报 ⇒ `heap` 报 0、psram 报"没有"。
  // ★ 这与"内存用光了"在数值上撞车，所以诊断页那一行**只在设备端**有意义 ——
  //   文档里写明（预览里看这一格是没意义的，看"数字在动"要看别的格）。
  in.heap_free_kb  = 0;
  in.psram_present = false;
#endif
  in.ui_fps10 = g_ui_fps10;

  // 面板统计：本轮 RGB 驱动**还没有**这个出口（它每秒把 frames/vsync/blit
  //   打进串口，但没暴露 getter）⇒ 照实报"驱动没上报"，诊断页显示 n/a。
  //   ★ 刻意不在这一轮去驱动里加 getter：那是别人的文件（蜂鸣器那一轮在改
  //     `dash_display_rgb.cpp`），而这一格"有则显示、没有就 n/a"本来就是对的设计。
  in.display_stats_known = false;

  // ---- ⑦ 告警 / 静音 ----
  in.beep_muted   = g_beep_muted;
  // ★ 用"回执那一份"：`g_alert_last` 的 0xFF 是"还没报过"的哨兵（不是一条告警）
  //   ⇒ 转成 0（= AlertKind::None）再给诊断页，免得屏上出现 alert=255。
  in.alert_active = (g_alert_last == 0xFFu) ? 0u : g_alert_last;

  return in;
}

// 串口离线回放 VAN 帧:一行 "VAN 824 18F82710000000" 喂一帧(见 van_replay.h),
// 实车接收发器前先用抓到的帧联调,不用先焊板。
//
// ★ 两个口**都收**:S3 上插原生 USB 口是 Serial(USB-CDC),插板载 CH340 那个
//   UART 口是 Serial0(UART0)—— 手头只有一根线,插哪个口都得能贴帧
//   (2026-09-18:就是把线插去了 UART 口,才有了这条)。
//   一行只允许来自一个口,所以两个口各自维护自己的行缓冲(共用一个 len 会串行)。
//   ★ 2026-09-23 补一句边界:链路构建(dash_log.h 的 `DASH_LOG_UART0=0`)里
//     **日志已不写 UART0**,但下面这段**仍然从 Serial0 读**(回放输入)——
//     读与链路的"收"共用同一个 RX FIFO,谁先取走谁处理:两边都是非阻塞的、都不改写
//     对方的配置,所以没有新的冲突;真要拿 43/44 跑链路时,链路那一路优先。
//     (链路自己 `begin()` 时会把 43/44 按 §0 §1.1 重新配一遍,那时 Serial0 的读
//      拿到的是链路字节 —— 那是"贴帧口被链路占着"这个既定事实,不是新问题。)
static void van_replay_feed(const char c, char* line, uint8_t& len, uint32_t now) {
  if (c == '\r' || c == '\n') {
    if (len) {
      line[len] = '\0';
      VanPacket p{};
      if (parseVanReplayLine(line, &p, now)) {
        g_data.onVanPacket(p);
      } else {
        dash_logf("VAN? %s\n", line);   // 解析失败回显,方便排错
      }
      len = 0;
    }
  } else if (len < 79) {
    line[len++] = c;
  }
}

static void van_replay_poll(uint32_t now) {
  static char line_usb[80];
  static uint8_t len_usb = 0;
  while (Serial.available()) {
    van_replay_feed((char)Serial.read(), line_usb, len_usb, now);
  }
#if defined(ARDUINO) && defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  // 只有 CDC_ON_BOOT 时 Serial0 才是"另一个口";经典 ESP32 上两者是同一个 UART0
  static char line_uart[80];
  static uint8_t len_uart = 0;
  while (Serial0.available()) {
    van_replay_feed((char)Serial0.read(), line_uart, len_uart, now);
  }
#endif
}

// ---- 上电示位标(beacon):主循环卡死也照打 ----
//
// 为什么非要有这么个东西:USB-CDC 在**没有主机**时没有缓冲,setup() 里那几行
// 开机日志一旦错过就永远看不到了。于是下面两种情况在串口上**长得一模一样**
// (都是空白),完全无法区分:
//     (a) 程序卡在某个初始化里(比如 LVGL 或某个驱动等硬件);
//     (b) 芯片根本没跑我们的固件(还在 ROM 下载模式)。
// 2026-09-18 就在这个岔路口上卡了很久。这个回调挂在 esp_timer 的
// **独立任务**上,主循环哪怕死在某一行,它也照打;而且把"停在第几步"一起报出来,
// 一次刷机就能定位。
//
// 只在前 40 秒打(靠计数器闭嘴,不去 stop 自己 —— 免得在回调里操作自身)。
// 车上是没有 USB 主机的,过了这段就安静,不白占带宽。
#if defined(DASH_DEVICE_SELFTEST)
static volatile uint8_t g_boot_stage = 0;
#define BOOT_STAGE(n) do { g_boot_stage = (uint8_t)(n); } while (0)
static const char* const kStageNames[] = {
    "还没进 setup",       // 0
    "Serial 就绪",        // 1
    "数据服务就绪",        // 2
    "VAN 物理层就绪",      // 3
    "主题+图片加载完",     // 4
    "UI 初始化完",        // 5
    "loop: 刚开始一轮",    // 6
    "loop: 数据已更新",    // 7
    "loop: UI 已 tick",   // 8
    "loop: 已渲染一帧",    // 9
};
static const uint8_t kStageMax =
    (uint8_t)(sizeof(kStageNames) / sizeof(kStageNames[0]) - 1u);

static void beacon_cb(void*) {
  static uint32_t n = 0;
  if (++n > 40) return;
  const uint8_t s = g_boot_stage;
  // 自检里那几个关键数字(PSRAM / flash)也塞进这一行:监视器晚接上时,
  // 开机那次完整自检已经错过了,而示位标还在打 → 一眼就能核对硬件。
  dash_logf("BEACON %2u  step=%u(%s)  uptime=%us heap=%uKB psram=%uKB flash=%uMB\n",
                (unsigned)n, (unsigned)s,
                (s <= kStageMax ? kStageNames[s] : "?"),
                (unsigned)(esp_timer_get_time() / 1000000),
                (unsigned)(ESP.getFreeHeap() / 1024u),
                (unsigned)(ESP.getPsramSize() / 1024u),
                (unsigned)(ESP.getFlashChipSize() / (1024u * 1024u)));
}
#else
#define BOOT_STAGE(n) do { } while (0)
#endif

// ---- 板子自检(换板/换芯片后第一眼要看的就是这几行)----
// 为什么值得占几行:手里有经典 ESP32 和 S3 N16R8 两块板,
// 而"刷进去了但跑的是另一块板的固件"、"买到的是 N8R2 而不是 N16R8"
// 这类问题在串口上**一眼就能看出来**,不写就得靠猜。
//   · PSRAM 那一行是"双 480×480 屏能不能做"的判据:报 0 就是没起来,
//     要么板子不是 R8,要么 memory_type 配错了(见 platformio.ini 的 [env:esp32s3])。
//   · Flash 大小决定分区表能不能用:16MB 的表刷到 4MB 板子上会直接起不来。
// ★ 整段是设备专属的(ESP.* / esp_partition 宿主机没有),见文件头的
//   DASH_DEVICE_SELFTEST 判定 —— pcpreview 编这一步会直接编不过。
#if defined(DASH_DEVICE_SELFTEST)
static void print_selftest(const char* tag) {
  dash_logf("--- 自检(%s)---\n", tag);
  dash_logf("chip  : %s rev%d, %d 核 @ %u MHz\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores(),
                (unsigned)getCpuFrequencyMhz());
  dash_logf("flash : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024u * 1024u)));
  // PSRAM 这一行是"双 480×480 屏能不能做"的判据。报 0 的**最常见**原因不是板子,
  // 而是漏了 -DBOARD_HAS_PSRAM(核心会主动把 CONFIG_SPIRAM #undef 掉,
  // 见 platformio.ini 的 [env:esp32s3])。所以这里连"编译期有没有开"一起报。
  dash_logf("psram : %u KB 可用 / %u KB 总%s\n",
                (unsigned)(ESP.getFreePsram() / 1024u), (unsigned)(ESP.getPsramSize() / 1024u),
#if defined(BOARD_HAS_PSRAM)
                ""
#else
                "  ← 编译期没开!检查 -DBOARD_HAS_PSRAM"
#endif
  );
  dash_logf("heap  : %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024u));
  // 分区表里的图片分区大小 —— 与编译期的口径对不上就说明烧错了分区表
  const esp_partition_t* ip = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "image");
  if (ip) {
    dash_logf("image : 分区 %u KB @ 0x%06X(编译期口径 %u KB)%s\n",
                  (unsigned)(ip->size / 1024u), (unsigned)ip->address,
                  (unsigned)(IMAGE_PARTITION_BYTES / 1024u),
                  (ip->size == IMAGE_PARTITION_BYTES) ? "" : "  ← 不一致,检查分区表!");
  } else {
    dash_logf("image : 分区不存在(检查分区表)\n");
  }
}
#endif  // DASH_DEVICE_SELFTEST

// ============================================================================
//  链路（**两个角色共用**的一步）：收帧 → 路由
// ============================================================================
// ★ 为什么共用：主板收 B 的 STATUS/EVENT（只进日志，§3 的单一日志出口），从板收
//   A 的 TICK/DATA（进数据层，§3 + §4）—— 解帧、重同步、角色自检、时基是**同一套**。
// ★ §3 已自记的缺口就落在这里：`FieldSource` 原来只有 None/Sim/Obd/Van，从板
//   "没有本地源"这件事表达不出来 ⇒ 本轮给 data_service 加了第五档 Link
//   （见 lib/dashcore/data_service.h 与 applyLinkData() 的实现口径）。
//
// 返回 true = 这一轮里有 DATA 被喂进了数据层。
// ★ 调用时机是硬的：必须在 `g_data.update(now)` **之前** —— 这样"这一圈收到的
//   数据"当圈就能进快照与上屏（`applyLinkData` 只是存下来，合并发生在 update 里）。
// ★ 本函数是**从板侧**的收帧形状（顺带喂时基、喂数据层）。主板侧另有
//   link_poll_inbound()：它要按 §3 的"单一日志出口"把 B 的状态转成自己的一行日志，
//   两边的差别就在"非 TICK/DATA 的帧怎么处理"这一处，各写一遍更清楚。
#if LINK_ROLE != 1
static bool link_poll_frames_slave(uint32_t now) {
  bool got_data = false;
  dashlink::Frame f;
  // ★ 注意 `LinkData` 是**全局作用域**的（它在 lib/dashcore/data_service.h 里，
  //   与 dashlink 命名空间无关）—— 写成 dashlink::LinkData 会编不过。
  ::LinkData ld;
  while (g_link_rx.poll(g_link_phy_slave, &f)) {
    if (dashlink::handleInbound(f, &g_link_time, now, &ld)) {
      // TICK / DATA：handleInbound 已经把 TICK 喂了时基、把 DATA 解成了 LinkData。
      if (f.type == (uint8_t)dashlink::MsgType::Data) {
        g_data.applyLinkData(ld);   // §3：从板把收到的 DATA 喂进自己的数据层
        got_data = true;
      }
      continue;
    }
    // HELLO / STATUS / EVENT：v1 实际只用 B→A（§3），A→B 收到只说明"对端在说话"，
    // 不参与数据面 —— 从板这一侧本轮不打日志（它自己的 USB-C 上要看的是数据层那几行）。
  }
  g_link_time.update(now);   // §4：推进年龄与三级超时（100 ms/500 ms/3 s）
  return got_data;
}
#endif

#if LINK_ROLE == 1
// ============================================================================
//  链路（主板侧）：一行日志 + 主循环里的四个动作
// ============================================================================
// 这一段把 §1.2/§3/§4/§5 里"主板该做的"收在一处，loop() 里只留两个调用点。
// 收到的 STATUS/EVENT 按 §3「单一日志出口」转成**自己 USB-C 上的一行**。
static void link_log_peer_line(const dashlink::Frame& f) {
  switch (f.type) {
    case (uint8_t)dashlink::MsgType::Status: {
      dashlink::StatusMsg s;
      if (!dashlink::unpackStatus(f.payload, f.len, &s)) {
        dash_logf("link: B STATUS 载荷解不出(len=%u)\n", (unsigned)f.len);
        return;
      }
      dash_logf("link: B uptime=%lums rx_ok=%u dropped=%u crc=%u gap=%u face=%u flags=0x%02X\n",
                (unsigned long)s.uptime_ms, (unsigned)s.frames_ok,
                (unsigned)s.frames_dropped, (unsigned)s.crc_err,
                (unsigned)s.last_gap_ms, (unsigned)s.left_face, (unsigned)s.flags);
      break;
    }
    case (uint8_t)dashlink::MsgType::Event: {
      dashlink::EventMsg e;
      if (!dashlink::unpackEvent(f.payload, f.len, &e)) return;
      dash_logf("link: B EVENT id=0x%02X value=%u face=%u%s\n", (unsigned)e.evt_id,
                (unsigned)e.value, (unsigned)e.face,
                dashlink::evtIdKnown(e.evt_id) ? "" : "  <-- 本机不认识的 evt_id(次版本只加东西)");
      break;
    }
    case (uint8_t)dashlink::MsgType::Hello: {
      dashlink::HelloMsg h;
      if (!dashlink::unpackHello(f.payload, f.len, &h)) return;
      dash_logf("link: B HELLO fw=%u build=%u boot=%u%s\n", (unsigned)h.fw_ver,
                (unsigned)h.build_tag, (unsigned)h.boot_reason,
                (h.fw_ver == kLinkFwVer && h.build_tag == kLinkBuildTag)
                    ? ""
                    : "  <-- 与本机 fw/build 不同(§3：HELLO 就是用来对账这个的)");
      break;
    }
    default:
      break;
  }
}

// 从板侧要报的那些"结构化状态"在主板这一侧只进日志（§3 的单一日志出口）。
static void link_poll_inbound(uint32_t now) {
  static bool announced_peer = false;
  static bool announced_conflict = false;
  static bool announced_ver = false;

  dashlink::Frame f;
  while (g_link_rx.poll(g_link_phy, &f)) {
    g_link_peer_ms = now;   // 收到的任何一帧都算"从板还活着"（§8 L13 的 30 s 判据用它）
    if (!announced_peer) {
      announced_peer = true;
      dash_logf("link: 首次收到从板帧(%s, role=%u)\n", dashlink::msgTypeName(f.type),
                (unsigned)f.role);
    }
    if (f.ver_mismatch && !announced_ver) {
      announced_ver = true;
      dash_logf("link: ver 不匹配(本机 0x%02X, 对端 0x%02X) —— §2：不断链、不降级\n",
                (unsigned)dashlink::kVer, (unsigned)f.ver);
    }
    // 主板侧**不需要** LinkData（那是从板喂数据层用的）⇒ 传 nullptr。
    if (dashlink::handleInbound(f, nullptr, now, nullptr)) {
      // TICK/DATA 从从板过来 = 角色刷反（§5 ①）。LinkRx 那边**已经丢帧并计数**了，
      // 这里只补一行说明 —— 不升级为"只收不发"、更不改本机角色（定案 L12）。
      dash_logf("link: B 发来 %s(不该由从板发,检查 §5 的角色)\n",
                dashlink::msgTypeName(f.type));
      continue;
    }
    if (f.type == (uint8_t)dashlink::MsgType::Hello) g_link_hello_acked = true;
    link_log_peer_line(f);   // §3 的"单一日志出口"：B 的状态变成 A 的一行
  }
  if (g_link_rx.roleConflictSeen() && !announced_conflict) {
    announced_conflict = true;
    dash_logf("link: 角色冲突(对端 ROLE 与本机相同=%u) —— 已丢帧计数(§5 ①, 定案 L12)\n",
              (unsigned)dashlink::kLocalRole);
  }
}

// 主循环每圈的四步（顺序与理由见 loop() 里那段注释）。
// ★ 四个动作都只"入队"或"排水"，**没有任何一处写 UART** —— 写 UART 的只有最后
//   那两次 `pump()`。这是 §1.2 ①（不在 ISR/回调里发）在主板侧的可读形态。
// ★ `snapshot` 必须由调用方传**刚刚 g_data.update(now) 的返回值**进来 ——
//   这就是 §1.2 ③ 的"从快照发"。别在这个函数里自己再调一次 g_data.update()。
static void link_master_tick(uint32_t now, const VehicleState& snapshot) {
  // ① TICK：50 Hz（§3）。主循环被 LVGL 拖慢时**不补发突发**（见 TickGen::due）。
  dashlink::TickMsg tm;
  if (g_link_tick.due(now, &tm)) {
    uint8_t payload[dashlink::kTickLen];
    if (dashlink::packTick(tm, payload)) {
      // 空间不够时 enqueueFrame 会**整帧丢**并计数（§1.2 ②）—— 这里不重试、不等待。
      g_link_tx.enqueueFrame((uint8_t)dashlink::MsgType::Tick, payload, dashlink::kTickLen,
                             dashlink::kLocalRole);
    }
  }

  // ② HELLO：上电 1 次，之后每 5 s 重发，**直到收到对端 HELLO**（§3）。
  if (!g_link_hello_acked &&
      (g_link_hello_ms == 0u || (now - g_link_hello_ms) >= 5000u)) {
    g_link_hello_ms = now;
    dashlink::HelloMsg hm;
    hm.fw_ver = kLinkFwVer;
    hm.build_tag = kLinkBuildTag;
    hm.boot_reason = 0;   // 仓库里没有既有编码（§3 原话）⇒ 先恒 0
    uint8_t payload[dashlink::kHelloLen];
    if (dashlink::packHello(hm, payload)) {
      g_link_tx.enqueueFrame((uint8_t)dashlink::MsgType::Hello, payload, dashlink::kHelloLen,
                             dashlink::kLocalRole);
    }
  }

  // ③ DATA：跟随 VAN 0x824 到达（§3，≈79.7 Hz，**不另建定时器**）。
  //    snapshot_ms = 0x824 最后一次到达的时刻（0 = 到现在一帧都没收到过）。
  //    ★ 拿的是**上面 g_data.update(now) 之后**的快照与来源表（§1.2 ③）。
  dashlink::DataMsg dm;
  if (g_link_data.due(now, g_data.vanSource().lastUpdateMs(), snapshot, g_data.status(),
                      &dm)) {
    uint8_t payload[dashlink::kDataLen];
    if (dashlink::packData(dm, payload)) {
      g_link_tx.enqueueFrame((uint8_t)dashlink::MsgType::Data, payload, dashlink::kDataLen,
                             dashlink::kLocalRole);
    }
  }

  // ④ 排水：`LinkTx` 的两级环 → PHY 的环 → UART 的 FIFO。两步都**只走能走的那些字节**。
  g_link_tx.pump(g_link_phy);
  g_link_phy.pumpTx();
}
#endif  // LINK_ROLE == 1

void setup() {
  dash_log_begin(115200);
  delay(200);
  // 开机握手行:刷机后靠它确认固件真的跑起来了(见 ACCEPTANCE.md)。
  // 放在最前面 —— 即使后面的初始化有问题,至少能看到这一行。
  dash_logf("206 dash ok\n");

  // ★ 设备端的 USB-CDC 是**没有主机的缓冲**的:监视器如果没在开机前打开,
  //   这几行就永远看不到了(实测踩过:刷完立刻开监视器,一片空白)。
  //   所以开机打一次,loop() 里"主机第一次连上"时再补打一次 —— 见 loop()。
  //   这里只等 200ms(上面那句 delay),**不做**"等主机"的阻塞等待:
  //   车上是没有 USB 主机的,阻塞等待会让每次上电都白等几秒。
#if defined(DASH_DEVICE_SELFTEST)
  print_selftest("上电");
#endif

  // ★ 示位标定时器:立刻起,1 Hz(**在 setup 一开头就起**,因为它存在的意义
  //   就是"后面的初始化万一卡住,也要有人替我们说话")。
  BOOT_STAGE(1);
#if defined(DASH_DEVICE_SELFTEST)
  {
    esp_timer_create_args_t args = {};
    args.callback = &beacon_cb;
    args.name = "dash_beacon";
    esp_timer_handle_t t = nullptr;
    if (esp_timer_create(&args, &t) == ESP_OK) {
      esp_timer_start_periodic(t, 1000000);
    }
  }
#endif

  // ★ OBD 串口要在 g_data.begin() **之前**接上:ObdSource::begin() 会立刻
  //   发第一条 AT,那时 UART1 必须已经 begin 过(见文件头 OBD 那一节)。
  g_data = attachObdSerial();
#if OBD_SERIAL
  // 诊断页那一格"OBD 连接与 PID 状态"要先知道**这一份固件有没有启用 OBD** ——
  //   这是编译期事实（`-DOBD_SERIAL=0` 的构建里根本没有那一路），不是运行期探测。
  g_obd_enabled = true;
#endif
  // 静音开关的掉电保存（默认有声；静音是车主的选择，要跨上电记住）。
  mute_load();
  g_alerts.setMuted(g_beep_muted);
  g_data.begin();
  BOOT_STAGE(2);

  // 告警层 + 蜂鸣器：把实现**挂上**（挂哪个见上面那一段注释 —— L14 还没裁决）。
#if defined(DASH_DISPLAY_PREVIEW)
  g_buzzer = &g_buzzer_host;      // 预览：打印 BEEP 行（可选系统提示音）
#endif
  g_buzzer->begin();
  dash_logf("alerts: 已就绪(超速 %.0f / 红区 %.0f / 门 / 转向灯忘关 %us)\n",
            (double)g_alerts.config().overspeed_kmh,
            (double)g_alerts.config().redline_rpm,
            (unsigned)(g_alerts.config().turn_signal_on_ms / 1000u));
#if defined(DASH_DISPLAY_PREVIEW)
  // 预览的输入注入：键盘 + preview/inject.txt（用法见 src/preview_input.h）
  preview_input_begin("preview/inject.txt");
  g_preview_prev = g_preview;     // 免得第一帧就报"变了"
  // 把遮罩的初值推给显示侧（默认开；启动时**不打**那行 inject 回执，
  // 免得盖住 banner）。
  dash_display_preview_set_panel_mask(g_preview.panel_mask);
#endif
  // 物理层 → 打印层 → 数据源。打印层只旁观,不影响数据流
  // (抓帧时那行文本就是回放格式,见 VanLogSink 的说明)。
  g_van_log.setNext(&g_van_sink);
  g_van_phy.setSink(&g_van_log);
  g_van_phy.begin();
  BOOT_STAGE(3);
#if LINK_ROLE == 1
  // 链路物理层（主板侧）：§0 的 43 发 / 44 收、§1.1 的 115200 8N1。
  // ★ 顺序上的一个已知事实（不改行为，只记清楚）：`dash_log_begin()` 在**本构建**里
  //   只开 USB-CDC（`DASH_LOG_UART0=0`，见本文件上方那段），所以它**不碰** UART0；
  //   下面这一行才是 UART0/43/44 上唯一的占用者。
  //   ★ 而 VAN 采集那个构建（不带 `-DLINK_PHY_UART`）里没有这一段：那边
  //     `dash_log_begin()` 照旧开着 UART0 双通道日志 —— 行为一字未变。
  g_link_phy.begin(false);
  g_link_rx.setLocalRole(dashlink::kLocalRole);
  g_link_tick.reset(millis());   // §4：tick_ms 是主板**自己**的单调毫秒(从复位起算)
  dash_logf("link: 主板侧就绪 TX=GPIO%d RX=GPIO%d @%u 8N1(§0/§1.1)\n",
            (int)g_link_phy.txPin(), (int)g_link_phy.rxPin(), (unsigned)dashlink::kLinkBaud);
#else
  // 从板侧：只需要"我知道我是谁"（§5 的角色自检）+ 时基状态机的初值。
  // ★ 这里**不 begin 任何 UART**：本轮从板的 PHY 是 LinkPhyNull（见上面那段说明）。
  g_link_rx.setLocalRole(dashlink::kLocalRole);
  g_link_time.reset();
  dash_logf("link: 从板侧就绪(§5, LINK_ROLE=0) —— 收帧路径已就位,PHY 本轮是桩"
            "(真实 UART 等最终 2.8\" 板到手,见 main.cpp 里 LinkPhyNull 的说明)\n");
#endif
  // 主题:先默认值(由 dash_ui_init 兜底),再尝试用 flash 里的主题文件覆盖。
  // 加载失败不影响启动 —— 降级到默认主题继续跑。
  theme_load();
  // 图片资源(背景/表情)也从 flash 分区加载,同样是失败即降级。
  // 加载结果在下面的 dash_ui_init() 里用:有图就 lv_image 画出来
  // (背景在最底层、表情在最上层),没有就退回程序化表情 + 主题纯色背景。
  // 这一步必须在 dash_ui_init() 之前 —— 否则"刷了图片没反应"在设备上是
  // 完全静默的,无从判断是分区表、mmap 还是格式的问题。
  // 换图只刷 image 分区(0x254000),固件不用重编;镜像是启动时 mmap 的,
  // 所以刷完要重启。
  image_load();
  BOOT_STAGE(4);
  dash_ui_init();
  BOOT_STAGE(5);
  // 一行汇总:有没有图片资源一眼可见(没刷图片是正常情况,不是错误)。
  {
    const ImageBlobHeader* ih = image_blob_header();
    if (ih) {
      dash_logf("image ok: %u 张,数据 %u 字节,镜像 %u 字节\n",
                    (unsigned)ih->count, (unsigned)ih->data_bytes,
                    (unsigned)image_blob_len());
    } else {
      dash_logf("image none: 无图片资源,背景用主题纯色\n");
    }
  }
  // 物理层类型:抓帧时第一眼要确认的就是这一行 ——
  // 写着 stub 就说明这次编译**没有**启用 GPIO 接收(-DVAN_PHY_GPIO=1),
  // 那样即使收发器接好了也不会有任何帧进来。
#if defined(VAN_PHY_GPIO)
  // 具体引脚与"空闲关帧"的阈值由 VanPhyGpio::begin() 自己打印(见 van_phy_gpio.cpp)
#else
  dash_logf("van phy: stub(没启用 GPIO 接收;要抓帧请用 -DVAN_PHY_GPIO=1 编译)\n");
#endif
}

void loop() {
  const uint32_t now = millis();
  // 示位标报的"第几步"= 本轮**已经走到**的最后一步(不是累计值):
  // 所以卡在哪一步,串口上看到的就是哪一步。
  BOOT_STAGE(6);
  g_van_phy.tick(now);   // VAN 物理层解帧 → 喂给 data_service
                         // (桩 / GPIO 收帧两种实现共用这一个接口,见 van_phy.h:
                         //  加 -DVAN_PHY_GPIO=1 时这里就是真的 GPIO 收帧)
  van_replay_poll(now);  // 串口贴帧离线回放(和物理层等价,先到的先写)

#if LINK_ROLE != 1
  // ---- 链路（从板侧）：**先收后合并** ----
  // ★ 顺序是硬要求：收到的 DATA 必须先 `applyLinkData()`、再 `g_data.update(now)`，
  //   这样这一圈收到的值当圈就进快照与上屏（晚一圈也行，但没必要）。
  // ★ 本轮从板的 PHY 是 `LinkPhyNull`（见上面那段说明）：真实 UART 要等最终 2.8"
  //   板到手、且 §8 L1 的 ⓐⓑⓒ 三条实测做完才接 —— 这套"收 → 喂数据层"的逻辑
  //   已经由 native 用例整条跑通（test_link_app.cpp 的端到端那一条）。
  link_poll_frames_slave(now);
#endif

  // ★ 2026-09-24：由 `const VehicleState st` 改成**可写**的 `st_mut` ——
  //   pcpreview 的输入注入要在这份快照上覆写几个字段（见下面 preview_apply）。
  //   设备侧一个字都没变：`st_mut` 在那边从来不会被改（那一段在 #if 里）。
  //   名字刻意带 `_mut`：后面读代码的人一眼知道"这里可能被注入改过"。
  VehicleState st_mut = g_data.update(now);
  BOOT_STAGE(7);

#if LINK_ROLE == 1
  // ---- 链路（主板侧）：收 → 发 TICK / HELLO / DATA → 排水 ----
  // ★ 三个顺序上的口径：
  //   ① 收在 `g_data.update()` **之后**：收到的 STATUS/EVENT 只进日志，不参与本机的
  //      数据合并（本机的真值来自它自己的 VAN/OBD，§1.2 ③ 也不许它回头影响快照）。
  //   ② TICK/HELLO/DATA 用的都是**刚才那一份快照**（`st_mut` 与 `g_data.status()`）——
  //      这就是 §1.2 ③ 的"从快照发、不从回调发"。
  //   ③ 排水放在**同一轮的最末尾**：先把该排的都排完，下一圈再进新的。两步都有上界、
  //      都不忙等（见 link_tx.h / link_phy_uart.h）。
  link_poll_inbound(now);
  link_master_tick(now, st_mut);
#endif
#if defined(DASH_DISPLAY_PREVIEW)
  // ---- pcpreview 的输入注入：在数据合并**之后**、算视图**之前** ----
  // 顺序是硬的：注入是"人替传感器说话"，所以要压过 sim/VAN/OBD 的合并结果；
  // 但它**不回流**进 data_service（不碰优先级、不产生协议行为）。
  if (preview_input_poll(g_preview)) {
    preview_apply(g_preview, st_mut);
  }
  // ★ 诊断页那一位（K）**不是注入**（见 preview_input.h 的说明）：它是一个
  //   **事件**（"按了一下"），所以这里按**边沿**处理 —— 一直按着不放 / 控制文件
  //   里一直写着 `diag=1`，都只翻一页，不会每帧翻一页。
  //   一个键干三件事：关着 ⇒ 打开第 1 页；开着且还有下一页 ⇒ 翻页；
  //   开着且在最后一页 ⇒ 关闭（回到表盘）。
  {
    const bool req = g_preview.diag_toggle_req;
    if (req && !g_diag_req_last) {
      if (!dash_ui_diag_open()) {
        dash_ui_diag_toggle();
      } else if ((uint8_t)(dash_ui_diag_page() + 1u) >= kDiagPageCount) {
        dash_ui_diag_toggle();
      } else {
        dash_ui_diag_next();
      }
      dash_logf("diag: %s page=%u/%u\n", dash_ui_diag_open() ? "open" : "closed",
                (unsigned)(dash_ui_diag_page() + 1u), (unsigned)kDiagPageCount);
    }
    g_diag_req_last = req;
  }
  // 「数据层长什么样」那一位（T / 控制文件的 `sim=`）：**只在预览里**取出来，
  // 由下面 sys_inputs_build 的调用点施加到输入上（不进 VehicleState，见说明）。
  g_sim_ok = g_preview.sim_ok;
#endif

  // ---- 告警层（2026-09-24）----
  // ★ 喂的是**这一轮最终的快照**（注入之后），所以 pcpreview 上按 O/R
  //   能立刻看到告警与蜂鸣器的反应 —— 而判据本身与真车跑的是同一份代码。
  const AlertKind alert = g_alerts.update(st_mut, now);
  if (g_alerts.beeping()) {
    g_buzzer->beep(g_alerts.pattern(), g_alerts.config().beep_ms);
  } else {
    g_buzzer->off();
  }
  // 状态变化时打一行（不是每轮都打：红区一直守着会变成每秒一行噪音）。
  const uint8_t alert_id = (uint8_t)alert;
  if (alert_id != g_alert_last) {
    g_alert_last = alert_id;
    dash_logf("alert: %s%s\n", alertName(alert), g_alerts.muted() ? " (muted)" : "");
  }
#if defined(DASH_DISPLAY_PREVIEW)
  // 屏上告警闪与蜂鸣器**同一拍**：这里用"刚响过的那一拍"当相位源
  // （听起来在叫、看起来在闪 = 一条信息；两者不同步会很怪）。
  if (g_alerts.beeping()) {
    if (!g_lamp_pulse) {
      // 新的一拍：记下**当前已经落了几帧**，等它落出下一帧再放开。
      g_lamp_pulse = true;
      g_lamp_pulse_armed = true;
      g_lamp_pulse_frame = dash_display_preview_frames();
      g_lamp_pulse_ms = now;
    }
  } else if (g_lamp_pulse) {
    // 蜂鸣器已经不响了（`beep_ms` = 120ms 的窗口过去了），但**先别急着熄**：
    // 预览每 200ms 才落一帧，这一拍必须活到被拍下来为止（见上面的说明）。
    const bool captured = g_lamp_pulse_armed &&
                          (dash_display_preview_frames() != g_lamp_pulse_frame);
    if (captured || (uint32_t)(now - g_lamp_pulse_ms) > 1500u) {
      g_lamp_pulse = false;
      g_lamp_pulse_armed = false;
    }
  }
  const LampView lamps = make_lamps(st_mut, alert_id, now, g_lamp_pulse);
#else
  // 设备侧：告警闪的相位由 beep 的节奏给（真机上蜂鸣器一响，屏上就闪那一拍）。
  const LampView lamps = make_lamps(st_mut, alert_id, now, g_alerts.beeping());
#endif

  dash_ui_tick(now);   // LVGL 心跳,每个循环都跑
  dash_display_poll(); // 设备上为空;pcpreview 落 BMP 帧
  BOOT_STAGE(8);

  if (now - last_ui_ms >= 200) {
    last_ui_ms = now;
    // ---- 系统状态层（2026-09-24）：先备好这一拍的输入，再做两件事 ----
    // ① 把这一拍的车状态存进 `st_state_cache`（`sys_inputs_build()` 读它取车速/转速）；
    // ② 自己推一次 `g_sys`（**每拍只推一次**，判据住在 lib/dashcore/），
    //    然后按 `beepDue()` 决定要不要响那一声轻提示。
    // ★ 为什么在这里推而不是在 dash_ui 里推：那一声"轻提示"归主循环管
    //   （它拿着 Buzzer 与静音开关），而"状态机推了几次"必须是**唯一**的一处 ——
    //   两处各推一次会让去抖窗口按两倍速走（屏上提示早出来半秒，且很难查）。
    // ★ `diag_in` 在 if 里初始化：它只在"真的按过键"时才需要，而声明放在这里
    //   是因为下面 `dash_ui_render` 要用它（诊断页关着时 dash_ui 一个字节都不读）。
    st_state_cache = st_mut;
    SysStatusInputs diag_in = sys_inputs_build(now);
#if defined(DASH_DISPLAY_PREVIEW)
    // ★ 演示注入（**只在预览里**）：把这一拍的**数据层口径**换成"实测"。
    //   为什么默认是"坏"、注入才是"好"：pcpreview **没有 VAN 硬件** —— 弧与
    //   数字全部由 `sim_source` 的假数据驱动、四个字段的来源恒为 `Sim`
    //   ⇒ 判据（正确地）认为"数据不可信"一直成立，角标一上电就挂着。
    //   于是"数据恢复 ⇒ 提示自动消失"这半条只能反过来演：注入"来源 Van +
    //   VAN 帧新鲜"，判据自己就会在 800 ms 的去抖窗口之后把角标撤掉。
    //   注入只改这份局部输入（碰不到 data_service、碰不到协议），
    //   而 `g_sys` 的去抖/限速/恢复判据**全是真的**。
    if (g_sim_ok) {
      diag_in.speed_src   = 3;          // FieldSource::Van
      diag_in.rpm_src     = 3;
      diag_in.coolant_src = 2;          // Obd（真板上水温/进气只有 OBD 这一个真源）
      diag_in.intake_src  = 2;
      diag_in.van_ever_framed = true;
      diag_in.van_age_ms = 20u;         // 帧很新鲜（<< kTrustVanStaleMs）
      // ★ 链路那一档也要一起切：预览的角色是**从板**（`LINK_ROLE` 默认 0），
      //   而从板开机以来一帧 TICK 都没见过 ⇒ `LinkTime` 的结论是 **SimFallback**，
      //   于是即使来源标成 Van，"链路回退 Sim"这一条仍然成立、角标照样挂着
      //   （实测就是这样：只切来源，角标不消失 ✗）。
      //   所以"实测"这一档要连链路一起演：把状态摆成 **Locked**、age 很小 ——
      //   那正是"主板的 TICK 正常到达"时 `LinkTime` 的样子。
      diag_in.link_state = 0;           // Locked
      diag_in.link_tick_age_ms = 20u;
      // ★ **不编造** ticks/seq_gap 那几个计数：它们是真实的累计量（预览里本来就是 0，
      //   因为一帧 TICK 都没收到）。状态与 age 是"这一拍长什么样"，可以演；
      //   计数是"发生过什么"，演它就成了假证据。
    }
#endif
    const DataTrustReason trust = g_sys.update(diag_in, now);
    // 一声轻提示（**走既有 Buzzer 抽象**）：只在"变坏"那一跳响一次、受 5 秒限速，
    // 而且**静音开关优先**（车主的选择 > 这一声提示）。
    // ★ 时长用 kTrustBeepMs（120 ms）：有源蜂鸣器只能开/关 ⇒ 就是"开这么久然后关"；
    //   它必须 <= 300 ms（不许长时间连续高电平，理由见 ARCHITECTURE 的提示音说明）。
    bool trust_beeped = false;
    if (g_sys.beepDue()) {
      if (!g_beep_muted && !g_alerts.muted()) {
        g_buzzer->beep(BeepPattern::Short, kTrustBeepMs);
        trust_beeped = true;
      }
    }
    // 状态变化的一行回执（不是每拍都打：数据在阈值上下抖会变成刷屏）。
    // ★ 把"有没有响那一声"并进这一行 —— 分成两行会让"该响没响"与"状态没变"
    //   看起来一模一样（而静音与否正是要能一眼看出来的东西）。
    static uint8_t trust_last = 0xFF;
    if ((uint8_t)trust != trust_last) {
      trust_last = (uint8_t)trust;
      dash_logf("trust: %s%s%s (episodes=%lu)\n", dataTrustReasonName(trust),
                trust == DataTrustReason::kNone ? "" : "  <-- 屏上出现数据不可信提示",
                trust_beeped ? " beep" : (g_sys.untrusted() ? " muted" : ""),
                (unsigned long)g_sys.episodes());
    }
    dash_ui_render(make_view(st_mut, now), lamps, g_sys, diag_in, now);
    // 渲染帧率（EMA，alpha = 1/8，×10 定点）：它回答"这条 200 ms 节流有没有被卡住"
    // （RGB 那条路上第一次整屏刷新要 ≈1 秒 ⇒ 那一秒 fps 会掉下来）。
    // ★ 用 `dt` 的**实际值**（夹在 1..1000 ms）：主循环被长 flush 挡住时
    //   dt 会变很大，不夹的话 10000/dt 会算出 0 或者被 32 位除法截没。
    {
      uint32_t dt = now - last_render_tick_ms;
      if (dt < 1u) dt = 1u;
      if (dt > 1000u) dt = 1000u;
      const uint32_t inst10 = 10000u / dt;
      g_ui_fps10 = (g_ui_fps10 == 0u) ? inst10
                                      : (g_ui_fps10 - g_ui_fps10 / 8u + inst10 / 8u);
    }
    last_render_tick_ms = now;
    BOOT_STAGE(9);
  }

  // ★ 上电后**不再重复打完整自检** —— 那是 2026-09-18 排查"串口一片空白"时
  //   加的应急手段(当时既不知道 USB-CDC 没缓冲,也不知道监视器会把芯片按进
  //   下载模式)。现在有两条可靠的日志通道(UART 口 + USB 口),而且示位标
  //   每秒已经把那几个关键数字(heap/psram/flash)带出来了,再刷屏只是噪音:
  //   实测整个自检每秒 5 行 × 20 秒,真正的 VAN 帧会被淹掉。
  //   开机打一次(上面 setup 里)+ 40 秒示位标就够了。

  // 每 5 秒打印各字段当前由哪个源供给 + 当前数值(调试用)。
  // 数值是必须的:桩驱动丢弃画面,开机动画/换屏之前只能靠串口确认
  // 假数据弧确实在扫量程(见 ACCEPTANCE.md 的"假数据扫表"一条)。
  // ★ 无条件打印(别加"主机连上才打"的判断 —— 那会把 connected 卡死,见上)。
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    const DataSourceStatus& s = g_data.status();

    // OBD 的 0100 位图结论**只报一次** —— 到车上第一眼要看的就这一行:
    // ECU 到底认不认 010D(车速),以及车速那一路开没开。
    // 为什么值得单独一行:它决定了"要不要花时间做 VAN 抓帧"。
    static bool support_announced = false;
    if (!support_announced && s.obd_support_known) {
      support_announced = true;
      dash_logf("obd: ECU 位图 0x%08lX -> 车速(010D)%s,车速轮询%s\n",
                    (unsigned long)s.obd_support_mask,
                    s.obd_speed_supported ? "支持" : "不支持",
                    s.obd_speed_polled ? "已开" : "关闭");
    }

    dash_logf("SRC speed=%s rpm=%s coolant=%s intake=%s | v=%.1fkm/h %.0frpm %.1fC %.1fC\n",
                  fieldSourceName(s.speed),
                  fieldSourceName(s.rpm),
                  fieldSourceName(s.coolant),
                  fieldSourceName(s.intake),
                  st_mut.speed_kmh, st_mut.rpm, st_mut.coolant_c, st_mut.intake_c);
    // 实测刷新率:判断"K 线够不够用"的唯一依据(见 obd_source.h 的时隙账)
    dash_logf("SRC-Hz rpm=%.1f cool=%.1f intake=%.1f speed=%.1f\n",
                  s.obd_rpm_hz, s.obd_coolant_hz, s.obd_intake_hz, s.obd_speed_hz);
    // ★ 2026-09-24 新增的一行:**VAN 上已解出、这一轮才接进数据层的四类字段**。
    //   为什么要单独一行（而不是塞进上面 SRC 那行）：上面那行是"六个标量字段
    //   各自的源"，这四类的来源**恒为 VAN 或 None**（唯一来源，见 data_service.h），
    //   合并到一行里会让"为什么 speed 那格能是 obd、灯那格永远不是"看着像 bug。
    //   age 也一起报：灯位那一格的 age 是"最近一帧 0x4FC 的年龄"，
    //   而灯**亮不亮**另由 600 ms 的保持窗口决定（见 van_source.h）。
    //   ★ 拿不到的值一律打 `-`：UINT32_MAX 这种哨兵直接打出来是 4294967295，
    //     到车上会被误读成"这个数有意义"（踩过同类坑：age 字段写成 now-0）。
    {
      const VehicleState& cur = st_mut;
      char age_lights[16], age_door[16], age_vin[16];
      snprintf(age_lights, sizeof(age_lights), "%lu",
               (unsigned long)((s.lights_age_ms == UINT32_MAX) ? 0u : s.lights_age_ms));
      snprintf(age_door, sizeof(age_door), "%lu",
               (unsigned long)((s.door_age_ms == UINT32_MAX) ? 0u : s.door_age_ms));
      snprintf(age_vin, sizeof(age_vin), "%lu",
               (unsigned long)((s.vin_age_ms == UINT32_MAX) ? 0u : s.vin_age_ms));
      dash_logf("SRC-VAN turn=%s%s%s low=%s pos=%s door=%s vin=%s\n",
                    cur.indicator_left ? "L" : "-",
                    cur.indicator_right ? "R" : "-",
                    cur.hazard ? "(haz)" : "",
                    fieldSourceName(s.low_beam),
                    fieldSourceName(s.position_lamp),
                    cur.door_activity ? "active" : "idle",
                    (cur.vin[0] != '\0') ? cur.vin : "-");
      dash_logf("SRC-VAN age lights=%sms door=%sms vin=%sms | src turn=%s door=%s vin=%s"
                " | alert=%s beeps=%lu%s\n",
                    (s.lights_age_ms == UINT32_MAX) ? "-" : age_lights,
                    (s.door_age_ms == UINT32_MAX) ? "-" : age_door,
                    (s.vin_age_ms == UINT32_MAX) ? "-" : age_vin,
                    fieldSourceName(s.indicator_left),
                    fieldSourceName(s.door),
                    fieldSourceName(s.vin),
                    alertName(g_alerts.active()),
                    (unsigned long)g_alerts.beepCount(),
                    g_alerts.muted() ? " muted" : "");
    }
#if LINK_ROLE == 1
    // 链路质量（§7 的失败模式表：链路断/从板无响应那一行就看这里）。
    // ★ §8 L13：STATUS 超时门限 **30 s**（不是 5 s），而且**只用于日志、不上屏**
    //   （owner 裁决 2026-09-22）。从板可能只是刚上电/重启/没接线 ⇒ 主板**照常发数据**。
    {
      const dashlink::LinkRxStats& rs = g_link_rx.stats();
      const bool alive = (g_link_peer_ms != 0u) && ((now - g_link_peer_ms) < 30000u);
      dash_logf("link: tx=%luB rx_ok=%lu crc=%lu bad_len=%lu unk=%lu role=%lu   %s(对端已 %lums 没动静)\n",
                (unsigned long)g_link_tx.sentBytes(), (unsigned long)rs.frames_ok,
                (unsigned long)rs.crc_err, (unsigned long)rs.bad_len,
                (unsigned long)rs.unknown_type, (unsigned long)rs.role_conflict,
                alive ? "B 在线" : "从板无响应",
                (unsigned long)(g_link_peer_ms == 0u ? now : (now - g_link_peer_ms)));
    }
#else
    // 从板侧：§4 的三级超时状态就是它唯一要看的链路指标。
    // ★ §8 L10（owner 裁决）：>500 ms 降级 = **冻结最后值 + 表情退常态**；
    //   >3 s 仍没恢复 ⇒ 回退 Sim（§3 的 DATA 行）。回退 Sim 这一档已经由
    //   data_service 的 3 秒规则自然实现了（LinkData 也吃同一套 fresh() 判据）。
    //   ★ L10 的"冻结最后值"要冻的是**UI 上的弧**，那是 dash_ui 侧的事，
    //   本轮没做（见回报里的"下一轮还差什么"）。
    dash_logf("link: %s tick_age=%ums seen=%u seq_gap=%u miss=%u offset=%ldms\n",
              dashlink::linkTimeStateName(g_link_time.state()),
              (unsigned)g_link_time.tickAgeMs(), (unsigned)g_link_time.ticksSeen(),
              (unsigned)g_link_time.seqGaps(), (unsigned)g_link_time.seqMissing(),
              (long)g_link_time.offsetMs());
#endif
#if VAN_SNIFF
    van_sniff_report(now);
#endif
  }
}

#endif  // !defined(LINK_LOOPBACK_FIRMWARE)
