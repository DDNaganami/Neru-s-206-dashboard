#include <Arduino.h>
#include "dash_log.h"   // 日志同时打到 USB-CDC 与 UART0(见文件头说明)
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
#include "image_load.h"
#include "theme_store.h"
#include "ui_model.h"
#include "van_phy.h"
#include "van_replay.h"

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
  // 找出计数最高的几个 IDEN(桶只有 4096 个,直接扫,不用排序)
  char buf[220];
  int n = snprintf(buf, sizeof(buf), "sniff: 共 %lu 帧, IDEN: ",
                   (unsigned long)g_iden_total);
  // 每轮挑出当前最大的、且没打过的那一个,最多打 6 个
  bool shown[4096] = {};
  for (int pick = 0; pick < 6; ++pick) {
    uint16_t best = 0;
    bool found = false;
    for (int i = 0; i < 4096; ++i) {
      if (shown[i] || g_iden_count[i] == 0) continue;
      if (!found || g_iden_count[i] > g_iden_count[best]) { best = (uint16_t)i; found = true; }
    }
    if (!found) break;
    shown[best] = true;
    if (n < (int)sizeof(buf) - 24) {
      n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%03X=%u ",
                    (unsigned)best, (unsigned)g_iden_count[best]);
    }
  }
  dash_logf("%s\n", buf);
  // 车速帧单独点名 —— 它决定 SRC speed 能不能从 sim 变 van
  dash_logf("sniff: 车速帧 IDEN 0x824 计数 = %u%s\n",
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
    if (next_) next_->onPacket(pkt);   // 转发给数据源:打印归打印,数据照收
  }
  void setNext(VanSink* n) { next_ = n; }

private:
  VanSink* next_ = nullptr;
};

static VanLogSink g_van_log;

static uint32_t last_ui_ms = 0;
static uint32_t last_status_ms = 0;

// 串口离线回放 VAN 帧:一行 "VAN 824 18F82710000000" 喂一帧(见 van_replay.h),
// 实车接收发器前先用抓到的帧联调,不用先焊板。
//
// ★ 两个口**都收**:S3 上插原生 USB 口是 Serial(USB-CDC),插板载 CH340 那个
//   UART 口是 Serial0(UART0)—— 手头只有一根线,插哪个口都得能贴帧
//   (2026-09-18:就是把线插去了 UART 口,才有了这条)。
//   一行只允许来自一个口,所以两个口各自维护自己的行缓冲(共用一个 len 会串行)。
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
  g_data.begin();
  BOOT_STAGE(2);
  // 物理层 → 打印层 → 数据源。打印层只旁观,不影响数据流
  // (抓帧时那行文本就是回放格式,见 VanLogSink 的说明)。
  g_van_log.setNext(&g_van_sink);
  g_van_phy.setSink(&g_van_log);
  g_van_phy.begin();
  BOOT_STAGE(3);
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
  const VehicleState st = g_data.update(now);
  BOOT_STAGE(7);
  dash_ui_tick(now);   // LVGL 心跳,每个循环都跑
  dash_display_poll(); // 设备上为空;pcpreview 落 BMP 帧
  BOOT_STAGE(8);

  if (now - last_ui_ms >= 200) {
    last_ui_ms = now;
    dash_ui_render(make_view(st, now), now);
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
                  st.speed_kmh, st.rpm, st.coolant_c, st.intake_c);
    // 实测刷新率:判断"K 线够不够用"的唯一依据(见 obd_source.h 的时隙账)
    dash_logf("SRC-Hz rpm=%.1f cool=%.1f intake=%.1f speed=%.1f\n",
                  s.obd_rpm_hz, s.obd_coolant_hz, s.obd_intake_hz, s.obd_speed_hz);
#if VAN_SNIFF
    van_sniff_report(now);
#endif
  }
}
