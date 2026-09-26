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
#include <esp_system.h>  // esp_reset_reason()/ESP_RST_* —— 开机那行 `boot: reason=…`
#include <esp_timer.h>   // 上电示位标(见 g_boot_stage / beacon_cb)
#define DASH_DEVICE_SELFTEST 1
#endif
#include "data_service.h"
#include "obd_transport_serial.h"   // ObdTransportSerial：把 Serial1 包成 ObdTransport
#if defined(OBD_BLE)
#include "obd_transport_ble.h"      // ObdTransportBle：走 BLE 诊断头（见 docs/BLE-OBD.md）
#endif
#include "dash_display.h"
#include "dash_ui.h"
#include "alerts.h"       // 告警层:只用已解字段(超速/红区/门/转向灯忘关)
#include "buzzer.h"       // 蜂鸣器抽象（"什么时候该响"与"谁发声"分开）
// ★ 2026-09-24:真机那一档的实现（2.8C 板载**有源**蜂鸣器，走 TCA9554 的 EXIO8）。
//   只在 `DASH_DISPLAY_RGB`（= `[env:esp32s3-rgb]`）里编进固件 —— 抓帧盒那三个 env
//   的编译单元里**一行都不存在**（门就是那个既有宏，没新增任何 -D）✓
#include "buzzer_exio.h"  // L14 的落地：发声的是**这块 2.8C（右/主机板）**
// ★ 2026-09-25:**跨重启留档**（上一次的复位原因 / 上一次运行了多久 / 守护累计计数）。
//   判据层与 NVS 读写回调的分工见 `lib/dashcore/boot_persist.h` 的文件头；
//   起因是本单那个悬案：累计启动次数 31→32，而上一次的 reason 没有留档 ⇒ 判不出死因。
#include "boot_persist.h"
#include "expression.h"   // Face（STATUS.left_face 要报"左屏当前档位"，§3）
#include "image_load.h"
#include "link_app.h"     // 双板链路 v1 的应用层接线（§1.2 ③ / §3 / §4 / §5）
#include "link_phy_pins.h"
#include "link_role.h"    // LINK_ROLE / kLocalRole（编译期是唯一权威，§5）
#include "link_time.h"
#include "link_tx.h"
#include "theme_store.h"
#include "system_status.h"   // 数据不可信提示 + 诊断页（判据/映射都在 lib/dashcore/）
#include "serial_cmd.h"      // 串口单字符命令的**判据层**（d/m/b/r；宿主机可测）
#include "ui_model.h"
#include "van_phy.h"
#include "van_replay.h"

// pcpreview 的输入注入（键盘 + preview/inject.txt）—— 只在这个 env 里编
// （实现整个在 src/preview_input.cpp 的 `#if defined(DASH_DISPLAY_PREVIEW)` 里）。
#if defined(DASH_DISPLAY_PREVIEW)
#include "preview_input.h"
#endif

// ★★ 链路物理层（真实 UART）**两个角色都编**（2026-09-25 起）——
//   v1 的契约要的是**双向**：A→B 是车数据（TICK/DATA），B→A 是状态上报
//   （STATUS/EVENT，§0 那条"本节对「接线定案」的一处新增"）。所以"只有主板有 PHY"
//   那种做法只能验下行，从板的上行一行都跑不到。
//   契约 §0 的引脚口径本来就是**每块板都是"43 发、44 收"**（`link_phy_pins.h`），
//   所以两侧用的是**同一个类、同一组脚、同一个 115200 8N1**，编译期不分叉。
//
//   门是**既有的** `LINK_PHY_UART`（platformio.ini 里链路固件才加的那个宏），
//   不是 `LINK_ROLE`：
//     · 有这个宏 ⇒ 真 `LinkPhyUart`（UART0 + 43/44）；`dash_log.h` 见到它就把
//       `DASH_LOG_UART0` 置 0 ⇒ 日志只走原生 USB-CDC，43/44 上只有链路一个占用者
//       —— 而且 `link_phy_uart.cpp` 里那道闸门会拦住任何"把日志又放回 UART0"的构建。
//     · 没有这个宏 ⇒ `LinkPhyNull`（空壳）。今天落到这一档的是：pcpreview
//       （宿主机预览，LINK_ROLE 默认 0）与 `[env:esp32dev]`（经典 ESP32，
//       那边连 `Serial0` 都不存在）。空壳的行为 = 链路静默，不卡主循环（§1.2 ②）。
//   ★ 角色仍旧是**编译期唯一权威**（`link_role.h` 的 `LINK_ROLE`）：它决定
//     "发什么、收什么怎么消费"，**不**决定"有没有 PHY"。§5 的 ①②③ 三条判据一行没动。
#include "link_phy_null.h"
#if LINK_PHY_ESP_NOW
// ★★ 2026-09-27（另一单）：**无线那一档**（ESP-NOW）。
//   理由一句话：有线（UART0 的 43/44）在台面上卡住了（信号到得了副板排针、但副板
//   `link rx bytes=0`，主假设是那颗 FSUSB42UMX 把 4Pin UART 从 ESP32 上摘走了，
//   见 ARCHITECTURE §8.2 / docs/LINK-TWO-BOARD.md 的「前置条件」一节）⇒ 车主拍板
//   **再加一条无线链路**。跑在 WiFi 那半边射频上（S3 没有经典蓝牙）。
//   ★ 它与 UART 那一档**抢同一份资源**（一个占 UART0、一个占射频与外设矩阵），
//     所以不许同时进固件 —— 下面紧跟着那条 static_assert 就是这道闸门。
#include "link_phy_espnow.h"
#include "link_meas.h"      // 测速/测丢包（信封 + 判据；协议一个字没动）
#endif
#if LINK_PHY_UART
#include "link_phy_uart.h"
#endif

// ★★ 板上 USB/串口"人格"（`FSUSB42UMX` 的 `SEL` = GPIO0）—— 2026-09-26。
//   ★ **默认不驱动**（`USB_PERSONALITY_AUTO` 默认 0）：方案/风险/退回路径全写在
//     那个头文件里，一句话是"GPIO0 同时是 BOOT strapping 脚，复位期间保持低
//     有可能把芯片带进下载模式"。要不要开由 owner 拍板（纪律照 §7.5.7）。
//   ★ 它的**唯一**调用点在 `setup()` 里 `boot_note()` 之后（那里有注释说明顺序）。
#include "usb_personality.h"

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
// ★ 蜂鸣器挂哪个实现（**这是 §8 L14 的落点，2026-09-24 已落地**）：
//   · **真机（`[env:esp32s3-rgb]`，= 车主手上这块 2.8C）= `BuzzerExio`** ——
//     经 `dash_buzzer_set()` 写 TCA9554 的 **EXIO8**（那颗芯片本来就在驱动这块屏，
//     所以**零额外引脚**）。硬件结论（**有源**：只会响/不响，没有音调、音量不可调）
//     的实测在 `docs/RGB-PANEL-2.8C.md` §13.6，落地形态见 §13.7。
//     ★★ **L14 的答复：发声的是 2.8C 这一块（右板 = 主机板）** —— 当下手上只有
//        这一块 2.8C（第二块还没到货），"另一块板上有没有蜂鸣器"**未实测**，
//        所以按"这一块能响、就用这一块"接线，不假设另一块也有（文档里写明）。
//     ★ 三条硬约束（单次哔 ≤300ms / `Long` 降级 3 短哔 / 绝不做长鸣）落在
//        `lib/dashcore/buzzer_exio.*` 里，理由与那次黑屏事故的关系见那里的文件头。
//   · pcpreview 上仍是 `BuzzerHost`（打印 `BEEP pattern=… ms=…` 一行，
//     可选 -DBUZZER_HOST_SOUND=1 出系统提示音）⇒ 模拟页上能看见"什么时候会响"。
//   · 未接显示的构建（抓帧盒 `esp32s3`/`esp32dev`）仍是 `BuzzerNull`（不发声）：
//     那边连屏都没有，更不需要发声。
static Alerts g_alerts;
static BuzzerNull g_buzzer_null;
#if defined(DASH_DISPLAY_PREVIEW)
static BuzzerHost g_buzzer_host;
#endif
#if defined(DASH_DISPLAY_RGB)
// ★★ 这一整段（实例 + 绑定点 + 串口命令 `b`）只在真屏那一份构建里存在。
//   门用的是**既有的** `DASH_DISPLAY_RGB` 宏（platformio.ini 里早就有、且**只有**
//   `[env:esp32s3-rgb]` 定义它）⇒ `[env:esp32dev]` / `[env:esp32s3]`（**VAN 抓帧盒**）
//   以及 extends 它们的 `-vaninv` / `-vansniff` / `-spi` / `-linkloop` 的编译单元里
//   **一行都不存在**，行为逐字节不变 ✓。**没有新增任何 -D** ✓。
//
// ★ 为什么要一个 `setExio` 转发：`BuzzerExio` 与显示驱动之间靠一个**函数指针**
//   解耦（`BuzzerExioSetFn`），而不是让 `lib/dashcore` 反过来 include `src/` 的头
//   —— 口径与 `buzzer_host_printf()` 那条一致（见 buzzer.cpp 的说明）。
//   真机写总线那一侧**只有一处**：`src/dash_display_rgb.cpp` 的 `dash_buzzer_set()`
//   （它拿着影子寄存器做读-改-写；另写第二份 = 丢位 = 面板复位）。
static BuzzerExio g_buzzer_exio(&dash_buzzer_set, nullptr, &millis);
#endif
static Buzzer* g_buzzer = &g_buzzer_null;

// ★ 蜂鸣器"绝对上限掐过几次"的两个出口（2026-09-25 新增）。
//   ★ 为什么要有这个读数：真机上"主循环有没有停过"这件事，除了屏上的现象，
//     只有这一格是**硬的** —— `BuzzerExio::safety()` 只在"起表之后超过 2 秒还在响"
//     时才 +1，而那正是"没人去执行到点关"的形态（见 `lib/dashcore/buzzer_exio.h`
//     的 `kBuzzerSafetyMs` 那一段）。
//   ★ 门用**既有的** `DASH_DISPLAY_RGB`（真屏那一档）：其余构建里 `g_buzzer` 挂的是
//     `BuzzerNull`/`BuzzerHost`（没有这个读数）⇒ 这里必须编译期分叉，不新增任何 -D。
#if defined(DASH_DISPLAY_RGB)
static uint32_t buzzer_safety_cuts() { return g_buzzer_exio.safetyCuts(); }
#else
static uint32_t buzzer_safety_cuts() { return 0u; }
#endif

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

// ---- 原始帧转发（`0x21 VANRAW`，2026-09-27）的两个方向各自计数 ----
// 为什么两个方向各计一套：它们回答的是**完全不同**的问题 ——
//   · 主板：我**搬出去**了多少帧、有没有搬不动/搬丢的（发送侧的健康度）；
//   · 从板：我**收下并解出**了多少帧、有几帧解不开（接收侧的健康度）。
// 判据（回放时对数用）：主板的 `push` 数 ≈ 从板的 `ok` 数（链路丢包由它们的差可见），
//   而两者都应当与"设备自己解出的 VAN 帧数"（`g_van_frames_seen`）同量级。
#if LINK_ROLE == 1
// 主板侧没有额外计数器：发送侧那几个数直接读 `g_van_raw` 自己的（见打印处）。
#else
static uint32_t g_vanraw_ok = 0;
static uint32_t g_vanraw_bad = 0;           // 载荷长度对不上 ⇒ 丢帧并计数
static uint32_t g_vanraw_last_ms = 0;       // 最近一帧成功解出的时刻（0 = 还没有过）
#endif
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

// ============================================================================
// ★★ 复位原因 + 复位次数（2026-09-24 新增）—— 第 ③ 层防线的**取证手段**
// ============================================================================
// ★ 起因（业主原话）："屏幕回来了，**上实车的时候可不能这样，这毕竟是仪表盘，
//   要常亮的**。" 而 2026-09-24 当晚的现场是"**屏黑了、固件一直活着**"：
//   复位前抓到的原文是 `rgb: vsync=68942(+54/s) … timeout=0 … fullrb=0/s`
//   + 每秒一行 `206 dash ok`；我按了一次 RTS 脉冲（复位）之后立刻恢复。
//
// ★ 这一次事故的**两条候选机制**（`docs/RGB-PANEL-2.8C.md` §13.8.2）里：
//     A = 运行期改 I2C 时钟 ⇒ 打错字节 ⇒ 面板复位 —— **已删除**（那条路径没了）；
//     B = **电流/供电把 3.3V 轨拉低** ⇒ 面板（ST7701 内部寄存器/电荷泵）掉状态，
//         而 ESP32 活着 —— **一直没排除**。
//   ⇒ 机制 B 如果成立，"**电压掉到 brownout 门限以下**"这件事在芯片上**有记录**：
//     `esp_reset_reason()` 会给出 `ESP_RST_BROWNEOUT`（枚举名见 esp_system.h，
//     本项目这一版 IDF 里写作 **`ESP_RST_BROWNOUT`**）。
//   ⇒ 所以开机打这一行，是**最便宜、也最直接**的一次取证：
//        `boot: reason=BROWNOUT n=3 (raw=9)`
//     如果屏哪天又黑了，而**之后**的某次复位原因是 BROWNOUT ⇒ 机制 B 基本坐实
//     （那种"自己重启过"的复位与"我手动按 RST"的 POWERON 在枚举上是可分的）。
//
// ★ 次数的口径（说清楚，免得读歪）：
//   `n` 是**上电/复位累计次数**，存在 NVS 里（命名空间 `dash`、键 `bootn`），
//   掉电不丢。它回答的是"这块板到底自己重启过多少次" —— 车上没有 USB 主机、
//   也没人盯着串口，所以"重启过"这件事只能靠**自己记着**。
//   ★ 它**不是**"黑屏次数"（软件感知不到玻璃，见下面那条边界）。
//
// ★★ **边界（必须写清楚）**：`esp_reset_reason()` 说的是**芯片**为什么复位，
//   它**不能**证明"面板黑过"。反之亦然：机制 B 若只是"电压瞬间跌到面板掉状态、
//   而没跌到 ESP32 的 brownout 门限"，那这一行**什么都不会报** ——
//   这种情况下本行给不出结论，只能靠守护的读数（诊断页那一行）+ 现场 `r` 命令。
static const char* kPrefsBootKey = "bootn";

// ★★ 2026-09-25：**跨重启留档**（上一次的复位原因 / 上一次运行了多久 / 守护累计计数）。
//   起因是本单的那个悬案（逐字记在案）：累计启动次数在夜里 `31 → 32`（多了一次），
//   而串口上只看到**当前**这次开机的 `reason=POWERON` —— **上一次的 reason 没有留档**
//   ⇒ "那次到底是 USB 被断电（POWERON）还是板子自己掉电（BROWNOUT）"**判不出来**。
//
//   ⇒ 判据层（读写回调、心跳、累计、边界）全部在 `lib/dashcore/boot_persist.{h,cpp}`
//     （宿主机逐条钉着：`test/test_dashcore/test_boot_persist.cpp`）；**这里只接 NVS**。
//     ★ 命名空间复用既有的 `dash`（`mute` / `bootn` 都在那里）—— 一块板上的持久化
//       状态只有这一处有主，不新开命名空间。
//     ★ 掉电不丢的那几样：`b_prevr`/`b_prevn`（上一次的复位原因，名字 + 原始值）、
//       `b_up_ms`（上一次开机时的 uptime）、`b_hb_ms`/`b_hb_n`（心跳：上一次跑到哪）、
//       `b_grd`/`b_gfix`/`b_gbl`/`b_ganm`/`b_gsnap`（守护四个计数的累计 + 落盘次数）、
//       以及既有的 `bootn`（启动次数）。
static bool bootNvsRead(BootKey k, uint32_t* out, void*) {
  if (!g_prefs.begin(kPrefsNamespace, /*readOnly=*/true)) return false;
  bool ok = false;
  switch (k) {
    case BootKey::BootCount:      ok = g_prefs.isKey(kPrefsBootKey);          if (ok) *out = g_prefs.getUInt(kPrefsBootKey, 0u); break;
    case BootKey::PrevReasonRaw:  ok = g_prefs.isKey("b_prevn");              if (ok) *out = g_prefs.getUInt("b_prevn", 0u); break;
    case BootKey::PrevUpMs:       ok = g_prefs.isKey("b_up_ms");              if (ok) *out = g_prefs.getUInt("b_up_ms", 0u); break;
    case BootKey::GuardRd:        ok = g_prefs.isKey("b_grd");                if (ok) *out = g_prefs.getUInt("b_grd", 0u); break;
    case BootKey::GuardFix:       ok = g_prefs.isKey("b_gfix");               if (ok) *out = g_prefs.getUInt("b_gfix", 0u); break;
    case BootKey::GuardBl:        ok = g_prefs.isKey("b_gbl");                if (ok) *out = g_prefs.getUInt("b_gbl", 0u); break;
    case BootKey::GuardAnom:      ok = g_prefs.isKey("b_ganm");               if (ok) *out = g_prefs.getUInt("b_ganm", 0u); break;
    case BootKey::GuardSnaps:     ok = g_prefs.isKey("b_gsnap");              if (ok) *out = g_prefs.getUInt("b_gsnap", 0u); break;
    case BootKey::HeartbeatMs:    ok = g_prefs.isKey("b_hb_ms");              if (ok) *out = g_prefs.getUInt("b_hb_ms", 0u); break;
    case BootKey::HeartbeatN:     ok = g_prefs.isKey("b_hb_n");               if (ok) *out = g_prefs.getUInt("b_hb_n", 0u); break;
    default: break;   // PrevReason（名字）走下面那条字符串回调
  }
  g_prefs.end();
  return ok;
}

static bool bootNvsWrite(BootKey k, uint32_t v, void*) {
  if (!g_prefs.begin(kPrefsNamespace, /*readOnly=*/false)) return false;
  bool ok = true;
  switch (k) {
    case BootKey::BootCount:     ok = g_prefs.putUInt(kPrefsBootKey, v) > 0u; break;
    case BootKey::PrevReasonRaw: ok = g_prefs.putUInt("b_prevn", v) > 0u; break;
    case BootKey::PrevUpMs:      ok = g_prefs.putUInt("b_up_ms", v) > 0u; break;
    case BootKey::GuardRd:       ok = g_prefs.putUInt("b_grd", v) > 0u; break;
    case BootKey::GuardFix:      ok = g_prefs.putUInt("b_gfix", v) > 0u; break;
    case BootKey::GuardBl:       ok = g_prefs.putUInt("b_gbl", v) > 0u; break;
    case BootKey::GuardAnom:     ok = g_prefs.putUInt("b_ganm", v) > 0u; break;
    case BootKey::GuardSnaps:    ok = g_prefs.putUInt("b_gsnap", v) > 0u; break;
    case BootKey::HeartbeatMs:   ok = g_prefs.putUInt("b_hb_ms", v) > 0u; break;
    case BootKey::HeartbeatN:    ok = g_prefs.putUInt("b_hb_n", v) > 0u; break;
    default: ok = false; break;
  }
  g_prefs.end();
  return ok;
}

static bool bootNvsReadStr(BootKey, char* out, uint32_t cap, void*) {
  if (!g_prefs.begin(kPrefsNamespace, /*readOnly=*/true)) return false;
  // ★ `getString` 在旧核心上会**截断**（copy 到固定缓冲）—— 传进去的 `cap` 就是
  //   那个缓冲的大小，本层只用"上一次的 reason 名字"（最长 8 字符），够放。
  const size_t n = g_prefs.getString("b_prevr", out, cap);
  g_prefs.end();
  return n > 0u;
}

static bool bootNvsWriteStr(BootKey, const char* s, void*) {
  if (!g_prefs.begin(kPrefsNamespace, /*readOnly=*/false)) return false;
  const size_t n = g_prefs.putString("b_prevr", s);
  g_prefs.end();
  return n > 0u;
}

static BootPersist g_boot_persist(bootNvsRead, bootNvsWrite, bootNvsReadStr,
                                  bootNvsWriteStr, nullptr);
// 开机那一行读到的"上一次"（诊断页与开机日志都要用，所以留在文件作用域里）。
static BootInfo g_boot_info;
// 最近一次心跳：落盘时打一行（平时一个字都不打）。
static uint32_t g_boot_hb_n = 0;   // 心跳累计次数（上一次开机时读回来的 + 本次）


static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";    // 上电（插线/上电）
    case ESP_RST_EXT:      return "EXT";        // 外部复位脚（板上那颗 RST 按钮）
    case ESP_RST_SW:       return "SW";         // 软件复位（esp_restart）
    case ESP_RST_PANIC:    return "PANIC";      // 异常/看门狗 panic（崩溃）
    case ESP_RST_INT_WDT:  return "INT_WDT";    // 中断看门狗
    case ESP_RST_TASK_WDT: return "TASK_WDT";   // 任务看门狗
    case ESP_RST_WDT:      return "WDT";        // 其它看门狗
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";   // ★ 电压跌落 —— 机制 B 的判据
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
  }
}

// 打这一行 + 把"本次"存回 NVS（上一次的 reason / 上一次的 uptime / 启动次数 +1）。
// ★ 顺序是**有意的**（每一步都对着一个"掉电时刻"，实现在 `BootPersist::begin()`）：
//     ① 先把 NVS 里"上一次"读出来 → ② 把本次的 reason 与 uptime 写下去（这一次写不能省）
//     → ③ 启动次数 +1 → ④ 才算得出"上一次运行了多久" → ⑤ 最后才打日志。
static void boot_note() {
  const esp_reset_reason_t r = esp_reset_reason();
  g_boot_info = g_boot_persist.begin(millis(), (uint32_t)r, resetReasonName(r));
  g_boot_hb_n = g_boot_persist.heartbeats();
  const uint32_t n = g_boot_info.boot_count;
  // `prev_up=`：拿不到就是 `-`（**不许**写 0 —— 0 会被读成"上一次刚起来就重启了"，
  //   而那正好是我们要判的两种病之一）。
  char prev_up[12];
  if (g_boot_info.prev_up_ms == kBootUpUnknown) {
    prev_up[0] = '-'; prev_up[1] = '\0';
  } else {
    snprintf(prev_up, sizeof(prev_up), "%lumin",
             (unsigned long)boot_minutes(g_boot_info.prev_up_ms));
  }
  // `prev=`：第一次跑带本层的固件时是 `-`（不是 `UNKNOWN` —— 那会被读成"上一次是
  //   一种叫 UNKNOWN 的复位"，而事实是"**没有留档**"）。
  const char* prev_name = "-";
  char prev_with_raw[20];
  prev_with_raw[0] = '\0';
  if (g_boot_info.prev_valid) {
    if (g_boot_info.prev_reason_name[0] != '\0') {
      prev_name = g_boot_info.prev_reason_name;
      snprintf(prev_with_raw, sizeof(prev_with_raw), "%s (raw=%lu)",
               g_boot_info.prev_reason_name, (unsigned long)g_boot_info.prev_reason_raw);
    } else {
      // 只有原始值（名字那个键没存下来）⇒ 名字这一格写 `?`，原始值照给。
      prev_name = "?";
      snprintf(prev_with_raw, sizeof(prev_with_raw), "? (raw=%lu)",
               (unsigned long)g_boot_info.prev_reason_raw);
    }
  }
  // ★★ 一行打完（`dash_logf` 一行上限 320 字节；这行是 ASCII）。字段口径：
  //   · `reason` / `raw` = **本次**为什么起来（既有）；`n` = 第几次上电/复位（既有）；
  //   · `prev=` = **上一次**的 reason + 原始值 —— ★ 2026-09-25 新增，本单的核心：
  //     `prev` 与 `reason` **不一样**时（例：`reason=POWERON prev=BROWNOUT`）说明
  //     上一次不是人插拔电，而是**板子自己掉电**；
  //   · `prev_up=` = **上一次运行了多久**（靠 10 分钟一次的心跳留档，见 boot_persist.h）；
  //     `prev_up=27min` = 长跑后断电（正常），几分钟的读数 = 异常；
  //   · `nvs-` = 这一次的计数/留档**没写进 NVS**（下一次开机看不到这一次）；
  //     它与 `prev=-`（NVS 好用、只是**还没有**上一次）是**两件不同的事**，别读混。
  // ★★ 2026-09-24 深夜**撤回**：这里原来还有一项 `tc=%.1fC`（片上温度）。
  //   它被砍掉的理由是**代价与收益不成比例**（不是"没用"）：
  //     · 收益：车上"温度"那条风险（`ARCHITECTURE.md` §3.5.3）多一个随手读数；
  //     · 代价：实测 **+6,080 B flash** —— 本构建这一版 **arduino-esp32 3.3.9** 的
  //       `temperatureRead()`（`cores/esp32/esp32-hal-misc.c`）在 **S3** 上走的是
  //       `#elif SOC_TEMP_SENSOR_SUPPORTED` 那一支，也就是 **IDF 的
  //       `temperature_sensor` 驱动**（`temperature_sensor_install/enable/get_celsius`），
  //       不是旧 ESP32 那条"读一个 ROM 函数"的免费路径 ⇒ 整个驱动被链进来。
  //       ★ "换成 Arduino 的 `temperatureRead()` 会不会便宜"这条**已经试过**：
  //         这里用的**就是**它 ⇒ 没得再省。
  //     · 而且 **片上温度 ≠ 环境温度** ✗（那是芯片自己的**结温**，还会被自己的功耗抬起来）
  //       —— 我们真正要问的那个问题是"**车上那个温度传感器的读数**"，
  //       那属于 VAN 那一单，不归这里。
  //   ⇒ 一行日志换 6 KB flash 不值当，砍掉。
  //
  //   ★★ **想加回来怎么做**（照这个顺序，代价已知 ≈6 KB）：
  //     ① 这里加回 `const float tc = temperatureRead();`
  //     ② 格式串加回 `tc=%.1fC`，参数加 `(double)tc`；
  //     ③ 重新量一次 flash（`pio run -e esp32s3-rgb` 的 `Flash:` 行）确认代价仍是 ~6 KB；
  //     ④ 想清楚要它回答的是什么问题 —— 若是"舱内温度"，**应该从 VAN 取**，不是加这个。
  dash_logf("boot: reason=%s n=%lu%s (raw=%d) up=%ums | prev=%s prev_up=%s | hb=%lu | role=%s\n",
            resetReasonName(r),
            (unsigned long)n,
            g_boot_info.nvs_ok ? "" : "(nvs-)",
            (int)r,
            (unsigned)millis(),
            prev_with_raw[0] != '\0' ? prev_with_raw : prev_name,
            prev_up,
            (unsigned long)g_boot_hb_n,
            // ★★ 2026-09-25：本机角色（**编译期**定死，§5）—— 两块 2.8C 外观一样，
            //   而"这块板是谁"只存在于刷进去的那份固件里 ⇒ 开机那行必须自报家门。
            //   取值与 `link_role.h` 的 `kRoleMaster/kRoleSlave` 同一口径（1=主板/右）。
            //   ★ 纯 ASCII（日志这条路上中文会踩 GBK 控制台，见 README 那条纪律）。
            LINK_ROLE == 1 ? "MASTER" : "SLAVE");
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
#if OBD_BLE
// ★★ BLE 那条路（2026-09-27 新增）：2.8C 上 UART 版**物理上没脚** ——
//   RGB 并口把 GPIO17/18 占了（正是 OBD 的默认 RX/TX）⇒ 只剩 BLE。
//   诊断头结构（服务 FFF0 / 通知 FFF1 / 写 FFF2）是笔记本侧实测出来的，
//   见 `docs/BLE-OBD.md`。
// ★ 为什么定义在**文件作用域**而不是 `attachObdSerial()` 里：主循环那边
//   （1 Hz 体检行 + 每圈的 `tick()`）也要用它。函数内的 static 出不来。
static ObdTransportBle g_obd_ble;
#endif

// 挂上 OBD 串口,并把 VehicleDataService 的指针换成 &Serial1。
// ★ 必须在 setup() 里做,不能在静态初始化期做:Serial1 的 begin() 要等
//   运行时(时钟/外设都就绪)才安全。
// 返回该服务,setup() 里这样用:`g_data = attachObdSerial();`
VehicleDataService& attachObdSerial() {
#if OBD_BLE
  // ★★ 2026-09-27 车上踩到（**屏幕全黑**的根因）：**只把指针接上，不要在这里 start()**。
  //   `g_obd_ble.start()` 会调 `NimBLEDevice::init()` —— 而 NimBLE 协议栈要吃掉一大块
  //   **内部 RAM**（实测 ~50KB：heap 从 98KB 掉到 49KB）。而这个函数跑在
  //   `dash_ui_init()` **之前** ⇒ 等轮到 RGB 面板分配弹跳缓冲时已经没内存了：
  //       E lcd_panel.rgb: lcd_rgb_panel_alloc_frame_buffers(190): no mem for bounce buffer
  //       E lcd_panel.rgb: esp_lcd_new_rgb_panel(357): alloc frame buffers failed
  //       rgb: 面板创建失败 err=257
  //   ⇒ 现象是"主板上电后屏一直黑"（`rgb: vsync=0 flush=0 refresh=0/0/0us`），
  //     但主循环、链路、VAN 全都正常 —— 极易误判成"屏坏了/线松了"。
  //   ⇒ 真正的 start() 挪到显示初始化**之后**（见 setup() 里那一处 `obdBleStartLate()`）。
  g_data = VehicleDataService(&g_obd_ble);
  dash_logf("obd: BLE 那条路已挂上(目标服务 FFF0 / 通知 FFF1 / 写 FFF2) —— start() 推迟到显示初始化之后\n");
#elif OBD_SERIAL
  Serial1.begin(kObdBaud, SERIAL_8N1, OBD_RX_PIN, OBD_TX_PIN);
  // ★ 2026-09-27：`VehicleDataService` 的参数从 `HardwareSerial*` 换成了
  //   `ObdTransport*`（理由见 `lib/dashcore/obd_transport.h`）。
  //   串口这条路用 `ObdTransportSerial` 包一层 —— 行为与抽取之前逐字节一致。
  static ObdTransportSerial g_obd_serial(&Serial1);
  g_data = VehicleDataService(&g_obd_serial);
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

#if LINK_ROLE == 1
// 原始帧的转发队列（只有主板发这一路）。★ 类型与用法见 `lib/link/link_app.h`
// 的 `VanRawQueue`；其余链路对象（`g_link_tx` 等）在下面"双板链路 v1 的接线"
// 那一节，这里单独放是因为 `van_frame_in()` 要用它 —— **只有主板**有生产者。
static dashlink::VanRawQueue g_van_raw;
// 主循环一圈最多往 `LinkTx` 搬几帧（封顶，剩下的下一圈再走）。
static const uint8_t kVanRawFramesPerLoop = 8u;
#endif

// ============================================================================
//  VAN 帧的**唯一入口**（2026-09-27 新增：原始帧转发那一单）
// ============================================================================
// 为什么要有这一个函数：这条链路上原来有**两个入口**，而且口径不一样 ——
//   · 物理层那条：`g_van_phy.tick()` → `VanLogSink`（打印 + 数帧）→ `g_van_sink`；
//   · 串口回放那条（`van_replay_feed`）：直接 `g_data.onVanPacket()` ——
//     **绕过了 VanLogSink**。
//   ★ 于是 `VanLogSink` 上那句"它两条路径都覆盖：GPIO 物理层收帧、以及串口离线
//     回放"**是错的**，而且实测可见：整趟实车录像回放（6561 帧）期间，
//     主板日志里 `VAN 824` 一行都没有、`g_van_frames_seen` 也**一帧都没涨**
//     （用它做诊断页的"本机解出的帧数"会显示 0）。
//   ⇒ 本单要加"原始帧转发"时这件事就变成了硬约束：**两条入口必须汇到同一处**，
//     否则回放**永远验不了转发**（那正是手上唯一能做的验证手段）。
// 本函数就是那一处：数帧 → （主板）入转发队列 → 喂数据层。
//   ★ 打印**不在这里**：80 Hz × 一行 46 B ≈ 3.7 KB/s 的日志负担只有物理层那条
//     路径才付（它本来就在付），回放那条继续不打印 —— 回放的证据是 `SRC` 那几行
//     与 `vanraw` 计数，不是逐帧行。
static void van_frame_in(const VanPacket& pkt) {
  ++g_van_frames_seen;
  if (pkt.fcs_ok) ++g_van_frames_fcs_ok;
  g_van_last_rx_ms = pkt.rx_ms;
#if LINK_ROLE == 1
  // 只往队列里拷字节（纯内存、不碰 PHY）—— **不在回调里发**，见 link_app.h。
  g_van_raw.push(dashlink::vanRawFromPacket(pkt));
#endif
  g_van_sink.onPacket(pkt);
}

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
    // ★★ 数帧与喂数据层**都不在这里**了（2026-09-27）：统一走 `van_frame_in()`
    //   —— 回放那条入口原来绕过本 sink，导致"诊断页的帧数"在回放时恒为 0、
    //   而原始帧转发在回放里**根本不会发生**（回放是手上唯一的验证手段）。
    //   本类的职责从此只剩"把这一帧打成一行日志"，见 `van_frame_in()` 的说明。
    van_frame_in(pkt);
  }
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
//    ★★ 同一条道理适用于**回放口**（2026-09-25 补）：`van_replay_poll()` 原来无条件
//      从 `Serial0` 收文本回放行 —— 那在"43/44 是链路"的构建里就是**抢**：它会把
//      链路帧的字节当文本吃掉。现在那一支由 **`DASH_LOG_UART0`** 这门一起看着
//      （见 `van_replay_poll()` 里那行 `#if`）：UART0 归链路时它**一行都不存在**。
//      ⇒ 带链路 PHY 的构建里，文本回放只走原生 USB-CDC（插 12PIN 的 19/20 即可），
//        与日志同一个口、也同一套"这台机器上怎么看串口"的做法（见 docs/LINK-TWO-BOARD.md）。
// ★★ 两个角色各自的链路对象（2026-09-25 起**两侧都有真 PHY**）
//
//   形态刻意保持不变（`g_link_phy` 这个名字两侧同一个、`LinkRx` 两侧共用一套
//   解帧/重同步/角色自检），改的只有两处：
//     · 从板的 PHY 从 `LinkPhyNull` 换成 `LinkPhyUart`（有 `LINK_PHY_UART` 时）
//       —— 上行（B→A）这才真的跑得起来；
//     · 从板多了一个 `pumpTx()`：排水这一步两侧都要有。
//
//   ★★ 2026-09-27：上面那句"从板那个环今天还空着"**已经作废** —— 从板现在真的发
//     两个消息（见下面 `link_slave_tick()` 那一段）：
//       · `HELLO`(0x01) —— **双向**（§3 表 `0x01` 行），与主板同一条口径
//         （`helloDue()`：上电 1 次、之后每 5 s，直到收到对端 HELLO）；
//       · `STATUS`(0x30) —— §3 表写的就是 **B → A 的 2 Hz**，而从板此前**一行发送
//         代码都没有** ⇒ v1 的"双向"只兑现了 A→B 那一半，主板那 30 s 的
//         "从板无响应"判据恒为真（§8 L13 那行日志永远等不到 `B 在线`）。
//     这两条**只补"谁在什么时刻 enqueue"**：TYPE / LEN / 字段次序 / CRC 覆盖范围
//     与 `LINK_ROLE` 的编译期权威（§5）一个字都没动。
//
//   ★ 两侧的"谁发什么"仍旧由 `LINK_ROLE` 说了算，**没有**变成运行期判断（§5）。
#if LINK_ROLE == 1
static dashlink::LinkTx      g_link_tx;    // §1.2 ②：自有环 ≥512 B，整帧进出
static dashlink::LinkRx      g_link_rx;    // §2 的重同步 + §5 的角色冲突自检
static dashlink::TickGen     g_link_tick;  // §3 的 TICK（50 Hz / 20 ms）
static dashlink::DataSender  g_link_data;  // §3 的 DATA（跟随 0x824 到达，不另建定时器）
#else
static dashlink::LinkRx      g_link_rx;    // 收：§2 的重同步 + §5 的角色冲突自检
static dashlink::LinkTime    g_link_time;  // §4：TICK 偏移估计 + 三级超时
static dashlink::LinkTx      g_link_tx;    // 发：B→A 的 HELLO + STATUS（2026-09-27 起）
static dashlink::StatusSender g_link_status;  // §3 `0x30` 行的 2 Hz 节奏
#endif

// 链路 PHY 本身：**一份定义、两个角色共用**（§0：谁都是 43 发、44 收）。
// 有 `LINK_PHY_ESP_NOW` ⇒ ESP-NOW（无线那一档，**没有引脚**：txPin()/rxPin() 报 -1、
//                            port() 报 -1，见 link_phy_espnow.h）；
// 有 `LINK_PHY_UART`    ⇒ 真 UART0（115200 8N1 / GPIO43=TX、GPIO44=RX）；
// 都没有                ⇒ 空壳（pcpreview / esp32dev，见文件头）。
// ★★ 三条编译期纪律（下面那条 static_assert 判前两条）：
//   ① **一份固件只能有一个链路 PHY**（UART 与 ESP-NOW 抢的是同一份资源）；
//   ② 两个宏都为 0 是**合法**的（空壳档，链路静默，不卡主循环）；
//   ③ 谁被选到就**只由这两个宏**决定 —— `LINK_ROLE` 仍然只管"发什么、怎么消费"（§5）。
#if LINK_PHY_ESP_NOW && LINK_PHY_UART
// 这条不是防御性代码：两个宏同时为 1 只可能来自"复制了一个 env 忘了改一行"，
// 而那种构建在板上的表现是"某一档 PHY 静默不工作"（没有编译错误、只有行为丢失）。
// 宁可在编译期炸掉，也不要上板猜。
// ★ 用 `#error` 而不是 `static_assert`：2026-09-23 实测过"套模板的 static_assert
//   在 -Os 下不一定被求值"（见 link_phy_uart.cpp 里那段留档）—— 这种闸门唯一的价值
//   就是"一定拦得住"，所以用最朴素、最没有解释余地的 `#error`。
#error "一份固件里只能有一个链路 PHY：LINK_PHY_UART 与 LINK_PHY_ESP_NOW 不许同时为 1（见 platformio.ini 里那两个 -now env：它们显式写 -DLINK_PHY_UART=0）"
#endif
#if LINK_PHY_ESP_NOW
static dashlink::LinkPhyEspNow g_link_phy;
#elif LINK_PHY_UART
static dashlink::LinkPhyUart g_link_phy;
#else
static dashlink::LinkPhyNull g_link_phy;
#endif

#if LINK_PHY_ESP_NOW
// ★ 测速/测丢包的两个全局量**放在这里**（而不是 `meas_poll()` 旁边）：
//   两处收帧函数（`link_poll_frames_slave` / `link_poll_bounded_slave`，在本文件更前面）
//   里的"测量信封闸门"要用到 `g_meas_rx`，C++ 要求**先声明后使用**。
static dashlink::MeasSender   g_meas_tx;
static dashlink::MeasReceiver g_meas_rx;

// ============================================================================
//  ★★ 选项 (a) 的**可行性探针**（2026-09-27 深夜；车主选了 (a)，但先验前提）
// ============================================================================
//  (a) 的内容：把 TICK 的**发送**搬到一个更高优先级的任务里，好让它躲开主循环那
//  ~90ms 的抢占。可它**有一个前提必须先验证**：那 90ms 到底是
//    · "**另一个任务在跑**"  ⇒ 更高优先级的任务能躲开 ⇒ (a) 可行；
//    · "**关中断 / 关 cache**"（例如 flash 擦写期间 cache 停了）
//                            ⇒ **任何任务都跑不了**，加 TX 任务也一样没用 ⇒ 只能走 (b)。
//
//  判据：这个 **20ms 周期**的 `esp_timer` 回调（跑在 esp_timer 任务里，优先级远高于
//  loopTask）在 5 秒窗口里记到的**两次回调之间的最大间隔**：
//    · ≈20ms（与主循环的 88~219ms 无关）⇒ 躲得开 ⇒ (a) 可行；
//    · 也出现 ~90ms 的空档 ⇒ 抢占是全局性的 ⇒ (a) 无效，如实回报、改走 (b)。
//  ★ 只加探针、**不碰发送路径**：这一步本身不改变任何行为（与"先加归因再动手"同一条纪律）。
static volatile uint32_t g_probe_n       = 0;   // 回调次数
static volatile uint32_t g_probe_max_us  = 0;   // 本窗口内"两次回调之间的最大间隔"
static volatile uint32_t g_probe_last_us = 0;   // 上一次回调的时刻

static void probe_cb(void*) {
  // ★ 写者只有 esp_timer 任务一个（主循环只读并清零），所以这里不需要临界区。
  const uint32_t t = (uint32_t)esp_timer_get_time();
  const uint32_t d = t - g_probe_last_us;
  g_probe_last_us = t;
  if (d > g_probe_max_us) g_probe_max_us = d;
  g_probe_n = g_probe_n + 1u;
}

// 只在主循环里调一次（懒启动：不必去改 setup 的初始化顺序）。
static void probe_start_once() {
  static bool started = false;
  if (started) return;
  started = true;
  esp_timer_create_args_t args = {};
  args.callback = &probe_cb;
  args.name = "lnk_probe";
  esp_timer_handle_t t = nullptr;
  if (esp_timer_create(&args, &t) == ESP_OK) {
    esp_timer_start_periodic(t, 20000);   // 20ms = 与 TICK 同档
  }
}

// ============================================================================
//  ★★ 选项 (a)：把 **TICK 的发送**搬离主循环
//  （2026-09-27 深夜，车主拍板选 (a)；前提已用 `txprobe` 实测验证）
// ============================================================================
//  为什么做：实测 `wire_gap_max` = 92~99ms —— **发端自己**就有 ~95ms 的空档。它来自
//  主循环被抢占（258 个窗口里 52% 超过 100ms），而且就是收端 `gap_max`(105~120ms) 与
//  p99(39~47ms) 达不到契约的**下界**。
//  可行性：`txprobe` 显示 20ms 周期的高优先级定时器在主循环被抢占 86~108ms 的同时，
//  自己的节拍**稳在 20.16~20.50ms** ⇒ 这是"低优先级任务被饿着"，不是关中断/关 cache
//  ⇒ **更高优先级的任务躲得开**。
//
//  设计（★ 刻意把改动**关在链路里**：不动主循环优先级、不碰显示/渲染路径）：
//    · 这个任务**独占 TICK 的生成与发送**：每 20ms 直接往 PHY 的环里写一帧 TICK，
//      然后 `pumpTx()` ⇒ TICK 的节拍不再受主循环抢占影响。
//    · **DATA / HELLO / STATUS 仍走 `LinkTx`、仍由主循环排水** ⇒ `LinkTx` 依旧是
//      **单线程**（它的环没有跨任务共享）⇒ 那一层一行都不用改。这是本设计的关键取舍：
//      宁可让 TICK 绕过 `LinkTx`，也不去给 `LinkTx` 加锁。
//    · 于是**只有 PHY 的 TX 环**变成两个写者（任务写 TICK、主循环写 DATA）
//      ⇒ 用**互斥量**保护 `write()` + `pumpTx()` 这一小段。
//      ★ 为什么必须是互斥量而不是自旋锁：`esp_now_send()` **绝不能在关中断的临界区里调**
//        （它要拿驱动的锁）。互斥量允许阻塞，而 `esp_now_send()` 本身只把包拷进驱动的
//        待发队列就返回 ⇒ 持锁时间很短。
//      ★ 主循环那一侧用 **0 超时的 try-take**：拿不到就这一圈不排（帧留在环里，下一圈再走）
//        ⇒ **主循环永远不会被这个锁阻塞**（与"日志不许阻塞主循环"同一条纪律）。
//    · 任务优先级 `kLinkTxTaskPrio` = 15：远高于 loopTask 的 1，又低于 WiFi 的 23 与
//      esp_timer 的 22 ⇒ 不会饿着射频协议栈，但足以躲开那个抢占者（它 < 22）。
//    · 只在**主板**上建这个任务：从板发的是 HELLO(5s)+STATUS(2Hz)，没有时基节拍要保。
static SemaphoreHandle_t g_phy_tx_mux = nullptr;   // 保护 PHY 的 TX 环

static inline bool phy_tx_lock(uint32_t wait_ms) {
  if (g_phy_tx_mux == nullptr) return true;        // 还没建（开机极早期）⇒ 退化成旧行为
  return xSemaphoreTake(g_phy_tx_mux, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}
static inline void phy_tx_unlock() {
  if (g_phy_tx_mux != nullptr) xSemaphoreGive(g_phy_tx_mux);
}

#if LINK_PHY_ESP_NOW
static const uint8_t  kLinkTxTaskPrio     = 15u;
// ★ 5ms 一拍：够 50Hz 的 TICK（每 4 拍一次）与 100Hz 的测速 burst（每 2 拍一次）。
static const uint32_t kLinkTxTaskPeriodMs = 5u;

// ★★ 这个任务现在是**所有"按节拍发"的唯一出口**：TICK（主板）与测速 burst（两板都有）。
//   为什么把 burst 也搬进来：burst 帧原来由主循环发（`meas_poll` 的 ①发），于是
//   `wire_gap_max` 量到的是**主循环被抢占**（实测 98~108ms），而不是链路本身 ——
//   工具就没在量它该量的东西。搬进来之后，发端节拍由本任务保证。
//
//   ★ 线程纪律（本次改动唯一的风险点）：**`g_meas_tx` 只被本任务碰**。
//     主循环的 `w` 命令不再直接 `g_meas_tx.start()`，而是**置一个请求位**
//     （`g_meas_start_req`），由本任务在下一拍真正开跑 ⇒ 无跨任务竞态、无需加锁。
//     ★ 收端（`g_meas_rx`）**不进这个任务**：它由主循环的收帧闸门喂、也由主循环算摘要，
//       那条路本来就是单线程的 —— 别顺手把它也搬进来。
static volatile bool g_meas_start_req = false;

static void link_tick_task(void*) {
  TickType_t last = xTaskGetTickCount();
  uint8_t tick_div = 0;
  for (;;) {
    // ★ 固定节拍（不是 delay：那个会累积漂移）—— 这一条正是本任务存在的意义。
    vTaskDelayUntil(&last, pdMS_TO_TICKS(kLinkTxTaskPeriodMs));
    const uint32_t now = (uint32_t)millis();

    // ---- ① 测速 burst 的发送（`w` 的请求在这里真正开跑）----
    if (g_meas_start_req) {
      g_meas_start_req = false;
      g_meas_tx.start(now, dashlink::kLocalRole);
      dash_logf("meas: 开跑 count=%lu period=%ums -- 收端那行 `meas rx:` 会在 burst 结束后自动打出\n",
                (unsigned long)g_meas_tx.planned(), (unsigned)g_meas_tx.periodMs());
    }
    if (g_meas_tx.active()) {
      if (phy_tx_lock(5u)) {
        g_meas_tx.poll(now, g_link_phy);   // 只往 PHY 的环里写（不碰射频）
        g_link_phy.pumpTx(now);            // 真发在这里
        phy_tx_unlock();
      }
      if (!g_meas_tx.active()) {
        // 刚好发满：把发端那一行打出来（**发端判据**只有"真的按节奏发出去了"这一条；
        // 丢包/间隔/抖动**只有收端量得到**，别拿"发出去了"当"收到了"）。
        char line[256] = {0};
        dashlink::measFormatTxSummary(g_meas_tx, now, line, (int)sizeof(line));
        dash_logf("%s\n", line);
      }
    }

#if LINK_ROLE == 1
    // ---- ② TICK（50 Hz = 每 4 拍一次；只在主板）----
    if (++tick_div >= (uint8_t)(20u / kLinkTxTaskPeriodMs)) {
      tick_div = 0;
      dashlink::TickMsg tm;
      if (g_link_tick.due(now, &tm)) {              // 没到点就直接跳过这一拍
        uint8_t payload[dashlink::kTickLen];
        if (dashlink::packTick(tm, payload)) {
          uint8_t frame[dashlink::kFrameBytesMax];
          const uint16_t n = dashlink::encodeFrame((uint8_t)dashlink::MsgType::Tick, payload,
                                                   dashlink::kTickLen, dashlink::kLocalRole,
                                                   frame, (uint16_t)sizeof(frame));
          // ★ 拿锁有上界（5ms）：拿不到就放弃这一拍（丢的是**这一帧**，不是主循环的时间）。
          if (n != 0u && phy_tx_lock(5u)) {
            if (g_link_phy.write(frame, (size_t)n) == (size_t)n) {
              g_link_phy.pumpTx(now);
            }
            phy_tx_unlock();
          }
        }
      }
    }
#endif  // LINK_ROLE == 1
  }
}
#endif  // LINK_PHY_ESP_NOW

// 只在主循环里调一次（懒启动）：建互斥量 + （主板上）建 TICK 任务。
static void link_tx_task_start_once() {
  static bool started = false;
  if (started) return;
  started = true;
  if (g_phy_tx_mux == nullptr) {
    g_phy_tx_mux = xSemaphoreCreateMutex();
  }
  // ★ 两个角色都建这个任务：主板上它发 TICK + burst；从板上它只发 burst
  //   （从板没有时基节拍要保，但 `w` 也要能在从板上跑）。
  // 钉在 core 1（与 loopTask 同核：比它优先级高 ⇒ 到点就抢占它，这正是要的）。
  xTaskCreatePinnedToCore(&link_tick_task, "lnk_tick", 4096, nullptr,
                          (UBaseType_t)kLinkTxTaskPrio, nullptr, 1);
}
#endif  // LINK_PHY_ESP_NOW

#if !LINK_PHY_ESP_NOW
// 有线 / 空壳档：PHY 的 TX 环**只有一个写者**（主循环）⇒ 不需要锁。
// 这两个空壳让下面两处排水点的代码两种档位**逐字相同**，且那两档行为与改之前一致。
static inline bool phy_tx_lock(uint32_t) { return true; }
static inline void phy_tx_unlock() {}
#endif

// ★★ 一条走错过的路，留档（2026-09-27 上板实测）——**"让无线档只留一个读者"是错的**。
//
//   当时的推理：无线档下 PHY 的字节流有**两个读者**（`meas_poll()` 按帧分流 + 两处收帧
//   里的 `g_link_rx.poll(g_link_phy, …)`），后者会把测量信封当字节流吃进数据层
//   ⇒ 于是给 `LinkRx` 换了一个"永远不在线"的空壳 PHY（`LinkPhyNull`），想让
//   `meas_poll()` 当唯一读者。
//
//   **结果：链路直接死了** —— 从板 `link: sim seen=0`，而它的 PHY 明明收了 9966 帧、
//   `gap_rx=1ms`、`rx_foreign=0`（射频与分流都好好的）。
//   原因在 `LinkRx::feed()` 的语义：**`feed() = push + advance`，凑齐的帧会通过
//   `out` 直接交出来** —— 而 `meas_poll()` 里那个 `Frame f` 是局部变量，**帧被丢掉了**。
//   也就是说：原先链路能通，靠的**正是**那个"第二个读者"自己读 PHY；把它去掉之后，
//   `meas_poll()` 读到的帧既没被交出、也没留在缓冲里 ⇒ 一帧都到不了上层。
//   （`LinkRxStats::frames_ok` 当时记的 7867 就是这些被丢掉的帧 —— 那个数不是"收下了"，
//     而是"解出来了但没人接"。）
//
//   ⇒ 正确的修法是**把闸门加在消费端**：见两处 `handleInbound` 之前那段
//     "测量信封的最后一道闸门"。谁读的 PHY 不重要，**测量帧不许进数据层**才重要。
//   ★ 仍然存在的已知缺陷（下一步，不在本单）：`meas_poll()` 里 `feed()` 交出来的帧
//     被丢掉 ⇒ 那部分帧是**真丢**（会算进测量丢包）。正确做法是给它们一个小队列、
//     由收帧函数先取队列 —— 那时才真正是"单读者"，而且不丢帧。本单没做，如实留档。

// 开机那一行要报"这一份固件用的是哪种链路 PHY"（两种 PHY 的形状不同：
// UART 有引脚、无线没有，而 `txPin()` 在无线那一档报 -1 —— 直接打
// "TX=GPIO-1" 是读不懂的）。★ 放在这里、**不**散进 setup() 的两个分支：
// 这样"两个角色共用一份形状"那条设计在日志上也成立。
#if LINK_PHY_ESP_NOW
static const char* kLinkPhyTail() { return "—— 真 PHY(ESP-NOW)"; }
#elif LINK_PHY_UART
static const char* kLinkPhyTail() { return "—— 真 PHY(UART0)"; }
#else
static const char* kLinkPhyTail() { return "PHY 是空壳(这份固件没编 -DLINK_PHY_UART / -DLINK_PHY_ESP_NOW)"; }
#endif

#if LINK_ROLE == 1
// ★★ DATA 的**下限**（2026-09-25 上板实测补的一行）：**12 ms ⇒ ≤80 帧/s**。
//
// 为什么必须有它（这是实测出来的，不是防御性代码）：`DataSender::due()` 对
// `snapshot_ms == 0`（= 本机到现在还没收到过任何一帧 0x824 —— 车睡着、台面上没接
// VAN 收发器、或 VAN 那一路没启用）有一条**刻意的**"照发"口径，它把 `now_ms`
// 当快照时刻 ⇒ **主循环每一圈都能发一帧**。而主循环在 2.8C 上是 ~900 圈/秒 ⇒
// 实测（2026-09-25，COM6，直接把 UART0 上的字节流解出来）：
//     **DATA 946 帧/秒 / 12288 B/s = 115200 8N1 的满线速**（3 秒 2839 帧、CRC 全过）。
// 满线速意味着 `LinkPhyUart::pumpTx()` 每圈都在"FIFO 有位置"的边界上跑 ——
// 那正是 §1.2 要说清的那种"把主循环拖住"的形态（当年 `Serial0.write` 阻塞就是这么
// 毁掉 VAN 边沿采集的），而 §1.3 要求"链路发送单次很短、可随时被打断"。
// ⇒ 用**既有**的那个旋钮（`setMinIntervalMs`，`link_app.h` 里本来就有、语义就是
//   "别比这更快"）把速率压回契约给的那一档：§3 的 `DATA ≈ 79.7 Hz`（= 0x824 的周期
//   ≈12.5 ms）⇒ 取 **12 ms**（80 Hz，比 79.7 略快一点，不丢掉真实的 0x824 节奏）。
//
// ★ 它**不影响有 VAN 的那条主路径**：那时 `snapshot_ms != 0`，`due()` 走的是
//   "只有新的 0x824 才发"那一条，而 12 ms 的窗口对 80 Hz 的到达率是透明的。
// ★ 它**也不改** `DataSender` 自身的默认值（默认仍是 0 = 不限）—— 那是给用例与
//   "就想全速发"的场景留的口径；生产固件在两处显式设定：这里，以及
//   `test_link_app.cpp` 的 `test_link_app_data_sender_min_interval`（同一条判据的宿主面）。
static const uint32_t kLinkDataMinIntervalMs = 12u;

#endif  // LINK_ROLE == 1

// ============================================================================
//  HELLO 的三个状态 —— ★ **两个角色共用**（2026-09-27 从主板那一支里搬出来的）
// ============================================================================
// 为什么搬：§3 表 `0x01` 行的 HELLO 是**双向**的，两个角色的口径逐字相同
// （上电 1 次、之后每 5 s、直到收到对端 HELLO）。这三个变量原来只在 `LINK_ROLE==1`
// 那一支里定义 —— 从板那一支要发 HELLO 就得再定义一份，而"两份同名状态"迟早会漂。
// ⇒ 定义提到角色分支**外面**，两个角色共用；`kHelloRepeatMs`（那个 5 s）也搬到了
//   `lib/link/link_msg.h`，于是"5 s"在全仓库只出现一次。
//   ★ 分支外面定义**不代表两个角色都会发**：谁发由 `LINK_ROLE` 说了算（§5），
//     只是两边的**状态与判据**是同一份。
static uint32_t g_link_hello_ms = 0;         // 上一次发 HELLO 的时刻（0 = 还没发过）
static uint32_t g_link_peer_ms  = 0;         // 最近一次收到**对端任何一帧**的时刻
static bool     g_link_hello_acked = false;  // 收到过对端 HELLO ⇒ 停止重发（§3）
// 主板自己的固件版本/构建标记（§3 的 HELLO：`fw_ver` 与协议 `VER` **分开**）。
// 取值口径：仓库里没有既有编码（§3 也这么说），所以先定 0/0 并把出处写在这里 ——
// 真要拿它判"两块板是不是同一份固件"，得在两块板各自刷同一份固件时才可比。
// ★ 从板也报**同一对常量**：两块板刷的是同一次构建里的两份镜像，`fw_ver`/`build_tag`
//   本来就是"这份固件是哪一版"的标记（角色不算版本）⇒ 主板那行 `link: B HELLO fw=…
//   build=…` 后面**不该**出现"与本机 fw/build 不同"那句尾巴。真出现尾巴了，
//   说明两块板刷的不是同一次构建 —— 那正是这个字段要回答的问题。
static const uint16_t kLinkFwVer    = 0;
static const uint16_t kLinkBuildTag = 0;

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
  // ★ 角色这一格**两个角色都填**（2026-09-25）：诊断页第一行要写 `role=MASTER/SLAVE`
  //   —— 两块 2.8C 外观一样，而角色是编译期定死的（§5）⇒ 打开诊断页第一眼就该
  //   知道手里这块是谁。取值就是 `LINK_ROLE`（编译期唯一权威），没有第二处判据。
  in.link_role = (uint8_t)LINK_ROLE;
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

  // ---- ⑥.5 面板健康守护（第 ② 层防线；**只有真屏那一份构建**有这四个数）----
  // ★ 见 `lib/dashcore/panel_guard.h`：它每 2 秒回读一次那颗 TCA9554 的输出寄存器
  //   并与影子对账（`LCD_RST`/`LCD_CS` 就在那个寄存器里），另外核验背光占空。
  //   `fix` 不为 0 = 真的发生过"位被改写并且已经按影子修回来"。
  // ★ 其余构建（预览/抓帧盒）里这四个数恒 0 —— 那些构建里没有面板可守，
  //   "0"本来就是正确的读数（诊断页那一行不写 n/a，理由见 system_status.cpp）。
#if defined(DASH_DISPLAY_RGB)
  in.guard_rd_ok   = dash_panel_guard_rd_ok();
  in.guard_fix     = dash_panel_guard_fix();
  in.guard_bl      = dash_panel_guard_bl();
  in.guard_anomaly = dash_panel_guard_anomalies();
  // "守护上线多久"用 `millis()` 直接算：守护是在 `dash_display_init()` 里 begin 的，
  // 而那一步在 setup 里、离这里只有几百毫秒 ⇒ 用系统 uptime 当近似足够了
  // （这一格要回答的只是"守护还在跑吗"，不是精确的相位）。
  in.guard_uptime_ms = now_ms;
  // ★★ 2026-09-25：**跨重启累计**（上面四个数随重启归零，这四个不归零）。
  //   口径 = "上一次开机为止的累计（NVS 里的基线）+ 本次" —— 于是"某次夜里守护
  //   救过几回"不会随重启丢失（见 lib/dashcore/boot_persist.h）。
  //   ★ 累计取的是**开了机就写下去的基线**，所以"上一次运行结束时的累计"是完整的；
  //     最容易丢的只是"最后一次心跳之后那不到 10 分钟"的增量。
  {
    const GuardTotals base = g_boot_persist.guardBase();
    in.guard_total_rd    = boot_accum(base.rd,  in.guard_rd_ok);
    in.guard_total_fix   = boot_accum(base.fix, in.guard_fix);
    in.guard_total_bl    = boot_accum(base.bl,  in.guard_bl);
    in.guard_total_anom  = boot_accum(base.anom, in.guard_anomaly);
    in.guard_tot_snaps   = g_boot_persist.snapshots();
    in.boot_hb_n         = g_boot_persist.heartbeats();
    in.boot_count        = g_boot_info.boot_count;
  }
#endif

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
// ============================================================================
//  真机的**串口单字符命令**（2026-09-24 新增）—— 「诊断页 / 静音」的真机入口
// ============================================================================
// ★ 为什么真机入口是串口、不是按键：**这块板上没有可用的按键** ——
//   2.8C 整板上能当输入用的只剩 12PIN 的 `GPIO0`（= BOOT strap，不建议）
//   与排针上剩下的 `GPIO7`（I2C 的 SCL）。而 Type-C 口（板载 CH343P =
//   `Serial0`/UART0 = COMx）**本来就是**要接的那根线（看日志走同一个口）
//   ⇒ 用串口命令当下入口：零额外引脚、零额外接线。
//   与诊断页的既有口径一致：**平时不显示**，只有收到命令才唤出（L11 合规
//   说明见 system_status.h 的文件头）。
//
// ★★ 为什么整段只在 `DASH_DISPLAY_RGB`（= `[env:esp32s3-rgb]`）里编 ✗：
//   `[env:esp32s3]` / `[env:esp32dev]`（以及 extends 它们的 `-vaninv`/`-vansniff`）
//   是 **VAN 抓帧盒** —— 它们的串口上跑的是**抓帧数据/回放文本**，一个字节都
//   不许被吃掉。所以这两个命令**只**进真屏那一份构建，而且**不新增任何 `-D`**：
//   `DASH_DISPLAY_RGB` 是 platformio.ini 里**早就有的**、且**只有**
//   `[env:esp32s3-rgb]` 定义的那一个宏（见 dash_display.cpp 的驱动分支）
//   ⇒ 其余 env 的编译单元里**这段代码一行都不存在**，行为逐字节不变 ✓。
//
// ★ 命令（小写单字符，收到即回一行确认日志；**不做**行缓冲、不等回车）：
//   · `d` ⇒ 诊断页：与预览的 `K` **同一个循环** —— 关着 ⇒ 打开第 1 页；
//           还有下一页 ⇒ 翻页；在最后一页 ⇒ 关闭。回执与预览逐字同一行
//           （`diag: open page=1/2` / `diag: closed page=2/2`）。
//   · `m` ⇒ **静音开关取反**，并**写 NVS**（掉电保存；与 `setup()` 里读回的
//           是同一对 load/save）⇒ 车主**不用按键**就能静音。回执 `mute: 1 (saved)`。
//   · `b` ⇒ **蜂鸣器测试：响一声**（2026-09-24 新增）。★ 这是**人耳判据**的入口：
//           有源/无源那半问在 §13.6 已经由车主听出来了，但"**接上以后到底响不响**"
//           只能靠耳朵 —— 机上没有麦克风，所以留一个**按需**触发的按键：
//           不用等告警、不用等开机那 2.44 秒的 `trust: sim-fallback`。
//           ★ 用的是 `BeepPattern::Long` **故意如此**：验收要看的正是驱动里那条
//             "**长鸣 ⇒ 3 短哔**"的降级（`ARCHITECTURE.md` §4 第 2/3 条 +
//             `lib/dashcore/buzzer_exio.h` 文件头 ②）。
//             时长给 `kTrustBeepMs`（120ms，就是"数据不可信"那一声轻提示的量）
//             ⇒ 听到的是 **3 声、每声 120ms、中间隔 80ms**，绝不会是长鸣。
//           ★ 它走的是**与告警同一个 `g_buzzer`**（挂的是真机那一档 `BuzzerExio`）
//             ⇒ 这一声与"真告警响的那一声"是同一条路径，不是另开一条测试旁路。
//           ★ **静音（`m`）不拦它**：`m` 的语义是"关掉**自动告警**"（`alerts` +
//             那一声轻提示），而 `b` 是车主**主动按下去**的测试动作 ——
//             前提是"静音开关不绕开既有 Alerts/NVS 那条路"，这一条没动（见
//             `mute_toggle_from_serial()` 与 loop() 里那两处 `muted()` 判据）。
//             回执那一行会把当前静音态一起打出来，免得混起来。
//
// ★★ 命令**只认"行首"**（这个口上当前没有未完成的一行）—— 判据在下面
//   `serial_cmd_handle` 里：回放行里的十六进制**本来就可能含 `d`**（例
//   `VAN 824 18F8271D000000`）⇒ "缓冲里已经有东西"时不能无脑当命令；
//   **例外**是那串东西**不可能是回放行**（首字节不是 V/v）—— 串口线上一颗杂散
//   字节就能把"行首"这个位置一直占着、让后面每条命令都失联，所以那种时刻要认。
//   认不出来的字节一律原样交给下面既有的回放路径（一个字都不多吃）。
//   两个口（`Serial` = 原生 USB / `Serial0` = 板载 CH343P）共用这一个判据，
//   各自维护自己的行缓冲。
#if defined(DASH_DISPLAY_RGB)
// 诊断页那一个键的三步循环。★ 与 loop() 里预览那一支（`#if defined
// (DASH_DISPLAY_PREVIEW)` 的 `K`）**逐字同一条**：改这里要连那一段一起改
// （两处都只是"把请求喂给 dash_ui 的三个函数"，判据与页数都在 system_status.h）。
static void diag_key_press() {
  if (!dash_ui_diag_open()) {
    dash_ui_diag_toggle();
  } else if ((uint8_t)(dash_ui_diag_page() + 1u) >= kDiagPageCount) {
    dash_ui_diag_toggle();
  } else {
    dash_ui_diag_next();
  }
  // ★ 这一行与预览的回执**同一格式**（它只报页号，不报 fps —— 理由见 loop() 里
  //   那一段：`g_ui_fps10` 在本拍渲染之前读只会是 0 或上一拍的旧值）。
  dash_logf("diag: %s page=%u/%u\n", dash_ui_diag_open() ? "open" : "closed",
            (unsigned)(dash_ui_diag_page() + 1u), (unsigned)kDiagPageCount);
}

// 静音取反 + **落盘**（`Preferences`/NVS，命名空间 `dash`、键 `mute`，见上面那段）。
// ★ 三件事的顺序是有意的：先改内存态（这一拍就生效）→ 写 NVS（掉电记住）
//   → 同步给告警层（`g_alerts` 与 `g_beep_muted` 是两份状态，必须一起动）。
static void mute_toggle_from_serial() {
  g_beep_muted = !g_beep_muted;
  mute_save(g_beep_muted);
  g_alerts.setMuted(g_beep_muted);
  dash_logf("mute: %d (saved)\n", g_beep_muted ? 1 : 0);
}

// `b`：**响一声**（人耳判据的入口，见上面命令表里那段说明）。
// ★ 这里只做两件事：起一拍 + 打一行回执 —— **绝不阻塞**（真正的高低电平由
//   `loop()` 每轮推进，显示那一秒的帧照样照常画）。
// ★★ 为什么要那个"窗口"（`g_beep_cmd_until_ms`）：`loop()` 里驱动蜂鸣器那一支是
//   `if (g_alerts.beeping()) beep() else off()` —— 串口 `b` 起的那一拍里
//   `Alerts::beeping()` 恒为假，于是主循环**每轮都会调 `off()`**，而
//   `BuzzerExio::off()` 的语义是"**取消**"（静音那一跳靠它立刻掐断）
//   ⇒ 不处理的话 `b` 只会响第一声（**实测就是这么发现的**：3 短哔变成 1 声）。
//   修法：命令侧记一个"这一拍到什么时候为止"的窗口，`loop()` 那一支在窗口内
//   走"**只推进、不取消**"（序列由 `g_buzzer->tick()` 推进，见 loop() 里那一段）。
//   ★ 判据只有这一处（`beep_cmd_window()`），别在别处再写一遍。
//   ★ 窗口内**不调 `beep()`**（只 tick）：序列已经在跑了，重复起表只会把相位归零。
// ★ 静音（`m`）**不拦**这一声：`m` 关的是**自动告警**，而 `b` 是车主**主动**
//   按下去的测试动作。回执那一行会把当前静音态打出来，免得两者混起来。
static uint32_t g_beep_cmd_until_ms = 0;   // 串口 `b` 那一拍的到期时刻（0 = 没有）

// 串口 `b` 的那一拍还在窗口里吗？
// ★ 这里用 `millis() < 到期` 的**绝对值**比较（不是 `now - start` 那种差值形式）：
//   本窗口最长 560ms，`millis()` 那道 49.7 天的回绕**不可能**落进这么短的窗口里，
//   所以绝对值比较是安全的、也更好读。窗口一过就自动失效（`g_beep_cmd_until_ms`
//   跟着被清成 0）⇒ 下一次告警照旧走正常的 `beep()/off()` 那一支。
static bool beep_cmd_window(uint32_t now) {
  if (g_beep_cmd_until_ms == 0u) return false;
  if ((int32_t)(now - g_beep_cmd_until_ms) >= 0) {
    g_beep_cmd_until_ms = 0u;
    return false;
  }
  return true;
}

static void beep_test_from_serial() {
  // ★ `Long` 是**有意**的：这一声要验的正是驱动里"**长鸣 ⇒ 3 短哔**"那条降级
  //   （`ARCHITECTURE.md` §4 第 2/3 条 + `lib/dashcore/buzzer_exio.h` 文件头 ②）。
  g_buzzer->beep(BeepPattern::Long, kTrustBeepMs);
  const uint32_t total = 3u * kTrustBeepMs + 2u * kGapMs;   // 3 声 + 2 个间隙 = 520ms
  g_beep_cmd_until_ms = millis() + total + 40u;             // 留一点余量
  dash_logf("buzz: test beep cmd=b(Long>3short) ms=%u x3 gap=%ums muted=%d\n",
            (unsigned)kTrustBeepMs, (unsigned)kGapMs, g_beep_muted ? 1 : 0);
}

// `r`：**面板重初始化 + 重发当前画面**（2026-09-24 新增，第 ③ 层防线）。
//
// ★★ 为什么需要这条命令（业主原话就是它的需求文档）：
//     "屏幕回来了，**上实车的时候可不能这样，这毕竟是仪表盘，要常亮的**。"
//   而 2026-09-24 当晚真的出现过"**屏黑了、固件一直活着**"（串口上 `vsync` 照涨、
//   `timeout=0`、每秒一行 `206 dash ok`）。那次是靠**复位**（RTS 脉冲）救回来的 ——
//   可复位会把 ESP32 一起重启，车上没有 USB 主机、也没人按得到板上的 RST。
//   ⇒ 把这个动作做成一条**串口命令**：现场不用按物理 RST 也能救回来。
//
// ★ 命令**只做一件事**：调 `dash_display_panel_reinit()`（它自己打
//   `panel: reinit by cmd r (#n, Xms; …)` 那一行）。这一行与守护自动恢复那一路
//   共用同一个函数 ⇒ 两条路径的行为**逐字相同**（不存在"只有命令那条能修好"的分叉）。
//
// ★★ 为什么命令与自动恢复共用同一个入口（而不是各写一份）：
//   重初始化要动 LCD_RST/LCD_CS 那两位、要重跑 41 步、要重发一帧 —— 这套动作
//   只能有**一份**实现（与"那颗扩展器的影子寄存器只能有一份"是同一条纪律）。
static void reinit_from_serial() {
#if defined(DASH_DISPLAY_RGB)
  const bool ok = dash_display_panel_reinit("cmd r");
  if (!ok) {
    // 失败时把"为什么没做"也打出来（面板压根没初始化过 —— 那是另一种病：
    // 该查引脚/时序/PSRAM，而不是"黑屏恢复"）。
    dash_logf("panel: reinit by cmd r 未执行(见上一行)\n");
  }
#else
  dash_logf("panel: reinit 不适用(这份构建没有 RGB 面板驱动)\n");
#endif
}

// ★★ 临时故障注入的串口入口（`i`）—— **默认构建里恒为 false**。
//
//   用途（两个上板判据共用这一条路径）：
//     ② "最坏自愈窗 2s → ~200ms"：**注入一次** ⇒ 看守护多快发现；
//     ③ "自动重初始化"：**连续注入若干次** ⇒ 让守护"连续 3 次修不好"⇒
//        触发 `panelguard: anomaly x3 -> auto reinit`（那是唯一没上板验过的路径）。
//   ★ 打开方式：在 `[env:esp32s3-rgb]` 的 build_flags 里**临时**加
//     `-DPANEL_GUARD_FAULT_INJECT=1`（就是驱动里那个同一个宏），测完删掉。
//   ★ 默认构建（没有那个 `-D`）：本函数**恒返回 false**
//     ⇒ `serial_cmd_handle` 把 `i` 当成普通字符交回回放路径
//     ⇒ 与"命令表里没有 `i`"**逐字节相同**（这一条是刻意的：抓帧盒与正式固件
//        的串口行为一个字节都不许变）。
//   ★ 注入什么：把 `g_exio_out` 的**对位写反**（只写那一位到真寄存器，
//     影子故意不动）⇒ "影子 vs 回读"立刻不一致，正是黑屏那条最可能的路径。
//     位的含义：bit0=LCD_RST(EXIO1) / bit2=LCD_CS(EXIO3) / bit7=蜂鸣器(EXIO8)。
//
//   ★★★ 为什么要"按守护的节拍"注入（这一段是本单踩出来的关键，别改回去）：
//     守护的修复动作是"**按影子重写**"⇒ 注入之后**只要它检查一次就修好了**。
//     所以"连续 3 次异常"**不可能**靠"随便连点几下 `i`"造出来：第二次注入如果落在
//     "上一次检查之后、下一次检查之前"，那一次检查读到的就是**干净的**（影子已经被
//     写回去了）⇒ `streak_` 被清零 ⇒ 永远攒不够 3 次。
//     ⇒ 注入必须**正好落在守护下一次检查之前**（让它一读就撞上坏值）。
//     为此这里按"下一次检查的时刻"排注入：每 `kInjectGapMs` 一次、每次都在
//     `nextCheckMs() - kInjectLeadMs` 那一刻写坏，共 `kInjectTimes` 次。
//   ★ 安全：**只做一轮**、次数写死（不许循环折腾）；任何时刻屏都会在 ~1 个检查周期
//     内被修回来（最坏 200ms），并且"久不恢复就烧回"的镜像一直在手边。
#if defined(PANEL_GUARD_FAULT_INJECT) && (PANEL_GUARD_FAULT_INJECT == 1)
static const uint8_t  kInjectDefaultTimes = 4u; // `i` 不跟数字时注入几次（4 ⇒ 必然攒够 3 次）
static const uint8_t  kInjectMaxTimes = 9u;     // 上限（一位数字最大就是 9）
static const uint32_t kInjectGapMs    = 300u;  // 两次注入之间至少隔多久（车主看着屏时的观感）
static const uint32_t kInjectLeadMs   = 40u;   // 提前多久写坏（必须 > 一次 I2C 写事务）
static uint8_t  g_inject_left = 0;             // 还剩几次
static uint8_t  g_inject_want = kInjectDefaultTimes;  // 这一串一共几次（日志用）
static uint32_t g_inject_next_ms = 0;          // 下一次注入的时刻
static uint32_t g_inject_checks_at_last = 0;   // 上一次注入时守护的检查次数
static bool     g_inject_take_digit = false;   // 下一颗字节是不是"`i` 的次数位"
#endif

static bool fault_inject_from_serial(uint8_t times) {
#if defined(PANEL_GUARD_FAULT_INJECT) && (PANEL_GUARD_FAULT_INJECT == 1)
  // ★ 一次**开启**一串：由 `fault_inject_poll()` 按守护的节拍打完
  //   （它**不阻塞**这里 —— 串口路径上不许等）。
  //   位固定取 bit0（EXIO1 = LCD_RST）：这一位被打错就是一次面板复位，
  //   正是要复现的那条路径。
  //   ★ 次数：`i` 后面那一位（`i1` = 一次 ⇒ 量"发现窗"；`i`/`i4` = 四次 ⇒ 造"连续 3 次"）。
  if (times == 0u) times = kInjectDefaultTimes;
  if (times > kInjectMaxTimes) times = kInjectMaxTimes;
  g_inject_left = times;
  g_inject_want = times;
  g_inject_next_ms = millis();      // 第一次立刻（下一轮 poll 就写）
  g_inject_checks_at_last = dash_panel_guard_checks();   // 从"现在这一次"起算
  dash_logf("inject: armed x%u gap=%ums lead=%ums bit0(LCD_RST) checks=%u\n",
            (unsigned)times, (unsigned)kInjectGapMs, (unsigned)kInjectLeadMs,
            (unsigned)g_inject_checks_at_last);
  return true;
#else
  (void)times;
  return false;
#endif
}

// 每轮调一次：到点就把"注入 → 守护检查"这一步走完（**不阻塞**）。
// ★ 默认构建里这个函数体是空的（`#if` 那一支不存在）⇒ 主循环一个指令都没多。
static void fault_inject_poll(uint32_t now) {
#if defined(PANEL_GUARD_FAULT_INJECT) && (PANEL_GUARD_FAULT_INJECT == 1)
  if (g_inject_left == 0u) return;
  // 目标时刻 = **守护下一次检查之前** `kInjectLeadMs`（它一读就撞上坏值）。
  const uint32_t target = dash_panel_guard_next_check_ms();
  const bool on_target =
      (int32_t)(now - target) >= -(int32_t)kInjectLeadMs &&
      (int32_t)(now - target) < 0;
  if (!on_target) return;
  // ★★ **一次检查只喂一个坏值**（`dash_panel_guard_checks()` 变了才允许下一次）。
  //   ★ 这一条是本单踩出来的，写法也**故意没有时间间隔**（`kInjectGapMs` 只用于
  //     日志/说明，不参与门限）：
  //     · 不按"检查次数"锁：注入器会跑在守护前面连写坏值，守护读到的永远是坏值 ——
  //       它**自己**"连续发现三次"这件事反而攒不出来；
  //     · 按 300ms 间隔也不行（试过一版）：守护发现第一次之后进**快速复检**
  //       （200ms 一眼）⇒ 300ms 的间隔里必然夹着**一到两次干净读数** ⇒ `streak_`
  //       每次都被清零 ⇒ 三连异常永远攒不够。
  //     ⇒ 正确做法是"**每一拍检查之前都写一次坏值**"：那一拍读到坏值 ⇒ `streak_`
  //       才会 1→2→3。★ 安全性由"每次注入都在检查前 40ms、检查一读就修回来"
  //       这条保证：坏值在总线上只存在 ~25ms，屏闪一下就被修好。
  if (dash_panel_guard_checks() == g_inject_checks_at_last) return;
  --g_inject_left;
  g_inject_checks_at_last = dash_panel_guard_checks();
  g_inject_next_ms = now;           // 只用于日志（本路径不按时间门限）
  dash_panel_guard_fault_inject(0u);
#else
  (void)now;
#endif
}

// ★★ 2026-09-27（另一单）：无线那一档的**测速/测丢包**由这个命令发起
//   （`SerialCmd::Wire`）。它的**定义**在下面（无线 PHY 那一段里），
//   而命令表在这里 ⇒ 先给一条声明，形状与 `fault_inject_from_serial` 一致：
//   命令表只"发起"，动作在后面。
static void meas_start_from_serial();
static void meas_poll(uint32_t now);

static bool serial_cmd_handle(char c, char* line, uint8_t& len) {
  if (len != 0u) {
    if (line[0] == 'V' || line[0] == 'v') return false;   // 可能是回放行 ⇒ 交给它
    len = 0;                                             // 不可能是回放行 ⇒ 清掉
  }
#if defined(PANEL_GUARD_FAULT_INJECT) && (PANEL_GUARD_FAULT_INJECT == 1)
  // ★ 临时注入路径的"次数位"：`i1` / `i4` 里的那一位数字。
  //   在**默认构建里这一段不存在**（`i` 后面跟什么都不会被多吃掉一个字节）。
  if (g_inject_take_digit) {
    g_inject_take_digit = false;
    const uint8_t n = serial_cmd_inject_count(c, kInjectDefaultTimes);
    fault_inject_from_serial(n);
    return true;                   // 那一位数字也算"被这条命令吃掉了"
  }
#endif
  // ★ 判据是抽出去的纯函数（`lib/dashcore/serial_cmd.h`）：本文件这段与行缓冲/
  //   回放路径纠缠在一起，宿主机上编不到 ⇒ 把"哪几个字符算命令"单独放一处，
  //   由 native 用例逐字钉住（含"只认行首"这条）。
  switch (serial_cmd_classify(c)) {
    case SerialCmd::Diag:
      diag_key_press();
      return true;
    case SerialCmd::Mute:
      mute_toggle_from_serial();
      return true;
    case SerialCmd::Beep:
      beep_test_from_serial();
      return true;
    case SerialCmd::Reinit:
      reinit_from_serial();
      return true;
    case SerialCmd::Inject:
      // ★★ 临时故障注入（**默认构建里恒为 false**，见下面那一段）。
      //   ★ 开了注入时，这里只**记下**"下一位是次数"，真正的动作在下一颗字节
      //     （见函数开头那段）—— 这样 `i1` 与 `i` 都能用，而默认构建里
      //     `i` 后面那个字符**一个字节都不会被多吃掉**。
#if defined(PANEL_GUARD_FAULT_INJECT) && (PANEL_GUARD_FAULT_INJECT == 1)
      g_inject_take_digit = true;
      return true;
#else
      return false;                // 没开注入 ⇒ 这个字符照旧走回放路径（行为不变）
#endif
    case SerialCmd::Wire:
      // ★★ RF 测速/测丢包（2026-09-27 新增，另一单）—— 与 `i` **逐字同一个口径**：
      //   判据层只说"`w` 可能是这条命令"，**动作**由编译开关决定。
      //   没有无线 PHY 的构建（抓帧盒 / 真屏 / pcpreview）里恒 false ⇒
      //   这个字符照旧进回放路径 ⇒ 它们的串口行为**一个字节都没变**。
#if LINK_PHY_ESP_NOW
      // 只**开跑**（enqueue 一行 + 打一行）；真正的收发在主循环的 `meas_poll()` 里，
      // 一行都不在这里等 —— 串口路径上不许阻塞（与 `fault_inject_from_serial` 同一条纪律）。
      meas_start_from_serial();
      return true;
#else
      return false;
#endif
    case SerialCmd::None:
    default:
      return false;
  }
}
#endif  // DASH_DISPLAY_RGB

// ★★ 什么时候才认这一颗字节是命令（`line`/`len` = **这个口**的回放行缓冲）：
//   ① 缓冲是空的（最常见：命令就是单独一个字符）；**或者**
//   ② 缓冲里攒下的那几个字节**不可能**是回放行 —— 回放行的头三个字符必须是
//      `VAN`/`van`（`van_replay.cpp` 的 `parseVanReplayLine` 逐字要求，大小写都认）
//      ⇒ 首字节不是 V/v 的那一串**永远成不了一帧回放数据**，只可能是串口线上的
//      一颗杂散字节（上电/复位时 RX 悬空、或串口桥开合时的抖动）。
//      ★ 只判"缓冲空不空"**不够**：那一颗字节会把"行首"这个位置一直占着，于是
//        后面发的命令**全被判成回放数据**、一条都没反应 —— 直到有人发一个换行
//        把它冲掉为止（"按了没反应"是最难查的一类现象）。
//      ⇒ 与其让车主记住"先按一下回车"，不如在这里把这种时刻**认出来**：
//        清掉那串字节（它不可能是回放行，留着只会在下次换行时 echo 出一行
//        `VAN? …`），然后把这一颗字节当命令。
//        ★ 这条路径 2026-09-24 **上板验过**（不是纸上推的）：先发一颗 `x`
//          把那种杂散字节**造出来**，再发 `m` ⇒ `mute: 1 (saved)` 照常出来，
//          而且**没有** `VAN? x` 回显（缓冲确实被清掉了）。
//   ★ 首字节是 V/v 时**一律不动它**：那可能就是一行正在贴进来的回放帧，
//     命令字符（`d`/`m`）在 hex 里本来就可能出现（例 `VAN 824 18F8271D000000`）。
//   ★ 回放路径（`van_replay_feed`）**一个字都没动** —— 上面这套判据只决定
//     "这一颗字节要不要当命令吃掉"。
//   ★★ 2026-09-24 补：命令表本身（`d`/`m`/`b`/**`r`**）抽成了纯函数
//      `lib/dashcore/serial_cmd.h` 的 `serial_cmd_classify()` —— 上面这套"行首"
//      判据只决定**要不要问它**，而"哪几个字符算命令"那一层现在宿主机上可测
//      （见 `test/test_dashcore/test_serial_cmd.cpp` 里那一组）。
//      ★ 上面 `serial_cmd_handle()` 的实现整个在这道门里 ⇒ 这里的说明也留在门内。

static void van_replay_feed(const char c, char* line, uint8_t& len, uint32_t now) {
  if (c == '\r' || c == '\n') {
    if (len) {
      line[len] = '\0';
      VanPacket p{};
      if (parseVanReplayLine(line, &p, now)) {
        // ★★ 2026-09-27：这里原来调的是 `g_data.onVanPacket(p)` —— 它**绕过**
        //   `VanLogSink`，于是"回放进来的帧"既不计数、也不进原始帧转发队列
        //   （后果见 `van_frame_in()` 那段说明：回放是手上唯一能验证转发的手段）。
        //   现在与物理层那条**走同一个入口**。
        van_frame_in(p);
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
    const char c = (char)Serial.read();
#if defined(DASH_DISPLAY_RGB)
    // ★ 真屏构建才有这两个命令（`d` / `m`，见上面那一段）；其余构建里这两行
    //   根本不存在 ⇒ 抓帧盒的字节流一个字节都不会被吃掉。
    if (serial_cmd_handle(c, line_usb, len_usb)) continue;
#endif
    van_replay_feed(c, line_usb, len_usb, now);
  }
#if defined(ARDUINO) && defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  // 只有 CDC_ON_BOOT 时 Serial0 才是"另一个口";经典 ESP32 上两者是同一个 UART0
  // ★★ 2026-09-25：**UART0 归链路时这一整支不存在** —— 判据就是既有的
  //   `DASH_LOG_UART0`（dash_log.h 里"UART0 上有没有日志"的唯一出处，带链路 PHY
  //   的构建里它是 0）。为什么必须如此：`g_link_phy` 正把 UART0 当数据面用，
  //   而下面的 `Serial0.read()` 会把**链路帧的字节**当文本回放行吃掉 ⇒ 从板
  //   一边收帧一边把帧嚼碎（症状是 `crc_err`/噪声计数涨、`frames_ok` 不涨）。
  //   · 这一支今天只为"UART0 还是日志口"的那些构建存在（esp32s3 / esp32s3-rgb
  //     不带链路 PHY 的档、抓帧盒三个 env）—— 它们的字节流**一个字节都没变**；
  //   · 带链路 PHY 的构建里，**文本回放改走原生 USB-CDC**（上面那一支照旧收
  //     `Serial`）：插 12PIN 的 19/20 就能贴帧，见 docs/LINK-TWO-BOARD.md。
#if DASH_LOG_UART0
  static char line_uart[80];
  static uint8_t len_uart = 0;
  while (Serial0.available()) {
    const char c = (char)Serial0.read();
#if defined(DASH_DISPLAY_RGB)
    if (serial_cmd_handle(c, line_uart, len_uart)) continue;
#endif
    van_replay_feed(c, line_uart, len_uart, now);
  }
#endif  // DASH_LOG_UART0
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
// ★★ 主循环**停顿探测**（2026-09-25 新增）—— "下次再卡就有现场"的唯一办法
// ============================================================================
// ★ 起因（车主的现场，逐字记）：新板（从板镜像）插着 Type-C、没接 4Pin 的时候
//   **"现在会长鸣一会儿，画面也卡住了"**。而这块板上的蜂鸣器是**软开关**
//   （写 TCA9554 的 EXIO8，没有硬件定时，见 lib/dashcore/buzzer_exio.*）⇒
//   "一直响"只可能是**主循环停住了、没人去执行到点关掉**；画面冻结是同一件事的
//   另一半（渲染也在主循环里）。⇒ **这两个症状是同一次"主循环被堵死"**。
//
// ★ 为什么先要探测而不是直接猜原因：那一夜的症状是**偶发**的，而我们要的不是
//   "也许是这样"，是**一行数字**。所以这里把两件事记下来：
//     ① 每一圈花了多久（`millis()` 差值）—— 超阈值就打一行，带当时的一圈态：
//        链路收了多少字节（`LinkRxStats::bytes_read`）、一圈跑完花了多少微秒；
//     ② 一圈的**上界**（见 `link_poll_bounded_*`）—— "任何输入速率下都能回到渲染"。
//
// ★ 阈值：**200 ms**（任务书给的那个数）。为什么是这个量级而不是几十毫秒：
//   本构建上有两处**合法的**长圈，它们不是故障 ——
//     · 开机第一次整屏刷新（RGB 那条路上按块写、每块等一个消隐期）≈ **1 秒**；
//     · 每 30 ms 一次的整屏失效那一拍的重绘（实测 `整屏刷新 141.0ms`）。
//   把阈值压到几十毫秒就会把这两条**正常**路径打成噪音（噪音一多，真现场就被淹了）。
//   而"卡死"是**秒级到永久**的，200 ms 这条线抓得住它、又不会天天误报。
//   ★ 每 5 秒**一行摘要**（`loop: max=…`）是有意加的：不做摘要的话，
//     "到底有没有 200 ms 以上的圈"只能靠"没有 WARN 行"来推断，而那是**无法证明**的
//     （日志可能只是没打出来）。有摘要就是**正面读数**：`stall=0` 是有证据的 0。
//
// ★ 循环号那一格（`n=`）的用途：它是"这两行之间主循环真的转过多少圈"的**唯一**读数
//   —— 只有 WARN 行时，你分不出"卡了 1 次"还是"卡了 900 次每次都很短"。
static uint32_t g_loop_n = 0;          // 主循环跑了多少圈（探测用；每个摘要窗口从 0 数）
static uint32_t g_loop_max_ms = 0;     // 自上次摘要以来，最长的一圈
static uint32_t g_loop_stalls = 0;     // 自上次摘要以来，超过 kLoopStallMs 的圈数
static uint32_t g_loop_last_report_ms = 0;
static uint32_t g_loop_prev_ms = 0;    // 上一圈的时刻（探测要的 `millis()` 差值）
static const uint32_t kLoopStallMs        = 200u;    // 打一行的阈值（任务书给的数）
static const uint32_t kLoopStallReportMs  = 5000u;   // 摘要周期（与 SRC 那一行同一个节拍）

// 探测行里的 `prev=`：**这一次运行是"上一次怎么结束"之后的**，而上一次是
// 看门狗/PANIC 复位还是断电，跟"这一次会不会卡"直接相关 ⇒ 一行里带上。
// ★ 门与 `boot_note()` 同一道（`DASH_DEVICE_SELFTEST` = 真设备）：宿主机没有
//   `esp_reset_reason()`，pcpreview 编到这一行会直接报未声明。
#if defined(DASH_DEVICE_SELFTEST)
static const char* resetReasonShort() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "UNKNOWN";
  }
}
#else
static const char* resetReasonShort() { return "host"; }
#endif

// 探测行里 `probe_took=` 用的微秒时钟。
// ★ 为什么宿主机构建里没有它：pcpreview 的 Arduino 桩（`preview/arduino_shim`）
//   只提供 `millis()`，没有 `micros()` ⇒ 直接调会 `use of undeclared identifier`。
//   而"打印这一行自己花了多久"只有**真机**上有意义（宿主机上那一行也不会被打出来）
//   ⇒ 宿主机构建里报 0，一个符号都不引用。
#if defined(DASH_DEVICE_SELFTEST)
static uint32_t probe_us() { return (uint32_t)micros(); }
#else
static uint32_t probe_us() { return 0u; }
#endif

// ★★ 步骤②归因（2026-09-27）：主循环**每个阶段各花了多久**。
//
//   背景（为什么必须加这个才能动手）：实测 `loop: … max=83~100ms`，而测量里的
//   `wire_gap_max=93ms`（**发端信封里记的发出间隔最大值**）说明：`gap_max`(113ms) 与
//   p99(42ms) 这两项不达标，地板来自这些长圈，而不是空中。
//   可 `loop:` 那一行只报 `max=`，**没说卡在哪** —— 而纪律是"先加归因、不许盲改渲染路径"。
//
//   原理：`loop_stage(x)` 在**进入** x 时调用；它把"距上一次进入阶段的时长"记到
//   **上一个**阶段的账上（那一整段就是上个阶段的真实耗时）。
//   ⇒ 窗口结束时 `g_stage_worst` 就是"最慢的那个阶段"，直接回答"卡在哪"。
//   ★ 只在 `DASH_DEVICE_SELFTEST`（真机）下取值：宿主机没有 `micros()`（见 `probe_us()`）。
static const char* g_stage_name  = "start";   // 当前进入的阶段
static const char* g_stage_worst = "-";       // 本窗口最慢的阶段名
static uint32_t g_stage_mark_us  = 0;         // 上一次进入阶段的时刻
static uint32_t g_stage_worst_us = 0;         // 本窗口内"单阶段最长"的微秒数

static inline void loop_stage(const char* s) {
  const uint32_t t = probe_us();
  if (t != 0u) {
    const uint32_t d = t - g_stage_mark_us;
    g_stage_mark_us = t;
    if (d > g_stage_worst_us) {
      g_stage_worst_us = d;
      g_stage_worst = g_stage_name;
    }
  }
  g_stage_name = s;
}

// 每圈开头调一次：记时长、超阈值就**当场**打一行（带链路字节数 + 打印这一行花了几微秒）。
static void loop_probe_begin(uint32_t now) {
  if (g_loop_last_report_ms == 0u) {
    g_loop_last_report_ms = now;
    g_loop_prev_ms = now;
  }
  ++g_loop_n;
  const uint32_t dt = (uint32_t)(now - g_loop_prev_ms);

  if (dt >= kLoopStallMs) {
    ++g_loop_stalls;
    // `probe_took` = **打印这一行自己**花了多久。它是有意加的：探测本身要读
    // `millis()`/`micros()` 并格式化几十个字节，而它跑的正是"主循环已经卡了"的那一刻
    // ⇒ 得能看见"是不是我把日志打爆了"。真机上这个数是几十微秒。
    const uint32_t t0 = probe_us();
    const dashlink::LinkRxStats& rs = g_link_rx.stats();
    const uint32_t spent = probe_us() - t0;
    dash_logf("loop: stalled %lums (n=%lu link rx bytes=%lu crc=%lu bad_len=%lu noise=%lu) "
              "probe_took=%luus prev=%s\n",
              (unsigned long)dt, (unsigned long)g_loop_n,
              (unsigned long)rs.bytes_read, (unsigned long)rs.crc_err,
              (unsigned long)rs.bad_len, (unsigned long)rs.noise_bytes,
              (unsigned long)spent, resetReasonShort());
  }
  if (dt > g_loop_max_ms) g_loop_max_ms = dt;
  g_loop_prev_ms = now;

  // ★★ 步骤②归因的关键一步（2026-09-27）：把"上一圈最后一个阶段标记 → 本圈开始"
  //   这一段**也记账**，归到上一个阶段（正常就是 `log`）。
  //   ★ 为什么不能只重置：第一版就是只重置 → 实测出现
  //     `max=97ms` 而**每个阶段都只有几十微秒**的自相矛盾结果 —— 因为
  //     `dash_log_drain()`（整圈最后一步）+ 收尾那一段正好落在
  //     "log 标记 → 下一圈 probe_begin" 之间，被这条重置**丢掉了** ⇒
  //     盲区刚好盖住真凶。这一条补上之后 `stage=` 才可能指向它。
  if (g_stage_mark_us != 0u) {
    const uint32_t t = probe_us();
    const uint32_t d = t - g_stage_mark_us;
    if (d > g_stage_worst_us) {
      g_stage_worst_us = d;
      g_stage_worst = g_stage_name;   // 此时它应当是 "log"
    }
  }
  g_stage_mark_us = probe_us();   // ★ 步骤②归因：本圈从"这里"开始计时
  g_stage_name = "start";
}

// 每圈**末尾**调一次：到点打一行摘要。
// ★ 它是"没有 WARN 行"这句推断变成**正面读数**的那一半：`max=…` 是这一窗口里
//   最长的一圈，`stall=0` 是"一次 200 ms 都没超过"的实测值（而不是"没看见"）。
static void loop_probe_end(uint32_t now) {
  if ((uint32_t)(now - g_loop_last_report_ms) < kLoopStallReportMs) return;
  const uint32_t span = (uint32_t)(now - g_loop_last_report_ms);
  g_loop_last_report_ms = now;
  const dashlink::LinkRxStats& rs = g_link_rx.stats();
  // ★★ 日志那一路的读数（2026-09-26）：**丢了多少必须看得见**。
  //   `drop=`/`dropped=` 非 0 ⇒ "这一窗口里日志被丢过"（环满，因为没人读）；
  //   `blocked=` ⇒ "有多少圈在排空时撞上'端口满'、一个字节都没写出去"。
  //   ★ 这两个数**不是错误**，是设计取舍的读数：宁可丢日志，也不能堵主循环。
  //   ★ `drain=` 是累计交付字节数（差值 = 这一窗口真的送出去多少）。
  const dashlog::Stats ls = dash_log_stats();
#if LINK_PHY_ESP_NOW
  // ★ 选项(a) 可行性探针的读数：`txprobe=` 是"高优先级 20ms 定时器回调"在本窗口里
  //   两次之间的**最大间隔**。与同一行的 `max=`（主循环最长的一圈）并排读：
  //     max=95ms 而 txprobe≈20ms ⇒ 高优先级任务躲得开 ⇒ (a) 可行
  //     max=95ms 且 txprobe≈95ms ⇒ 抢占是全局的（关中断/关 cache）⇒ (a) 无效
  const uint32_t probe_max_us = g_probe_max_us;
  g_probe_max_us = 0;
#else
  const uint32_t probe_max_us = 0;
#endif
  // ★ 步骤②归因：`stage=<最慢阶段耗时>us@<阶段名>` —— "这一窗口最长的一圈卡在哪"。
  //   与 `max=` 配套读：`max=95ms` 而 `stage=94000us@render` ⇒ 卡在渲染块里。
  dash_logf("loop: n=%lu in %lums (%lu/s) max=%lums stall=%lu | link rx bytes=%lu frames=%lu | "
            "stage=%luus@%s | txprobe=%luus | "
            "log drain=%lu drop=%lu dropped=%lu blocked=%lu ring=%lu/%lu hwm=%lu\n",
            (unsigned long)g_loop_n, (unsigned long)span,
            (unsigned long)(span ? (g_loop_n * 1000u / span) : 0u),
            (unsigned long)g_loop_max_ms, (unsigned long)g_loop_stalls,
            (unsigned long)rs.bytes_read, (unsigned long)rs.frames_ok,
            (unsigned long)(g_stage_worst_us / 1000u), g_stage_worst,
            (unsigned long)probe_max_us,
            (unsigned long)ls.drained_bytes, (unsigned long)ls.drop_count,
            (unsigned long)ls.dropped_bytes, (unsigned long)ls.blocked_drains,
            (unsigned long)ls.ring_bytes, (unsigned long)ls.ring_capacity,
            (unsigned long)ls.high_water_mark);
  g_loop_n = 0;
  g_loop_max_ms = 0;
  g_loop_stalls = 0;
  g_stage_worst_us = 0;     // ★ 窗口复位（与 max/stall 同步）
  g_stage_worst = "-";
}

// ============================================================================
// ★★ 链路收帧的**每圈上界**（2026-09-25 新增）
// ============================================================================
// ★ 为什么要"再包一层"：`LinkRx::poll()` 自己已经有单次预算（默认 64 B/次），
//   但主循环用的是 `while (g_link_rx.poll(...)) { … }` —— **每一圈可以调它很多次**，
//   于是"一圈总共能吃多少字节"这件事**根本没有上界**。
//   悬空 RX 脚（Type-C 插着时 4Pin 那一路被 FSUSB42 断开，见 ARCHITECTURE §8 L1）收到的
//   伪字节流会让这个 while 一遍遍地跑；一圈里吃掉的字节数只由**线路上的字节速率**决定，
//   不由我们决定 ⇒ 极端情况下主循环回不到"渲染那一行"。
//
// ★ 判据（`LinkRxStats::bytes_read` 的差值）：它是"真的从 PHY 读进来"的累计值，
//   与"解出了几帧""丢了多少噪声"都无关 —— 悬空脚上最典型的形态正是
//   **一直在读、什么都没解出来**（`noise_bytes` 甚至可能不怎么涨：字节里只要夹着一个
//   SYNC，后面那串就不算 noise）。
//
// ★ 上界取多少：`kLinkRxBytesPerLoop = 512`。
//   · 115200 8N1 的线速 = 11520 B/s ⇒ 512 B ≈ **44 ms 的线上时间**；而解帧那条路
//     实测 ~0.43 µs/B（`loop: stalled` 行里的 `probe_took` 同一量级）⇒ CPU 上是零点几毫秒。
//   · 剩下的字节**不丢**：它们还在 PHY 的环/驱动的 FIFO 里，下一圈接着读
//     （`LinkRx::poll` 的早退语义就是"剩下的下一圈再来"，见 link_rx.h ①）。
//   · 从板每圈要收的是主板那 ≈80 Hz 的 TICK/DATA（十几个字节/圈）⇒ 512 B 这一档
//     对**正常流量完全不构成限制**，它只在"线路出问题"的时候生效。
//   · ★ 它**不改**收发协议、不改 §5 的角色、也不改 §1.2 的任何一条：只是把
//     "一圈"这个粒度上的工作量封了顶。
static const uint32_t kLinkRxBytesPerLoop = 512u;

// 累计读进字节数（判据只有一个：`LinkRxStats::bytes_read`）。
static uint32_t link_rx_bytes_now() { return g_link_rx.stats().bytes_read; }

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
  while (g_link_rx.poll(g_link_phy, &f)) {
#if LINK_PHY_ESP_NOW
    // ★★ 测量信封的**最后一道闸门**（2026-09-27 上板实测后加，这是当晚最后一个泄漏源）。
    //
    //   不管这一帧是从哪个读者出来的（`meas_poll()` 按帧分流那条路，还是本函数自己
    //   `g_link_rx.poll(g_link_phy, …)` 读 PHY 那条路），**只要它是测量帧就只统计、
    //   绝不进数据层**。
    //   ★ 为什么需要两道闸门：`meas_poll()` 是第一道，但它一圈有字节预算（1536 B），
    //     而 WiFi 任务会在两次调用之间继续往 PHY 环里填新包 ⇒ 本函数这个"第二个读者"
    //     仍可能读到测量信封（实测：把预算调到大于环容量**挡不住**，因为环会被重新填上）。
    //   ★ 为什么证据是硬的（不是猜的）：信封载荷 = `D` `S` `M` `1` + ver + seq + ms，
    //     按 DataMsg 布局（rpm u16 BE / speed u8 / coolant u8 / intake u8 / flags u8）解出来
    //       coolant_raw = 载荷[3] = `'1'` = 0x31 = 49  ⇒ 49 − 40 = **9.0 ℃**
    //       intake_raw  = 载荷[4] = ver = 1            ⇒  1 − 40 = **−39.0 ℃**
    //     —— 与屏上那两个**恒定不变**的"不可能读数"逐字节吻合（恒定正是因为
    //     magic 与版本字节恒定）。所以不用再猜是哪个字段串了。
    if (f.type == (uint8_t)dashlink::MsgType::Data &&
        g_meas_rx.noteArrival(f.payload, f.len, now)) {
      continue;   // 测量帧：只统计，不进数据层
    }
#endif
    if (dashlink::handleInbound(f, &g_link_time, now, &ld)) {
      // TICK / DATA：handleInbound 已经把 TICK 喂了时基、把 DATA 解成了 LinkData。
      if (f.type == (uint8_t)dashlink::MsgType::Data) {
        g_data.applyLinkData(ld);   // §3：从板把收到的 DATA 喂进自己的数据层
        got_data = true;
      }
      continue;
    }
    // ★★ VANRAW（`0x21`，2026-09-27）：主板把**原始 VAN 帧**搬过来了 ⇒ 从板用
    //   **自己**的 `VanSource` 解一遍，于是"只有 VAN 才有"的那些字段（灯位/门/VIN）
    //   在从板上也有了，而它**不用再接一套收发器**。
    //   ★ 走 `van_frame_in()`（与主板物理层、串口回放**同一个入口**）⇒
    //     数帧、3 秒新鲜度、以及各字段自己的 age 判据全部沿用既有那一套，
    //     本单在数据层**一行都没改**（见 data_service.h 的合并顺序）。
    //   ★ 解不出来的载荷（长度对不上）**只计数、不喂**：半个 VAN 帧喂进 VanSource
    //     会解出错误的转速/车速，比丢帧糟得多（理由同 link_msg.cpp 的 unpackVanRaw）。
    if (f.type == (uint8_t)dashlink::MsgType::VanRaw) {
      dashlink::VanRawMsg vm;
      if (dashlink::unpackVanRaw(f.payload, f.len, &vm)) {
        van_frame_in(dashlink::unpackVanRawToVanPacket(vm, now));
        ++g_vanraw_ok;
        g_vanraw_last_ms = now;
      } else {
        ++g_vanraw_bad;
      }
      continue;
    }
    // HELLO / STATUS / EVENT：v1 实际只用 B→A（§3），A→B 收到只说明"对端在说话"，
    // 不参与数据面 —— 从板这一侧本轮不打日志（它自己的 USB-C 上要看的是数据层那几行）。
  }
  g_link_time.update(now);   // §4：推进年龄与三级超时（100 ms/500 ms/3 s）
  return got_data;
}

// ★★ 从板侧的**有界**收帧（2026-09-25）：与 `link_poll_frames_slave()` **同一个形状**，
//   只多一件事 —— 一整圈吃掉的字节数封顶（理由与取值见 `kLinkRxBytesPerLoop` 那一段）。
//   ★ 判据是 `bytes_read` 的**差值**，不是"解出了几帧"：悬空脚上最典型的形态正是
//     "一直在读、一帧都没解出来"，那种时候帧计数一个都不动，只有这个差值在涨。
//   ★ 超预算时**不是丢字节**：剩下的还在 PHY 里，下一圈接着读（`LinkRx::poll` 的早退语义）。
static bool link_poll_bounded_slave(uint32_t now) {
  bool got_data = false;
  dashlink::Frame f;
  ::LinkData ld;
  const uint32_t bytes0 = link_rx_bytes_now();
  for (;;) {
    // ★ 预算判据放在**每一趟循环的入口**（不是只在"解出帧"那一支）：`poll()` 返回
    //   false 的那条路同样在吃字节（噪声 / 半截帧 / CRC 不过），而"一直返回 false、
    //   一直有字节"正是悬空 RX 脚上的形态 ⇒ 这里才是必须封顶的地方。
    if ((uint32_t)(link_rx_bytes_now() - bytes0) >= kLinkRxBytesPerLoop) break;
    if (!g_link_rx.poll(g_link_phy, &f)) {
      // ★★ **这一条是本单用探测程序实测出来的**（差点写成 `break`，那是个真 bug）：
      //   `poll()` 返回 false 有**两种**完全不同的意思 ——
      //     ① "PHY 现在没有字节了"（`phy.read() < 0` ⇒ 这一圈真的收完了）；
      //     ② "**这一次调用**的预算用完了"（默认 64 B ⇒ 我一次只读这么多）。
      //   把 ② 当成 ① 就地退出，就等于"每圈最多读 64 字节" —— 那会在**连续字节流**
      //   （悬空脚上的伪字节流、或对端背靠背发）里把主循环的收帧速率压到 64 B/圈，
      //   而主循环是 ~900 圈/秒 ⇒ 约 57 KB/s 的上限看着够用，**但它同时把
      //   `rxLeft` 变成了一个只增不减的积压**：一旦线路速率超过"每圈 64 B"，
      //   环里就永远排不干净，`LinkTime` 的 tick 年龄跟着一直涨（"链路看着像断了"）。
      //   ⇒ 正确的判据是**分清这两种 false**：
      //     · 缓冲里还有没解完的字节（`pending()`）⇒ 继续（还有活干）；
      //     · 环里还有没取走的字节（`available()`）⇒ 继续（还有货可吃）；
      //     · 都不是 ⇒ 这一圈真的空了，退出。
      //   （上限仍然由上面那一行的 `kLinkRxBytesPerLoop` 兜着 ⇒ 不会变成死循环。）
      if (g_link_rx.pending() || g_link_phy.available() > 0) continue;
      break;
    }
#if LINK_PHY_ESP_NOW
    // ★★ 测量信封的**最后一道闸门**（2026-09-27 上板实测后加，这是当晚最后一个泄漏源）。
    //
    //   不管这一帧是从哪个读者出来的（`meas_poll()` 按帧分流那条路，还是本函数自己
    //   `g_link_rx.poll(g_link_phy, …)` 读 PHY 那条路），**只要它是测量帧就只统计、
    //   绝不进数据层**。
    //   ★ 为什么需要两道闸门：`meas_poll()` 是第一道，但它一圈有字节预算（1536 B），
    //     而 WiFi 任务会在两次调用之间继续往 PHY 环里填新包 ⇒ 本函数这个"第二个读者"
    //     仍可能读到测量信封（实测：把预算调到大于环容量**挡不住**，因为环会被重新填上）。
    //   ★ 为什么证据是硬的（不是猜的）：信封载荷 = `D` `S` `M` `1` + ver + seq + ms，
    //     按 DataMsg 布局（rpm u16 BE / speed u8 / coolant u8 / intake u8 / flags u8）解出来
    //       coolant_raw = 载荷[3] = `'1'` = 0x31 = 49  ⇒ 49 − 40 = **9.0 ℃**
    //       intake_raw  = 载荷[4] = ver = 1            ⇒  1 − 40 = **−39.0 ℃**
    //     —— 与屏上那两个**恒定不变**的"不可能读数"逐字节吻合（恒定正是因为
    //     magic 与版本字节恒定）。所以不用再猜是哪个字段串了。
    if (f.type == (uint8_t)dashlink::MsgType::Data &&
        g_meas_rx.noteArrival(f.payload, f.len, now)) {
      continue;   // 测量帧：只统计，不进数据层
    }
#endif
    if (dashlink::handleInbound(f, &g_link_time, now, &ld)) {
      if (f.type == (uint8_t)dashlink::MsgType::Data) {
        g_data.applyLinkData(ld);
        got_data = true;
      }
      continue;
    }
    // ★★ VANRAW（`0x21`）：与上面 `link_poll_frames_slave()` 里那一段**逐字同形**
    //   （两个收帧函数刻意保持同一形状，见它们头顶的说明）。理由与判据见那一段。
    if (f.type == (uint8_t)dashlink::MsgType::VanRaw) {
      dashlink::VanRawMsg vm;
      if (dashlink::unpackVanRaw(f.payload, f.len, &vm)) {
        van_frame_in(dashlink::unpackVanRawToVanPacket(vm, now));
        ++g_vanraw_ok;
        g_vanraw_last_ms = now;
      } else {
        ++g_vanraw_bad;
      }
      continue;
    }
    // HELLO / STATUS / EVENT：v1 从板这一侧 **A→B 的这三类只用来对账**
    // （TICK/DATA 已经走上面那一支进了时基与数据层）。
    // ★ 2026-09-27：HELLO 多了一件**必须做的事** —— 收到对端的 HELLO 就要
    //   `g_link_hello_acked = true`，否则从板会**永远**每 5 s 重复发 HELLO
    //   （§3 那句"直到收到对端 HELLO"就是这条状态的唯一判据）。
    //   与主板侧那一行（`link_poll_inbound()` 里）逐字同形 —— 两边的状态是**同一份**
    //   全局变量（见它定义处那段说明）。
    if (f.type == (uint8_t)dashlink::MsgType::Hello) g_link_hello_acked = true;
    // ★ 其余两类仍然不打日志：从板自己的 USB-C 上要看的是数据层那几行
    //   （§3 的"单一日志出口"是**从板→主板**那一向，不是反过来）。
  }
  g_link_time.update(now);
  return got_data;
}

// ============================================================================
//  链路（从板侧）：主循环里的"发 HELLO + 发 STATUS + 排水"（★ 2026-09-27 新增）
// ============================================================================
// 这一段补的是契约里那个**从板也会发**的缺口（§3 表：`0x01` HELLO 是**双向**、
// `0x30` STATUS 的方向写的就是 **B → A**）。补之前从板那一支**一行 enqueue 都没有**
// ⇒ v1 的"双向"只兑现了 A→B 那一半，而主板那 30 s 的 `从板无响应` 判据（§8 L13）
// 恒为真、`link: 首次收到从板帧(…)` 那一行永远等不到。
//
// ★★ 发送预算（**两个发送方都在 115200 上说话**，所以这笔账要在这里算清）
//   一字节 8N1 = 10 bit ÷ 115200 = 86.8 µs（§1.1 那张表第一列）。
//     从板这一侧新增的两个消息（帧长 = 7 + 载荷，§2）：
//       · HELLO  0x01 ：载荷 5 B ⇒ 帧 **12 B** ⇒ 1.04 ms/帧
//                       节奏 = 上电 1 次 + 之后每 5 s ⇒ **0.2 帧/s ⇒ 0.21 ms/s**
//                       （而且收到对端 HELLO 就**永久停发** ⇒ 这是上界，不是稳态）
//       · STATUS 0x30 ：载荷 16 B ⇒ 帧 **23 B** ⇒ 2.00 ms/帧
//                       节奏 = 2 Hz（§3 表那一行的 500 ms）⇒ **2 帧/s ⇒ 4.00 ms/s**
//     ⇒ 从板合计 **≤ 4.21 ms/s ≈ 0.42% 线时**。
//   主板那一侧（§1.1 的同一张表）：`DATA` ≈79.7 Hz × 1.13 ms/s + `TICK` 50 Hz ×
//   1.04 ms/s ⇒ 约 **142 ms/s ≈ 14%**（实测那一条 12 ms 限速把 DATA 压回 80 Hz，
//   见 `kLinkDataMinIntervalMs` 那段）。
//     ⇒ **A→B + B→A 合计 ≈ 14.4%**，与契约 §1.1 原话"≈15%（A→B ≈14%，B→A ≈0.4%）"
//       逐项吻合 —— 也就是说**契约当初就是按"从板 2 Hz STATUS"算的那笔账**，
//       本轮补上的正是那个一直被算进去、却一直没有实现的那 0.4%。
//   ★ 结论：**不需要动波特率、不需要降频、不需要改 `platformio.ini`**。
//
// ★★ RX 优先（从板**不会**因为发送而挤掉接收）—— 这一条靠两处**顺序**保证：
//     ① `loop()` 里 `link_poll_bounded_slave(now)` 在 `link_slave_tick()` **之前**
//        （收是 latency-sensitive 的：TICK 20 ms 一格、三级超时 100 ms 起算）；
//     ② 一个**渲染周期**里也只做一次收发：`link_poll_bounded_slave()` 在 `loop()`
//        开头，而 `link_slave_tick()` 在"上一帧渲染已过 200 ms"那一支里（与
//        `make_view()` 同一支，见下面那段说明）⇒ 每 200 ms 最多排一帧 STATUS。
//   而且本函数**只往环里写**（`LinkTx::enqueue` 是纯内存、无阻塞），真的碰 UART 的
//   只有 `pump()` 那两行，两次都**只走能走的字节**、`availableForWrite()` 报 0 就
//   一个字节都不写（§1.2 ②③）。⇒ 发送最多让主循环多花"一次环拷贝",
//   而收帧的预算（`kLinkRxBytesPerLoop`）在它**之前**就已经花掉了。
//   ★ 还有一层：环满时 `enqueueFrame` **整帧丢并计数**（不会把半帧写进去，
//     也不会重试等待）⇒ 最坏情况是"少一条 STATUS"，不是"收帧被拖住"。
//
// ★ 顺序上的第三件事（**不要改**）：enqueue 之后才 pump。先 pump 再 enqueue 会让
//   这一拍刚排进去的帧白等一整圈（主循环 ~900 圈/秒 ⇒ 浪费 1.04~2.00 ms 的发送窗口）。
//
// ★ 为什么必须收 `ArcDashView`（而不是在 loop() 里另算一个 `face_update()`）：
//   `STATUS.left_face` 要报的是"**这一拍上屏的那个档位**"（§3 表 `0x30` 行）。
//   再调一次 `face_update()` 会**推进第二条状态机**（它有超速迟滞记忆）⇒ 报出去的值
//   与屏上的值在边界上会不一致，而且两次调用互相搅动对方的记忆。
//   ⇒ 只认 `make_view()` 那一次的结果，它在 loop() 里每 200 ms 算一次（与渲染同拍）。
//
// ★ 本函数**不碰** `LINK_ROLE`：它是 `#if LINK_ROLE != 1` 那一支里的代码
//   （§5：谁发什么由编译期角色定，运行期没有任何判据）。
static void link_slave_tick(uint32_t now, const ArcDashView& view) {
  // ① HELLO：上电 1 次，之后每 5 s，直到收到对端 HELLO（§3）。
  //    判据与主板那一支是**同一个函数**（`dashlink::helloDue`）——
  //    不是"抄了一遍"，所以两边不可能漂。
  if (dashlink::helloDue(now, g_link_hello_ms, g_link_hello_acked)) {
    g_link_hello_ms = now;
    dashlink::HelloMsg hm;
    hm.fw_ver      = kLinkFwVer;      // 与主板同一对常量（见定义处那段）
    hm.build_tag   = kLinkBuildTag;
    hm.boot_reason = 0;               // §3：取值由发送侧定，仓库里没有既有编码
    uint8_t payload[dashlink::kHelloLen];
    if (dashlink::packHello(hm, payload)) {
      g_link_tx.enqueueFrame((uint8_t)dashlink::MsgType::Hello, payload, dashlink::kHelloLen,
                             dashlink::kLocalRole);
    }
  }

  // ② STATUS：2 Hz（§3 表 `0x30` 行的 500 ms）。字段逐个照契约填，**一个不多一个不少**：
  //      fw_ver         = 与 HELLO 同一对常量（§3：与协议 VER 分开）
  //      uptime_ms      = 本机单调毫秒（§7 失败模式 7 靠它发现"从板在反复重启"）
  //      frames_ok      = LinkRx 收下并交给上层的帧
  //      frames_dropped = §2 那三种（crc / bad_len / unknown_type）+ 角色冲突等
  //                       —— 直接用 `LinkRxStats::framesDropped()`（与 §3 的语义同源）
  //      crc_err        = §2 的 CRC 不过
  //      last_gap_ms    = **保留、v1 一律 0**（§3 那条定案；`StatusSender` 刻意不碰它）
  //      left_face      = 左屏当前档位（`expression.h` 的 Face 槽位下标）
  //      flags          = `slaveStatusFlags()`：bit0/bit1/bit2 有生产者，bit3 恒 0
  //    ★ 计数**截到 u16** 是契约的宽度（§3 表：三个都是 u16）—— 累计量在
  //      `LinkRxStats` 里是 u32，这里按契约窄化；截断由主板那行日志的单调性可见。
  dashlink::StatusMsg sm;
  if (g_link_status.due(now, &sm)) {
    const dashlink::LinkRxStats& rs = g_link_rx.stats();
    sm.fw_ver         = kLinkFwVer;
    sm.frames_ok      = (uint16_t)rs.frames_ok;
    sm.frames_dropped = (uint16_t)rs.framesDropped();
    sm.crc_err        = (uint16_t)rs.crc_err;
    // last_gap_ms 不写：见上面那一行（保持 StatusMsg 的默认值 0）
    sm.left_face      = (uint8_t)view.face_left;
    sm.flags          = dashlink::slaveStatusFlags(g_link_rx.verMismatchSeen(),
                                                   g_link_rx.roleConflictSeen(),
                                                   g_link_time.dataState());
    uint8_t payload[dashlink::kStatusLen];
    if (dashlink::packStatus(sm, payload)) {
      g_link_tx.enqueueFrame((uint8_t)dashlink::MsgType::Status, payload, dashlink::kStatusLen,
                             dashlink::kLocalRole);
    }
  }

  // ③ 排水：`LinkTx` 的环 → PHY 的环 → UART 的 FIFO。与主板那一支**同一形状**。
  //   ★ 传 `now` 进去：ESP-NOW 那一档要用它打"**第一次发送失败发生在第几毫秒**"
  //     以及周期性计数器（上板实测"跑几秒后停住"那一单补的可观测性）。
  //     UART / 空壳那两档的 `pumpTx()` 参数有默认值 ⇒ 一个字都不受影响。
  // ★★ 选项 (a)：PHY 的 TX 环现在有**两个写者**（TICK 任务 + 这里）⇒ 用互斥量护住这一小段。
  //   **0 超时的 try-take**：拿不到锁就这一圈不排水（帧留在环里，下一圈再走）
  //   ⇒ 主循环**永远不会**被这个锁阻塞（与"日志不许阻塞主循环"同一条纪律）。
  //   拿到锁之后 `esp_now_send()` 只把包拷进驱动待发队列就返回 ⇒ 持锁时间很短。
  if (phy_tx_lock(0u)) {
    g_link_tx.pump(g_link_phy);
    g_link_phy.pumpTx(now);
    phy_tx_unlock();
  }
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
      // ★★ 2026-09-27：这一行是"**从板真的活了**"在主板上的**唯一**可见证据
      //   （§3 的"单一日志出口"就是它）。为什么本单要专门说它：
      //     · 从板那一侧此前**从来不发** STATUS（`LinkTx` 的环恒空）⇒ v1 的
      //       "双向"只兑现了 A→B，而这个 `switch` 里 STATUS/EVENT 两支
      //       **一次都没被执行过** —— 现场看到的就是"线接好了、两板都在跑、
      //       主板上那些 `link: B …` 一行都没有"（本单的现场卡点）。
      //     · 加上它之后，只插**主板**一个口就能同时看到 A 与 B 的关键状态：
      //       `uptime` 单调涨 = 从板活着且没在反复重启（§7 失败模式 7：
      //       5V 带不动 ⇒ 这个数会反复归零）；`rx_ok` 涨 = 它在收我的 TICK/DATA；
      //       `crc`/`dropped` = 链路质量（§7 失败模式 8）。
      //   ★ 格式**一个字都没改**（`link: B uptime=…`）：它已经在
      //     `docs/LINK-TWO-BOARD.md` §4.1 的判据表与 §3 的"单一日志出口"
      //     示例里逐字写着了（那份示例的**建议**就是这一行）——
      //     本单补的是"它终于会出现了"，不是"换一个更好听的前缀"。
      //   ★ 频率提醒：**2 Hz**（§3 表 `0x30` 行）⇒ 每 500 ms 一条。
      //     刻意**不再降频**（与 `206 dash ok` 那两行不同）：它是"从板在线"的心跳，
      //     与 `link: tx=… B 在线` 那个 1 Hz 的门限（§8 L13 的 30 s）互补 ——
      //     降频会让"从板刚开始不上报"与"从板掉线"看起来一样。
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
// ★★ 2026-09-25：循环里加了**每圈字节上界**（`kLinkRxBytesPerLoop`，理由与判据见那一段）。
//   形态与从板侧 `link_poll_bounded_slave()` **同一个形状**：判据在每一趟循环的入口，
//   超了就地退出 —— 剩下的字节留在 PHY 里，下一圈接着来。
static void link_poll_inbound(uint32_t now) {
  static bool announced_peer = false;
  static bool announced_conflict = false;
  static bool announced_ver = false;

  const uint32_t bytes0 = link_rx_bytes_now();
  dashlink::Frame f;
  for (;;) {
    if ((uint32_t)(link_rx_bytes_now() - bytes0) >= kLinkRxBytesPerLoop) break;
    if (!g_link_rx.poll(g_link_phy, &f)) {
      // ★ 与从板那一侧**逐字同一条**：`poll()` 的 false 分两种（"真的空了" vs
      //   "这一次调用的预算用完了"），只有"缓冲也空、环也空"才是这一圈收完了。
      //   理由与那次实测（差点写成 `break`）见 `link_poll_bounded_slave()` 里那段。
      if (g_link_rx.pending() || g_link_phy.available() > 0) continue;
      break;
    }
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
  // ① TICK：★★ 2026-09-27 深夜**已搬到 `link_tick_task`**（选项 (a)，见那个任务的说明）。
  //   原来这里是"每圈判 `g_link_tick.due()` 再入 `LinkTx`"，而排水也在主循环 ⇒
  //   主循环被抢占 ~95ms 时，TICK 就跟着停 ~95ms（实测 `wire_gap_max` = 92~99ms，
  //   它又是收端 `gap_max` 与 p99 达不到契约的下界）。
  //   现在 TICK 由**高优先级任务**以固定 20ms 节拍**直接写进 PHY 的环**。
  //   ★★ 注意：`g_link_tick` 从此**只被那个任务碰** —— 这里**不要**再调 `due()`，
  //      否则两个上下文会同时推进同一个生成器（那正是一处竞态）。

  // ② HELLO：上电 1 次，之后每 5 s 重发，**直到收到对端 HELLO**（§3）。
  //    ★ 2026-09-27：判据搬进 `dashlink::helloDue()`（`lib/link/link_app.h`）——
  //      从板那一支要用**逐字同一条**，抄一遍就一定会漂。
  if (dashlink::helloDue(now, g_link_hello_ms, g_link_hello_acked)) {
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

  // ③' VANRAW：把队列里积压的**原始 VAN 帧**搬进 `LinkTx`（2026-09-27 新增）。
  //    为什么放在 DATA 之后、排水之前：与 DATA 同一个生产者侧、同一个排水点，
  //    顺序本身不影响正确性（两种消息各自独立、都有 CRC），只是让"这一圈要发的东西"
  //    在同一个地方看得全。
  //    ★ 一圈封顶 `kVanRawFramesPerLoop` 帧（§1.3：单次很短、可随时被打断）——
  //      剩下的下一圈再走。**圈数**远多于帧数（主循环 ≈9k 圈/秒、VAN ≈80~100 帧/秒），
  //      所以常态下这个队列是空的；它存在只是为了吸收"主循环被抢占"的那几帧。
  //    ★ 满了会**丢整帧并计数**（`VanRawQueue::dropped()` / `tooLong()`），
  //      计数在下面那条 `vanraw:` 日志行里可见 —— 绝不写半帧、绝不等待。
  {
    uint8_t rp[dashlink::kLenMax];
    for (uint8_t i = 0; i < kVanRawFramesPerLoop; ++i) {
      const uint8_t n = g_van_raw.pop(rp, (uint8_t)sizeof(rp));
      if (n == 0u) break;
      if (!g_link_tx.enqueueFrame((uint8_t)dashlink::MsgType::VanRaw, rp, n,
                                  dashlink::kLocalRole)) {
        break;   // LinkTx 满了：这一帧已由 enqueueFrame 计数丢掉，剩下的下一圈再来
      }
    }
  }

  // ④ 排水：`LinkTx` 的两级环 → PHY 的环 → UART 的 FIFO。两步都**只走能走的那些字节**。
  //   ★ 同样把 `now` 传进去（理由见从板那一支同一处的说明）。
  // ★★ 选项 (a)：PHY 的 TX 环现在有**两个写者**（TICK 任务 + 这里）⇒ 用互斥量护住这一小段。
  //   **0 超时的 try-take**：拿不到锁就这一圈不排水（帧留在环里，下一圈再走）
  //   ⇒ 主循环**永远不会**被这个锁阻塞（与"日志不许阻塞主循环"同一条纪律）。
  //   拿到锁之后 `esp_now_send()` 只把包拷进驱动待发队列就返回 ⇒ 持锁时间很短。
  if (phy_tx_lock(0u)) {
    g_link_tx.pump(g_link_phy);
    g_link_phy.pumpTx(now);
    phy_tx_unlock();
  }
}
#endif  // LINK_ROLE == 1

#if LINK_PHY_ESP_NOW
// ============================================================================
//  ★★ 无线那一档的**测速/测丢包**（2026-09-27 新增，另一单）
// ============================================================================
// 用途：下一单要在**真机、真距离（约 20 cm）**上回答"这条无线链路能不能用"。
//   判据（丢包 < 0.1 % / 连续最大间隔 < 100 ms / p99 抖动 < 20 ms）与理由写在
//   `lib/link/link_meas.h` 的文件头；**下一单 10 分钟怎么跑**写在
//   `docs/LINK-TWO-BOARD.md` 的「无线档」一节。本函数只负责"接上主循环"。
//
// ★ 为什么要"先按帧长把测量帧摘出来、再把剩下的字节喂给 `LinkRx`"：
//   测量帧本身**也是**一帧合法的 v1 DATA 帧（协议一个字没动，见 link_meas.h），
//   所以它躺在 PHY 的环里时与普通数据帧**长得一样**。如果直接让 `LinkRx` 去吃，
//   它会把它当 DATA 解出来并喂进数据层（那是**测量流量**，不该污染车辆数据）。
//   ⇒ 这里按"帧头 + LEN"（`LinkRx` 同样的一条判据）把整帧取出来看一眼：
//     是测量信封 ⇒ 交给 `MeasReceiver`（只统计，不进数据层）；不是 ⇒ 交给 `LinkRx`。
//   ★ 顺序是**硬的**：本函数必须在 `link_poll_*()` **之前**跑（否则 `LinkRx` 会先
//     把测量帧吃进数据层）；而且它**读空** PHY 之后，紧随其后的
//     `link_poll_*()` 看到的是"有新字节再来"这一条正常路径。
//
// ★ 有界与不阻塞（与 `kLinkRxBytesPerLoop` 同一条口径）：
//   · 一圈最多从 PHY 取走这么多字节 —— **这就是上界**（帧数由它天然封顶：最小帧
//     11 B ⇒ 最多约 140 帧）。
//   · ★★ 2026-09-27 上板实测：这个数**必须大于 PHY 接收环的容量**，否则无线档下
//     会出现**两个读者**：`meas_poll()` 一圈只取走这么多，剩下的留在环里，
//     紧接着 `link_poll_bounded_slave()` / `link_poll_inbound()` 里的
//     `g_link_rx.poll(g_link_phy, …)` 会把它按**字节流**吃掉 —— 于是
//      ① 被切开的测量帧不再算测量（白算一次丢包）；
//      ② 它经 `LinkRx` 进了数据层（实测从板日志里还有 7 行 `coolant=9.0C`）。
//     ⇒ 取 `1536 > 1024`（环容量，见 `LINK_ESPNOW_RX_RING`）：
//       **每圈必然把环读空** ⇒ 无线档下事实上的唯一读者就是 `meas_poll()`，
//       上面那条泄漏路径被结构性堵死（不靠"谁先跑"这种约定）。
//   · 发送侧一拍最多 `MeasSender::kMaxPerPoll` 帧，且只往 PHY 的环里写（不碰射频）；
//   · **不格式化、不打印**任何东西（真发/真收都在 PHY 与回调里，见它们的文件头）。
static const uint16_t kMeasBytesPerLoop = 1536u;

// ★ `g_meas_tx` / `g_meas_rx` 的**声明在文件上方**（链路实例那一段之后）——
//   两处收帧函数里的"测量信封闸门"要用到 `g_meas_rx`，C++ 要求先声明后使用。

// 串口 `w`：开一次测量流（只在无线 PHY 的构建里真的有动作；其余构建里这个字符
// 照旧是一个普通字符 ⇒ 串口行为逐字节不变，见 `lib/dashcore/serial_cmd.h`）。
// ★ 那一行日志刻意写成**纯 ASCII**（`--` 而不是破折号）：下一单要在串口上贴这一行
//   回来对账，而"抄一行"这件事在中文标点上最容易出岔（同一仓库已有先例注释）。
// ★★ 测量帧的**到达钩子**（2026-09-27 深夜，本单第二步）：在**射频回调里**
//   （WiFi 任务上下文）给测量帧打时间戳。
//   为什么必须在这一刻打：若在主循环里打（收帧闸门那条路），量到的"到达间隔"会混进
//   **收端主循环被抢占**（实测 `gap_max` 113ms / p99 42ms）—— 而同一轮的
//   `wire_gap_max` 只有 10ms（发端已经严格按 10ms 节拍发了）⇒ 那 100ms 是**我们自己**的，
//   不是链路的。钩子把时间戳挪回射频收到包的那一刻，`gap_max`/p99 才开始代表链路。
//   ★ 这里只做"入队 + 返回"：不格式化、不打日志、不碰 LinkRx（与回调纪律一致）。
static bool meas_arrival_sink(const uint8_t* payload, uint8_t len, uint32_t now_ms) {
  return g_meas_rx.noteArrival(payload, len, now_ms);
}

static void meas_sink_register_once() {
  static bool done = false;
  if (done) return;
  done = true;
  dashlink::LinkPhyEspNow::setMeasSink(&meas_arrival_sink);
}

static void meas_start_from_serial() {  // ★★ 不在这里直接 `g_meas_tx.start()`：`g_meas_tx` **只被链路 TX 任务碰**
  //    （见 `link_tick_task` 的说明）。这里只**置请求位**，任务下一拍真正开跑
  //    ⇒ 无跨任务竞态、也不需要加锁。打印用常量（真正的 planned/period 由任务打）。
  g_meas_start_req = true;
  dash_logf("meas: 已请求开跑（发端节拍由链路任务保证）count=%lu period=%ums\n",
            (unsigned long)dashlink::MeasSender::kDefaultCount,
            (unsigned)dashlink::MeasSender::kDefaultPeriodMs);
}

static void meas_poll(uint32_t now) {
  // ---- ① 发：**已搬到 `link_tick_task`**（2026-09-27 深夜，本单第二步）----
  //   原来这里调 `g_meas_tx.poll()`，于是 burst 帧由**主循环**发 ⇒ `wire_gap_max`
  //   量到的是主循环被抢占（实测 98~108ms），而不是链路本身。现在发端节拍由那个
  //   高优先级任务保证，`gap_max`/p99 才开始代表链路。
  //   ★ 配套：`w` 命令不再直接 `start()`，只置 `g_meas_start_req`（线程纪律见任务说明）。

  // ---- ② 收：**已删除**（2026-09-27 深夜；由 (a) 那一步暴露出来的既有缺陷）----
  //
  // 这里原来自己把 PHY 的环读空、按帧长分流：测量信封交给 `MeasReceiver`，
  // 其余"整帧"用 `g_link_rx.feed(byte, &f)` 逐字节喂给 `LinkRx`。
  //
  // ★★ 那条路是错的，而且错得很隐蔽：`LinkRx::feed() = push + advance`，
  //    它会把**凑齐的帧通过 `out` 直接交出来** —— 而这里的 `f` 是**局部变量**
  //    ⇒ **那一帧被丢掉了**；能活下来的只有"喂完还剩在缓冲里、下一圈由
  //    `advance()` 解出来"的那部分残渣。
  //    ⇒ 症状（实测）：**真帧（TICK/DATA）大量丢，而测量帧 0% 丢包** ——
  //      因为测量帧走 `noteArrival`，不经过这条 feed 路。
  //      主板射频实发 **199 帧/秒**、`tx_fail=0`、`pending=0`、`txring=0`（发送健康），
  //      而从板只收到 **~6%** 的 TICK（`seen` 3.8/s，`miss` 每 60 秒涨 3531）
  //      ⇒ **空中没问题，丢在"分发"这一步**。
  //
  // ★ 为什么现在可以整段删掉：稍早加的"**测量信封闸门**"已经把测量帧的识别搬到了
  //   **消费端**（两处 `handleInbound` 之前），于是
  //     · 真帧由 `link_poll_*()` 自己从 PHY 读出来正常处理（那条路一直是好的）；
  //     · 测量帧在消费端被 `noteArrival` 认出来 —— 只统计、不进数据层。
  //   ⇒ 本函数**不再需要接收半边**，只需要 ①发（`w` 的 burst）+ ③收端汇总。
  //   ★ 顺带：`kMeasBytesPerLoop` 与那段"跨圈 carry"也随之不再被使用（常量定义保留，
  //     免得动到别处的注释与编译期守卫；它现在只描述"曾经"的做法）。
  //   ★★ 下面这一行**必须留在注释之外**：它把到达队列取出来算间隔/丢包，是收端唯一的
  //      推进点。本单就把它并进过注释一次 —— 后果是 `mActive` 永不置位、`endedBy()`
  //      永不触发、`meas rx:` 汇总行**静默消失**（而链路上一切正常，很难看出少了什么）。
  g_meas_rx.poll();

  // ---- ③ burst 结束 ⇒ 打收端那一行（只打一次）----
  if (g_meas_rx.endedBy(now)) {
    char line[320] = {0};
    dashlink::measFormatRxSummary(g_meas_rx.result(), line, (int)sizeof(line));
    const dashlink::LinkRxStats& rs = g_link_rx.stats();
    // ★ 既有计数器一起打（`link rx bytes/frames` 那两项就是"这条链路收了多少"）：
    //   不新造一套格式，把测量行的读数接在既有口径后面。
    dash_logf("%s | link rx bytes=%lu frames=%lu crc=%lu\n", line,
              (unsigned long)rs.bytes_read, (unsigned long)rs.frames_ok,
              (unsigned long)rs.crc_err);
  }
}
#endif  // LINK_PHY_ESP_NOW

void setup() {
  dash_log_begin(115200);
  delay(200);
  // 开机握手行:刷机后靠它确认固件真的跑起来了(见 ACCEPTANCE.md)。
  // 放在最前面 —— 即使后面的初始化有问题,至少能看到这一行。
  dash_logf("206 dash ok\n");

  // ★★ 复位原因 + 复位次数（2026-09-24，第 ③ 层防线的取证手段，见 boot_note 那段说明）。
  //   ★ 为什么紧跟在握手行之后：它是"这一板**为什么**从这一行开始"的唯一记录，
  //     而黑屏那件事的候选机制 B（供电把 3.3V 轨拉低）**只有这一行能作证**。
  //   ★ 它要读/写一次 NVS（`bootn`）+ 读一次片上温度 —— 只在 setup 里跑一次，
  //     不影响时间线。
  //   ★ 门是 `ARDUINO`：`esp_reset_reason()` / `temperatureRead()` / NVS 都只有
  //     设备端才有（pcpreview 编这一段会直接编不过，与自检那一段同一条纪律）。
#if defined(ARDUINO)
  boot_note();
#endif

  // ★★ 板上串口"人格"（`FSUSB42UMX` 的 `SEL` = GPIO0）—— 2026-09-26。
  //   位置的两条理由（都写在这儿，免得被挪走）：
  //     ① **在日志起来之后**：这一行是"固件要求哪一边"的自证，
  //        没有 `dash_log_begin()` 就没地方打；
  //     ② **在 43/44 被 UART0 抓走之前**：这里离 `dash_display_init()` /
  //        链路 `begin()` 都还远，GPIO0 这一个脚与 UART 无关，
  //        但顺序写清楚 = 将来没人把它塞到显示初始化之后。
  //   ★ 默认那一档（`USB_PERSONALITY_AUTO = 0`）**一个寄存器都不碰**，
  //     日志照打（它说的是"我们没去扳开关"），与今天的行为逐字节相同。
#if defined(ARDUINO)
  {
    const bool drove = usb_personality_select();
    dash_logf("usb: personality=%s (GPIO0 %s)\n",
              dashusb::requestedPersonalityName(),
              drove ? "driven LOW by us -> FSUSB42 SEL = native USB"
                    : "left alone -> FSUSB42 SEL follows R44/R45 (ch343p)");
  }
#endif

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
#if defined(DASH_DISPLAY_RGB)
  // ★ 真屏构建：把"**这一板上电时记着的是什么**"打出来。
  //   静音开关在这块板上没有按键入口（见上面串口命令那一段），"掉电保存成不成立"
  //   就全靠这一行可测：发过 `m` 之后复位/断电重上，这里应当是
  //   `mute: 1 (loaded from NVS)`；再发一次 `m` 就回到 0。
  //   ★ 只编进 `[env:esp32s3-rgb]`（`DASH_DISPLAY_RGB`，见上面那一段的说明）——
  //     抓帧盒两个 env 的串口日志**一个字节都不变** ✓。
  dash_logf("mute: %d (loaded from NVS)\n", g_beep_muted ? 1 : 0);
#endif
  g_data.begin();
  BOOT_STAGE(2);

  // 告警层 + 蜂鸣器：把实现**挂上**（挂哪个见上面那一段注释 —— L14 的落点）。
#if defined(DASH_DISPLAY_PREVIEW)
  g_buzzer = &g_buzzer_host;      // 预览：打印 BEEP 行（可选系统提示音）
#elif defined(DASH_DISPLAY_RGB)
  // ★★ 真机（2.8C，右/主机板）：挂上写 TCA9554 的 EXIO8 那一档。
  //   这是本单的**核心那一行** —— 少了它，`g_buzzer` 还指着 `BuzzerNull`
  //   （空实现），于是"日志里一切正常、板子一声不响"，而且**看不出来**：
  //   `beep()` 被照常调用、返回 void、没有人报错。
  //   ★ 只是"把 g_buzzer_exio 声明出来"是不够的 —— 必须在**这里**绑。
  g_buzzer = &g_buzzer_exio;
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
  // 物理层 → 打印层 → （`van_frame_in()`：数帧 + 入转发队列 + 数据源）。
  // 打印层只旁观,不影响数据流
  // (抓帧时那行文本就是回放格式,见 VanLogSink 的说明)。
  // ★ 2026-09-27：原来这里还有一行 `g_van_log.setNext(&g_van_sink);` —— 那个
  //   "next 链"已经取消，改由 `van_frame_in()` 一处统一收口（理由见它的说明）。
#if !OBD_BLE_ONLY_TEST
  g_van_phy.setSink(&g_van_log);
  g_van_phy.begin();
  BOOT_STAGE(3);
#endif
#if LINK_ROLE == 1
  // 链路物理层（主板侧）：§0 的 43 发 / 44 收、§1.1 的 115200 8N1。
  // ★ 顺序上的一个已知事实（不改行为，只记清楚）：`dash_log_begin()` 在**本构建**里
  //   只开 USB-CDC（`DASH_LOG_UART0=0`，见本文件上方那段），所以它**不碰** UART0；
  //   下面这一行才是 UART0/43/44 上唯一的占用者。
  //   ★ 而 VAN 采集那个构建（不带 `-DLINK_PHY_UART`）里没有这一段：那边
  //     `dash_log_begin()` 照旧开着 UART0 双通道日志 —— 行为一字未变。
  //
  // ★★ 临时诊断构建（`-DOBD_BLE_ONLY_TEST=1`，2026-09-27 车上）：**不启动链路 PHY**，
  //   让 BLE 独占 2.4G 射频。目的：判定 `status=13` 到底是不是"ESP-NOW 与 BLE 抢射频"
  //   造成的。测完**必须去掉这个宏**（没有链路 = 从板没数据）。
#if !OBD_BLE_ONLY_TEST
  g_link_phy.begin(false);
#endif
  g_link_rx.setLocalRole(dashlink::kLocalRole);
  g_link_tick.reset(millis());   // §4：tick_ms 是主板**自己**的单调毫秒(从复位起算)
  // ★★ DATA 的速率下限（见 `kLinkDataMinIntervalMs` 那一段的实测与理由）：
  //   "没有 VAN 快照"那一条路会把主循环每一圈都当一份新快照 ⇒ 不设这一行就会
  //   以满线速发（实测 946 帧/秒）。设成 12 ms = 契约 §3 给的那一档（≈80 Hz）。
  g_link_data.setMinIntervalMs(kLinkDataMinIntervalMs);
  // ★ 2026-09-27（另一单）：这一行按**编译进来的 PHY** 分两种说法 —— 无线那一档
  //   没有引脚（`txPin()/rxPin()` 报 -1），打 "TX=GPIO-1" 是读不懂的；而 UART 那一档
  //   的字符串**一个字都没改**（`#if` 把无线那一支整个排除在外 ⇒ 逐字节相同）。
#if LINK_PHY_ESP_NOW
  dash_logf("link: 主板侧就绪 %s ch=%u (no pins) @%u%s%s\n",
            g_link_phy.phyName(), (unsigned)g_link_phy.channel(), g_link_phy.baud(),
            g_link_phy.online() ? "" : "  <-- PHY 没起来(见上面 espnow: 那几行)",
            kLinkPhyTail());
#else
  dash_logf("link: 主板侧就绪 TX=GPIO%d RX=GPIO%d @%u 8N1(§0/§1.1)%s\n",
            (int)g_link_phy.txPin(), (int)g_link_phy.rxPin(), (unsigned)dashlink::kLinkBaud,
            g_link_phy.online() ? "" : "  <-- PHY 是空壳(这份固件没编 -DLINK_PHY_UART)");
#endif
#else
  // 从板侧（§5 的角色自检 + §4 时基状态机的初值）+ **真 PHY**（2026-09-25 起）。
  // ★ 与主板那一侧**同一个 `LinkPhyUart`、同一组脚（43 发 / 44 收）、同一个 115200**
  //   —— 契约 §0 的引脚口径本来就不分角色（link_phy_pins.h）。
  //   ★ 没有 `LINK_PHY_UART` 的构建（pcpreview / esp32dev）走空壳 `LinkPhyNull`：
  //     `online()` 报 false、`availableForWrite()` 报 0 ⇒ 链路静默，不卡主循环。
  g_link_phy.begin(false);
  g_link_rx.setLocalRole(dashlink::kLocalRole);
  g_link_time.reset();
  // ★ 2026-09-27：从板**也发**了（HELLO + STATUS，见 `link_slave_tick()`）。
  //   `reset()` 让"上电第一条 STATUS"立刻发（`mHasSent == false`），
  //   与 `g_link_time.reset()` 同一个位置、同一个理由：开机态要显式摆正。
  g_link_status.reset();
  // ★ 2026-09-27：开机这一行把"从板也会发"写进去 —— 判据不能只活在代码里。
  //   两块板外观一样、两个镜像的**这一行是可以对账的**：从板写"发 HELLO/STATUS"，
  //   主板那行写"收 B 的 STATUS/EVENT"（见上面主板那一支）。照 §3：从板这条
  //   UART 是数据面，它自己的日志在**原生 USB-CDC** 上。
  //   ★ `docs/LINK-TWO-BOARD.md` §3.1「应当看到的关键几行（从板）」里那句
  //     `… —— 真 PHY(UART0),等主板的 TICK/DATA` 随之改成了 `—— 真 PHY(UART0)`
  //     （"等主板"已经不是全部了：它现在也发）。两处一起改的，别只改一处。
  // ★ 2026-09-27（另一单）：尾缀改成 `kLinkPhyTail()` —— 那一句现在按**编译进来的
  //   PHY** 分档（"真 PHY(UART0)" / "真 PHY(ESP-NOW)" / "空壳"）。UART 那一档打出来的
  //   字符串**逐字节与改之前相同**（见 kLinkPhyTail() 那段）；无线那一档另有
  //   `espnow: …` 那几行（信道 / 学到对端 MAC）补上"引脚"之外的信息。
#if LINK_PHY_ESP_NOW
  dash_logf("link: 从板侧就绪(§5, LINK_ROLE=0) %s ch=%u (no pins) @%u%s"
            " —— 收主板的 TICK/DATA，发 HELLO(每 5s 直到收到对端)+STATUS(2Hz,§3)\n",
            g_link_phy.phyName(), (unsigned)g_link_phy.channel(), g_link_phy.baud(),
            kLinkPhyTail());
#else
  dash_logf("link: 从板侧就绪(§5, LINK_ROLE=0) TX=GPIO%d RX=GPIO%d @%u 8N1%s"
            " —— 收主板的 TICK/DATA，发 HELLO(每 5s 直到收到对端)+STATUS(2Hz,§3)\n",
            (int)g_link_phy.txPin(), (int)g_link_phy.rxPin(), (unsigned)dashlink::kLinkBaud,
            g_link_phy.online() ? " —— 真 PHY(UART0)"
                                : " —— PHY 是空壳(这份固件没编 -DLINK_PHY_UART)");
#endif
#endif
  // 主题:先默认值(由 dash_ui_init 兜底),再尝试用 flash 里的主题文件覆盖。
  // 加载失败不影响启动 —— 降级到默认主题继续跑。
  theme_load();
  // ★★ 2026-09-27：同一份主题文件里的 `alerts` 段（告警阈值 / 蜂鸣器参数）。
  //   为什么要紧挨着 theme_load()：**同一个文件、同一次启动、同一条刷写路径**
  //   （刷主题分区即生效，固件不用重编）—— 两段分家会让人以为"刷了主题但告警没变"。
  //   口径三条（都别改）：
  //     ① 从**当前配置**出发（= alerts.h 的编译期默认值），文件只覆盖它写了的那几个
  //        ⇒ 老主题文件（没有 alerts 段）行为**一个字节都不变**；
  //     ② 越界值由 `alerts_config_clamp()` 挡（那份配置现在是外部输入）；
  //     ③ **静音不在这个文件里** —— `muted` 是运行时状态（`m` 键 / 预览页 `M`，
  //        存 NVS、跨上电记住，见下面 `g_beep_muted` 那一段）。把它也写进文件会
  //        造出"两个真相"，所以刻意不写；要静音就是按一下 `m`（一次，之后一直记住）。
  {
    AlertsConfig ac = g_alerts.config();
    if (theme_load_alerts(ac)) {
      g_alerts.setConfig(ac);
      // 这一行是**判据**：刷一份带 alerts 段的主题后，这里打出来的必须就是
      // 文件里那几个数（编辑器导出的就是下面这些字段名，逐项对得上）。
      dash_logf("alerts: 已应用主题里的 alerts 段 "
                "(超速 %.0f/迟滞 %.0f, 红区 %.0f/迟滞 %.0f, 门去抖 %ums, "
                "转向忘关 %us, 去抖 %ums, 响 %ums, 最短间隔 %ums, only_highest=%u)\n",
                (double)ac.overspeed_kmh, (double)ac.overspeed_hyst_kmh,
                (double)ac.redline_rpm, (double)ac.redline_hyst_rpm,
                (unsigned)ac.door_debounce_ms,
                (unsigned)(ac.turn_signal_on_ms / 1000u),
                (unsigned)ac.debounce_ms, (unsigned)ac.beep_ms,
                (unsigned)ac.beep_min_interval_ms,
                ac.only_highest ? 1u : 0u);
    }
  }
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
#if OBD_BLE
  // ★★ BLE 的 `start()` **必须**放在这里 —— 显示初始化**之后**。
  //   它内部会 `NimBLEDevice::init()`，而 NimBLE 协议栈要吃 ~50KB **内部 RAM**；
  //   放到显示之前会让 RGB 面板的弹跳缓冲分配失败（实测：屏一直黑、
  //   `rgb: 面板创建失败 err=257`，而主循环/链路/VAN 全都正常）。
  //   放在这里之后，面板已经把它的内部 RAM 拿走了，剩下的才给 NimBLE。
  {
    const bool ok = g_obd_ble.start();
    dash_logf("obd: BLE start() = %d  heap=%uKB(面板之后)\n",
              (int)ok, (unsigned)(ESP.getFreeHeap() / 1024u));
  }
#endif
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
  // ★★ 停顿探测（2026-09-25）：**放在第一行** —— 它量的就是"上一圈整个花了多久"，
  //   而"上一圈"包括下面所有分支（链路收帧、数据更新、LVGL 渲染、显示 poll）。
  //   放在第一行的另一个好处：卡死那一刻**打不出来的那一行**就是证据本身 ——
  //   串口上最后一行 `loop: stalled` 的 `n=` 与"之后一共转过几圈"能对上账。
  loop_probe_begin(now);
  // 示位标报的"第几步"= 本轮**已经走到**的最后一步(不是累计值):
  // 所以卡在哪一步,串口上看到的就是哪一步。
  BOOT_STAGE(6);
  loop_stage("van");     // ★ 步骤②归因：以下各阶段标记只为把长圈归因，不改变行为
#if LINK_PHY_ESP_NOW
  probe_start_once();    // ★ 选项(a) 可行性探针：20ms 定时器回调的实际节拍（见它的说明）
  link_tx_task_start_once();   // ★★ 选项(a)：建互斥量 +（主板上）建 TICK 发送任务
  meas_sink_register_once();   // ★★ 测量帧的到达钩子：时间戳打在射频收到包那一刻
#endif
  g_van_phy.tick(now);   // VAN 物理层解帧 → 喂给 data_service
                         // (桩 / GPIO 收帧两种实现共用这一个接口,见 van_phy.h:
                         //  加 -DVAN_PHY_GPIO=1 时这里就是真的 GPIO 收帧)
  van_replay_poll(now);  // 串口贴帧离线回放(和物理层等价,先到的先写)

#if LINK_PHY_ESP_NOW
  // ---- 无线那一档的测速/测丢包：**必须在 `link_poll_*()` 之前** ----
  // ★ 顺序是硬的：测量帧本身也是合法的 v1 DATA 帧，`LinkRx` 会把它当车辆数据
  //   喂进数据层 ⇒ 必须先在这里按帧长把测量信封摘走（见 `meas_poll()` 的说明）。
  // ★ 它在 UART / 空壳两档里**整个不存在**（`#if` 之外一行都没有）⇒
  //   那两档的主循环逐字节与以前相同。
  //
  // ★★ 2026-09-27 上板实测抓到的事故（这一行**原来在 `link_poll_*()` 之后**）：
  //   注释写着"必须在之前"，代码却摆在从板 `link_poll_bounded_slave()` 的**后面**
  //   ⇒ 两件事同时发生，而且都看见了：
  //     ① **测量帧被 `LinkRx` 吃进数据层**：从板屏上出现 `coolant=9.0C intake=-39.0C`
  //        这种不可能的读数，`SRC …` 在 `sim`/`link` 之间来回跳（信封的字节被当成
  //        车辆标量解读）；
  //     ② **收端永远打不出 `meas rx:` 汇总行** —— `meas_poll()` 那一圈从 PHY 读到的
  //        字节已经被 `link_poll_*()` 取空了 ⇒ 统计器一帧都没见过。
  //   ⇒ 位置就是判据：**放在两个角色分支之前**（一处、两种角色共用），
  //     而不是"放在从板分支后面、再在主板分支后面补一次"（那样主板还会被调两次）。
  loop_stage("linkrx");
  meas_poll(now);
#endif

#if LINK_ROLE != 1
  // ---- 链路（从板侧）：**先收后合并** ----
  // ★ 顺序是硬要求：收到的 DATA 必须先 `applyLinkData()`、再 `g_data.update(now)`，
  //   这样这一圈收到的值当圈就进快照与上屏（晚一圈也行，但没必要）。
  // ★ 从板的 PHY 从 2026-09-25 起是**真 UART**（有 `LINK_PHY_UART` 时，见本文件上方
  //   那段）—— 所以这一行现在真的会去读 GPIO44。
  // ★★ 2026-09-25：改名 `link_poll_frames_slave` → `link_poll_bounded_slave`。
  //   同名同形，唯一的差别是**每圈吃进来的字节数封顶**（`kLinkRxBytesPerLoop`）——
  //   车主那条"画面卡住 + 蜂鸣器长鸣"就指着这一条不再发生（见那一段的说明）。
  // ★★ 2026-09-27：**从板也会发**了 —— 排水那一行不再是空转的注释（见
  //   `link_slave_tick()` 那一段）。顺序是硬的，两处都在下面写着：
  //     ① 这里是 loop() 开头 ⇒ **先收**（`link_poll_bounded_slave` 已经跑完了）；
  //     ② 真正的"enqueue + 排水"在下面"上一帧渲染已过 200 ms"那一支里 ——
  //        因为 `STATUS.left_face` 要的是**这一拍上屏的档位**，而 `make_view()`
  //        就在那一支里算（理由与"为什么不能再调一次 face_update()"见上面那段）。
  //   ⇒ 去掉下面那一支里的 `link_slave_tick()` 会让从板退回到"收得到、一个字都不发"，
  //     而且**不会有任何编译期信号**（这正是本单要补的那个缺口）。
  link_poll_bounded_slave(now);
#endif

  // ★ 2026-09-24：由 `const VehicleState st` 改成**可写**的 `st_mut` ——
  //   pcpreview 的输入注入要在这份快照上覆写几个字段（见下面 preview_apply）。
  //   设备侧一个字都没变：`st_mut` 在那边从来不会被改（那一段在 #if 里）。
  //   名字刻意带 `_mut`：后面读代码的人一眼知道"这里可能被注入改过"。
  loop_stage("data");
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
  // ★★ 2026-09-27：无线那一档的 `meas_poll(now)` **已上移到两个角色分支之前**，
  //   全固件只有那一处调用。原先这里是"主板侧再补调一次"，而从板那一侧真正的
  //   调用点落在 `link_poll_bounded_slave()` **之后** ⇒ 从板顺序反了（实测记录
  //   在那一处）。现在两种角色共用同一处，且保证在 `link_poll_*()` 之前。
#if defined(DASH_DISPLAY_PREVIEW)
  // ---- pcpreview 的输入注入：在数据合并**之后**、算视图**之前** ----
  // 顺序是硬的：注入是"人替传感器说话"，所以要压过 sim/VAN/OBD 的合并结果；
  // 但它**不回流**进 data_service（不碰优先级、不产生协议行为）。
  if (preview_input_poll(g_preview)) {
    preview_apply(g_preview, st_mut);
  }
  // ★★ 开机窗口那条**预览专用**的钩子（2026-09-25）：控制文件里的 `hold=1`
  //   把"开机窗口还开着"钉住，好让**开机角色标签**稳定地留在落盘的帧上；
  //   置 0 之后标签应当在下一拍消失（判据见 src/dash_display.h 那段）。
  //   ★ 与 `mask` 同一类（"预览这一层怎么画"），所以走"推给显示侧"这条路，
  //     不往车状态快照上写任何字段 —— 设备端这一整段不存在（门是既有的
  //     `DASH_DISPLAY_PREVIEW`）。
  if (g_preview.hold_set) {
    dash_display_preview_set_boot_hold(g_preview.hold);
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
      // ★ 这一行**只报页号**，不报 fps：`g_ui_fps10` 是**本拍渲染完之后**才更新的
      //   （见下面那段 EMA）⇒ 在这里读它要么是 0、要么是 200 ms 之前的旧值，
      //   而那是"看着像 bug"的数字。**上屏那一格**是渲染时读的，所以它是对的；
      //   要核对它只需要看屏（或控制台里每秒那行 `206 dash ok` 的节奏）。
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
  // ★ 先推进时序，再决定"这一拍该不该响"（顺序是硬的）：
  //   · `tick()` 负责"到点关 + 多相序列的下一相"（真机那一档才有实际动作；
  //     `BuzzerNull`/`BuzzerHost` 是默认空实现 ⇒ 这两档一个字节都没变）；
  //   · 没有它，`Triple`/`Urgent`/`Long⇒3 短哔` 这些**跨好几拍**的序列
  //     会永远停在第一相 —— 听起来就是"本该 3 声、只响 1 声"。
  //   ★ 它**不阻塞**（`BuzzerExio::tick()` 只是几次整数比较 + 至多一次写位）。
  g_buzzer->tick();
  // ★★ 蜂鸣器的**绝对上限**（2026-09-25 新增，`Buzzer::safety()`）：
  //   `tick()` 那条路是"到点关"，可它**只在主循环转得动时才走**。车主那条
  //   "长鸣一会儿"说明存在"主循环停住 ⇒ 谁都没去关"的形态 ⇒ 这里再加一条
  //   **与序列状态机无关**的兜底：任何一次 `beep()` 起表之后，只要墙钟超过
  //   `kBuzzerSafetyMs`（2 s）还在响，就无条件关掉 + 丢弃序列 + 打一行日志。
  //   ★ 它**不动**既有那条"单次哔 ≤300ms"的硬约束（序列自己仍然按相走）——
  //     这一条是**额外**的、只在"那段逻辑没机会跑"的时候才生效的天花板。
  //   ★ 代价：每圈两次整数比较（`beep()` 里的起表时刻 + `millis()`）。
  g_buzzer->safety();
#if defined(DASH_DISPLAY_RGB)
  // ★★ 串口 `b` 那一拍的窗口内：**只推进、不取消**（理由见 `beep_cmd_window()`）。
  //   这一段**只进真屏那一份构建**（`DASH_DISPLAY_RGB`，与命令 `b` 同一道门）
  //   ⇒ 抓帧盒三个 env 里一行都不存在 ✓。
  const bool beep_window = beep_cmd_window(now);
#endif
  if (g_alerts.beeping()) {
    g_buzzer->beep(g_alerts.pattern(), g_alerts.config().beep_ms);
#if defined(DASH_DISPLAY_RGB)
  } else if (beep_window) {
    // 串口 `b` 的窗口内、且没有告警在响 ⇒ **不调 off()**（它是"取消"，
    // 会把这一拍掐成一声）。这一拍已经由上面的 `tick()` 在推进了。
  } else {
    g_buzzer->off();
  }
#else
  } else {
    g_buzzer->off();
  }
#endif
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
#if defined(PANEL_GUARD_FAULT_INJECT) && (PANEL_GUARD_FAULT_INJECT == 1)
  // ★ 临时注入路径（默认构建里**这一行不存在**）：按守护的节拍把故障写进去。
  //   放在 `dash_display_poll()` **之前**：那一拍里守护可能会做检查 ⇒
  //   先写坏、再让它读，才是"一次注入 = 一次异常"。
  fault_inject_poll(now);
#endif
  dash_display_poll(); // 设备上为空;pcpreview 落 BMP 帧
  BOOT_STAGE(8);

  // ★★ 2026-09-25：**10 分钟一次的心跳**（把"当前 uptime + 守护累计"落进 NVS）。
  //   起因：开机那行只能报**当前**这一次为什么起来，而"上一次运行了多久"在掉电那一刻
  //   没有任何人来得及记 ⇒ 只能运行期不断地写（判据/边界在 `lib/dashcore/boot_persist.h`，
  //   宿主机逐条钉着）。这一行**只在到点那一拍**真写 NVS，其余每一拍就是一次整数比较
  //   ⇒ 不占显示时间线、也不磨损 flash（10 分钟一次 ≈ 5.3 万次/年，见那份文件头）。
  //   ★ 它读的是**守护此刻的四个数**（只读，不改守护的任何状态）。
#if defined(DASH_DISPLAY_RGB)
  {
    GuardTotals cur;
    cur.rd   = dash_panel_guard_rd_ok();
    cur.fix  = dash_panel_guard_fix();
    cur.bl   = dash_panel_guard_bl();
    cur.anom = dash_panel_guard_anomalies();
    if (g_boot_persist.tick(now, cur)) {
      g_boot_hb_n = g_boot_persist.heartbeats();
      // ★ 心跳那一行（10 分钟一次）—— `k` 是累计值落盘次数（跨重启单调 +1）。
      dash_logf("hb: up=%lus rd=%lu fix=%lu bl=%lu anom=%lu k=%lu\n",
                (unsigned long)(now / 1000u),
                (unsigned long)boot_accum(g_boot_persist.guardBase().rd, cur.rd),
                (unsigned long)boot_accum(g_boot_persist.guardBase().fix, cur.fix),
                (unsigned long)boot_accum(g_boot_persist.guardBase().bl, cur.bl),
                (unsigned long)boot_accum(g_boot_persist.guardBase().anom, cur.anom),
                (unsigned long)g_boot_persist.snapshots());
    }
  }
#endif

  loop_stage("render");   // ★ 归因重点：这一支是"上一帧渲染已过 200ms"的大块
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
    // ★★ 这一行是**唯一**算 `ArcDashView` 的地方 —— 于是它也是从板唯一能拿到
    //   `STATUS.left_face` 的地方（理由见 `link_slave_tick()` 文件头那段：
    //   不能另调一次 `face_update()`，那会推进第二条状态机）。
    //   ★ 顺序：**先 render（把这一拍的档位推上屏）、再把它报给主板** ——
    //     反过来写会让 `left_face` 比屏上晚一拍（200 ms 的错位，肉眼看不出来，
    //     但"屏上显示的档位"与"主板日志里记的档位"就不是同一个时刻的了）。
    const ArcDashView view = make_view(st_mut, now);
    dash_ui_render(view, lamps, g_sys, diag_in, now);
#if LINK_ROLE != 1
    // ---- 链路（从板侧）：发 HELLO + STATUS + 排水（★ 2026-09-27 新增）----
    // ★ 位置就是上面那段的第一条理由：`view` 是刚刚推上屏的那一份 ⇒
    //   `left_face` 与屏上一致；而且它与渲染同拍（200 ms），正好把 2 Hz 的
    //   STATUS 节奏卡得整整齐齐（§3 的 500 ms 是节流的整数倍，不会抖动）。
    link_slave_tick(now, view);
#endif
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
  //   ★★ 2026-09-26 补：那条纪律**依然成立**，但它的形态变了 —— 现在
  //     `dash_logf()` 只是"格式化进环"（`lib/dashcore/dash_log.h` 的硬约定）：
  //     有没有人在读**完全不影响**主循环这一行花多久；没人读时环满就丢 + 计数。
  //     所以"无条件打印"现在是一个**便宜**的选择（以前它是一次可能阻塞 2 s 的写）。
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

    // ★★ 2026-09-26：这几行**周期性遥测**统一走"每 5 秒那一拍"（本来就是
    //   同一个 `if`），不再额外挂闸门 —— 它们已经在最低频那一档了。
    //   真正需要节流的是**每秒**那几条（`206 dash ok` / `rgb:` / `SRC` 家族），
    //   见 `kTelemetryLogMs` 与各调用点上的 `RateGate`。
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
                " | alert=%s beeps=%lu%s | buzz safety=%lu\n",
                    (s.lights_age_ms == UINT32_MAX) ? "-" : age_lights,
                    (s.door_age_ms == UINT32_MAX) ? "-" : age_door,
                    (s.vin_age_ms == UINT32_MAX) ? "-" : age_vin,
                    fieldSourceName(s.indicator_left),
                    fieldSourceName(s.door),
                    fieldSourceName(s.vin),
                    alertName(g_alerts.active()),
                    (unsigned long)g_alerts.beepCount(),
                    g_alerts.muted() ? " muted" : "",
                    // ★ 2026-09-25：被"2 秒绝对上限"掐过几次（真屏那一档才有实际值）。
                    //   `0` = 没发生过"到点关那个动作没被执行"这件事。
                    (unsigned long)buzzer_safety_cuts());
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
      // ★★ 原始帧转发（`0x21`）的发送侧读数（2026-09-27）。
      //   三个数的含义**别混**：pushed 只说明"进了队列"，真的有没有发出去要看
      //   从板那一行的 ok（链路丢包由两者之差可见 —— 与 DATA 同一条口径）。
      //   · `long=` = 数据太长搬不过去（VIN 那 17 字节）——**这是设计限制，不是故障**；
      //   · `drop=` = 环满/`LinkTx` 满丢掉的整帧 —— **这个必须是 0**，非 0 说明
      //     "VAN 的到达率 + 主循环的排水能力"不匹配，先看 `loop: max=` 有没有被抢占。
      dash_logf("vanraw: pushed=%lu drop=%lu long=%lu queued=%uB\n",
                (unsigned long)g_van_raw.pushed(), (unsigned long)g_van_raw.dropped(),
                (unsigned long)g_van_raw.tooLong(), (unsigned)g_van_raw.queuedBytes());
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
    // ★★ 原始帧转发（`0x21`）的**接收侧**读数（2026-09-27）。
    //   怎么判"这一单成了"：`ok` 与主板那行的 `pushed` 同量级（差值 = 链路丢的），
    //   而且从板的 `SRC speed=` 应当从 `link` 变成 **`van`** —— 那说明这些速度/转速
    //   是**从板自己**从原始帧里解出来的，不再是主板算好送过来的。
    //   `bad>0` 说明有帧长度对不上（那是协议/版本不一致的信号，不是噪声）。
    dash_logf("vanraw: ok=%lu bad=%lu age=%lums\n",
              (unsigned long)g_vanraw_ok, (unsigned long)g_vanraw_bad,
              (unsigned long)(g_vanraw_last_ms == 0u ? 0u : (now - g_vanraw_last_ms)));
#endif
#if OBD_BLE
    // ★ BLE OBD 那条路的体检行（2026-09-27）。跟着 1 Hz 心跳打，不另开节拍。
    //   怎么判"这一单成了"：`state=ready` + 上面的 `SRC … intake=obd`（或 rpm/coolant）。
    //   `drop>0` = 通知环满丢过字节 —— 那会让回答被截断、解出错的 PID 值，
    //   先看是不是主循环被抢占太久（同一行的 BEACON 里有 `loop: max=`）。
    //   ★ 末尾那两个 `cs=a/b` 是**车上排查用的计数器**（2026-09-27）：
    //     `a` = 进 connecting 分支几次、`b` = 真的发起 `client->connect()` 几次。
    //     它能不依赖日志就分清三种情况：`a=0` 进不去分支 / `a>0,b=0` 卡在等 600ms /
    //     `a=b` 说明连接请求真发出去了（那问题在对端或控制器）。
    dash_logf("obd-ble: state=%s peer=%s conn=%u drop=%lu connects=%lu cs=%lu/%lu\n",
              g_obd_ble.stateName(), g_obd_ble.peerText(),
              (unsigned)g_obd_ble.connected(), (unsigned long)g_obd_ble.dropped(),
              (unsigned long)g_obd_ble.connects(),
              (unsigned long)g_obd_ble.csAttempts(), (unsigned long)g_obd_ble.csCalls());
#endif
#if VAN_SNIFF
    van_sniff_report(now);
#endif
  }

#if OBD_BLE
  // ★★ BLE 那条路的心跳（**每圈都调**，不是 1 Hz）：它推进"扫描 → 连接 → 重连"。
  //   放在心跳块**外面** —— 那个块是 1 Hz 的，而连接状态机需要更细的推进
  //   （退避最小 500ms）。`tick()` 自己非阻塞、自己判时间。
  g_obd_ble.tick(now);
#endif

#if defined(LINK_PHY_ESP_NOW) && (LINK_PHY_ESP_NOW != 0) && (LINK_ROLE != 1)
  // ==========================================================================
  //  ★★ 从板：**收面停摆看门狗**（2026-09-27 新增，起因是一次实测挂死）
  // ==========================================================================
  //  实测现场（240 秒抓包，车主报"长时间怠速后副板进模拟数据 + 角标"）：
  //    · 副板 `espnow: rx_frames=190250 … gap_rx=1469482ms` —— **射频接收整整
  //      24.5 分钟没进过一帧**，而同一行的 `tx_frames` 一直正常在涨；
  //    · 于是 `link: sim tick_age=1469s` ⇒ 屏上全字段回退模拟数据 + 挂"数据不可信"角标；
  //    · **不会自愈**（重启两块板立刻恢复）。
  //
  //  为什么必须靠重启而不是"软恢复"：那是**射频接收面**停了 ——
  //  收包回调本身只做"拷字节 + 计数"（见 `link_phy_espnow.cpp` 的 `onRecv`），
  //  没有会卡住的分支；`overflow=0`、`rx_foreign=0` 说明环没满、也没被杂包误导。
  //  这种状态在应用层没有任何可用的抓手（重注册回调也救不回一条停掉的 RX 通道）。
  //
  //  ★ 判据用 `tickAgeMs()`：它是"距上次收到 TICK 多久"，正常时**永远**在几百 ms 级
  //    （TICK 是 20 Hz）。取 **15 秒**（= 正常值的 ~300 倍）保证不会误判：
  //    · 主循环被长任务抢占、或链路短暂劣化（实测 >500 ms 只是"降级"）都到不了 15 s；
  //    · 而从板挂死时它是**单调涨**的（1469 s），一定触发。
  //  ★ 只在从板加：主板挂着屏和 VAN 接收，重启它代价大得多；而且主板的收面挂了
  //    会让从板超时 → 从板重启 → 重连，已经能兜住大部分情形。
  //  ★ 重启代价：从板左屏黑一两秒、随后自己重连（`AH`/peer MAC 走 NVS，不用重配）。
  //    与"永久卡在模拟数据"相比这一步是净赚。
  {
    static uint32_t wd_seen_age = 0;
    static uint32_t wd_since_ms = 0;
    const uint32_t age = g_link_time.tickAgeMs();
    if (age != wd_seen_age) {        // 还在收到东西 ⇒ 计时清零
      wd_seen_age = age;
      wd_since_ms = now;
    } else if ((uint32_t)(now - wd_since_ms) >= 15000u) {
      dash_logf("link: ★ 收面停摆看门狗触发 —— tick_age 连续 15s 不动(=%lums) ⇒ 重启从板\n",
                (unsigned long)age);
      dash_log_drain();              // ★ 先把这行写出去(它是重启前唯一的现场)
      delay(50);
      ESP.restart();
    }
  }
#endif

  // ★★ 日志排空（2026-09-26）：**整圈的最后一步**。
  //   为什么放在最后：`dash_logf()` 现在只是"格式化进环"（`lib/dashcore/dash_log.h`），
  //   真的写串口只有这一处 —— 于是"这一圈花了多久"里**包含**了写串口那一段，
  //   而 `loop_probe_begin` 量的正是它 ⇒ 判据（`max=` / `stall=`）是**带日志成本**的。
  //   它**不会阻塞**：预算 512 B/圈 + 写之前先问 `availableForWrite()`；
  //   端口报满就一个字节都不写（剩下的下一圈再来，环满了就丢并计数）。
  //   ⇒ 车上的常态"没有电脑读串口"从此不再是"卡死"，而是"日志丢几行"。
  loop_stage("log");
  dash_log_drain();

  // ★★ 停顿探测的收尾（2026-09-25）：到点打一行摘要（`n=` / `max=` / `stall=`）。
  //   放在**整圈的最后一行** —— 于是它统计的 `n=` 就是"这一窗口里完整跑完的圈数"，
  //   而"卡住的那一圈"永远不会被计入（它根本没走到这里）⇒ 这正是要的读数。
  loop_probe_end(now);
}

#endif  // !defined(LINK_LOOPBACK_FIRMWARE)
