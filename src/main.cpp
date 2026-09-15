#include <Arduino.h>
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

// VAN 物理层:现在是桩(无硬件);SN65HVD230 到货后换成 VanPhyWire
// (见 van_phy_wire.h),把 GPIO 边沿时间戳喂进去即可,数据层不用动。
// 数据源不是 VanSink,用 VanSourceSink 转一层(见 van_phy.h);
// sink 必须在 g_data 之后构造,所以依赖的是它内部的 vanSource()。
static VanPhyStub g_van_phy;
static VanSourceSink g_van_sink(&g_data.vanSource());

static uint32_t last_ui_ms = 0;
static uint32_t last_status_ms = 0;

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

void setup() {
  Serial.begin(115200);
  delay(200);
  // 开机握手行:刷机后靠它确认固件真的跑起来了(见 ACCEPTANCE.md)。
  // 放在最前面 —— 即使后面的初始化有问题,至少能看到这一行。
  Serial.println("206 dash ok");
  g_data.begin();
  g_van_phy.setSink(&g_van_sink);   // 物理层 → 数据源(转一层,见 van_phy.h)
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
  Serial.println("van phy: stub");   // 物理层类型,换实驱动后改这一行
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

  // 每 5 秒打印各字段当前由哪个源供给 + 当前数值(调试用)。
  // 数值是必须的:桩驱动丢弃画面,开机动画/换屏之前只能靠串口确认
  // 假数据弧确实在扫量程(见 ACCEPTANCE.md 的"假数据扫表"一条)。
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    const DataSourceStatus& s = g_data.status();
    Serial.printf("SRC speed=%s rpm=%s coolant=%s | v=%.1fkm/h %.0frpm %.1fC\n",
                  fieldSourceName(s.speed),
                  fieldSourceName(s.rpm),
                  fieldSourceName(s.coolant),
                  st.speed_kmh, st.rpm, st.coolant_c);
  }
}
