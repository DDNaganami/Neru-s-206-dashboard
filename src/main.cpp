#include <Arduino.h>
// ★ 开机自检与 esp_partition 只有**设备端**才有(宿主机没有 ESP.* / esp_partition.h)。
//   用 ARDUINO 判定:真 Arduino 框架(esp32dev / esp32s3)会定义它,
//   pcpreview 的宿主机桩不定义 —— 于是同一份 main.cpp 两端都能编。
#if defined(ARDUINO)
#include <esp_partition.h>
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

// OBD(K 线)串口:板子和引脚定了以后在这里接上。
//   例: Serial1.begin(38400, SERIAL_8N1, OBD_RX_PIN, OBD_TX_PIN);
//       static VehicleDataService g_data(&Serial1);
// 没接 OBD 时传 nullptr,只跑假数据。
static VehicleDataService g_data(nullptr);

// VAN 物理层:默认是桩(无硬件)。
//   ★ 收发器(SN65HVD230)到货后:编译时加 -DVAN_PHY_GPIO=1
//     (见 platformio.ini 的 [env:esp32s3]),RO 接 GPIO16,DE/RE 接 GND。
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

// 打印每一帧 VAN,格式**故意与 van_replay 的行格式一致**:
//     VAN 824 18 F8 27 10 00 00 00
// 于是"车上抓到的串口日志"可以直接粘回设备的串口(或喂给宿主机测试)来回放 ——
// 抓帧、分析、复现用的是同一份文本,不用转换。
// 校验不过的帧单独用 '# ' 开头打印(它不能拿去回放,但"有没有收到东西"要看它)。
class VanLogSink : public VanSink {
public:
  // 只要 824(车速/转速)这一帧?先全打 —— 反查协议时缺的就是"别的帧长什么样"。
  void onPacket(const VanPacket& pkt) override {
    if (!pkt.fcs_ok) {
      Serial.printf("# VAN 校验失败 iden=%03X len=%u\n", (unsigned)pkt.iden, (unsigned)pkt.len);
    } else {
      Serial.printf("VAN %03X", (unsigned)pkt.iden);
      for (uint8_t i = 0; i < pkt.len; ++i) Serial.printf(" %02X", (unsigned)pkt.data[i]);
      Serial.printf("   # cmd=%u ack=%u\n", (unsigned)pkt.cmd, (unsigned)pkt.ack);
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

// 上电后"每秒补打自检"的窗口长度(见 loop 里的说明:USB-CDC 的
// connected 标志要靠数据流动才置位,所以不得不主动打)。
static const uint32_t kBringupMs = 20000;

// 串口离线回放 VAN 帧:一行 "VAN 824 18F82710000000" 喂一帧(见 van_replay.h),
// 实车接收发器前先用抓到的帧联调,不用先焊板。
static void van_replay_poll(uint32_t now) {
  static char line[80];
  static uint8_t len = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len) {
        line[len] = '\0';
        VanPacket p{};
        if (parseVanReplayLine(line, &p, now)) {
          g_data.onVanPacket(p);
        } else {
          Serial.printf("VAN? %s\n", line);   // 解析失败回显,方便排错
        }
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

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
  Serial.printf("--- 自检(%s)---\n", tag);
  Serial.printf("chip  : %s rev%d, %d 核 @ %u MHz\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores(),
                (unsigned)getCpuFrequencyMhz());
  Serial.printf("flash : %u MB (IDE 编译目标 %u MB)\n",
                (unsigned)(ESP.getFlashChipSize() / (1024u * 1024u)),
                (unsigned)(IMAGE_PARTITION_BYTES / (1024u * 1024u)));
  Serial.printf("psram : %u KB 可用 / %u KB 总\n",
                (unsigned)(ESP.getFreePsram() / 1024u), (unsigned)(ESP.getPsramSize() / 1024u));
  Serial.printf("heap  : %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024u));
  // 分区表里的图片分区大小 —— 与编译期的口径对不上就说明烧错了分区表
  const esp_partition_t* ip = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "image");
  if (ip) {
    Serial.printf("image : 分区 %u KB @ 0x%06X(编译期口径 %u KB)%s\n",
                  (unsigned)(ip->size / 1024u), (unsigned)ip->address,
                  (unsigned)(IMAGE_PARTITION_BYTES / 1024u),
                  (ip->size == IMAGE_PARTITION_BYTES) ? "" : "  ← 不一致,检查分区表!");
  } else {
    Serial.println("image : 分区不存在(检查分区表)");
  }
}
#endif  // DASH_DEVICE_SELFTEST

void setup() {
  Serial.begin(115200);
  delay(200);
  // 开机握手行:刷机后靠它确认固件真的跑起来了(见 ACCEPTANCE.md)。
  // 放在最前面 —— 即使后面的初始化有问题,至少能看到这一行。
  Serial.println("206 dash ok");

  // ★ 设备端的 USB-CDC 是**没有主机的缓冲**的:监视器如果没在开机前打开,
  //   这几行就永远看不到了(实测踩过:刷完立刻开监视器,一片空白)。
  //   所以开机打一次,loop() 里"主机第一次连上"时再补打一次 —— 见 loop()。
  //   这里只等 200ms(上面那句 delay),**不做**"等主机"的阻塞等待:
  //   车上是没有 USB 主机的,阻塞等待会让每次上电都白等几秒。
#if defined(DASH_DEVICE_SELFTEST)
  print_selftest("上电");
#endif

  g_data.begin();
  // 物理层 → 打印层 → 数据源。打印层只旁观,不影响数据流
  // (抓帧时那行文本就是回放格式,见 VanLogSink 的说明)。
  g_van_log.setNext(&g_van_sink);
  g_van_phy.setSink(&g_van_log);
  g_van_phy.begin();
  // 主题:先默认值(由 dash_ui_init 兜底),再尝试用 flash 里的主题文件覆盖。
  // 加载失败不影响启动 —— 降级到默认主题继续跑。
  theme_load();
  // 图片资源(背景/开机帧/表情)也从 flash 分区加载,同样是失败即降级。
  // 现在还没有代码把图画到屏上(等真屏),但这一步必须先接上:
  // 否则"刷了图片没反应"在设备上是完全静默的,到时候无从判断是
  // 分区表、mmap 还是格式的问题。接上后上电串口就会说明白。
  image_load();
  dash_ui_init();
  // 一行汇总:有没有图片资源一眼可见(没刷图片是正常情况,不是错误)。
  {
    const ImageBlobHeader* ih = image_blob_header();
    if (ih) {
      Serial.printf("image ok: %u 张,数据 %u 字节,镜像 %u 字节\n",
                    (unsigned)ih->count, (unsigned)ih->data_bytes,
                    (unsigned)image_blob_len());
    } else {
      Serial.println("image none: 无图片资源,背景用主题纯色");
    }
  }
  // 物理层类型:抓帧时第一眼要确认的就是这一行 ——
  // 写着 stub 就说明这次编译**没有**启用 GPIO 接收(-DVAN_PHY_GPIO=1),
  // 那样即使收发器接好了也不会有任何帧进来。
#if defined(VAN_PHY_GPIO)
  // 具体引脚与"空闲关帧"的阈值由 VanPhyGpio::begin() 自己打印(见 van_phy_gpio.cpp)
#else
  Serial.println("van phy: stub(没启用 GPIO 接收;要抓帧请用 -DVAN_PHY_GPIO=1 编译)");
#endif
}

void loop() {
  const uint32_t now = millis();
  g_van_phy.tick(now);   // VAN 物理层解帧 → 喂给 data_service(当前为桩)
  van_replay_poll(now);  // 串口贴帧离线回放(和物理层等价,先到的先写)
  const VehicleState st = g_data.update(now);
  dash_ui_tick(now);   // LVGL 心跳,每个循环都跑
  dash_display_poll(); // 设备上为空;pcpreview 落 BMP 帧

  if (now - last_ui_ms >= 200) {
    last_ui_ms = now;
    dash_ui_render(make_view(st, now), now);
  }

  // ★ 上电后的"补打窗口":头 20 秒每秒打一次自检。
  //
  // 为什么不是"等主机连上再打"(第一版就是这么写的,结果更糟):
  //   HWCDC 的 connected 标志**要靠数据流动才能置位** —— 看 cores/esp32/HWCDC.cpp:
  //   SERIAL_IN_EMPTY 中断(host 把发出去的数据收走了)里才 connected = true;
  //   而端口一打开(BUS_RESET)反而会把它清成 false。
  //   所以"没连接就不打印"会变成死锁:不打 → 没人收 → 永远不 connected → 永远不打。
  //   实测现象就是监视器一片空白,看起来跟固件没跑一样。
  //   改成无条件重打:只要监视器接上,最多 1 秒就能看到这几行。
  //   车上是没有 USB 主机的,那时这些字进环形缓冲后被丢掉 —— 代价可忽略。
#if defined(DASH_DEVICE_SELFTEST)
  {
    static uint32_t last_bringup_ms = 0;
    if (now < kBringupMs && (uint32_t)(now - last_bringup_ms) >= 1000) {
      last_bringup_ms = now;
      print_selftest("上电后 1Hz 补打,监视器随时接上都能看到");
    }
  }
#endif

  // 每 5 秒打印各字段当前由哪个源供给 + 当前数值(调试用)。
  // 数值是必须的:桩驱动丢弃画面,开机动画/换屏之前只能靠串口确认
  // 假数据弧确实在扫量程(见 ACCEPTANCE.md 的"假数据扫表"一条)。
  // ★ 无条件打印(别加"主机连上才打"的判断 —— 那会把 connected 卡死,见上)。
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    const DataSourceStatus& s = g_data.status();
    Serial.printf("SRC speed=%s rpm=%s coolant=%s intake=%s | v=%.1fkm/h %.0frpm %.1fC %.1fC\n",
                  fieldSourceName(s.speed),
                  fieldSourceName(s.rpm),
                  fieldSourceName(s.coolant),
                  fieldSourceName(s.intake),
                  st.speed_kmh, st.rpm, st.coolant_c, st.intake_c);
  }
}
